/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include <sys/stat.h>
#define main vncb_wifi_main
#include "../src/vnc-bridge-wifi.c"
#undef main
#include "harness.h"

static void expect_display(int quality, const char *expected)
{
	char text[128];
	const char *color = wifi_display(quality, text, sizeof text);
	char got[256];
	snprintf(got, sizeof got, "%s|%s", text, color);
	CHECK_STR(got, expected);
}

static void make_dir(const char *path)
{
	CHECK(mkdir(path, 0700) == 0);
}

int main(void)
{
	unsetenv("COLOR_DOWN");
	unsetenv("COLOR_GE80");
	unsetenv("COLOR_GE60");
	unsetenv("COLOR_GE40");
	unsetenv("COLOR_LOWR");

	/* Icon follows three levels; color follows four thresholds. */
	expect_display(-1, "  --% 󰤭 |#586e75");
	expect_display(0, "   0% 󰤟 |#dc322f");
	expect_display(33, "  33% 󰤟 |#dc322f");
	expect_display(34, "  34% 󰤢 |#dc322f");
	expect_display(40, "  40% 󰤢 |#cb4b16");
	expect_display(60, "  60% 󰤢 |#b58900");
	expect_display(66, "  66% 󰤢 |#b58900");
	expect_display(67, "  67% 󰤨 |#b58900");
	expect_display(80, "  80% 󰤨 |#859900");
	expect_display(100, " 100% 󰤨 |#859900");

	setenv("COLOR_DOWN", "#111111", 1);
	setenv("COLOR_GE80", "#222222", 1);
	setenv("COLOR_GE60", "#333333", 1);
	setenv("COLOR_GE40", "#444444", 1);
	setenv("COLOR_LOWR", "#555555", 1);
	expect_display(-1, "  --% 󰤭 |#111111");
	expect_display(90, "  90% 󰤨 |#222222");
	expect_display(70, "  70% 󰤨 |#333333");
	expect_display(50, "  50% 󰤢 |#444444");
	expect_display(10, "  10% 󰤟 |#555555");

	/* dBm to percentage: (dBm + 100) * 2, rounded and clamped. */
	CHECK(wifi_quality("\tsignal: -50 dBm\n") == 100);
	CHECK(wifi_quality("\tsignal: -30 dBm\n") == 100);
	CHECK(wifi_quality("\tsignal: -100 dBm\n") == 0);
	CHECK(wifi_quality("\tsignal: -120 dBm\n") == 0);
	CHECK(wifi_quality("\tsignal: -75.5 dBm\n") == 49);
	CHECK(wifi_quality("Not connected.\n") == -1);
	CHECK(wifi_quality("\tsignal: nan dBm\n") == -1);
	CHECK(wifi_quality("") == -1);

	/* First interface reported by `iw dev`. */
	char iface[64];
	CHECK(wifi_first_interface("phy#0\n\tInterface wlan0\n\t\tifindex 3\n"
				   "\tInterface wlan1\n",
				   iface, sizeof iface));
	CHECK_STR(iface, "wlan0");
	CHECK(!wifi_first_interface("phy#0\n", iface, sizeof iface));

	/* Only interfaces with a sysfs wireless directory qualify. */
	char root[] = "/tmp/vncb-wifi-XXXXXX";
	CHECK(mkdtemp(root) != NULL);
	char path[256];
	snprintf(path, sizeof path, "%s/wlan0", root);
	make_dir(path);
	snprintf(path, sizeof path, "%s/wlan0/wireless", root);
	make_dir(path);
	snprintf(path, sizeof path, "%s/eth0", root);
	make_dir(path);
	CHECK(wifi_is_wireless(root, "wlan0"));
	CHECK(!wifi_is_wireless(root, "eth0"));
	CHECK(!wifi_is_wireless(root, "missing"));
	CHECK(!wifi_is_wireless(root, "../wlan0"));
	CHECK(!wifi_is_wireless(root, ""));

	snprintf(path, sizeof path, "%s/wlan0/wireless", root);
	rmdir(path);
	snprintf(path, sizeof path, "%s/wlan0", root);
	rmdir(path);
	snprintf(path, sizeof path, "%s/eth0", root);
	rmdir(path);
	rmdir(root);

	/* The built program documents its usage. */
	char program[256];
	snprintf(program, sizeof program, "%s/vnc-bridge-wifi",
		 getenv("VNCB_BIN") ? getenv("VNCB_BIN") : "build/bin");
	char *help_cmd[] = {program, "--help", NULL};
	char *help = capture(help_cmd, 2000);
	CHECK_STR(
	    help,
	    "Usage: vnc-bridge-wifi (INTERFACE or BLOCK_INSTANCE optional)\n");
	free(help);
	char *version_cmd[] = {program, "--version", NULL};
	char *version = capture(version_cmd, 2000);
	CHECK_STR(version, BASE_VERSION "\n");
	free(version);

	return vncb_finish("test_wifi");
}
