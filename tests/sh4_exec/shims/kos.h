/* Test-only KOS shim for the qemu-sh4 dynarec harness.  The harness
   implements the cache maintenance calls so it can check that every
   emitted instruction is written back before it runs. */
#ifndef GPSP_SH4_EXEC_KOS_SHIM_H
#define GPSP_SH4_EXEC_KOS_SHIM_H

#include <stdint.h>

void dcache_flush_range(uint32_t start, uint32_t count);
void icache_flush_range(uint32_t start, uint32_t count);

#endif
