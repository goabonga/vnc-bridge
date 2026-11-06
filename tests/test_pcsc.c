/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "harness.h"
#include "run.h"
#include <ftw.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define PCSC "build/bin/vnc-bridge-pcsc"

static char root[] = "/tmp/tvx-pcsc-XXXXXX";
static int sockets[8];
static int open_sockets;

/* Create a listening-less Unix socket file <root>/pcsc/<pid>-<number>.sock. */
static void endpoint(char *path, size_t size, int pid, int number)
{
	char directory[300];
	snprintf(directory, sizeof directory, "%s/pcsc", root);
	mkdir(directory, 0700);
	snprintf(path, size, "%s/%d-%d.sock", directory, pid, number);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof addr.sun_path, "%.100s", path);
	CHECK(bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
	CHECK(chmod(path, 0600) == 0);
	sockets[open_sockets++] = fd;
}

static int status_code(char *const argv[])
{
	run_result r = run_cmd(argv, NULL);
	free(r.out);
	return r.status;
}

static void test_selection(void)
{
	char first[400], second[400], client[64];
	endpoint(first, sizeof first, getpid(), 7);
	endpoint(second, sizeof second, getpid(), 8);
	snprintf(client, sizeof client, "%d:7", getpid());

	/* Two viewers: the choice must be explicit. */
	char *ambiguous[] = {PCSC,   "run", "--runtime-dir", root, "--",
			     "true", NULL};
	CHECK(status_code(ambiguous) == 1);

	/* The selected socket reaches the program, whose arguments are not
	 * interpreted by a shell. */
	char *selected[] = {
	    PCSC,
	    "run",
	    "--runtime-dir",
	    root,
	    "--client",
	    client,
	    "--",
	    "sh",
	    "-c",
	    "printf '%s\\n%s\\n' \"$PCSCLITE_CSOCK_NAME\" \"$1\"",
	    "sh",
	    "$(false); literal argument",
	    NULL};
	run_result r = run_cmd(selected, NULL);
	CHECK(r.status == 0);
	char want[600];
	snprintf(want, sizeof want, "%s\n$(false); literal argument\n", first);
	CHECK_STR(r.out, want);
	free(r.out);

	char *list[] = {PCSC, "status", "--runtime-dir", root, NULL};
	r = run_cmd(list, NULL);
	CHECK(r.status == 0);
	char listing[900];
	snprintf(listing, sizeof listing, "%d:7\t%s\n%d:8\t%s\n", getpid(),
		 first, getpid(), second);
	CHECK_STR(r.out, listing);
	free(r.out);

	/* Sockets of processes which no longer exist are ignored. */
	char stale[400];
	endpoint(stale, sizeof stale, 2147483646, 1);
	r = run_cmd(list, NULL);
	CHECK_STR(r.out, listing);
	free(r.out);

	/* The default directory follows XDG_RUNTIME_DIR. */
	char xdg[200], pcsc_dir[300];
	snprintf(xdg, sizeof xdg, "%s/xdg", root);
	CHECK(mkdir(xdg, 0700) == 0);
	snprintf(pcsc_dir, sizeof pcsc_dir, "%s/vnc-bridge", xdg);
	CHECK(mkdir(pcsc_dir, 0700) == 0);
	snprintf(pcsc_dir, sizeof pcsc_dir, "%s/vnc-bridge/pcsc", xdg);
	CHECK(mkdir(pcsc_dir, 0700) == 0);
	char other[600];
	snprintf(other, sizeof other, "%s/%d-3.sock", pcsc_dir, getpid());
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof addr.sun_path, "%.100s", other);
	CHECK(bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
	chmod(other, 0600);
	sockets[open_sockets++] = fd;
	setenv("XDG_RUNTIME_DIR", xdg, 1);
	char *by_default[] = {PCSC, "status", NULL};
	r = run_cmd(by_default, NULL);
	snprintf(listing, sizeof listing, "%d:3\t%s\n", getpid(), other);
	CHECK_STR(r.out, listing);
	free(r.out);
	unsetenv("XDG_RUNTIME_DIR");
	char *no_runtime[] = {PCSC, "status", NULL};
	CHECK(status_code(no_runtime) == 2);
}

