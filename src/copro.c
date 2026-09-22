/*
 * Fujitsu MB86233/86234 "TGP" math coprocessor, plus the Model 2 board glue.
 *
 * Ported from MAME (cpu/mb86233/mb86233.cpp and model2_tgp_state in
 * sega/model2.cpp). Only floating-point mode is implemented, which is all any
 * Sega program uses.
 *
 * Driving model: the i960 is recompiled straight-line C, so there is no
 * instruction interleaving to hang the DSP off. The DSP is a pure slave -
 * it loops reading its input FIFO - so it is run on demand: whenever the game
 * reads the output FIFO (or polls it for emptiness) the core runs until it
 * produces a word or starves on an empty input FIFO.
 */

#include "model2recomp/copro.h"
#include "model2recomp/bus.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- status flags ---- */
#define F_ZRD   0x00000002
#define F_SGD   0x00000008
#define F_CPD   0x00000020
#define F_OVD   0x00000080
#define F_UND   0x00000200
#define F_DVZD  0x00000800
#define F_ZRC   0x00000001
#define F_ZX0   0x08000000
#define F_ZX1   0x10000000
#define F_ZX2   0x20000000
#define F_ZC0   0x40000000
#define F_ZC1   0x80000000

#define D_MASK  (F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD)

static inline float u2f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline int32_t sext(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v & (m * 2 - 1)) ^ m) - (int32_t)m;
}

/* ---- CPU state ---- */
static uint32_t s_st, s_a, s_b, s_d, s_p;
static uint32_t s_alu_stmask, s_alu_stset, s_alu_r1, s_alu_r2;
static uint16_t s_ppc, s_pc, s_sp, s_b0, s_b1, s_x0, s_x1, s_i0, s_i1;
static uint16_t s_vsmr, s_pcs[4], s_mask, s_m;
static uint8_t  s_r, s_rpc, s_c0, s_c1, s_sft, s_vsm;
static bool     s_gpio0, s_gpio1, s_gpio2, s_gpio3;
static bool     s_stall, s_starved;
static int      s_icount;

static uint32_t s_prog[0x1000];
static uint32_t s_dram[0x400];
static uint32_t s_rf[16];

/* ---- board glue ---- */
static uint32_t *s_tables;          /* copro_tgp_tables, 0x10000 dwords */
static uint32_t  s_tables_count;
static uint32_t  s_sincos_base, s_inv_base, s_isqrt_base, s_atan_base[4];
static uint32_t  s_bank_reg;
static bool      s_bank_on;

static uint32_t  s_ctl;
static uint32_t  s_upload_cnt;

/* The coprocessor's external data ROM, loaded from copro_data.bin. Sized in
 * dwords and masked to a power of two, as MAME does. */
static uint32_t *s_data;
static uint32_t  s_data_words;
static bool      s_booted;

/* FIFOs. MAME's are 8 deep with flow control that halts the other side; here
 * the i960 half never blocks, so they are simply made deep enough not to. */
#define FIFO_MAX 8192
static uint32_t s_in[FIFO_MAX], s_out[FIFO_MAX];
static uint32_t s_in_head, s_in_tail, s_out_head, s_out_tail;

static void fifo_push(uint32_t *f, uint32_t *head, uint32_t tail, uint32_t v)
{
    uint32_t next = (*head + 1) % FIFO_MAX;
    if (next == tail) return;   /* full: drop (never happens in practice) */
    f[*head] = v;
    *head = next;
}

/* ---- board address spaces ---- */

static uint32_t sincos_r(uint32_t offset)
{
    uint32_t ang = s_sincos_base + offset * 0x4000;
    uint32_t index = ang & 0x3FFF;
    if (ang & 0x4000) {
        int v = 0x4000 - (int)index;
        index = (uint32_t)(v < 0x3FFF ? v : 0x3FFF);
    }
    uint32_t result = index < s_tables_count ? s_tables[index] : 0;
    if (ang & 0x8000)
        result ^= 0x80000000;
    return result;
}

static uint32_t inv_r(uint32_t offset)
{
    uint32_t index = ((s_inv_base >> 9) & 0x3FFE) | (offset & 1);
    index |= 0x8000;
    uint32_t result = index < s_tables_count ? s_tables[index] : 0;
    uint8_t bexp = (s_inv_base >> 23) & 0xFF;
    uint8_t exp = (uint8_t)((result >> 23) + (0x7F - bexp));
    result = (result & 0x007FFFFF) | ((uint32_t)exp << 23);
    if ((s_inv_base & 0x80000000) && offset)
        result |= 0x80000000;
    return result;
}

