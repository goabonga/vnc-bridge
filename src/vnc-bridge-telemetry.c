/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "exec.h"
#include "files.h"
#include <dirent.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

typedef struct json_object J;

#define EXPIRY_SECONDS 90
#define MAX_CLIENTS 256

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

static void print_battery(int percent, bool charging)
{
	const char *icon = percent < 10	  ? "\uF244"
			   : percent < 35 ? "\uF243"
			   : percent < 65 ? "\uF242"
			   : percent < 90 ? "\uF241"
					  : "\uF240";
	printf(" %3d%% %s%s \n", percent, icon, charging ? " \uF0E7" : "");
}

static void print_audio(int percent, bool muted)
{
	const char *icon = muted || percent <= 25 ? "\uF026"
			   : percent <= 65	  ? "\uF027"
						  : "\uF028";
	char text[128];
	snprintf(text, sizeof text, " %3d%% %s ", percent, icon);
	printf("%s\n%s\n%s\n", text, text, muted ? "#586e75" : "#fc9600");
}

static char *supply_field(const char *root, const char *device, const char *key)
{
	char path[PATH_MAX];
	if (snprintf(path, sizeof path, "%s/%s/%s", root, device, key) >=
	    (int)sizeof path)
		return NULL;
	char *value = read_file(path);
	if (value)
		trim(value);
	return value;
}

static bool supply_field_is(const char *root, const char *device,
			    const char *key, const char *expected)
{
	char *value = supply_field(root, device, key);
	bool same = value && !strcmp(value, expected);
	free(value);
	return same;
}

static bool parse_percent(const char *text, int *value)
{
	if (!text || !*text)
		return false;
	char *end;
	errno = 0;
	long n = strtol(text, &end, 10);
	if (errno || *end || n < 0 || n > 100)
		return false;
	*value = (int)n;
	return true;
}

/* Charging state of the first external supply reporting a known status. */
static bool external_supply_charging(struct dirent **list, int count,
				     const char *root)
{
	for (int i = 0; i < count; i++) {
		const char *device = list[i]->d_name;
		if (!supply_field_is(root, device, "type", "USB") &&
		    !supply_field_is(root, device, "type", "Mains"))
			continue;
		char *state = supply_field(root, device, "status");
		bool known = state && (!strcmp(state, "Charging") ||
				       !strcmp(state, "Discharging") ||
				       !strcmp(state, "Not charging"));
		bool charging = known && !strcmp(state, "Charging");
		free(state);
		if (known)
			return charging;
	}
	return false;
}

/* Print the block of the first present battery with a valid capacity. */
static void local_battery(const char *root)
{
	struct dirent **list = NULL;
	int count = scandir(root, &list, NULL, alphasort);
	bool found = false;
	for (int i = 0; i < count && !found; i++) {
		const char *device = list[i]->d_name;
		if (!supply_field_is(root, device, "type", "Battery") ||
		    supply_field_is(root, device, "present", "0"))
			continue;
		char *capacity = supply_field(root, device, "capacity");
		int percent;
		bool valid = parse_percent(capacity, &percent);
		free(capacity);
		if (!valid)
			continue;
		char *state = supply_field(root, device, "status");
		bool charging = state && !strcmp(state, "Charging");
		if (state && !strcmp(state, "Unknown"))
			charging = external_supply_charging(list, count, root);
		free(state);
		print_battery(percent, charging);
		found = true;
	}
	for (int i = 0; i < count; i++)
		free(list[i]);
	if (count >= 0)
		free(list);
	if (!found)
		puts("");
}

/* Parse a "mixer --get" reply, "<percent|-> <on|off|->". The volume stays -1
 * when the reply is malformed or the control has no volume. */
