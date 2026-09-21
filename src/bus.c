/*
 * Model 2 memory bus implementation.
 *
 * Routes 32-bit addresses to the appropriate hardware subsystem.
 * Reference: MAME model2.cpp memory maps (BSD-3-Clause)
 */

#include "model2recomp/bus.h"
#include "model2recomp/model2recomp.h"
#include "model2recomp/video.h"
#include "model2recomp/i960.h"
#include "model2recomp/copro.h"
#include "model2recomp/sound.h"
#include "model2recomp/io.h"
#include "model2recomp/timer.h"
#include "model2recomp/eeprom.h"
#include "model2recomp/i960.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory regions */
static uint8_t *s_program_rom = NULL;   /* 0x00000000, 2MB */
static uint32_t s_program_rom_size = 0;
/* Program RAM at 0x00200000. The original board puts 128KB here and shows the
 * second 128KB of program ROM at 0x00220000; every CRX board has 256KB of RAM
 * across the whole range and no ROM mirror at all. A 2A game that keeps its
 * variables above 0x00220000 therefore reads program ROM and writes nowhere,
 * which looks like a hang rather than a memory-map fault. */
static uint8_t *s_program_ram = NULL;
static uint32_t s_program_ram_size = 0x20000;
#define PROGRAM_RAM_TOP (0x00200000u + s_program_ram_size)
static model2_variant_t s_variant = MODEL2_ORIGINAL;
static bool is_crx(void) { return s_variant != MODEL2_ORIGINAL; }
static uint8_t *s_workram = NULL;       /* 0x00500000, 1MB */
static uint8_t *s_bufferram = NULL;     /* 0x00900000, 128KB */
static uint8_t *s_cpu_control = NULL;   /* 0x00E00000, 56 bytes */
static uint8_t *s_tile_ram = NULL;      /* 0x01000000, 64KB */
static uint8_t *s_char_ram = NULL;      /* 0x01080000, 512KB */
static uint8_t *s_backup_sram = NULL;   /* 0x01D00000, 16KB */
static uint8_t *s_data_rom = NULL;      /* 0x02000000, up to 32MB */
static uint32_t s_data_rom_size = 0;
static uint8_t *s_extra_data = NULL;    /* 0x06000000, up to 16MB */
static uint32_t s_extra_data_size = 0;

/* Texture ROM: not in the i960 address space at all - only the rasterizer
 * reads it, for texture pixels and for the per-vertex UVs the geometry engine
 * indexes by "texture point address". */
static uint8_t *s_texture_rom = NULL;
static uint32_t s_texture_rom_size = 0;

/* DPRAM for I/O board */
static uint8_t s_dpram[0x1000];

/* VBlank callback */
static bus_vblank_callback_t s_vblank_cb = NULL;

void bus_set_variant(model2_variant_t v)
{
    s_variant = v;
    /* The only thing the variant changes in the map today: 256KB of program
     * RAM across 0x00200000-0x0023FFFF on CRX, against 128KB plus a ROM mirror
     * on the original board. Call before bus_init. */
    s_program_ram_size = (v == MODEL2_ORIGINAL) ? 0x20000u : 0x40000u;
}

void bus_init(void)
{
    s_program_ram = (uint8_t *)calloc(1, 0x40000);   /* 128KB, or 256KB on CRX */
    s_workram     = (uint8_t *)calloc(1, 0x100000);  /* 1MB */
    s_bufferram   = (uint8_t *)calloc(1, 0x20000);   /* 128KB */
    s_cpu_control = (uint8_t *)calloc(1, 0x38);      /* 56 bytes */
    s_tile_ram    = (uint8_t *)calloc(1, 0x10000);   /* 64KB */
    s_char_ram    = (uint8_t *)calloc(1, 0x80000);   /* 512KB */
    s_backup_sram = (uint8_t *)calloc(1, 0x4000);    /* 16KB */
    memset(s_dpram, 0xFF, sizeof(s_dpram));
    sega5649_reset();

    printf("[bus] Memory bus initialized\n");
}

void bus_shutdown(void)
{
    free(s_program_rom);   s_program_rom = NULL;
    free(s_program_ram);   s_program_ram = NULL;
    free(s_workram);       s_workram = NULL;
    free(s_bufferram);     s_bufferram = NULL;
    free(s_cpu_control);   s_cpu_control = NULL;
    free(s_tile_ram);      s_tile_ram = NULL;
    free(s_char_ram);      s_char_ram = NULL;
    free(s_backup_sram);   s_backup_sram = NULL;
    free(s_data_rom);      s_data_rom = NULL;
    free(s_extra_data);    s_extra_data = NULL;
    free(s_texture_rom);   s_texture_rom = NULL;
}

void bus_set_vblank_callback(bus_vblank_callback_t cb)
{
    s_vblank_cb = cb;
}

/* ---- Helper: little-endian memory access ---- */

static inline uint8_t mem_read8(const uint8_t *base, uint32_t offset)
{
    return base[offset];
}

static inline uint16_t mem_read16(const uint8_t *base, uint32_t offset)
{
    return (uint16_t)base[offset] | ((uint16_t)base[offset + 1] << 8);
}

static inline uint32_t mem_read32(const uint8_t *base, uint32_t offset)
{
    return (uint32_t)base[offset]
         | ((uint32_t)base[offset + 1] << 8)
         | ((uint32_t)base[offset + 2] << 16)
         | ((uint32_t)base[offset + 3] << 24);
}

static inline void mem_write8(uint8_t *base, uint32_t offset, uint8_t val)
{
    base[offset] = val;
}

static inline void mem_write16(uint8_t *base, uint32_t offset, uint16_t val)
{
    base[offset]     = (uint8_t)(val);
    base[offset + 1] = (uint8_t)(val >> 8);
}

static inline void mem_write32(uint8_t *base, uint32_t offset, uint32_t val)
{
    base[offset]     = (uint8_t)(val);
    base[offset + 1] = (uint8_t)(val >> 8);
    base[offset + 2] = (uint8_t)(val >> 16);
    base[offset + 3] = (uint8_t)(val >> 24);
}



