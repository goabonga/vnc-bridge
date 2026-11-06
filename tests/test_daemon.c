/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#define main vncb_telemetry_main
#include "../src/vnc-bridge-telemetry.c"
#undef main
#include "harness.h"
#include "run.h"
#include <ftw.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

static char base[] = "/tmp/vncb-daemon-XXXXXX";

/* ---- decoding and expiry, without a process ---- */

static bool decode(const char *message, uid_t uid, pid_t pid, struct client *c)
{
	return decode_message(message, strlen(message), uid, pid, c);
}

static void test_decode(void)
{
	struct client c;
	uid_t uid = getuid();

	CHECK(decode("5:7|73|2|nano", uid, 5, &c));
	CHECK(c.pid == 5 && c.connection == 7 && c.capacity == 73 &&
	      c.state == 2);
	CHECK_STR(c.hostname, "nano");
	CHECK(!c.has_audio);

	CHECK(decode("5:7|73|1|nano|38|1", uid, 5, &c));
	CHECK(c.state == 1 && c.has_audio && c.volume == 38 && c.muted);

	CHECK(decode("5:7|255|0||65535|0", uid, 5, &c));
	CHECK(c.capacity == -1 && c.hostname[0] == 0 && c.volume == -1 &&
	      !c.muted);

	CHECK(decode("5:7|100|4|a.b_c-D9|1000|0", uid, 5, &c));
	CHECK(c.capacity == 100 && c.state == 4 && c.volume == 1000);

	/* The sender must be the same user and the pid claimed. */
	CHECK(!decode("5:7|73|2|nano", uid + 1, 5, &c));
	CHECK(!decode("5:7|73|2|nano", uid, 6, &c));
	CHECK(!decode("0:7|73|2|nano", uid, 0, &c));
	CHECK(!decode("05:7|73|2|nano", uid, 5, &c));

	/* Field limits. */
	CHECK(!decode("5:7|101|2|bad", uid, 5, &c));
	CHECK(!decode("5:7|1000|2|bad", uid, 5, &c));
	CHECK(!decode("5:7|73|5|bad", uid, 5, &c));
	CHECK(!decode("5:7|73|2|bad host", uid, 5, &c));
	CHECK(!decode("5:7|73|2|bad\n", uid, 5, &c));
	CHECK(!decode("5:7|73|2|bad|1001|0", uid, 5, &c));
	CHECK(!decode("5:7|73|2|bad|65535|1", uid, 5, &c));
	CHECK(!decode("5:7|73|2|bad|65|2", uid, 5, &c));
	CHECK(!decode("5:7|73|2|bad|65|0|extra", uid, 5, &c));
	CHECK(!decode("5:7|73|2|bad|65", uid, 5, &c));
	CHECK(!decode("5:7|73|2", uid, 5, &c));
	CHECK(!decode("5:|73|2|bad", uid, 5, &c));
	CHECK(!decode("5:99999999999999999999999|73|2|bad", uid, 5, &c));
	CHECK(!decode("", uid, 5, &c));

	char longname[200];
	snprintf(longname, sizeof longname, "5:7|73|2|%.48s",
		 "a123456789012345678901234567890123456789012345678");
	CHECK(decode(longname, uid, 5, &c));
	snprintf(longname, sizeof longname, "5:7|73|2|%.49s",
		 "a1234567890123456789012345678901234567890123456789");
	CHECK(!decode(longname, uid, 5, &c));

	/* An embedded NUL is rejected. */
	CHECK(!decode_message("5:7|73|2|bad\0x", 14, uid, 5, &c));
	/* So is a datagram over 128 bytes. */
	char big[200];
	memset(big, 'a', sizeof big);
	CHECK(!decode_message(big, sizeof big, uid, 5, &c));
}

static void test_table(void)
{
	struct clients table;
	clients_init(&table);
	struct client c;
	CHECK(decode("5:1|10|2|a", getuid(), 5, &c));
	CHECK(clients_store(&table, &c, 100.0));
	CHECK(decode("5:2|20|2|b", getuid(), 5, &c));
	CHECK(clients_store(&table, &c, 150.0));
	CHECK(table.count == 2);

	/* The same identifier replaces its record. */
	CHECK(decode("5:1|11|2|a", getuid(), 5, &c));
	CHECK(clients_store(&table, &c, 160.0));
	CHECK(table.count == 2);

	/* Records not refreshed for 90 seconds expire. */
	CHECK(!clients_expire(&table, 239.0));
	CHECK(clients_expire(&table, 240.0));
	CHECK(table.count == 1);
	CHECK(!clients_expire(&table, 249.0));
	CHECK(clients_expire(&table, 250.0));
	CHECK(table.count == 0);

	/* At most 256 clients are kept. */
	for (unsigned long i = 0; i < 256; i++) {
		char message[64];
		snprintf(message, sizeof message, "5:%lu|1|1|x", i);
		CHECK(decode(message, getuid(), 5, &c));
		CHECK(clients_store(&table, &c, 1.0));
	}
	CHECK(decode("5:1000|1|1|x", getuid(), 5, &c));
	CHECK(!clients_store(&table, &c, 1.0));
	CHECK(table.count == 256);
	CHECK(decode("5:10|2|1|x", getuid(), 5, &c));
	CHECK(clients_store(&table, &c, 2.0));
	CHECK(table.count == 256);
}

