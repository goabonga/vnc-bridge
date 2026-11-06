/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#ifndef VNCB_EXEC_H
#define VNCB_EXEC_H
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VNCB_CAPTURE_MAX 1048576

static inline double clocksec(clockid_t clock)
{
	struct timespec t;
	clock_gettime(clock, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Remove trailing whitespace in place. */
static inline void trim(char *s)
{
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = 0;
}

/* Run argv directly (no shell) with a time limit and a bounded output.
 * Returns the NUL-terminated standard output, or NULL when the command
 * cannot be started, exits non-zero, exceeds the limits or times out. */
static inline char *capture(char *const argv[], int timeout_ms)
{
	int fds[2];
	if (pipe2(fds, O_CLOEXEC))
		return NULL;
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return NULL;
	}
	if (!pid) {
		setpgid(0, 0);
		dup2(fds[1], 1);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, 2);
		setenv("LC_ALL", "C", 1);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	setpgid(pid, pid);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);

	char *out = malloc(VNCB_CAPTURE_MAX + 1);
	size_t len = 0;
	int status = 0;
	bool ok = out != NULL, eof = false;
	double deadline = clocksec(CLOCK_MONOTONIC) + timeout_ms / 1000.;
	while (ok && !eof) {
		int left = (int)((deadline - clocksec(CLOCK_MONOTONIC)) * 1000);
		if (left <= 0) {
			ok = false;
			break;
		}
		struct pollfd pfd = {fds[0], POLLIN, 0};
		int ready = poll(&pfd, 1, left);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0) {
			ok = false;
			break;
		}
		ssize_t n = read(fds[0], out + len, VNCB_CAPTURE_MAX - len);
		if (n > 0) {
			len += (size_t)n;
			if (len == VNCB_CAPTURE_MAX)
				ok = false;
		} else if (n == 0) {
			eof = true;
		} else if (errno != EAGAIN && errno != EINTR) {
			ok = false;
		}
	}
	close(fds[0]);
	/* A child may close stdout without exiting: keep waiting bounded too.
	 */
	while (waitpid(pid, &status, WNOHANG) == 0) {
		if (clocksec(CLOCK_MONOTONIC) >= deadline) {
			ok = false;
			break;
		}
		poll(NULL, 0, 5);
	}
	if (!ok) {
		kill(-pid, SIGKILL);
		kill(pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
	}
	if (!ok || !WIFEXITED(status) || WEXITSTATUS(status)) {
		free(out);
		return NULL;
	}
	out[len] = 0;
	return out;
}
#endif
