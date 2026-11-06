/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "../src/exec.h"
#include "harness.h"

static double elapsed(double start)
{
	return clocksec(CLOCK_MONOTONIC) - start;
}

int main(void)
{
	char *out;

	char *echo[] = {"printf", "%s", "hello", NULL};
	out = capture(echo, 1000);
	CHECK_STR(out, "hello");
	free(out);

	char *locale[] = {"sh", "-c", "printf %s \"$LC_ALL\"", NULL};
	out = capture(locale, 1000);
	CHECK_STR(out, "C");
	free(out);

	char *noisy[] = {"sh", "-c", "echo out; echo err >&2", NULL};
	out = capture(noisy, 1000);
	CHECK_STR(out, "out\n");
	free(out);

	char *fail[] = {"sh", "-c", "echo partial; exit 3", NULL};
	CHECK(capture(fail, 1000) == NULL);

	char *missing[] = {"tvx-no-such-command", NULL};
	CHECK(capture(missing, 1000) == NULL);

	char *slow[] = {"sleep", "5", NULL};
	double start = clocksec(CLOCK_MONOTONIC);
	CHECK(capture(slow, 200) == NULL);
	CHECK(elapsed(start) < 2.0);

	/* A child that closes stdout but keeps running is also bounded. */
	char *detached[] = {"sh", "-c", "exec >&-; sleep 5", NULL};
	start = clocksec(CLOCK_MONOTONIC);
	CHECK(capture(detached, 200) == NULL);
	CHECK(elapsed(start) < 2.0);

	char *flood[] = {"sh", "-c", "head -c 2000000 /dev/zero", NULL};
	CHECK(capture(flood, 5000) == NULL);

	char padded[] = "value \t\n";
	trim(padded);
	CHECK_STR(padded, "value");
	char blank[] = " \n";
	trim(blank);
	CHECK_STR(blank, "");

	return vncb_finish("test_exec");
}
