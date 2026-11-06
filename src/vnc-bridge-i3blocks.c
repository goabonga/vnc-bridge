/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "base.h"
#include "exec.h"
#include <json-c/json.h>
#include <stdio.h>

typedef struct json_object J;

static volatile sig_atomic_t stop;

static void stopping(int signum)
{
	(void)signum;
	stop = 1;
}

static const char *string_field(J *object, const char *key)
{
	J *value = NULL;
	if (!json_object_object_get_ex(object, key, &value))
		return "";
	const char *s = json_object_get_string(value);
	return s ? s : "";
}

static void set_string(J *object, const char *key, const char *value)
{
	json_object_object_add(object, key, json_object_new_string(value));
}

static void set_layout(J *object)
{
	json_object_object_add(object, "separator", json_object_new_boolean(0));
	json_object_object_add(object, "separator_block_width",
			       json_object_new_int(0));
}

/* Make the text start with one space and end with exactly one. */
static void pad_text(J *item, const char *key)
{
	const char *text = string_field(item, key);
	if (!*text)
		return;
	size_t length = strlen(text);
	while (length && isspace((unsigned char)text[length - 1]))
		length--;
	size_t size = length + 3;
	char *padded = malloc(size);
	if (!padded)
		exit(1);
	snprintf(padded, size, "%s%.*s ", *text == ' ' ? "" : " ", (int)length,
		 text);
	set_string(item, key, padded);
	free(padded);
}

/* Return a new array where every visible block gets an alternating Solarized
 * background, preceded by an arrow joining it to the previous background. */
static J *decorate(J *blocks)
{
	J *out = json_object_new_array();
	const char *previous = "#002b36";
	size_t visible = 0;
	for (size_t i = 0; i < json_object_array_length(blocks); i++) {
		J *block = json_object_array_get_idx(blocks, i);
		const char *name = string_field(block, "name");
		const char *text = string_field(block, "full_text");
		text += strspn(text, " \t\n\v\f\r");
		if (!*text || !strcmp(name, "pad") || !strcmp(name, "sep"))
			continue;
		const char *color = visible++ % 2 ? "#002b36" : "#073642";

		J *arrow = json_object_new_object();
		set_string(arrow, "name", "__solarized_sep");
		set_string(arrow, "full_text", "\xee\x82\xb2");
		set_string(arrow, "color", color);
		set_string(arrow, "background", previous);
		set_layout(arrow);
		json_object_array_add(out, arrow);

		J *item = NULL;
		json_object_deep_copy(block, &item, NULL);
		set_string(item, "background", color);
		set_layout(item);
		pad_text(item, "full_text");
		pad_text(item, "short_text");
		json_object_array_add(out, item);
		previous = color;
	}
	return out;
}

static void wait_for_child(pid_t pid, int *status)
{
	double end = clocksec(CLOCK_MONOTONIC) + 2;
	pid_t waited;
	while ((waited = waitpid(pid, status, WNOHANG)) == 0 ||
	       (waited < 0 && errno == EINTR)) {
		if (clocksec(CLOCK_MONOTONIC) > end) {
			kill(pid, SIGKILL);
			while (waitpid(pid, status, 0) < 0 && errno == EINTR) {
			}
			break;
		}
		poll(NULL, 0, 10);
	}
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--help")) {
		puts("Usage: vnc-bridge-i3blocks [i3blocks options]");
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--version")) {
		puts(BASE_VERSION);
		return 0;
	}
	int fds[2];
	if (pipe(fds))
		return 1;
	pid_t pid = fork();
	if (pid < 0)
		return 1;
	if (!pid) {
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		argv[0] = "i3blocks";
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	struct sigaction action = {.sa_handler = stopping};
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	signal(SIGPIPE, SIG_IGN);

	FILE *in = fdopen(fds[0], "r");
	char *line = NULL;
	size_t cap = 0;
	int rc = 0;
	bool opening = true, first = true;
	if (getline(&line, &cap, in) > 0) {
		fputs(line, stdout);
		puts("[");
		fflush(stdout);
	}
	while (!stop && getline(&line, &cap, in) > 0) {
		char *s = line;
		while (isspace((unsigned char)*s))
			s++;
		if (!*s)
			continue;
		if (opening) {
			if (*s != '[') {
				rc = 1;
				break;
			}
			s++;
			opening = false;
		}
		while (*s == ',' || isspace((unsigned char)*s))
			s++;
		if (!*s || *s == ']')
			continue;
		J *blocks = json_tokener_parse(s);
		if (!blocks || !json_object_is_type(blocks, json_type_array)) {
			if (blocks)
				json_object_put(blocks);
			rc = 1;
			break;
		}
		J *out = decorate(blocks);
		if (printf("%s%s\n", first ? "" : ",",
			   json_object_to_json_string_ext(
			       out, JSON_C_TO_STRING_PLAIN)) < 0)
			stop = 1;
		fflush(stdout);
		first = false;
		json_object_put(out);
		json_object_put(blocks);
	}
	bool completed = !stop && !rc && feof(in);
	free(line);
	fclose(in);
	if (!completed)
		kill(pid, SIGTERM);
	int status = 0;
	wait_for_child(pid, &status);
	if (completed && WIFEXITED(status))
		return WEXITSTATUS(status);
	return rc;
}
