/* Internal register-bank switching, shared with the host behavior tests.
 * gameplaySP, Copyright (C) 2006 Exophase; GPL-2.0-or-later, see LICENSE.
 */
#ifndef GPSP_CPU_MODE_H
#define GPSP_CPU_MODE_H

static inline void cpu_switch_mode(u32 *reg, u32 reg_mode[7][7],
 cpu_mode_type new_mode)
{
  u32 i;
  cpu_mode_type cpu_mode = reg[CPU_MODE];

  if(cpu_mode == new_mode)
    return;

  /* SP/LR have a separate bank in every exception mode. */
  reg_mode[cpu_mode][5] = reg[REG_SP];
  reg_mode[cpu_mode][6] = reg[REG_LR];

  /* All non-FIQ modes share R8-R12. Keep that shared bank in MODE_USER,
     regardless of which exception mode FIQ interrupted. */
  if(cpu_mode == MODE_FIQ || new_mode == MODE_FIQ)
  {
    u32 old_bank = cpu_mode == MODE_FIQ ? MODE_FIQ : MODE_USER;
    u32 new_bank = new_mode == MODE_FIQ ? MODE_FIQ : MODE_USER;
    for(i = 8; i < 13; i++)
    {
      reg_mode[old_bank][i - 8] = reg[i];
      reg[i] = reg_mode[new_bank][i - 8];
    }
  }

  reg[REG_SP] = reg_mode[new_mode][5];
  reg[REG_LR] = reg_mode[new_mode][6];
  reg[CPU_MODE] = new_mode;
}

#endif
