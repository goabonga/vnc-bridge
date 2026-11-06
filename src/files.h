/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#ifndef VNCB_FILES_H
#define VNCB_FILES_H
#include <stdio.h>
#include <stdlib.h>

#define VNCB_FILE_MAX (4 * 1024 * 1024)

/* Read a whole text file into a NUL terminated malloc'd string. Returns NULL
 * when it cannot be opened, is larger than 4 MiB or memory runs out. */
static inline char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	size_t cap = 4096, len = 0;
	char *data = malloc(cap);
	size_t n;
	while (data && (n = fread(data + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (len > VNCB_FILE_MAX) {
			free(data);
			data = NULL;
			break;
		}
		if (len + 1 >= cap) {
			char *bigger = realloc(data, cap *= 2);
			if (!bigger) {
				free(data);
				data = NULL;
			} else {
				data = bigger;
			}
		}
	}
	fclose(f);
	if (data)
		data[len] = 0;
	return data;
}
#endif
