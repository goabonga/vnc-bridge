/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "exec.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>

#define BUFFER 131072
#define MAX_PAYLOAD 55
#define HIGH_WATER 65536

static volatile sig_atomic_t resized, interrupted;

static void on_resize(int signum)
{
	(void)signum;
	resized = 1;
}

static void on_stop(int signum)
{
	interrupted = signum;
}

/* ---- byte queue and frames: kind, 16-bit big-endian length, payload ---- */

struct buffer {
	unsigned char data[BUFFER];
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

static int frame(struct buffer *b, unsigned char kind, const void *p, size_t n)
{
	if (n > MAX_PAYLOAD)
		return -1;
	unsigned char header[] = {kind, (unsigned char)(n >> 8),
				  (unsigned char)n};
	return append(b, header, sizeof header) ? -1 : append(b, p, n);
}

/* Window size frame (kind 'O' at start, 'W' on resize): rows and columns as
 * 16-bit big-endian numbers, 24x80 when standard input is not a terminal. */
static int size_frame(struct buffer *b, unsigned char kind)
{
	struct winsize ws = {0};
	if (ioctl(0, TIOCGWINSZ, &ws) || !ws.ws_row || !ws.ws_col) {
		ws.ws_row = 24;
		ws.ws_col = 80;
	}
	unsigned char size[] = {
	    (unsigned char)(ws.ws_row >> 8), (unsigned char)(ws.ws_row & 255),
	    (unsigned char)(ws.ws_col >> 8), (unsigned char)(ws.ws_col & 255)};
	return frame(b, kind, size, sizeof size);
}

static int flush_fd(int fd, struct buffer *b)
{
	ssize_t n = write(fd, b->data, b->len);
	if (n > 0) {
		consume(b, (size_t)n);
		return 0;
	}
	return n < 0 && (errno == EAGAIN || errno == EINTR) ? 0 : -1;
}

/* ---- channel ---- */

static int connect_channel(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path) >=
	    (int)sizeof addr.sun_path) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
		int err = 0;
		socklen_t len = sizeof err;
		struct pollfd p = {fd, POLLOUT, 0};
		if (errno != EINPROGRESS || poll(&p, 1, 3000) <= 0 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) {
			if (errno == EINPROGRESS || !err)
				errno = ETIMEDOUT;
			else
				errno = err;
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

/* Handle the frames received from the viewer. Returns false on a protocol
 * error; *exit_code is set by the 'E' frame. */
static bool receive_frames(struct buffer *incoming, struct buffer *output,
			   int *exit_code)
{
	while (incoming->len >= 3) {
		size_t len =
		    ((size_t)incoming->data[1] << 8) | incoming->data[2];
		if (len > MAX_PAYLOAD)
			return false;
		if (incoming->len < len + 3)
			return true;
		unsigned char kind = incoming->data[0];
		if (kind == 'D') {
			if (append(output, incoming->data + 3, len))
				return false;
		} else if (kind == 'E' && len == 1) {
			*exit_code = incoming->data[3];
			if (*exit_code == 125) {
				fputs("Un terminal est déjà ouvert pour ce "
				      "viewer\n",
				      stderr);
				return false;
			}
		} else {
			return false;
		}
		consume(incoming, len + 3);
	}
	return true;
}

/* Relay the terminal over the channel until the viewer reports the exit
 * status of the remote shell. Returns that status, 128+signal when
 * interrupted, or -1 on failure. */
static int relay(int fd)
{
	struct buffer pending = {.len = 0}, incoming = {.len = 0},
		      output = {.len = 0};
	if (size_frame(&pending, 'O'))
		return -1;
	struct termios saved;
	bool tty = isatty(0);
	int flags = fcntl(1, F_GETFL);
	if (flags < 0)
		return -1;
	if (tty) {
		struct termios raw;
		if (tcgetattr(0, &saved))
			return -1;
		raw = saved;
		cfmakeraw(&raw);
		if (tcsetattr(0, TCSANOW, &raw))
			return -1;
	}
	struct sigaction on_winch = {.sa_handler = on_resize};
	struct sigaction on_term = {.sa_handler = on_stop};
	struct sigaction old_winch, old_term, old_int, old_hup;
	sigaction(SIGWINCH, &on_winch, &old_winch);
	sigaction(SIGTERM, &on_term, &old_term);
	sigaction(SIGINT, &on_term, &old_int);
	sigaction(SIGHUP, &on_term, &old_hup);

	int result = -1, exit_code = -1;
	if (!fcntl(1, F_SETFL, flags | O_NONBLOCK)) {
		bool eof = false;
		while (!interrupted && (exit_code < 0 || output.len)) {
			if (resized) {
				resized = 0;
				if (size_frame(&pending, 'W'))
					break;
			}
			bool open = exit_code < 0;
			struct pollfd f[3] = {
			    {open ? fd : -1,
			     (short)((open && output.len < HIGH_WATER ? POLLIN
								      : 0) |
				     (open && pending.len ? POLLOUT : 0)),
			     0},
			    {!eof && open ? 0 : -1,
			     (short)(!eof && open && pending.len < HIGH_WATER
					 ? POLLIN
					 : 0),
			     0},
			    {output.len ? 1 : -1,
			     (short)(output.len ? POLLOUT : 0), 0}};
			if (poll(f, 3, 100) < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if ((f[0].revents & POLLOUT) && flush_fd(fd, &pending))
				break;
			if ((f[2].revents & POLLOUT) && flush_fd(1, &output))
				break;
			if (f[2].revents & (POLLERR | POLLNVAL))
				break;
			if (f[1].revents & (POLLIN | POLLHUP)) {
				unsigned char b[MAX_PAYLOAD];
				ssize_t r = read(0, b, sizeof b);
				if (r > 0) {
					if (frame(&pending, 'D', b, (size_t)r))
						break;
				} else if (!r) {
					eof = true;
					unsigned char eot = 4;
					if (frame(&pending, 'D', &eot, 1))
						break;
				} else if (errno != EINTR && errno != EAGAIN) {
					break;
				}
			}
			if (f[0].revents & (POLLIN | POLLHUP | POLLERR)) {
				unsigned char b[4096];
				ssize_t r = read(fd, b, sizeof b);
				if (r <= 0) {
					if (r < 0 &&
					    (errno == EINTR || errno == EAGAIN))
						continue;
					break;
				}
				if (append(&incoming, b, (size_t)r) ||
				    !receive_frames(&incoming, &output,
						    &exit_code)) {
					exit_code = -1;
					break;
				}
			}
		}
		if (interrupted)
			result = 128 + interrupted;
		else if (exit_code >= 0 && !output.len)
			result = exit_code;
	}
	if (tty)
		tcsetattr(0, TCSADRAIN, &saved);
	fcntl(1, F_SETFL, flags);
	sigaction(SIGWINCH, &old_winch, NULL);
	sigaction(SIGTERM, &old_term, NULL);
	sigaction(SIGINT, &old_int, NULL);
	sigaction(SIGHUP, &old_hup, NULL);
	return result;
}

static int run_channel(const char *path)
{
	int fd = connect_channel(path);
	int rc = fd < 0 ? -1 : relay(fd);
	if (fd >= 0)
		close(fd);
	if (rc < 0) {
		fputs("\nShell du GPD : connexion fermée ou canal terminal "
		      "invalide\n",
		      stderr);
		return 1;
	}
	return rc;
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	bool connecting = false;
	const char *socket_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version")) {
			puts(BASE_VERSION);
			return 0;
		}
		if (!strcmp(argv[i], "--help")) {
			puts("Usage: vnc-bridge-shell --connect --socket PATH");
			return 0;
		}
		if (!strcmp(argv[i], "--connect"))
			connecting = true;
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			socket_path = argv[++i];
		else {
			fprintf(stderr, "Invalid argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (!connecting || !socket_path)
		return 2;
	return run_channel(socket_path);
}