/* MODEL2_UNMAPPED=1 reports every address the guest touches that the map does
 * not cover, each one once, with a count of how often at exit.
 *
 * This is the memory-side twin of func_table's "no function at 0x..." - the
 * other way a recompiled game sits doing nothing for a reason nothing tells
 * you about. A game polling an unmapped status register reads the same zero
 * forever, and from the outside that is indistinguishable from a hang. Across
 * a thirty-title corpus it is the difference between "this one is stuck" and
 * "this one wants a register we have not modelled".
 */
#define UNMAPPED_SLOTS 256
static struct { uint32_t addr; uint32_t reads, writes; } s_unmapped[UNMAPPED_SLOTS];
static int s_unmapped_n = -1;

static void unmapped_note(uint32_t addr, bool write)
{
    if (s_unmapped_n < 0) {
        const char *e = getenv("MODEL2_UNMAPPED");
        s_unmapped_n = (e && atoi(e)) ? 0 : -2;
    }
    if (s_unmapped_n < 0) return;

    /* Round to the dword: a register polled with byte, half and word loads is
     * one register, not three findings. */
    addr &= ~3u;
    for (int i = 0; i < s_unmapped_n; i++) {
        if (s_unmapped[i].addr == addr) {
            if (write) s_unmapped[i].writes++; else s_unmapped[i].reads++;
            return;
        }
    }
    if (s_unmapped_n >= UNMAPPED_SLOTS) return;
    s_unmapped[s_unmapped_n].addr = addr;
    s_unmapped[s_unmapped_n].reads = write ? 0 : 1;
    s_unmapped[s_unmapped_n].writes = write ? 1 : 0;
    printf("[bus] unmapped %s 0x%08X\n", write ? "write" : "read", addr);
    s_unmapped_n++;
}

/* MODEL2_HOTREADS=1 counts reads of hardware registers and prints the busiest
 * at exit.
 *
 * A game that has stopped making progress is almost always sitting on one
 * status register waiting for a bit that never changes, and the address is
 * computed at run time so the lifted C does not name it. MODEL2_TRACE names the
 * function; this names the register it is reading, which is the half that says
 * what to implement. Sky Target's UART spin took a trace plus a read of the
 * generated source to find; this would have said "0x01C80002, four million
 * times" on its own.
 *
 * Work and program RAM are excluded - they are not what a game waits on, and
 * counting them would bury the signal. */
#define HOTREAD_SLOTS 512
static struct { uint32_t addr; uint32_t hits; } s_hotread[HOTREAD_SLOTS];
static int s_hotread_n = -1;

static void hotread_note(uint32_t addr)
{
    if (s_hotread_n < 0) {
        const char *e = getenv("MODEL2_HOTREADS");
        s_hotread_n = (e && atoi(e)) ? 0 : -2;
    }
    if (s_hotread_n < 0) return;
    addr &= ~3u;
    for (int i = 0; i < s_hotread_n; i++)
        if (s_hotread[i].addr == addr) { s_hotread[i].hits++; return; }
    if (s_hotread_n >= HOTREAD_SLOTS) return;
    s_hotread[s_hotread_n].addr = addr;
    s_hotread[s_hotread_n].hits = 1;
    s_hotread_n++;
}

void bus_report_hotreads(void)
{
    if (s_hotread_n <= 0) return;
    /* Selection sort of the top 15; the table is small and this runs once. */
    for (int i = 0; i < 15 && i < s_hotread_n; i++) {
        int best = i;
        for (int j = i + 1; j < s_hotread_n; j++)
            if (s_hotread[j].hits > s_hotread[best].hits) best = j;
        if (best != i) {
            struct { uint32_t addr, hits; } t = { s_hotread[i].addr, s_hotread[i].hits };
            s_hotread[i] = s_hotread[best];
            s_hotread[best].addr = t.addr; s_hotread[best].hits = t.hits;
        }
        printf("[bus] hot read 0x%08X  x%u\n", s_hotread[i].addr, s_hotread[i].hits);
    }
}

void bus_report_unmapped(void)
{
    if (s_unmapped_n <= 0) return;
    printf("[bus] unmapped addresses touched: %d\n", s_unmapped_n);
    for (int i = 0; i < s_unmapped_n; i++)
        printf("    0x%08X  r=%u w=%u\n", s_unmapped[i].addr,
               s_unmapped[i].reads, s_unmapped[i].writes);
}

/* ---- Bus read ---- */

/* ---- Link (comm) board ----
 *
 * 16KB of shared RAM at 0x01A00000 with two byte registers just past it -
 * 0x01A04000 latches the node enable, 0x01A04002 the handshake flag - and the
 * whole thing mirrored at 0x01A10000.
 *
 * The board is fitted and this cabinet is on its own, which is not the same as
 * the board being absent: one cabinet is a ring of one node.
 *
 * Enabling the board zeroes the shared RAM, publishes the frame geometry, and
 * opens a four-second discovery window. Throughout, the board's own processor
 * services every vertical interrupt and flips the handshake bit the game
 * watches in bit 7 of the flag register - the game will not advance its own
 * countdown until it sees that bit alternate. When the window closes the board
 * reports the ring it found. With nobody else on the cable that is itself:
 * link alive, node 1 of 1.
 *
 * The game insists on exactly that. After its countdown it reads shared byte 0
 * and demands 0x01, then range-checks the node id and node count into 1..8;
 * anything else prints CANCELLED and soft-resets the machine.
 *
 * MAME reaches the same state, but only with a socket open to another
 * instance, because it models the ring as the cable rather than as the board -
 * with no socket it leaves the link unestablished and the handshake bit still.
 * Everything here except closing that ring on ourselves follows its
 * sega/m2comm.cpp; see NOTICE.
 */
#define COMM_BASE     0x01A00000u
#define COMM_MIRROR   0x01A10000u
#define COMM_SHARED   0x4000u          /* shared RAM size */
#define COMM_SIZE     0x4008u          /* shared RAM plus the two registers */
#define COMM_CN       0x4000u          /* node enable latch */
#define COMM_FG       0x4002u          /* handshake flag latch */
#define COMM_LINK_MS  0xE8u            /* MAME's 58 fps * 4 seconds */

#define COMM_FRAME_START  0x2000u      /* where the transmit window begins */
#define COMM_FRAME_SIZE   0x0E00u      /* bytes of game state per node */
#define COMM_FRAME_OFFSET 0x01C0u      /* where this node's slot starts */

