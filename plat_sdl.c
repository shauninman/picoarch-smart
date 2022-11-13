#include <SDL/SDL.h>
#include <unistd.h>
#include "core.h"
#include "libpicofe/fonts.h"
#include "libpicofe/plat.h"
#include "menu.h"
#include "plat.h"
#include "scale.h"
#include "util.h"

// ------------------------------------------------------------
#include <dlfcn.h>

typedef struct _KeyShmInfo{
    int id;
    void *addr;
}KeyShmInfo;

#define MONITOR_VOLUME 0
#define MONITOR_BRIGHTNESS 1
#define MONITOR_ADC_VALUE 8

typedef int (*InitKeyShm_t)(KeyShmInfo *);
typedef int (*SetKeyShm_t)(KeyShmInfo* info, int key, int value);
typedef int (*GetKeyShm_t)(KeyShmInfo* info, int key);
typedef int (*UninitKeyShm_t)(KeyShmInfo *);

static void* libshmvar;
static GetKeyShm_t GetKeyShm;

static KeyShmInfo info;
static void monitor_init(void) {
	libshmvar	 = dlopen("libshmvar.so", RTLD_LAZY);
	GetKeyShm	 = dlsym(libshmvar, "GetKeyShm"); 

	InitKeyShm_t InitKeyShm	 = dlsym(libshmvar, "InitKeyShm");
	InitKeyShm(&info);
}
static void monitor_quit(void) {
	UninitKeyShm_t UninitKeyShm = dlsym(libshmvar, "UninitKeyShm");
	UninitKeyShm(&info);
	// dlcose(libshmvar); // not available?
}
static int monitor_charge(void) {
	int adc = GetKeyShm(&info, MONITOR_ADC_VALUE);
	if (adc>43) return 5;
	if (adc>42) return 4;
	if (adc>41) return 3;
	if (adc>39) return 2;
	return 1;
}
static int monitor_volume(void) {
	return GetKeyShm(&info, MONITOR_VOLUME);
}
static int monitor_brightness(void) {
	return GetKeyShm(&info, MONITOR_BRIGHTNESS);
}

// ------------------------------------------------------------
// based on eggs Miyoo Mini GFX lib (any bad decisions are mine)

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <sys/time.h>

#define FB_BUFFER_COUNT 3
#define SCREEN_DEPTH SCREEN_BPP * 8

static int		fb_idx = 0;
static int		fd_fb = 0;
static void	   *fb_addr;
static uint32_t fb_size = 0;

pthread_t		flip_pt;
pthread_mutex_t	flip_mx;
pthread_cond_t	flip_req;
pthread_cond_t	flip_start;
volatile uint32_t now_flipping;

static SDL_Surface* video;
static SDL_Surface* screen;
static SDL_Surface* menu;
static void* flip_thread(void* param) {
	pthread_mutex_lock(&flip_mx);
	while (1) {
		while (!now_flipping) pthread_cond_wait(&flip_req, &flip_mx);
		do {
			pthread_cond_signal(&flip_start);
			pthread_mutex_unlock(&flip_mx);
			int arg = 0;
			if (limit_frames) ioctl(fd_fb, FBIO_WAITFORVSYNC, &arg);
			SDL_Flip(video); // this handles rotation
			pthread_mutex_lock(&flip_mx);
		} while (--now_flipping);
	}
	return NULL;
}

static void flip_init(void) {
	fd_fb = open("/dev/fb0", O_RDWR);
	video = SDL_SetVideoMode(SCREEN_WIDTH,SCREEN_HEIGHT,SCREEN_DEPTH,SDL_SWSURFACE);
	
	int	pitch = SCREEN_WIDTH * SCREEN_BPP;
	fb_size = pitch * SCREEN_HEIGHT;
	fb_addr = malloc(fb_size * FB_BUFFER_COUNT);
	screen = SDL_CreateRGBSurfaceFrom(fb_addr,SCREEN_WIDTH,SCREEN_HEIGHT,SCREEN_DEPTH,pitch,0,0,0,0);
	
	flip_mx = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	flip_req = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	flip_start = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	now_flipping = 0;
	pthread_create(&flip_pt, NULL, flip_thread, NULL);
}