/* ---- the process ---- */

static pid_t start_daemon(const char *directory, int *error_fd)
{
	int fds[2];
	if (pipe(fds))
		return -1;
	pid_t pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);
		dup2(null, 0);
		dup2(null, 1);
		dup2(fds[1], 2);
		close(fds[0]);
		close(fds[1]);
		execl("build/bin/vnc-bridge-telemetry", "vnc-bridge-telemetry",
		      "daemon", "--runtime-dir", directory, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	*error_fd = fds[0];
	return pid;
}

static bool exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static bool wait_until(bool (*check)(const char *), const char *arg)
{
	double end = clocksec(CLOCK_MONOTONIC) + 5;
	while (clocksec(CLOCK_MONOTONIC) < end) {
		if (check(arg))
			return true;
		poll(NULL, 0, 20);
	}
	return false;
}

static int wait_exit(pid_t pid)
{
	double end = clocksec(CLOCK_MONOTONIC) + 5;
	int status = 0;
	while (clocksec(CLOCK_MONOTONIC) < end) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
		poll(NULL, 0, 20);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return -1;
}

static char *drain(int fd)
{
	char buf[512];
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	return strdup(buf);
}

static size_t client_count(const char *status)
{
	char *data = read_file(status);
	J *root = data ? json_tokener_parse(data) : NULL;
	free(data);
	J *clients = member(root, "clients");
	size_t n = clients ? json_object_array_length(clients) : (size_t)-1;
	if (root)
		json_object_put(root);
	return n;
}

static size_t expected_count;

static bool count_reached(const char *status)
{
	return client_count(status) == expected_count;
}

static void send_to(const char *socket_path, const char *message)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof addr.sun_path, "%.100s", socket_path);
	sendto(fd, message, strlen(message), 0, (struct sockaddr *)&addr,
	       sizeof addr);
	close(fd);
}

static void send_with_descriptor(const char *socket_path)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	int null = open("/dev/null", O_RDONLY);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof addr.sun_path, "%.100s", socket_path);
	char data[] = "bad";
	struct iovec io = {data, 3};
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(int))];
	} control;
	memset(&control, 0, sizeof control);
	struct msghdr msg = {.msg_name = &addr,
			     .msg_namelen = sizeof addr,
			     .msg_iov = &io,
			     .msg_iovlen = 1,
			     .msg_control = control.bytes,
			     .msg_controllen = sizeof control.bytes};
	struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &null, sizeof(int));
	sendmsg(fd, &msg, 0);
	close(null);
	close(fd);
}

