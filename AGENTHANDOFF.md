# Agent handoff: dynarec, ROM loading, performance

Session date: 2026-09-13. Branch `claude/dynarec-rom-loading-cfa987`, based on
`b9cb1e1`. Committed in four commits; `git log b9cb1e1..` lists
them. No pull request is open yet.

## Where things stand

| Check | Result |
|---|---|
| Host C tests (`scripts/host-tests-msvc.bat`, MSVC 2022) | 13 of 13 pass. The SH-4 helper suite runs 159,892 checks. `save_io_test.py --cc cl` also passes; it compiles extracted save/config functions, not ROM paging code. |
| New tests fail when their fix is reverted | Confirmed for the SWI LR fix (2 failures), STM SMC dispatch (6) and the store tag check (3) |
| Dreamcast cross-build (CI container) | Clean. Only pre-existing unused-variable warnings. |
| Super Puzzle Fighter II in Flycast 2.7, stock 16 MB | Runs with every change applied |
| Grand Theft Auto Advance | **Still crashes**: [GTA_BAD_JUMP_LOG_2026-09-13.md](GTA_BAD_JUMP_LOG_2026-09-13.md), ROADMAP B10 |
| Audio | **Reported bad by the user; not fixed.** Findings below. |

### Frame rate, Super Puzzle Fighter II attract mode

Flycast's counter, sampled every 7 s over 91 s. The mean excludes the t=7 s
intro sample. These are single runs, and Flycast's SH-4 timing is approximate.

| Build | Mean fps | Worst sample |
|---|---|---|
| This session's correctness fixes, before F1 | 25.8 | 17.6 |
| + F1, flush only newly emitted code | **29.3** | 26.2 |
| + F4, constant shifts | 27.9 | 22.0 |

Earlier reference: 23.2 fps on 2026-09-10, from a different sampling window.

F1's gain is concentrated where translation happens: the t=14/21/28 s samples
went from 24.9/22.5/17.6 to 36.0/35.0/28.0. The steady state stayed near
28.5 fps, so emitted-code quality is now the limit.

**The F4 row is not comparable with the F1 row.** Sampling is by wall-clock
time, so a faster run reaches the heavier parts of the attract sequence
sooner. At 91 s the F4 run was mid-match, while the F1 run was still on the VS
screen. F4's early samples were also higher (t=14/21/28 s: 36.0/37.0/34.7).
Judging F4 needs a fixed emulated-frame benchmark (Phase 0), not wall-clock
samples.

An earlier F4 run was discarded. Flycast's fast-forward (Tab; the counter
shows `>>`) was on, and the counter read 34 to 250. Both F4 runs rendered the
game correctly.

## What changed, and why

### 1. A SWI taken in Supervisor mode now sets the live LR

Files: `dc/sh4_helpers.c` (`execute_swi`); `cpu.c` (ARM and Thumb SWI paths).

**Why.** Every SWI path wrote the *banked* LR and then called
`set_cpu_mode(MODE_SUPERVISOR)`. `cpu_switch_mode` returns early on a same-mode
switch, so a SWI taken in SVC mode never set r14. The BIOS then read the SWI
number through a stale return address.

**How.** Set `reg[REG_LR]` after the mode switch.

**Test.** `swi_entry()` in `tests/sh4_helpers_behavior_test.c` covers every
mode in ARM and Thumb state.

### 2. Block transfers detect writes over translated code

Files: `dc/sh4_helpers.c`, `dc/sh4_stub.c`, `dc/sh4_instr.inc`, `cpu.h`,
`cpu_threaded.c`.

**Why.** ARM `STM` goes through `execute_arm_block_memory`, whose stores used
`execute_aligned_store32`, and that never checked translation tags. Copying
code over translated RAM therefore left stale translations running. Upstream
gpSP checks at least the final store; this port checked none.

**How.**

- `execute_aligned_store32` sets `sh4_block_store_smc_pending` when the tag
  before the written word is non-zero, or when the slow path returns
  `CPU_ALERT_SMC`.
- On Dreamcast, `execute_arm_block_memory` takes a third argument: the live
  cycle counter, which the emitter passes in r6. After an STM with a pending
  alert it calls `sh4_block_store_smc(insn_pc + 4, cycles)`, which flushes the
  RAM cache and resumes through the dispatcher.
