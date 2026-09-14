/* Register-relative loads and stores read and write this small memory, based
   at SIM_MEMORY_BASE; any other address fails the simulation. */
#define SIM_MEMORY_BASE 0x00100000
static u32 sim_memory[64];

static int sim_memory_index(u32 address, u32 *index)
{
  if((address & 3) || (address < SIM_MEMORY_BASE) ||
   (address - SIM_MEMORY_BASE) / 4 >= sizeof(sim_memory) / sizeof(sim_memory[0]))
    return 0;
  *index = (address - SIM_MEMORY_BASE) / 4;
  return 1;
}

/* Deliberately small test interpreter for the emitter sequences below.
   Unsupported instructions, invalid targets, and non-NOP delay slots fail.
   It is not an SH-4 CPU emulator or a timing model. */
static int simulate_emission(u32 regs[16], u32 t)
{
  u32 base = (u32)(uintptr_t)code_buffer;
  u32 pc = base;
  u32 bytes = (u32)((u8 *)translation_ptr - (u8 *)code_buffer);
  unsigned steps;
  for(steps = 0; steps < 256; steps++)
  {
    u32 offset = pc - base, next = pc + 2;
    u16 op;
    u32 n, m;
    if(offset == bytes) return 1;
    if(offset >= bytes || (offset & 1)) return 0;
    op = code_buffer[offset / 2];
    n = (op >> 8) & 15;
    m = (op >> 4) & 15;
    if(op == 0x0009) { /* nop */ }
    else if((op & 0xF000) == 0xE000)
      regs[n] = (u32)(int32_t)(int8_t)op;
    else if((op & 0xF000) == 0x7000)
      regs[n] += (u32)(int32_t)(int8_t)op;
    else if((op & 0xF0FF) == 0x4000) regs[n] <<= 1;
    else if((op & 0xF0FF) == 0x4001) regs[n] >>= 1;
    else if((op & 0xF0FF) == 0x4021) regs[n] = (u32)((int32_t)regs[n] >> 1);
    else if((op & 0xF0FF) == 0x4004) regs[n] = (regs[n] << 1) | (regs[n] >> 31);
    else if((op & 0xF0FF) == 0x4005) regs[n] = (regs[n] >> 1) | (regs[n] << 31);
    else if((op & 0xF0FF) == 0x4008) regs[n] <<= 2;
    else if((op & 0xF0FF) == 0x4009) regs[n] >>= 2;
    else if((op & 0xF0FF) == 0x4018) regs[n] <<= 8;
    else if((op & 0xF0FF) == 0x4019) regs[n] >>= 8;
    else if((op & 0xF0FF) == 0x4028) regs[n] <<= 16;
    else if((op & 0xF0FF) == 0x4029) regs[n] >>= 16;
    else if((op & 0xF00F) == 0x400C || (op & 0xF00F) == 0x400D)
    {
      /* shad/shld: non-negative Rm shifts left by Rm & 31; negative Rm
         shifts right by 32 - (Rm & 31), where Rm & 31 == 0 means 32. */
      int arithmetic = (op & 0xF00F) == 0x400C;
      if((int32_t)regs[m] >= 0)
        regs[n] <<= regs[m] & 31;
      else if((regs[m] & 31) == 0)
        regs[n] = arithmetic ? (u32)((int32_t)regs[n] >> 31) : 0;
      else if(arithmetic)
        regs[n] = (u32)((int32_t)regs[n] >> ((~regs[m] & 31) + 1));
      else
        regs[n] >>= (~regs[m] & 31) + 1;
    }
    else if((op & 0xF00F) == 0x600C) regs[n] = regs[m] & 255;
    else if((op & 0xF00F) == 0x6003) regs[n] = regs[m];
    else if((op & 0xF00F) == 0x6007) regs[n] = ~regs[m];
    else if((op & 0xF00F) == 0x300C) regs[n] += regs[m];
    else if((op & 0xF00F) == 0x3008) regs[n] -= regs[m];
    else if((op & 0xF00F) == 0x2009) regs[n] &= regs[m];
    else if((op & 0xF00F) == 0x200A) regs[n] ^= regs[m];
    else if((op & 0xF00F) == 0x200B) regs[n] |= regs[m];
    else if((op & 0xFF00) == 0x8900 || (op & 0xFF00) == 0x8B00)
    {
      if(t == ((op & 0xFF00) == 0x8900))
        next = pc + 4 + 2 * (int32_t)(int8_t)op;
    }
    else if((op & 0xF000) == 0xA000 || (op & 0xF0FF) == 0x402B)
    {
      /* Both bra and jmp execute the next instruction before branching. */
      if(offset + 4 > bytes || code_buffer[offset / 2 + 1] != 0x0009)
        return 0;
      if((op & 0xF000) == 0xA000)
      {
        s32 disp = op & 0x0FFF;
        if(disp & 0x800) disp -= 4096;
        next = pc + 4 + disp * 2;
      }
      else next = regs[n];
    }
    else if((op & 0xF000) == 0x5000)
    {
      /* mov.l @(disp,Rm),Rn */
      u32 index;
      if(!sim_memory_index(regs[m] + (op & 15) * 4, &index)) return 0;
      regs[n] = sim_memory[index];
    }
    else if((op & 0xF000) == 0x1000)
    {
      /* mov.l Rm,@(disp,Rn) */
      u32 index;
      if(!sim_memory_index(regs[n] + (op & 15) * 4, &index)) return 0;
      sim_memory[index] = regs[m];
    }
    else if((op & 0xF000) == 0xD000)
    {
      u32 literal = ((pc + 4) & ~3u) + (op & 255) * 4 - base;
      if(literal > bytes || bytes - literal < 4) return 0;
      memcpy(&regs[n], (u8 *)code_buffer + literal, 4);
    }
    else return 0;
    pc = next;
  }
  return 0;
}