static uint32_t isqrt_r(uint32_t offset)
{
    uint32_t index = 0x2000 ^ (((s_isqrt_base >> 10) & 0x3FFE) | (offset & 1));
    index |= 0xC000;
    uint32_t result = index < s_tables_count ? s_tables[index] : 0;
    uint8_t bexp = (s_isqrt_base >> 24) & 0x7F;
    uint8_t exp = (uint8_t)((result >> 23) + (0x3F - bexp));
    result = (result & 0x807FFFFF) | ((uint32_t)exp << 23);
    if (!(offset & 1))
        result &= 0x7FFFFFFF;
    return result;
}

static uint32_t atan_r(void)
{
    uint8_t ie = (uint8_t)(0x88 - (s_atan_base[3] >> 23));

    bool s0 = (s_atan_base[0] & 0x80000000) != 0;
    bool s1 = (s_atan_base[1] & 0x80000000) != 0;
    bool s2 = (s_atan_base[0] & 0x7FFFFFFF) <= (s_atan_base[1] & 0x7FFFFFFF);

    uint32_t im = s_atan_base[3] & 0x7FFFFF;
    uint32_t index = ie <= 0x17 ? (im | 0x800000) >> ie : 0;
    if (index == 0x4000)
        index = 0x3FFF;
    index |= 0x4000;

    uint32_t result = index < s_tables_count ? s_tables[index] : 0;

    if (s0 ^ s1 ^ s2)
        result >>= 16;
    if (s2)
        result += 0x4000;
    if ((s0 && !s2) || (s1 && s2))
        result += 0x8000;

    return result & 0xFFFF;
}

/* Banked external window: the high half of the bank register supplies the top
 * address bits. Two devices hang off it - buffer RAM at bit 22, and the
 * coprocessor's own data ROM socket at bit 23.
 *
 * ponytail: the data ROM socket reads zero. It is empty on Virtua Cop, so
 * there is nothing here to test against. A title that fills it (Daytona USA
 * puts 4MB of collision and height-map data there) needs a copro_data region
 * loaded and returned here, masked to the region size in dwords - see
 * model2_tgp_state::copro_tgp_memory_r and docs/technical/porting-targets.md. */
static uint32_t tgp_memory_r(uint32_t offset)
{
    uint32_t adr = (s_bank_reg & 0xFF0000) | offset;
    if (adr & 0x800000) {
        /* The coprocessor's external data socket. Virtua Cop leaves it empty
         * and this returned zero for every title because of that; Daytona USA
         * fills it with 4 MB of collision meshes and height maps and reads
         * them through this window. MAME masks the address to the region size
         * in dwords, which is what makes a partly-filled socket wrap rather
         * than read past its end. */
        if (!s_data || !s_data_words) return 0;
        return s_data[adr & (s_data_words - 1)];
    }
    if (adr & 0x400000) return bus_bufferram_read32((adr & 0x7FFF) * 4);
    return 0;
}

static void tgp_memory_w(uint32_t offset, uint32_t data)
{
    uint32_t adr = (s_bank_reg & 0xFF0000) | offset;
    if (adr & 0x400000)
        bus_bufferram_write32((adr & 0x7FFF) * 4, data);
}

static uint32_t io_read(uint16_t adr)
{
    /* The bank view covers the whole IO space when enabled, shadowing the
     * math table ports - the microcode turns it off before using those. */
    if (s_bank_on)
        return tgp_memory_r(adr);

    if (adr >= 0x20 && adr <= 0x23) return sincos_r(adr - 0x20);
    if (adr >= 0x24 && adr <= 0x27) return atan_r();
    if (adr >= 0x28 && adr <= 0x29) return inv_r(adr - 0x28);
    if (adr >= 0x2A && adr <= 0x2B) return isqrt_r(adr - 0x2A);
    return 0;
}

static void io_write(uint16_t adr, uint32_t data)
{
    if (s_bank_on) {
        tgp_memory_w(adr, data);
        return;
    }

    if (adr >= 0x20 && adr <= 0x23) { s_sincos_base = data; return; }
    if (adr >= 0x24 && adr <= 0x27) {
        s_atan_base[adr - 0x24] = data;
        s_gpio0 = (s_atan_base[0] & 0x7FFFFFFF) <= (s_atan_base[1] & 0x7FFFFFFF);
        return;
    }
    if (adr >= 0x28 && adr <= 0x29) { s_inv_base = data; return; }
    if (adr >= 0x2A && adr <= 0x2B) { s_isqrt_base = data; return; }
}

static uint32_t data_read(uint16_t adr)
{
    return adr < 0x400 ? s_dram[adr] : 0;
}

static void data_write(uint16_t adr, uint32_t v)
{
    if (adr < 0x400) s_dram[adr] = v;
}