- Alerts left by Thumb block transfers are consumed later:
  - by the final `execute_store_u32` of that transfer (fast path only);
  - otherwise by the next `sh4_update_gba` call.
- `flush_translation_cache_ram` clears the flag.
- `cpu.h` declares the three-argument prototype only for Dreamcast. The x86
  backend keeps two arguments.

**Tests.** `block_store_smc()` covers a hit on the first, middle, last or no
word, and an LDM that leaves an alert pending. The block-transfer test buffers
gained a 32 KB tag area. The contract count of `sh4_lookup_pc(cycles);` went
from 13 to 15.

**Caveat.** A Thumb `PUSH {..., lr}` or `POP {..., pc}` over translated code is
handled late, at the next `update_gba`.

### 3. ROM hash retry after a ROM cache flush

File: `cpu_threaded.c`, the ROM case of `block_lookup_address_builder`.

**Why.** When a translation failed because the ROM cache flushed, the retry
wrote its hash entry through a chain link inside the discarded cache. The
retried block could not be found, and the write landed in cache memory that
was about to be reused.

**How.** When the ROM write pointer is at or below the failed entry (the cache
was flushed), rehang the entry from the now-empty bucket.

**No test.** `cpu_threaded.c` does not build on the host.

### 4. ROM demand paging: empty buffer slots

File: `memory.c`: `init_memory_gamepak`, `unmap_gamepak_physical_page`.

**Why.** Empty slots started with `physical_index = 0`. Claiming one unmapped
page 0 while another slot still held it, so early page loads repeatedly
re-read page 0. Loaded pages also started at timestamp 0, tying with empty
slots, so the next load could evict the page just loaded.

On hardware every page load is a GD-ROM seek. Flycast does not show this
cost.

**How.**

- Empty slots use `0xFFFFFFFF` as their physical index.
- `unmap_gamepak_physical_page` ignores out-of-range indices.
- `page_time` restarts at 1.

**No behavioral test.** The phase3 contract strings still pass.

### 5. Diagnosable dynarec fatal errors

Files: `cpu_threaded.c`, `dc/sh4_stub.c`, `main.c`.

**Why.** The old `bad jump X (Y) (Z)` gave a middle value that was only the
last PC a helper stored, not the jump source. Serial output is not usable:
Flycast shows it in a separate window, and hardware needs a cable.

**How.**

- A 64-entry log of translated blocks is recorded once per translation. It
  holds the guest range and host range.
- `sh4_dispatch_return` records `__builtin_return_address(0)` in the
  indirect-branch helpers, and NULL for dispatches from C.
- `dynarec_bad_jump_report` puts about 22 lines on screen:
  - source block;
  - registers;
  - banked SVC SP and LR;
  - SVC stack;
  - words at `LR_svc - 4`;
  - newest translation;
  - guest-code tail;
  - emitted SH-4 before the call;
  - recent translations.
- `gpsp_dynarec_fatal_error` now splits its detail on newlines.

### 6. Performance F1: flush only newly emitted code

File: `cpu_threaded.c`.

**Why.** After every top-level translation, the dynarec flushed the operand
and instruction caches over *each region's whole used range*. KOS walks that
range 32 bytes at a time, with an `ocbwb` and a cache address-array write per
line (see `kernel/arch/dreamcast/kernel/cache.s` in the container). The cost
grew with cache fill.

**How.**

- `translation_flush_begin()` records all three write pointers before a
  top-level translation, before any `redo`.
- `translation_flush_new_code()` flushes `[start, ptr)` in every region.
- The three flush functions set `TRANSLATION_FLUSHED_*`, so a region flushed
  mid-translation is flushed from its base.
- Block linking patches only code inside the new ranges.

**Test.** Contract test only, in `tests/sh4_integration_contract_test.c`.

### 7. Performance F4: constant shifts

Files: `dc/sh4_emit.h`, `tests/sh4_emit_simulator.h`,
`tests/sh4_emit_encoding_test.c`.

**Why.** Constant shifts emitted one instruction per bit, up to 31.

**How.**

- New primitives: `rotl`, `shll2`, `shlr2`, `shlr8`, `shll16`, `shlr16`,
  `shad`, `shld`.
