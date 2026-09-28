/* Executes the production SH-4 dynarec under qemu-sh4 and compares it
 * with the production interpreter (cpu.c).  See harness.c.
 * gameplaySP, Copyright (C) 2006 Exophase; GPL-2.0-or-later, see LICENSE.
 */
#include "common.h"
#include "harness.h"

static int failures;
static int checks;

#define EXPECT(cond, ...)                                                    \
  do {                                                                        \
    checks++;                                                                 \
    if(!(cond)) {                                                             \
      failures++;                                                             \
      printf("FAIL %s:%d: ", __func__, __LINE__);                             \
      printf(__VA_ARGS__);                                                    \
      printf("\n");                                                           \
    }                                                                         \
  } while(0)

static const char *status_name(harness_status s)
{
  switch(s)
  {
    case HARNESS_RUN_OK: return "ok";
    case HARNESS_RUN_TIMEOUT: return "timeout";
    case HARNESS_RUN_FATAL: return "fatal";
    case HARNESS_RUN_INCOHERENT: return "incoherent";
  }
  return "?";
}

/* ---- tiny encoders ---------------------------------------------------- */

#define AL 0xE
#define ARM_B_SELF 0xEAFFFFFE          /* b . */
#define THUMB_B_SELF 0xE7FE            /* b . */

static u32 arm_dp_imm(u32 cond, u32 op, u32 s, u32 rn, u32 rd, u32 imm8,
 u32 rot)
{
  return (cond << 28) | (1 << 25) | (op << 21) | (s << 20) | (rn << 16) |
   (rd << 12) | (rot << 8) | imm8;
}

static u32 arm_swi(u32 comment)
{
  return (AL << 28) | 0x0F000000 | (comment << 16);
}

/* LDR/STR rd, [rn, #imm] (pre-indexed, up, no writeback) */
static u32 arm_ldst_imm(u32 load, u32 rn, u32 rd, u32 imm)
{
  return (AL << 28) | 0x05800000 | (load << 20) | (rn << 16) | (rd << 12) |
   imm;
}

static void put_arm(u32 address, const u32 *code, u32 count)
{
  u32 i;
  for(i = 0; i < count; i++)
    harness_write32(address + i * 4, code[i]);
}

static void put_thumb(u32 address, const u16 *code, u32 count)
{
  u32 i;
  for(i = 0; i < count; i++)
    harness_write16(address + i * 2, code[i]);
}

/* Minimal BIOS: SWI returns with movs pc, lr after bumping r11; the IRQ
   vector bumps r9, acknowledges IF, and returns with subs pc, lr, #4. */
static void put_bios(void)
{
  static const u32 swi_vector[] =
  {
    0xE28BB001,   /* add r11, r11, #1 */
    0xE1B0F00E    /* movs pc, lr */
  };
  static const u32 irq_vector[] =
  {
    0xE92D0003,   /* stmfd sp!, {r0, r1} */
    0xE2899001,   /* add r9, r9, #1 */
    0xE3A00301,   /* mov r0, #0x04000000 */
    0xE2800C02,   /* add r0, r0, #0x200 */
    0xE1D010B2,   /* ldrh r1, [r0, #2] (IF) */
    0xE1C010B2,   /* strh r1, [r0, #2] (acknowledge) */
    0xE8BD0003,   /* ldmfd sp!, {r0, r1} */
    0xE25EF004    /* subs pc, lr, #4 */
  };
  put_arm(0x08, swi_vector, 2);
  put_arm(0x18, irq_vector, 8);
}

/* ---- directed tests --------------------------------------------------- */

static harness_status run_both(harness_state *initial, u32 exit_pc,
 harness_state *dyn, harness_state *interp)
{
  harness_status s_dyn, s_int;

  harness_load(initial);
  s_dyn = harness_run(HARNESS_DYNAREC, exit_pc);
  if(s_dyn != HARNESS_RUN_OK)
    printf("  dynarec: %s %s\n", status_name(s_dyn), harness_fatal_message);
  harness_save(dyn);

  harness_load(initial);
  s_int = harness_run(HARNESS_INTERPRETER, exit_pc);
  if(s_int != HARNESS_RUN_OK)
    printf("  interpreter: %s\n", status_name(s_int));
  harness_save(interp);

  return (s_dyn == HARNESS_RUN_OK && s_int == HARNESS_RUN_OK) ?
   HARNESS_RUN_OK : (s_dyn != HARNESS_RUN_OK ? s_dyn : s_int);
}

