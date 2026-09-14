/* Execute the production SH-4 C helpers, with host stubs for memory/IRQ I/O. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define COMMON_H
#define function_cc
#define file_tag_type FILE *
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
/* cpu.h is included without _arch_dreamcast, so it declares the two-argument
   block-transfer helper used by other backends; the SH-4 helper under test
   takes the live cycle counter as well. */
#define execute_arm_block_memory execute_arm_block_memory_other_backends
#include "../cpu.h"
#undef execute_arm_block_memory
#define REG_IE 0
#define REG_IF 1
#define REG_IME 2
#define address32(base, offset) (*(u32 *)((u8 *)(base) + (offset)))
#define ror(dest, value, shift) dest = ((value) >> (shift)) | ((value) << (32 - (shift)))
u32 reg[64], reg_mode[7][7], spsr[6], cpu_modes[32], bios_read_protect;
u16 io_registers[3];
u8 *memory_map_read[8192], *memory_map_write[8192];
const u8 bit_count[256] = {
  0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
  1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
  1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
  1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
  2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
  3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
  3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
  4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};
u32 read_memory8(u32 address) { return 0; }
u32 read_memory16(u32 address) { return 0; }
u32 read_memory16_signed(u32 address) { return 0; }
u32 read_memory32(u32 address) { return 0; }
cpu_alert_type write_memory8(u32 a, u8 v) { return CPU_ALERT_NONE; }
cpu_alert_type write_memory16(u32 a, u16 v) { return CPU_ALERT_NONE; }
cpu_alert_type write_memory32(u32 a, u32 v) { return CPU_ALERT_NONE; }
#include "../cpu_mode.h"
void set_cpu_mode(u32 mode) { cpu_switch_mode(reg, reg_mode, (cpu_mode_type)mode); }
#include "../dc/sh4_helpers.c"

static unsigned failures, checks;
static void check(int pass, const char *name, u32 a, u32 b, u32 carry)
{
  checks++;
  if(!pass && failures++ < 12)
    printf("%s failed: a=%08x b=%08x carry=%u\n", name, a, b, carry);
}

static void arithmetic(u32 a, u32 b, u32 carry)
{
  u32 result, expected, c, v;
  unsigned op;
  for(op = 0; op < 6; op++)
  {
    u32 lhs = op >= 4 ? b : a;
    u32 rhs = op >= 4 ? a : b;
    u32 extra = (op & 1) ? (op < 2 ? carry : 1 - carry) : 0;
    s64 signed_result = op < 2 ? (s64)(s32)lhs + (s32)rhs + extra :
     (s64)(s32)lhs - (s32)rhs - extra;
    u64 wide = op < 2 ? (u64)lhs + rhs + extra : (u64)rhs + extra;
    expected = (u32)signed_result;
    c = op < 2 ? (u32)(wide >> 32) : (u64)lhs >= wide;
    v = signed_result > INT32_MAX || signed_result < INT32_MIN;
    reg[REG_C_FLAG] = carry;
    switch(op)
    {
      case 0: result = execute_adds(b, a); break;
      case 1: result = execute_adcs(b, a); break;
      case 2: result = execute_subs(b, a); break;
      case 3: result = execute_sbcs(b, a); break;
      case 4: result = execute_rsbs(b, a); break;
      default: result = execute_rscs(b, a); break;
    }
    check(result == expected && reg[REG_C_FLAG] == c &&
     reg[REG_V_FLAG] == v && reg[REG_Z_FLAG] == (expected == 0) &&
     reg[REG_N_FLAG] == (expected >> 31), "arithmetic NZCV", a, b, carry);
  }
}

