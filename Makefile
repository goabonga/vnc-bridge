# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>

CC       ?= cc
CFLAGS   ?= -O2 -g
WARN     := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Iinclude
SANITIZE ?= address,undefined
BUILD    := build
SOURCES  := $(wildcard src/*.c src/*.h include/*.h tests/*.c tests/*.h)
TESTS    := $(patsubst tests/%.c,$(BUILD)/tests/%,$(wildcard tests/test_*.c))

.PHONY: test format format-check lint clean

# Unit tests, built with the address and undefined behaviour sanitizers.
# Disable them with `make test SANITIZE=` where the platform rejects them.
test: $(TESTS)
	@set -e; for t in $(TESTS); do $$t; done

$(BUILD)/tests/%: tests/%.c $(wildcard tests/*.h src/*.h src/*.c include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) \
		$(if $(SANITIZE),-fsanitize=$(SANITIZE) -fno-sanitize-recover=all) \
		$(LDFLAGS) $< $(LDLIBS) -o $@

format:
	clang-format -i $(SOURCES)

format-check:
	clang-format --dry-run -Werror $(SOURCES)

# clang-tidy checks the program sources; the test code deliberately breaks
# the rules it verifies (redirected stderr, forced failures).
lint:
	@$(if $(wildcard src/*.c),clang-tidy --quiet $(wildcard src/*.c) -- -std=c11 -D_GNU_SOURCE -Iinclude -Isrc,echo "no program sources")

clean:
	rm -rf $(BUILD)
