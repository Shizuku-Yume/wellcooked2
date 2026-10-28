# WellCooked core -- GNU make, no cmake (the course machines have gcc/make only).
#
# Everything is discovered with wildcard, so adding src/*.c, tests/test_*.c or a
# new tests/test_foo.c suite never requires editing this file. Source files that
# own main() are filtered out of the objects that test binaries link against; the
# list is explicit because a wrong filter fails at link time with duplicate main().

CC ?= gcc

BUILD    := build
OBJ_DIR  := $(BUILD)/obj

CFLAGS   := -std=c99 -Wall -Wextra -Werror -O2 -g -Iinclude -pthread -D_POSIX_C_SOURCE=200809L
# -lrt carries mq_unlink on glibc older than 2.34; on newer ones it is an empty stub.
# -lm carries atan2f in macro.c (面向判定), not inlined under -std=c99.
LDFLAGS  := -pthread -lrt -lm

SRCS     := $(wildcard src/*.c)
TESTS    := $(wildcard tests/test_*.c)

# Every executable's main() must never be pulled into a test binary. The list is
# explicit: a wildcard rule would silently decide that a new daemon belongs in
# the test link, and two main()s then fail at link time with a confusing error.
MAIN_SRCS := src/bridge.c src/supervisor.c src/scheduler.c src/worker.c src/navd.c src/monitor_main.c
LINK_SRCS := $(filter-out $(MAIN_SRCS),$(SRCS))

OBJS      := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
LINK_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(LINK_SRCS))
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/%,$(TESTS))
DEPS      := $(OBJS:.o=.d)

# `all` has to keep working before src/bridge.c exists (tasks land in any order).
ifeq ($(wildcard src/bridge.c),)
BRIDGE_BIN :=
else
BRIDGE_BIN := $(BUILD)/wellcooked-bridge
endif

ifeq ($(wildcard src/supervisor.c),)
SUPERVISOR_BIN :=
else
SUPERVISOR_BIN := $(BUILD)/wellcooked-supervisor
endif

ifeq ($(wildcard src/scheduler.c),)
SCHEDULER_BIN :=
else
SCHEDULER_BIN := $(BUILD)/wellcooked-scheduler
endif

ifeq ($(wildcard src/worker.c),)
WORKER_BIN :=
else
WORKER_BIN := $(BUILD)/wellcooked-worker
endif

ifeq ($(wildcard src/navd.c),)
NAVD_BIN :=
else
NAVD_BIN := $(BUILD)/wellcooked-navd
endif

ifeq ($(wildcard src/monitor_main.c),)
MONITOR_BIN :=
else
MONITOR_BIN := $(BUILD)/wellcooked-monitor
endif

# tools/ holds test-only binaries that are not part of the daemon: worker_stub
# stands in for a Chef Worker so the whole frame barrier can be driven without
# the game or a real Scheduler. Conditional for the same reason as bridge.c.
TOOL_BINS :=
ifneq ($(wildcard tools/worker_stub.c),)
TOOL_BINS += $(BUILD)/wellcooked-worker-stub
endif
ifneq ($(wildcard tools/silent_child.c),)
TOOL_BINS += $(BUILD)/wellcooked-silent-child
endif

.PHONY: all test test-san clean

# $(OBJS) is a prerequisite so that `make` really means "every module compiles".
# Without it, a tree with sources but no test suite or bridge yet would build
# nothing, print "built:", and exit 0 — a green signal over broken code.
all: $(OBJS) $(TEST_BINS) $(BRIDGE_BIN) $(SUPERVISOR_BIN) $(SCHEDULER_BIN) $(WORKER_BIN) $(NAVD_BIN) $(MONITOR_BIN) $(TOOL_BINS)
	@echo "built: $(notdir $(TEST_BINS) $(BRIDGE_BIN) $(SUPERVISOR_BIN) $(SCHEDULER_BIN) $(WORKER_BIN) $(NAVD_BIN) $(MONITOR_BIN) $(TOOL_BINS))"

$(BUILD)/wellcooked-worker-stub: tools/worker_stub.c $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LINK_OBJS) $(LDFLAGS)

$(BUILD)/wellcooked-silent-child: tools/silent_child.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Each daemon links its own main object plus every non-main module. Linking all
# of $(OBJS) would drag the other three main()s in and fail.
$(BUILD)/wellcooked-bridge: $(OBJ_DIR)/bridge.o $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/wellcooked-supervisor: $(OBJ_DIR)/supervisor.o $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/wellcooked-scheduler: $(OBJ_DIR)/scheduler.o $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/wellcooked-worker: $(OBJ_DIR)/worker.o $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/wellcooked-navd: $(OBJ_DIR)/navd.o $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/wellcooked-monitor: $(OBJ_DIR)/monitor_main.o $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# One binary per tests/test_*.c suite, linked against every non-main module object.
$(BUILD)/%: tests/%.c $(LINK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LINK_OBJS) $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD) $(OBJ_DIR):
	@mkdir -p $@

# Runs every suite even after one fails, so a single run shows the whole picture.
# Integration suites exec the daemons, so `make test` must build them too. Without
# this dependency `make test` after a clean tree (or `make test-san`) linked the
# suites but left them unable to spawn anything.
test: $(TEST_BINS) $(BRIDGE_BIN) $(SUPERVISOR_BIN) $(SCHEDULER_BIN) $(WORKER_BIN) $(NAVD_BIN) $(MONITOR_BIN) $(TOOL_BINS)
	@failed=0; total=0; \
	for t in $(TEST_BINS); do \
	  total=$$((total + 1)); \
	  if ./$$t; then echo "[PASS] $$t"; else echo "[FAIL] $$t"; failed=$$((failed + 1)); fi; \
	done; \
	echo "=== $$total test suites, $$failed failed ==="; \
	test $$failed -eq 0

# The parsers take bytes off the network, so the suites have to be runnable under the
# sanitizers with one committed command. Doing it by hand with a CFLAGS override on the
# command line left no trace in the tree, which made the result unreproducible.
#
# Sanitizer objects get their own build directory instead of reusing build/. Sharing one
# directory left sanitized .o files behind after this target, and the next plain `make`
# then linked them without -fsanitize=address and died with "undefined reference to
# __asan_init" -- a failure caused by the previous command, not by the change at hand.
# BUILD is a command-line override, so it carries into OBJ_DIR and the test binaries too.
SAN_FLAGS := -std=c99 -Wall -Wextra -Werror -O1 -g -Iinclude -pthread \
             -D_POSIX_C_SOURCE=200809L -fsanitize=address,undefined -fno-omit-frame-pointer

test-san:
	$(MAKE) BUILD=$(BUILD)/san CFLAGS="$(SAN_FLAGS)" test

clean:
	rm -rf $(BUILD)

-include $(DEPS)