static void shifts(u32 value, u32 amount, u32 carry)
{
  u32 type, variant;
  for(type = 0; type < 4; type++)
  {
    u32 expected = value, c = carry, i;
    /* Bit-by-bit reference avoids host shifts by 32 or more. */
    for(i = 0; i < (amount & 255); i++)
    {
      c = type == 0 ? expected >> 31 : expected & 1;
      if(type == 0) expected <<= 1;
      if(type == 1) expected >>= 1;
      if(type == 2) expected = (expected >> 1) | (expected & 0x80000000);
      if(type == 3) expected = (expected >> 1) | (c << 31);
    }
    for(variant = 0; variant < 3; variant++)
    {
      u32 result;
      reg[REG_C_FLAG] = carry;
      reg[REG_N_FLAG] = reg[REG_Z_FLAG] = reg[REG_V_FLAG] = 1;
      if(variant == 0)
      {
        switch(type) {
          case 0: result = execute_lsl_no_flags_reg(value, amount); break;
          case 1: result = execute_lsr_no_flags_reg(value, amount); break;
          case 2: result = execute_asr_no_flags_reg(value, amount); break;
          default: result = execute_ror_no_flags_reg(value, amount); break;
        }
      }
      else if(variant == 1)
      {
        switch(type) {
          case 0: result = execute_lsl_flags_reg(value, amount); break;
          case 1: result = execute_lsr_flags_reg(value, amount); break;
          case 2: result = execute_asr_flags_reg(value, amount); break;
          default: result = execute_ror_flags_reg(value, amount); break;
        }
      }
      else
      {
        switch(type) {
          case 0: result = execute_lsl_reg_op(value, amount); break;
          case 1: result = execute_lsr_reg_op(value, amount); break;
          case 2: result = execute_asr_reg_op(value, amount); break;
          default: result = execute_ror_reg_op(value, amount); break;
        }
      }
      check(result == expected && reg[REG_C_FLAG] == (variant ? c : carry) &&
       reg[REG_V_FLAG] == 1 &&
       reg[REG_N_FLAG] == (variant == 2 ? expected >> 31 : 1) &&
       reg[REG_Z_FLAG] == (variant == 2 ? expected == 0 : 1),
       "register shift", value, amount, carry);
    }
  }
}

static void cpsr_masked_write(void)
{
  u32 result;
  memset(reg, 0, sizeof(reg));
  memset(io_registers, 0, sizeof(io_registers));
  cpu_modes[0x13] = MODE_SUPERVISOR;
  reg[REG_CPSR] = 0x10;
  reg[REG_N_FLAG] = reg[REG_C_FLAG] = 1;
  result = execute_store_cpsr(0x13, 0xFF, 0x08000000);
  check(result == 0 && reg[REG_CPSR] == 0xA0000013 &&
   reg[REG_N_FLAG] == 1 && reg[REG_C_FLAG] == 1,
   "MSR control preserves live NZCV", 0, 0, 0);
  result = execute_store_cpsr(0x50000000, 0xF0000000, 0x08000000);
  check(result == 0 && reg[REG_CPSR] == 0x50000013 &&
   reg[REG_N_FLAG] == 0 && reg[REG_Z_FLAG] == 1 &&
   reg[REG_C_FLAG] == 0 && reg[REG_V_FLAG] == 1,
   "MSR flags replace live NZCV", 0, 0, 0);
  reg[REG_CPSR] = 0x93;
  reg[REG_N_FLAG] = reg[REG_C_FLAG] = 1;
  reg[REG_Z_FLAG] = reg[REG_V_FLAG] = 0;
  io_registers[REG_IE] = io_registers[REG_IF] = io_registers[REG_IME] = 1;
  result = execute_store_cpsr(0x13, 0xFF, 0x08000000);
  check(result == 0x18 && spsr[MODE_IRQ] == 0xA0000013,
   "MSR IRQ snapshots live NZCV", 0, 0, 0);
}