static void test_executed_emission(void)
{
  u32 regs[16], seed = 0x12345678, iteration, r, shape, t, align;
  static const u32 edges[] = {0x100, 0x7fff, 0x8000, 0xffff,
   0x10000, 0x7fffff, 0x800000, 0xffffff, 0x1000000, 0x80000000,
   0xffffff7f};
  int before = failures;
  for(iteration = 0; iteration < 2048; iteration++)
  {
    u32 value = iteration < 512 ? iteration - 256 :
     iteration < 512 + sizeof(edges) / sizeof(edges[0]) ?
     edges[iteration - 512] : seed;
    seed = seed * 1664525u + 1013904223u;
    for(r = 0; r < 16; r++)
    {
      memset(regs, 0x5A, sizeof(regs));
      reset_buffer();
      SH4_EMIT_LOAD_IMM(r, value);
      if(!simulate_emission(regs, 0) || regs[r] != value)
        failures++;
      {
        u32 other;
        for(other = 0; other < 16; other++)
          if(other != r && regs[other] != 0x5A5A5A5A) failures++;
      }
      if(value == 0x100 && translation_ptr - code_buffer != 2) failures++;
      if(value == 0x10000 && translation_ptr - code_buffer != 3) failures++;
    }
  }
  for(shape = 0; shape < 4; shape++)
    for(t = 0; t < 2; t++)
      for(align = 0; align < 2; align++)
      {
        u8 *branch;
        memset(regs, 0, sizeof(regs));
        reset_buffer();
        if(align) SH4_EMIT_NOP();
        if(shape == 0) SH4_EMIT_COND_SKIP_T(branch);
        if(shape == 1) SH4_EMIT_COND_SKIP_F(branch);
        if(shape == 2) SH4_EMIT_COND_SKIP_T_FAR(branch);
        if(shape == 3) SH4_EMIT_COND_SKIP_F_FAR(branch);
        SH4_EMIT_MOVI(sh4_reg_r4, 1);
        if(shape < 2)
          generate_branch_patch_conditional(branch, translation_ptr);
        else
          /* Exercise the literal veneer even though this target is near. */
          *sh4_long_branch_literal(branch) = (u32)(uintptr_t)translation_ptr;
        if(!simulate_emission(regs, t) || regs[4] != (t != !(shape & 1)))
          failures++;
      }
  printf("executed immediate/conditional sequences: %s\n",
   failures == before ? "ok" : "FAILED");
}

/* Inline data processing, executed against the helpers' definitions in
   dc/sh4_helpers.c. r4 holds the second operand (rm), r5 holds rn, and r8
   points at reg[16]; r1 and r2 are scratch. */
