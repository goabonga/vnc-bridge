/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "exec.h"
#include "fido.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void stop(int signum)
{
	stopping = signum;
}

/* ---- byte queue ---- */

struct buffer {
	unsigned char data[FIDO_BUFFER];
	size_t len;
};

static int append(struct buffer *b, const void *p, size_t n)
{
	if (n > sizeof b->data - b->len) {
		errno = ENOBUFS;
		return -1;
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
	return 0;
}

static void consume(struct buffer *b, size_t n)
{
	b->len -= n;
	memmove(b->data, b->data + n, b->len);
}

/* Append a frame: kind, 16-bit big-endian length, payload (at most 64). */
static int frame(struct buffer *b, unsigned char kind, const void *p, size_t n)
{
	unsigned char header[] = {kind, (unsigned char)(n >> 8),
				  (unsigned char)n};
	if (n > FIDO_PACKET || sizeof header + n > sizeof b->data - b->len) {
		errno = ENOBUFS;
		return -1;
	}
	return append(b, header, sizeof header) || append(b, p, n) ? -1 : 0;
}

static int flush(int fd, struct buffer *b)
{
	if (!b->len)
		return 0;
	ssize_t n = write(fd, b->data, b->len);
	if (n > 0)
		consume(b, (size_t)n);
	else if (!n || (errno != EINTR && errno != EAGAIN))
		return -1;
	return 0;
}

static int nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---- hidraw devices ---- */

/* Open a hidraw node and accept it only when it is a USB interface whose
 * report descriptor is the unnumbered FIDO descriptor. A writable open also
 * takes an exclusive lock so a single exporter owns the key. */
static int read_device(const char *path, struct hidraw_devinfo *info,
		       bool writeable)
{
	int fd = open(path,
		      (writeable ? O_RDWR : O_RDONLY) | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int size = 0;
	struct hidraw_report_descriptor descriptor = {0};
	if (ioctl(fd, HIDIOCGRAWINFO, info) || info->bustype != BUS_USB ||
	    ioctl(fd, HIDIOCGRDESCSIZE, &size) || size <= 0 ||
	    (size_t)size > sizeof descriptor.value) {
		close(fd);
		errno = ENODEV;
		return -1;
	}
	descriptor.size = (unsigned)size;
	if (ioctl(fd, HIDIOCGRDESC, &descriptor) ||
	    !fido_descriptor_valid(descriptor.value, descriptor.size)) {
		close(fd);
		errno = ENODEV;
		return -1;
	}
	if (writeable && flock(fd, LOCK_EX | LOCK_NB)) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Does fd refer to the file identified by "DEV:INO:RDEV" (decimal)? */
static bool identity_matches(int fd, const char *identity)
{
	struct stat st;
	if (fstat(fd, &st))
		return false;
	const uintmax_t expected[] = {
	    (uintmax_t)st.st_dev, (uintmax_t)st.st_ino, (uintmax_t)st.st_rdev};
	const char *p = identity;
	for (unsigned i = 0; i < 3; i++) {
		if (!isdigit((unsigned char)*p))
			return false;
		char *end;
		errno = 0;
		uintmax_t value = strtoumax(p, &end, 10);
		if (errno || value != expected[i] ||
		    (i < 2 ? *end != ':' : *end != 0))
			return false;
		p = i < 2 ? end + 1 : end;
	}
	return true;
}

static int list(void)
{
	glob_t paths = {0};
	int r = glob("/dev/hidraw*", 0, NULL, &paths);
	if (r && r != GLOB_NOMATCH)
		return 1;
	for (size_t i = 0; i < paths.gl_pathc; i++) {
		struct hidraw_devinfo info;
		int fd = read_device(paths.gl_pathv[i], &info, false);
		if (fd < 0)
			continue;
		char name[128] = {0};
		if (ioctl(fd, HIDIOCGRAWNAME(sizeof name - 1), name) < 0)
			snprintf(name, sizeof name, "FIDO authenticator");
		for (char *p = name; *p; p++)
			if ((unsigned char)*p < 32 || *p == 127)
				*p = '_';
		printf("%s %04x:%04x %s\n", paths.gl_pathv[i],
		       (unsigned)(unsigned short)info.vendor,
		       (unsigned)(unsigned short)info.product, name);
		close(fd);
	}
	globfree(&paths);
	return 0;
}

/* ---- export: device <-> standard input/output ----
 *
 * The stream protocol does not depend on HID or UHID ABIs:
 *   I  length(2, BE)  vendor(2) product(2)   sent once, first
 *   D  length(2, BE)  64 opaque report bytes in both directions
 * No report descriptor or secret is logged. */
static int export_reports(int device, const struct hidraw_devinfo *info)
{
	struct buffer incoming = {.len = 0}, output = {.len = 0},
		      hid = {.len = 0};
	unsigned char identity[] = {(unsigned char)(info->vendor >> 8),
				    (unsigned char)(info->vendor & 255),
				    (unsigned char)(info->product >> 8),
				    (unsigned char)(info->product & 255)};
	if (nonblock(0) || nonblock(1) || nonblock(device) ||
	    frame(&output, 'I', identity, sizeof identity))
		return 1;
	while (!stopping) {
		if (hid.len) {
			unsigned char packet[FIDO_PACKET + 1] = {0};
			memcpy(packet + 1, hid.data,
			       FIDO_PACKET); /* zero report id first */
			ssize_t n = write(device, packet, sizeof packet);
			if (n == (ssize_t)sizeof packet)
				consume(&hid, FIDO_PACKET);
			else if (n >= 0 || (errno != EAGAIN && errno != EINTR))
				return 1;
		}
		struct pollfd f[] = {
		    {0,
		     incoming.len < FIDO_BUFFER - 4096 &&
			     hid.len < FIDO_BUFFER - 4096
			 ? POLLIN
			 : 0,
		     0},
		    {1, output.len ? POLLOUT : 0, 0},
		    {device, output.len < FIDO_BUFFER - 4096 ? POLLIN : 0, 0}};
		if (poll(f, 3, hid.len ? 10 : 100) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if ((f[1].revents & POLLOUT) && flush(1, &output))
			return 1;
		if ((f[0].revents | f[1].revents | f[2].revents) &
		    (POLLERR | POLLNVAL))
			return 1;
		if (f[0].revents & (POLLIN | POLLHUP)) {
			unsigned char data[4096];
			ssize_t n = read(0, data, sizeof data);
			if (!n)
				return incoming.len ? 1 : 0;
			if (n < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				return 1;
			}
			if (append(&incoming, data, (size_t)n))
				return 1;
			while (incoming.len >= 3) {
				size_t size = ((size_t)incoming.data[1] << 8) |
					      incoming.data[2];
				if (incoming.data[0] != 'D' ||
				    size != FIDO_PACKET)
					return 1;
				if (incoming.len < size + 3)
					break;
				if (append(&hid, incoming.data + 3, size))
					return 1;
				consume(&incoming, size + 3);
			}
		}
		if (f[2].revents & (POLLIN | POLLHUP)) {
			unsigned char packet[FIDO_PACKET + 1];
			ssize_t n = read(device, packet, sizeof packet);
			if (n == FIDO_PACKET) {
				if (frame(&output, 'D', packet, FIDO_PACKET))
					return 1;
			} else if (n >= 0 ||
				   (errno != EAGAIN && errno != EINTR)) {
				return 1;
			}
		}
		if (f[1].revents & POLLHUP)
			return 1;
	}
	return 0;
}

/* Send a CTAPHID_INIT request on the broadcast channel and check the answer.
 * INIT only negotiates a channel: it never registers or signs anything. */
static int probe(int fd, int timeout_ms)
{
	unsigned char packet[FIDO_PACKET + 1] = {0}, nonce[8];
	if (getrandom(nonce, sizeof nonce, 0) != (ssize_t)sizeof nonce)
		return 1;
	memset(packet + 1, 255, 4);
	packet[5] = 0x86;
	packet[7] = sizeof nonce;
	memcpy(packet + 8, nonce, sizeof nonce);
	if (write(fd, packet, sizeof packet) != (ssize_t)sizeof packet)
		return 1;
	double deadline = clocksec(CLOCK_MONOTONIC) + timeout_ms / 1000.;
	while (clocksec(CLOCK_MONOTONIC) < deadline) {
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, 100) < 0 && errno != EINTR)
			break;
		unsigned char response[FIDO_PACKET + 1];
		ssize_t n = read(fd, response, sizeof response);
		if (n == FIDO_PACKET && !memcmp(response, packet + 1, 4) &&
		    response[4] == 0x86 && response[5] == 0 &&
		    response[6] == 17 &&
		    !memcmp(response + 7, nonce, sizeof nonce)) {
			printf("CTAPHID_INIT OK; HID framing=%u; "
			       "capabilities=0x%02x\n",
			       response[19], response[23]);
			return 0;
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
			break;
	}
	fputs("FIDO INIT timed out\n", stderr);
	return 1;
}

int main(int argc, char **argv)
{
	struct sigaction action = {.sa_handler = stop};
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (argc > 1 && !strcmp(argv[1], "--version")) {
		puts(BASE_VERSION);
		return 0;
	}
	if (argc < 2 || !strcmp(argv[1], "--help")) {
		puts("Usage: vnc-bridge-fido list\n"
		     "       vnc-bridge-fido probe --device /dev/hidrawN\n"
		     "       vnc-bridge-fido export --device /dev/hidrawN "
		     "(binary stdio)");
		return argc < 2 ? 2 : 0;
	}
	if (!strcmp(argv[1], "list"))
		return argc == 2 ? list() : 2;

	const char *device = NULL, *identity = NULL;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else if (!strcmp(argv[i], "--identity") && i + 1 < argc)
			identity = argv[++i];
		else
			return 2;
	}
	bool exporting = !strcmp(argv[1], "export");
	if ((!exporting && strcmp(argv[1], "probe") != 0) || !device ||
	    (exporting && (isatty(0) || isatty(1))))
		return 2;
	struct hidraw_devinfo info;
	int fd = read_device(device, &info, true);
	if (fd < 0) {
		perror(
		    "FIDO device (requires an unnumbered USB FIDO interface)");
		return 1;
	}
	if (identity && !identity_matches(fd, identity)) {
		fputs("Selected FIDO device changed; reconnect the viewer\n",
		      stderr);
		close(fd);
		return 1;
	}
	int result = exporting ? export_reports(fd, &info) : probe(fd, 3000);
	close(fd);
	return result;
}