static void exception_return(void)
{
  u32 thumb, low, irq;
  cpu_modes[0x10] = MODE_USER;
  for(thumb = 0; thumb < 2; thumb++)
    for(low = 0; low < 4; low++)
      for(irq = 0; irq < 2; irq++)
      {
        u32 target = 0x08000100 | low;
        u32 aligned = target & (thumb ? ~1u : ~3u);
        u32 expected = irq ? 0x18 : aligned | thumb;
        u32 result;
        memset(reg, 0, sizeof(reg));
        reg[CPU_MODE] = MODE_SUPERVISOR;
        spsr[MODE_SUPERVISOR] = 0xA0000010 | (thumb << 5);
        io_registers[REG_IE] = io_registers[REG_IF] = io_registers[REG_IME] = irq;
        result = execute_spsr_restore(target);
        check(result == expected && reg[REG_N_FLAG] == 1 &&
         reg[REG_C_FLAG] == 1 &&
         (!irq || reg_mode[MODE_IRQ][6] == aligned + 4),
         "exception-return alignment/state", target, thumb, irq);
      }
}

static u32 smc_dispatches, smc_dispatch_pc, smc_dispatch_cycles;

void sh4_block_store_smc(u32 next_pc, u32 cycles)
{
  /* Production flushes the RAM cache (clearing the alert) and never
     returns; record the dispatch instead. */
  smc_dispatches++;
  smc_dispatch_pc = next_pc;
  smc_dispatch_cycles = cycles;
  sh4_block_store_smc_pending = 0;
}

static void block_store_smc(void)
{
  static u32 tagged_words[16384];
  u32 *const words = tagged_words + 8192; /* 32KB tag area first */
  u32 hit;
  memory_map_read[0x03000000 >> 15] = (u8 *)words;
  memory_map_write[0x03000000 >> 15] = (u8 *)words;
  for(hit = 0; hit < 4; hit++)
  {
    /* STMIA R0!,{R1-R3} with the first, middle, last or no stored word
       over translated code. */
    memset(tagged_words, 0, sizeof(tagged_words));
    memset(reg, 0, sizeof(reg));
    reg[CPU_MODE] = MODE_USER;
    reg[0] = 0x03000100;
    reg[1] = 0x11; reg[2] = 0x22; reg[3] = 0x33;
    if(hit < 3)
      tagged_words[0x40 + hit] = 0xFFFFFFFF;
    smc_dispatches = 0;
    sh4_block_store_smc_pending = 0;
    execute_arm_block_memory(0xE8A0000E, 0x03000200, 0x777);
    check(words[0x40] == 0x11 && words[0x41] == 0x22 && words[0x42] == 0x33 &&
     reg[0] == 0x0300010C, "STM stores and writes back before SMC dispatch",
     hit, 0, 0);
    check(hit < 3 ? (smc_dispatches == 1 && smc_dispatch_pc == 0x03000204 &&
     smc_dispatch_cycles == 0x777) : smc_dispatches == 0,
     "STM over translated code resumes at the next instruction", hit, 0, 0);
    check(sh4_block_store_smc_pending == 0, "STM consumes the SMC alert",
     hit, 0, 0);
  }

  /* LDM leaves an earlier alert for a consumer that can dispatch. */
  memset(tagged_words, 0, sizeof(tagged_words));
  memset(reg, 0, sizeof(reg));
  reg[0] = 0x03000100;
  smc_dispatches = 0;
  sh4_block_store_smc_pending = 1;
  execute_arm_block_memory(0xE8B0000E, 0x03000200, 0x777); /* LDMIA R0!,{R1-R3} */
  check(smc_dispatches == 0 && sh4_block_store_smc_pending == 1,
   "LDM leaves a pending SMC alert", 0, 0, 0);
  sh4_block_store_smc_pending = 0;
}

