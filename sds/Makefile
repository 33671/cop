CC      := gcc
CFLAGS  := -Wall -Wextra -std=c99

.PHONY: all test clean

all: test

# ── Run tests ──────────────────────────────────────────
test: build/arena-sds-test
	@echo "=== SDS tests ==="
	@./build/arena-sds-test
	@echo ""

build/arena-sds-test: test_arena_sds.c sds.c sds.h arena.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_arena_sds.c sds.c

# ── Clean ──────────────────────────────────────────────
clean:
	rm -rf build
