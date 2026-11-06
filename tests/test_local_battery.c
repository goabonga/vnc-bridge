/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#define main vncb_telemetry_main
#include "../src/vnc-bridge-telemetry.c"
#undef main
#include "harness.h"
#include "run.h"
#include <ftw.h>
#include <sys/stat.h>

static char root[] = "/tmp/vncb-power-XXXXXX";

static void put(const char *device, const char *key, const char *value)
{
	char dir[300], path[400];
	snprintf(dir, sizeof dir, "%s/%s", root, device);
	mkdir(dir, 0700);
	snprintf(path, sizeof path, "%s/%s", dir, key);
	write_file(path, value, 0600);
}

static int remove_entry(const char *path, const struct stat *st, int flag,
			struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

static void remove_tree(void)
{
	CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
}

static void reset(void)
{
	remove_tree();
	CHECK(mkdir(root, 0700) == 0);
}

/* Run local_battery and return what it printed. */
static char *printed(void)
{
	char out[300];
	snprintf(out, sizeof out, "%s.out", root);
	fflush(stdout);
	int saved = dup(1);
	FILE *f = freopen(out, "w", stdout);
	CHECK(f != NULL);
	local_battery(root);
	fflush(stdout);
	dup2(saved, 1);
	close(saved);
	clearerr(stdout);
	char *text = read_file(out);
	unlink(out);
	return text;
}

static void expect(const char *want)
{
	char *got = printed();
	CHECK_STR(got, want);
	free(got);
}

int main(void)
{
	CHECK(mkdtemp(root) != NULL);

	/* No power supply. */
	expect("\n");

	/* Capacity bounds and parsing. */
	put("BAT0", "type", "Battery\n");
	put("BAT0", "status", "Discharging\n");
	put("BAT0", "capacity", "95\n");
	expect("  95% \uF240 \n");
	put("BAT0", "capacity", "0\n");
	expect("   0% \uF244 \n");
	put("BAT0", "capacity", "100");
	expect(" 100% \uF240 \n");
	put("BAT0", "capacity", "101\n");
	expect("\n");
	put("BAT0", "capacity", "bad\n");
	expect("\n");
	put("BAT0", "capacity", "-5\n");
	expect("\n");
	put("BAT0", "capacity", "\n");
	expect("\n");

	/* A battery which is not present is skipped. */
	put("BAT0", "capacity", "50\n");
	put("BAT0", "present", "0\n");
	expect("\n");
	put("BAT0", "present", "1\n");
	expect("  50% \uF242 \n");

	/* Charging status shows the bolt. */
	put("BAT0", "status", "Charging\n");
	expect("  50% \uF242 \uF0E7 \n");

	/* Other power supplies are not batteries. */
	reset();
	put("ac", "type", "Mains\n");
	put("ac", "capacity", "80\n");
	expect("\n");

	/* The first valid battery in name order is used. */
	put("BAT1", "type", "Battery\n");
	put("BAT1", "capacity", "20\n");
	put("BAT1", "status", "Full\n");
	put("BAT0", "type", "Battery\n");
	put("BAT0", "capacity", "bad\n");
	expect("  20% \uF243 \n");
	put("BAT0", "capacity", "70\n");
	expect("  70% \uF241 \n");

	/* An Unknown battery status defers to the first external supply that
	 * reports a known state. */
	reset();
	put("BAT0", "type", "Battery\n");
	put("BAT0", "capacity", "95\n");
	put("BAT0", "status", "Unknown\n");
	expect("  95% \uF240 \n");
	put("charger", "type", "USB\n");
	put("charger", "status", "Charging\n");
	expect("  95% \uF240 \uF0E7 \n");
	put("charger", "status", "Not charging\n");
	expect("  95% \uF240 \n");
	put("charger", "status", "Unknown\n");
	put("usbc", "type", "Mains\n");
	put("usbc", "status", "Charging\n");
	expect("  95% \uF240 \uF0E7 \n");
	put("BAT0", "status", "Discharging\n");
	expect("  95% \uF240 \n");

	/* The command works without a runtime directory. */
	char *help[] = {"build/bin/vnc-bridge-telemetry", "local-battery",
			NULL};
	run_result r = run_cmd(help, NULL);
	CHECK(r.status == 0);
	CHECK(r.out != NULL && r.out[strlen(r.out) - 1] == '\n');
	free(r.out);

	remove_tree();
	return vncb_finish("test_local_battery");
}
