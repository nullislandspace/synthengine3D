# SynthEngine3D host harness (see ../docs/testing.md).
#
#   make check     build and run the harness self-test
#   make clean
#
# A game's own checker uses the same three include paths and links
# se_host_stub.c the same way; shims/ MUST come before ../include so the
# host umbrella header wins over the badge one.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
INCS    := -Ishims -I. -I../include
BUILD   ?= build

.PHONY: check clean
check: $(BUILD)/se_host_selftest
	$(BUILD)/se_host_selftest

$(BUILD)/se_host_selftest: se_host_selftest.c se_host_stub.c se_host.h shims/*.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INCS) se_host_selftest.c se_host_stub.c -lm -o $@

clean:
	rm -rf $(BUILD)