static int compare_states(const harness_state *a, const harness_state *b,
 int verbose)
{
  u32 i;
  int same = 1;

  for(i = 0; i < 16; i++)
  {
    if(a->reg[i] != b->reg[i])
    {
      same = 0;
      if(verbose)
        printf("  r%u: dynarec %08x interpreter %08x\n", i, a->reg[i],
         b->reg[i]);
    }
  }
  if((a->reg[REG_CPSR] & 0xF00000FF) != (b->reg[REG_CPSR] & 0xF00000FF))
  {
    same = 0;
    if(verbose)
      printf("  cpsr: dynarec %08x interpreter %08x\n", a->reg[REG_CPSR],
       b->reg[REG_CPSR]);
  }
  if(memcmp(a->iwram, b->iwram, sizeof(a->iwram)) != 0 ||
   memcmp(a->ewram_lo, b->ewram_lo, sizeof(a->ewram_lo)) != 0)
  {
    same = 0;
    if(verbose)
      printf("  RAM contents differ\n");
  }
  return same;
}

static void test_arm_rom_basic(void)
{
  harness_state initial, dyn, interp;
  const u32 code[] =
  {
    arm_dp_imm(AL, 0xD, 0, 0, 0, 5, 0),     /* mov r0, #5 */
    0xE0801100,                             /* add r1, r0, r0, lsl #2 */
    arm_dp_imm(AL, 0x2, 1, 1, 2, 25, 0),    /* subs r2, r1, #25 */
    ARM_B_SELF
  };

  harness_reset();
  put_arm(HARNESS_ROM_BASE, code, 4);
  reg[REG_PC] = HARNESS_ROM_BASE;
  harness_save(&initial);

  EXPECT(run_both(&initial, HARNESS_ROM_BASE + 12, &dyn, &interp) ==
   HARNESS_RUN_OK, "run failed");
  EXPECT(dyn.reg[1] == 25 && dyn.reg[2] == 0, "r1=%u r2=%u", dyn.reg[1],
   dyn.reg[2]);
  EXPECT(dyn.reg[REG_CPSR] & 0x40000000, "Z not set: cpsr %08x",
   dyn.reg[REG_CPSR]);
  EXPECT(compare_states(&dyn, &interp, 1), "engines disagree");
}

/* execute_arm_translate must honour CPSR.T on entry (e.g. a Thumb-mode
   savestate loaded before the dynarec first runs). */
static void test_thumb_entry(void)
{
  harness_state initial, dyn, interp;
  const u16 code[] =
  {
    0x2007,        /* mov r0, #7 */
    0x3003,        /* add r0, #3 */
    THUMB_B_SELF
  };

  harness_reset();
  put_thumb(HARNESS_ROM_BASE, code, 3);
  reg[REG_PC] = HARNESS_ROM_BASE;
  reg[REG_CPSR] |= 0x20;
  harness_save(&initial);

  EXPECT(run_both(&initial, HARNESS_ROM_BASE + 4, &dyn, &interp) ==
   HARNESS_RUN_OK, "run failed");
  EXPECT(dyn.reg[0] == 10, "r0=%u", dyn.reg[0]);
  EXPECT(compare_states(&dyn, &interp, 1), "engines disagree");
}

/* A SWI's exit is linked while its block is translated, which translates
   the BIOS vector recursively into the BIOS cache.  That code must be
   written back and invalidated before it runs. */
static void test_swi_links_bios_coherently(u32 code_base)
{
  harness_state initial, dyn, interp;
  const u32 code[] =
  {
    arm_swi(0x05),                          /* swi 0x05 (not HLE) */
    arm_dp_imm(AL, 0x4, 0, 11, 11, 16, 0),  /* add r11, r11, #16 */
    ARM_B_SELF
  };

  harness_reset();
  put_bios();
  put_arm(code_base, code, 3);
  reg[REG_PC] = code_base;
  harness_save(&initial);

  EXPECT(run_both(&initial, code_base + 8, &dyn, &interp) == HARNESS_RUN_OK,
   "run failed (base %08x)", code_base);
  EXPECT(dyn.reg[11] == 17, "r11=%u", dyn.reg[11]);
  EXPECT(compare_states(&dyn, &interp, 1), "engines disagree");
}

