# Agent handoff: dynarec, ROM loading, performance

Session date: 2026-09-13. Branch `claude/dynarec-rom-loading-cfa987`, based on
`b9cb1e1`. The original work is committed in four commits through `cd2ed67`
and pushed to `origin/claude/dynarec-rom-loading-cfa987`. The audio follow-up
below is included in the subsequent audio-fix commit. Frame pacing and auto
frameskip (§9, 2026-09-14) follow in their own commit. Three more commits
followed on 2026-09-14: header dependencies for the Dreamcast build, a
fixed-frame benchmark mode, and emission changes (§10 to §12). No pull
request has been opened by this task.

## Where things stand

| Check | Result |
|---|---|
| Host tests | All 13 C suites and the save/config, audio callback and frame pacing tests pass under MSVC. Hosted CI (GCC) passed on every pushed commit through `19c52f1`, including the frame pacing test. Audio also passed GCC AddressSanitizer and UndefinedBehaviorSanitizer. Both test runners include the Python tests. The save test compiles extracted save/config functions, not ROM paging code. |
| New tests fail when their fix is reverted | Confirmed for the SWI LR fix (2 failures), STM SMC dispatch (6) and the store tag check (3) |
| Dreamcast cross-build (CI container) | Clean from scratch. Header dependencies are tracked since `555016c`. Only pre-existing warnings. |
| Super Puzzle Fighter II in Flycast 2.7, stock 16 MB | Runs with every change applied |
| Grand Theft Auto Advance | **Still crashes**: [GTA_BAD_JUMP_LOG_2026-09-13.md](GTA_BAD_JUMP_LOG_2026-09-13.md), ROADMAP B10 |
| Audio | Wrap corruption and callback sample waits addressed; behavioral tests and Dreamcast cross-build pass. Listening in Flycast/hardware is still pending; see P1. |

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
shows `>>`) was on, and the counter read 34 to 250.

**Correction, 2026-09-14: none of the builds in this table contained F4.**
`dc/Makefile` had no header dependencies, so editing `dc/sh4_emit.h` never
rebuilt `cpu_threaded.o`, and every run above used the recompiler as of F1.
The "+ F4" row reflects sampling position, not F4. F4 first ran on target in
the fixed-frame benchmark (§10).

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
instruction description, not from hardware. The 2026-09-13 Flycast runs did
not contain F4 (see the correction in the frame-rate section). Its first
on-target runs, on 2026-09-14, rendered correctly through the benchmark span
but measured slower than the stale build; see §10.

### 8. Smaller changes

- **`main.c`:** the Dreamcast `synchronize()` no longer runs a float `sprintf`
  for `SDL_WM_SetCaption` every frame. That call does nothing on the
  Dreamcast.
- **`scripts/dc-disc.sh`:** packages a CDI inside the CI container.
- **`scripts/flycast-run.ps1`:** unattended Flycast run producing screenshots
  and an fps strip.
- **`scripts/host-tests-msvc.bat`:** host tests without gcc or make.
- **`GTA_BAD_JUMP_LOG_2026-09-13.md` and ROADMAP row B10.**

### 9. Dreamcast frame pacing and auto frameskip (F12)

Files: `main.c`, `tests/frame_pacing_test.py`, `tests/Makefile`,
`scripts/host-tests-msvc.bat`.

**Why.** Automatic frameskip was the default but did nothing on Dreamcast: the
non-PSP `synchronize()` honored only manual frameskip. It also paced frames
with a 15 ms delay, though a GBA frame lasts 16.743 ms, measured with
`SDL_GetTicks()`, which counts whole milliseconds.

**How.**

- `dc_frame_pace()` keeps a real-time deadline that advances one GBA frame
  (16,743 µs) per emulated frame.
  - Ahead of the deadline, `synchronize()` waits.
  - At least a frame behind, auto frameskip skips drawing the next frame, at
    most `frameskip_value` frames in a row.
  - More than eight frames behind, the backlog is dropped rather than skipped
    through.
  - Fast forward draws one frame in five. Manual frameskip and "off" behave
    as before.
- Skipping saves only drawing: scanline rendering and the flip. The CPU,
  timers and sound still run every frame.
