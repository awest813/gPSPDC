/* qemu-sh4 execution harness for the SH-4 dynarec (see harness.c).
 * gameplaySP, Copyright (C) 2006 Exophase; GPL-2.0-or-later, see LICENSE.
 */
#ifndef SH4_EXEC_HARNESS_H
#define SH4_EXEC_HARNESS_H

#define HARNESS_ROM_BASE 0x08000000u
#define HARNESS_ROM_SIZE (256 * 1024)

/* Unused IO offset: a write raises the written IRQ bits the way a DMA
   completion does in memory.c, returning CPU_ALERT_IRQ when taken. */
#define HARNESS_IO_RAISE_IRQ 0x3F0

typedef enum
{
  HARNESS_DYNAREC,
  HARNESS_INTERPRETER
} harness_engine;

typedef enum
{
  HARNESS_RUN_OK = 1,        /* reached the exit PC */
  HARNESS_RUN_TIMEOUT,       /* exceeded harness_update_limit */
  HARNESS_RUN_FATAL,         /* gpsp_dynarec_fatal_error or fell out */
  HARNESS_RUN_INCOHERENT     /* code ran before write-back/invalidate */
} harness_status;

typedef struct
{
  u32 reg[64];
  u32 reg_mode[7][7];
  u32 spsr[6];
  u16 io[0x200];
  u8 iwram[0x8000];
  u8 ewram_lo[0x8000];
} harness_state;

extern u8 harness_rom[HARNESS_ROM_SIZE];
extern u32 harness_update_calls;
extern u32 harness_update_limit;
extern u32 harness_halt_wakeups;
extern u32 harness_icache_flushes;
extern u32 harness_dcache_flushes;
extern u32 harness_coherence_checks;
extern char harness_fatal_message[256];

void harness_init(void);
void harness_reset(void);
void harness_write32(u32 address, u32 value);
void harness_write16(u32 address, u16 value);
u32 harness_read32(u32 address);
void harness_save(harness_state *s);
void harness_load(const harness_state *s);
harness_status harness_run(harness_engine engine, u32 exit_pc);
void harness_check_coherence(const u8 *target);

#endif
