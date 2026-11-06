/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Chris <goabonga@pm.me> */
#ifndef VNCB_FIDO_H
#define VNCB_FIDO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIDO_PACKET 64
#define FIDO_BUFFER 65536
#define FIDO_USAGE_PAGE 0xf1d0u

/* Only this descriptor is ever supplied to UHID. Remote descriptors cannot
 * create keyboard, pointer, feature or numbered reports on the host. */
static const unsigned char fido_descriptor[] = {
    0x06, 0xd0, 0xf1, 0x09, 0x01, 0xa1, 0x01, 0x09, 0x20, 0x15, 0x00, 0x26,
    0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02, 0x09, 0x21, 0x15, 0x00,
    0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02, 0xc0};

enum { HID_MAIN = 0, HID_GLOBAL = 1, HID_LOCAL = 2 };
enum { HID_INPUT = 8, HID_OUTPUT = 9, HID_COLLECTION = 10, HID_END = 12 };
enum { HID_USAGE_PAGE = 0, HID_REPORT_SIZE = 7, HID_REPORT_COUNT = 9 };

/* Accept exactly one unnumbered FIDO application collection holding a 64-byte
 * input report and a 64-byte output report. Fail closed on composite,
 * feature, long and unknown HID items. */
static inline bool fido_descriptor_valid(const unsigned char *p, size_t n)
{
	unsigned page = 0, usage = 0, bits = 0, count = 0;
	bool collection = false, ended = false, input = false, output = false;
	for (size_t i = 0; i < n;) {
		unsigned prefix = p[i++];
		if (prefix == 0xfe) /* Long items. */
			return false;
		unsigned size = prefix & 3u;
		if (size == 3)
			size = 4;
		if (size > n - i)
			return false;
		uint32_t value = 0;
		for (unsigned j = 0; j < size; j++)
			value |= (uint32_t)p[i++] << (8 * j);
		unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;
		if (ended)
			return false;

		if (type == HID_GLOBAL) {
			if (tag == HID_USAGE_PAGE) {
				page = value;
				if (page != FIDO_USAGE_PAGE)
					return false;
			} else if (tag == HID_REPORT_SIZE) {
				bits = value;
			} else if (tag == HID_REPORT_COUNT) {
				count = value;
			} else if (tag > 6) { /* No report IDs, push/pop or
						 unknown globals. */
				return false;
			}
		} else if (type == HID_LOCAL) {
			if (tag != 0) /* Only usage is accepted. */
				return false;
			if (size == 4) {
				if ((value >> 16) != FIDO_USAGE_PAGE)
					return false;
				usage = value & 0xffffu;
			} else {
				usage = value;
			}
		} else if (type == HID_MAIN) {
			if (tag == HID_COLLECTION) {
				if (collection || page != FIDO_USAGE_PAGE ||
				    usage != 1 || value != 1)
					return false;
				collection = true;
			} else if (tag == HID_END) {
				if (!collection || size || !input || !output)
					return false;
				ended = true;
			} else if (tag == HID_INPUT || tag == HID_OUTPUT) {
				bool is_input = tag == HID_INPUT;
				if (!collection || page != FIDO_USAGE_PAGE ||
				    bits != 8 || count != FIDO_PACKET ||
				    value != 2 ||
				    usage != (is_input ? 0x20u : 0x21u) ||
				    (is_input ? input : output))
					return false;
				if (is_input)
					input = true;
				else
					output = true;
			} else {
				return false;
			}
			usage = 0;
		} else {
			return false;
		}
	}
	return ended;
}
#endif