static uint32_t rf_read(uint32_t adr)
{
    if ((adr & 0xF) == 1) {
        if (s_in_head == s_in_tail) {     /* pop on empty: stall and retry */
            s_stall = true;
            s_starved = true;
            return 0;
        }
        uint32_t v = s_in[s_in_tail];
        s_in_tail = (s_in_tail + 1) % FIFO_MAX;
        return v;
    }
    return s_rf[adr & 0xF];
}

static void rf_write(uint32_t adr, uint32_t v)
{
    switch (adr & 0xF) {
    case 0: break;                                       /* leds/busy */
    case 2: fifo_push(s_out, &s_out_head, s_out_tail, v); break;
    case 3:
        s_bank_reg = v;
        s_bank_on = (v & 0xC00000) != 0;
        break;
    default: s_rf[adr & 0xF] = v; break;
    }
}

/* ---- ALU ---- */

static uint32_t set_exp(uint32_t val, uint32_t exp)
{
    return (val & 0x807FFFFF) | ((exp & 0xFF) << 23);
}

static uint32_t set_mant(uint32_t val, uint32_t mant)
{
    return (val & 0x7F800000) | ((mant & 0x00800000) << 8) | (mant & 0x007FFFFF);
}

static uint32_t get_exp(uint32_t val) { return (val >> 23) & 0xFF; }

static uint32_t get_mant(uint32_t val)
{
    return (val & 0x80000000) ? (val | 0x7F800000) : (val & 0x807FFFFF);
}

static void stset_sz_int(uint32_t val)
{
    s_alu_stset = val ? ((val & 0x80000000) ? F_SGD : 0) : F_ZRD;
}

static void stset_sz_fp(uint32_t val)
{
    s_alu_stset = (val & 0x7FFFFFFF) ? ((val & 0x80000000) ? F_SGD : 0) : F_ZRD;
}

static void alu_pre(uint32_t alu)
{
    s_alu_stmask = D_MASK;
    switch (alu) {
    case 0x00: s_alu_stmask = 0; break;                                   /* no alu */
    case 0x01: s_alu_r1 = s_d & s_a;  stset_sz_int(s_alu_r1); break;      /* andd */
    case 0x02: s_alu_r1 = s_d | s_a;  stset_sz_int(s_alu_r1); break;      /* orad */
    case 0x03: s_alu_r1 = s_d ^ s_a;  stset_sz_int(s_alu_r1); break;      /* eord */
    case 0x04: s_alu_r1 = ~s_d;       stset_sz_int(s_alu_r1); break;      /* notd */
    case 0x05: stset_sz_fp(f2u(u2f(s_d) - u2f(s_a))); break;              /* fcpd */
    case 0x06: s_alu_r1 = f2u(u2f(s_d) + u2f(s_a)); stset_sz_fp(s_alu_r1); break; /* fadd */
    case 0x07: s_alu_r1 = f2u(u2f(s_d) - u2f(s_a)); stset_sz_fp(s_alu_r1); break; /* fsbd */
    case 0x08:                                                            /* fml */
        s_alu_stmask = 0;
        s_alu_r1 = f2u(u2f(s_a) * u2f(s_b));
        s_alu_stset = 0;
        break;
    case 0x09:                                                            /* fmsd */
        s_alu_r1 = f2u(u2f(s_d) + u2f(s_p));
        s_alu_r2 = f2u(u2f(s_a) * u2f(s_b));
        stset_sz_fp(s_alu_r1);
        break;
    case 0x0A:                                                            /* fmrd */
        s_alu_r1 = f2u(u2f(s_d) - u2f(s_p));
        s_alu_r2 = f2u(u2f(s_a) * u2f(s_b));
        stset_sz_fp(s_alu_r1);
        break;
    case 0x0B: s_alu_r1 = s_d & 0x7FFFFFFF; stset_sz_fp(s_alu_r1); break; /* fabd */
    case 0x0C: s_alu_r1 = f2u(u2f(s_d) + u2f(s_p)); stset_sz_fp(s_alu_r1); break; /* fsmd */
    case 0x0D:                                                            /* fspd */
        s_alu_r1 = s_p;
        s_alu_r2 = f2u(u2f(s_a) * u2f(s_b));
        stset_sz_fp(s_alu_r1);
        break;
    case 0x0E: s_alu_r1 = f2u((float)(int32_t)s_d); stset_sz_int(s_alu_r1); break; /* cxfd */
    case 0x0F:                                                            /* cfxd */
        switch ((s_m >> 1) & 3) {
        case 0: s_alu_r1 = (uint32_t)(int32_t)roundf(u2f(s_d)); break;
        case 1: s_alu_r1 = (uint32_t)(int32_t)ceilf(u2f(s_d));  break;
        case 2: s_alu_r1 = (uint32_t)(int32_t)floorf(u2f(s_d)); break;
        case 3: s_alu_r1 = (uint32_t)(int32_t)u2f(s_d);         break;
        }
        stset_sz_int(s_alu_r1);
        break;
    case 0x10: s_alu_r1 = f2u(u2f(s_d) / u2f(s_a)); stset_sz_fp(s_alu_r1); break; /* fdvd */
    case 0x11: s_alu_r1 = s_d ? s_d ^ 0x80000000 : 0; stset_sz_fp(s_alu_r1); break; /* fned */
    case 0x13: s_alu_r1 = f2u(u2f(s_b) + u2f(s_a)); stset_sz_fp(s_alu_r1); break;
    case 0x14: s_alu_r1 = f2u(u2f(s_b) - u2f(s_a)); stset_sz_fp(s_alu_r1); break;
    case 0x16: s_alu_r1 = s_d >> s_sft; stset_sz_int(s_alu_r1); break;    /* lsrd */
    case 0x17: s_alu_r1 = s_d << s_sft; stset_sz_int(s_alu_r1); break;    /* lsld */
    case 0x18: s_alu_r1 = (uint32_t)((int32_t)s_d >> s_sft); stset_sz_int(s_alu_r1); break; /* asrd */
    case 0x19: s_alu_r1 = (uint32_t)((int32_t)s_d << s_sft); stset_sz_int(s_alu_r1); break; /* asld */
    case 0x1A: s_alu_r1 = s_d + s_a; stset_sz_int(s_alu_r1); break;       /* addd */
    case 0x1B: s_alu_r1 = s_d - s_a; stset_sz_int(s_alu_r1); break;       /* subd */
    default: s_alu_stmask = 0; break;
    }
}

