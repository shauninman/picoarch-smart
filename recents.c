#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "core.h"
#include "recents.h"
#include "util.h"

int recents_len = 0;
struct recent recents[MAX_RECENTS];

static void normalize_newline(char* line) {
	int len = strlen(line);
	if (len>1 && line[len-1]=='\n' && line[len-2]=='\r') { // windows!
		line[len-2] = '\n';
		line[len-1] = '\0';
	}
}
static void trim_newlines(char* line) {
	int len = strlen(line);
	while (len>0 && line[len-1]=='\n') {
		line[len-1] = '\0'; // trim newline
		len -= 1;
	}
}

void recents_load(void) {
	char recents_path[MAX_PATH];
	sprintf(recents_path, "%s/.picoarch/recent.txt", getenv("HOME"));
	
	recents_len = 0;
	FILE *file = fopen(recents_path, "r");
	if (file) {
		char line[MAX_PATH];
		while (fgets(line,MAX_PATH,file)!=NULL) {
			normalize_newline(line);
			trim_newlines(line);
			if (strlen(line)==0) continue; // skip empty lines
			
			if (recents_len>=MAX_RECENTS) break;
			
			char core_path[MAX_PATH];
			char content_path[MAX_PATH];
			
			char* tmp = strchr(line, ' '); // this means there can be no spaces in the core path...
			tmp[0] = '\0';
			strcpy(core_path, line);
			strcpy(content_path, tmp+1);
			
			if (!file_exists(core_path) || !file_exists(content_path)) continue;
			
			struct recent* item = &recents[recents_len++];
			strcpy(item->core_path, core_path);
			strcpy(item->content_path, content_path);
		}
		fclose(file);
	}
}
void recents_add(const char* core_path, const char* content_path) {
	char recents_path[MAX_PATH];
	sprintf(recents_path, "%s/.picoarch/recent.txt", getenv("HOME"));
	
	FILE* file = fopen(recents_path, "w");
	if (file) {
		char line[MAX_PATH];
		sprintf(line, "%s %s", core_path, content_path);
		fputs(line, file);
		putc('\n', file);
		
		for (int i=0; i<recents_len; i++) {
			if (i>=MAX_RECENTS) break;
			
			struct recent* item = &recents[i];
			if (string_match(item->core_path,core_path) && string_match(item->content_path,content_path)) continue;
			
			sprintf(line, "%s %s", item->core_path, item->content_path);
			fputs(line, file);
			putc('\n', file);
		}
		fclose(file);
	}
}