- `get_ticks_us()` on Dreamcast reads KOS `timer_us_gettime64()`.
- Building with `GPSP_EXTRA_CFLAGS=-DGPSP_DC_SHOW_FPS` draws
  `emu NN.N fps NN%  drawn NN.N fps` below the GBA picture, updated every
  second. make does not track flags, so touch `main.c` when switching.

**Tests.** `frame_pacing_test.py` compiles `dc_frame_pace()` from `main.c` and
simulates per-frame costs:

- full speed stays exactly on schedule without skipping;
- "off" never skips;
- 12 ms of emulation plus 8 ms of drawing holds real time while drawing 55% to
  65% of frames;
- a 30 ms emulation cost skips exactly four in a row and keeps lag bounded;
- a one-second stall causes at most one skipped frame;
- fast forward draws one frame in five;
- manual frameskip 2 skips two frames in three.

Each of these reverted breaks the test: the skip, the backlog reset, the skip
cap.

**Measured.** Super Puzzle Fighter II attract mode, Flycast 2.7, stock 16 MB,
automatic frameskip with value 4. Emulated and drawn rates come from the
on-screen counter.

| Moment | Emulated | Share of full speed | Drawn |
|---|---|---|---|
| Menus, 30 s | 37.4 fps | 63% | 8.8 fps |
| Match, 60 s | 21.2 fps | 35% | 3.8 fps |
| Score screen, 90 s | 38.6 fps | 65% | 7.9 fps |

Flycast's counter, which counts presented frames, read 5.9 to 11.0 over the
run (mean 8.2). Drawing every frame, the previous build ran about 28 fps in
menus (47%). These runs also used the pre-F4 recompiler (see the correction
in the frame-rate section). That changes the absolute figures, not the
frameskip analysis.

From those two menu figures, a frame costs about 24.5 ms to emulate and 11 ms
to draw. During a match emulation alone takes about 45 ms. **Frameskip cannot
reach full speed on this title; the dynarec is the bottleneck.** In heavy
scenes it buys about 20% more speed for a four- to five-fold drop in drawn
frames.

**Open decision.** gpSP's default, automatic with value 4, now takes effect.
Games look choppier than before in exchange for speed. Two alternatives are
each a one-line change in `main.c`: a lower default value (1 draws at least
every other frame), or frameskip off by default. Users can change both in the
menu.

### 10. Fixed-frame benchmark

File: `main.c` (commit `4d4941f`).

**Why.** Wall-clock samples cannot compare builds. A faster build reaches the
heavier parts of the attract sequence sooner, so its samples come from
different scenes.

**How.**

- Build with `GPSP_EXTRA_CFLAGS=-DGPSP_DC_BENCHMARK -DGPSP_DC_SHOW_FPS`.
- The build draws every frame, never waits, and times emulated frames 600 to
  2400 after boot. The result appears below the picture as
  `bench 600-2400: NN.NN fps, NNNNN ms`.
- With no input, the attract sequence repeats exactly in emulated frames, so
  the span is the same work in every build.
- Timing uses the guest's own clock, so host load during a run does not
  change the result.

**Deterministic.** Two runs of the same Super Puzzle Fighter II disc both
measured 66,137 ms. Differences of a few milliseconds between builds are real.

**Results.** Super Puzzle Fighter II, Flycast 2.7, stock 16 MB. Every build is
clean except the first row.

| Build | Frames 600–2400 | fps |
|---|---|---|
| Stale objects: `19c52f1` sources, `cpu_threaded.o` from before F4 | 61,926 ms | 29.06 |
| Clean `19c52f1`, which includes F4 | 66,137 ms | 27.21 |
| Same disc, second run | 66,137 ms | 27.21 |
| Clean `19c52f1` without F4 | 66,419 ms | 27.10 |
| Clean `19c52f1` without the audio fix | not measured: the run was interrupted | |
| Clean `19c52f1` + F5 flag base | 65,848 ms | 27.33 |
| + F5 + F2 inline ALU | not measured: Flycast closed 69 s into the run | |
| + F5 + F2 + helper call table (`42c08ea`) | 57,628 ms | 31.23 |

**F4 is not the regression.** The clean build without F4 was slower
(66,419 ms against 66,137 ms), so F4 is worth about 0.4%.

**Unexplained: the stale-object build is about 7% faster than every clean
build.** Its sources match the clean no-F4 build, including the audio fix:
that build recompiled only `main.c`, so make considered `sound.o` newer than
`sound.c`. `make clean` has since removed those objects, so the stale build
cannot be reproduced. The remaining suspects are objects compiled on
2026-09-13, one set possibly by another session with different flags.