static void swi_entry(void)
{
  unsigned from, thumb;
  for(from = MODE_USER; from <= MODE_UNDEFINED; from++)
    for(thumb = 0; thumb < 2; thumb++)
    {
      u32 return_pc = 0x03007DDC - (thumb * 2);
      u32 old_cpsr = 0x1F | (thumb << 5);
      memset(reg, 0, sizeof(reg));
      memset(reg_mode, 0, sizeof(reg_mode));
      memset(spsr, 0, sizeof(spsr));
      reg[CPU_MODE] = from;
      reg[REG_CPSR] = old_cpsr;
      reg[REG_N_FLAG] = 1;
      reg[REG_C_FLAG] = 1;
      reg[REG_SP] = 0x4000; reg[REG_LR] = 0x4001;
      reg_mode[MODE_SUPERVISOR][5] = 0x5000;
      reg_mode[MODE_SUPERVISOR][6] = 0x5001;
      execute_swi(return_pc);
      /* LR_svc must hold the return address even for a SWI taken in
         Supervisor mode, where the mode switch leaves the bank alone. */
      check(reg[REG_LR] == return_pc, "SWI sets live LR_svc", from, thumb, 0);
      check(reg[CPU_MODE] == MODE_SUPERVISOR &&
       (reg[REG_CPSR] & 0x3F) == 0x13, "SWI enters ARM Supervisor",
       from, thumb, 0);
      check(spsr[MODE_SUPERVISOR] == (0xA0000000 | old_cpsr),
       "SWI snapshots CPSR into SPSR_svc", from, thumb, 0);
      if(from == MODE_SUPERVISOR)
        check(reg[REG_SP] == 0x4000, "SWI in SVC keeps SP", from, thumb, 0);
      else
        check(reg[REG_SP] == 0x5000 && reg_mode[from][5] == 0x4000 &&
         reg_mode[from][6] == 0x4001, "SWI banks caller SP/LR",
         from, thumb, 0);
    }
}

static void mode_banks(void)
{
  unsigned from, to, i;
  for(from = MODE_USER; from <= MODE_UNDEFINED; from++)
    for(to = MODE_USER; to <= MODE_UNDEFINED; to++)
    {
      memset(reg, 0, sizeof(reg));
      memset(reg_mode, 0, sizeof(reg_mode));
      reg[CPU_MODE] = from;
      for(i = 8; i < 13; i++)
      {
        reg[i] = 0x1000 + i;
        reg_mode[MODE_USER][i - 8] = 0x2000 + i;
        reg_mode[MODE_FIQ][i - 8] = 0x3000 + i;
      }
      reg[13] = 0x4000; reg[14] = 0x4001;
      reg_mode[to][5] = 0x5000; reg_mode[to][6] = 0x5001;
      set_cpu_mode(to);
      for(i = 8; i < 13; i++)
      {
        u32 want = from == to ? 0x1000 + i :
         to == MODE_FIQ ? 0x3000 + i :
         from == MODE_FIQ ? 0x2000 + i : 0x1000 + i;
        check(reg[i] == want, "mode switch R8-R12", from, to, i);
      }
      check(reg[CPU_MODE] == to &&
       reg[13] == (from == to ? 0x4000 : 0x5000) &&
       reg[14] == (from == to ? 0x4001 : 0x5001),
       "mode switch SP/LR", from, to, 0);
      if(from != to)
      {
        set_cpu_mode(from);
        for(i = 8; i < 15; i++)
          check(reg[i] == (i < 13 ? 0x1000 + i : 0x4000 + i - 13),
           "mode switch round trip", from, to, i);
      }
    }
}