- LSL and LSR use the fixed shifts when that takes at most two instructions.
  Otherwise they emit `mov #n,r1` then `shld`.
- ASR emits `shar` for 1 or 2 bits, otherwise `mov #-n,r1` then `shad`.
- ROR rotates bit by bit when within 3 bits of either end, otherwise uses a
  six-instruction shift pair.
- r1 and r2 are scratch, as elsewhere in the emitter.

**Tests.**

- `test_executed_constant_shifts()` executes LSL, LSR, ASR and ROR for every
  amount 0 to 31 over six edge values. It checks scratch-register discipline
  and length bounds of 2, 2, 2 and 6.
- The encoding table now includes the new opcodes.

**Risk.** The simulator's `shad`/`shld` semantics come from the SH-4
instruction description, not from hardware. Gameplay looked correct in
Flycast.

### 8. Smaller changes

- **`main.c`:** the Dreamcast `synchronize()` no longer runs a float `sprintf`
  for `SDL_WM_SetCaption` every frame. That call does nothing on the
  Dreamcast.
- **`scripts/dc-disc.sh`:** packages a CDI inside the CI container.
- **`scripts/flycast-run.ps1`:** unattended Flycast run producing screenshots
  and an fps strip.
- **`scripts/host-tests-msvc.bat`:** host tests without gcc or make.
- **`GTA_BAD_JUMP_LOG_2026-09-13.md` and ROADMAP row B10.**

## What needs work next, in priority order

### P1. Audio (user-reported)

Nothing here is fixed. All four findings come from code reading.

1. **Output corruption at every ring-buffer wrap.** In `sound_callback`
   (`sound.c`), the wrap branch calls `sound_copy` twice. Both calls write
   from `stream_base[0]`. The second half overwrites the start of the output,
   and the end keeps the previous buffer's samples. `sound_copy_null` has the
   same bug.
   - *When:* every time `sound_buffer_base` wraps `BUFFER_SIZE` (32,768
     values), roughly every 0.74 s at 22,050 Hz stereo. Expect a periodic
     click.
   - *How:* advance the destination by the first half's sample count before
     the second copy, in both branches.
2. **The callback blocks.** It waits on `sound_cv` until the emulator has
   produced a buffer. Its check compares the buffered count against `length`
   in bytes, so it waits for twice what it needs.
   - *Effect:* KOS SDL (`kos-ports/SDL/files/SDL_dcaudio.c`) plays a two-half
     ring in AICA RAM, and the AICA loops the old half while the callback
     waits. Below full speed, which is always today, that is a repeating
     buzz.
   - *How:* on Dreamcast, never wait. Copy what is buffered. Pad the rest
     with a short fade from the last sample. Never move `sound_buffer_base`
     past `gbc_sound_buffer_index`. Keep the producer-side throttle in
     `update_gbc_sound`, which waits above 1.5x `audio_buffer_size`.
3. **Upload cost.** `SDL_dcaudio` uploads one stereo sample at a time
   (`spu_memload_stereo16`). Each write does a G2 FIFO wait and disables and
   restores interrupts. It busy-waits on the AICA position with `thd_pass`.
   - *How:* consider KOS `snd_stream` (`dc/sound/stream.h`), whose pull
     callback returns a buffer and uploads in blocks, polled with
     `snd_stream_poll`.
4. **Minor.** `sound_timer` locks `sound_mutex` on every direct-sound timer
   tick.

Verify by ear in Flycast. Frameskip (F12, below) changes how audio behaves
when the emulator is slow.

### P2. Performance

The plan is `DC_PERFORMANCE_PLAN_2026-09-10.md`.

- **Judge F4 properly.** Wall-clock samples could not separate its speed from
  attract-mode position (see the table note). Measure a fixed number of
  emulated frames from a fixed starting point, which is Phase 0's benchmark
  mode.
- **F5.** `reg[16]` to `reg[20]` (the flags and CPSR) sit outside the 4-bit
  load/store displacement, so each access takes three instructions.
  - *How:* pin a second base register at `&reg[16]` (r8 to r11 are
    callee-saved and unused) and set it in `sh4_dispatch_block`. It is cheap.
