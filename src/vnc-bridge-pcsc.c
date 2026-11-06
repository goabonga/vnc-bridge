/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char usage[] =
    "Usage: vnc-bridge-pcsc status [--runtime-dir DIR]\n"
    "       vnc-bridge-pcsc run [--runtime-dir DIR] [--client PID:FD] -- "
    "COMMAND [ARGS...]\n"
    "       vnc-bridge-pcsc run --socket PATH -- COMMAND [ARGS...]";

/* A usable channel is a socket, not reached through a symbolic link, owned by
 * the current user in a directory owned by the current user, both closed to
 * other users. */
static bool private_socket(const char *path)
{
	char parent[PATH_MAX];
	struct stat st;
	if (snprintf(parent, sizeof parent, "%s", path) >= (int)sizeof parent)
		return false;
	char *slash = strrchr(parent, '/');
	if (!slash || slash == parent)
		return false;
	*slash = 0;
	return !lstat(parent, &st) && S_ISDIR(st.st_mode) &&
	       st.st_uid == getuid() && !(st.st_mode & 077u) &&
	       !lstat(path, &st) && S_ISSOCK(st.st_mode) &&
	       st.st_uid == getuid() && !(st.st_mode & 077u);
}

typedef struct {
	bool status;
	const char *runtime;
	const char *client;
	const char *exact;
	int command; /* index of the program in argv, 0 when absent */
} options;

static bool parse(int argc, char **argv, options *o)
{
	memset(o, 0, sizeof *o);
	o->status = !strcmp(argv[1], "status");
	bool run = !strcmp(argv[1], "run");
	if (!o->status && !run)
		return false;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--") && i + 1 < argc) {
			o->command = i + 1;
			break;
		}
		if (!strcmp(argv[i], "--runtime-dir") && i + 1 < argc)
			o->runtime = argv[++i];
		else if (!strcmp(argv[i], "--client") && i + 1 < argc)
			o->client = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			o->exact = argv[++i];
		else
			return false;
	}
	if (o->status && (o->command || o->client || o->exact))
		return false;
	if (run && !o->command)
		return false;
	return !(o->exact && (o->runtime || o->client));
}

/* Parse "PID-FD.sock" with a positive PID and a numeric FD. */
static bool parse_channel_name(const char *name, long *pid, unsigned long *fd)
{
	char *end;
	errno = 0;
	*pid = strtol(name, &end, 10);
	if (errno || end == name || *end != '-' || *pid <= 0 || *pid > INT_MAX)
		return false;
	const char *digits = end + 1;
	errno = 0;
	*fd = strtoul(digits, &end, 10);
	return !errno && end != digits && isdigit((unsigned char)*digits) &&
	       !strcmp(end, ".sock");
}

/* Collect <runtime>/pcsc/PID-FD.sock channels of live processes. Prints them
 * for `status`. Returns the number found, or -1 on error. */
static int find_channels(const char *runtime, const char *client, bool print,
			 char *selected, size_t size)
{
	char pattern[PATH_MAX];
	if (snprintf(pattern, sizeof pattern, "%s/pcsc/*.sock", runtime) >=
	    (int)sizeof pattern)
		return -1;
	glob_t paths = {0};
	int result = glob(pattern, 0, NULL, &paths);
	if (result && result != GLOB_NOMATCH)
		return -1;
	int found = 0;
	for (size_t i = 0; i < paths.gl_pathc; i++) {
		const char *name = strrchr(paths.gl_pathv[i], '/') + 1;
		long pid;
		unsigned long fd;
		if (!parse_channel_name(name, &pid, &fd) ||
		    kill((pid_t)pid, 0) || !private_socket(paths.gl_pathv[i]))
			continue;
		char id[96];
		snprintf(id, sizeof id, "%ld:%lu", pid, fd);
		if ((client && strcmp(client, id) != 0) ||
		    snprintf(selected, size, "%s", paths.gl_pathv[i]) >=
			(int)size)
			continue;
		found++;
		if (print)
			printf("%s\t%s\n", id, selected);
	}
	globfree(&paths);
	return found;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--version")) {
		puts(BASE_VERSION);
		return 0;
	}
	if (argc < 2 || !strcmp(argv[1], "--help")) {
		puts(usage);
		return argc < 2 ? 2 : 0;
	}
	options o;
	if (!parse(argc, argv, &o) || getuid() != geteuid())
		return 2;

	char selected[PATH_MAX] = "";
	int found = 0;
	if (o.exact) {
		if (!private_socket(o.exact) ||
		    snprintf(selected, sizeof selected, "%s", o.exact) >=
			(int)sizeof selected) {
			fputs("PC/SC channel must be a private socket owned by "
			      "the current user\n",
			      stderr);
			return 1;
		}
		found = 1;
	} else {
		char dir[PATH_MAX];
		if (!o.runtime) {
			const char *xdg = getenv("XDG_RUNTIME_DIR");
			if (!xdg || snprintf(dir, sizeof dir, "%s/vnc-bridge",
					     xdg) >= (int)sizeof dir) {
				fputs("XDG_RUNTIME_DIR is unset; pass "
				      "--runtime-dir\n",
				      stderr);
				return 2;
			}
			o.runtime = dir;
		}
		found = find_channels(o.runtime, o.client, o.status, selected,
				      sizeof selected);
		if (found < 0)
			return 1;
	}
	if (o.status)
		return found ? 0 : 1;
	if (found != 1) {
		fputs(found ? "Several PC/SC viewers: specify --client PID:FD\n"
			    : "No PC/SC channel found\n",
		      stderr);
		return 1;
	}
	if (setenv("PCSCLITE_CSOCK_NAME", selected, 1)) {
		perror("PCSCLITE_CSOCK_NAME");
		return 1;
	}
	execvp(argv[o.command], argv + o.command);
	int error = errno;
	perror(argv[o.command]);
	return error == ENOENT ? 127 : 126;
}