static uint8_t s_comm_shared[COMM_SHARED];
static uint8_t s_comm_cn;
static uint8_t s_comm_fg;
static uint8_t s_comm_zfg;             /* the board's half of the handshake */
static uint16_t s_comm_timer;          /* fields left in the discovery window */

static bool comm_offset(uint32_t addr, uint32_t *out)
{
    uint32_t base = (addr >= COMM_MIRROR) ? COMM_MIRROR : COMM_BASE;
    if (addr < base || addr - base >= COMM_SIZE)
        return false;
    *out = addr - base;
    return true;
}

static uint8_t comm_read8(uint32_t off)
{
    /* The registers read back their latched bit 0 with the unused bits high,
     * and the flag register carries the peer's toggle in bit 7 - inverted, and
     * there is no peer, so it stays set. */
    if (off == COMM_CN) return (uint8_t)(s_comm_cn | 0xFE);
    if (off == COMM_FG) return (uint8_t)(s_comm_fg | (s_comm_zfg ? 0x00 : 0x80) | 0x7E);
    if (off < COMM_SHARED) return s_comm_shared[off];
    return 0xFF;
}

static void comm_write8(uint32_t off, uint8_t val)
{
    if (off == COMM_CN) {
        s_comm_cn = val & 0x01;
        if (s_comm_cn) {
            memset(s_comm_shared, 0, sizeof(s_comm_shared));
            s_comm_shared[0x01] = 0x02;
            s_comm_shared[0x00] = 0x00;   /* link not established yet */
            s_comm_shared[0x02] = 0xFF;
            s_comm_shared[0x03] = 0xFF;
            s_comm_shared[0x12] = (uint8_t)(COMM_FRAME_SIZE & 0xFF);
            s_comm_shared[0x13] = (uint8_t)(COMM_FRAME_SIZE >> 8);
            s_comm_shared[0x14] = (uint8_t)(COMM_FRAME_OFFSET & 0xFF);
            s_comm_shared[0x15] = (uint8_t)(COMM_FRAME_OFFSET >> 8);
            s_comm_timer = COMM_LINK_MS;
        } else {
            s_comm_fg = 0;
            s_comm_zfg = 0;
            s_comm_timer = 0;
        }
        return;
    }
    if (off == COMM_FG) { s_comm_fg = val & 0x01; return; }
    if (off < COMM_SHARED) s_comm_shared[off] = val;
}

/* Byte and halfword access has to reach these registers directly. The generic
 * narrow-write path reads the surrounding word, patches a byte and writes the
 * word back, which is right for memory and wrong for a register with side
 * effects: a byte store to the flag register at +2 was rewriting the enable
 * register at +0 with its own read-back value, and enabling the board a second
 * time zeroes the shared RAM and restarts the discovery timer. The board never
 * finished discovering because the game's own writes kept resetting it.
 */
/* Called once per field, from the same place the vertical interrupt is
 * raised - which is when the board's processor would see it. */
static void bus_watchdog_field(void);

void bus_comm_tick(void)
{
    bus_watchdog_field();

    if (!s_comm_cn)
        return;

    /* The board is alive whether or not anyone answers, and the game watches
     * this bit alternate to decide the board is alive. */
    s_comm_zfg ^= 1;

    if (s_comm_timer && --s_comm_timer == 0) {
        s_comm_shared[0x00] = 0x01;    /* link established */
        s_comm_shared[0x02] = 0x01;    /* this node's id */
        s_comm_shared[0x03] = 0x01;    /* nodes in the ring: just us */
    }
    if (s_comm_shared[0x00] != 0x01)
        return;

    /*
     * Close the ring. Every node's frame travels all the way round and comes
     * back to the node that sent it - that is what makes it a ring rather than
     * a broadcast, and the game relies on it: before it will start, it waits
     * until it has received a frame from every node, its own included, and
     * counts them against the node count the board reported.
     *
     * With one cabinet the loop is short. The board takes what the game put in
     * the transmit window and delivers it to the receive window the game is
     * pointing at, which is where a second cabinet's frame would have landed.
     */
    uint32_t frame_size = (uint32_t)s_comm_shared[0x13] << 8 | s_comm_shared[0x12];
    uint32_t frame_off  = COMM_FRAME_START
                        | ((uint32_t)s_comm_shared[0x15] << 8 | s_comm_shared[0x14]);
    if (frame_size == 0 || frame_off < COMM_FRAME_START)
        return;
    if (frame_off + frame_size > COMM_SHARED)
        frame_size = COMM_SHARED - frame_off;
    memmove(s_comm_shared + frame_off, s_comm_shared + COMM_FRAME_START, frame_size);
}

/* MODEL2_WATCHDOG=N aborts after N million bus operations without a field
 * boundary, naming the function the guest was last dispatched into.
 *
 * A guest loop that neither calls anything nor reads the field-sync register
 * is invisible to every other diagnostic here: no dispatch for the trace or
 * the ring to record, no field for the profile to be dumped at, and killing
 * the process loses whatever was still buffered. Almost any such loop does
 * touch memory, though, so this catches it. */
static unsigned long s_bus_ops;

static void bus_watchdog_field(void) { s_bus_ops = 0; }

static void bus_watchdog_tick(void)
{
    static unsigned long limit = ~0UL;
    if (limit == ~0UL) {
        const char *e = getenv("MODEL2_WATCHDOG");
        limit = e ? strtoul(e, NULL, 0) * 1000000UL : 0;
    }
    if (!limit || ++s_bus_ops < limit)
        return;
    extern uint32_t g_cur_func;
    fprintf(stderr, "[watchdog] %lu bus ops with no field boundary; last function %08X fp=%08X sp=%08X\n",
            s_bus_ops, g_cur_func, g_i960.r[31], g_i960.r[1]);
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "  g%-2d %08X%s", i, g_i960.r[16 + i],
                (i % 4) == 3 ? "\n" : "");
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "  r%-2d %08X%s", i, g_i960.r[i],
                (i % 4) == 3 ? "\n" : "");
    extern void func_table_dump_ring(void);
    func_table_dump_ring();
    fflush(stderr);
    exit(3);
}

