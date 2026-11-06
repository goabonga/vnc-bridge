/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "harness.h"
#include "run.h"
#include <ftw.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>

#define SHELL "build/bin/vnc-bridge-shell"

static char root[] = "/tmp/tvx-shell-XXXXXX";
static char socket_path[300];
static int server;

static void listen_on(const char *path)
{
	server = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof addr.sun_path, "%.100s", path);
	unlink(path);
	CHECK(bind(server, (struct sockaddr *)&addr, sizeof addr) == 0);
	CHECK(listen(server, 1) == 0);
}

struct peer {
	child_process child;
	int fd; /* accepted connection, the viewer side of the channel */
};

static struct peer start(void)
{
	listen_on(socket_path);
	char *argv[] = {SHELL, "--connect", "--socket", socket_path, NULL};
	struct peer p;
	p.child = spawn_child(argv);
	struct pollfd pf = {server, POLLIN, 0};
	CHECK(poll(&pf, 1, 3000) > 0);
	p.fd = accept(server, NULL, NULL);
	CHECK(p.fd >= 0);
	return p;
}

static size_t read_bytes(int fd, unsigned char *buf, size_t size)
{
	size_t got = 0;
	while (got < size) {
		struct pollfd pf = {fd, POLLIN, 0};
		if (poll(&pf, 1, 3000) <= 0)
			break;
		ssize_t n = read(fd, buf + got, size - got);
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	return got;
}

static void send_bytes(int fd, const void *data, size_t size)
{
	CHECK(write(fd, data, size) == (ssize_t)size);
}

/* Read the initial size frame sent by the program. */
static void expect_size_frame(struct peer *p)
{
	unsigned char f[7];
	CHECK(read_bytes(p->fd, f, sizeof f) == sizeof f);
	static const unsigned char want[] = {'O', 0, 4, 0, 24, 0, 80};
	CHECK(memcmp(f, want, sizeof want) == 0);
}

static void finish_peer(struct peer *p, int expected_status)
{
	close(p->fd);
	CHECK(reap_child(&p->child, 3000) == expected_status);
	close(server);
}

static void test_output_and_exit_status(void)
{
	struct peer p = start();
	expect_size_frame(&p);
	send_bytes(p.fd, "D\0\3abcE\0\1\x07", 10);
	char out[64] = "";
	size_t n = read_available(p.child.out, out, sizeof out, 3000);
	CHECK(n == 3 && !strcmp(out, "abc"));
	finish_peer(&p, 7);
}

static void test_input(void)
{
	struct peer p = start();
	expect_size_frame(&p);

	send_bytes(p.child.in, "hello", 5);
	unsigned char f[8];
	CHECK(read_bytes(p.fd, f, sizeof f) == sizeof f);
	CHECK(memcmp(f, "D\0\5hello", 8) == 0);

	/* Input is split in frames of at most 55 bytes. */
	unsigned char big[200];
	memset(big, 'x', sizeof big);
	send_bytes(p.child.in, big, sizeof big);
	size_t total = 0;
	while (total < sizeof big) {
		unsigned char header[3];
		CHECK(read_bytes(p.fd, header, 3) == 3);
		CHECK(header[0] == 'D' && header[1] == 0 && header[2] >= 1 &&
		      header[2] <= 55);
		unsigned char payload[55];
		CHECK(read_bytes(p.fd, payload, header[2]) == header[2]);
		total += header[2];
	}
	CHECK(total == sizeof big);

	/* End of input is sent as Ctrl+D. */
	close(p.child.in);
	p.child.in = -1;
	unsigned char eot[4];
	CHECK(read_bytes(p.fd, eot, sizeof eot) == sizeof eot);
	CHECK(memcmp(eot, "D\0\1\4", 4) == 0);
	send_bytes(p.fd, "E\0\1\0", 4);
	finish_peer(&p, 0);
}

static void test_output_is_flushed_before_exit(void)
{
	struct peer p = start();
	expect_size_frame(&p);
	unsigned char chunk[3 + 55];
	chunk[0] = 'D';
	chunk[1] = 0;
	chunk[2] = 55;
	memset(chunk + 3, 'z', 55);
	for (int i = 0; i < 20; i++)
		send_bytes(p.fd, chunk, sizeof chunk);
	send_bytes(p.fd, "E\0\1\3", 4);
	size_t total = 0;
	char buf[2048];
	while (total < 1100) {
		size_t n = read_available(p.child.out, buf, sizeof buf, 3000);
		if (!n)
			break;
		total += n;
	}
	CHECK(total == 1100);
	finish_peer(&p, 3);
}

static void test_bad_frames(void)
{
	static const struct {
		const char *data;
		size_t size;
	} bad[] = {
	    {"D\0\x38", 3}, /* longer than a frame can be */
	    {"X\0\0", 3},   /* unknown kind */
	    {"E\0\2ab", 5}, /* exit status of the wrong size */
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		struct peer p = start();
		expect_size_frame(&p);
		send_bytes(p.fd, bad[i].data, bad[i].size);
		finish_peer(&p, 1);
	}
}

static void test_already_open(void)
{
	struct peer p = start();
	expect_size_frame(&p);
	send_bytes(p.fd, "E\0\1}", 4);
	char err[512] = "";
	read_available(p.child.err, err, sizeof err, 3000);
	CHECK(strstr(
		  err,
		  "Un terminal est d\xc3\xa9j\xc3\xa0 ouvert pour ce viewer") !=
	      NULL);
	finish_peer(&p, 1);
}

static void test_connection_closed(void)
{
	struct peer p = start();
	expect_size_frame(&p);
	close(p.fd);
	CHECK(reap_child(&p.child, 3000) == 1);
	close(server);
}

static void test_signal(void)
{
	struct peer p = start();
	expect_size_frame(&p);
	kill(p.child.pid, SIGTERM);
	CHECK(reap_child(&p.child, 3000) == 128 + SIGTERM);
	close(p.fd);
	close(server);
}

/* With a terminal on standard input the program reports its size, follows
 * resizes and passes keystrokes through unmodified (raw mode). */
static void test_terminal(void)
{
	listen_on(socket_path);
	int master, slave;
	struct winsize size = {.ws_row = 30, .ws_col = 100};
	CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
	int out[2];
	CHECK(pipe(out) == 0);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		ioctl(slave, TIOCSCTTY, 0);
		dup2(slave, 0);
		dup2(out[1], 1);
		close(master);
		close(slave);
		close(out[0]);
		close(out[1]);
		execl(SHELL, SHELL, "--connect", "--socket", socket_path,
		      (char *)NULL);
		_exit(127);
	}
	close(slave);
	close(out[1]);
	struct pollfd pf = {server, POLLIN, 0};
	CHECK(poll(&pf, 1, 3000) > 0);
	int fd = accept(server, NULL, NULL);
	CHECK(fd >= 0);

	unsigned char f[7];
	CHECK(read_bytes(fd, f, sizeof f) == sizeof f);
	static const unsigned char initial[] = {'O', 0, 4, 0, 30, 0, 100};
	CHECK(memcmp(f, initial, sizeof f) == 0);

	struct winsize bigger = {.ws_row = 40, .ws_col = 120};
	CHECK(ioctl(master, TIOCSWINSZ, &bigger) == 0);
	CHECK(read_bytes(fd, f, sizeof f) == sizeof f);
	static const unsigned char resized_frame[] = {'W', 0, 4, 0, 40, 0, 120};
	CHECK(memcmp(f, resized_frame, sizeof f) == 0);

	/* Raw mode: no echo and no translation of the carriage return. */
	CHECK(write(master, "ls\r", 3) == 3);
	unsigned char typed[6];
	CHECK(read_bytes(fd, typed, sizeof typed) == sizeof typed);
	CHECK(memcmp(typed, "D\0\3ls\r", 6) == 0);

	send_bytes(fd, "E\0\1\0", 4);
	close(fd);
	int status = 0;
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(master);
	close(out[0]);
	close(server);
}