static void do_flip(void) {
	// flipping heck
	pthread_mutex_lock(&flip_mx);
	while (now_flipping==2) pthread_cond_wait(&flip_start, &flip_mx);
	
	// flip
	SDL_BlitSurface(screen, NULL, video, NULL); // because the screen is rotated this blits to a shadow buffer
	
	// advance screen buffer so we can draw the next frame while waiting for vsync
	fb_idx += 1;
	if (fb_idx>=FB_BUFFER_COUNT) fb_idx -= FB_BUFFER_COUNT;
	screen->pixels = fb_addr + (fb_size * fb_idx);
	
	if (!now_flipping) {
		now_flipping = 1;
		pthread_cond_signal(&flip_req);
		pthread_cond_wait(&flip_start, &flip_mx);
	} else {
		now_flipping = 2;
	}
	
	pthread_mutex_unlock(&flip_mx);
}

static void flip_clear(void) {
	memset(fb_addr, 0, fb_size * FB_BUFFER_COUNT);
}

static void flip_quit(void) {
	SDL_FillRect(video, NULL, 0);
	SDL_Flip(video);
	
	pthread_cancel(flip_pt);
	pthread_join(flip_pt, NULL);
	
	close(fd_fb);
	
	SDL_FreeSurface(screen);
	free(fb_addr);
}

// ------------------------------------------------------------

struct audio_state {
	unsigned buf_w;
	unsigned max_buf_w;
	unsigned buf_r;
	size_t buf_len;
	struct audio_frame *buf;
	int in_sample_rate;
	int out_sample_rate;
	int sample_rate_adj;
	int adj_out_sample_rate;
};

struct audio_state audio;

static void plat_sound_select_resampler(void);
void (*plat_sound_write)(const struct audio_frame *data, int frames);

#define DRC_MAX_ADJUSTMENT 0.003
#define DRC_ADJ_BELOW 40
#define DRC_ADJ_ABOVE 60

static char msg[HUD_LEN];
static unsigned msg_priority = 0;
static unsigned msg_expire = 0;

static bool frame_dirty = false;
static int frame_time = 1000000 / 60;

static void video_expire_msg(void)
{
	msg[0] = '\0';
	msg_priority = 0;
	msg_expire = 0;
}

static void video_update_msg(void)
{
	if (msg[0] && msg_expire < plat_get_ticks_ms())
		video_expire_msg();
}

static void video_clear_msg(uint16_t *dst, uint32_t h, uint32_t pitch)
{
	memset(dst + (h - 10) * pitch, 0, 10 * pitch * sizeof(uint16_t));
}

static void video_print_msg(uint16_t *dst, uint32_t h, uint32_t pitch, char *msg)
{
	basic_text_out16_nf(dst, pitch, 2, h - 10, msg);
}

static int audio_resample_passthrough(struct audio_frame data) {
	audio.buf[audio.buf_w++] = data;
	if (audio.buf_w >= audio.buf_len) audio.buf_w = 0;

	return 1;
}

static int audio_resample_nearest(struct audio_frame data) {
	static int diff = 0;
	int consumed = 0;

	if (diff < audio.adj_out_sample_rate) {
		audio.buf[audio.buf_w++] = data;
		if (audio.buf_w >= audio.buf_len) audio.buf_w = 0;

		diff += audio.in_sample_rate;
	}

	if (diff >= audio.adj_out_sample_rate) {
		consumed++;
		diff -= audio.adj_out_sample_rate;
	}

	return consumed;
}

static void *fb_flip(void)
{
	plat_draw_hud();
	SDL_BlitSurface(screen, NULL, video, NULL);
	do_flip();
	return screen->pixels;
}

void *plat_prepare_screenshot(int *w, int *h, int *bpp)
{
	if (w) *w = SCREEN_WIDTH;
	if (h) *h = SCREEN_HEIGHT;
	if (bpp) *bpp = SCREEN_BPP;

	return screen->pixels;
}