static void parse_mixer_reply(const char *reply, int *volume, bool *muted)
{
	*volume = -1;
	*muted = false;
	char *end;
	long n = -1;
	if (*reply == '-')
		end = (char *)reply + 1;
	else if (isdigit((unsigned char)*reply))
		n = strtol(reply, &end, 10);
	else
		return;
	if (*end != ' ')
		return;
	const char *state = end + 1;
	size_t len = strcspn(state, " \t\r\n");
	const char *tail = state + len;
	while (isspace((unsigned char)*tail))
		tail++;
	bool off = len == 3 && !strncmp(state, "off", 3);
	bool on = len == 2 && !strncmp(state, "on", 2);
	bool none = len == 1 && *state == '-';
	if (*tail || !(off || on || none))
		return;
	if (n >= 0 && n <= 1000) {
		*volume = (int)n;
		*muted = off;
	}
}

/* Parse amixer "sget" output: the loudest channel, muted when no channel is
 * on. */
static void parse_amixer(char *output, int *volume, bool *muted)
{
	bool any_state = false, any_on = false;
	for (char *line = output; line && *line;) {
		char *end = strchr(line, '\n');
		if (end)
			*end = 0;
		if (strstr(line, "Playback")) {
			for (char *p = line; (p = strchr(p, '['));) {
				int n, used = 0;
				if (sscanf(p, "[%d%%]%n", &n, &used) == 1 &&
				    used > 0) {
					if (n > *volume)
						*volume = n;
					if (strstr(p + used, "[on]"))
						any_state = any_on = true;
					if (strstr(p + used, "[off]"))
						any_state = true;
					break;
				}
				p++;
			}
		}
		line = end ? end + 1 : NULL;
	}
	*muted = any_state && !any_on;
}

/* Parse "wpctl get-volume" output: "Volume: 0.38" with an optional
 * "[MUTED]". */
static void parse_wpctl(const char *output, int *volume, bool *muted)
{
	double level;
	int used = 0;
	if (sscanf(output, "Volume: %lf%n", &level, &used) != 1 ||
	    !isfinite(level) || level < 0 || level > 10)
		return;
	const char *p = output + used;
	while (isspace((unsigned char)*p))
		p++;
	if (!strncmp(p, "[MUTED]", 7)) {
		*muted = true;
		p += 7;
	}
	while (isspace((unsigned char)*p))
		p++;
	if (!*p)
		*volume = (int)nearbyint(level * 100);
}

static const char *env_or(const char *name, const char *fallback)
{
	const char *value = getenv(name);
	return value ? value : fallback;
}

/* Print the audio block of the local machine. cards_path is the ALSA card
 * list used to recognize the GPD Pocket sound card. */
static void local_audio(const char *cards_path)
{
	char *cards = read_file(cards_path);
	bool pocket = cards && strstr(cards, "chtrt5645");
	free(cards);
	const char *backend =
	    env_or("VNC_BRIDGE_AUDIO_BACKEND", pocket ? "alsa" : "pipewire");
	bool alsa = !strcmp(backend, "alsa");
	if (!alsa && strcmp(backend, "pipewire")) {
		puts("");
		return;
	}
	const char *card =
	    env_or("VNC_BRIDGE_ALSA_CARD", pocket ? "chtrt5645" : "default");
	const char *control =
	    env_or("VNC_BRIDGE_ALSA_CONTROL", pocket ? "DAC1" : "Master");
	const char *tool = env_or("VNC_BRIDGE_ALSA_TOOL", "");
	if (!*tool)
		tool =
		    pocket && !strcmp(card, "chtrt5645") ? "mixer" : "amixer";

	int volume = -1;
	bool muted = false, answered = false;
	if (alsa && !strcmp(tool, "mixer")) {
		char target[128];
		snprintf(target, sizeof target, "sysdefault:%s", card);
		char *mixer[] = {"mixer", "--card",	   target,
				 "--get", (char *)control, NULL};
		char *reply = capture(mixer, 1000);
		if (reply) {
			parse_mixer_reply(reply, &volume, &muted);
			free(reply);
			answered = true;
		}
	}
	if (!answered) {
		char *amixer[] = {"amixer",	   "-c", (char *)card, "sget",
				  (char *)control, NULL};
		char *wpctl[] = {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@",
				 NULL};
		char *output = capture(alsa ? amixer : wpctl, 1000);
		if (output && alsa)
			parse_amixer(output, &volume, &muted);
		else if (output)
			parse_wpctl(output, &volume, &muted);
		free(output);
	}
	if (volume >= 0 && volume <= 1000)
		print_audio(volume, muted);
	else
		puts("");
}