static void alu_update_st(void)
{
    s_st = (s_st & ~s_alu_stmask) | s_alu_stset;
}

/* integer results land immediately; float ones only after the transfer */
static void alu_post_1(uint32_t alu)
{
    switch (alu) {
    case 0x01: case 0x02: case 0x03: case 0x04:
    case 0x0E: case 0x0F: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
        s_d = s_alu_r1;
        alu_update_st();
        break;
    default: break;
    }
}

static void alu_post_2(uint32_t alu)
{
    switch (alu) {
    case 0x05:
        alu_update_st();
        s_icount--;
        break;
    case 0x06: case 0x07: case 0x0B: case 0x0C:
    case 0x10: case 0x11: case 0x13: case 0x14:
        s_d = s_alu_r1;
        alu_update_st();
        s_icount--;
        break;
    case 0x08:
        s_p = s_alu_r1;
        s_icount--;
        break;
    case 0x09: case 0x0A: case 0x0D:
        s_d = s_alu_r1;
        s_p = s_alu_r2;
        alu_update_st();
        s_icount--;
        break;
    default: break;
    }
}

/* ---- addressing ---- */

static uint16_t ea_pre_0(uint32_t r)
{
    switch (r & 0x180) {
    case 0x000: return (uint16_t)(r & 0x7F);
    case 0x080: case 0x100: return (uint16_t)((r & 0x7F) + s_b0 + s_x0);
    case 0x180:
        switch (r & 0x60) {
        case 0x00: return (uint16_t)(s_b0 + s_x0);
        case 0x20: return s_x0;
        case 0x40: return (uint16_t)(s_b0 + (s_x0 & s_vsmr));
        case 0x60: return (uint16_t)(s_x0 & s_vsmr);
        }
    }
    return 0;
}

static void ea_post_0(uint32_t r)
{
    if (!(r & 0x100)) return;
    if (!(r & 0x080)) s_x0 += s_i0;
    else              s_x0 = (uint16_t)(s_x0 + sext(r, 5));
}

static uint16_t ea_pre_1(uint32_t r)
{
    switch (r & 0x180) {
    case 0x000: return (uint16_t)(r & 0x7F);
    case 0x080: case 0x100: return (uint16_t)((r & 0x7F) + s_b1 + s_x1);
    case 0x180:
        switch (r & 0x60) {
        case 0x00: return (uint16_t)(s_b1 + s_x1);
        case 0x20: return s_x1;
        case 0x40: return (uint16_t)(s_b1 + (s_x1 & s_vsmr));
        case 0x60: return (uint16_t)(s_x1 & s_vsmr);
        }
    }
    return 0;
}

static void ea_post_1(uint32_t r)
{
    if (!(r & 0x100)) return;
    if (!(r & 0x080)) s_x1 += s_i1;
    else              s_x1 = (uint16_t)(s_x1 + sext(r, 5));
}