int plat_dump_screen(const char *filename) {
	char imgname[MAX_PATH];
	int ret = -1;
	SDL_Surface *surface = NULL;

	snprintf(imgname, MAX_PATH, "%s.bmp", filename);

	if (g_menuscreen_ptr) {
		surface = SDL_CreateRGBSurfaceFrom(g_menubg_src_ptr,
		                                   g_menubg_src_w,
		                                   g_menubg_src_h,
		                                   16,
		                                   g_menubg_src_w * sizeof(uint16_t),
		                                   0xF800, 0x07E0, 0x001F, 0x0000);
		if (surface) {
			ret = SDL_SaveBMP(surface, imgname);
			SDL_FreeSurface(surface);
		}
	} else {
		ret = SDL_SaveBMP(screen, imgname);
	}

	return ret;
}

int plat_load_screen(const char *filename, void *buf, size_t buf_size, int *w, int *h, int *bpp) {
	int ret = -1;
	char imgname[MAX_PATH];
	SDL_Surface *imgsurface = NULL;
	SDL_Surface *surface = NULL;

	snprintf(imgname, MAX_PATH, "%s.bmp", filename);
	imgsurface = SDL_LoadBMP(imgname);
	if (!imgsurface)
		goto finish;

	surface = SDL_DisplayFormat(imgsurface);
	if (!surface)
		goto finish;

	if (surface->pitch > SCREEN_PITCH ||
	    surface->h > SCREEN_HEIGHT ||
	    surface->w == 0 ||
	    surface->h * surface->pitch > buf_size)
		goto finish;

	memcpy(buf, surface->pixels, surface->pitch * surface->h);
	*w = surface->w;
	*h = surface->h;
	*bpp = surface->pitch / surface->w;

	ret = 0;

finish:
	if (imgsurface)
		SDL_FreeSurface(imgsurface);
	if (surface)
		SDL_FreeSurface(surface);
	return ret;
}


void plat_video_menu_enter(int is_rom_loaded)
{
	SDL_LockSurface(screen);
	memcpy(g_menubg_src_ptr, screen->pixels, g_menubg_src_h * g_menubg_src_pp * sizeof(uint16_t));
	SDL_UnlockSurface(screen);
	g_menuscreen_ptr = fb_flip();
}

void plat_video_menu_begin(void)
{
	SDL_LockSurface(screen);
	menu_begin();
}

void plat_video_menu_end(void)
{
	menu_end();
	SDL_UnlockSurface(screen);
	g_menuscreen_ptr = fb_flip();
}

void plat_video_menu_leave(void)
{
	memset(g_menubg_src_ptr, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));

	flip_clear();
	
	// SDL_LockSurface(screen);
	// memset(screen->pixels, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	// SDL_UnlockSurface(screen);
	// fb_flip();
	// SDL_LockSurface(screen);
	// memset(screen->pixels, 0, g_menuscreen_h * g_menuscreen_pp * sizeof(uint16_t));
	// SDL_UnlockSurface(screen);

	g_menuscreen_ptr = NULL;
}

void plat_video_open(void)
{
}

void plat_video_set_msg(const char *new_msg, unsigned priority, unsigned msec)
{
	if (!new_msg) {
		video_expire_msg();
	} else if (priority >= msg_priority) {
		snprintf(msg, HUD_LEN, "%s", new_msg);
		string_truncate(msg, HUD_LEN - 1);
		msg_priority = priority;
		msg_expire = plat_get_ticks_ms() + msec;
	}
}

uint64_t plat_get_ticks_us_u64(void) {
    uint64_t ret;
    struct timeval tv;

    gettimeofday(&tv, NULL);

    ret = (uint64_t)tv.tv_sec * 1000000;
    ret += (uint64_t)tv.tv_usec;

    return ret;
}

