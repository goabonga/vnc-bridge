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
#include <limits.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

/* ---- attach: viewer channel -> virtual UHID device ---- */

static int uhid_write(int fd, const struct uhid_event *event)
{
	for (int attempt = 0; attempt < 3 && !stopping; attempt++) {
		ssize_t n = write(fd, event, sizeof *event);
		if (n == (ssize_t)sizeof *event)
			return 0;
		if (n < 0 && errno == EINTR) {
			attempt--;
			continue;
		}
		if (n < 0 && errno == EAGAIN) {
			struct pollfd p = {fd, POLLOUT, 0};
			if (poll(&p, 1, 100) >= 0)
				continue;
		}
		return -1;
	}
	return -1;
}

/* Create the canonical FIDO device from an identity frame payload. */
static int uhid_create(int uhid, const unsigned char *identity)
{
	struct uhid_event event = {.type = UHID_CREATE2};
	struct uhid_create2_req *c = &event.u.create2;
	snprintf((char *)c->name, sizeof c->name, "vnc-bridge remote FIDO");
	snprintf((char *)c->phys, sizeof c->phys, "vnc-bridge/fido");
	c->bus = BUS_USB;
	c->vendor = ((unsigned)identity[0] << 8) | identity[1];
	c->product = ((unsigned)identity[2] << 8) | identity[3];
	c->version = 1;
	c->rd_size = sizeof fido_descriptor;
	memcpy(c->rd_data, fido_descriptor, sizeof fido_descriptor);
	return uhid_write(uhid, &event);
}

/* Apply the frames queued in `incoming`. Returns false on a protocol error or
 * when the peer reported a failure. */
static bool apply_frames(struct buffer *incoming, int uhid, bool *created)
{
	while (incoming->len >= 3) {
		unsigned kind = incoming->data[0];
		size_t size =
		    ((size_t)incoming->data[1] << 8) | incoming->data[2];
		bool valid = size <= FIDO_PACKET &&
			     (kind == 'I'   ? size == 4 && !*created
			      : kind == 'D' ? size == FIDO_PACKET && *created
			      : kind == 'E' ? size == 1
					    : false);
		if (!valid)
			return false;
		if (incoming->len < size + 3)
			return true;
		const unsigned char *payload = incoming->data + 3;
		if (kind == 'I') {
			if (uhid_create(uhid, payload))
				return false;
			*created = true;
			fputs("vnc-bridge: remote FIDO device attached; Ctrl+C "
			      "detaches it.\n",
			      stderr);
		} else if (kind == 'D') {
			struct uhid_event event = {.type = UHID_INPUT2};
			event.u.input2.size = FIDO_PACKET;
			memcpy(event.u.input2.data, payload, FIDO_PACKET);
			if (uhid_write(uhid, &event))
				return false;
		} else {
			fputs(payload[0] == 125
				  ? "FIDO device is already attached\n"
				  : "FIDO forwarding failed\n",
			      stderr);
			return false;
		}
		consume(incoming, size + 3);
	}
	return true;
}

/* Strip the optional zero report id of a 64-byte output report. */
static bool output_packet(const unsigned char **p, size_t *size)
{
	if (*size == FIDO_PACKET + 1 && !**p) {
		(*size)--;
		(*p)++;
	}
	return *size == FIDO_PACKET;
}

/* Handle one event written by the kernel to the virtual device. Returns
 * false when the device must be torn down. */
