/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#define main vncb_shell_main
#include "../src/vnc-bridge-shell.c"
#undef main
#include "harness.h"
#include "run.h"
#include <ftw.h>

#define SHELL "build/bin/vnc-bridge-shell"

static char root[] = "/tmp/vncb-select-XXXXXX";
static char runtime[300];
static int servers[8];
static int open_servers;

static int listen_at(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof addr.sun_path, "%.100s", path);
	CHECK(bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
	CHECK(listen(fd, 4) == 0);
	servers[open_servers] = fd;
	return open_servers++;
}

static void shell_socket(char *path, size_t size, const char *directory,
			 long pid, int number)
{
	snprintf(path, size, "%s/shell/%ld-%d.sock", directory, pid, number);
}

static void write_status(const char *directory, const char *json)
{
	char path[400];
	snprintf(path, sizeof path, "%s/status.json", directory);
	write_file(path, json, 0600);
}

static void test_choose(void)
{
	char dir[300], path[400], chosen[400], id5[64], id6[64], json[600];
	snprintf(dir, sizeof dir, "%s/choose", root);
	snprintf(path, sizeof path, "%s/shell", dir);
	CHECK(mkdir(dir, 0700) == 0);
	CHECK(mkdir(path, 0700) == 0);

	/* No channel at all. */
	CHECK(choose(chosen, sizeof chosen, dir, NULL, NULL) == -1);

	char first[400], second[400];
	shell_socket(first, sizeof first, dir, getpid(), 5);
	shell_socket(second, sizeof second, dir, getpid(), 6);
	listen_at(first);
	CHECK(choose(chosen, sizeof chosen, dir, NULL, NULL) == 0);
	CHECK_STR(chosen, first);

	listen_at(second);
	snprintf(id5, sizeof id5, "%d:5", getpid());
	snprintf(id6, sizeof id6, "%d:6", getpid());
	snprintf(json, sizeof json,
		 "{\"clients\":[{\"id\":\"%s\",\"hostname\":\"nano\"},"
		 "{\"id\":\"%s\",\"hostname\":\"other\"}]}",
		 id5, id6);
	write_status(dir, json);

	/* Two viewers: the choice must be narrowed. */
	CHECK(choose(chosen, sizeof chosen, dir, NULL, NULL) == -1);
	CHECK(choose(chosen, sizeof chosen, dir, "nano", NULL) == 0);
	CHECK_STR(chosen, first);
	CHECK(choose(chosen, sizeof chosen, dir, "other", NULL) == 0);
	CHECK_STR(chosen, second);
	CHECK(choose(chosen, sizeof chosen, dir, "absent", NULL) == -1);
	CHECK(choose(chosen, sizeof chosen, dir, NULL, id6) == 0);
	CHECK_STR(chosen, second);
	CHECK(choose(chosen, sizeof chosen, dir, NULL, "1:2") == -1);
	CHECK(choose(chosen, sizeof chosen, dir, "nano", id6) == -1);

	/* Stale processes and files which are not sockets are ignored. */
	char stale[400], plain[400];
	shell_socket(stale, sizeof stale, dir, 2147483646, 1);
	listen_at(stale);
	snprintf(plain, sizeof plain, "%s/shell/%d-7.sock", dir, getpid());
	write_file(plain, "", 0600);
	CHECK(choose(chosen, sizeof chosen, dir, "nano", NULL) == 0);
	CHECK_STR(chosen, first);
}

/* Run the program with a server which immediately reports exit status
 * `code`; the result tells which socket was used. */
/* Returns the exit status; *which is the index of the listener which got the
 * connection, or -1. */
static int connect_via(char *const argv[], int *which)
{
	child_process c = spawn_child(argv);
	*which = -1;
	struct pollfd pf[8];
	for (int i = 0; i < open_servers; i++)
		pf[i] = (struct pollfd){servers[i], POLLIN, 0};
	if (poll(pf, (nfds_t)open_servers, 3000) > 0) {
		for (int i = 0; i < open_servers; i++) {
			if (!(pf[i].revents & POLLIN))
				continue;
			int fd = accept(servers[i], NULL, NULL);
			*which = fd >= 0 ? i : -1;
			unsigned char frame_in[7];
			CHECK(read(fd, frame_in, sizeof frame_in) ==
			      (ssize_t)sizeof frame_in);
			CHECK(write(fd, "E\0\1\x2a", 4) == 4);
			close(fd);
			break;
		}
	}
	return reap_child(&c, 3000);
}