#define FRAME_LIMIT_US 12000 
void plat_video_process(const void *data, unsigned width, unsigned height, size_t pitch) {
	static uint64_t last_flip_time_us = 0;
	static uint64_t next_frame_time_us = 0;
	
	uint64_t time = plat_get_ticks_us_u64();
	int skip_flip = !limit_frames && time-last_flip_time_us<FRAME_LIMIT_US;
	if (!enable_drc) {
		if (!skip_flip) {
			last_flip_time_us = time;
		}
		
		next_frame_time_us = 0;
		
		if (skip_flip) return;
		
	} else {
		if ( (!next_frame_time_us) || (!limit_frames) ) {
			next_frame_time_us = time;
		}
		
		if (!skip_flip) {
			last_flip_time_us = time;
		}

		do {
			next_frame_time_us += frame_time;
		} while (next_frame_time_us < time);
		
		if (skip_flip) return;
	}
	
	static int had_msg = 0;
	frame_dirty = true;
	SDL_LockSurface(screen);
	
	// clean up undrawn space
	static unsigned last_width = 0;
	static unsigned last_height = 0;
	if (width!=last_width || height!=last_height) {
		flip_clear();
		last_width = width;
		last_height = height;
	}
	
	if (had_msg) {
		video_clear_msg(screen->pixels, screen->h, screen->pitch / SCREEN_BPP);
		had_msg = 0;
	}

	scale(width, height, pitch, data, screen->pixels);

	if (msg[0]) {
		video_print_msg(screen->pixels, screen->h, screen->pitch / SCREEN_BPP, msg);
		had_msg = 1;
	}

	SDL_UnlockSurface(screen);

	video_update_msg();
}

void plat_video_flip(void)
{	
	// commented out bits not needed because we're waiting for vsync in fb_flip()
	
	// static unsigned int next_frame_time_us = 0;
	//
	if (frame_dirty) {
	// 	unsigned int time = plat_get_ticks_us();
	//
	// 	if (limit_frames && enable_drc && time < next_frame_time_us) {
	// 		usleep(next_frame_time_us - time);
	// 	}
	//
	// 	if (!next_frame_time_us)
	// 		next_frame_time_us = time;
	//
		fb_flip();
	//
	// 	do {
	// 		next_frame_time_us += frame_time;
	// 	} while (next_frame_time_us < time);
	}

	frame_dirty = false;
}

void plat_video_clear(void) {
	flip_clear();
}

void plat_video_close(void)
{
}

