# Runs the production SH-4 dynarec under qemu-sh4 (see harness.c).
# Needs an SH-4 Linux cross compiler and qemu user mode, e.g. on Ubuntu:
#   apt-get install gcc-sh4-linux-gnu qemu-user

SH4_CC ?= sh4-linux-gnu-gcc
QEMU_SH4 ?= qemu-sh4
FUZZ_CASES ?= 2000

ROOT = ../..
CFLAGS = -O2 -std=gnu99 -D_arch_dreamcast=1 \
  -DGPSP_SH4_TEST_DISPATCH_HOOK=harness_check_coherence \
  -Ishims -I$(ROOT) -I$(ROOT)/dc -w
LDFLAGS = -static -no-pie

SOURCES = sh4_exec_test.c harness.c $(ROOT)/cpu_threaded.c $(ROOT)/cpu.c \
  $(ROOT)/dc/sh4_helpers.c $(ROOT)/dc/sh4_stub.c
DEPS = $(SOURCES) harness.h shims/SDL.h shims/kos.h $(ROOT)/dc/sh4_emit.h \
  $(ROOT)/dc/sh4_instr.inc $(ROOT)/cpu.h $(ROOT)/cpu_mode.h

.PHONY: test clean

test: sh4_exec_test
	$(QEMU_SH4) ./sh4_exec_test $(FUZZ_CASES)

sh4_exec_test: $(DEPS)
	$(SH4_CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)

clean:
	rm -f sh4_exec_test
