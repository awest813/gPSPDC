/* qemu-sh4 execution harness for the SH-4 dynarec.
 *
 * Links the production translator (cpu_threaded.c, dc/sh4_helpers.c,
 * dc/sh4_stub.c) and the production interpreter (cpu.c) into one SH-4
 * Linux binary.  This file supplies the rest of the emulator: a small GBA
 * memory system laid out exactly like memory.c (translation tags below each
 * RAM page), an update_gba() that ends a run when the PC reaches a
 * sentinel, and KOS cache-maintenance shims that model the SH-4's split
 * instruction/operand caches.
 *
 * qemu-user keeps self-modifying code coherent on its own, so a missing
 * cache flush would never show up as a wrong result.  The shims instead
 * keep a shadow of what the instruction cache may fetch, updated only by
 * icache_flush_range(); every dispatch into translated code checks that
 * all emitted code matches that shadow.
 *
 * gameplaySP, Copyright (C) 2006 Exophase; GPL-2.0-or-later, see LICENSE.
 */
#include <setjmp.h>
#include <stdint.h>
#include <sys/mman.h>
#include "common.h"
#include "harness.h"

/* ---- memory system (layout mirrors memory.c) ------------------------ */

u8 *memory_map_read[8 * 1024];
u8 *memory_map_write[8 * 1024];
u32 reg[64];
u16 io_registers[1024 * 16];
u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u8 bios_rom[1024 * 32];
u32 bios_read_protect;
u8 *write_mem_ptr;
u8 harness_rom[HARNESS_ROM_SIZE];
static u8 harness_open_bus_page[0x8000];

u32 waitstate_cycles_sequential[16][3] =
{
  { 1, 1, 1 }, { 1, 1, 1 }, { 3, 3, 6 }, { 1, 1, 1 },
  { 1, 1, 1 }, { 1, 1, 2 }, { 1, 1, 2 }, { 1, 1, 2 },
  { 3, 3, 6 }, { 3, 3, 6 }, { 5, 5, 9 }, { 5, 5, 9 },
  { 9, 9, 17 }, { 9, 9, 17 }, { 1, 1, 1 }, { 1, 1, 1 }
};

/* ---- globals the production objects expect from main.c et al. ------- */

debug_state current_debug_state = RUN;
u32 breakpoint_value;
u32 global_cycles_per_instruction = 1;
u32 flush_ram_count;
u32 sound_initialized;
SDL_mutex *sound_mutex;

void SDL_PauseAudio(int pause_on) { (void)pause_on; }
int SDL_LockMutex(SDL_mutex *mutex) { (void)mutex; return 0; }
int SDL_UnlockMutex(SDL_mutex *mutex) { (void)mutex; return 0; }
void quit() { exit(2); }
u32 cheat_pc_is_hook(u32 pc) { (void)pc; return 0; }
void process_cheats() { }

u8 *load_gamepak_page(u32 physical_index)
{
  (void)physical_index;
  return harness_open_bus_page;
}

static u8 *harness_backing(u32 address)
{
  u8 *map;

  if(address & 0xF0000000)
    return NULL;
  map = memory_map_read[address >> 15];
  return map ? map + (address & 0x7FFF) : NULL;
}

static u32 harness_backing_read(u32 address, u32 size)
{
  u8 *p = harness_backing(address);
  u32 value = 0;

  if(p)
    memcpy(&value, p, size);
  return value;
}

u8 function_cc read_memory8(u32 address)
{
  return harness_backing_read(address, 1);
}

u32 function_cc read_memory16(u32 address)
{
  /* memory.c: unaligned halfword reads rotate the aligned value. */
  u32 value = harness_backing_read(address & ~1u, 2);

  if(address & 1)
    ror(value, value, 8);
  return value;
}

u16 function_cc read_memory16_signed(u32 address)
{
  if(address & 1)
    return (s8)read_memory8(address);
  return harness_backing_read(address, 2);
}

u32 function_cc read_memory32(u32 address)
{
  u32 value = harness_backing_read(address & ~3u, 4);
  u32 rotate = (address & 3) * 8;

  if(rotate)
    ror(value, value, rotate);
  return value;
}

/* IO writes.  Only a few registers have side effects; one unused register
   raises an IRQ the way a DMA completion does in memory.c, so tests can
   drive the store-alert IRQ path deterministically. */
