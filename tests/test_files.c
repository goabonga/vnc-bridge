/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "../src/files.h"
#include "harness.h"
#include "run.h"

int main(void)
{
	char path[] = "/tmp/tvx-files-XXXXXX";
	int fd = mkstemp(path);
	CHECK(fd >= 0);
	close(fd);

	CHECK(read_file("/nonexistent/tvx-file") == NULL);

	write_file(path, "", 0600);
	char *text = read_file(path);
	CHECK_STR(text, "");
	free(text);

	write_file(path, "line one\nline two\n", 0600);
	text = read_file(path);
	CHECK_STR(text, "line one\nline two\n");
	free(text);

	/* Files larger than one read buffer are read whole. */
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	for (int i = 0; i < 10000; i++)
		fputs("0123456789", f);
	fclose(f);
	text = read_file(path);
	CHECK(text != NULL && strlen(text) == 100000);
	free(text);

	/* Files over 4 MiB are refused. */
	f = fopen(path, "w");
	CHECK(f != NULL);
	char block[65536];
	memset(block, 'a', sizeof block);
	for (int i = 0; i < 65; i++)
		fwrite(block, 1, sizeof block, f);
	fclose(f);
	CHECK(read_file(path) == NULL);

	unlink(path);
	return vncb_finish("test_files");
}