static void pcs_push(void)
{
    for (int i = 3; i; i--) s_pcs[i] = s_pcs[i - 1];
    s_pcs[0] = s_pc;
}

static void pcs_pop(void)
{
    s_pc = s_pcs[0];
    for (int i = 0; i != 3; i++) s_pcs[i] = s_pcs[i + 1];
}

/* ---- registers ---- */

static uint32_t read_reg(uint32_t r)
{
    r &= 0x3F;
    if (r >= 0x20 && r < 0x30) return rf_read(r & 0x1F);
    switch (r) {
    case 0x00: return s_b0;
    case 0x01: return s_b1;
    case 0x02: return s_x0;
    case 0x03: return s_x1;
    case 0x0C: return s_c0;
    case 0x0D: return s_c1;
    case 0x10: return s_a;
    case 0x11: return get_exp(s_a);
    case 0x12: return get_mant(s_a);
    case 0x13: return s_b;
    case 0x14: return get_exp(s_b);
    case 0x15: return get_mant(s_b);
    case 0x19: return s_d;
    case 0x1A: return get_exp(s_d);
    case 0x1B: return get_mant(s_d);
    case 0x1C: return s_p;
    case 0x1D: return get_exp(s_p);
    case 0x1E: return get_mant(s_p);
    case 0x1F: return s_sft;
    case 0x34: return s_rpc;
    default: return 0;
    }
}

static void write_reg(uint32_t r, uint32_t v)
{
    r &= 0x3F;
    if (r >= 0x20 && r < 0x30) { rf_write(r & 0x1F, v); return; }
    switch (r) {
    case 0x00: s_b0 = (uint16_t)v; break;
    case 0x01: s_b1 = (uint16_t)v; break;
    case 0x02: s_x0 = (uint16_t)v; break;
    case 0x03: s_x1 = (uint16_t)v; break;
    case 0x05: s_i0 = (uint16_t)v; break;
    case 0x06: s_i1 = (uint16_t)v; break;
    case 0x08: s_sp = (uint16_t)v; break;
    case 0x0A: s_vsm = v & 7; s_vsmr = (uint16_t)((8 << s_vsm) - 1); break;
    case 0x0C:
        s_c0 = (uint8_t)v;
        if (s_c0 == 1) s_st |= F_ZC0; else s_st &= ~F_ZC0;
        break;
    case 0x0D:
        s_c1 = (uint8_t)v;
        if (s_c1 == 1) s_st |= F_ZC1; else s_st &= ~F_ZC1;
        break;
    case 0x0F: break;
    case 0x10: s_a = v; break;
    case 0x11: s_a = set_exp(s_a, v); break;
    case 0x12: s_a = set_mant(s_a, v); break;
    case 0x13: s_b = v; break;
    case 0x14: s_b = set_exp(s_b, v); break;
    case 0x15: s_b = set_mant(s_b, v); break;
    case 0x19: s_d = v; break;
    case 0x1A: s_d = set_exp(s_d, v); break;
    case 0x1B: s_d = set_mant(s_d, v); break;
    case 0x1C: s_p = v; break;
    case 0x1D: s_p = set_exp(s_p, v); break;
    case 0x1E: s_p = set_mant(s_p, v); break;
    case 0x1F: s_sft = (uint8_t)v; break;
    case 0x34: s_rpc = (uint8_t)v; break;
    case 0x3C: s_mask = (uint16_t)v; break;
    default: break;
    }
}

static void write_mem_internal_1(uint32_t r, uint32_t v, bool bank)
{
    uint16_t ea = ea_pre_1(r);
    if (bank) ea = (uint16_t)(ea + 0x200);
    data_write(ea, v);
    ea_post_1(r);
}

static void write_mem_io_1(uint32_t r, uint32_t v)
{
    uint16_t ea = ea_pre_1(r);
    io_write(ea, v);
    ea_post_1(r);
}

/* ---- execute ---- */

static void copro_reset(void)
{
    s_pc = 0;
    s_ppc = 0;
    s_st = F_ZRC|F_ZRD|F_ZX0|F_ZX1|F_ZX2|F_ZC0|F_ZC1;
    s_sp = 0;
    s_a = s_b = s_d = s_p = 0;
    s_r = s_rpc = s_c0 = s_c1 = 1;
    s_b0 = s_b1 = s_x0 = s_x1 = s_i0 = s_i1 = 0;
    s_sft = 0;
    s_vsm = 0;
    s_vsmr = 7;
    s_mask = 0;
    s_m = 1;
    s_alu_stmask = s_alu_stset = s_alu_r1 = s_alu_r2 = 0;
    memset(s_pcs, 0, sizeof(s_pcs));
    memset(s_dram, 0, sizeof(s_dram));
    memset(s_rf, 0, sizeof(s_rf));
    s_stall = false;
    s_bank_reg = 0;
    s_bank_on = false;
}