static cpu_alert_type harness_write_io(u32 offset, u32 value, u32 size)
{
  if(offset == HARNESS_IO_RAISE_IRQ)
  {
    raise_interrupt((irq_type)(value & 0x3FFF));
    return (reg[CHANGED_PC_STATUS] != 0) ? CPU_ALERT_IRQ : CPU_ALERT_NONE;
  }

  if(offset == 0x202)
  {
    io_registers[REG_IF] &= ~value;
    return CPU_ALERT_NONE;
  }

  /* HALTCNT: byte 0x301, or the high byte of a halfword write to 0x300. */
  if((size == 1 && offset == 0x301) || (size == 2 && offset == 0x300))
  {
    u32 stop = (size == 2) ? ((value >> 8) & 0x01) : (value & 0x01);
    reg[CPU_HALT_STATE] = stop ? CPU_STOP : CPU_HALT;
    return CPU_ALERT_HALT;
  }

  if(offset < 0x400)
    memcpy((u8 *)io_registers + offset, &value, size);
  return CPU_ALERT_NONE;
}

static cpu_alert_type harness_write(u32 address, u32 value, u32 size)
{
  u8 *map;

  if(address & 0xF0000000)
    return CPU_ALERT_NONE;
  if((address >> 24) == 0x04)
    return harness_write_io(address & 0x3FF, value, size);

  /* Mapped RAM normally takes the callers' fast paths; a misaligned or
     slow-path write lands here and must still see the SMC tags. */
  map = memory_map_write[address >> 15];
  if(map)
  {
    u32 offset = address & 0x7FFF;
    memcpy(map + offset, &value, size);
    if(map[offset - 0x8000] != 0)
      return CPU_ALERT_SMC;
  }
  return CPU_ALERT_NONE;
}

cpu_alert_type function_cc write_memory8(u32 address, u8 value)
{
  return harness_write(address, value, 1);
}

cpu_alert_type function_cc write_memory16(u32 address, u16 value)
{
  return harness_write(address & ~1u, value, 2);
}

cpu_alert_type function_cc write_memory32(u32 address, u32 value)
{
  return harness_write(address & ~3u, value, 4);
}

/* ---- run control ------------------------------------------------------ */

u32 harness_exit_pc;
u32 harness_update_calls;
u32 harness_update_limit = 100000;
u32 harness_halt_wakeups;
char harness_fatal_message[256];
static jmp_buf harness_exit_jmp;
static int harness_running;

extern u32 translation_recursion_level;
extern u32 translation_redo_attempts;

u32 update_gba()
{
  harness_update_calls++;

  if(reg[REG_PC] == harness_exit_pc)
    longjmp(harness_exit_jmp, HARNESS_RUN_OK);
  if(harness_update_calls > harness_update_limit)
    longjmp(harness_exit_jmp, HARNESS_RUN_TIMEOUT);

  if(reg[CPU_HALT_STATE] != CPU_ACTIVE)
  {
    harness_halt_wakeups++;
    reg[CPU_HALT_STATE] = CPU_ACTIVE;
  }

  return 64;
}

void gpsp_dynarec_fatal_error(const char *detail)
{
  snprintf(harness_fatal_message, sizeof(harness_fatal_message), "%s",
   detail);
  if(harness_running)
    longjmp(harness_exit_jmp, HARNESS_RUN_FATAL);
  fprintf(stderr, "dynarec fatal outside a run: %s\n", detail);
  exit(3);
}

/* ---- cache-coherence model ------------------------------------------- */

typedef struct
{
  u8 *cache;
  u32 size;
  u8 **ptr;
  u8 *shadow;
  const char *name;
} harness_cache;

static u8 shadow_rom[ROM_TRANSLATION_CACHE_SIZE];
static u8 shadow_ram[RAM_TRANSLATION_CACHE_SIZE];
static u8 shadow_bios[BIOS_TRANSLATION_CACHE_SIZE];

static harness_cache harness_caches[3] =
{
  { rom_translation_cache, ROM_TRANSLATION_CACHE_SIZE, &rom_translation_ptr,
    shadow_rom, "rom" },
  { ram_translation_cache, RAM_TRANSLATION_CACHE_SIZE, &ram_translation_ptr,
    shadow_ram, "ram" },
  { bios_translation_cache, BIOS_TRANSLATION_CACHE_SIZE,
    &bios_translation_ptr, shadow_bios, "bios" }
};

