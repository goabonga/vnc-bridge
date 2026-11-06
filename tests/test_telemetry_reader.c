/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "harness.h"
#include "run.h"
#include <stdarg.h>
#include <time.h>

static char dir[] = "/tmp/tvx-telemetry-XXXXXX";
static char status_path[256];

static void put_status(const char *clients_fmt, ...)
    __attribute__((format(printf, 1, 2)));

static void put_status(const char *clients_fmt, ...)
{
	char clients[2048];
	va_list ap;
	va_start(ap, clients_fmt);
	vsnprintf(clients, sizeof clients, clients_fmt, ap);
	va_end(ap);
	char body[2200];
	snprintf(body, sizeof body, "{\"version\":1,\"clients\":[%s]}\n",
		 clients);
	write_file(status_path, body, 0600);
}

/* One client record received `age` seconds ago. */
static void record(char *out, size_t size, const char *id, const char *host,
		   const char *battery, const char *audio, int age)
{
	snprintf(out, size,
		 "{\"id\":\"%s\",\"hostname\":\"%s\",\"battery\":%s,%s"
		 "\"received_at\":%.3f}",
		 id, host, battery, audio, (double)time(NULL) - age);
}

static char *report(const char *command, const char *extra)
{
	char *argv[16];
	int n = 0;
	argv[n++] = "build/bin/vnc-bridge-telemetry";
	argv[n++] = (char *)command;
	argv[n++] = "--runtime-dir";
	argv[n++] = dir;
	char copy[256];
	snprintf(copy, sizeof copy, "%s", extra);
	for (char *t = strtok(copy, " "); t; t = strtok(NULL, " "))
		argv[n++] = strdup(t);
	argv[n] = NULL;
	run_result r = run_cmd(argv, NULL);
	for (int i = 4; i < n; i++)
		free(argv[i]);
	return r.out;
}

static void expect(const char *command, const char *extra, const char *want)
{
	char *got = report(command, extra);
	CHECK_STR(got, want);
	free(got);
}

static int status_of(char *const argv[])
{
	run_result r = run_cmd(argv, NULL);
	free(r.out);
	return r.status;
}

static void test_plain_text(void)
{
	char r[512];
	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":73,\"status\":\"Charging\"}",
	       "\"audio\":{\"volume\":65,\"muted\":false},", 1);
	put_status("%s", r);
	expect("battery", "", "BAT 73%+ (nano)\n");
	expect("audio", "", "VOL 65%\n");

	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":9,\"status\":\"Discharging\"}",
	       "\"audio\":{\"volume\":38,\"muted\":true},", 1);
	put_status("%s", r);
	expect("battery", "", "BAT 9% (nano)\n");
	expect("audio", "", "VOL MUET\n");
}

static void test_icons(void)
{
	static const struct {
		int level;
		const char *icon;
	} battery_icons[] = {{0, "\uF244"},  {9, "\uF244"},  {10, "\uF243"},
			     {34, "\uF243"}, {35, "\uF242"}, {64, "\uF242"},
			     {65, "\uF241"}, {89, "\uF241"}, {90, "\uF240"},
			     {100, "\uF240"}};
	char r[512], battery[128], want[128];
	for (size_t i = 0; i < sizeof battery_icons / sizeof *battery_icons;
	     i++) {
		int level = battery_icons[i].level;
		snprintf(battery, sizeof battery,
			 "{\"capacity\":%d,\"status\":\"Full\"}", level);
		record(r, sizeof r, "1:5", "nano", battery, "", 1);
		put_status("%s", r);
		snprintf(want, sizeof want, " %3d%% %s \n", level,
			 battery_icons[i].icon);
		expect("battery", "--i3blocks", want);
	}
	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":73,\"status\":\"Charging\"}", "", 1);
	put_status("%s", r);
	expect("battery", "--i3blocks", "  73% \uF241 \uF0E7 \n");

	static const struct {
		int level;
		const char *icon;
	} volume_icons[] = {{0, "\uF026"},  {25, "\uF026"}, {26, "\uF027"},
			    {65, "\uF027"}, {66, "\uF028"}, {1000, "\uF028"}};
	char audio[128];
	for (size_t i = 0; i < sizeof volume_icons / sizeof *volume_icons;
	     i++) {
		int level = volume_icons[i].level;
		snprintf(audio, sizeof audio,
			 "\"audio\":{\"volume\":%d,\"muted\":false},", level);
		record(r, sizeof r, "1:5", "nano",
		       "{\"capacity\":1,\"status\":\"Full\"}", audio, 1);
		put_status("%s", r);
		snprintf(want, sizeof want, " %3d%% %s \n %3d%% %s \n#fc9600\n",
			 level, volume_icons[i].icon, level,
			 volume_icons[i].icon);
		expect("audio", "--i3blocks", want);
	}
	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":1,\"status\":\"Full\"}",
	       "\"audio\":{\"volume\":80,\"muted\":true},", 1);
	put_status("%s", r);
	expect("audio", "--i3blocks",
	       "  80% \uF026 \n  80% \uF026 \n#586e75\n");
}