unsigned plat_cpu_ticks(void)
{
	long unsigned ticks = 0;
	long ticksps = 0;
	FILE *file = NULL;

	file = fopen("/proc/self/stat", "r");
	if (!file)
		goto finish;

	if (!fscanf(file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu", &ticks))
		goto finish;

	ticksps = sysconf(_SC_CLK_TCK);

	if (ticksps)
		ticks = ticks * 100 / ticksps;

finish:
	if (file)
		fclose(file);

	return ticks;
}

static void plat_sound_callback(void *unused, uint8_t *stream, int len)
{
	int16_t *p = (int16_t *)stream;
	if (audio.buf_len == 0)
		return;

	len /= (sizeof(int16_t) * 2);

	while (audio.buf_r != audio.buf_w && len > 0) {
		*p++ = audio.buf[audio.buf_r].left;
		*p++ = audio.buf[audio.buf_r].right;
		audio.max_buf_w = audio.buf_r;

		len--;
		audio.buf_r++;

		if (audio.buf_r >= audio.buf_len) audio.buf_r = 0;
	}

	while(len > 0) {
		*p++ = 0;
		--len;
	}
}

static void plat_sound_finish(void)
{
	SDL_PauseAudio(1);
	SDL_CloseAudio();
	if (audio.buf) {
		free(audio.buf);
		audio.buf = NULL;
	}
}

static int plat_sound_init(void)
{
	if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		return -1;
	}

	SDL_AudioSpec spec, received;

	spec.freq = MIN(sample_rate, MAX_SAMPLE_RATE);
	spec.format = AUDIO_S16;
	spec.channels = 2;
	spec.samples = 512;
	spec.callback = plat_sound_callback;

	if (SDL_OpenAudio(&spec, &received) < 0) {
		plat_sound_finish();
		return -1;
	}

	audio.in_sample_rate = sample_rate;
	audio.out_sample_rate = received.freq;
	audio.sample_rate_adj = audio.out_sample_rate * DRC_MAX_ADJUSTMENT;
	audio.adj_out_sample_rate = audio.out_sample_rate;

	plat_sound_select_resampler();
	plat_sound_resize_buffer();

	SDL_PauseAudio(0);
	return 0;
}

int plat_sound_occupancy(void)
{
	int buffered = 0;
	if (audio.buf_len == 0)
		return 0;

	if (audio.buf_w != audio.buf_r) {
		buffered = audio.buf_w > audio.buf_r ?
			audio.buf_w - audio.buf_r :
			(audio.buf_w + audio.buf_len) - audio.buf_r;
	}

	return buffered * 100 / audio.buf_len;
}

#define BATCH_SIZE 100
void plat_sound_write_resample(const struct audio_frame *data, int frames, int (*resample)(struct audio_frame data), bool drc)
{
	int consumed = 0;
	if (audio.buf_len == 0)
		return;

	if (drc) {
		int occupancy = plat_sound_occupancy();

		if (occupancy < DRC_ADJ_BELOW) {
			audio.adj_out_sample_rate = audio.out_sample_rate + audio.sample_rate_adj;
		} else if (occupancy > DRC_ADJ_ABOVE) {
			audio.adj_out_sample_rate = audio.out_sample_rate - audio.sample_rate_adj;
		} else {
			audio.adj_out_sample_rate = audio.out_sample_rate;
		}
	}

	SDL_LockAudio();

	while (frames > 0) {
		int tries = 0;
		int amount = MIN(BATCH_SIZE, frames);

		while (tries < 10 && audio.buf_w == audio.max_buf_w) {
			tries++;
			SDL_UnlockAudio();

			if (!limit_frames)
				return;

			plat_sleep_ms(1);
			SDL_LockAudio();
		}

		while (amount && audio.buf_w != audio.max_buf_w) {
			consumed = resample(*data);
			data += consumed;
			amount -= consumed;
			frames -= consumed;
		}
	}
	SDL_UnlockAudio();
}

void plat_sound_write_passthrough(const struct audio_frame *data, int frames)
{
	plat_sound_write_resample(data, frames, audio_resample_passthrough, false);
}

void plat_sound_write_nearest(const struct audio_frame *data, int frames)
{
	plat_sound_write_resample(data, frames, audio_resample_nearest, false);
}

void plat_sound_write_drc(const struct audio_frame *data, int frames)
{
	plat_sound_write_resample(data, frames, audio_resample_nearest, true);
}

void plat_sound_resize_buffer(void) {
	size_t buf_size;
	SDL_LockAudio();

	audio.buf_len = frame_rate > 0
		? current_audio_buffer_size * audio.in_sample_rate / frame_rate
		: 0;

		/* Dynamic adjustment keeps buffer 50% full, need double size */
	if (enable_drc)
		audio.buf_len *= 2;

	if (audio.buf_len == 0) {
		SDL_UnlockAudio();
		return;
	}

	buf_size = audio.buf_len * sizeof(struct audio_frame);
	audio.buf = realloc(audio.buf, buf_size);

	if (!audio.buf) {
		SDL_UnlockAudio();
		PA_ERROR("Error initializing sound buffer\n");
		plat_sound_finish();
		return;
	}

	memset(audio.buf, 0, buf_size);
	audio.buf_w = 0;
	audio.buf_r = 0;
	audio.max_buf_w = audio.buf_len - 1;
	SDL_UnlockAudio();
}

static void plat_sound_select_resampler(void)
{
	if (enable_drc) {
		PA_INFO("Using audio adjustment (in: %d, out: %d-%d)\n", audio.in_sample_rate, audio.out_sample_rate - audio.sample_rate_adj, audio.out_sample_rate + audio.sample_rate_adj);
		plat_sound_write = plat_sound_write_drc;
	} else if (audio.in_sample_rate == audio.out_sample_rate) {
		PA_INFO("Using passthrough resampler (in: %d, out: %d)\n", audio.in_sample_rate, audio.out_sample_rate);
		plat_sound_write = plat_sound_write_passthrough;
	} else {
		PA_INFO("Using nearest resampler (in: %d, out: %d)\n", audio.in_sample_rate, audio.out_sample_rate);
		plat_sound_write = plat_sound_write_nearest;
	}
}

void plat_sdl_event_handler(void *event_)
{
}

int plat_init(void)
{
	plat_sound_write = plat_sound_write_nearest;

	// for trimui smart
	putenv("SDL_VIDEO_FBCON_ROTATION=CCW");
	putenv("SDL_USE_PAN=true");
	
	SDL_Init(SDL_INIT_VIDEO);
	
	flip_init();
	
	SDL_ShowCursor(0);

	g_menuscreen_w = SCREEN_WIDTH;
	g_menuscreen_h = SCREEN_HEIGHT;
	g_menuscreen_pp = SCREEN_WIDTH;
	g_menuscreen_ptr = NULL;

	g_menubg_src_w = SCREEN_WIDTH;
	g_menubg_src_h = SCREEN_HEIGHT;
	g_menubg_src_pp = SCREEN_WIDTH;
	
	menu = SDL_CreateRGBSurfaceFrom(g_menuscreen_ptr,g_menuscreen_w,g_menuscreen_h,16,g_menuscreen_w * sizeof(uint16_t),0,0,0,0);

	if (in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler)) {
		PA_ERROR("SDL input failed to init: %s\n", SDL_GetError());
		return -1;
	}
	in_probe();

	if (plat_sound_init()) {
		PA_ERROR("SDL sound failed to init: %s\n", SDL_GetError());
		return -1;
	}
	
	monitor_init();
	return 0;
}