u32 harness_icache_flushes;
u32 harness_dcache_flushes;
u32 harness_coherence_checks;

void dcache_flush_range(uint32_t start, uint32_t count)
{
  (void)start;
  (void)count;
  harness_dcache_flushes++;
}

/* KOS icache_flush_range writes back (ocbwb) and invalidates every 32-byte
   line from start rounded down to start + count rounded up. */
void icache_flush_range(uint32_t start, uint32_t count)
{
  u32 line_start = start & ~31u;
  u32 line_end = (start + count + 31) & ~31u;
  u32 i;

  harness_icache_flushes++;
  for(i = 0; i < 3; i++)
  {
    harness_cache *c = harness_caches + i;
    u32 base = (u32)c->cache;
    u32 lo = line_start > base ? line_start : base;
    u32 hi = line_end < base + c->size ? line_end : base + c->size;

    if(lo < hi)
      memcpy(c->shadow + (lo - base), (u8 *)lo, hi - lo);
  }
}

/* ROM hash headers (pc, next) share the ROM cache with code; the next
   field is legitimately rewritten after its block was flushed. */
static int harness_is_rom_header_byte(u32 offset)
{
  u32 i;

  for(i = 0; i < ROM_BRANCH_HASH_SIZE; i++)
  {
    u32 *entry = rom_branch_hash[i];
    while(entry)
    {
      u32 header = (u8 *)entry - rom_translation_cache;
      if(offset >= header && offset < header + 8)
        return 1;
      entry = (u32 *)entry[1];
    }
  }
  return 0;
}

static u8 *harness_last_ptr[3];
static u32 harness_last_flush_count = 0xFFFFFFFF;

void harness_check_coherence(const u8 *target)
{
  u32 i, offset;
  int changed = (harness_last_flush_count != harness_icache_flushes);

  for(i = 0; i < 3; i++)
    changed |= (harness_last_ptr[i] != *harness_caches[i].ptr);
  if(!changed)
    return;

  harness_coherence_checks++;
  for(i = 0; i < 3; i++)
  {
    harness_cache *c = harness_caches + i;
    u32 used = *c->ptr - c->cache;

    for(offset = 0; offset < used; offset++)
    {
      if(c->cache[offset] == c->shadow[offset])
        continue;
      if(i == 0 && harness_is_rom_header_byte(offset))
        continue;

      snprintf(harness_fatal_message, sizeof(harness_fatal_message),
       "%s cache offset %x executed before write-back/invalidate "
       "(dispatch to %p)", c->name, offset, (const void *)target);
      longjmp(harness_exit_jmp, HARNESS_RUN_INCOHERENT);
    }
    harness_last_ptr[i] = *c->ptr;
  }
  harness_last_flush_count = harness_icache_flushes;
}

/* ---- setup ------------------------------------------------------------ */

#define map_pages(type, start, end, expr)                                    \
  for(page = (start) >> 15; page < ((end) >> 15); page++)                    \
    memory_map_##type[page] = (expr)

