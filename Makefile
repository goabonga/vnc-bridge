# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>

CC       ?= cc
CFLAGS   ?= -O2 -g
WARN     := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Iinclude
SANITIZE ?= address,undefined
PREFIX   ?= /usr/local
LDLIBS   := -lm
BUILD    := build
PROGRAMS := vnc-bridge-wifi vnc-bridge-i3blocks
JSON_CFLAGS := $(shell pkg-config --cflags json-c)
JSON_LIBS   := $(shell pkg-config --libs json-c)
BINARIES := $(addprefix $(BUILD)/bin/,$(PROGRAMS))
SOURCES  := $(wildcard src/*.c src/*.h include/*.h tests/*.c tests/*.h)
TESTS    := $(patsubst tests/%.c,$(BUILD)/tests/%,$(wildcard tests/test_*.c))

.PHONY: all build test format format-check lint install clean

all: build

build: $(BINARIES)

$(BUILD)/bin/%: src/%.c $(wildcard src/*.h) include/base.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) $(JSON_CFLAGS) $(LDFLAGS) $< $(LDLIBS) $(JSON_LIBS) -o $@

# Unit tests, built with the address and undefined behaviour sanitizers.
# Disable them with `make test SANITIZE=` where the platform rejects them.
test: build $(TESTS)
	@set -e; for t in $(TESTS); do VNCB_BIN=$(BUILD)/bin $$t; done

$(BUILD)/tests/%: tests/%.c $(wildcard tests/*.h src/*.h src/*.c include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) \
		$(if $(SANITIZE),-fsanitize=$(SANITIZE) -fno-sanitize-recover=all) \
		$(JSON_CFLAGS) $(LDFLAGS) $< $(LDLIBS) $(JSON_LIBS) -o $@

format:
	clang-format -i $(SOURCES)

format-check:
	clang-format --dry-run -Werror $(SOURCES)

# clang-tidy checks the program sources; the test code deliberately breaks
# the rules it verifies (redirected stderr, forced failures).
lint:
	@$(if $(wildcard src/*.c),clang-tidy --quiet $(wildcard src/*.c) -- -std=c11 -D_GNU_SOURCE $(JSON_CFLAGS) -Iinclude -Isrc,echo "no program sources")

install: build
	for p in $(PROGRAMS); do \
		install -Dm755 $(BUILD)/bin/$$p $(DESTDIR)$(PREFIX)/bin/$$p; \
	done

clean:
	rm -rf $(BUILD)
