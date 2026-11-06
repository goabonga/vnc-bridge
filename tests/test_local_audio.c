/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#define main vncb_telemetry_main
#include "../src/vnc-bridge-telemetry.c"
#undef main
#include "harness.h"
#include "run.h"
#include <ftw.h>
#include <sys/stat.h>

static char dir[] = "/tmp/vncb-audio-XXXXXX";
static char cards[300];
static char arguments[300];
static char full_path[2048];

static const char *const variables[] = {
    "VNC_BRIDGE_AUDIO_BACKEND", "VNC_BRIDGE_ALSA_CARD",
    "VNC_BRIDGE_ALSA_CONTROL", "VNC_BRIDGE_ALSA_TOOL"};

static void clear_environment(void)
{
	for (size_t i = 0; i < sizeof variables / sizeof *variables; i++)
		unsetenv(variables[i]);
}

static void tool(const char *name, const char *body)
{
	char path[400];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	write_file(path, body, 0700);
}

static void remove_tool(const char *name)
{
	char path[400];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	unlink(path);
}

static char *printed(void)
{
	return capture_stdout_of(local_audio, cards);
}

static void expect(const char *want)
{
	char *got = printed();
	CHECK_STR(got, want);
	free(got);
}

static char *recorded_arguments(void)
{
	return read_file(arguments);
}

static void expect_arguments(const char *want)
{
	char *got = recorded_arguments();
	CHECK_STR(got, want);
	free(got);
}

static void test_pipewire(void)
{
	clear_environment();
	setenv("VNC_BRIDGE_AUDIO_BACKEND", "pipewire", 1);
	tool("wpctl", "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$ARGUMENTS\"\n"
		      "printf '%s' \"$SAMPLE\"\n");

	setenv("SAMPLE", "Volume: 0.38\n", 1);
	expect("  38% \uF027 \n  38% \uF027 \n#fc9600\n");
	expect_arguments("get-volume\n@DEFAULT_AUDIO_SINK@\n");
	setenv("SAMPLE", "Volume: 0.80 [MUTED]\n", 1);
	expect("  80% \uF026 \n  80% \uF026 \n#586e75\n");
	setenv("SAMPLE", "Volume: 0.00 [MUTED]\n", 1);
	expect("   0% \uF026 \n   0% \uF026 \n#586e75\n");
	setenv("SAMPLE", "Volume: 1.50\n", 1);
	expect(" 150% \uF028 \n 150% \uF028 \n#fc9600\n");
	setenv("SAMPLE", "Volume: 0.15\n", 1);
	expect("  15% \uF026 \n  15% \uF026 \n#fc9600\n");

	setenv("SAMPLE", "garbage", 1);
	expect("\n");
	setenv("SAMPLE", "Volume: 11.00\n", 1);
	expect("\n");
	setenv("SAMPLE", "Volume: -1.00\n", 1);
	expect("\n");
	setenv("SAMPLE", "Volume: 0.50 extra\n", 1);
	expect("\n");

	tool("wpctl", "#!/bin/sh\nexit 1\n");
	expect("\n");
	tool("wpctl", "#!/bin/sh\nsleep 10\n");
	double start = clocksec(CLOCK_MONOTONIC);
	expect("\n");
	CHECK(clocksec(CLOCK_MONOTONIC) - start < 3);
	remove_tool("wpctl");
	setenv("PATH", dir, 1);
	expect("\n");
	setenv("PATH", full_path, 1);
}

static void test_amixer(void)
{
	clear_environment();
	setenv("VNC_BRIDGE_AUDIO_BACKEND", "alsa", 1);
	tool("amixer", "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$ARGUMENTS\"\n"
		       "printf '%s' \"$SAMPLE\"\n");

	setenv("SAMPLE",
	       "Front Left: Playback 15 [38%] [-24dB] [off]\n"
	       "Front Right: Playback 15 [38%] [off]\n",
	       1);
	expect("  38% \uF026 \n  38% \uF026 \n#586e75\n");
	expect_arguments("-c\ndefault\nsget\nMaster\n");

	setenv("SAMPLE",
	       "Front Left: Playback 15 [38%] [off]\n"
	       "Front Right: Playback 15 [65%] [on]\n",
	       1);
	expect("  65% \uF027 \n  65% \uF027 \n#fc9600\n");

	setenv("SAMPLE", "Mono: Playback 40 [90%] [-3dB]\n", 1);
	expect("  90% \uF028 \n  90% \uF028 \n#fc9600\n");

	setenv("SAMPLE", "Capture: [50%] [on]\n", 1);
	expect("\n");

	setenv("VNC_BRIDGE_ALSA_CARD", "chtrt5645", 1);
	setenv("VNC_BRIDGE_ALSA_CONTROL", "Speaker", 1);
	setenv("SAMPLE", "Front Left: Playback 15 [38%] [on]\n", 1);
	expect("  38% \uF027 \n  38% \uF027 \n#fc9600\n");
	expect_arguments("-c\nchtrt5645\nsget\nSpeaker\n");

	tool("amixer", "#!/bin/sh\nexit 1\n");
	expect("\n");

	setenv("VNC_BRIDGE_AUDIO_BACKEND", "oss", 1);
	expect("\n");
}