The run without the audio fix was interrupted and should be repeated: build
HEAD with `sound.c` from `cd2ed67`, then compare it with the clean HEAD row.

### 11. Emission: flag base, inline ALU, helper call table

Files: `dc/sh4_emit.h`, `dc/sh4_instr.inc`, `dc/sh4_stub.c`, host tests
(commit `42c08ea`).

- **Flag base (F5).** The dispatcher points r8 at `&reg[16]`. The flags, CPSR,
  CPU mode and dispatch state (`reg[16]` to `reg[31]`) then load and store in
  one instruction instead of three.
- **Inline ALU (F2).**
  - Forms that set no flags are emitted inline, with the result in r0: ADD,
    SUB, RSB, AND, ORR, EOR, BIC, MOV, MVN, ADC, SBC, RSC.
  - The same applies to Thumb high-register ADD and MOV.
  - ARM MOV no longer calls an identity helper.
  - Flag-setting forms still call their helpers.
- **Helper call table.**
  - Helpers take slots in a 48-entry table reached from r9, r10 and r11,
    which point at slots 0, 16 and 32.
  - A call is `mov.l @(disp,Rn),r1; jsr @r1; nop`: three instructions instead
    of up to 16.
  - Slots are appended when a call is first emitted and are never reused. A
    full table falls back to building the address.

r8 to r11 are callee-saved under the SH-4 ABI, and the emitter never writes
them. The dispatcher, which sets all four, is the only way into translated
code.

**Tests.**

- The emitted-code simulator now executes register-relative loads and stores.
- `test_executed_inline_alu` runs every inline sequence against the helper
  definitions, over edge values and both carry states. It also checks that
  only r0 to r2 change.
- Encoding tests cover the flag window, plus the helper table's slots, reuse
  and overflow.
- Each of these mutations fails a test: broken SBC, broken RSB, a narrower
  flag window, a wrong table base, no slot reuse, an unscaled table
  displacement.

**On target.** F5 completed the benchmark span with correct rendering.
F2 and the helper call table completed the benchmark span with correct rendering, and the run continued through the attract sequence back to the title screen. Together they cut frames 600 to 2400 from 65,848 ms to 57,628 ms (12.5% less time; 27.33 to 31.23 fps). The split between them is unmeasured, because the F2-only run was interrupted.

### 12. Header dependencies in the Dreamcast build

Files: `dc/Makefile`, `.gitignore` (commit `555016c`).

The build passes `-MMD -MP`, includes `$(OBJS:.o=.d)`, and removes the `.d`
files on `make clean`. Before this, editing a header rebuilt nothing that
included it; see the correction in the frame-rate section.

## What needs work next, in priority order

### P1. Audio (user-reported)

The first two findings now have fixes in `sound.c`, with behavioral coverage in
`tests/audio_callback_test.py`. This test compiles the actual callback and copy
macros with SDL shims in both Dreamcast and desktop modes. It covers wrap and
exact-boundary copies, mute, clipping, stereo ordering, empty/partial buffers,
a fade spanning callbacks, resumption, and preservation of unconsumed ring
slots. The Dreamcast shim rejects any condition-variable wait. Both variants
pass under MSVC and GCC, including GCC address/undefined-behavior sanitizers.
Temporary mutations confirmed the tests reject the old wrap destination and
desktop byte/sample wait bugs, consumption beyond available samples, and a
fade that restarts every callback.

The Dreamcast ELF also cross-builds with the existing toolchain. No listening
test has been performed for this follow-up, so audible improvement remains to
be verified. AICA backend and timer-lock changes are still proposals.
The attempted Flycast listening check was interrupted by the user before
gameplay or audio output could be verified; it is not a completed test.
A local ignored disc image, `dc/audio-fix-spf.cdi`, packages this build with
the user's Super Puzzle Fighter II assets for the listening check.

1. **Output corruption at every ring-buffer wrap.** In `sound_callback`
   (`sound.c`), the wrap branch calls `sound_copy` twice. Both calls write
   from `stream_base[0]`. The second half overwrites the start of the output,
   and the end keeps the previous buffer's samples. `sound_copy_null` has the
   same bug.
   - *When:* every time `sound_buffer_base` wraps `BUFFER_SIZE` (32,768
     values), roughly every 0.74 s at 22,050 Hz stereo. Expect a periodic
     click.
   - *Fixed:* advance the destination by the first half's sample count before
     the second copy, in both branches. Sample scaling now uses multiplication
     after clipping, avoiding a signed left shift of negative samples.