static void test_unreachable(void)
{
	char *argv[] = {SHELL, "--connect", "--socket",
			"/tmp/tvx-no-such-socket", NULL};
	child_process c = spawn_child(argv);
	char err[512] = "";
	read_available(c.err, err, sizeof err, 3000);
	CHECK(strstr(err, "connexion ferm\xc3\xa9"
			  "e") != NULL);
	CHECK(reap_child(&c, 3000) == 1);
}

static void test_arguments(void)
{
	char *bad[] = {SHELL, "--bogus", NULL};
	child_process c = spawn_child(bad);
	CHECK(reap_child(&c, 3000) == 2);
	char *help[] = {SHELL, "--help", NULL};
	c = spawn_child(help);
	char out[256] = "";
	read_available(c.out, out, sizeof out, 3000);
	CHECK(strncmp(out, "Usage: vnc-bridge-shell", 23) == 0);
	CHECK(reap_child(&c, 3000) == 0);
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
	snprintf(socket_path, sizeof socket_path, "%s/shell.sock", root);
	signal(SIGPIPE, SIG_IGN);
	test_output_and_exit_status();
	test_input();
	test_output_is_flushed_before_exit();
	test_bad_frames();
	test_already_open();
	test_connection_closed();
	test_signal();
	test_terminal();
	test_unreachable();
	test_arguments();
	CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
	{
		char *version_argv[] = {"build/bin/vnc-bridge-shell",
					"--version", NULL};
		run_result version_run = run_cmd(version_argv, NULL);
		CHECK(version_run.status == 0);
		CHECK_STR(version_run.out, BASE_VERSION "\n");
		free(version_run.out);
	}
	return vncb_finish("test_shell_channel");
}