uint32_t bus_read32(uint32_t addr)
{
    bus_watchdog_tick();
    /*
     * The i960 does not fault on an unaligned word access - it splits it in
     * microcode and pays for it in cycles - and the compiler leans on that.
     * Daytona's tilemap tables start on a two-byte boundary, so "ld (g0)" on
     * one straddles two words; masking the address off read the wrong word
     * and turned a row count of 6 into 0x00060001, which is a blit that never
     * finishes. Split it here the way the hardware does.
     */
    if (addr & 3)
        return (uint32_t)bus_read8(addr)
             | ((uint32_t)bus_read8(addr + 1) << 8)
             | ((uint32_t)bus_read8(addr + 2) << 16)
             | ((uint32_t)bus_read8(addr + 3) << 24);

    if (addr >= 0x00800000 && addr < 0x02000000)
        hotread_note(addr);

    /* Program ROM: 0x00000000-0x001FFFFF */
    if (addr < 0x00200000) {
        if (s_program_rom && addr < s_program_rom_size)
            return mem_read32(s_program_rom, addr);
        return 0;
    }

    /* Program RAM: 128KB on the original board, 256KB on every CRX. */
    if (addr >= 0x00200000 && addr < PROGRAM_RAM_TOP) {
        return mem_read32(s_program_ram, addr - 0x00200000);
    }

    /* Program ROM extension: 0x00220000-0x0023FFFF, original board only. */
    if (addr >= 0x00220000 && addr < 0x00240000) {
        if (s_program_rom && (addr - 0x00220000 + 0x20000) < s_program_rom_size)
            return mem_read32(s_program_rom, addr - 0x00220000 + 0x20000);
        return 0;
    }

    /* Work RAM: 0x00500000-0x005FFFFF */
    if (addr >= 0x00500000 && addr < 0x00600000) {
        return mem_read32(s_workram, addr - 0x00500000);
    }

    /* Geometry engine: 0x00800000-0x00803FFF */
    if (addr >= 0x00800000 && addr < 0x00804000) {
        return geo_read((addr - 0x00800000) >> 2);
    }

    /* Geo program: 0x00804000-0x00807FFF */
    if (addr >= 0x00804000 && addr < 0x00808000) {
        return geo_prg_read((addr - 0x00804000) >> 2);
    }

    /* Copro FIFO read: 0x00884000-0x00887FFF */
    if (addr >= 0x00884000 && addr < 0x00888000) {
        return copro_fifo_read();
    }

    /* Buffer RAM: 0x00900000-0x0091FFFF (mirrored at 0x60000 intervals) */
    if (addr >= 0x00900000 && addr < 0x00980000) {
        uint32_t offset = (addr - 0x00900000) & 0x1FFFF;
        return mem_read32(s_bufferram, offset);
    }

    /* System control registers */
    if (addr >= 0x00980000 && addr < 0x00980040) {
        uint32_t reg = (addr - 0x00980000) >> 2;
        switch (reg) {
            case 0: return copro_ctl_read();        /* 0x00980000 */
            case 1: return fifo_control_read();     /* 0x00980004 */
            /* Field status: the frame boundary for the recompiled game. */
            case 3: return model2recomp_field_sync(); /* 0x0098000C */
            case 5: return copro_status_read();     /* 0x00980014 */
            case 12: case 13: case 14: case 15:     /* 0x00980030-0x0098003F */
                return tgpid_read(reg - 12);
            default: return 0;
        }
    }

    /* CPU control: 0x00E00000-0x00E00037 */
    if (addr >= 0x00E00000 && addr < 0x00E00038) {
        return mem_read32(s_cpu_control, addr - 0x00E00000);
    }

    /* IRQ request/ack: 0x00E80000 */
    if (addr >= 0x00E80000 && addr < 0x00E80004) {
        return irq_request_read();
    }

    /* IRQ enable: 0x00E80004 */
    if (addr >= 0x00E80004 && addr < 0x00E80008) {
        return irq_enable_read();
    }

    /* Timers: 0x00F00000-0x00F0000F */
    if (addr >= 0x00F00000 && addr < 0x00F00010) {
        return timer_read((addr - 0x00F00000) >> 2);
    }

    /* System 24 tile RAM: 0x01000000-0x0100FFFF */
    if (addr >= 0x01000000 && addr < 0x01010000) {
        uint32_t off = addr - 0x01000000;
        return (uint32_t)tile_read(off >> 1) | ((uint32_t)tile_read((off >> 1) + 1) << 16);
    }

    /* System 24 char RAM: 0x01080000-0x010FFFFF */
    if (addr >= 0x01080000 && addr < 0x01100000) {
        uint32_t off = addr - 0x01080000;
        return (uint32_t)char_read(off >> 1) | ((uint32_t)char_read((off >> 1) + 1) << 16);
    }

    /* Palette: 0x01800000-0x01803FFF */
    if (addr >= 0x01800000 && addr < 0x01804000) {
        uint32_t off = (addr - 0x01800000) >> 1;
        return (uint32_t)palette_read(off) | ((uint32_t)palette_read(off + 1) << 16);
    }

    /* Color translate: 0x01810000-0x0181BFFF */
    if (addr >= 0x01810000 && addr < 0x0181C000) {
        uint32_t off = (addr - 0x01810000) >> 1;
        return (uint32_t)colorxlat_read(off) | ((uint32_t)colorxlat_read(off + 1) << 16);
    }

    /* DPRAM (I/O board): 0x01C00000-0x01C00FFF.
     *
     * MB8421 is 2Kx8 on a 32-bit bus with byte lanes 0 and 2 populated
     * (MAME's umask32(0x00ff00ff)), so 0x1000 bytes of i960 space cover 0x800
     * DPRAM bytes: each dword holds two consecutive ones. The game reads them
     * with 16-bit loads at consecutive even addresses, which is only
     * consecutive in DPRAM if the halving is done here. */
    if (addr >= 0x01C00000 && addr < 0x01C01000) {
        uint32_t d = (addr - 0x01C00000) >> 1;
        /* Same byte lanes, a different chip behind them: the CRX boards have a
         * 32-byte 315-5649 register file where the original has 2KB of
         * dual-port RAM and a command protocol. */
        if (is_crx()) {
            if (d >= 0x20) return 0xFFFFFFFFu;
            return (uint32_t)sega5649_read((uint8_t)d)
                 | ((uint32_t)sega5649_read((uint8_t)(d + 1)) << 16);
        }
        return (uint32_t)dpram_read(d) | ((uint32_t)dpram_read(d + 1) << 16);
    }

    /* UART: 0x01C80000-0x01C80003.
     *
     * An 8-bit device on a 32-bit bus with byte lanes 0 and 2 populated
     * (MAME's umask16(0x00ff) over the pair of halves), exactly like the
     * DPRAM: data register at +0, status at +2. Returning only register 0 for
     * the whole dword is what made a 16-bit read of the status - which is how
     * the games actually poll it - come back as zero. Sky Target spins on bit
     * 0 of 0x01C80002 forever, and it is not alone; the original board has the
     * same mapping, Virtua Cop and Daytona simply never read it this way. */
    if (addr >= 0x01C80000 && addr < 0x01C80004) {
        return (uint32_t)uart_read(0) | ((uint32_t)uart_read(1) << 16);
    }

    /* 2B-CRX puts the same two registers at 0x009C0000 and 0x009C0004. */
    if (s_variant == MODEL2B_CRX && addr >= 0x009C0000 && addr < 0x009C0008) {
        return (uint32_t)uart_read((addr - 0x009C0000) >> 2);
    }

    /* Link board: 0x01A00000 and its mirror at 0x01A10000 */
    {
        uint32_t off;
        if (comm_offset(addr, &off)) {
            off &= ~3u;
            return (uint32_t)comm_read8(off)
                 | ((uint32_t)comm_read8(off + 1) << 8)
                 | ((uint32_t)comm_read8(off + 2) << 16)
                 | ((uint32_t)comm_read8(off + 3) << 24);
        }
    }

    /* Backup SRAM: 0x01D00000-0x01D03FFF */
    if (addr >= 0x01D00000 && addr < 0x01D04000) {
        return backup_sram_read((addr - 0x01D00000) >> 2);
    }

    /* Data ROM: 0x02000000-0x03FFFFFF */
    if (addr >= 0x02000000 && addr < 0x04000000) {
        uint32_t off = addr - 0x02000000;
        if (s_data_rom && off < s_data_rom_size)
            return mem_read32(s_data_rom, off);
        return 0;
    }

    /* Extra data: 0x06000000-0x06FFFFFF */
    if (addr >= 0x06000000 && addr < 0x07000000) {
        uint32_t off = addr - 0x06000000;
        if (s_extra_data && off < s_extra_data_size)
            return mem_read32(s_extra_data, off);
        return 0;
    }

    /* Render mode: 0x10000000-0x101FFFFF */
    if (addr >= 0x10000000 && addr < 0x10200000) {
        return render_mode_read();
    }

    /* Polygon count: 0x10400000-0x105FFFFF */
    if (addr >= 0x10400000 && addr < 0x10600000) {
        return polygon_count_read();
    }

    /* Polygon count (nop read): 0x10800000 */
    if (addr >= 0x10800000 && addr < 0x10800004) {
        return 0;
    }

    /* Framebuffer A: 0x11600000-0x1167FFFF */
    if (addr >= 0x11600000 && addr < 0x11680000) {
        uint32_t off = (addr - 0x11600000) >> 1;
        return (uint32_t)fbvram_bankA_read(off) | ((uint32_t)fbvram_bankA_read(off + 1) << 16);
    }

    /* Framebuffer B: 0x11680000-0x116FFFFF */
    if (addr >= 0x11680000 && addr < 0x11700000) {
        uint32_t off = (addr - 0x11680000) >> 1;
        return (uint32_t)fbvram_bankB_read(off) | ((uint32_t)fbvram_bankB_read(off + 1) << 16);
    }

    /* Texture RAM 0: 0x12000000-0x123FFFFF (mirrored) */
    if (addr >= 0x12000000 && addr < 0x12400000) {
        /* Read handled by video subsystem */
        return 0; /* TODO: texture RAM read */
    }

    /* Texture RAM 1: 0x12400000-0x127FFFFF (mirrored) */
    if (addr >= 0x12400000 && addr < 0x12800000) {
        return 0; /* TODO: texture RAM read */
    }

    /* Luma RAM: 0x12800000-0x1281FFFF */
    if (addr >= 0x12800000 && addr < 0x12820000) {
        /* Only the low byte of each 32-bit slot is luma RAM (MAME maps it
         * umask32 0x000000ff), so the index is a dword index. */
        return (uint32_t)lumaram_read((addr - 0x12800000) >> 2);
    }

    /* The CRX video board puts the same memories somewhere else: texture RAM
     * at 0x11000000 (two 1MB banks, each mirrored once) and luma RAM at
     * 0x11400000 on a 16-bit lane rather than a 32-bit one. A 2B game writing
     * its textures there was writing into nothing. */
    if (is_crx() && addr >= 0x11400000 && addr < 0x11410000) {
        return (uint32_t)lumaram_read((addr - 0x11400000) >> 1);
    }
    if (is_crx() && addr >= 0x11000000 && addr < 0x11400000) {
        return 0;                       /* TODO: texture RAM read, as below */
    }

    /* Unmapped */
    /* printf("[bus] Unmapped read32: 0x%08X\n", addr); */
    return 0;
}

