/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#define main fido_cli_main
#include "../src/vnc-bridge-fido.c"
#undef main
#include "harness.h"
#include "run.h"
#include <ftw.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

/* The simulated kernel is one end of a SOCK_SEQPACKET pair standing for
 * /dev/uhid; the viewer channel is one end of a stream socket pair. The child
 * runs import_reports() on the other ends. */
struct session {
	pid_t pid;
	int channel;
	int kernel;
};

static struct session start_import(void)
{
	struct session s;
	int stream[2], seq[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
	CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, seq) == 0);
	s.pid = fork();
	if (s.pid == 0) {
		close(stream[0]);
		close(seq[0]);
		int null = open("/dev/null", O_WRONLY);
		dup2(null, 2);
		struct sigaction action = {.sa_handler = stop};
		sigaction(SIGTERM, &action, NULL);
		signal(SIGPIPE, SIG_IGN);
		_exit(import_reports(stream[1], seq[1]));
	}
	close(stream[1]);
	close(seq[1]);
	s.channel = stream[0];
	s.kernel = seq[0];
	return s;
}

static int finish(struct session *s)
{
	int status = 0, result = -1;
	double end = clocksec(CLOCK_MONOTONIC) + 3;
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
	close(s->channel);
	close(s->kernel);
	return result;
}

static void send_frame(struct session *s, char kind, const void *payload,
		       size_t size)
{
	unsigned char header[] = {(unsigned char)kind,
				  (unsigned char)(size >> 8),
				  (unsigned char)size};
	CHECK(write(s->channel, header, sizeof header) ==
	      (ssize_t)sizeof header);
	if (size)
		CHECK(write(s->channel, payload, size) == (ssize_t)size);
}

/* Receive one event from the child; false on timeout or when it closed. */
static bool kernel_event(struct session *s, struct uhid_event *event)
{
	struct pollfd p = {s->kernel, POLLIN, 0};
	if (poll(&p, 1, 3000) <= 0)
		return false;
	memset(event, 0, sizeof *event);
	return recv(s->kernel, event, sizeof *event, 0) > 0;
}

static void kernel_send(struct session *s, const struct uhid_event *event)
{
	CHECK(send(s->kernel, event, sizeof *event, 0) ==
	      (ssize_t)sizeof *event);
}

static bool channel_frame(struct session *s, unsigned char *out, size_t size)
{
	size_t got = 0;
	while (got < size) {
		struct pollfd p = {s->channel, POLLIN, 0};
		if (poll(&p, 1, 3000) <= 0)
			return false;
		ssize_t n = read(s->channel, out + got, size - got);
		if (n <= 0)
			return false;
		got += (size_t)n;
	}
	return true;
}

static const unsigned char identity[] = {0x10, 0x50, 0x04, 0x07};

static void attach_device(struct session *s)
{
	/* The identity frame may arrive in pieces. */
	const unsigned char head[] = {'I', 0};
	const unsigned char tail[] = {4, 0x10, 0x50, 0x04, 0x07};
	CHECK(write(s->channel, head, sizeof head) == (ssize_t)sizeof head);
	poll(NULL, 0, 20);
	CHECK(write(s->channel, tail, sizeof tail) == (ssize_t)sizeof tail);
	struct uhid_event event;
	CHECK(kernel_event(s, &event));
	CHECK(event.type == UHID_CREATE2);
}

static void test_relay(void)
{
	struct session s = start_import();
	unsigned char ident[] = {'I', 0, 4, 0x10, 0x50, 0x04, 0x07};
	CHECK(write(s.channel, ident, 2) == 2);
	poll(NULL, 0, 20);
	CHECK(write(s.channel, ident + 2, sizeof ident - 2) ==
	      (ssize_t)(sizeof ident - 2));
	struct uhid_event event;
	CHECK(kernel_event(&s, &event));
	CHECK(event.type == UHID_CREATE2);
	CHECK(event.u.create2.rd_size == sizeof fido_descriptor);
	CHECK(memcmp(event.u.create2.rd_data, fido_descriptor,
		     sizeof fido_descriptor) == 0);
	CHECK(event.u.create2.vendor == 0x1050 &&
	      event.u.create2.product == 0x0407);
	CHECK(event.u.create2.bus == BUS_USB);
	CHECK_STR((const char *)event.u.create2.name, "vnc-bridge remote FIDO");
	CHECK_STR((const char *)event.u.create2.phys, "vnc-bridge/fido");

	/* Reports from the viewer become input reports. */
	unsigned char packet[64];
	for (int i = 0; i < 64; i++)
		packet[i] = (unsigned char)i;
	send_frame(&s, 'D', packet, sizeof packet);
	CHECK(kernel_event(&s, &event));
	CHECK(event.type == UHID_INPUT2 && event.u.input2.size == 64);
	CHECK(memcmp(event.u.input2.data, packet, 64) == 0);

	/* Output reports, with or without a leading zero report id, go back. */
	for (int with_id = 0; with_id < 2; with_id++) {
		struct uhid_event out = {.type = UHID_OUTPUT};
		if (with_id) {
			out.u.output.data[0] = 0;
			memcpy(out.u.output.data + 1, packet, 64);
			out.u.output.size = 65;
		} else {
			memcpy(out.u.output.data, packet, 64);
			out.u.output.size = 64;
		}
		out.u.output.rtype = UHID_OUTPUT_REPORT;
		kernel_send(&s, &out);
		unsigned char frame_in[67];
		CHECK(channel_frame(&s, frame_in, sizeof frame_in));
		CHECK(frame_in[0] == 'D' && frame_in[1] == 0 &&
		      frame_in[2] == 64);
		CHECK(memcmp(frame_in + 3, packet, 64) == 0);
	}

	/* When the viewer goes away the device is destroyed. */
	close(s.channel);
	CHECK(kernel_event(&s, &event));
	CHECK(event.type == UHID_DESTROY);
	s.channel = open("/dev/null", O_RDONLY);
	CHECK(finish(&s) == 1);
}

