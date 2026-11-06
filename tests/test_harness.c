/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "harness.h"

int main(void)
{
	CHECK(1 + 1 == 2);
	CHECK_STR("abc", "abc");
	int passed = vncb_checks;
	CHECK(passed == 2);
	CHECK(vncb_failures == 0);

	/* A failing check is counted; silence its diagnostic while exercising
	 * it. */
	FILE *saved = stderr;
	stderr = fopen("/dev/null", "w");
	CHECK(1 == 2);
	CHECK_STR("abc", "abd");
	CHECK_STR(NULL, "abc");
	fclose(stderr);
	stderr = saved;
	int failed = vncb_failures;
	vncb_failures = 0;
	CHECK(failed == 3);
	return vncb_finish("test_harness");
}
