# Grand Theft Auto Advance "bad jump" — investigation log, 2026-09-13

Status: **open**. Tracked as ROADMAP B10 and performance plan finding F13.

Grand Theft Auto Advance (USA), 16 MB, stops 30 to 45 seconds after boot,
after its intro art, with a dynarec fatal error. The failure is deterministic.
Three runs in Flycast 2.7 (stock 16 MB, default settings) produced identical
registers: one at HEAD `b9cb1e1`, one with the diagnostic report added, and
one with the two fixes described below.

## Reproducing

Build with the CI container and package a disc as in
[DC_PERFORMANCE_PLAN_2026-09-10.md](DC_PERFORMANCE_PLAN_2026-09-10.md) §4,
with the ROM as the autoload title. No input is needed; the game crashes
unattended.

## What the fatal screen now reports

The one-line message (`bad jump 1a3019f8 (3000154) (0)`) could not be acted
on. Its middle value is only the last PC some helper wrote to `reg[REG_PC]`,
not the code that jumped. The report on the fatal screen now includes:

- the requested address and lookup type;
- the guest block that made the jump, found by matching the SH-4 return address
  of the dispatch call against a 64-entry log of recent translations;
- all ARM registers, CPSR, flags, SPSR and the RAM flush count;
- the banked Supervisor SP and LR, the four words at the Supervisor stack
  pointer, and the words at `LR_svc - 4`;
- the first words of the most recent translation;
- the tail of the source block's guest code, when the block is known;
- the emitted SH-4 code before the dispatch call;
- the four most recent translations.

Everything goes on screen, because serial output is not visible: Flycast shows
it in a separate window, and hardware needs a cable.

## Captured report

```
bad jump 1a3019f6 (dual lookup)
from host 8c1e4bf8, block unknown
r0  03007ff8 02001fd0 08ebeb44 086ad668
r4  ffffff80 0800d52b 0000003c 0000002d
r8  00000085 040000d4 02000130 0000001f
r12 1a3019f6 03007da8 00000170 03000154
cpsr 1f nzcv 0000 spsr 00000000 rf 4
mode 0 svc sp 03007fd0 lr 03007ddc
svc stk 6000001f 02023700 02035ca8 03007ddc
@lr-4 efef0000 00000000
newest 03007dd8: efef0000 00000000
4118 7164 410b 0009 e400 e501 4518 7570
e18c 611c 4118 7101 4118 7108 4118 717c
410b 0009 1c0e 54cc 65d3 e18c 611c 4118
7101 4118 7102 4118 717f 7179 410b 0009
A 03000574-0300058c A 03000680-03000694
A 03000270-03000274 A 03007dd8-03007ddc
```

The last block of SH-4 words is from the first diagnostic run, where the
report showed 32 words. Later runs show 24 words and moved host addresses. The
guest state was identical in every run.

## Decoding

1. **The failing dispatch is in the BIOS.** The emitted code before the call
   decodes as `mov #0,r4; mov #0x170,r5; jsr execute_add; mov.l r0,@(56,r12)`,
   then `mov.l @(48,r12),r4; mov r13,r5; jsr sh4_indirect_branch_dual`. That is
   ARM `ADD LR, PC, #0` followed by `BX R12`, with PC + 8 = 0x170. Those two
   instructions sit at BIOS 0x168 and 0x16C. The source block is "unknown" only
   because the BIOS was translated long before the log's last 64 entries.
2. **That code is the BIOS SWI dispatcher.** BIOS 0x140 to 0x16C:
   `stmdb sp!,{r11,r12,lr}`, `ldrb r12,[lr,#-2]`, `adr r11,0x1C8`,
   `ldr r12,[r11,r12,lsl #2]`, then a switch to System mode, `add lr,pc,#0`,
   `bx r12`.
3. **The SWI number was 0xEF.** Index 0xEF (BIOS offset 0x584) is the only
   table slot holding `0x1a3019f6`.
4. **The return address was correct.** The BIOS pushed LR_svc = 0x03007ddc and
   SPSR = 0x6000001F (System mode, ARM state, Z and C set). That is the right
   return address for an ARM SWI at 0x03007dd8.
5. **The instruction really is `SWI 0xEF0000`.** The word at 0x03007dd8 is
   `EFEF0000`. The BIOS read the SWI number faithfully.
6. **0x03007dd8 is data on the IWRAM stack.** The user SP is 0x03007da8. Real
   hardware would fail the same way if it executed this word, so the emulation
   error happened earlier: something transferred control, in ARM state, to a
   stack address.
7. **Context.** The translation just before the stack block was a
   one-instruction ARM exit block at IWRAM 0x03000270. Blocks at 0x03000574 and
   0x03000680 are also ARM IWRAM code. r9 = 0x040000D4 (the DMA3 source
   register) and r2/r3 hold ROM addresses, so the game was driving DMA3
   transfers from ROM. The log records translation time, not execution order,
   so "just before" means translated just before.

## Ruled out, with fixes that landed anyway

Both of these were real bugs with test coverage. Neither changed the crash; the
registers were byte-for-byte identical afterwards.

- **Stale LR_svc for a SWI taken in Supervisor mode.** `execute_swi` and both
  interpreter SWI paths wrote the *banked* LR and then switched mode. A
  same-mode switch leaves the bank untouched, so the live LR was never set. The
  fix sets the live LR after the switch. GTA's SWI came from System mode, so
  this was not the cause.
- **Stale IWRAM translations after ARM `STM`.** The Dreamcast LDM/STM helper
  wrote through `execute_aligned_store32`, which never checked translation
  tags, so copying code over translated RAM left the old translation running.
  Upstream gpSP at least checks the final store. Stores now raise a pending
  alert. The ARM helper flushes and resumes at the next instruction. A Thumb
  block transfer's final store, or the next `update_gba`, consumes alerts left
  by Thumb transfers that have no checked final store.
- **ROM paging.** The translator reads no ROM data at translate time, and the
  page map stays consistent across evictions. Unused buffer slots claimed
  physical page 0, so early loads repeatedly unmapped and re-read page 0.
  That is fixed but was only a performance problem.

## Next steps

1. **Record the jump into the stack.** Keep a small ring of
   (host return address, target) pairs for every indirect dispatch and print
   the last few pairs on a bad jump. Mapping each return address through the
   translation log gives the guest instruction that branched to 0x03007dd8.
2. **Dump more IWRAM at the fault:** the words at 0x03000270 and the user stack
   from SP, including the LR that the BIOS saved at 0x03007dac.
3. **Separate paging from the dynarec.** Run once with a resident buffer large
   enough to hold the whole ROM, which a 32 MB Flycast RAM setting allows, and
   once with the adjacent-page prefetch disabled. If either run survives, the
   corrupt jump comes from ROM data read through a stale page.
4. **Compare against a reference emulator.** Breaking in mGBA when execution
   first reaches 0x03000270 would show what that instruction should branch to.