static void test_mixer(void)
{
	clear_environment();
	setenv("PATH", dir, 1);
	setenv("VNC_BRIDGE_AUDIO_BACKEND", "alsa", 1);
	setenv("VNC_BRIDGE_ALSA_TOOL", "mixer", 1);
	setenv("VNC_BRIDGE_ALSA_CARD", "chtrt5645", 1);
	setenv("VNC_BRIDGE_ALSA_CONTROL", "Speaker", 1);
	tool("amixer",
	     "#!/bin/sh\nprintf 'Front Left: Playback 15 [22%%] [on]\\n'\n");
	tool("mixer", "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$ARGUMENTS\"\n"
		      "printf '%s' \"$SAMPLE\"\n");

	static const struct {
		const char *reply;
		const char *want;
	} replies[] = {
	    {"41 on\n", "  41% \uF027 \n  41% \uF027 \n#fc9600\n"},
	    {"41 off\n", "  41% \uF026 \n  41% \uF026 \n#586e75\n"},
	    {"41 -\n", "  41% \uF027 \n  41% \uF027 \n#fc9600\n"},
	    {"41 on", "  41% \uF027 \n  41% \uF027 \n#fc9600\n"},
	    {"007 on\n", "   7% \uF026 \n   7% \uF026 \n#fc9600\n"},
	    {"1000 off\n", " 1000% \uF026 \n 1000% \uF026 \n#586e75\n"},
	    {"- on\n", "\n"},
	    {"- -\n", "\n"},
	    {"41%\n", "\n"},
	    {"", "\n"},
	    {"41 maybe\n", "\n"},
	    {"41 on 1\n", "\n"},
	    {"1001 on\n", "\n"},
	};
	for (size_t i = 0; i < sizeof replies / sizeof *replies; i++) {
		setenv("SAMPLE", replies[i].reply, 1);
		expect(replies[i].want);
	}
	expect_arguments("--card\nsysdefault:chtrt5645\n--get\nSpeaker\n");

	/* A mixer that fails or is missing leaves the answer to amixer. */
	tool("mixer", "#!/bin/sh\nexit 1\n");
	expect("  22% \uF026 \n  22% \uF026 \n#fc9600\n");
	remove_tool("mixer");
	expect("  22% \uF026 \n  22% \uF026 \n#fc9600\n");

	/* Any other tool name means amixer, even when mixer answers. */
	tool("mixer", "#!/bin/sh\nprintf '41 on\\n'\n");
	setenv("VNC_BRIDGE_ALSA_TOOL", "amixer", 1);
	expect("  22% \uF026 \n  22% \uF026 \n#fc9600\n");
}

static void test_detection(void)
{
	clear_environment();
	setenv("PATH", dir, 1);
	remove_tool("mixer");
	tool("mixer", "#!/bin/sh\nprintf '%s\\n' \"$@\" > "
		      "\"$ARGUMENTS\"\nprintf '41 on\\n'\n");
	tool("amixer", "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$ARGUMENTS\"\n"
		       "printf 'Front Left: Playback 15 [38%%] [on]\\n'\n");
	tool("wpctl", "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$ARGUMENTS\"\n"
		      "printf 'Volume: 0.50\\n'\n");

	/* The GPD sound card selects ALSA, its card, control and mixer. */
	write_file(cards, " 1 [chtrt5645      ]: chtrt5645 - chtrt5645\n",
		   0600);
	expect("  41% \uF027 \n  41% \uF027 \n#fc9600\n");
	expect_arguments("--card\nsysdefault:chtrt5645\n--get\nDAC1\n");

	/* The card name alone does not select mixer on another card. */
	setenv("VNC_BRIDGE_ALSA_CARD", "other", 1);
	expect("  38% \uF027 \n  38% \uF027 \n#fc9600\n");
	expect_arguments("-c\nother\nsget\nDAC1\n");
	clear_environment();

	/* Any other machine uses PipeWire. */
	write_file(cards, " 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n",
		   0600);
	expect("  50% \uF027 \n  50% \uF027 \n#fc9600\n");
	expect_arguments("get-volume\n@DEFAULT_AUDIO_SINK@\n");
	unlink(cards);
	expect("  50% \uF027 \n  50% \uF027 \n#fc9600\n");
}

static int remove_entry(const char *path, const struct stat *st, int flag,
			struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

int main(void)
{
	CHECK(mkdtemp(dir) != NULL);
	snprintf(cards, sizeof cards, "%s/cards", dir);
	snprintf(arguments, sizeof arguments, "%s/arguments", dir);
	snprintf(full_path, sizeof full_path, "%s:%s", dir, getenv("PATH"));
	setenv("PATH", full_path, 1);
	setenv("ARGUMENTS", arguments, 1);

	test_pipewire();
	test_amixer();
	test_mixer();
	test_detection();

	CHECK(nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
	return vncb_finish("test_local_audio");
}