uint16_t bus_read16(uint32_t addr)
{
    addr &= ~1;

    /* Fast path for common regions */
    if (addr >= 0x00500000 && addr < 0x00600000)
        return mem_read16(s_workram, addr - 0x00500000);

    if (addr < 0x00200000 && s_program_rom && addr < s_program_rom_size)
        return mem_read16(s_program_rom, addr);

    if (addr >= 0x00200000 && addr < PROGRAM_RAM_TOP)
        return mem_read16(s_program_ram, addr - 0x00200000);

    /* Fall through to 32-bit read and extract */
    uint32_t val32 = bus_read32(addr & ~3);
    return (uint16_t)(val32 >> ((addr & 2) * 8));
}

uint8_t bus_read8(uint32_t addr)
{
    /* Fast path for common regions */
    if (addr >= 0x00500000 && addr < 0x00600000)
        return s_workram[addr - 0x00500000];

    if (addr < 0x00200000 && s_program_rom && addr < s_program_rom_size)
        return s_program_rom[addr];

    if (addr >= 0x00200000 && addr < PROGRAM_RAM_TOP)
        return s_program_ram[addr - 0x00200000];

    {
        uint32_t off;
        if (comm_offset(addr, &off))
            return comm_read8(off);
    }

    /* Fall through */
    uint32_t val32 = bus_read32(addr & ~3);
    return (uint8_t)(val32 >> ((addr & 3) * 8));
}

