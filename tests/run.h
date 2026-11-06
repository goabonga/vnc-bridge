/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#ifndef VNCB_RUN_H
#define VNCB_RUN_H
#include "../src/exec.h"
#include <stdio.h>
#include <sys/stat.h>

typedef struct {
	char *out;  /* standard output, malloc'd and NUL terminated */
	int status; /* exit status, or -1 when killed or timed out */
} run_result;

/* Run argv with standard input read from stdin_path (or /dev/null) and
 * collect standard output; standard error is discarded. The child inherits the
 * caller's environment. */
static inline run_result run_cmd(char *const argv[], const char *stdin_path)
{
	run_result r = {NULL, -1};
	int fds[2];
	if (pipe(fds))
		return r;
	pid_t pid = fork();
	if (pid < 0)
		return r;
	if (!pid) {
		int in = open(stdin_path ? stdin_path : "/dev/null", O_RDONLY);
		dup2(in, 0);
		dup2(fds[1], 1);
		int null = open("/dev/null", O_WRONLY);
		if (null >= 0)
			dup2(null, 2);
		close(fds[0]);
		close(fds[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	size_t len = 0, cap = 4096;
	r.out = malloc(cap);
	double deadline = clocksec(CLOCK_MONOTONIC) + 5;
	for (;;) {
		int left = (int)((deadline - clocksec(CLOCK_MONOTONIC)) * 1000);
		struct pollfd pfd = {fds[0], POLLIN, 0};
		if (left <= 0 || poll(&pfd, 1, left) <= 0) {
			kill(pid, SIGKILL);
			break;
		}
		if (len + 1024 + 1 > cap) {
			char *bigger = realloc(r.out, cap * 2);
			if (!bigger)
				break;
			r.out = bigger;
			cap *= 2;
		}
		ssize_t n = read(fds[0], r.out + len, cap - len - 1);
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	r.out[len] = 0;
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	return r;
}

static inline void write_file(const char *path, const char *text, int mode)
{
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
	chmod(path, (mode_t)mode);
}

typedef struct {
	pid_t pid;
	int in;	 /* write end of the child's standard input */
	int out; /* read end of the child's standard output */
	int err; /* read end of the child's standard error */
} child_process;

/* Start argv with pipes on standard input, output and error. */
static inline child_process spawn_child(char *const argv[])
{
	child_process c = {-1, -1, -1, -1};
	int in[2], out[2], err[2];
	if (pipe(in) || pipe(out) || pipe(err))
		return c;
	c.pid = fork();
	if (c.pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		dup2(err[1], 2);
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		close(err[0]);
		close(err[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	close(err[1]);
	c.in = in[1];
	c.out = out[0];
	c.err = err[0];
	return c;
}

/* Read up to size-1 bytes available within timeout_ms (at least one byte when
 * possible); the result is NUL terminated. Returns the byte count. */
static inline size_t read_available(int fd, char *buf, size_t size,
				    int timeout_ms)
{
	struct pollfd p = {fd, POLLIN, 0};
	if (poll(&p, 1, timeout_ms) <= 0)
		return 0;
	ssize_t n = read(fd, buf, size - 1);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	return (size_t)n;
}

/* Wait for the child to exit; returns its exit status, 128+signal when
 * killed by a signal, or -1 on timeout (the child is then killed). */
static inline int reap_child(child_process *c, int timeout_ms)
{
	int status = 0;
	double end = clocksec(CLOCK_MONOTONIC) + timeout_ms / 1000.;
	for (;;) {
		if (waitpid(c->pid, &status, WNOHANG) == c->pid)
			break;
		if (clocksec(CLOCK_MONOTONIC) > end) {
			kill(c->pid, SIGKILL);
			waitpid(c->pid, &status, 0);
			status = -1;
			break;
		}
		poll(NULL, 0, 10);
	}
	if (c->in >= 0)
		close(c->in);
	close(c->out);
	close(c->err);
	if (status == -1)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* Run fn(arg) with standard output redirected to a file and return what it
 * printed (malloc'd), or NULL when the capture failed. */
static inline char *capture_stdout_of(void (*fn)(const char *), const char *arg)
{
	char path[] = "/tmp/tvx-stdout-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0)
		return NULL;
	fflush(stdout);
	int saved = dup(1);
	dup2(fd, 1);
	close(fd);
	fn(arg);
	fflush(stdout);
	dup2(saved, 1);
	close(saved);
	char *text = NULL;
	FILE *f = fopen(path, "r");
	if (f) {
		char buf[4096];
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		buf[n] = 0;
		text = malloc(n + 1);
		if (text)
			memcpy(text, buf, n + 1);
		fclose(f);
	}
	unlink(path);
	return text;
}
#endif