static void test_invalid_frames(void)
{
	static const struct {
		const char *data;
		size_t size;
	} bad[] = {
	    {"D\0\x40", 3},  /* a report before the identity */
	    {"I\0\x41", 3},  /* larger than any frame */
	    {"X\0\0", 3},    /* unknown kind */
	    {"I\0\3abc", 6}, /* identity of the wrong size */
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
		struct session s = start_import();
		unsigned char data[80] = {0};
		memcpy(data, bad[i].data, bad[i].size);
		size_t size = bad[i].size + (bad[i].data[0] == 'D' ? 64 : 0);
		CHECK(write(s.channel, data, size) == (ssize_t)size);
		CHECK(finish(&s) == 1);
	}

	/* A second identity detaches the device. */
	struct session s = start_import();
	attach_device(&s);
	send_frame(&s, 'I', identity, sizeof identity);
	struct uhid_event event;
	CHECK(kernel_event(&s, &event));
	CHECK(event.type == UHID_DESTROY);
	CHECK(finish(&s) == 1);

	/* The viewer reports an error. */
	for (unsigned char code = 0; code < 2; code++) {
		s = start_import();
		unsigned char value = code ? 125 : 1;
		send_frame(&s, 'E', &value, 1);
		CHECK(finish(&s) == 1);
	}
}

static void test_control_requests(void)
{
	struct session s = start_import();
	attach_device(&s);
	struct uhid_event event, reply;

	/* Feature reports are answered with an error and never forwarded. */
	memset(&event, 0, sizeof event);
	event.type = UHID_GET_REPORT;
	event.u.get_report.id = 42;
	event.u.get_report.rnum = 0;
	event.u.get_report.rtype = UHID_FEATURE_REPORT;
	kernel_send(&s, &event);
	CHECK(kernel_event(&s, &reply));
	CHECK(reply.type == UHID_GET_REPORT_REPLY);
	CHECK(reply.u.get_report_reply.id == 42 &&
	      reply.u.get_report_reply.err != 0);

	memset(&event, 0, sizeof event);
	event.type = UHID_SET_REPORT;
	event.u.set_report.id = 43;
	event.u.set_report.rtype = UHID_FEATURE_REPORT;
	kernel_send(&s, &event);
	CHECK(kernel_event(&s, &reply));
	CHECK(reply.type == UHID_SET_REPORT_REPLY);
	CHECK(reply.u.set_report_reply.id == 43 &&
	      reply.u.set_report_reply.err != 0);

	/* An output report sent as SET_REPORT takes the same path as OUTPUT. */
	unsigned char packet[64];
	for (int i = 0; i < 64; i++)
		packet[i] = (unsigned char)(100 + i);
	memset(&event, 0, sizeof event);
	event.type = UHID_SET_REPORT;
	event.u.set_report.id = 7;
	event.u.set_report.rtype = UHID_OUTPUT_REPORT;
	event.u.set_report.rnum = 0;
	event.u.set_report.size = 64;
	memcpy(event.u.set_report.data, packet, 64);
	kernel_send(&s, &event);
	CHECK(kernel_event(&s, &reply));
	CHECK(reply.type == UHID_SET_REPORT_REPLY);
	CHECK(reply.u.set_report_reply.id == 7 &&
	      reply.u.set_report_reply.err == 0);
	unsigned char frame_in[67];
	CHECK(channel_frame(&s, frame_in, sizeof frame_in));
	CHECK(memcmp(frame_in + 3, packet, 64) == 0);

	/* A termination signal ends cleanly without destroying the device
	 * first. */
	kill(s.pid, SIGTERM);
	CHECK(finish(&s) == 0);
}