- **F12.** Automatic frameskip is the default, but on the non-PSP path
  `synchronize()` only honors manual frameskip. Wiring it helps both
  smoothness and audio. It also changes what Flycast's counter measures, so
  land it with an on-target counter.
- **Phase 0.** Add an on-target frame and time overlay. Flycast's counter is
  the only instrument today: it cannot run on hardware and fast-forward breaks
  it.
- **Larger items:**
  - F3: literal pool, the biggest code-size lever. Every helper call currently
    spends up to 14 instructions building an address.
  - F2: inline ALU operations.
  - F8: inline memory-map probe.
  - F11: measure -O3 and LTO.

### P3. Grand Theft Auto Advance bad jump (B10)

Control reaches `SWI 0xEF0000`, which is data on the IWRAM stack at
0x03007dd8, so the BIOS dispatcher indexes past its table. The jump into the
stack is still unidentified. Next steps, detailed in the log:

1. Record a ring of (host return address, target) for each indirect dispatch.
2. Dump the word at 0x03000270 and the user stack.
3. Run paging A/B tests: a whole-ROM resident buffer, and prefetch disabled.
4. Compare against mGBA.

Two hypotheses are ruled out: a stale LR_svc, and stale translations from STM.

### P4. Test and robustness gaps

- The ROM hash retry fix (§3) and the paging slot fix (§4) have no behavioral
  tests. F1 is covered only by a contract test.
- The ROM lookup's retry has no attempt limit; the RAM path stops after 32.
  A block that fails without flushing, such as one with more than
  `MAX_EXITS` external exits, would loop forever.
- `execute_store_u32` honors a pending block-store alert only on its fast
  path.

### P5. Review and merge

The work is committed in four commits:

1. ROM paging empty-slot fix (`memory.c`).
2. Dynarec correctness, the fatal-error report and F1. They share
   `cpu_threaded.c` and `dc/sh4_stub.c`, so they are one commit.
3. F4 constant shifts, with the simulator and encoding tests.
4. Documentation and scripts.

The host tests have not been run with gcc, which is what CI uses
(`make -C tests test`, including `save_io_test.py`); MSVC passed all 13 host C suites and the save/config fault-injection test. Check
CI on the pushed branch before opening a pull request against `dreamcast`.

## How to build, run and measure

Paths are from this machine; adjust as needed. The BIOS and commercial ROMs
live in the main checkout's `dc/cd/` and are gitignored.

```bash
# Cross-compile dc/gdC.elf. Docker Desktop must be running.
MSYS_NO_PATHCONV=1 docker run --rm -v "<repo>:/src" -w /src/dc \
  einsteinx2/dcdev-kos-toolchain:gcc-9__v2.0.0 make all
```

```bash
# Package a disc that autoloads one ROM. /assets needs gba_bios.bin and gbaDC/<rom>.
MSYS_NO_PATHCONV=1 docker run --rm -v "<repo>:/src:ro" \
  -v "F:/GitHub/gPSPDC/dc/cd:/assets:ro" -v "<out dir>:/out" \
  einsteinx2/dcdev-kos-toolchain:gcc-9__v2.0.0 \
  sh /src/scripts/dc-disc.sh "Super Puzzle Fighter II (USA) (Rev 1).gba" spf.cdi
```

```powershell
# Unattended fps measurement. Needs rend.ShowFPS = yes in Flycast's emu.cfg.
powershell -File scripts\flycast-run.ps1 -Cdi <out dir>\spf.cdi -Tag spf `
  -Seconds 91 -FpsEvery 7 -FlycastExe C:\Users\allen\Downloads\flycast-win64-2.7\flycast.exe
```

```bat
scripts\host-tests-msvc.bat
```

Lessons from this session:

- Do not touch Flycast during a measurement. Input changes the attract
  sequence, and Tab toggles fast-forward (`>>`).
- A failed PowerShell run can leave Flycast open. Close it before the next
  run, because two emulators skew timing.
- Flycast writes `flycast.log` into its working directory.
  `flycast-run.ps1` uses `-OutDir` so it stays out of the repo.
- KOS `printf` goes to Flycast's separate serial window and cannot be
  captured. Put diagnostics on the fatal screen.
- `cpu_threaded.c` mixes LF and CRLF lines. Line-based patching that ignores
  padding works; exact multi-line regexes did not.
