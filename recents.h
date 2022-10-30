#ifndef _RECENTS_H__
#define _RECENTS_H__

#include "main.h"

struct recent {
	char core_path[MAX_PATH];
	char content_path[MAX_PATH];
};

#define MAX_RECENTS 100
extern int recents_len;
extern struct recent recents[MAX_RECENTS];

void recents_load(void);
void recents_add(const char* core_path, const char* content_path);

void quicksave_save(void);
int quicksave_load(void);

#endif