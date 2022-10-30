#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util.h"

struct string_list *string_split(const char *string, char delim) {
	const char *loc = string;
	int size = 2; /* rest of string, NULL terminator */
	int index = 0;
	char delims[2] = {0};
	char *saveptr = NULL;
	struct string_list *list = calloc(1, sizeof(struct string_list));

	if (!list)
		goto finish;

	list->source = strdup(string);
	if (!list->source)
		goto finish;

	while((loc = strchr(loc, delim))) {
		size++;
		while (*loc && *loc == delim) loc++;
	}

	list->list = calloc(size, sizeof(char *));
	if (!list->list)
		goto finish;

	delims[0] = delim;

	while((loc = strtok_r(index == 0 ? list->source : NULL, delims, &saveptr))) {
		list->list[index++] = loc;
		if (index >= size)
			break;
	}

finish:
	return list;
}

void string_list_free(struct string_list *list) {
	if (list) {
		if (list->list) {
			free(list->list);
			list->list = NULL;
		}
		if (list->source) {
			free(list->source);
			list->source = NULL;
		}

		free(list);
	}
}

void string_truncate(char *string, size_t max_len) {
	size_t len = strlen(string) + 1;
	if (len <= max_len) return;

	strncpy(&string[max_len - 4], "...\0", 4);
}

void string_wrap(char *string, size_t max_len, size_t max_lines) {
	char *line = string;

	for (size_t i = 1; i < max_lines; i++) {
		char *p = line;
		char *prev;
		do {
			prev = p;
			p = strchr(prev+1, ' ');
		} while (p && p - line < (int)max_len);

		if (!p && strlen(line) < max_len) break;

		if (prev && prev != line) {
			line = prev + 1;
			*prev = '\n';
		}
	}
	string_truncate(line, max_len);
}

int string_match(const char* str1, const char* str2) {
	int len1 = strlen(str1);
	if (len1!=strlen(str2)) return 0;
	return (strncmp(str1,str2,len1)==0);
}

void file_put_string(const char* path, const char* str) {
	FILE* file = fopen(path, "w");
	if (file) {
		fputs(str, file);
		fclose(file);
	}
}

int file_get_int(const char* path) {
	int i = 0;
	FILE *file = fopen(path, "r");
	if (file!=NULL) {
		fscanf(file, "%i", &i);
		fclose(file);
	}
	return i;
}
void file_put_int(const char* path, int value) {
	char buffer[8];
	sprintf(buffer, "%d", value);
	file_put_string(path, buffer);
}

int file_exists(const char* path) {
	return access(path, F_OK)==0;
}