/* A store overwrites an instruction of an already translated IWRAM block;
   the SMC flush must retranslate, and the new code must be coherent. */
static void test_iwram_smc(void)
{
  harness_state initial, dyn, interp;
  const u32 base = 0x03000100;
  const u32 code[] =
  {
    arm_dp_imm(AL, 0xD, 0, 0, 0, 1, 0),     /* 00 mov r0, #1 */
    arm_ldst_imm(1, 15, 1, 12),             /* 04 ldr r1, [pc, #12] (=20) */
    arm_ldst_imm(0, 15, 1, 0),              /* 08 str r1, [pc] (-> 10) */
    arm_dp_imm(AL, 0xD, 0, 0, 2, 0, 0),     /* 0c mov r2, #0 */
    arm_dp_imm(AL, 0xD, 0, 0, 0, 2, 0),     /* 10 mov r0, #2 (patched) */
    ARM_B_SELF,                             /* 14 */
    arm_dp_imm(AL, 0xD, 0, 0, 0, 3, 0)      /* 18 literal: mov r0, #3 */
  };
  u32 fixed[7];

  memcpy(fixed, code, sizeof(fixed));
  fixed[1] = arm_ldst_imm(1, 15, 1, 0x18 - 0x04 - 8);
  fixed[2] = arm_ldst_imm(0, 15, 1, 0x10 - 0x08 - 8);

  harness_reset();
  put_arm(base, fixed, 7);
  reg[REG_PC] = base;
  harness_save(&initial);

  EXPECT(run_both(&initial, base + 0x14, &dyn, &interp) == HARNESS_RUN_OK,
   "run failed");
  EXPECT(dyn.reg[0] == 3, "r0=%u (stale translation executed)", dyn.reg[0]);
  EXPECT(compare_states(&dyn, &interp, 1), "engines disagree");
}

/* A store that raises an IRQ (DMA-completion style) enters the handler
   and resumes at the next instruction with flags intact. */
static void test_store_irq_alert(void)
{
  harness_state initial, dyn, interp;
  u32 seq[10];
  u32 n = 0;

  seq[n++] = arm_dp_imm(AL, 0xD, 0, 0, 0, 1, 3);    /* mov r0, #0x04000000 */
  seq[n++] = arm_dp_imm(AL, 0x4, 0, 0, 3, 2, 12);   /* add r3, r0, #0x200 */
  seq[n++] = arm_dp_imm(AL, 0xD, 0, 0, 1, 1, 0);    /* mov r1, #1 */
  seq[n++] = 0xE1C310B0;                            /* strh r1, [r3] (IE) */
  seq[n++] = 0xE1C310B8;                            /* strh r1, [r3, #8] (IME) */
  seq[n++] = arm_dp_imm(AL, 0xA, 1, 1, 0, 1, 0);    /* cmp r1, #1 (Z=1,C=1) */
  seq[n++] = 0xE58013F0;                            /* str r1, [r0, #0x3f0] */
  seq[n++] = 0x03A0A007;                            /* moveq r10, #7 */
  seq[n++] = ARM_B_SELF;

  harness_reset();
  put_bios();
  put_arm(HARNESS_ROM_BASE, seq, n);
  reg[REG_PC] = HARNESS_ROM_BASE;
  harness_save(&initial);

  EXPECT(run_both(&initial, HARNESS_ROM_BASE + (n - 1) * 4, &dyn, &interp)
   == HARNESS_RUN_OK, "run failed");
  EXPECT(dyn.reg[9] == 1, "IRQ handler ran %u times", dyn.reg[9]);
  EXPECT(dyn.reg[10] == 7, "flags lost across IRQ: r10=%u", dyn.reg[10]);
  EXPECT(compare_states(&dyn, &interp, 1), "engines disagree");
}

/* ---- differential fuzz ------------------------------------------------ */

static u32 rng_state;

/* The interpreter still carries flag and shift bugs the dynarec helpers
   fixed in the September 10 audit (ADC/SBC/RSC carry-in edge cases and
   register-specified shift amounts).  The gating fuzz avoids those classes
   so every disagreement it reports is a dynarec defect; the full mode is a
   triage aid. */