/* ---- channel selection and connection ---- */

static char root[] = "/tmp/tvx-attach-XXXXXX";
static int listeners[8];
static int open_listeners;

static void listen_at(const char *path, mode_t mode)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof addr.sun_path, "%.100s", path);
	CHECK(bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
	CHECK(listen(fd, 4) == 0);
	CHECK(chmod(path, mode) == 0);
	listeners[open_listeners++] = fd;
}

static void test_choose(void)
{
	char directory[300], path[400], chosen[400];
	snprintf(directory, sizeof directory, "%s/fido", root);
	CHECK(mkdir(directory, 0700) == 0);

	CHECK(choose(chosen, sizeof chosen, root, NULL) == -1);

	snprintf(path, sizeof path, "%s/%d-5.sock", directory, getpid());
	listen_at(path, 0600);
	CHECK(choose(chosen, sizeof chosen, root, NULL) == 0);
	CHECK_STR(chosen, path);

	/* Sockets of processes which no longer exist are ignored. */
	char stale[400];
	snprintf(stale, sizeof stale, "%s/2147483646-1.sock", directory);
	listen_at(stale, 0600);
	CHECK(choose(chosen, sizeof chosen, root, NULL) == 0);
	CHECK_STR(chosen, path);

	char other[400], client[64];
	snprintf(other, sizeof other, "%s/%d-6.sock", directory, getpid());
	listen_at(other, 0600);
	CHECK(choose(chosen, sizeof chosen, root, NULL) == -1);
	snprintf(client, sizeof client, "%d:6", getpid());
	CHECK(choose(chosen, sizeof chosen, root, client) == 0);
	CHECK_STR(chosen, other);
	CHECK(choose(chosen, sizeof chosen, root, "1:2") == -1);
}

static void test_connect(void)
{
	char directory[300], path[400];
	snprintf(directory, sizeof directory, "%s/channels", root);
	CHECK(mkdir(directory, 0700) == 0);
	snprintf(path, sizeof path, "%s/channel.sock", directory);
	listen_at(path, 0600);

	int fd = connect_channel(path);
	CHECK(fd >= 0);
	if (fd >= 0)
		close(fd);

	/* Symbolic link, missing file, not a socket, no directory part. */
	char link[400];
	snprintf(link, sizeof link, "%s/link.sock", directory);
	CHECK(symlink(path, link) == 0);
	CHECK(connect_channel(link) == -1);
	char missing[400];
	snprintf(missing, sizeof missing, "%s/missing.sock", directory);
	CHECK(connect_channel(missing) == -1);
	CHECK(connect_channel("/etc/passwd") == -1);
	CHECK(connect_channel("channel.sock") == -1);

	/* A socket or a directory open to others is refused. */
	CHECK(chmod(path, 0666) == 0);
	CHECK(connect_channel(path) == -1);
	CHECK(chmod(path, 0600) == 0);
	CHECK(chmod(directory, 0755) == 0);
	CHECK(connect_channel(path) == -1);
	CHECK(chmod(directory, 0700) == 0);
	fd = connect_channel(path);
	CHECK(fd >= 0);
	if (fd >= 0)
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
	unsetenv("XDG_RUNTIME_DIR");
	char *no_runtime[] = {"build/bin/vnc-bridge-fido", "attach", NULL};
	CHECK(cli_status(no_runtime) == 2);
	char *with_device[] = {"build/bin/vnc-bridge-fido", "attach",
			       "--device", "x", NULL};
	CHECK(cli_status(with_device) == 2);
	char *exclusive[] = {"build/bin/vnc-bridge-fido",
			     "attach",
			     "--socket",
			     "x",
			     "--client",
			     "1:2",
			     NULL};
	CHECK(cli_status(exclusive) == 2);
	char *no_channel[] = {"build/bin/vnc-bridge-fido", "attach",
			      "--runtime-dir", root, NULL};
	CHECK(cli_status(no_channel) == 1);
	char *bad_socket[] = {"build/bin/vnc-bridge-fido", "attach", "--socket",
			      "/nonexistent/dir/x.sock", NULL};
	CHECK(cli_status(bad_socket) == 1);
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
	test_relay();
	test_invalid_frames();
	test_control_requests();
	test_choose();
	test_connect();
	test_command_line();
	for (int i = 0; i < open_listeners; i++)
		close(listeners[i]);
	CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
	return vncb_finish("test_fido_attach");
}