static void test_exact_socket(void)
{
	char path[400];
	endpoint(path, sizeof path, getpid(), 4);

	char *exit_seven[] = {PCSC, "run", "--socket", path, "--",
			      "sh", "-c",  "exit 7",   NULL};
	CHECK(status_code(exit_seven) == 7);
	char *missing[] = {PCSC, "run", "--socket",
			   path, "--",	"/definitely/missing/program",
			   NULL};
	CHECK(status_code(missing) == 127);
	char *not_executable[] = {PCSC, "run",	       "--socket", path,
				  "--", "/etc/passwd", NULL};
	CHECK(status_code(not_executable) == 126);

	/* A symbolic link, a missing file, a public socket and a public
	 * directory are refused. */
	char link[400];
	snprintf(link, sizeof link, "%s/pcsc/link.sock", root);
	CHECK(symlink(path, link) == 0);
	char *via_link[] = {PCSC, "run", "--socket", link, "--", "true", NULL};
	CHECK(status_code(via_link) == 1);
	char absent[400];
	snprintf(absent, sizeof absent, "%s/absent.sock", root);
	char *no_file[] = {PCSC, "run", "--socket", absent, "--", "true", NULL};
	CHECK(status_code(no_file) == 1);
	char *plain_file[] = {PCSC, "run",  "--socket", "/etc/passwd",
			      "--", "true", NULL};
	CHECK(status_code(plain_file) == 1);

	char *with_socket[] = {PCSC, "run",  "--socket", path,
			       "--", "true", NULL};
	CHECK(status_code(with_socket) == 0);
	CHECK(chmod(path, 0666) == 0);
	CHECK(status_code(with_socket) == 1);
	CHECK(chmod(path, 0600) == 0);
	char directory[300];
	snprintf(directory, sizeof directory, "%s/pcsc", root);
	CHECK(chmod(directory, 0755) == 0);
	CHECK(status_code(with_socket) == 1);
	CHECK(chmod(directory, 0700) == 0);
	unlink(link);
}

static void test_arguments(void)
{
	char empty[300];
	snprintf(empty, sizeof empty, "%s/empty", root);
	CHECK(mkdir(empty, 0700) == 0);

	/* No channel at all. */
	char *status_none[] = {PCSC, "status", "--runtime-dir", empty, NULL};
	CHECK(status_code(status_none) == 1);
	char *run_none[] = {PCSC,   "run", "--runtime-dir", empty, "--",
			    "true", NULL};
	CHECK(status_code(run_none) == 1);

	char *unknown[] = {PCSC, "unknown", NULL};
	CHECK(status_code(unknown) == 2);
	char *no_command[] = {PCSC, "run", NULL};
	CHECK(status_code(no_command) == 2);
	char *status_client[] = {PCSC, "status", "--client", "1:2", NULL};
	CHECK(status_code(status_client) == 2);
	char *status_command[] = {PCSC, "status", "--", "true", NULL};
	CHECK(status_code(status_command) == 2);
	char *exclusive[] = {PCSC,  "run", "--socket", "x", "--client",
			     "1:2", "--",  "true",     NULL};
	CHECK(status_code(exclusive) == 2);
	char *bare[] = {PCSC, NULL};
	CHECK(status_code(bare) == 2);
	char *bogus[] = {PCSC, "status", "--bogus", NULL};
	CHECK(status_code(bogus) == 2);

	char *help[] = {PCSC, "--help", NULL};
	run_result r = run_cmd(help, NULL);
	CHECK(r.status == 0);
	CHECK(r.out != NULL &&
	      strncmp(r.out, "Usage: vnc-bridge-pcsc status", 29) == 0);
	free(r.out);
}

static int remove_entry(const char *path, const struct stat *st, int flag,
			struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

int main(void)
{
	CHECK(mkdtemp(root) != NULL);
	test_selection();
	test_exact_socket();
	test_arguments();
	for (int i = 0; i < open_sockets; i++)
		close(sockets[i]);
	CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
	{
		char *version_argv[] = {"build/bin/vnc-bridge-pcsc",
					"--version", NULL};
		run_result version_run = run_cmd(version_argv, NULL);
		CHECK(version_run.status == 0);
		CHECK_STR(version_run.out, BASE_VERSION "\n");
		free(version_run.out);
	}
	return vncb_finish("test_pcsc");
}