static u32 fuzz_full;

static u32 rng(void)
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static u32 rng_below(u32 n)
{
  return rng() % n;
}

static u32 fuzz_reg(void)
{
  /* r0-r12 and r14; r13 stays a valid stack pointer. */
  u32 r = rng_below(14);
  return (r == 13) ? 14 : r;
}

static u32 fuzz_arm_insn(void)
{
  u32 cond = rng_below(8) ? AL : rng_below(15);
  u32 kind = rng_below(10);

  if(kind < 6)
  {
    /* Data processing; rd never PC, operands may read PC. */
    u32 op = rng_below(16);
    u32 s;
    while(!fuzz_full && op >= 0x5 && op <= 0x7)
      op = rng_below(16);
    s = (op >= 0x8 && op <= 0xB) ? 1 : rng_below(2);
    u32 rn = rng_below(8) ? fuzz_reg() : 15;
    u32 rd = fuzz_reg();
    u32 insn = (cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12);

    if(rng_below(3) == 0)
      return insn | (1 << 25) | (rng_below(16) << 8) | rng_below(256);

    {
      u32 rm = rng_below(8) ? fuzz_reg() : 15;
      u32 type = rng_below(4);
      if(fuzz_full && rng_below(3) == 0)
        return insn | (fuzz_reg() << 8) | (type << 5) | 0x10 | rm;
      return insn | (rng_below(32) << 7) | (type << 5) | rm;
    }
  }

  if(kind < 8)
  {
    /* MUL/MLA and the long multiplies with distinct registers. */
    u32 rm = fuzz_reg(), rs = fuzz_reg(), rd, rn;
    do rd = fuzz_reg(); while(rd == rm);
    do rn = fuzz_reg(); while(rn == rd || rn == rm);
    if(rng_below(2))
      return (cond << 28) | (rng_below(4) << 20) | (rd << 16) | (rn << 12) |
       (rs << 8) | 0x90 | rm;
    return (cond << 28) | 0x00800000 | (rng_below(8) << 20) | (rd << 16) |
     (rn << 12) | (rs << 8) | 0x90 | rm;
  }

  if(kind == 8)
  {
    /* MSR CPSR_f, #imm and MRS rd, CPSR. */
    if(rng_below(2))
      return (cond << 28) | 0x0328F000 | (rng_below(16) << 8) |
       rng_below(256);
    return (cond << 28) | 0x010F0000 | (fuzz_reg() << 12);
  }

  /* Short forward conditional branch or branch-with-link over 0-2 insns. */
  return (cond << 28) | ((rng_below(2) ? 0xB : 0xA) << 24) | rng_below(3);
}

static u16 fuzz_thumb_insn(void)
{
  u32 rd = rng_below(8), rs = rng_below(8), rn = rng_below(8);

  switch(rng_below(6))
  {
    case 0: /* shift by immediate */
      return (rng_below(3) << 11) | (rng_below(32) << 6) | (rs << 3) | rd;
    case 1: /* add/sub register or 3-bit immediate */
      return 0x1800 | (rng_below(4) << 9) | (rn << 6) | (rs << 3) | rd;
    case 2: /* mov/cmp/add/sub 8-bit immediate */
      return 0x2000 | (rng_below(4) << 11) | (rd << 8) | rng_below(256);
    case 3: /* ALU operations, including MUL */
    {
      u32 op = rng_below(16);
      /* ADC, SBC and the register shifts (LSL/LSR/ASR/ROR). */
      while(!fuzz_full && op >= 2 && op <= 7)
        op = rng_below(16);
      return 0x4000 | (op << 6) | (rs << 3) | rd;
    }
    case 4: /* hi-register ADD/CMP/MOV, never writing PC */
    {
      u32 op = rng_below(3);
      u32 hd = rng_below(15), hs = rng_below(16);
      if(hd == 13) hd = 12;
      if(hs == 13) hs = 12;
      if((hd < 8) && (hs < 8)) hs += 8;
      return 0x4400 | (op << 8) | ((hd & 8) << 4) | (hs << 3) | (hd & 7);
    }
    default: /* short forward conditional branch */
      return 0xD000 | (rng_below(14) << 8) | rng_below(3);
  }
}

