# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Chris <goabonga@pm.me>

SOURCES := $(wildcard src/*.c src/*.h)

.PHONY: format format-check lint

format:
	@$(if $(SOURCES),clang-format -i $(SOURCES),echo "no C sources")

format-check:
	@$(if $(SOURCES),clang-format --dry-run -Werror $(SOURCES),echo "no C sources")

lint:
	@$(if $(filter %.c,$(SOURCES)),clang-tidy --quiet $(filter %.c,$(SOURCES)) -- -std=c11 -Isrc,echo "no C sources")
