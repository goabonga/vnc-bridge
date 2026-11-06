/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#ifndef VNCB_HARNESS_H
#define VNCB_HARNESS_H
#include <stdio.h>
#include <string.h>

static int vncb_checks;
static int vncb_failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		vncb_checks++;                                                 \
		if (!(cond)) {                                                 \
			vncb_failures++;                                       \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #cond);                              \
		}                                                              \
	} while (0)

#define CHECK_STR(actual, expected)                                            \
	do {                                                                   \
		const char *a_ = (actual), *e_ = (expected);                   \
		vncb_checks++;                                                 \
		if (!a_ || strcmp(a_, e_) != 0) {                              \
			vncb_failures++;                                       \
			fprintf(stderr,                                        \
				"%s:%d: expected \"%s\", got \"%s\"\n",        \
				__FILE__, __LINE__, e_, a_ ? a_ : "(null)");   \
		}                                                              \
	} while (0)

static inline int vncb_finish(const char *name)
{
	printf("%s: %d checks, %d failures\n", name, vncb_checks,
	       vncb_failures);
	return vncb_failures ? 1 : 0;
}
#endif
