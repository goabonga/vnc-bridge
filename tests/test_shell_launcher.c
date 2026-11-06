/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "../src/files.h"
#include "harness.h"
#include "run.h"
#include <ftw.h>
#include <limits.h>

#define SHELL "build/bin/vnc-bridge-shell"
#define TITLE "GPD \xc2\xb7 shell local RFB"

static char root[] = "/tmp/tvx-launcher-XXXXXX";
static char bin[300], log_path[300], home[300];

static void tool(const char *name, const char *body)
{
	char path[400];
	snprintf(path, sizeof path, "%s/%s", bin, name);
	write_file(path, body, 0700);
}

static void remove_tool(const char *name)
{
	char path[400];
	snprintf(path, sizeof path, "%s/%s", bin, name);
	unlink(path);
}

static void clear_log(void)
{
	unlink(log_path);
}

static char *logged(void)
{
	char *text = read_file(log_path);
	return text ? text : strdup("");
}

/* Fake i3-msg: logs its arguments, answers get_tree with $TREE and the focus
 * command with $FOCUS. */
static void install_i3msg(void)
{
	tool("i3-msg", "#!/bin/sh\nprintf 'i3-msg %s\\n' \"$*\" >> \"$LOG\"\n"
		       "case \"$*\" in\n"
		       "  *get_tree*) printf '%s' \"$TREE\" ;;\n"
		       "  *focus*) printf '%s' \"$FOCUS\" ;;\n"
		       "esac\n");
}

/* Fake terminal: logs each argument on its own line. */
static void install_terminal(const char *name)
{
	char body[512];
	snprintf(
	    body, sizeof body,
	    "#!/bin/sh\nprintf '%s' >> \"$LOG\"\nfor a in \"$@\"; do printf ' "
	    "[%%s]' \"$a\" >> \"$LOG\"; done\n"
	    "printf '\\n' >> \"$LOG\"\n",
	    name);
	tool(name, body);
}

static int launch(char *const argv[], char **log)
{
	run_result r = run_cmd(argv, NULL);
	free(r.out);
	*log = logged();
	return r.status;
}

static void test_focus(void)
{
	install_i3msg();
	install_terminal("alacritty");
	setenv("TREE",
	       "{\"nodes\":[{\"nodes\":[],\"floating_nodes\":[{\"id\":42,"
	       "\"window\":7,"
	       "\"window_properties\":{\"title\":\"" TITLE "\"}}]}]}",
	       1);
	setenv("FOCUS", "[{\"success\":true}]", 1);
	char *argv[] = {SHELL, NULL};
	char *log;
	clear_log();
	CHECK(launch(argv, &log) == 0);
	CHECK(strstr(log, "i3-msg -r -t get_tree") != NULL);
	CHECK(strstr(log, "i3-msg -r [con_id=42] focus") != NULL);
	CHECK(strstr(log, "alacritty") == NULL);
	free(log);

	/* A window that is not the shell is ignored: a terminal opens. */
	setenv("TREE",
	       "{\"nodes\":[{\"id\":5,\"window\":9,\"window_properties\":{"
	       "\"title\":"
	       "\"other\"}}]}",
	       1);
	clear_log();
	CHECK(launch(argv, &log) == 0);
	CHECK(strstr(log, "focus") == NULL);
	CHECK(strstr(log, "alacritty") != NULL);
	free(log);

	/* A failed focus opens a terminal. */
	setenv("TREE",
	       "{\"nodes\":[{\"id\":42,\"window\":7,\"window_properties\":{"
	       "\"title\":"
	       "\"" TITLE "\"}}]}",
	       1);
	setenv("FOCUS", "[{\"success\":false}]", 1);
	clear_log();
	CHECK(launch(argv, &log) == 0);
	CHECK(strstr(log, "alacritty") != NULL);
	free(log);

	/* Without i3-msg, a terminal opens. */
	remove_tool("i3-msg");
	clear_log();
	CHECK(launch(argv, &log) == 0);
	CHECK(strstr(log, "alacritty") != NULL);
	free(log);
}