static bool handle_uhid_event(int uhid, struct buffer *output)
{
	struct uhid_event event = {0}, reply = {0};
	ssize_t n = read(uhid, &event, sizeof event);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return true;
	if (n < (ssize_t)sizeof event.type)
		return false;
	if (event.type == UHID_OUTPUT) {
		size_t size = event.u.output.size;
		const unsigned char *p = event.u.output.data;
		return event.u.output.rtype == UHID_OUTPUT_REPORT &&
		       output_packet(&p, &size) && !frame(output, 'D', p, size);
	}
	if (event.type == UHID_GET_REPORT) {
		/* Feature and input requests are not forwarded. */
		reply.type = UHID_GET_REPORT_REPLY;
		reply.u.get_report_reply.id = event.u.get_report.id;
		reply.u.get_report_reply.err = EOPNOTSUPP;
		return !uhid_write(uhid, &reply);
	}
	if (event.type == UHID_SET_REPORT) {
		reply.type = UHID_SET_REPORT_REPLY;
		reply.u.set_report_reply.id = event.u.set_report.id;
		reply.u.set_report_reply.err = EOPNOTSUPP;
		size_t size = event.u.set_report.size;
		const unsigned char *p = event.u.set_report.data;
		if (event.u.set_report.rtype == UHID_OUTPUT_REPORT &&
		    !event.u.set_report.rnum && output_packet(&p, &size) &&
		    !frame(output, 'D', p, size))
			reply.u.set_report_reply.err = 0;
		return !uhid_write(uhid, &reply);
	}
	return event.type != UHID_STOP;
}

/* Relay frames from the channel into a UHID device and the device's output
 * reports back. Returns 0 only when stopped by a signal. */
static int import_reports(int channel, int uhid)
{
	struct buffer incoming = {.len = 0}, output = {.len = 0};
	bool created = false;
	if (nonblock(channel) || nonblock(uhid))
		return 1;
	while (!stopping) {
		struct pollfd f[] = {
		    {channel, POLLIN | (output.len ? POLLOUT : 0), 0},
		    {uhid,
		     created && output.len < FIDO_BUFFER - 4096 ? POLLIN : 0,
		     0}};
		if (poll(f, 2, 100) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if ((f[0].revents & POLLOUT) && flush(channel, &output))
			break;
		if ((f[0].revents | f[1].revents) & (POLLERR | POLLNVAL))
			break;
		if (f[0].revents & (POLLIN | POLLHUP)) {
			unsigned char data[4096];
			ssize_t n = read(channel, data, sizeof data);
			if (n <= 0) {
				if (n < 0 &&
				    (errno == EAGAIN || errno == EINTR))
					continue;
				break;
			}
			if (append(&incoming, data, (size_t)n) ||
			    !apply_frames(&incoming, uhid, &created))
				break;
		}
		if ((f[1].revents & (POLLIN | POLLHUP)) &&
		    !handle_uhid_event(uhid, &output))
			break;
	}
	if (created && !stopping) {
		/* Closing the descriptor also destroys the device, even on
		 * SIGKILL. */
		struct uhid_event event = {.type = UHID_DESTROY};
		(void)uhid_write(uhid, &event);
	}
	return stopping ? 0 : 1;
}

/* Connect to a channel socket which belongs to the current user, lives in a
 * directory closed to others and is served by the current user. */
static int connect_channel(const char *path)
{
	struct stat st;
	char parent[PATH_MAX];
	if (snprintf(parent, sizeof parent, "%s", path) >= (int)sizeof parent) {
		errno = ENAMETOOLONG;
		return -1;
	}
	char *slash = strrchr(parent, '/');
	if (!slash || slash == parent) {
		errno = EPERM;
		return -1;
	}
	*slash = 0;
	if (lstat(parent, &st) || !S_ISDIR(st.st_mode) ||
	    st.st_uid != getuid() || (st.st_mode & 077u) || lstat(path, &st) ||
	    !S_ISSOCK(st.st_mode) || st.st_uid != getuid() ||
	    (st.st_mode & 077u)) {
		errno = EPERM;
		return -1;
	}
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	if (snprintf(address.sun_path, sizeof address.sun_path, "%s", path) >=
	    (int)sizeof address.sun_path) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&address, sizeof address)) {
		int e = 0;
		socklen_t size = sizeof e;
		struct pollfd f = {fd, POLLOUT, 0};
		if (errno != EINPROGRESS || poll(&f, 1, 3000) <= 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &size) || e) {
			errno = e ? e : ETIMEDOUT;
			close(fd);
			return -1;
		}
	}
	struct ucred cred;
	socklen_t size = sizeof cred;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) ||
	    cred.uid != getuid()) {
		errno = EPERM;
		close(fd);
		return -1;
	}
	return fd;
}