/* ---- i960 IAC (Interagent Communication) ----
 *
 * synmov/synmovq to 0xFF000010 delivers an IAC message to this processor.
 * Virtua Cop's boot ROM uses message 0x93 (Reinitialize Processor) to hand
 * control from the reset stub to the real firmware entry with a new PRCB:
 *   field0 = 0x93000000, field2 = new PRCB, field3 = new IP.
 * Only reinit is modelled; other messages are accepted and ignored. */
#define IAC_MSG_BASE   0xFF000010u
#define IAC_REINIT     0x93u
#define IAC_ICR        0xFF000004u  /* synmov here loads the interrupt control register */

static uint32_t s_iac[4];
static uint32_t s_iac_reinit_ip;
static uint32_t s_iac_reinit_prcb;
static uint32_t s_i960_icr;

/* The ICR packs one interrupt vector per external IRQ line, line 0 in the low
 * byte. Virtua Cop loads 0x0F0E0D0C, so VBlank (line 0) is vector 12. */
uint32_t bus_i960_icr(void)
{
    return s_i960_icr;
}

/* Current Process Control Block: whatever the last reinitialize IAC named, or
 * the reset PRCB from the System Address Table if the guest never reinitialized. */
uint32_t bus_i960_prcb(void)
{
    return s_iac_reinit_prcb ? s_iac_reinit_prcb : bus_read32(4);
}

bool bus_iac_reinit_pending(void)
{
    return s_iac_reinit_ip != 0;
}

uint32_t bus_iac_take_reinit(uint32_t *out_prcb)
{
    uint32_t ip = s_iac_reinit_ip;
    if (out_prcb) *out_prcb = s_iac_reinit_prcb;
    s_iac_reinit_ip = 0;
    return ip;
}

/* ---- Bus write ---- */