typedef struct {
	const char *client;
	const char *hostname;
} filter;

/* The most recently received, unexpired record matching the filters. */
static J *latest_record(J *root, const filter *f, double now)
{
	J *records = member(root, "clients");
	J *best = NULL;
	double latest = -1;
	if (!records || !json_object_is_type(records, json_type_array))
		return NULL;
	for (size_t i = 0; i < json_object_array_length(records); i++) {
		J *r = json_object_array_get_idx(records, i);
		J *stamp = member(r, "received_at");
		if (!stamp)
			continue;
		double received = json_object_get_double(stamp);
		double age = now - received;
		if (age < 0 || age >= EXPIRY_SECONDS ||
		    (f->client &&
		     strcmp(f->client, text_of(member(r, "id")))) ||
		    (f->hostname &&
		     strcmp(f->hostname, text_of(member(r, "hostname")))))
			continue;
		if (received > latest) {
			best = r;
			latest = received;
		}
	}
	return best;
}

static void report(const char *path, bool battery, const filter *f, bool hide,
		   bool icons)
{
	J *root = NULL;
	char *data = read_file(path);
	if (data) {
		root = json_tokener_parse(data);
		free(data);
	}
	J *best = latest_record(root, f, clocksec(CLOCK_REALTIME));
	J *group = member(best, battery ? "battery" : "audio");
	J *value = member(group, battery ? "capacity" : "volume");
	if (!value || !json_object_is_type(value, json_type_int)) {
		puts(hide      ? ""
		     : battery ? (icons ? "n/a " : "BAT n/a")
			       : (icons ? "n/a " : "VOL n/a"));
	} else {
		int percent = json_object_get_int(value);
		bool flag =
		    battery
			? !strcmp(text_of(member(group, "status")), "Charging")
			: json_object_get_boolean(member(group, "muted"));
		if (icons) {
			if (battery)
				print_battery(percent, flag);
			else
				print_audio(percent, flag);
		} else if (battery) {
			printf("BAT %d%%%s (%s)\n", percent, flag ? "+" : "",
			       text_of(member(best, "hostname")));
		} else if (flag) {
			puts("VOL MUET");
		} else {
			printf("VOL %d%%\n", percent);
		}
	}
	if (root)
		json_object_put(root);
}

/* ---- collector ---- */

static const char *const battery_states[] = {
    "Unknown", "Charging", "Discharging", "Full", "Not charging"};

struct client {
	long pid;
	unsigned long connection;
	char hostname[49];
	int capacity; /* -1 when unknown */
	int state;
	bool has_audio;
	int volume; /* -1 when unknown */
	bool muted;
	double received_at; /* wall clock, written to status.json */
	double seen;	    /* monotonic, drives the expiry */
};

struct clients {
	struct client items[MAX_CLIENTS];
	size_t count;
};

static void clients_init(struct clients *table)
{
	table->count = 0;
}

static bool clients_store(struct clients *table, const struct client *c,
			  double now)
{
	for (size_t i = 0; i < table->count; i++) {
		if (table->items[i].pid == c->pid &&
		    table->items[i].connection == c->connection) {
			table->items[i] = *c;
			table->items[i].seen = now;
			return true;
		}
	}
	if (table->count >= MAX_CLIENTS)
		return false;
	table->items[table->count] = *c;
	table->items[table->count++].seen = now;
	return true;
}

/* Drop records not refreshed for EXPIRY_SECONDS; true when any was dropped. */
static bool clients_expire(struct clients *table, double now)
{
	size_t kept = 0;
	for (size_t i = 0; i < table->count; i++) {
		if (now - table->items[i].seen < EXPIRY_SECONDS)
			table->items[kept++] = table->items[i];
	}
	bool changed = kept != table->count;
	table->count = kept;
	return changed;
}

