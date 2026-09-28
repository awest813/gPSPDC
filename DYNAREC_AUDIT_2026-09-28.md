# Dynarec audit 5: executing translated code — September 28, 2026

Starting checkout: `b9cb1e1`. Earlier audits tested C helpers on the host,
inspected encodings, and simulated a few emitted sequences, but never ran
the translator's actual output. This pass adds a harness that does. It then
uses the harness to fix three defects: one visible only on real hardware,
one at startup, and one in the ROM block cache.

## New harness: `tests/sh4_exec`

`make -C tests sh4-exec` cross-compiles the production `cpu_threaded.c`,
`dc/sh4_helpers.c` and `dc/sh4_stub.c`, plus the `cpu.c` interpreter, for
SH-4 Linux. It runs them under `qemu-sh4` (`apt-get install
gcc-sh4-linux-gnu qemu-user`). A small memory system mirrors `memory.c`'s
layout, including the translation tags below each RAM page. The tests end
at a `b .` sentinel.

- **Cache-coherence model.** qemu keeps self-modifying code coherent, so a
  missing SH-4 cache flush would never produce a wrong result there.
  Flycast has the same blind spot. The harness implements KOS's
  `icache_flush_range` as a shadow of what the instruction cache may fetch.
  At every dispatch into translated code, it checks that each emitted byte
  was written back and invalidated.
- **Directed tests.** Covers ARM and Thumb from ROM, Thumb entry, SWI into
  the BIOS from ROM and IWRAM, IWRAM self-modifying code, and a store that
  raises an IRQ.
- **Differential fuzz.** Seeded random ARM and Thumb sequences run on both
  engines, and the results are compared. ARM coverage includes data
  processing with PC operands, multiplies including long forms, MRS/MSR,
  and conditional branches and BL. Thumb coverage includes formats 1–5 and
  conditional branches. The comparison covers registers, flags and RAM.
  The default run is 4,020 checks in about 6 s and runs in the host-tests
  CI workflow.

The Dreamcast stub gained one test-only hook, `GPSP_SH4_TEST_DISPATCH_HOOK`.
It is compiled out unless the harness defines it.

## Fixed

| Fix | Before | After |
|---|---|---|
| Recursive translations not flushed | Linking a SWI exit translates the BIOS vector recursively into the BIOS cache. Only the top-level cache was flushed, so on hardware the new BIOS code could run from stale memory. The harness reproduced it as `bios cache offset 0 executed before write-back/invalidate`. | `sh4_flush_new_translations()` flushes exactly the new code in every cache after each top-level translation. Cache resets rewind the marks. The whole used cache is no longer re-flushed after every block, so performance finding F1 is also addressed. |
| Thumb-mode dynarec entry | `execute_arm_translate` always did an ARM lookup. A Thumb-mode state reaching the first entry was translated as ARM and crashed the harness. | Selects the lookup from CPSR.T. |
| ROM hash retry after a flush | If a failed attempt flushed the ROM cache, the retry linked through a chain entry inside the discarded cache. The retried block was never entered in the hash. | The retry relinks from the bucket when the flush emptied it. |
| BIOS cache reset | ROM and RAM blocks jump directly into BIOS translations for SWI. Resetting only the BIOS cache, which is 64 KB on Dreamcast, would leave those jumps pointing into reused space. | A BIOS flush also drops the ROM and RAM caches. |

The first two fixes each have a harness test that failed before the fix
and passes after it. The ROM hash and BIOS reset fixes need a cache
overflow during a nested translation to trigger; they are covered by code
review only.

## Interpreter findings (not fixed here)

The full fuzz mode, `qemu-sh4 ./sh4_exec_test N full`, also exercises
ADC/SBC/RSC and register-specified shifts. In a 300-case run per
instruction set, 24% of ARM cases and 13% of Thumb cases disagreed. Two decoded cases were both interpreter defects that audit 4
already fixed in the dynarec helpers:

- Thumb `ADC` computes carry from `Rs + C`, which overflows when
  `Rs = 0xFFFFFFFF`.
- Thumb `LSL Rd, Rs` uses all 32 bits of `Rs` instead of the low byte.

With those two instruction classes excluded, 4,000 of 4,000 cases agree.
That mode is the gating one. Not every full-mode disagreement was decoded
individually. The interpreter is only the shipping engine in
`GPSP_DC_INTERPRETER` and host builds, but it should be brought to parity.
That would let the gating fuzz cover those classes.

## Remaining

- `SWP`: if the store raises an alert (SMC, IRQ, halt), `Rd` is never
  written. The interpreter has the same behaviour. It needs `Rd` written
  before the store.
- KOS `icache_flush_range` already writes back the operand cache, so the
  preceding `dcache_flush_range` looks redundant. Confirm this against the
  KOS 2.0 source before removing it.
- Extend the fuzz to loads, stores, LDM/STM, SWI and IRQ timing, and run
  the new flush on hardware or Flycast. Docker was unavailable here, so the
  KOS build is left to the Dreamcast CI workflow.
- Emitter performance work (DC performance plan phases 1–3) was not
  started in this pass.
