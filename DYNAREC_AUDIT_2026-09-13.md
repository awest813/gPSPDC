# Dynarec continuation — September 13, 2026

Starting checkout: `bda07fd`, clean worktree. Earlier September 10 changes were already committed. This pass addresses four reproduced register-bank and block-transfer problems.

| Fix | Before | After |
|---|---|---|
| SH-4 LDM/STM user-bank selection | S-bit transfers treated R8–R12 as separately banked in every privileged mode, reading/writing stale saved values. | R8–R12 use the live shared registers outside FIQ. In FIQ, user transfers access the saved shared bank. SP/LR retain their separate banks. |
| SH-4 LDM exception return | LDM with S and PC set selected user registers and never restored CPSR. | Load into the current bank, complete writeback, then restore CPSR and the instruction state through the existing tested exception-return helper. Pending IRQs use the restored state. |
| SH-4 ordinary LDM PC alignment | Passing the raw loaded PC to the dual dispatcher could switch to Thumb from bit 0 or round an ARM address upward. | Ordinary ARMv4T LDM clears the low two PC bits before dispatch; an S-bit exception return selects state from SPSR. |
| Shared-core FIQ switching | Entering FIQ saved R8–R12 but did not load FIQ's bank. Leaving FIQ did not preserve its R8–R12, and could restore a stale non-FIQ bank. | FIQ entry/exit exchange R8–R12 with one shared non-FIQ bank. SP/LR switch independently for every exception mode. Same-mode calls leave registers untouched. |

The LDM distinction follows the ARM Architecture Reference Manual's LDM (1), LDM (2), and LDM (3) descriptions, including the ARMv4T rules. See [ARM DDI 0100I, hosted by TI](https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/1023/ARM-Architecture.pdf). Implementation is local; no external code was copied.

The register-bank implementation now lives in `cpu_mode.h`, called by the existing `set_cpu_mode()` wrapper in `cpu.c`. This allows the host helper tests to exercise the same production switching logic instead of a mode-number-only stub. Platform makefiles track the new header as a dependency of `cpu.o`.

## Evidence

- Initial mapped-memory LDM/STM cases reproduced **36 failures** across addressing modes, user-bank transfers, ordinary PC loads, and exception returns.
- Exercising the former production bank-switch implementation reproduced **70 failures** across mode transitions and round trips.
- The expanded suite now passes **159,831 checks**. It covers all 36 pairs of valid internal CPU modes, bank preservation, all four block-transfer addressing modes, writeback, FIQ user-bank transfers, and LDM returns with and without a pending IRQ.
- All **13 host C test programs** and the save/config fault-injection tests pass with MSVC `/O2` for the C programs. The build-contract test uses Windows `popen`/`pclose` aliases. No GCC/KOS result is implied by these runs.

## Remaining work

The SH-4 block-transfer corrections do not update the interpreter or other dynarec block-transfer implementations. Those still need a parity pass; only the register-bank switching fix is shared across cores. Existing savestates may contain stale inactive bank values produced by the old implementation, which this change cannot reconstruct.

Block-store alert/SMC propagation, MMIO ordering, empty register lists, and broader instruction timing remain audit targets. Mapped-memory tests and the actual bank switch are exercised here; real memory/IRQ hardware effects and SH-4 dispatch are not. Docker's engine is unavailable, so a fresh KOS build and on-target smoke test remain outstanding. This pass makes no performance claim.