/* Pick the single <runtime>/fido/PID-FD.sock of a running process (matching
 * `client` "PID:FD" when given). */
static int choose(char *path, size_t size, const char *runtime,
		  const char *client)
{
	char pattern[PATH_MAX];
	if (snprintf(pattern, sizeof pattern, "%s/fido/*.sock", runtime) >=
	    (int)sizeof pattern)
		return -1;
	glob_t paths = {0};
	int n = glob(pattern, 0, NULL, &paths), found = 0;
	if (n && n != GLOB_NOMATCH)
		return -1;
	for (size_t i = 0; i < paths.gl_pathc; i++) {
		const char *name = strrchr(paths.gl_pathv[i], '/') + 1;
		char *end;
		errno = 0;
		long pid = strtol(name, &end, 10);
		if (errno || end == name || *end != '-' || pid <= 0 ||
		    pid > INT_MAX || kill((pid_t)pid, 0))
			continue;
		const char *digits = end + 1;
		unsigned long fd = strtoul(digits, &end, 10);
		if (!isdigit((unsigned char)*digits) ||
		    strcmp(end, ".sock") != 0)
			continue;
		char id[96];
		snprintf(id, sizeof id, "%ld:%lu", pid, fd);
		if ((client && strcmp(client, id) != 0) ||
		    snprintf(path, size, "%s", paths.gl_pathv[i]) >= (int)size)
			continue;
		found++;
	}
	globfree(&paths);
	if (found != 1) {
		fputs(found ? "Several viewers: specify --client PID:FD\n"
			    : "No FIDO channel found\n",
		      stderr);
		return -1;
	}
	return 0;
}

static int attach(bool device_options, const char *path, const char *runtime,
		  const char *client)
{
	if (device_options || (path && (runtime || client)))
		return 2;
	char chosen[PATH_MAX], directory[PATH_MAX];
	if (!path) {
		if (!runtime) {
			const char *xdg = getenv("XDG_RUNTIME_DIR");
			if (!xdg || snprintf(directory, sizeof directory,
					     "%s/vnc-bridge",
					     xdg) >= (int)sizeof directory)
				return 2;
			runtime = directory;
		}
		if (choose(chosen, sizeof chosen, runtime, client))
			return 1;
		path = chosen;
	}
	/* Open UHID before connecting so a permission error never claims a key.
	 */
	int uhid = open("/dev/uhid", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (uhid < 0) {
		perror("/dev/uhid (the current user needs access)");
		return 1;
	}
	int fd = connect_channel(path);
	if (fd < 0) {
		perror("FIDO channel");
		close(uhid);
		return 1;
	}
	int result = import_reports(fd, uhid);
	close(fd);
	close(uhid);
	return result;
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
		     "(binary stdio)\n"
		     "       vnc-bridge-fido attach [--socket PATH | --client "
		     "PID:FD] "
		     "[--runtime-dir DIR]");
		return argc < 2 ? 2 : 0;
	}
	if (!strcmp(argv[1], "list"))
		return argc == 2 ? list() : 2;

	const char *device = NULL, *identity = NULL, *path = NULL,
		   *runtime = NULL, *client = NULL;
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc)
			device = argv[++i];
		else if (!strcmp(argv[i], "--identity") && i + 1 < argc)
			identity = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "--client") && i + 1 < argc)
			client = argv[++i];
		else if (!strcmp(argv[i], "--runtime-dir") && i + 1 < argc)
			runtime = argv[++i];
		else
			return 2;
	}
	if (!strcmp(argv[1], "attach"))
		return attach(device || identity, path, runtime, client);
	if (path || runtime || client)
		return 2;
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