/* Parse an unsigned decimal field of 1 to max_digits digits. */
static bool parse_digits(const char *s, size_t length, size_t max_digits,
			 unsigned long *value)
{
	if (!length || length > max_digits)
		return false;
	unsigned long n = 0;
	for (size_t i = 0; i < length; i++) {
		if (!isdigit((unsigned char)s[i]))
			return false;
		n = n * 10 + (unsigned long)(s[i] - '0');
	}
	*value = n;
	return true;
}

static bool valid_hostname(const char *s, size_t length)
{
	if (length > 48)
		return false;
	for (size_t i = 0; i < length; i++) {
		if (!isalnum((unsigned char)s[i]) && s[i] != '_' &&
		    s[i] != '.' && s[i] != '-')
			return false;
	}
	return true;
}

/* Decode "PID:CONNECTION|CAPACITY|STATE|HOSTNAME[|VOLUME|MUTE]" sent by
 * process `sender` of user `uid`. The message must come from the same user
 * and name its own pid. Capacity 255 and volume 65535 mean unknown. */
static bool decode_message(const char *data, size_t length, uid_t uid,
			   pid_t sender, struct client *out)
{
	if (uid != getuid() || length > 128 || memchr(data, 0, length))
		return false;
	char text[129];
	memcpy(text, data, length);
	text[length] = 0;

	char *fields[6];
	size_t count = 0;
	for (char *p = text;; p++) {
		char *bar = strchr(p, '|');
		if (count >= 6)
			return false;
		fields[count++] = p;
		if (!bar)
			break;
		*bar = 0;
		p = bar;
	}
	if (count != 4 && count != 6)
		return false;

	char *colon = strchr(fields[0], ':');
	if (!colon)
		return false;
	*colon = 0;
	unsigned long pid, connection, capacity, state, volume = 65535,
							mute = 0;
	if (fields[0][0] == '0' ||
	    !parse_digits(fields[0], strlen(fields[0]), 10, &pid) ||
	    pid != (unsigned long)sender || !pid ||
	    !parse_digits(colon + 1, strlen(colon + 1), 18, &connection) ||
	    !parse_digits(fields[1], strlen(fields[1]), 3, &capacity) ||
	    !parse_digits(fields[2], strlen(fields[2]), 1, &state) ||
	    state > 4 || (capacity > 100 && capacity != 255) ||
	    !valid_hostname(fields[3], strlen(fields[3])))
		return false;
	bool has_audio = count == 6;
	if (has_audio &&
	    (!parse_digits(fields[4], strlen(fields[4]), 5, &volume) ||
	     !parse_digits(fields[5], strlen(fields[5]), 1, &mute) ||
	     mute > 1 || (volume > 1000 && volume != 65535) ||
	     (volume == 65535 && mute)))
		return false;

	memset(out, 0, sizeof *out);
	out->pid = (long)pid;
	out->connection = connection;
	snprintf(out->hostname, sizeof out->hostname, "%s", fields[3]);
	out->capacity = capacity == 255 ? -1 : (int)capacity;
	out->state = (int)state;
	out->has_audio = has_audio;
	out->volume = volume == 65535 ? -1 : (int)volume;
	out->muted = mute;
	out->received_at = clocksec(CLOCK_REALTIME);
	return true;
}

static J *client_json(const struct client *c)
{
	J *record = json_object_new_object();
	char id[64];
	snprintf(id, sizeof id, "%ld:%lu", c->pid, c->connection);
	json_object_object_add(record, "id", json_object_new_string(id));
	json_object_object_add(record, "hostname",
			       json_object_new_string(c->hostname));
	J *battery = json_object_new_object();
	json_object_object_add(
	    battery, "capacity",
	    c->capacity < 0 ? NULL : json_object_new_int(c->capacity));
	json_object_object_add(
	    battery, "status",
	    json_object_new_string(battery_states[c->state]));
	json_object_object_add(record, "battery", battery);
	json_object_object_add(record, "received_at",
			       json_object_new_double(c->received_at));
	if (c->has_audio) {
		J *audio = json_object_new_object();
		json_object_object_add(
		    audio, "volume",
		    c->volume < 0 ? NULL : json_object_new_int(c->volume));
		json_object_object_add(audio, "muted",
				       json_object_new_boolean(c->muted));
		json_object_object_add(record, "audio", audio);
	}
	return record;
}

