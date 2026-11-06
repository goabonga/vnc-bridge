/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "exec.h"
#include <dirent.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdio.h>

typedef struct json_object J;

#define EXPIRY_SECONDS 90

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

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	size_t cap = 4096, len = 0;
	char *data = malloc(cap);
	size_t n;
	while (data && (n = fread(data + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (len > 4 * 1024 * 1024) {
			free(data);
			data = NULL;
			break;
		}
		if (len + 1 >= cap) {
			char *bigger = realloc(data, cap *= 2);
			if (!bigger) {
				free(data);
				data = NULL;
			} else {
				data = bigger;
			}
		}
	}
	fclose(f);
	if (data)
		data[len] = 0;
	return data;
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
			     "{battery,audio,status,local-battery} "
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
	bool battery = !strcmp(command, "battery");
	bool sound = !strcmp(command, "audio");
	bool status = !strcmp(command, "status");
	if (!battery && !sound && !status)
		return 2;

	char dir[PATH_MAX];
	if (!directory) {
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		if (!runtime) {
			if (!status && hide) {
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