2. **The callback blocks.** It waits on `sound_cv` until the emulator has
   produced a buffer. Its check compares the buffered count against `length`
   in bytes, so it waits for twice what it needs.
   - *Effect:* KOS SDL (`kos-ports/SDL/files/SDL_dcaudio.c`) plays a two-half
     ring in AICA RAM, and the AICA loops the old half while the callback
     waits. Below full speed, which is always today, that is a repeating
     buzz.
   - *Fixed:* on Dreamcast, do not wait for the producer. Copy only complete
     stereo frames already buffered. Fade each channel's last output to zero
     over 64 frames (about 2.9 ms at 22,050 Hz), keeping fade progress across
     callbacks, then fill silence. Mute/reset clear the fade. Never move
     `sound_buffer_base` past `gbc_sound_buffer_index` or clear future samples
     that direct sound may already be mixing. The callback still takes the
     existing mutex; this is not a lock-free implementation.
   - Desktop retains its wait, comparing sample counts consistently.
   - The producer now publishes `gbc_sound_buffer_index` while holding the
     mutex and before signaling. Previously publication happened after unlock.
     Its throttle above 1.5x `audio_buffer_size` is preserved.
3. **Upload cost.** `SDL_dcaudio` uploads one stereo sample at a time
   (`spu_memload_stereo16`). Each write does a G2 FIFO wait and disables and
   restores interrupts. It busy-waits on the AICA position with `thd_pass`.
   - *How:* consider KOS `snd_stream` (`dc/sound/stream.h`), whose pull
     callback returns a buffer and uploads in blocks, polled with
     `snd_stream_poll`.
4. **Minor.** `sound_timer` locks `sound_mutex` on every direct-sound timer
   tick.

Next, verify by ear in Flycast with fast-forward off: listen through repeated
ring wraps, slow gameplay, mute/unmute, and reset. Compare with `cd2ed67` at
the same game position. The fade avoids replaying an old output buffer but
cannot restore missing emulation time; gaps at low frame rates can remain.
Frameskip (F12, below) changes how audio behaves when the emulator is slow.

### P2. Performance

The plan is `DC_PERFORMANCE_PLAN_2026-09-10.md`. The frameskip measurements
in §9 show that emulation, not drawing, dominates the frame, so dynarec work
comes first.

- **Explain F4's benchmark result** (§10). If the build without F4 is faster,
  check whether Flycast implements `shad`/`shld` slowly before reverting
  anything. Real hardware is the target.
- **F5 is done** (§11).
- **F12 is done** (§9). Its default setting is an open decision.
- **Phase 0, remaining.** The on-screen counter (§9) and the fixed-frame
  benchmark (§10) exist. Still missing: a menu toggle and translation
  counters.
- **Larger items:**
  - F3: a literal pool for PC values and large immediates. Helper calls no
    longer need one (§11).
  - F2, remaining: inline the flag-setting forms, N and Z for logic
    operations, C and V for arithmetic.
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

The original work is committed and pushed in four commits through `cd2ed67`:

1. ROM paging empty-slot fix (`memory.c`).
2. Dynarec correctness, the fatal-error report and F1. They share
   `cpu_threaded.c` and `dc/sh4_stub.c`, so they are one commit.
3. F4 constant shifts, with the simulator and encoding tests.
4. Documentation and scripts.

The audio follow-up is included in the subsequent audio-fix commit.
GCC and MSVC host suites pass locally,
including save/config and audio callback tests; the Dreamcast cross-build
also passes. Check hosted CI after publishing the audio follow-up and before
opening a pull request against `dreamcast`.

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
- Before `555016c`, header edits did not rebuild the objects that include
  them. On an older checkout, run `make clean` after changing emitter headers.
- The fixed-frame benchmark is deterministic in Flycast: the same disc gives
  the same millisecond count.
- In this environment, commands run through the shell tool can collapse
  doubled backslashes and break long heredocs. Write C strings that contain
  escapes, and long text, with a file editor.
- `cmd` here does not run executables from the current directory. Give batch
  files absolute paths.
