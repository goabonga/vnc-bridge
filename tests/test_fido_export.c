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

/* A simulated hidraw device is one end of a SOCK_SEQPACKET pair; the child
 * runs export_reports() on the other end with pipes as stdin and stdout. */
struct session {
	pid_t pid;
	int in;	    /* write end of the child's stdin */
	int out;    /* read end of the child's stdout */
	int device; /* the test's end of the simulated device */
};

static struct session start_export(void)
{
	struct session s = {0, -1, -1, -1};
	int to_child[2], from_child[2], pair[2];
	CHECK(pipe(to_child) == 0);
	CHECK(pipe(from_child) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
	s.pid = fork();
	if (s.pid == 0) {
		dup2(to_child[0], 0);
		dup2(from_child[1], 1);
		close(to_child[0]);
		close(to_child[1]);
		close(from_child[0]);
		close(from_child[1]);
		close(pair[0]);
		struct sigaction action = {.sa_handler = stop};
		sigaction(SIGTERM, &action, NULL);
		signal(SIGPIPE, SIG_IGN);
		struct hidraw_devinfo info = {
		    .bustype = BUS_USB, .vendor = 0x0483, .product = 0x5741};
		_exit(export_reports(pair[1], &info));
	}
	close(to_child[0]);
	close(from_child[1]);
	close(pair[1]);
	s.in = to_child[1];
	s.out = from_child[0];
	s.device = pair[0];
	return s;
}

static bool read_exact(int fd, unsigned char *buf, size_t size)
{
	size_t got = 0;
	while (got < size) {
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, 3000) <= 0)
			return false;
		ssize_t n = read(fd, buf + got, size - got);
		if (n <= 0)
			return false;
		got += (size_t)n;
	}
	return true;
}

/* Wait for the child, then release the test's ends. The child's output and
 * device stay open until it has exited so closing them cannot influence it. */
static int finish(struct session *s)
{
	int status = 0;
	double end = clocksec(CLOCK_MONOTONIC) + 3;
	int result = -1;
	for (;;) {
		if (waitpid(s->pid, &status, WNOHANG) == s->pid) {
			result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
			break;
		}
		if (clocksec(CLOCK_MONOTONIC) > end) {
			kill(s->pid, SIGKILL);
			waitpid(s->pid, &status, 0);
			break;
		}
		poll(NULL, 0, 10);
	}
	if (s->in >= 0)
		close(s->in);
	close(s->out);
	close(s->device);
	return result;
}

static void send_bytes(struct session *s, const void *data, size_t size)
{
	CHECK(write(s->in, data, size) == (ssize_t)size);
}

static void test_relay(void)
{
	struct session s = start_export();
	unsigned char hello[7];
	CHECK(read_exact(s.out, hello, sizeof hello));
	CHECK(memcmp(hello, "I\0\4\x04\x83\x57\x41", 7) == 0);

	/* Reports arrive fragmented and reach the device with a zero report id.
	 */
	unsigned char packet[64], frame_in[67];
	for (int i = 0; i < 64; i++)
		packet[i] = (unsigned char)i;
	frame_in[0] = 'D';
	frame_in[1] = 0;
	frame_in[2] = FIDO_PACKET;
	memcpy(frame_in + 3, packet, 64);
	send_bytes(&s, frame_in, 1);
	poll(NULL, 0, 20);
	send_bytes(&s, frame_in + 1, 1);
	poll(NULL, 0, 20);
	send_bytes(&s, frame_in + 2, 7);
	poll(NULL, 0, 20);
	send_bytes(&s, frame_in + 9, 58);
	unsigned char at_device[128];
	struct pollfd p = {s.device, POLLIN, 0};
	CHECK(poll(&p, 1, 3000) > 0);
	ssize_t n = recv(s.device, at_device, sizeof at_device, 0);
	CHECK(n == 65);
	CHECK(at_device[0] == 0);
	CHECK(memcmp(at_device + 1, packet, 64) == 0);

	/* Reports from the device are framed on standard output. */
	unsigned char response[64], frame_out[67];
	for (int i = 0; i < 64; i++)
		response[i] = (unsigned char)(63 - i);
	CHECK(send(s.device, response, 64, 0) == 64);
	CHECK(read_exact(s.out, frame_out, sizeof frame_out));
	CHECK(memcmp(frame_out, "D\0\x40", 3) == 0);
	CHECK(memcmp(frame_out + 3, response, 64) == 0);

	/* Closing standard input at a frame boundary is a normal end. */
	close(s.in);
	s.in = -1;
	CHECK(finish(&s) == 0);
}