static void test_unavailable(void)
{
	char r[512];
	unlink(status_path);
	expect("battery", "", "BAT n/a\n");
	expect("audio", "", "VOL n/a\n");
	expect("battery", "--i3blocks", "n/a \n");
	expect("audio", "--hide-unavailable", "\n");

	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":73,\"status\":\"Full\"}",
	       "\"audio\":{\"volume\":65,\"muted\":false},", 91);
	put_status("%s", r);
	expect("battery", "", "BAT n/a\n");
	expect("audio", "--hide-unavailable", "\n");

	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":73,\"status\":\"Full\"}",
	       "\"audio\":{\"volume\":65,\"muted\":false},", 89);
	put_status("%s", r);
	expect("battery", "", "BAT 73% (nano)\n");
	expect("battery", "--hostname other", "BAT n/a\n");
	expect("battery", "--client missing", "BAT n/a\n");

	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":null,\"status\":\"Unknown\"}",
	       "\"audio\":{\"volume\":null,\"muted\":false},", 1);
	put_status("%s", r);
	expect("battery", "", "BAT n/a\n");
	expect("audio", "", "VOL n/a\n");

	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":73,\"status\":\"Full\"}", "", 1);
	put_status("%s", r);
	expect("audio", "", "VOL n/a\n");

	write_file(status_path, "not json", 0600);
	expect("battery", "", "BAT n/a\n");
}

static void test_selection(void)
{
	char a[512], b[512], c[512];
	record(a, sizeof a, "1:5", "first",
	       "{\"capacity\":10,\"status\":\"Full\"}", "", 30);
	record(b, sizeof b, "2:6", "second",
	       "{\"capacity\":20,\"status\":\"Full\"}", "", 5);
	record(c, sizeof c, "3:7", "third",
	       "{\"capacity\":30,\"status\":\"Full\"}", "", 60);
	put_status("%s,%s,%s", a, b, c);
	expect("battery", "", "BAT 20% (second)\n");
	expect("battery", "--hostname third", "BAT 30% (third)\n");
	expect("battery", "--client 1:5", "BAT 10% (first)\n");
	expect("battery", "--client 1:5 --hostname second", "BAT n/a\n");
}

static void test_command_line(void)
{
	char r[512];
	record(r, sizeof r, "1:5", "nano",
	       "{\"capacity\":73,\"status\":\"Full\"}", "", 1);
	put_status("%s", r);

	char *argv_status[] = {"build/bin/vnc-bridge-telemetry", "status",
			       "--runtime-dir", dir, NULL};
	run_result s = run_cmd(argv_status, NULL);
	CHECK(s.status == 0);
	char *body = NULL;
	FILE *f = fopen(status_path, "r");
	if (f) {
		char buf[1024] = "";
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		buf[n] = 0;
		fclose(f);
		body = strdup(buf);
	}
	CHECK_STR(s.out, body);
	free(body);
	free(s.out);

	unlink(status_path);
	CHECK(status_of(argv_status) == 1);

	char *no_command[] = {"build/bin/vnc-bridge-telemetry", NULL};
	CHECK(status_of(no_command) == 2);
	char *bad_option[] = {"build/bin/vnc-bridge-telemetry", "battery",
			      "--bogus", NULL};
	CHECK(status_of(bad_option) == 2);
	char *bad_command[] = {"build/bin/vnc-bridge-telemetry", "bogus",
			       "--runtime-dir", dir, NULL};
	CHECK(status_of(bad_command) == 2);
	char *dangling[] = {"build/bin/vnc-bridge-telemetry", "battery",
			    "--client", NULL};
	CHECK(status_of(dangling) == 2);

	char *help[] = {"build/bin/vnc-bridge-telemetry", "--help", NULL};
	run_result h = run_cmd(help, NULL);
	CHECK(h.status == 0);
	CHECK_STR(
	    h.out,
	    "Usage: vnc-bridge-telemetry {battery,audio,status,local-battery} "
	    "[--runtime-dir DIR] [--hostname HOST] [--client PID:FD] "
	    "[--hide-unavailable] [--i3blocks]\n");
	free(h.out);
}

static void test_default_directory(void)
{
	char *argv[] = {"build/bin/vnc-bridge-telemetry", "battery", NULL};
	char *hidden[] = {"build/bin/vnc-bridge-telemetry", "battery",
			  "--hide-unavailable", NULL};
	unsetenv("XDG_RUNTIME_DIR");
	CHECK(status_of(argv) == 2);
	run_result r = run_cmd(hidden, NULL);
	CHECK(r.status == 0);
	CHECK_STR(r.out, "\n");
	free(r.out);

	char sub[300], path[400], rec[512];
	snprintf(sub, sizeof sub, "%s/vnc-bridge", dir);
	CHECK(mkdir(sub, 0700) == 0);
	snprintf(path, sizeof path, "%s/status.json", sub);
	record(rec, sizeof rec, "1:5", "nano",
	       "{\"capacity\":42,\"status\":\"Full\"}", "", 1);
	char body[700];
	snprintf(body, sizeof body, "{\"version\":1,\"clients\":[%s]}\n", rec);
	write_file(path, body, 0600);
	setenv("XDG_RUNTIME_DIR", dir, 1);
	r = run_cmd(argv, NULL);
	CHECK(r.status == 0);
	CHECK_STR(r.out, "BAT 42% (nano)\n");
	free(r.out);
	unlink(path);
	rmdir(sub);
}

int main(void)
{
	CHECK(mkdtemp(dir) != NULL);
	snprintf(status_path, sizeof status_path, "%s/status.json", dir);
	test_plain_text();
	test_icons();
	test_unavailable();
	test_selection();
	test_command_line();
	test_default_directory();
	unlink(status_path);
	rmdir(dir);
	{
		char *version_argv[] = {"build/bin/vnc-bridge-telemetry",
					"--version", NULL};
		run_result version_run = run_cmd(version_argv, NULL);
		CHECK(version_run.status == 0);
		CHECK_STR(version_run.out, BASE_VERSION "\n");
		free(version_run.out);
	}
	return vncb_finish("test_telemetry_reader");
}
