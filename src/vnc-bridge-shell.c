/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "exec.h"
#include "files.h"
#include <dirent.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>

typedef struct json_object J;

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

/* ---- channel selection ---- */

static J *member(J *object, const char *key)
{
	J *value = NULL;
	if (object)
		json_object_object_get_ex(object, key, &value);
	return value;
}

static const char *text_of(J *value)
{
	const char *s = value ? json_object_get_string(value) : NULL;
	return s ? s : "";
}

static J *read_json(const char *path)
{
	char *text = read_file(path);
	J *value = text ? json_tokener_parse(text) : NULL;
	free(text);
	return value;
}

/* Does status.json describe a client with this "PID-FD" stem and hostname? */
static bool client_has_hostname(J *records, const char *stem,
				const char *hostname)
{
	if (!records || !json_object_is_type(records, json_type_array))
		return false;
	for (size_t i = 0; i < json_object_array_length(records); i++) {
		J *record = json_object_array_get_idx(records, i);
		char id[128];
		snprintf(id, sizeof id, "%s", text_of(member(record, "id")));
		for (char *p = id; *p; p++)
			if (*p == ':')
				*p = '-';
		if (!strcmp(id, stem) &&
		    !strcmp(hostname, text_of(member(record, "hostname"))))
			return true;
	}
	return false;
}

/* Is `name` a "PID-FD.sock" file of a running process? Fills stem. */
static bool live_channel_name(const char *name, char *stem, size_t size)
{
	size_t len = strlen(name);
	if (len < 6 || strcmp(name + len - 5, ".sock") != 0 || len - 5 >= size)
		return false;
	memcpy(stem, name, len - 5);
	stem[len - 5] = 0;
	char *end;
	errno = 0;
	long pid = strtol(stem, &end, 10);
	return !errno && end != stem && *end == '-' && pid > 0 &&
	       pid <= INT_MAX && !kill((pid_t)pid, 0);
}

/* Pick the single <directory>/shell/PID-FD.sock of a running process owned by
 * the current user, optionally restricted to a client "PID:FD" or to the
 * hostname recorded in status.json. */
static int choose(char *out, size_t size, const char *directory,
		  const char *hostname, const char *client)
{
	char path[PATH_MAX], wanted[128] = "";
	if (client) {
		if (snprintf(wanted, sizeof wanted, "%s", client) >=
		    (int)sizeof wanted)
			return -1;
		for (char *p = wanted; *p; p++)
			if (*p == ':')
				*p = '-';
	}
	if (snprintf(path, sizeof path, "%s/status.json", directory) >=
	    (int)sizeof path)
		return -1;
	J *root = read_json(path);
	J *records = member(root, "clients");
	if (snprintf(path, sizeof path, "%s/shell", directory) >=
	    (int)sizeof path) {
		if (root)
			json_object_put(root);
		return -1;
	}
	struct dirent **list = NULL;
	int count = scandir(path, &list, NULL, alphasort), found = 0;
	for (int i = 0; i < count; i++) {
		char stem[256], full[PATH_MAX];
		struct stat st;
		if (!live_channel_name(list[i]->d_name, stem, sizeof stem) ||
		    (client && strcmp(stem, wanted) != 0) ||
		    snprintf(full, sizeof full, "%s/%s", path,
			     list[i]->d_name) >= (int)sizeof full ||
		    lstat(full, &st) || !S_ISSOCK(st.st_mode) ||
		    st.st_uid != getuid() ||
		    (hostname &&
		     !client_has_hostname(records, stem, hostname)) ||
		    snprintf(out, size, "%s", full) >= (int)size)
			continue;
		found++;
	}
	for (int i = 0; i < count; i++)
		free(list[i]);
	if (count >= 0)
		free(list);
	if (root)
		json_object_put(root);
	if (found != 1) {
		fputs(
		    found
			? "Plusieurs viewers disponibles ; préciser --client "
			  "PID:FD\n"
			: "Aucun canal RFB du GPD disponible. Activer "
			  "VNC_BRIDGE_SHELL=1 dans le viewer et reconnecter.\n",
		    stderr);
		return -1;
	}
	return 0;
}

static const char *env_or(const char *name, const char *fallback)
{
	const char *value = getenv(name);
	return value ? value : fallback;
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

static int report_failure(void)
{
	fputs("\nShell du GPD : connexion fermée ou canal terminal invalide\n",
	      stderr);
	return 1;
}

static int run_channel(const char *path)
{
	int fd = connect_channel(path);
	int rc = fd < 0 ? -1 : relay(fd);
	if (fd >= 0)
		close(fd);
	return rc < 0 ? report_failure() : rc;
}

/* Runtime directory: option, configuration, directory of VNC_BRIDGE_SOCKET,
 * then $XDG_RUNTIME_DIR/vnc-bridge. */
static bool default_directory(char *out, size_t size)
{
	const char *configured = getenv("VNC_BRIDGE_SOCKET");
	if (configured && configured[0] == '/') {
		snprintf(out, size, "%s", configured);
		char *slash = strrchr(out, '/');
		if (slash == out)
			slash[1] = 0;
		else
			*slash = 0;
		return true;
	}
	char fallback[64];
	snprintf(fallback, sizeof fallback, "/run/user/%lu",
		 (unsigned long)getuid());
	return snprintf(out, size, "%s/vnc-bridge",
			env_or("XDG_RUNTIME_DIR", fallback)) < (int)size;
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	bool connecting = false;
	const char *directory = NULL, *hostname = NULL, *client = NULL,
		   *socket_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version")) {
			puts(BASE_VERSION);
			return 0;
		}
		if (!strcmp(argv[i], "--help")) {
			puts("Usage: vnc-bridge-shell --connect [--runtime-dir "
			     "DIR] "
			     "[--hostname HOST] [--client PID:FD] [--socket "
			     "PATH]");
			return 0;
		}
		if (!strcmp(argv[i], "--connect"))
			connecting = true;
		else if (!strcmp(argv[i], "--runtime-dir") && i + 1 < argc)
			directory = argv[++i];
		else if (!strcmp(argv[i], "--hostname") && i + 1 < argc)
			hostname = argv[++i];
		else if (!strcmp(argv[i], "--client") && i + 1 < argc)
			client = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			socket_path = argv[++i];
		else {
			fprintf(stderr, "Invalid argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (!connecting)
		return 2;

	char config_path[PATH_MAX], default_dir[PATH_MAX], chosen[PATH_MAX];
	J *config = NULL;
	if (snprintf(config_path, sizeof config_path,
		     "%s/.config/vnc-bridge/shell.json",
		     env_or("HOME", "")) < (int)sizeof config_path)
		config = read_json(config_path);
	if (!directory && *text_of(member(config, "runtime_dir")))
		directory = text_of(member(config, "runtime_dir"));
	if (!hostname && !client && *text_of(member(config, "hostname")))
		hostname = text_of(member(config, "hostname"));
	if (!directory) {
		if (!default_directory(default_dir, sizeof default_dir)) {
			if (config)
				json_object_put(config);
			return 1;
		}
		directory = default_dir;
	}
	int rc;
	if (socket_path)
		rc = run_channel(socket_path);
	else if (!choose(chosen, sizeof chosen, directory, hostname, client))
		rc = run_channel(chosen);
	else
		rc = report_failure();
	if (config)
		json_object_put(config);
	return rc;
}
