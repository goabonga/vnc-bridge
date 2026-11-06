/* SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from the i3blocks wifi script, Copyright 2014 Alexander Keller.
 * Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "exec.h"
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *env_or(const char *name, const char *fallback)
{
	const char *value = getenv(name);
	return value ? value : fallback;
}

/* Fill text with the block label for a quality in percent (-1 when the link
 * is down) and return the color to use. */
static const char *wifi_display(int quality, char *text, size_t size)
{
	if (quality < 0) {
		snprintf(text, size, "  --%% 󰤭 ");
		return env_or("COLOR_DOWN", "#586e75");
	}
	const char *icon = quality <= 33   ? "󰤟"
			   : quality <= 66 ? "󰤢"
					   : "󰤨";
	snprintf(text, size, " %3d%% %s ", quality, icon);
	if (quality >= 80)
		return env_or("COLOR_GE80", "#859900");
	if (quality >= 60)
		return env_or("COLOR_GE60", "#b58900");
	if (quality >= 40)
		return env_or("COLOR_GE40", "#cb4b16");
	return env_or("COLOR_LOWR", "#dc322f");
}

/* Convert the signal level of `iw dev IFACE link` output to a percentage,
 * or return -1 when no usable level is reported. */
static int wifi_quality(const char *link)
{
	const char *signal = strstr(link, "signal:");
	if (!signal)
		return -1;
	char *end;
	double dbm = strtod(signal + strlen("signal:"), &end);
	if (end == signal + strlen("signal:") || !isfinite(dbm))
		return -1;
	return (int)fmax(0, fmin(100, nearbyint((dbm + 100) * 2)));
}

/* Copy the first "Interface NAME" entry of `iw dev` output into out. */
static bool wifi_first_interface(const char *devices, char *out, size_t size)
{
	for (const char *line = devices; line && *line;) {
		while (*line == ' ' || *line == '\t')
			line++;
		char name[256];
		if (sscanf(line, "Interface %255s", name) == 1 &&
		    strlen(name) < size) {
			snprintf(out, size, "%s", name);
			return true;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return false;
}

static bool wifi_is_wireless(const char *sysfs_net, const char *iface)
{
	if (!*iface || strchr(iface, '/'))
		return false;
	char path[512];
	if (snprintf(path, sizeof path, "%s/%s/wireless", sysfs_net, iface) >=
	    (int)sizeof path)
		return false;
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--help")) {
		puts("Usage: vnc-bridge-wifi (INTERFACE or BLOCK_INSTANCE "
		     "optional)");
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--version")) {
		puts(BASE_VERSION);
		return 0;
	}
	char iface[256];
	snprintf(iface, sizeof iface, "%s",
		 env_or("INTERFACE", env_or("BLOCK_INSTANCE", "")));
	if (!*iface) {
		char *devices_cmd[] = {"iw", "dev", NULL};
		char *devices = capture(devices_cmd, 1000);
		if (!devices)
			return 0;
		bool found = wifi_first_interface(devices, iface, sizeof iface);
		free(devices);
		if (!found)
			return 0;
	}
	if (!wifi_is_wireless("/sys/class/net", iface))
		return 0;
	char *link_cmd[] = {"iw", "dev", iface, "link", NULL};
	char *link = capture(link_cmd, 1000);
	if (!link)
		return 0;
	int quality = wifi_quality(link);
	free(link);
	char text[128];
	const char *color = wifi_display(quality, text, sizeof text);
	printf("%s\n%s\n%s\n", text, text, color);
	return 0;
}
