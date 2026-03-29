/*
 * i960 CPU context management.
 *
 * Implements call/return with register cache, and CPU reset.
 * The actual instruction execution is replaced by statically recompiled code.
 *
 * Reference: MAME i960.cpp (BSD-3-Clause)
 */

#include "model2recomp/i960.h"
#include "model2recomp/bus.h"
#include <string.h>
#include <stdio.h>

/* Global CPU context */
I960Context g_i960;

void i960_reset(void)
{
    memset(&g_i960, 0, sizeof(g_i960));

    /* Register cache starts empty (all frames available) */
    g_i960.rcache_pos = 0;

    /* Default stack pointer */
    g_i960.r[1] = 0x00500000; /* SP points to work RAM */

    /* Default arithmetic controls */
    g_i960.AC = 0;

    /* Process Controls: supervisor mode, interrupts enabled */
    g_i960.PC = 0x001f0000;

    printf("[i960] CPU reset\n");
}

/*
 * Save current local registers and set up new frame.
 * The i960 has a register cache that holds up to 4 frames.
 * When the cache is full, the oldest frame spills to memory (stack).
 *
 * CALL instruction behavior:
 *   1. Save current r0-r15 to cache (or stack if cache full)
 *   2. Set r0 (PFP) = old FP | return type
 *   3. Set r2 (RIP) = return address
 *   4. Set g15 (FP) = new SP (aligned to 64 bytes)
 *   5. Set r1 (SP) = FP + 64
 *   6. Jump to target
 */
void i960_do_call(uint32_t target_addr, uint32_t return_addr)
{
    uint32_t old_fp = I960_FP;

    if (g_i960.rcache_pos < I960_RCACHE_SIZE) {
        /* Cache has space - save local regs to cache */
        memcpy(g_i960.rcache[g_i960.rcache_pos], g_i960.r, 16 * sizeof(uint32_t));
        g_i960.rcache_frame_addr[g_i960.rcache_pos] = old_fp;
        g_i960.rcache_pos++;
    } else {
        /* Cache full - spill oldest frame to stack memory */
        uint32_t spill_addr = g_i960.rcache_frame_addr[0];
        for (int i = 0; i < 16; i++) {
            bus_write32(spill_addr + i * 4, g_i960.rcache[0][i]);
        }
        /* Shift cache entries down */
        for (int i = 0; i < I960_RCACHE_SIZE - 1; i++) {
            memcpy(g_i960.rcache[i], g_i960.rcache[i + 1], 16 * sizeof(uint32_t));
            g_i960.rcache_frame_addr[i] = g_i960.rcache_frame_addr[i + 1];
        }
        /* Save current frame to top of cache */
        memcpy(g_i960.rcache[I960_RCACHE_SIZE - 1], g_i960.r, 16 * sizeof(uint32_t));
        g_i960.rcache_frame_addr[I960_RCACHE_SIZE - 1] = old_fp;
    }

    /* Set up new frame */
    I960_PFP = old_fp & ~0x3f; /* Previous Frame Pointer (64-byte aligned, type=0 for local call) */
    I960_RIP = return_addr;

    /* New FP = current SP, aligned to 64 bytes */
    I960_FP = (I960_SP + 63) & ~63;
    I960_SP = I960_FP + 64;

    /* Clear new local registers r3-r15 */
    for (int i = 3; i < 16; i++) {
        g_i960.r[i] = 0;
    }

    g_i960.IP = target_addr;
}

/*
 * Restore previous frame from cache or stack.
 *
 * RET instruction behavior:
 *   1. Restore r0-r15 from cache (or load from stack if not cached)
 *   2. Set IP = saved RIP (return address)
 */
void i960_do_ret(void)
{
    uint32_t old_pfp = I960_PFP & ~0x3f;

    if (g_i960.rcache_pos > 0) {
        /* Restore from cache */
        g_i960.rcache_pos--;
        memcpy(g_i960.r, g_i960.rcache[g_i960.rcache_pos], 16 * sizeof(uint32_t));
    } else {
        /* Cache empty - load from stack memory */
        for (int i = 0; i < 16; i++) {
            g_i960.r[i] = bus_read32(old_pfp + i * 4);
        }
    }

    /* Restore FP to the previous frame pointer */
    I960_FP = old_pfp;

    /* IP = saved return address (was in r2 of the restored frame) */
    g_i960.IP = I960_RIP;
}