static void block_transfers(void)
{
  static u32 tagged_words[16384];
  u32 *const words = tagged_words + 8192; /* 32KB tag area first */
  u32 up, pre, load, bank, i;
  memory_map_read[0x02000000 >> 15] = (u8 *)words;
  memory_map_write[0x02000000 >> 15] = (u8 *)words;
  for(up = 0; up < 2; up++)
    for(pre = 0; pre < 2; pre++)
      for(load = 0; load < 2; load++)
        for(bank = 0; bank < 2; bank++)
        {
          /* R8 and R12 are shared outside FIQ; R13 has a user bank. */
          const u32 list = (1u << 8) | (1u << 12) | (1u << 13);
          u32 start = 0x100 + (up ? (pre ? 4 : 0) : (pre ? -12 : -8));
          u32 opcode = 0xE8000000 | (pre << 24) | (up << 23) |
           (bank << 22) | ((!bank) << 21) | (load << 20) | list;
          memset(words, 0, 8192 * sizeof(u32));
          memset(reg, 0, sizeof(reg));
          memset(reg_mode, 0, sizeof(reg_mode));
          reg[CPU_MODE] = MODE_SUPERVISOR;
          reg[0] = 0x02000100;
          reg[8] = 0x88; reg[12] = 0xCC; reg[13] = 0xDD;
          reg_mode[MODE_USER][0] = 0xBAD8;
          reg_mode[MODE_USER][4] = 0xBADC;
          reg_mode[MODE_USER][5] = 0xAA;
          if(load)
            for(i = 0; i < 3; i++) words[start / 4 + i] = 0x1000 + i;
          execute_arm_block_memory(opcode, 0x08000000, 0x1234);
          if(load)
          {
            check(reg[8] == 0x1000 && reg[12] == 0x1001 &&
             (bank ? reg_mode[MODE_USER][5] == 0x1002 && reg[13] == 0xDD :
              reg[13] == 0x1002), "LDM register bank", up, pre, bank);
          }
          else
            check(words[start / 4] == 0x88 && words[start / 4 + 1] == 0xCC &&
             words[start / 4 + 2] == (bank ? 0xAA : 0xDD),
             "STM register bank", up, pre, bank);
          check(reg[0] == (bank ? 0x02000100 :
           up ? 0x0200010C : 0x020000F4), "block writeback", up, pre, bank);
        }
  for(up = 0; up < 2; up++)
    for(pre = 0; pre < 2; pre++)
      for(bank = 0; bank < 2; bank++)
        for(i = 0; i < 4; i++)
        {
          /* LDM with PC: S=0 stays ARM; S=1 restores CPSR and current bank. */
          u32 start = 0x100 + (up ? (pre ? 4 : 0) : (pre ? -12 : -8));
          u32 thumb = i & 1;
          u32 target = 0x08000100 | i;
          u32 expected = bank ? (target & (thumb ? ~1u : ~3u)) | thumb :
           target & ~3u;
          u32 opcode = 0xE830C100 | (pre << 24) | (up << 23) | (bank << 22);
          memset(reg, 0, sizeof(reg));
          memset(io_registers, 0, sizeof(io_registers));
          reg[CPU_MODE] = MODE_SUPERVISOR;
          reg[REG_CPSR] = 0x93;
          cpu_modes[0x13] = MODE_SUPERVISOR;
          spsr[MODE_SUPERVISOR] = 0xA0000013 | (thumb << 5);
          reg[0] = 0x02000100;
          words[start / 4] = 0x1234;
          words[start / 4 + 1] = 0x5678;
          words[start / 4 + 2] = target;
          execute_arm_block_memory(opcode, 0x08000000, 0x1234);
          check(reg[REG_PC] == expected && reg[8] == 0x1234 && reg[14] == 0x5678 &&
           reg[REG_CPSR] == (bank ? spsr[MODE_SUPERVISOR] : 0x93),
           "LDM PC/exception return", up, pre, bank);
          check(reg[0] == (up ? 0x0200010C : 0x020000F4),
           "LDM PC writeback", up, pre, bank);
        }
}