/* Replace status.json atomically with the current records. */
static int save_status(const char *directory, const struct clients *table)
{
	char temp[PATH_MAX], destination[PATH_MAX];
	if (snprintf(temp, sizeof temp, "%s/.status-XXXXXX", directory) >=
		(int)sizeof temp ||
	    snprintf(destination, sizeof destination, "%s/status.json",
		     directory) >= (int)sizeof destination)
		return -1;
	int fd = mkstemp(temp);
	if (fd < 0)
		return -1;
	J *root = json_object_new_object();
	J *array = json_object_new_array();
	for (size_t i = 0; i < table->count; i++)
		json_object_array_add(array, client_json(&table->items[i]));
	json_object_object_add(root, "version", json_object_new_int(1));
	json_object_object_add(root, "clients", array);
	FILE *f = fdopen(fd, "w");
	int result = -1;
	if (f) {
		bool ok = fprintf(f, "%s\n",
				  json_object_to_json_string_ext(
				      root, JSON_C_TO_STRING_PLAIN)) >= 0;
		if (fclose(f))
			ok = false;
		if (ok)
			result = rename(temp, destination);
	} else {
		close(fd);
	}
	unlink(temp);
	json_object_put(root);
	return result;
}

/* Create the directory (and parents) as 0700 and require that it belongs to
 * the current user and is closed to others. */
static int private_directory(const char *path)
{
	char copy[PATH_MAX];
	if (snprintf(copy, sizeof copy, "%s", path) >= (int)sizeof copy)
		return -1;
	for (char *p = copy + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(copy, 0700) && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(copy, 0700) && errno != EEXIST)
		return -1;
	struct stat st;
	if (lstat(copy, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
	    (st.st_mode & 077)) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

static volatile sig_atomic_t terminate;

static void request_stop(int signum)
{
	(void)signum;
	terminate = 1;
}

/* Receive one datagram and store it when it is a valid message from this
 * user. Returns true when the table changed. */
static bool receive_one(int fd, struct clients *table)
{
	char data[128];
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(struct ucred)) +
			   CMSG_SPACE(256 * sizeof(int))];
	} control;
	struct iovec io = {data, sizeof data};
	struct msghdr msg = {.msg_iov = &io,
			     .msg_iovlen = 1,
			     .msg_control = control.bytes,
			     .msg_controllen = sizeof control.bytes};
	ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
	struct ucred cred = {0};
	bool have_credentials = false;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); n >= 0 && c;
	     c = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level != SOL_SOCKET)
			continue;
		if (c->cmsg_type == SCM_CREDENTIALS &&
		    c->cmsg_len >= CMSG_LEN(sizeof cred)) {
			memcpy(&cred, CMSG_DATA(c), sizeof cred);
			have_credentials = true;
		} else if (c->cmsg_type == SCM_RIGHTS) {
			size_t count =
			    (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			int descriptors[256];
			memcpy(descriptors, CMSG_DATA(c), count * sizeof(int));
			for (size_t i = 0; i < count; i++)
				close(descriptors[i]);
		}
	}
	if (n < 0 || !have_credentials ||
	    (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
		return false;
	struct client client;
	if (!decode_message(data, (size_t)n, cred.uid, cred.pid, &client))
		return false;
	return clients_store(table, &client, clocksec(CLOCK_MONOTONIC));
}

/* Run the collector until SIGTERM or SIGINT. */
static int collect(const char *directory)
{
	if (private_directory(directory)) {
		perror("private directory");
		return 1;
	}
	char path[PATH_MAX];
	if (snprintf(path, sizeof path, "%s/collector.lock", directory) >=
	    (int)sizeof path)
		return 1;
	int lock = open(path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		perror("collector lock");
		if (lock >= 0)
			close(lock);
		return 1;
	}
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (snprintf(path, sizeof path, "%s/telemetry.sock", directory) >=
		(int)sizeof path ||
	    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path) >=
		(int)sizeof addr.sun_path) {
		close(lock);
		return 1;
	}

	static struct clients table;
	clients_init(&table);
	struct sigaction action = {.sa_handler = request_stop};
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	unlink(path);
	int one = 1;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int result = 1;
	bool bound = false;
	if (fd < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof one) ||
	    bind(fd, (struct sockaddr *)&addr, sizeof addr))
		goto done;
	bound = true;
	if (chmod(path, 0600) || save_status(directory, &table))
		goto done;
	result = 0;
	while (!terminate) {
		bool changed =
		    clients_expire(&table, clocksec(CLOCK_MONOTONIC));
		struct pollfd pfd = {fd, POLLIN, 0};
		int ready = poll(&pfd, 1, 1000);
		if (ready < 0 && errno != EINTR) {
			result = 1;
			break;
		}
		if (ready > 0 && receive_one(fd, &table))
			changed = true;
		if (changed && save_status(directory, &table)) {
			result = 1;
			break;
		}
	}
