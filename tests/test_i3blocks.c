/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#define main vncb_i3blocks_main
#include "../src/vnc-bridge-i3blocks.c"
#undef main
#include "base.h"
#include "harness.h"
#include "run.h"

static char *decorated(const char *input)
{
	J *blocks = json_tokener_parse(input);
	J *out = decorate(blocks);
	char *text =
	    strdup(json_object_to_json_string_ext(out, JSON_C_TO_STRING_PLAIN));
	json_object_put(out);
	json_object_put(blocks);
	return text;
}

#define ARROW "\xee\x82\xb2"

static void test_decorate(void)
{
	char *out = decorated("[]");
	CHECK_STR(out, "[]");
	free(out);

	/* Hidden blocks: pad, sep, empty and blank text. */
	out = decorated("[{\"name\":\"pad\",\"full_text\":\" \"},"
			"{\"name\":\"sep\",\"full_text\":\"x\"},"
			"{\"name\":\"a\",\"full_text\":\"\"},"
			"{\"name\":\"b\",\"full_text\":\"  \"},"
			"{\"name\":\"c\"}]");
	CHECK_STR(out, "[]");
	free(out);

	/* Visible blocks alternate backgrounds, each preceded by an arrow. */
	out = decorated(
	    "[{\"name\":\"audio\",\"full_text\":\"  38%  "
	    "\",\"color\":\"#586e75\"},"
	    "{\"name\":\"battery\",\"full_text\":\"\"},"
	    "{\"name\":\"memory\",\"instance\":\"ram\",\"full_text\":\"20%\","
	    "\"short_text\":\"2\",\"urgent\":true},"
	    "{\"name\":\"memory\",\"instance\":\"swap\",\"full_text\":\" 0% "
	    "\"}]");
	const char *expected =
	    "[{\"name\":\"__solarized_sep\",\"full_text\":\"" ARROW
	    "\",\"color\":\"#073642\",\"background\":\"#002b36\",\"separator\":"
	    "false,"
	    "\"separator_block_width\":0},"
	    "{\"name\":\"audio\",\"full_text\":\"  38% "
	    "\",\"color\":\"#586e75\","
	    "\"background\":\"#073642\",\"separator\":false,\"separator_block_"
	    "width\":0},"
	    "{\"name\":\"__solarized_sep\",\"full_text\":\"" ARROW
	    "\",\"color\":\"#002b36\",\"background\":\"#073642\",\"separator\":"
	    "false,"
	    "\"separator_block_width\":0},"
	    "{\"name\":\"memory\",\"instance\":\"ram\",\"full_text\":\" 20% \","
	    "\"short_text\":\" 2 \",\"urgent\":true,\"background\":\"#002b36\","
	    "\"separator\":false,\"separator_block_width\":0},"
	    "{\"name\":\"__solarized_sep\",\"full_text\":\"" ARROW
	    "\",\"color\":\"#073642\",\"background\":\"#002b36\",\"separator\":"
	    "false,"
	    "\"separator_block_width\":0},"
	    "{\"name\":\"memory\",\"instance\":\"swap\",\"full_text\":\" 0% \","
	    "\"background\":\"#073642\",\"separator\":false,\"separator_block_"
	    "width\":0}]";
	CHECK_STR(out, expected);
	free(out);
}

static void test_stream(void)
{
	char dir[] = "/tmp/vncb-i3blocks-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	char fake[256], click[256], input[256], path[1024];
	snprintf(fake, sizeof fake, "%s/i3blocks", dir);
	snprintf(click, sizeof click, "%s/click", dir);
	snprintf(input, sizeof input, "%s/input", dir);
	snprintf(path, sizeof path, "%s:%s", dir, getenv("PATH"));
	setenv("PATH", path, 1);
	setenv("CLICK_FILE", click, 1);
	setenv("BLOCKS", "[{\"name\":\"audio\",\"full_text\":\"38%\"}]", 1);
	write_file(input, "[{\"button\":1}]", 0600);
	write_file(fake,
		   "#!/bin/sh\ncat > \"$CLICK_FILE\"\n"
		   "printf '%s\\n' '{\"version\":1,\"click_events\":true}' '[' "
		   "\"$BLOCKS\" \",$BLOCKS\"\n",
		   0700);

	char *argv[] = {"build/bin/vnc-bridge-i3blocks", NULL};
	run_result r = run_cmd(argv, input);
	CHECK(r.status == 0);
	char *decorated_blocks = decorated(getenv("BLOCKS"));
	char expected[2048];
	snprintf(expected, sizeof expected,
		 "{\"version\":1,\"click_events\":true}\n[\n%s\n,%s\n",
		 decorated_blocks, decorated_blocks);
	CHECK_STR(r.out, expected);
	free(decorated_blocks);
	free(r.out);
	char *received = NULL;
	FILE *f = fopen(click, "r");
	if (f) {
		char buf[64] = "";
		CHECK(fgets(buf, sizeof buf, f) != NULL);
		fclose(f);
		received = strdup(buf);
	}
	CHECK_STR(received, "[{\"button\":1}]");
	free(received);

	/* The exit status of i3blocks is propagated. */
	write_file(fake, "#!/bin/sh\nexit 7\n", 0700);
	r = run_cmd(argv, NULL);
	CHECK(r.status == 7);
	free(r.out);

	/* A malformed block array ends the stream with a failure. */
	write_file(
	    fake,
	    "#!/bin/sh\nprintf '%s\\n' '{\"version\":1}' '[' 'not json'\n",
	    0700);
	r = run_cmd(argv, NULL);
	CHECK(r.status == 1);
	CHECK_STR(r.out, "{\"version\":1}\n[\n");
	free(r.out);

	/* Usage text. */
	char *help[] = {"build/bin/vnc-bridge-i3blocks", "--help", NULL};
	r = run_cmd(help, NULL);
	CHECK(r.status == 0);
	CHECK_STR(r.out, "Usage: vnc-bridge-i3blocks [i3blocks options]\n");
	free(r.out);

	char *version[] = {"build/bin/vnc-bridge-i3blocks", "--version", NULL};
	r = run_cmd(version, NULL);
	CHECK(r.status == 0);
	CHECK_STR(r.out, BASE_VERSION "\n");
	free(r.out);

	unlink(fake);
	unlink(click);
	unlink(input);
	rmdir(dir);
}

int main(void)
{
	test_decorate();
	test_stream();
	return vncb_finish("test_i3blocks");
}