static void copro_execute(int cycles)
{
    s_icount = cycles;

    while (s_icount > 0) {
        s_ppc = s_pc;
        uint32_t opcode = s_prog[s_pc++ & 0xFFF];

        switch ((opcode >> 26) & 0x3F) {
        case 0x00: {    /* lab */
            uint32_t r1 = opcode & 0x1FF;
            uint32_t r2 = (opcode >> 9) & 0x1FF;
            uint32_t alu = (opcode >> 21) & 0x1F;
            uint32_t op = (opcode >> 18) & 0x7;

            alu_pre(alu);

            switch (op) {
            case 0: case 1: {
                uint32_t ea1 = ea_pre_0(r1);
                uint32_t v1 = data_read((uint16_t)ea1);
                if (s_stall) goto do_stall;
                uint32_t ea2 = ea_pre_1(r2);
                uint32_t v2 = io_read((uint16_t)ea2);
                if (s_stall) goto do_stall;
                ea_post_0(r1);
                ea_post_1(r2);
                s_a = v1;
                s_b = v2;
                break;
            }
            case 3: {
                uint32_t ea1 = ea_pre_0(r1);
                uint32_t v1 = data_read((uint16_t)ea1);
                if (s_stall) goto do_stall;
                uint32_t ea2 = ea_pre_1(r2) + 0x200;
                uint32_t v2 = data_read((uint16_t)ea2);
                if (s_stall) goto do_stall;
                ea_post_0(r1);
                ea_post_1(r2);
                s_a = v1;
                s_b = v2;
                break;
            }
            case 4: {
                uint32_t ea1 = ea_pre_0(r1) + 0x200;
                uint32_t v1 = data_read((uint16_t)ea1);
                if (s_stall) goto do_stall;
                uint32_t ea2 = ea_pre_1(r2);
                uint32_t v2 = data_read((uint16_t)ea2);
                if (s_stall) goto do_stall;
                ea_post_0(r1);
                ea_post_1(r2);
                s_a = v1;
                s_b = v2;
                break;
            }
            default: break;
            }

            alu_post_1(alu);
            alu_post_2(alu);
            break;
        }

        case 0x07: {    /* ld / mov */
            uint32_t r1 = opcode & 0x1FF;
            uint32_t r2 = (opcode >> 9) & 0x1FF;
            uint32_t alu = (opcode >> 21) & 0x1F;
            uint32_t op = (opcode >> 18) & 0x7;

            alu_pre(alu);

            switch (op) {
            case 0: case 1: {
                uint32_t ea = ea_pre_0(r1);
                uint32_t v = data_read((uint16_t)ea);
                if (s_stall) goto do_stall;
                ea_post_0(r1);
                alu_post_1(alu);
                write_mem_io_1(r2, v);
                break;
            }
            case 2: {
                uint32_t ea = ea_pre_0(r1);
                uint32_t v = io_read((uint16_t)ea);
                if (s_stall) goto do_stall;
                ea_post_0(r1);
                alu_post_1(alu);
                write_mem_internal_1(r2, v, false);
                break;
            }
            case 3: {
                uint32_t ea = ea_pre_0(r1);
                uint32_t v = data_read((uint16_t)ea);
                if (s_stall) goto do_stall;
                ea_post_0(r1);
                alu_post_1(alu);
                write_mem_internal_1(r2, v, true);
                break;
            }
            case 4: {
                uint32_t ea = ea_pre_0(r1) + 0x200;
                uint32_t v = data_read((uint16_t)ea);
                if (s_stall) goto do_stall;
                ea_post_0(r1);
                alu_post_1(alu);
                write_mem_internal_1(r2, v, false);
                break;
            }
            case 5: {
                uint32_t ea = ea_pre_0(r1);
                uint32_t v = s_prog[ea & 0xFFF];
                if (s_stall) goto do_stall;
                ea_post_0(r1);
                alu_post_1(alu);
                write_mem_internal_1(r2, v, false);
                break;
            }
            case 7:
                switch (r2 >> 6) {
                case 0: {
                    uint32_t v = read_reg(r2);
                    if (s_stall) goto do_stall;
                    alu_post_1(alu);
                    write_mem_internal_1(r1, v, false);
                    break;
                }
                case 1: {
                    uint32_t v = read_reg(r2);
                    if (s_stall) goto do_stall;
                    alu_post_1(alu);
                    write_mem_io_1(r1, v);
                    break;
                }
                case 2: {
                    uint32_t ea = ea_pre_1(r1) + 0x200;
                    uint32_t v = data_read((uint16_t)ea);
                    if (s_stall) goto do_stall;
                    ea_post_1(r1);
                    alu_post_1(alu);
                    write_reg(r2, v);
                    break;
                }
                case 3: {
                    uint32_t ea = ea_pre_1(r1);
                    uint32_t v = data_read((uint16_t)ea);
                    if (s_stall) goto do_stall;
                    ea_post_1(r1);
                    alu_post_1(alu);
                    write_reg(r2, v);
                    break;
                }
                case 4: {
                    uint32_t ea = ea_pre_1(r1);
                    uint32_t v = io_read((uint16_t)ea);
                    if (s_stall) goto do_stall;
                    ea_post_1(r1);
                    alu_post_1(alu);
                    write_reg(r2, v);
                    break;
                }
                case 5: {
                    uint32_t ea = ea_pre_0(r1);
                    uint32_t v = s_prog[ea & 0xFFF];
                    if (s_stall) goto do_stall;
                    ea_post_0(r1);
                    alu_post_1(alu);
                    write_reg(r2, v);
                    break;
                }
                case 6: {
                    uint32_t v = read_reg(r1);
                    if (s_stall) goto do_stall;
                    alu_post_1(alu);
                    write_reg(r2, v);
                    break;
                }
                default: alu_post_1(alu); break;
                }
                break;

            default: alu_post_1(alu); break;
            }

            alu_post_2(alu);
            break;
        }

        case 0x0D:      /* stm/clm - only stmh (rounding/fp mode) is used */
            if (((opcode >> 17) & 7) == 5)
                s_m = (uint16_t)opcode;
            break;

        case 0x0E:      /* lipl / lia / lib / lid */
            switch ((opcode >> 24) & 0x3) {
            case 0: s_p = opcode & 0xFFFFFF; break;
            case 1: s_a = (uint32_t)sext(opcode, 24); break;
            case 2: s_b = (uint32_t)sext(opcode, 24); break;
            case 3: s_d = (uint32_t)sext(opcode, 24); break;
            }
            break;

        case 0x0F: {    /* rep / clr0 / clr1 / set */
            uint32_t alu = (opcode >> 20) & 0x1F;
            uint32_t sub2 = (opcode >> 17) & 7;

            alu_pre(alu);

            switch (sub2) {
            case 0:
                if (opcode & 0x0004) s_a = 0;
                if (opcode & 0x0008) s_b = 0;
                if (opcode & 0x0010) s_d = 0;
                break;
            case 1: break;
            case 2: {
                uint32_t r = (opcode & 0x8000) ? read_reg(opcode) : opcode;
                if (s_stall) goto do_stall;
                s_r = (uint8_t)r;
                goto rep_start;
            }
            case 3: break;
            default: break;
            }

            alu_post_1(alu);
            break;
        }

        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x14: case 0x15: case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            /* ldi */
            write_reg(opcode >> 24, (uint32_t)sext(opcode, 24));
            break;

        case 0x2F: case 0x3F: {     /* conditional branch of every kind */
            uint32_t cond = (opcode >> 20) & 0x1F;
            uint32_t subtype = (opcode >> 17) & 7;
            uint32_t data = opcode & 0xFFFF;
            bool invert = (opcode & 0x40000000) != 0;
            bool passed = false;

            switch (cond) {
            case 0x00: passed = (s_st & F_ZRD) != 0; break;
            case 0x01: passed = !(s_st & F_SGD); break;
            case 0x02: passed = (s_st & (F_ZRD | F_SGD)) != 0; break;
            case 0x0A: passed = s_gpio0; break;
            case 0x0B: passed = s_gpio1; break;
            case 0x0C: passed = s_gpio2; break;
            case 0x10: passed = !(s_st & F_ZC0); break;
            case 0x11: passed = !(s_st & F_ZC1); break;
            case 0x12: passed = s_gpio3; break;
            case 0x16: passed = true; break;
            default: break;
            }
            if (invert) passed = !passed;

            if (passed) {
                switch (subtype) {
                case 0: s_pc = (uint16_t)data; break;
                case 1:
                    if (opcode & 0x4000) {
                        uint32_t v = read_reg(opcode);
                        if (s_stall) goto do_stall;
                        s_pc = (uint16_t)v;
                    } else {
                        uint32_t ea = ea_pre_0(opcode);
                        uint32_t v = data_read((uint16_t)ea);
                        if (s_stall) goto do_stall;
                        ea_post_0(opcode);
                        s_pc = (uint16_t)v;
                    }
                    break;
                case 2: pcs_push(); s_pc = (uint16_t)data; break;
                case 3:
                    if (opcode & 0x4000) {
                        uint32_t v = read_reg(opcode);
                        if (s_stall) goto do_stall;
                        pcs_push();
                        s_pc = (uint16_t)v;
                    } else {
                        uint32_t ea = ea_pre_0(opcode);
                        uint32_t v = data_read((uint16_t)ea);
                        if (s_stall) goto do_stall;
                        ea_post_0(opcode);
                        pcs_push();
                        s_pc = (uint16_t)v;
                    }
                    break;
                case 5: pcs_pop(); break;
                case 6: {
                    uint32_t ea = ea_pre_0(opcode);
                    uint32_t v = data_read((uint16_t)ea);
                    if (s_stall) goto do_stall;
                    ea_post_0(opcode);
                    write_reg(opcode >> 9, v);
                    break;
                }
                default: break;
                }
            }

            if (subtype < 2) {
                if (cond == 0x10 && s_c0 != 1) {
                    s_c0--;
                    if (s_c0 == 1) s_st |= F_ZC0;
                } else if (cond == 0x11 && s_c1 != 1) {
                    s_c1--;
                    if (s_c1 == 1) s_st |= F_ZC1;
                }
            }
            break;
        }

        default: break;
        }

        if (s_r != 1) { s_pc = s_ppc; s_r--; }
        goto cycle_end;

    rep_start:
        goto cycle_end;

    do_stall:
        s_pc = s_ppc;
        s_stall = false;

    cycle_end:
        s_icount--;
        if (s_starved)
            return;     /* nothing left to chew on */
    }
}