static void test_rejections(void)
{
	static const struct {
		const char *data;
		size_t size;
	} bad[] = {
	    {"D\0\x41", 3},   /* report larger than a packet */
	    {"I\0\4abcd", 7}, /* the host never sends an identity */
	    {"D\0", 2},	      /* ends inside a header */
	    {"D\0\3bad", 6},  /* short report */
	    {"X\0\0", 3},     /* unknown frame */
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		struct session s = start_export();
		unsigned char hello[7];
		CHECK(read_exact(s.out, hello, sizeof hello));
		send_bytes(&s, bad[i].data, bad[i].size);
		close(s.in);
		s.in = -1;
		CHECK(finish(&s) == 1);
	}
}

static void test_termination(void)
{
	struct session s = start_export();
	unsigned char hello[7];
	CHECK(read_exact(s.out, hello, sizeof hello));
	kill(s.pid, SIGTERM);
	CHECK(finish(&s) == 0);
}

static void test_identity(void)
{
	int fd = open("/dev/null", O_RDONLY);
	struct stat st;
	CHECK(fstat(fd, &st) == 0);
	char identity[160];
	snprintf(identity, sizeof identity, "%ju:%ju:%ju", (uintmax_t)st.st_dev,
		 (uintmax_t)st.st_ino, (uintmax_t)st.st_rdev);
	CHECK(identity_matches(fd, identity));
	snprintf(identity, sizeof identity, "%ju:%ju:%ju", (uintmax_t)st.st_dev,
		 (uintmax_t)st.st_ino + 1, (uintmax_t)st.st_rdev);
	CHECK(!identity_matches(fd, identity));
	snprintf(identity, sizeof identity, "%ju:%ju:%ju:extra",
		 (uintmax_t)st.st_dev, (uintmax_t)st.st_ino,
		 (uintmax_t)st.st_rdev);
	CHECK(!identity_matches(fd, identity));
	CHECK(!identity_matches(fd, "-1:2:3"));
	CHECK(!identity_matches(fd, "1:2"));
	CHECK(!identity_matches(fd, "1:2:99999999999999999999999999"));
	CHECK(!identity_matches(fd, ""));
	close(fd);
}

static int cli_status(char *const argv[])
{
	run_result r = run_cmd(argv, NULL);
	free(r.out);
	return r.status;
}

static void test_command_line(void)
{
	char *none[] = {"build/bin/vnc-bridge-fido", NULL};
	CHECK(cli_status(none) == 2);
	char *help[] = {"build/bin/vnc-bridge-fido", "--help", NULL};
	run_result r = run_cmd(help, NULL);
	CHECK(r.status == 0);
	CHECK(r.out && strncmp(r.out, "Usage: vnc-bridge-fido list", 27) == 0);
	free(r.out);
	char *list_extra[] = {"build/bin/vnc-bridge-fido", "list", "x", NULL};
	CHECK(cli_status(list_extra) == 2);
	char *export_no_device[] = {"build/bin/vnc-bridge-fido", "export",
				    NULL};
	CHECK(cli_status(export_no_device) == 2);
	char *unknown[] = {"build/bin/vnc-bridge-fido", "bogus", NULL};
	CHECK(cli_status(unknown) == 2);
	char *bad_option[] = {"build/bin/vnc-bridge-fido", "export", "--bogus",
			      NULL};
	CHECK(cli_status(bad_option) == 2);
	/* /dev/null is not a USB FIDO interface. */
	char *not_fido[] = {"build/bin/vnc-bridge-fido", "export", "--device",
			    "/dev/null", NULL};
	/* Standard input and output are not terminals under the test runner. */
	CHECK(cli_status(not_fido) == 1);

	/* list prints one "path vendor:product name" line per FIDO interface.
	 */
	char *list[] = {"build/bin/vnc-bridge-fido", "list", NULL};
	r = run_cmd(list, NULL);
	CHECK(r.status == 0);
	for (char *line = strtok(r.out, "\n"); line;
	     line = strtok(NULL, "\n")) {
		unsigned vendor, product;
		int used = 0;
		CHECK(sscanf(line, "/dev/hidraw%*u %4x:%4x %n", &vendor,
			     &product, &used) == 2 &&
		      used > 0 && line[used] != 0);
	}
	free(r.out);
}

int main(void)
{
	test_relay();
	test_rejections();
	test_termination();
	test_identity();
	test_command_line();
	{
		char *version_argv[] = {"build/bin/vnc-bridge-fido",
					"--version", NULL};
		run_result version_run = run_cmd(version_argv, NULL);
		CHECK(version_run.status == 0);
		CHECK_STR(version_run.out, BASE_VERSION "\n");
		free(version_run.out);
	}
	return vncb_finish("test_fido_export");
}