int plat_reinit(void)
{
	if (sample_rate && sample_rate != audio.in_sample_rate) {
		plat_sound_finish();

		if (plat_sound_init()) {
			PA_ERROR("SDL sound failed to init: %s\n", SDL_GetError());
			return -1;
		}
	} else {
		plat_sound_resize_buffer();
		plat_sound_select_resampler();
	}

	if (frame_rate != 0)
		frame_time = 1000000 / frame_rate;

	scale_update_scaler();
	return 0;
}

void plat_finish(void)
{
	monitor_quit();
	plat_sound_finish();
	flip_quit();
	SDL_Quit();
}

void plat_draw_hud(void) {
	SDL_Surface* buffer = screen;
	if (g_menuscreen_ptr) {
		menu->pixels = g_menuscreen_ptr;
		buffer = menu;
	}
	
	uint32_t white = SDL_MapRGB(buffer->format,0xff,0xff,0xff);
	uint32_t red = SDL_MapRGB(buffer->format,0xff,0,0);
	
	// TODO: this is super gross
	Uint8 *keystate = SDL_GetKeyState(NULL);
	if (keystate[SDLK_RCTRL]) {
		int v = monitor_volume();
		int x = 130;
		int y = 8;
		int h = 6;
		for (int i=0; i<v; i++) {
			SDL_FillRect(buffer, &(SDL_Rect){x+(i*3),y,2,h}, white);
		}
	}
	else if (keystate[SDLK_RETURN]) {
		int b = monitor_brightness();
		int x = 130;
		int y = 8;
		int h = 6;
		for (int i=0; i<b; i++) {
			SDL_FillRect(buffer, &(SDL_Rect){x+(i*6),y,5,h}, white);
		}
	}
	
	int charge = monitor_charge();
	if (g_menuscreen_ptr || charge==1) {
		int w = 14;
		int h = 6;
		uint32_t c = charge == 1 ? red : white;

		int x = SCREEN_WIDTH-4-(w+8);
		int y = 4;

		SDL_FillRect(buffer, &(SDL_Rect){x,y,w+8,h+8}, 0);
		SDL_FillRect(buffer, &(SDL_Rect){x+2,y+2,w+4,h+4}, c);
		SDL_FillRect(buffer, &(SDL_Rect){x+3,y+3,w+2,h+2}, 0);
		for (int i=0; i<charge; i++) {
			SDL_FillRect(buffer, &(SDL_Rect){x+4+(i*3),y+4,2,h}, c);
		}
	}
}