/* Run until the DSP produces a word or runs out of input. */
static void copro_pump(void)
{
    if (!s_booted) return;
    if (s_in_head == s_in_tail && s_out_head != s_out_tail) return;

    s_starved = false;
    copro_execute(2000000);
    if (s_icount <= 0) fprintf(stderr, "[copro] budget exhausted at pc=%04X\n", s_ppc);
}

/* ---- i960-facing ports ---- */

void copro_load_data(const uint8_t *data, uint32_t size)
{
    free(s_data);
    s_data_words = size / 4;
    /* The mask below needs a power of two; region sizes are, but be explicit
     * rather than trust the caller. */
    uint32_t p2 = 1;
    while (p2 * 2 <= s_data_words) p2 *= 2;
    s_data_words = p2;
    s_data = (uint32_t *)malloc(s_data_words ? s_data_words * 4 : 4);
    if (s_data && s_data_words) memcpy(s_data, data, s_data_words * 4);
    printf("[copro] TGP data ROM loaded: %u dwords\n", s_data_words);
}

void copro_load_tables(const uint8_t *data, uint32_t size)
{
    free(s_tables);
    s_tables_count = size / 4;
    s_tables = (uint32_t *)malloc(size ? size : 4);
    if (s_tables && size) memcpy(s_tables, data, size);
    printf("[copro] TGP math tables loaded: %u entries\n", s_tables_count);
}

