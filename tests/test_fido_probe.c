/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#define main fido_cli_main
#include "../src/vnc-bridge-fido.c"
#undef main
#include "base.h"
#include "harness.h"
#include "run.h"
#include <sys/socket.h>
#include <sys/wait.h>

static char *read_file_text(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char buf[256];
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	buf[n] = 0;
	fclose(f);
	return strdup(buf);
}

enum behavior { ANSWER, WRONG_NONCE, SILENT, WRONG_COMMAND };

/* Fake authenticator: checks the CTAPHID_INIT request it receives and answers
 * according to `how`. Its exit status is 0 when the request was well formed. */
static pid_t start_device(int *fd, enum behavior how)
{
	int pair[2];
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
	pid_t pid = fork();
	if (pid == 0) {
		close(pair[0]);
		unsigned char request[65];
		struct pollfd p = {pair[1], POLLIN, 0};
		if (poll(&p, 1, 3000) <= 0 ||
		    recv(pair[1], request, sizeof request, 0) != 65)
			_exit(1);
		static const unsigned char head[] = {0x00, 0xff, 0xff, 0xff,
						     0xff, 0x86, 0x00, 0x08};
		if (memcmp(request, head, sizeof head) != 0)
			_exit(2);
		unsigned char response[64] = {0};
		memcpy(response, request + 1, 4);
		response[4] = how == WRONG_COMMAND ? 0x90 : 0x86;
		response[6] = 17;
		memcpy(response + 7, request + 8, 8);
		if (how == WRONG_NONCE)
			response[7] ^= 0xff;
		response[19] = 2;
		response[23] = 0x05;
		if (how != SILENT)
			send(pair[1], response, sizeof response, 0);
		poll(NULL, 0, 1000);
		_exit(0);
	}
	close(pair[1]);
	*fd = pair[0];
	return pid;
}

static void stop_device(pid_t pid, int fd, int expected)
{
	close(fd);
	int status = 0;
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

static void run_probe(enum behavior how, int want, const char *printed_text)
{
	int fd;
	pid_t pid = start_device(&fd, how);
	int result = -1;
	char *out = NULL;
	{
		char path[] = "/tmp/tvx-probe-XXXXXX";
		int file = mkstemp(path);
		fflush(stdout);
		int saved = dup(1);
		dup2(file, 1);
		close(file);
		result = probe(fd, 400);
		fflush(stdout);
		dup2(saved, 1);
		close(saved);
		out = read_file_text(path);
		unlink(path);
	}
	CHECK(result == want);
	CHECK_STR(out, printed_text);
	free(out);
	stop_device(pid, fd, 0);
}

int main(void)
{
	run_probe(ANSWER, 0,
		  "CTAPHID_INIT OK; HID framing=2; capabilities=0x05\n");
	run_probe(WRONG_NONCE, 1, "");
	run_probe(WRONG_COMMAND, 1, "");
	run_probe(SILENT, 1, "");
	{
		char *version_argv[] = {"build/bin/vnc-bridge-fido",
					"--version", NULL};
		run_result version_run = run_cmd(version_argv, NULL);
		CHECK(version_run.status == 0);
		CHECK_STR(version_run.out, BASE_VERSION "\n");
		free(version_run.out);
	}
	return vncb_finish("test_fido_probe");
}