static void test_process(void)
{
	char directory[300], sock[400], status[400], lock[400], message[128];
	snprintf(directory, sizeof directory, "%s/private", base);
	snprintf(sock, sizeof sock, "%s/telemetry.sock", directory);
	snprintf(status, sizeof status, "%s/status.json", directory);
	snprintf(lock, sizeof lock, "%s/collector.lock", directory);

	int err;
	pid_t daemon_pid = start_daemon(directory, &err);
	CHECK(daemon_pid > 0);
	CHECK(wait_until(exists, sock));
	struct stat st;
	CHECK(stat(directory, &st) == 0 && (st.st_mode & 0777) == 0700);
	CHECK(stat(sock, &st) == 0 && (st.st_mode & 0777) == 0600);
	CHECK(stat(status, &st) == 0 && (st.st_mode & 0777) == 0600);
	CHECK(client_count(status) == 0);

	const char *valid[] = {"5|73|2|nano", "6|255|0||65535|0",
			       "7|73|1|nano|38|1"};
	const char *invalid[] = {"8|101|2|bad",	       "8|73|5|bad",
				 "8|73|2|bad\nhost",   "8|73|2|bad|1001|0",
				 "8|73|2|bad|65535|1", "8|73|2|bad|65|0|extra"};
	for (size_t i = 0; i < 3; i++) {
		snprintf(message, sizeof message, "%d:%s", getpid(), valid[i]);
		send_to(sock, message);
	}
	for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
		snprintf(message, sizeof message, "%d:%s", getpid(),
			 invalid[i]);
		send_to(sock, message);
	}
	send_to(sock, "1:8|73|2|spoofed");
	send_with_descriptor(sock);
	expected_count = 3;
	CHECK(wait_until(count_reached, status));
	poll(NULL, 0, 150);
	CHECK(client_count(status) == 3);
	CHECK(stat(status, &st) == 0 && (st.st_mode & 0777) == 0600);

	char *data = read_file(status);
	J *root = json_tokener_parse(data);
	free(data);
	J *clients = member(root, "clients");
	CHECK(json_object_get_int(member(root, "version")) == 1);
	for (size_t i = 0; clients && i < json_object_array_length(clients);
	     i++) {
		J *r = json_object_array_get_idx(clients, i);
		char id5[64], id6[64], id7[64];
		snprintf(id5, sizeof id5, "%d:5", getpid());
		snprintf(id6, sizeof id6, "%d:6", getpid());
		snprintf(id7, sizeof id7, "%d:7", getpid());
		const char *id = text_of(member(r, "id"));
		J *battery = member(r, "battery");
		J *audio = member(r, "audio");
		CHECK(json_object_get_double(member(r, "received_at")) > 1e9);
		if (!strcmp(id, id5)) {
			CHECK_STR(text_of(member(r, "hostname")), "nano");
			CHECK(json_object_get_int(
				  member(battery, "capacity")) == 73);
			CHECK_STR(text_of(member(battery, "status")),
				  "Discharging");
			CHECK(audio == NULL);
		} else if (!strcmp(id, id6)) {
			CHECK_STR(text_of(member(r, "hostname")), "");
			CHECK(member(battery, "capacity") == NULL);
			CHECK_STR(text_of(member(battery, "status")),
				  "Unknown");
			CHECK(audio != NULL && member(audio, "volume") == NULL);
			CHECK(!json_object_get_boolean(member(audio, "muted")));
		} else if (!strcmp(id, id7)) {
			CHECK_STR(text_of(member(battery, "status")),
				  "Charging");
			CHECK(json_object_get_int(member(audio, "volume")) ==
			      38);
			CHECK(json_object_get_boolean(member(audio, "muted")));
		} else {
			CHECK(!"unexpected client id");
		}
	}
	json_object_put(root);

	/* The same identifier updates its record. */
	snprintf(message, sizeof message, "%d:5|10|3|nano", getpid());
	send_to(sock, message);
	poll(NULL, 0, 200);
	CHECK(client_count(status) == 3);
	char *updated = read_file(status);
	CHECK(updated != NULL && strstr(updated, "\"Full\"") != NULL);
	free(updated);

	/* A second collector for the same directory is refused and leaves the
	 * first one untouched. */
	int err2;
	pid_t second = start_daemon(directory, &err2);
	CHECK(wait_exit(second) == 1);
	free(drain(err2));
	CHECK(exists(sock));

	/* Stopping removes the socket and empties the status. */
	kill(daemon_pid, SIGTERM);
	CHECK(wait_exit(daemon_pid) == 0);
	char *errors = drain(err);
	CHECK_STR(errors, "");
	free(errors);
	CHECK(!exists(sock));
	CHECK(client_count(status) == 0);
	CHECK(exists(lock));

	/* A directory open to other users is refused. */
	CHECK(chmod(directory, 0755) == 0);
	pid_t open_dir = start_daemon(directory, &err);
	CHECK(wait_exit(open_dir) == 1);
	free(drain(err));
	CHECK(!exists(sock));
}

static void test_many_clients(void)
{
	char directory[300], sock[400], status[400], message[128];
	snprintf(directory, sizeof directory, "%s/many", base);
	snprintf(sock, sizeof sock, "%s/telemetry.sock", directory);
	snprintf(status, sizeof status, "%s/status.json", directory);
	int err;
	pid_t pid = start_daemon(directory, &err);
	CHECK(wait_until(exists, sock));
	for (int i = 1; i <= 260; i++) {
		snprintf(message, sizeof message, "%d:%d|50|2|n", getpid(), i);
		send_to(sock, message);
		if (i % 20 == 0 || i == 260) {
			expected_count = i < 256 ? (size_t)i : 256;
			CHECK(wait_until(count_reached, status));
		}
	}
	poll(NULL, 0, 200);
	CHECK(client_count(status) == 256);
	kill(pid, SIGTERM);
	CHECK(wait_exit(pid) == 0);
	free(drain(err));
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
	CHECK(mkdtemp(base) != NULL);
	test_decode();
	test_table();
	test_process();
	test_many_clients();
	CHECK(nftw(base, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
	return vncb_finish("test_daemon");
}