static void test_terminal_command(void)
{
	install_i3msg();
	install_terminal("alacritty");
	setenv("TREE", "{}", 1);
	setenv("FOCUS", "[]", 1);
	char *log;

	/* The terminal runs this program with --connect and the given options.
	 */
	char *with_args[] = {SHELL, "--hostname", "nano", NULL};
	clear_log();
	CHECK(launch(with_args, &log) == 0);
	char self[PATH_MAX];
	CHECK(realpath(SHELL, self) != NULL);
	char want[PATH_MAX + 200];
	snprintf(want, sizeof want,
		 "alacritty [-T] [" TITLE
		 "] [-e] [%s] [--connect] [--hostname] [nano]\n",
		 self);
	CHECK_STR(log, want);
	/* With arguments, an existing window is not searched. */
	CHECK(strstr(log, "i3-msg") == NULL);
	free(log);

	/* Without arguments the same command line has no extra option. */
	char *bare[] = {SHELL, NULL};
	clear_log();
	CHECK(launch(bare, &log) == 0);
	snprintf(want, sizeof want,
		 "i3-msg -r -t get_tree\nalacritty [-T] [" TITLE
		 "] [-e] [%s] [--connect]\n",
		 self);
	CHECK_STR(log, want);
	free(log);
}

static void test_terminal_choice(void)
{
	remove_tool("i3-msg");
	char config_dir[400], config[500];
	snprintf(config_dir, sizeof config_dir, "%s/.config", home);
	mkdir(config_dir, 0700);
	snprintf(config_dir, sizeof config_dir, "%s/.config/vnc-bridge", home);
	mkdir(config_dir, 0700);
	snprintf(config, sizeof config, "%s/shell.json", config_dir);
	char *argv[] = {SHELL, NULL};
	char *log;

	install_terminal("alacritty");
	install_terminal("xterm");
	install_terminal("wterm");

	/* The configured terminal comes first, by name or by path. */
	write_file(config, "{\"terminal\": \"wterm\"}", 0600);
	clear_log();
	CHECK(launch(argv, &log) == 0);
	CHECK(strncmp(log, "wterm ", 6) == 0);
	free(log);
	char json[600];
	snprintf(json, sizeof json, "{\"terminal\": \"%s/xterm\"}", bin);
	write_file(config, json, 0600);
	clear_log();
	CHECK(launch(argv, &log) == 0);
	CHECK(strncmp(log, "xterm ", 6) == 0);
	free(log);

	/* Unusable values fall back to alacritty, then xterm. */
	const char *fallbacks[] = {"{\"terminal\": \"absent\"}",
				   "{\"terminal\": \"\"}", "{\"terminal\": 42}",
				   "not json", "[1]"};
	for (size_t i = 0; i < sizeof fallbacks / sizeof *fallbacks; i++) {
		write_file(config, fallbacks[i], 0600);
		clear_log();
		CHECK(launch(argv, &log) == 0);
		CHECK(strncmp(log, "alacritty ", 10) == 0);
		free(log);
	}
	remove_tool("alacritty");
	clear_log();
	CHECK(launch(argv, &log) == 0);
	CHECK(strncmp(log, "xterm ", 6) == 0);
	free(log);

	/* No terminal at all. */
	remove_tool("xterm");
	remove_tool("wterm");
	clear_log();
	CHECK(launch(argv, &log) == 2);
	CHECK_STR(log, "");
	free(log);
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
	CHECK(mkdtemp(root) != NULL);
	snprintf(bin, sizeof bin, "%s/bin", root);
	snprintf(home, sizeof home, "%s/home", root);
	snprintf(log_path, sizeof log_path, "%s/log", root);
	CHECK(mkdir(bin, 0700) == 0);
	CHECK(mkdir(home, 0700) == 0);
	setenv("HOME", home, 1);
	setenv("PATH", bin, 1);
	setenv("LOG", log_path, 1);

	test_focus();
	test_terminal_command();
	test_terminal_choice();

	CHECK(nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
	return vncb_finish("test_shell_launcher");
}
