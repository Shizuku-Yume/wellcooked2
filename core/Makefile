# KitchenOS core -- GNU make, no cmake (the course machines have gcc/make only).
#
# Everything is discovered with wildcard, so adding src/*.c, tests/test_*.c or a
# new tests/test_foo.c suite never requires editing this file. src/bridge.c owns
# main(), so it is filtered out of the objects that test binaries link against.

CC ?= gcc

BUILD    := build
OBJ_DIR  := $(BUILD)/obj

CFLAGS   := -std=c99 -Wall -Wextra -Werror -O2 -g -Iinclude -pthread -D_POSIX_C_SOURCE=200809L
LDFLAGS  := -pthread

SRCS     := $(wildcard src/*.c)
TESTS    := $(wildcard tests/test_*.c)

# The bridge executable's main() must never be pulled into a test binary.
LINK_SRCS := $(filter-out src/bridge.c,$(SRCS))

OBJS      := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
LINK_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(LINK_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/%,$(TESTS))
DEPS      := $(OBJS:.o=.d)

# `all` has to keep working before src/bridge.c exists (tasks land in any order).
ifeq ($(wildcard src/bridge.c),)
BRIDGE_BIN :=
else
BRIDGE_BIN := $(BUILD)/kitchenos-bridge
endif

# tools/ holds test-only binaries that are not part of the daemon: worker_stub
# stands in for a Chef Worker so the whole frame barrier can be driven without
# the game or a real Scheduler. Conditional for the same reason as bridge.c.
ifeq ($(wildcard tools/worker_stub.c),)
TOOL_BINS :=
else
TOOL_BINS := $(BUILD)/kitchenos-worker-stub
endif

.PHONY: all test clean

# $(OBJS) is a prerequisite so that `make` really means "every module compiles".
# Without it, a tree with sources but no test suite or bridge yet would build
# nothing, print "built:", and exit 0 — a green signal over broken code.
all: $(OBJS) $(TEST_BINS) $(BRIDGE_BIN) $(TOOL_BINS)
	@echo "built: $(notdir $(TEST_BINS) $(BRIDGE_BIN) $(TOOL_BINS))"

$(BUILD)/kitchenos-worker-stub: tools/worker_stub.c $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LINK_OBJS) $(LDFLAGS)

$(BUILD)/kitchenos-bridge: $(OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# One binary per tests/test_*.c suite, linked against every module object
# except src/bridge.c.
$(BUILD)/%: tests/%.c $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LINK_OBJS) $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD) $(OBJ_DIR):
	@mkdir -p $@

# Runs every suite even after one fails, so a single run shows the whole picture.
test: $(TEST_BINS)
	@failed=0; total=0; \
	for t in $(TEST_BINS); do \
	  total=$$((total + 1)); \
	  if ./$$t; then echo "[PASS] $$t"; else echo "[FAIL] $$t"; failed=$$((failed + 1)); fi; \
	done; \
	echo "=== $$total test suites, $$failed failed ==="; \
	test $$failed -eq 0

clean:
	rm -rf $(BUILD)

-include $(DEPS)
