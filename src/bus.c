/*
 * Model 2 memory bus implementation.
 *
 * Routes 32-bit addresses to the appropriate hardware subsystem.
 * Reference: MAME model2.cpp memory maps (BSD-3-Clause)
 */

#include "model2recomp/bus.h"
#include "model2recomp/model2recomp.h"
#include "model2recomp/video.h"
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
static uint8_t *s_program_ram = NULL;   /* 0x00200000, 128KB (Model 2 original) */
static uint8_t *s_program_rom_ext = NULL; /* 0x00220000, 128KB (mapped from ROM offset 0x20000) */
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

void bus_init(void)
{
    s_program_ram = (uint8_t *)calloc(1, 0x20000);   /* 128KB */
    s_workram     = (uint8_t *)calloc(1, 0x100000);  /* 1MB */
    s_bufferram   = (uint8_t *)calloc(1, 0x20000);   /* 128KB */
    s_cpu_control = (uint8_t *)calloc(1, 0x38);      /* 56 bytes */
    s_tile_ram    = (uint8_t *)calloc(1, 0x10000);   /* 64KB */
    s_char_ram    = (uint8_t *)calloc(1, 0x80000);   /* 512KB */
    s_backup_sram = (uint8_t *)calloc(1, 0x4000);    /* 16KB */
    memset(s_dpram, 0xFF, sizeof(s_dpram));

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


/* ---- Bus read ---- */

uint32_t bus_read32(uint32_t addr)
{
    addr &= ~3; /* Align to 4 bytes */

    /* Program ROM: 0x00000000-0x001FFFFF */
    if (addr < 0x00200000) {
        if (s_program_rom && addr < s_program_rom_size)
            return mem_read32(s_program_rom, addr);
        return 0;
    }

    /* Program RAM: 0x00200000-0x0021FFFF (Model 2 original) */
    if (addr >= 0x00200000 && addr < 0x00220000) {
        return mem_read32(s_program_ram, addr - 0x00200000);
    }

    /* Program ROM extension: 0x00220000-0x0023FFFF */
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
            case 0: return copro_ctl1_read();       /* 0x00980000 */
            case 1: return fifo_control_read();     /* 0x00980004 */
            /* Field status: the frame boundary for the recompiled game. */
            case 3: return model2recomp_field_sync(); /* 0x0098000C */
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

    /* DPRAM (I/O board): 0x01C00000-0x01C00FFF, mask 0x00FF00FF */
    if (addr >= 0x01C00000 && addr < 0x01C01000) {
        uint32_t off = addr - 0x01C00000;
        return (uint32_t)dpram_read(off) | ((uint32_t)dpram_read(off + 2) << 16);
    }

    /* UART: 0x01C80000-0x01C80003 */
    if (addr >= 0x01C80000 && addr < 0x01C80004) {
        return (uint32_t)uart_read(addr - 0x01C80000);
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
        return (uint32_t)lumaram_read(addr - 0x12800000);
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

    if (addr >= 0x00200000 && addr < 0x00220000)
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

    if (addr >= 0x00200000 && addr < 0x00220000)
        return s_program_ram[addr - 0x00200000];

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
    addr &= ~3;

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

    /* Program RAM: 0x00200000-0x0021FFFF */
    if (addr >= 0x00200000 && addr < 0x00220000) {
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
        copro_function_port_write(val);
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
            case 0: copro_ctl1_write(val); return;
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

    /* DPRAM: 0x01C00000-0x01C00FFF */
    if (addr >= 0x01C00000 && addr < 0x01C01000) {
        uint32_t off = addr - 0x01C00000;
        dpram_write(off, (uint8_t)val);
        dpram_write(off + 2, (uint8_t)(val >> 16));
        return;
    }

    /* UART: 0x01C80000-0x01C80003 */
    if (addr >= 0x01C80000 && addr < 0x01C80004) {
        uart_write(addr - 0x01C80000, (uint8_t)val);
        return;
    }

    /* Backup SRAM: 0x01D00000-0x01D03FFF */
    if (addr >= 0x01D00000 && addr < 0x01D04000) {
        backup_sram_write((addr - 0x01D00000) >> 2, val);
        return;
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

    /* Luma RAM: 0x12800000-0x1281FFFF */
    if (addr >= 0x12800000 && addr < 0x12820000) {
        lumaram_write(addr - 0x12800000, (uint8_t)val);
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

    if (addr >= 0x00200000 && addr < 0x00220000) {
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

    if (addr >= 0x00200000 && addr < 0x00220000) {
        s_program_ram[addr - 0x00200000] = val;
        return;
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