void bus_write32(uint32_t addr, uint32_t val)
{
    /* Unaligned, as above: split it rather than write the wrong word. */
    if (addr & 3) {
        for (int i = 0; i < 4; i++)
            bus_write8(addr + i, (uint8_t)(val >> (i * 8)));
        return;
    }

    {
        static const char *watch_env; static uint32_t watch;
        if (!watch_env) { watch_env = getenv("MODEL2_WATCH"); if (!watch_env) watch_env = ""; watch = (uint32_t)strtoul(watch_env, NULL, 0); }
        if (watch && addr == watch) {
            float f; memcpy(&f, &val, 4);
            extern uint32_t g_cur_func;
            fprintf(stderr, "[watch] %08X = %08X (%g) in %08X g14=%08X fp=%08X sp=%08X\n", addr, val, (double)f, g_cur_func, g_i960.r[30], g_i960.r[31], g_i960.r[1]);
            if (getenv("MODEL2_WATCHPATH")) {
                extern void func_table_dump_ring(void);
                func_table_dump_ring();
            }
        }
    }

    /* Interrupt control register (synmov, not an IAC message) */
    if (addr == IAC_ICR) {
        s_i960_icr = val;
        return;
    }

    /* IAC message registers: 0xFF000010-0xFF00001F */
    if (addr >= IAC_MSG_BASE && addr < IAC_MSG_BASE + 16) {
        uint32_t word = (addr - IAC_MSG_BASE) >> 2;
        s_iac[word] = val;
        /* The quad is written low word first; act once the last word lands. */
        if (word == 3 && (s_iac[0] >> 24) == IAC_REINIT) {
            s_iac_reinit_prcb = s_iac[2];
            s_iac_reinit_ip   = s_iac[3];
        }
        return;
    }

    /* Program ROM: 0x00000000-0x001FFFFF (writes ignored) */
    if (addr < 0x00200000) return;

    /* Program RAM: 128KB on the original board, 256KB on every CRX. */
    if (addr >= 0x00200000 && addr < PROGRAM_RAM_TOP) {
        mem_write32(s_program_ram, addr - 0x00200000, val);
        return;
    }

    /* Work RAM: 0x00500000-0x005FFFFF */
    if (addr >= 0x00500000 && addr < 0x00600000) {
        mem_write32(s_workram, addr - 0x00500000, val);
        return;
    }

    /* Geometry engine: 0x00800000-0x00803FFF */
    if (addr >= 0x00800000 && addr < 0x00804000) {
        geo_write((addr - 0x00800000) >> 2, val);
        return;
    }

    /* Geo program: 0x00804000-0x00807FFF */
    if (addr >= 0x00804000 && addr < 0x00808000) {
        geo_prg_write(val);
        return;
    }

    /* Copro function port: 0x00880000-0x00883FFF */
    if (addr >= 0x00880000 && addr < 0x00884000) {
        copro_function_write((addr - 0x00880000) >> 2, val);
        return;
    }

    /* Copro FIFO write: 0x00884000-0x00887FFF */
    if (addr >= 0x00884000 && addr < 0x00888000) {
        copro_fifo_write(val);
        return;
    }

    /* Buffer RAM: 0x00900000-0x0091FFFF */
    if (addr >= 0x00900000 && addr < 0x00980000) {
        uint32_t offset = (addr - 0x00900000) & 0x1FFFF;
        mem_write32(s_bufferram, offset, val);
        return;
    }

    /* System control registers */
    if (addr >= 0x00980000 && addr < 0x00980040) {
        uint32_t reg = (addr - 0x00980000) >> 2;
        switch (reg) {
            case 0: copro_ctl_write(val); return;
            case 2: geo_ctl1_write(val); return;
            case 3: videoctl_write(val); return;
            default: return;
        }
    }

    /* CPU control: 0x00E00000-0x00E00037 */
    if (addr >= 0x00E00000 && addr < 0x00E00038) {
        mem_write32(s_cpu_control, addr - 0x00E00000, val);
        return;
    }

    /* IRQ ack: 0x00E80000 */
    if (addr >= 0x00E80000 && addr < 0x00E80004) {
        irq_ack_write(val);
        return;
    }

    /* IRQ enable: 0x00E80004 */
    if (addr >= 0x00E80004 && addr < 0x00E80008) {
        irq_enable_write(val);
        return;
    }

    /* Timers: 0x00F00000-0x00F0000F */
    if (addr >= 0x00F00000 && addr < 0x00F00010) {
        timer_write((addr - 0x00F00000) >> 2, val);
        return;
    }

    /* System 24 tile RAM: 0x01000000-0x0100FFFF */
    if (addr >= 0x01000000 && addr < 0x01010000) {
        uint32_t off = (addr - 0x01000000) >> 1;
        tile_write(off, (uint16_t)val);
        tile_write(off + 1, (uint16_t)(val >> 16));
        return;
    }

    /* System 24 char RAM: 0x01080000-0x010FFFFF */
    if (addr >= 0x01080000 && addr < 0x01100000) {
        uint32_t off = (addr - 0x01080000) >> 1;
        char_write(off, (uint16_t)val);
        char_write(off + 1, (uint16_t)(val >> 16));
        return;
    }

    /* Palette: 0x01800000-0x01803FFF */
    if (addr >= 0x01800000 && addr < 0x01804000) {
        uint32_t off = (addr - 0x01800000) >> 1;
        palette_write(off, (uint16_t)val);
        palette_write(off + 1, (uint16_t)(val >> 16));
        return;
    }

    /* Color translate: 0x01810000-0x0181BFFF */
    if (addr >= 0x01810000 && addr < 0x0181C000) {
        uint32_t off = (addr - 0x01810000) >> 1;
        colorxlat_write(off, (uint16_t)val);
        colorxlat_write(off + 1, (uint16_t)(val >> 16));
        return;
    }

    /* 3D Z clip: 0x0181C000 */
    if (addr >= 0x0181C000 && addr < 0x0181C004) {
        zclip_write(val);
        return;
    }

    /* DPRAM: 0x01C00000-0x01C00FFF (see bus_read32 for the lane mapping) */
    if (addr >= 0x01C00000 && addr < 0x01C01000) {
        uint32_t d = (addr - 0x01C00000) >> 1;
        if (is_crx()) {
            if (d < 0x20) {
                sega5649_write((uint8_t)d, (uint8_t)val);
                sega5649_write((uint8_t)(d + 1), (uint8_t)(val >> 16));
            }
            return;
        }
        dpram_write(d, (uint8_t)val);
        dpram_write(d + 1, (uint8_t)(val >> 16));
        return;
    }

    /* UART: 0x01C80000-0x01C80003 (see bus_read32 for the lane mapping) */
    if (addr >= 0x01C80000 && addr < 0x01C80004) {
        uart_write(0, (uint8_t)val);
        uart_write(1, (uint8_t)(val >> 16));
        return;
    }

    /* 2B-CRX puts the same two registers at 0x009C0000 and 0x009C0004, one per
     * dword on byte lane 0, instead of at 0x01C80000. */
    if (s_variant == MODEL2B_CRX && addr >= 0x009C0000 && addr < 0x009C0008) {
        uart_write((addr - 0x009C0000) >> 2, (uint8_t)val);
        return;
    }

    /* Backup SRAM: 0x01D00000-0x01D03FFF */
    if (addr >= 0x01D00000 && addr < 0x01D04000) {
        backup_sram_write((addr - 0x01D00000) >> 2, val);
        return;
    }

    /* Link board: 0x01A00000 and its mirror at 0x01A10000 */
    {
        uint32_t off;
        if (comm_offset(addr, &off)) {
            off &= ~3u;
            for (int i = 0; i < 4; i++)
                comm_write8(off + i, (uint8_t)(val >> (i * 8)));
            return;
        }
    }

    /* Render mode: 0x10000000-0x101FFFFF */
    if (addr >= 0x10000000 && addr < 0x10200000) {
        render_mode_write(val);
        return;
    }

    /* Framebuffer A: 0x11600000-0x1167FFFF */
    if (addr >= 0x11600000 && addr < 0x11680000) {
        uint32_t off = (addr - 0x11600000) >> 1;
        fbvram_bankA_write(off, (uint16_t)val);
        fbvram_bankA_write(off + 1, (uint16_t)(val >> 16));
        return;
    }

    /* Framebuffer B: 0x11680000-0x116FFFFF */
    if (addr >= 0x11680000 && addr < 0x11700000) {
        uint32_t off = (addr - 0x11680000) >> 1;
        fbvram_bankB_write(off, (uint16_t)val);
        fbvram_bankB_write(off + 1, (uint16_t)(val >> 16));
        return;
    }

    /* Texture RAM 0: 0x12000000-0x123FFFFF */
    if (addr >= 0x12000000 && addr < 0x12400000) {
        uint32_t off = (addr - 0x12000000) & 0x1FFFFF;
        tex0_write(off >> 2, val);
        return;
    }

    /* Texture RAM 1: 0x12400000-0x127FFFFF */
    if (addr >= 0x12400000 && addr < 0x12800000) {
        uint32_t off = (addr - 0x12400000) & 0x1FFFFF;
        tex1_write(off >> 2, val);
        return;
    }

    /* CRX video board: see bus_read32. */
    if (is_crx() && addr >= 0x11400000 && addr < 0x11410000) {
        lumaram_write((addr - 0x11400000) >> 1, (uint8_t)val);
        return;
    }
    if (is_crx() && addr >= 0x11000000 && addr < 0x11400000) {
        /* Bank 0 at 0x11000000 and bank 1 at 0x11200000, each 1MB mirrored
         * once - the same two memories tex0_write/tex1_write already serve at
         * 0x12000000 on the original board. */
        uint32_t off = (addr - 0x11000000) & 0xFFFFF;
        if (addr < 0x11200000) tex0_write(off >> 2, val);
        else                   tex1_write(off >> 2, val);
        return;
    }

    /* Luma RAM: 0x12800000-0x1281FFFF */
    if (addr >= 0x12800000 && addr < 0x12820000) {
        lumaram_write((addr - 0x12800000) >> 2, (uint8_t)val);
        return;
    }

    /* Unmapped write - silently ignore */
    /* printf("[bus] Unmapped write32: 0x%08X = 0x%08X\n", addr, val); */
}