done:
	if (bound) {
		unlink(path);
		clients_init(&table);
		if (save_status(directory, &table))
			result = 1;
	}
	if (result)
		perror("telemetry daemon");
	if (fd >= 0)
		close(fd);
	close(lock);
	return result;
}

int main(int argc, char **argv)
{
	const char *command = NULL, *directory = NULL;
	filter f = {NULL, NULL};
	bool hide = false, icons = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version")) {
			puts(BASE_VERSION);
			return 0;
		}
		if (!strcmp(argv[i], "--help")) {
			puts("Usage: vnc-bridge-telemetry "
			     "{daemon,battery,audio,status,local-battery,local-"
			     "audio} "
			     "[--runtime-dir DIR] [--hostname HOST] [--client "
			     "PID:FD] "
			     "[--hide-unavailable] [--i3blocks]");
			return 0;
		}
		if (!strcmp(argv[i], "--hide-unavailable"))
			hide = true;
		else if (!strcmp(argv[i], "--i3blocks"))
			icons = true;
		else if (!strcmp(argv[i], "--runtime-dir") && i + 1 < argc)
			directory = argv[++i];
		else if (!strcmp(argv[i], "--client") && i + 1 < argc)
			f.client = argv[++i];
		else if (!strcmp(argv[i], "--hostname") && i + 1 < argc)
			f.hostname = argv[++i];
		else if (argv[i][0] != '-' && !command)
			command = argv[i];
		else {
			fprintf(stderr, "Invalid argument: %s\n", argv[i]);
			return 2;
		}
	}
	if (!command)
		return 2;
	if (!strcmp(command, "local-battery")) {
		local_battery("/sys/class/power_supply");
		return 0;
	}
	if (!strcmp(command, "local-audio")) {
		local_audio("/proc/asound/cards");
		return 0;
	}
	bool battery = !strcmp(command, "battery");
	bool sound = !strcmp(command, "audio");
	bool status = !strcmp(command, "status");
	bool daemon = !strcmp(command, "daemon");
	if (!battery && !sound && !status && !daemon)
		return 2;

	char dir[PATH_MAX];
	if (!directory) {
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		if (!runtime) {
			if ((battery || sound) && hide) {
				puts("");
				return 0;
			}
			fputs("XDG_RUNTIME_DIR is unset; pass --runtime-dir\n",
			      stderr);
			return 2;
		}
		if (snprintf(dir, sizeof dir, "%s/vnc-bridge", runtime) >=
		    (int)sizeof dir)
			return 1;
		directory = dir;
	}
	if (daemon)
		return collect(directory);
	char path[PATH_MAX];
	if (snprintf(path, sizeof path, "%s/status.json", directory) >=
	    (int)sizeof path)
		return 1;

	if (status) {
		char *data = read_file(path);
		if (!data) {
			perror(path);
			return 1;
		}
		fputs(data, stdout);
		free(data);
		return 0;
	}
	report(path, battery, &f, hide, icons);
	return 0;
}
