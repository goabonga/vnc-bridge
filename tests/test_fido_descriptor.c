/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#include "../src/fido.h"
#include "harness.h"
#include <string.h>

/* The canonical FIDO usage page 0xF1D0 descriptor: one application
 * collection, one 64-byte input report and one 64-byte output report. */
static const unsigned char canonical[] = {
    0x06, 0xd0, 0xf1, 0x09, 0x01, 0xa1, 0x01, 0x09, 0x20, 0x15, 0x00, 0x26,
    0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02, 0x09, 0x21, 0x15, 0x00,
    0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02, 0xc0};

/* Replace the first occurrence of `from` by `to` in the canonical descriptor
 * and report whether the result is accepted. */
static bool accepts_with(const unsigned char *from, size_t from_len,
			 const unsigned char *to, size_t to_len)
{
	unsigned char d[128];
	size_t at = 0;
	while (at + from_len <= sizeof canonical &&
	       memcmp(canonical + at, from, from_len) != 0)
		at++;
	if (at + from_len > sizeof canonical)
		return false;
	size_t n = 0;
	memcpy(d, canonical, at);
	n = at;
	memcpy(d + n, to, to_len);
	n += to_len;
	memcpy(d + n, canonical + at + from_len,
	       sizeof canonical - at - from_len);
	n += sizeof canonical - at - from_len;
	return fido_descriptor_valid(d, n);
}

#define REPLACE(from, to)                                                      \
	accepts_with((const unsigned char *)(from), sizeof(from) - 1,          \
		     (const unsigned char *)(to), sizeof(to) - 1)

int main(void)
{
	CHECK(fido_descriptor_valid(canonical, sizeof canonical));
	CHECK(sizeof fido_descriptor == sizeof canonical);
	CHECK(memcmp(fido_descriptor, canonical, sizeof canonical) == 0);

	/* The report count may use a longer encoding. */
	CHECK(REPLACE("\x95\x40", "\x96\x40\x00"));

	/* Truncated, extended and repeated descriptors. */
	CHECK(!fido_descriptor_valid(canonical, 0));
	CHECK(!fido_descriptor_valid(canonical, sizeof canonical - 1));
	unsigned char extended[sizeof canonical + 1];
	memcpy(extended, canonical, sizeof canonical);
	extended[sizeof canonical] = 0;
	CHECK(!fido_descriptor_valid(extended, sizeof extended));
	unsigned char doubled[2 * sizeof canonical];
	memcpy(doubled, canonical, sizeof canonical);
	memcpy(doubled + sizeof canonical, canonical, sizeof canonical);
	CHECK(!fido_descriptor_valid(doubled, sizeof doubled));

	/* Wrong usage page, report size, direction, report id, usage and bit
	 * width. */
	CHECK(!REPLACE("\x06\xd0\xf1", "\x06\x01\x00"));
	CHECK(!REPLACE("\x95\x40", "\x95\x3f"));
	CHECK(!REPLACE("\x81\x02", "\xb1\x02"));
	CHECK(!REPLACE("\xa1\x01", "\xa1\x01\x85\x01"));
	CHECK(!REPLACE("\x09\x21", "\x09\x06"));
	CHECK(!REPLACE("\x75\x08", "\x75\x10"));

	/* Long items are refused. */
	unsigned char with_long_item[sizeof canonical + 3] = {0xfe, 0xff, 0x01};
	memcpy(with_long_item + 3, canonical, sizeof canonical);
	CHECK(!fido_descriptor_valid(with_long_item, sizeof with_long_item));

	/* An item cut short by the end of the data. */
	const unsigned char cut[] = {0x06, 0xd0};
	CHECK(!fido_descriptor_valid(cut, sizeof cut));
	return vncb_finish("test_fido_descriptor");
}