static void test_directory_resolution(void)
{
	char dir[300], first[400];
	snprintf(dir, sizeof dir, "%s/resolve", root);
	char shell_dir[400];
	snprintf(shell_dir, sizeof shell_dir, "%s/shell", dir);
	CHECK(mkdir(dir, 0700) == 0);
	CHECK(mkdir(shell_dir, 0700) == 0);
	shell_socket(first, sizeof first, dir, getpid(), 9);
	int first_index = listen_at(first);
	int which;
	/* Isolate the test from the user's own configuration and environment.
	 */
	setenv("HOME", root, 1);
	unsetenv("VNC_BRIDGE_SOCKET");
	unsetenv("XDG_RUNTIME_DIR");

	/* --runtime-dir */
	char *by_option[] = {SHELL, "--connect", "--runtime-dir", dir, NULL};
	CHECK(connect_via(by_option, &which) == 42 && which == first_index);

	/* The directory of $VNC_BRIDGE_SOCKET. */
	char socket_env[400];
	snprintf(socket_env, sizeof socket_env, "%s/telemetry.sock", dir);
	setenv("VNC_BRIDGE_SOCKET", socket_env, 1);
	setenv("HOME", root, 1);
	char *by_socket_env[] = {SHELL, "--connect", NULL};
	CHECK(connect_via(by_socket_env, &which) == 42 && which == first_index);
	unsetenv("VNC_BRIDGE_SOCKET");

	/* $XDG_RUNTIME_DIR/vnc-bridge. */
	char xdg[300], xdg_dir[400], xdg_shell[500], xdg_socket[600];
	snprintf(xdg, sizeof xdg, "%s/xdg", root);
	snprintf(xdg_dir, sizeof xdg_dir, "%s/vnc-bridge", xdg);
	snprintf(xdg_shell, sizeof xdg_shell, "%s/shell", xdg_dir);
	CHECK(mkdir(xdg, 0700) == 0);
	CHECK(mkdir(xdg_dir, 0700) == 0);
	CHECK(mkdir(xdg_shell, 0700) == 0);
	shell_socket(xdg_socket, sizeof xdg_socket, xdg_dir, getpid(), 3);
	int xdg_index = listen_at(xdg_socket);
	setenv("XDG_RUNTIME_DIR", xdg, 1);
	CHECK(connect_via(by_socket_env, &which) == 42 && which == xdg_index);

	/* The configuration file supplies the directory and the hostname. */
	char home[300], config_dir[400], config[500];
	snprintf(home, sizeof home, "%s/home", root);
	snprintf(config_dir, sizeof config_dir, "%s/.config", home);
	CHECK(mkdir(home, 0700) == 0);
	CHECK(mkdir(config_dir, 0700) == 0);
	snprintf(config_dir, sizeof config_dir, "%s/.config/vnc-bridge", home);
	CHECK(mkdir(config_dir, 0700) == 0);
	snprintf(config, sizeof config, "%s/shell.json", config_dir);
	char json[600];
	snprintf(json, sizeof json,
		 "{\"runtime_dir\":\"%s\",\"hostname\":\"nano\"}", dir);
	write_file(config, json, 0600);
	setenv("HOME", home, 1);

	char second[400];
	shell_socket(second, sizeof second, dir, getpid(), 10);
	int second_index = listen_at(second);
	char id9[64], id10[64], status[600];
	snprintf(id9, sizeof id9, "%d:9", getpid());
	snprintf(id10, sizeof id10, "%d:10", getpid());
	snprintf(status, sizeof status,
		 "{\"clients\":[{\"id\":\"%s\",\"hostname\":\"nano\"},"
		 "{\"id\":\"%s\",\"hostname\":\"other\"}]}",
		 id9, id10);
	write_status(dir, status);
	CHECK(connect_via(by_socket_env, &which) == 42 && which == first_index);
	/* --client overrides the configured hostname. */
	char client[64];
	snprintf(client, sizeof client, "%d:10", getpid());
	char *by_client[] = {SHELL, "--connect", "--client", client, NULL};
	CHECK(connect_via(by_client, &which) == 42 && which == second_index);
	/* A bad configuration is ignored. */
	write_file(config, "not json", 0600);
	unsetenv("VNC_BRIDGE_SOCKET");
	CHECK(connect_via(by_socket_env, &which) == 42 && which == xdg_index);
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
	snprintf(runtime, sizeof runtime, "%s/runtime", root);
	signal(SIGPIPE, SIG_IGN);
	test_choose();
	test_directory_resolution();
	for (int i = 0; i < open_servers; i++)
		close(servers[i]);
	CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
	return vncb_finish("test_shell_select");
}