static void test_executed_inline_alu(void)
{
  static const u32 values[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
   0x12345678, 0xDEADBEEF};
  static const char *names[] = {"add", "sub", "rsb", "and", "orr", "eor",
   "bic", "adc", "sbc", "rsc", "mov", "mvn"};
  const u32 count = sizeof(values) / sizeof(values[0]);
  const u32 flag_base = SIM_MEMORY_BASE + 16 * 4;
  u32 op, a, b, carry, r;
  int before = failures;

  for(op = 0; op < sizeof(names) / sizeof(names[0]); op++)
    for(a = 0; a < count; a++)
      for(b = 0; b < count; b++)
        for(carry = 0; carry < 2; carry++)
        {
          u32 regs[16], rm = values[a], rn = values[b], expected;

          memset(regs, 0x5A, sizeof(regs));
          memset(sim_memory, 0, sizeof(sim_memory));
          sim_memory[REG_C_FLAG] = carry;
          regs[4] = rm;
          regs[5] = rn;
          regs[8] = flag_base;
          reset_buffer();

          switch(op)
          {
            case 0: arm_data_proc_op_add(); expected = rn + rm; break;
            case 1: arm_data_proc_op_sub(); expected = rn - rm; break;
            case 2: arm_data_proc_op_rsb(); expected = rm - rn; break;
            case 3: arm_data_proc_op_and(); expected = rn & rm; break;
            case 4: arm_data_proc_op_orr(); expected = rn | rm; break;
            case 5: arm_data_proc_op_eor(); expected = rn ^ rm; break;
            case 6: arm_data_proc_op_bic(); expected = rn & ~rm; break;
            case 7: arm_data_proc_op_adc(); expected = rn + rm + carry; break;
            case 8:
              arm_data_proc_op_sbc();
              expected = rn - rm - (carry ^ 1);
              break;
            case 9:
              arm_data_proc_op_rsc();
              expected = rm + carry - 1 - rn;
              break;
            case 10: arm_data_proc_op_mov(); expected = rm; break;
            default: arm_data_proc_unary_op_mvn(); expected = ~rm; break;
          }

          if(!simulate_emission(regs, 0) || regs[0] != expected)
          {
            if(failures++ - before < 8)
              printf("inline %s rm=%08x rn=%08x c=%u: got %08x, expected %08x\n",
               names[op], rm, rn, carry, regs[0], expected);
          }

          for(r = 3; r < 16; r++)
          {
            u32 kept = (r == 4) ? rm : (r == 5) ? rn : (r == 8) ? flag_base :
             0x5A5A5A5A;
            if(regs[r] != kept)
              failures++;
          }
        }

  printf("executed inline data processing: %s\n",
   failures == before ? "ok" : "FAILED");
}

/* Every constant shift the translator emits, executed against C reference
   results for each amount. r1 and r2 are the emitter's scratch registers;
   everything else must survive. */
static void test_executed_constant_shifts(void)
{
  static const u32 values[] = {0x00000001, 0x80000000, 0xDEADBEEF,
   0x7FFFFFFF, 0xFFFFFFFF, 0x12345678};
  static const char *names[] = {"LSL", "LSR", "ASR", "ROR"};
  u32 longest[4] = {0, 0, 0, 0};
  u32 kind, amount, v, r;
  int before = failures;

  for(kind = 0; kind < 4; kind++)
    for(amount = 0; amount < 32; amount++)
      for(v = 0; v < sizeof(values) / sizeof(values[0]); v++)
      {
        u32 regs[16], value = values[v], expected = value, words;

        if(kind == 3 && amount == 0)
          continue; /* ROR #0 is RRX; the translator never emits it here. */

        memset(regs, 0x5A, sizeof(regs));
        regs[4] = value;
        reset_buffer();

        switch(kind)
        {
          case 0:
            generate_shift_left(a0, amount);
            if(amount) expected = value << amount;
            break;
          case 1:
            generate_shift_right(a0, amount);
            if(amount) expected = value >> amount;
            break;
          case 2:
            generate_shift_right_arithmetic(a0, amount);
            if(amount) expected = (u32)((int32_t)value >> amount);
            break;
          default:
            generate_rotate_right(a0, amount);
            expected = (value >> amount) | (value << (32 - amount));
            break;
        }

        words = (u32)(translation_ptr - code_buffer);
        if(words > longest[kind])
          longest[kind] = words;

        if(!simulate_emission(regs, 0) || regs[4] != expected)
        {
          if(failures++ - before < 8)
            printf("%s #%u of %08x: got %08x, expected %08x\n", names[kind],
             amount, value, regs[4], expected);
        }

        for(r = 0; r < 16; r++)
          if(r != 1 && r != 2 && r != 4 && regs[r] != 0x5A5A5A5A)
            failures++;
      }

  /* Previously one instruction per bit: up to 31 per shift. */
  if(longest[0] > 2 || longest[1] > 2 || longest[2] > 2 || longest[3] > 6)
  {
    printf("constant shift lengths LSL %u LSR %u ASR %u ROR %u exceed 2/2/2/6\n",
     longest[0], longest[1], longest[2], longest[3]);
    failures++;
  }

  printf("executed constant shifts: %s\n", failures == before ? "ok" : "FAILED");
}