static void fiq_and_ldm_returns(void)
{
  static u32 tagged_words[16384];
  u32 *const words = tagged_words + 8192; /* 32KB tag area first */
  u32 i, thumb, irq;
  memory_map_read[0x02000000 >> 15] = (u8 *)words;
  memory_map_write[0x02000000 >> 15] = (u8 *)words;
  memset(reg, 0, sizeof(reg));
  memset(reg_mode, 0, sizeof(reg_mode));
  reg[CPU_MODE] = MODE_IRQ;
  reg[0] = 0x02000100;
  for(i = 8; i < 13; i++)
  {
    reg[i] = 0x1000 + i;
    reg_mode[MODE_FIQ][i - 8] = 0x2000 + i;
  }
  reg_mode[MODE_USER][5] = 0x7000;
  reg_mode[MODE_USER][6] = 0x7001;
  set_cpu_mode(MODE_FIQ);
  execute_arm_block_memory(0xE8C07F00, 0x08000000, 0x1234); /* STMIA R0,{R8-R14}^ */
  for(i = 0; i < 7; i++)
  {
    check(words[64 + i] == (i < 5 ? 0x1008 + i : 0x7000 + i - 5),
     "FIQ stores shared user bank", i, 0, 0);
    words[64 + i] = 0x8000 + i;
  }
  execute_arm_block_memory(0xE8D07F00, 0x08000000, 0x1234); /* LDMIA R0,{R8-R14}^ */
  for(i = 8; i < 13; i++)
    check(reg[i] == 0x2000 + i, "FIQ load preserves FIQ bank", i, 0, 0);
  check(reg_mode[MODE_USER][5] == 0x8005 && reg_mode[MODE_USER][6] == 0x8006,
   "FIQ user SP/LR loads", 0, 0, 0);
  set_cpu_mode(MODE_IRQ);
  for(i = 8; i < 13; i++)
    check(reg[i] == 0x8000 + i - 8, "FIQ return restores updated shared bank", i, 0, 0);

  for(thumb = 0; thumb < 2; thumb++)
    for(irq = 0; irq < 2; irq++)
    {
      u32 restored = 0xA0000010 | (thumb << 5);
      memset(reg, 0, sizeof(reg));
      memset(reg_mode, 0, sizeof(reg_mode));
      reg[CPU_MODE] = MODE_SUPERVISOR;
      reg[REG_CPSR] = 0x93;
      reg[REG_SP] = 0x02000100;
      reg_mode[MODE_USER][5] = 0x02001000;
      reg_mode[MODE_USER][6] = 0xABCD;
      cpu_modes[0x10] = MODE_USER;
      spsr[MODE_SUPERVISOR] = restored;
      words[64] = 0x1234; words[65] = 0x5678; words[66] = 0x08000203;
      io_registers[REG_IE] = io_registers[REG_IF] = io_registers[REG_IME] = irq;
      execute_arm_block_memory(0xE8FDC001, 0x08000000, 0x1234);
      check(reg_mode[MODE_SUPERVISOR][5] == 0x0200010C &&
       reg_mode[MODE_SUPERVISOR][6] == 0x5678 && reg[0] == 0x1234 &&
       reg_mode[MODE_USER][5] == 0x02001000 && reg_mode[MODE_USER][6] == 0xABCD,
       "LDM return writes old bank before switching", thumb, irq, 0);
      check(reg[CPU_MODE] == (irq ? MODE_IRQ : MODE_USER) &&
       reg[REG_PC] == (irq ? 0x18 : thumb ? 0x08000203 : 0x08000200) &&
       (irq ? spsr[MODE_IRQ] == restored &&
        reg_mode[MODE_IRQ][6] == (thumb ? 0x08000206 : 0x08000204) :
        reg[REG_CPSR] == restored && reg[REG_SP] == 0x02001000 && reg[REG_LR] == 0xABCD),
       "LDM return state/IRQ", thumb, irq, 0);
    }
}

int main(void)
{
  static const u32 edge[] = {0, 1, 0x7fffffff, 0x80000000,
   0xfffffffe, 0xffffffff, 0x55555555, 0xaaaaaaaa};
  unsigned a, b, carry;
  u32 random = 0x12345678;
  for(a = 0; a < sizeof(edge) / sizeof(edge[0]); a++)
    for(carry = 0; carry < 2; carry++)
    {
      for(b = 0; b < sizeof(edge) / sizeof(edge[0]); b++)
        arithmetic(edge[a], edge[b], carry);
      for(b = 0; b < 512; b++) shifts(edge[a], b, carry);
      shifts(edge[a], 0xffffffff, carry);
    }
  for(a = 0; a < 10000; a++)
  {
    u32 lhs = random;
    random = random * 1664525u + 1013904223u;
    arithmetic(lhs, random, a & 1);
  }
  cpsr_masked_write();
  exception_return();
  swi_entry();
  block_transfers();
  block_store_smc();
  mode_banks();
  fiq_and_ldm_returns();
  printf("SH-4 helpers: %u checks, %u failures\n", checks, failures);
  return failures != 0;
}