static int fuzz_case(u32 thumb, u32 seed, int verbose)
{
  harness_state initial, dyn, interp;
  u32 i, count = 4 + rng_below(12);
  u32 end;

  harness_reset();
  put_bios();
  for(i = 0; i < 16; i++)
    reg[i] = rng_below(4) ? rng() : rng_below(4) * 0x40000000u + rng_below(3);
  reg[REG_SP] = 0x03007F00;
  reg[REG_CPSR] = (rng() & 0xF0000000) | 0x1F | (thumb ? 0x20 : 0);
  reg[REG_PC] = HARNESS_ROM_BASE;

  for(i = 0; i < count; i++)
  {
    if(thumb)
      harness_write16(HARNESS_ROM_BASE + i * 2, fuzz_thumb_insn());
    else
      harness_write32(HARNESS_ROM_BASE + i * 4, fuzz_arm_insn());
  }
  /* A few sentinel slots so any short forward branch lands on b . */
  end = HARNESS_ROM_BASE + count * (thumb ? 2 : 4);
  for(i = 0; i < 4; i++)
  {
    if(thumb)
      harness_write16(end + i * 2, THUMB_B_SELF);
    else
      harness_write32(end + i * 4, ARM_B_SELF);
  }
  harness_save(&initial);

  /* Every exit lands in the sentinel run; retry each sentinel address. */
  for(i = 0; i < 4; i++)
  {
    u32 exit_pc = end + i * (thumb ? 2 : 4);
    harness_status s;

    harness_update_limit = 64;
    harness_load(&initial);
    s = harness_run(HARNESS_INTERPRETER, exit_pc);
    if(s != HARNESS_RUN_OK)
      continue;
    harness_save(&interp);

    harness_load(&initial);
    s = harness_run(HARNESS_DYNAREC, exit_pc);
    harness_update_limit = 100000;
    harness_save(&dyn);
    if(s != HARNESS_RUN_OK)
    {
      if(verbose)
        printf("  seed %u: dynarec %s %s\n", seed, status_name(s),
         harness_fatal_message);
      return 0;
    }
    if(!compare_states(&dyn, &interp, verbose))
      return 0;
    return 1;
  }
  harness_update_limit = 100000;
  return 1;
}

static void test_fuzz(u32 thumb, u32 cases)
{
  u32 n, bad = 0;

  for(n = 1; n <= cases; n++)
  {
    u32 seed = n * 2654435761u + (thumb ? 7 : 3);
    rng_state = seed;
    checks++;
    if(!fuzz_case(thumb, seed, 0))
    {
      bad++;
      failures++;
      if(bad <= 5)
      {
        u32 i;
        printf("FAIL %s fuzz seed %u\n", thumb ? "thumb" : "arm", seed);
        rng_state = seed;
        fuzz_case(thumb, seed, 1);
        printf("  code:");
        for(i = 0; i < 16; i++)
        {
          if(thumb)
            printf(" %04x", harness_read32(HARNESS_ROM_BASE + i * 2) &
             0xFFFF);
          else
            printf(" %08x", harness_read32(HARNESS_ROM_BASE + i * 4));
        }
        printf("\n");
      }
    }
  }
  printf("%s fuzz: %u/%u cases agree\n", thumb ? "thumb" : "arm",
   cases - bad, cases);
}

int main(int argc, char **argv)
{
  u32 fuzz_cases = (argc > 1) ? (u32)strtoul(argv[1], NULL, 0) : 2000;

  fuzz_full = (argc > 2) && !strcmp(argv[2], "full");

  setvbuf(stdout, NULL, _IONBF, 0);
  harness_init();

  test_arm_rom_basic();
  test_thumb_entry();
  test_swi_links_bios_coherently(HARNESS_ROM_BASE);
  test_swi_links_bios_coherently(0x03000200);
  test_iwram_smc();
  test_store_irq_alert();
  test_fuzz(0, fuzz_cases);
  test_fuzz(1, fuzz_cases);

  printf("sh4 exec: %d checks, %d failures (%u coherence checks, "
   "%u icache flushes)\n", checks, failures, harness_coherence_checks,
   harness_icache_flushes);
  return failures ? 1 : 0;
}