void copro_ctl_write(uint32_t data)
{
    if ((data ^ s_ctl) == 0x80000000) {
        if (data & 0x80000000) {
            s_upload_cnt = 0;
            s_booted = false;
        } else {
            printf("[copro] Booting TGP, %u dwords of microcode\n", s_upload_cnt);
            copro_reset();
            s_booted = true;
        }
    }
    s_ctl = data;
}

uint32_t copro_ctl_read(void)
{
    return s_ctl;
}

void copro_function_write(uint32_t offset, uint32_t data)
{
    uint32_t a = (offset >> 2) & 0xFF;
    fifo_push(s_in, &s_in_head, s_in_tail, (data & 0x800FFFFF) | (a << 23));
}

void copro_fifo_write(uint32_t data)
{
    if (s_ctl & 0x80000000) {
        if (s_upload_cnt < 0x1000)
            s_prog[s_upload_cnt] = data;
        s_upload_cnt++;
        return;
    }
    fifo_push(s_in, &s_in_head, s_in_tail, data);
}

uint32_t copro_fifo_read(void)
{
    copro_pump();
    if (s_out_head == s_out_tail)
        return 0;
    uint32_t v = s_out[s_out_tail];
    s_out_tail = (s_out_tail + 1) % FIFO_MAX;
    return v;
}

bool copro_output_empty(void)
{
    copro_pump();
    return s_out_head == s_out_tail;
}

/* 0x00980014. MAME's copro_status_r: all ones until the coprocessor has been
 * given a program, zero afterwards. Sonic the Fighters reads bit 1 of it and
 * MAME's own comment says the purpose is unknown - which is a reason to match
 * the reference exactly rather than to guess, and a reason it matters on the
 * CRX boards, where there is no coprocessor here to upload anything to. */
uint32_t copro_status_read(void)
{
    return s_upload_cnt ? 0u : 0xFFFFFFFFu;
}