static void harness_make_executable(void *start, u32 size)
{
  unsigned long page = 4096;
  unsigned long lo = (unsigned long)start & ~(page - 1);
  unsigned long hi = ((unsigned long)start + size + page - 1) & ~(page - 1);

  if(mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
  {
    perror("mprotect translation cache");
    exit(4);
  }
}

void harness_init(void)
{
  u32 page;

  harness_make_executable(rom_translation_cache, ROM_TRANSLATION_CACHE_SIZE);
  harness_make_executable(ram_translation_cache, RAM_TRANSLATION_CACHE_SIZE);
  harness_make_executable(bios_translation_cache,
   BIOS_TRANSLATION_CACHE_SIZE);

  memset(memory_map_read, 0, sizeof(memory_map_read));
  memset(memory_map_write, 0, sizeof(memory_map_write));
  map_pages(read, 0x0000000, 0x1000000, bios_rom);
  map_pages(read, 0x2000000, 0x3000000,
   ewram + ((page % 8) * 0x10000) + 0x8000);
  map_pages(write, 0x2000000, 0x3000000,
   ewram + ((page % 8) * 0x10000) + 0x8000);
  map_pages(read, 0x3000000, 0x4000000, iwram + 0x8000);
  map_pages(write, 0x3000000, 0x4000000, iwram + 0x8000);
  map_pages(read, 0x4000000, 0x5000000, (u8 *)io_registers);
  map_pages(read, HARNESS_ROM_BASE, HARNESS_ROM_BASE + HARNESS_ROM_SIZE,
   harness_rom + ((page << 15) - HARNESS_ROM_BASE));
}

void harness_reset(void)
{
  u32 i;

  flush_translation_cache_rom();
  flush_translation_cache_ram();
  flush_translation_cache_bios();
  translation_recursion_level = 0;
  translation_redo_attempts = 0;
  idle_loop_target_pc = 0xFFFFFFFF;

  memset(reg, 0, sizeof(reg));
  memset(reg_mode, 0, sizeof(reg_mode));
  memset(spsr, 0, sizeof(spsr));
  memset(io_registers, 0, sizeof(io_registers));
  memset(iwram, 0, sizeof(iwram));
  memset(ewram, 0, sizeof(ewram));
  memset(bios_rom, 0, sizeof(bios_rom));
  memset(harness_rom, 0, sizeof(harness_rom));
  for(i = 0; i < 3; i++)
    harness_last_ptr[i] = NULL;
  harness_last_flush_count = 0xFFFFFFFF;

  reg[REG_CPSR] = 0x1F;
  reg[CPU_MODE] = MODE_USER;
  reg[REG_SP] = 0x03007F00;
  reg_mode[MODE_USER][5] = 0x03007F00;
  reg_mode[MODE_IRQ][5] = 0x03007FA0;
  reg_mode[MODE_SUPERVISOR][5] = 0x03007FE0;
  reg[CPU_HALT_STATE] = CPU_ACTIVE;
}

void harness_write32(u32 address, u32 value)
{
  u8 *p = harness_backing(address);
  if(p == NULL)
  {
    fprintf(stderr, "harness_write32: unmapped %08x\n", address);
    exit(5);
  }
  memcpy(p, &value, 4);
}

void harness_write16(u32 address, u16 value)
{
  u8 *p = harness_backing(address);
  if(p == NULL)
  {
    fprintf(stderr, "harness_write16: unmapped %08x\n", address);
    exit(5);
  }
  memcpy(p, &value, 2);
}

u32 harness_read32(u32 address)
{
  return harness_backing_read(address, 4);
}

void harness_save(harness_state *s)
{
  memcpy(s->reg, reg, sizeof(reg));
  memcpy(s->reg_mode, reg_mode, sizeof(reg_mode));
  memcpy(s->spsr, spsr, sizeof(spsr));
  memcpy(s->io, io_registers, sizeof(s->io));
  memcpy(s->iwram, iwram + 0x8000, sizeof(s->iwram));
  memcpy(s->ewram_lo, ewram + 0x8000, sizeof(s->ewram_lo));
}

void harness_load(const harness_state *s)
{
  u32 i;

  flush_translation_cache_rom();
  flush_translation_cache_ram();
  flush_translation_cache_bios();
  memset(iwram, 0, 0x8000);
  for(i = 0; i < 8; i++)
    memset(ewram + i * 0x10000, 0, 0x8000);
  memset(bios_rom + 0x4000, 0, 0x4000);

  memcpy(reg, s->reg, sizeof(reg));
  memcpy(reg_mode, s->reg_mode, sizeof(reg_mode));
  memcpy(spsr, s->spsr, sizeof(spsr));
  memcpy(io_registers, s->io, sizeof(s->io));
  memcpy(iwram + 0x8000, s->iwram, sizeof(s->iwram));
  memcpy(ewram + 0x8000, s->ewram_lo, sizeof(s->ewram_lo));
}

harness_status harness_run(harness_engine engine, u32 exit_pc)
{
  volatile harness_status status;

  harness_exit_pc = exit_pc;
  harness_update_calls = 0;
  harness_halt_wakeups = 0;
  harness_fatal_message[0] = 0;
  reg[CHANGED_PC_STATUS] = 0;
  harness_running = 1;

  status = (harness_status)setjmp(harness_exit_jmp);
  if(status == 0)
  {
    if(engine == HARNESS_DYNAREC)
      execute_arm_translate(64);
    else
      execute_arm(64);
    status = HARNESS_RUN_FATAL;
  }

  harness_running = 0;
  translation_recursion_level = 0;
  translation_redo_attempts = 0;
  return status;
}