void bus_write16(uint32_t addr, uint16_t val)
{
    addr &= ~1;

    /* Fast path for work RAM */
    if (addr >= 0x00500000 && addr < 0x00600000) {
        mem_write16(s_workram, addr - 0x00500000, val);
        return;
    }

    if (addr >= 0x00200000 && addr < PROGRAM_RAM_TOP) {
        mem_write16(s_program_ram, addr - 0x00200000, val);
        return;
    }

    /* Tile sync registers */
    if (addr >= 0x01040000 && addr < 0x01040002) {
        tile_xhout_write(val);
        return;
    }
    if (addr >= 0x01060000 && addr < 0x01060002) {
        tile_xvout_write(val);
        return;
    }

    {
        uint32_t off;
        if (comm_offset(addr, &off)) {
            comm_write8(off, (uint8_t)val);
            comm_write8(off + 1, (uint8_t)(val >> 8));
            return;
        }
    }

    /* For other regions, do read-modify-write through 32-bit */
    uint32_t aligned = addr & ~3;
    uint32_t cur = bus_read32(aligned);
    int shift = (addr & 2) * 8;
    uint32_t mask = 0xFFFF << shift;
    cur = (cur & ~mask) | ((uint32_t)val << shift);
    bus_write32(aligned, cur);
}

void bus_write8(uint32_t addr, uint8_t val)
{
    /* Fast path for work RAM */
    if (addr >= 0x00500000 && addr < 0x00600000) {
        s_workram[addr - 0x00500000] = val;
        return;
    }

    if (addr >= 0x00200000 && addr < PROGRAM_RAM_TOP) {
        s_program_ram[addr - 0x00200000] = val;
        return;
    }

    {
        uint32_t off;
        if (comm_offset(addr, &off)) {
            comm_write8(off, val);
            return;
        }
    }

    /* For other regions, do read-modify-write through 32-bit */
    uint32_t aligned = addr & ~3;
    uint32_t cur = bus_read32(aligned);
    int shift = (addr & 3) * 8;
    uint32_t mask = 0xFF << shift;
    cur = (cur & ~mask) | ((uint32_t)val << shift);
    bus_write32(aligned, cur);
}

/* ---- Direct access ---- */

uint8_t *bus_get_workram(void)       { return s_workram; }
uint8_t *bus_get_program_ram(void)   { return s_program_ram; }
uint8_t *bus_get_buffer_ram(void)    { return s_bufferram; }

/* Buffer RAM is where the geometry command stream lives; the engine walks it
 * by dword, so give it a direct accessor rather than routing through the
 * address decoder for every word. */
uint32_t bus_bufferram_read32(uint32_t offset)
{
    if (!s_bufferram) return 0;
    return mem_read32(s_bufferram, offset & 0x1FFFC);
}

void bus_bufferram_write32(uint32_t offset, uint32_t val)
{
    if (!s_bufferram) return;
    mem_write32(s_bufferram, offset & 0x1FFFC, val);
}
uint8_t *bus_get_backup_sram(void)   { return s_backup_sram; }

const uint8_t *bus_get_program_rom(uint32_t *size_out)
{
    if (size_out) *size_out = s_program_rom_size;
    return s_program_rom;
}

/* ---- ROM loading ---- */

static uint8_t *rom_dup(const uint8_t *data, uint32_t size)
{
    if (!data || size == 0) return NULL;
    uint8_t *buf = (uint8_t *)malloc(size);
    if (buf) memcpy(buf, data, size);
    return buf;
}

void bus_load_program_rom(const uint8_t *data, uint32_t size)
{
    free(s_program_rom);
    s_program_rom = rom_dup(data, size);
    s_program_rom_size = s_program_rom ? size : 0;
    printf("[bus] Program ROM loaded: %u bytes @ 0x00000000\n", s_program_rom_size);
}

void bus_load_data_rom(const uint8_t *data, uint32_t size)
{
    free(s_data_rom);
    s_data_rom = rom_dup(data, size);
    s_data_rom_size = s_data_rom ? size : 0;
    printf("[bus] Data ROM loaded: %u bytes @ 0x02000000\n", s_data_rom_size);
}

void bus_load_extra_data(const uint8_t *data, uint32_t size)
{
    free(s_extra_data);
    s_extra_data = rom_dup(data, size);
    s_extra_data_size = s_extra_data ? size : 0;
    printf("[bus] Extra data loaded: %u bytes @ 0x06000000\n", s_extra_data_size);
}

void bus_load_texture_rom(const uint8_t *data, uint32_t size)
{
    free(s_texture_rom);
    s_texture_rom = rom_dup(data, size);
    s_texture_rom_size = s_texture_rom ? size : 0;
    printf("[bus] Texture ROM loaded: %u bytes\n", s_texture_rom_size);
}

const uint16_t *bus_get_texture_rom(uint32_t *words_out)
{
    if (words_out) *words_out = s_texture_rom_size / 2;
    return (const uint16_t *)s_texture_rom;
}

/* Polygon ROM is the same image the i960 sees at 0x06000000. */
const uint32_t *bus_get_polygon_rom(uint32_t *words_out)
{
    if (words_out) *words_out = s_extra_data_size / 4;
    return (const uint32_t *)s_extra_data;
}

const uint8_t *bus_get_data_rom(uint32_t *size_out)
{
    if (size_out) *size_out = s_data_rom_size;
    return s_data_rom;
}

/* Direct work RAM accessors */
uint8_t  bus_workram_read8(uint32_t offset)  { return s_workram[offset & 0xFFFFF]; }
uint16_t bus_workram_read16(uint32_t offset) { return mem_read16(s_workram, offset & 0xFFFFF); }
uint32_t bus_workram_read32(uint32_t offset) { return mem_read32(s_workram, offset & 0xFFFFF); }
void bus_workram_write8(uint32_t offset, uint8_t val)   { s_workram[offset & 0xFFFFF] = val; }
void bus_workram_write16(uint32_t offset, uint16_t val) { mem_write16(s_workram, offset & 0xFFFFF, val); }
void bus_workram_write32(uint32_t offset, uint32_t val) { mem_write32(s_workram, offset & 0xFFFFF, val); }
