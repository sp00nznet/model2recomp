/*
 * i960KB instruction semantics, as macros.
 *
 * The generated C the lifter emits is a sequence of calls to these: one per
 * guest instruction, all operating on the global g_i960 context. The i960 is
 * the Model 2's CPU, so this is board-level - every game built on
 * model2recomp includes this same header.
 *
 * The i960KB instruction set includes:
 *   - REG operations (add, sub, mul, div, and, or, xor, shifts)
 *   - COBR (compare and branch)
 *   - CTRL (unconditional branches, calls)
 *   - MEM (load/store)
 *   - Floating point (addr, subr, mulr, divr)
 *
 * All operations work on the global g_i960 context.
 */

#ifndef MODEL2RECOMP_I960_OPS_H
#define MODEL2RECOMP_I960_OPS_H

#include "model2recomp/i960.h"
#include "model2recomp/bus.h"
#include "model2recomp/model2recomp.h"
#include <math.h>
#include <string.h>

/* MODI: floored modulo. Unlike remi, the result takes the sign of the
 * divisor, so -7 modi 3 is 2 where -7 remi 3 is -1. Daytona indexes the
 * link ring's node slots with it. Semantics from MAME's i960 core. */
static inline uint32_t op_modi(uint32_t src1, uint32_t src2)
{
    int32_t a = (int32_t)src1, b = (int32_t)src2;
    if (a == 0) return 0;
    int32_t dst = b - ((b / a) * a);
    if (((int64_t)b * (int64_t)a) < 0 && dst != 0)
        dst += a;
    return (uint32_t)dst;
}

/* ---- Busy-waits ---- */

/*
 * A guest busy-wait that never touches the bus spins inside one lifted C
 * function, so nothing else runs and the interrupt it is waiting for is never
 * delivered. Daytona's initialisation waits for the VBlank handler to count
 * three fields into work RAM and would hang there forever.
 *
 * The lifter emits this on tight backward branches. model2recomp_field_sync()
 * is wall-clock rate-limited already, so the only real cost is the counter -
 * which is per translation unit and deliberately unsynchronised; it decides
 * how often to ask, not what the answer is.
 */
static inline void i960_spin_poll(void)
{
    static unsigned s_spins;
    if ((++s_spins & 0x3FF) == 0)
        (void)model2recomp_field_sync();
}

/* ---- Arithmetic ---- */

/* ADD: dst = src1 + src2 */
static inline uint32_t op_addi(uint32_t src1, uint32_t src2)
{
    return src1 + src2;
}

/* ADDO: dst = src1 + src2 (same as addi for recomp, no overflow trap) */
static inline uint32_t op_addo(uint32_t src1, uint32_t src2)
{
    return src1 + src2;
}

/* SUB: dst = src2 - src1 */
static inline uint32_t op_subi(uint32_t src1, uint32_t src2)
{
    return src2 - src1;
}

static inline uint32_t op_subo(uint32_t src1, uint32_t src2)
{
    return src2 - src1;
}

/* MUL */
static inline uint32_t op_mulo(uint32_t src1, uint32_t src2)
{
    return src1 * src2;
}

/* DIV */
static inline uint32_t op_divo(uint32_t src1, uint32_t src2)
{
    if (src1 == 0) return 0;
    return src2 / src1;
}

static inline uint32_t op_remo(uint32_t src1, uint32_t src2)
{
    if (src1 == 0) return 0;
    return src2 % src1;
}

/* ---- Logical ---- */

static inline uint32_t op_and(uint32_t src1, uint32_t src2)
{
    return src1 & src2;
}

static inline uint32_t op_andnot(uint32_t src1, uint32_t src2)
{
    return src2 & ~src1;
}

static inline uint32_t op_or(uint32_t src1, uint32_t src2)
{
    return src1 | src2;
}

static inline uint32_t op_xor(uint32_t src1, uint32_t src2)
{
    return src1 ^ src2;
}

static inline uint32_t op_not(uint32_t src)
{
    return ~src;
}

static inline uint32_t op_ornot(uint32_t src1, uint32_t src2)
{
    return src2 | ~src1;
}

static inline uint32_t op_notor(uint32_t src1, uint32_t src2)
{
    return ~src2 | src1;
}

static inline uint32_t op_notand(uint32_t src1, uint32_t src2)
{
    return ~src2 & src1;
}

/* ---- Shifts ---- */

static inline uint32_t op_shlo(uint32_t count, uint32_t src)
{
    if (count >= 32) return 0;
    return src << count;
}

static inline uint32_t op_shro(uint32_t count, uint32_t src)
{
    if (count >= 32) return 0;
    return src >> count;
}

static inline int32_t op_shri(uint32_t count, int32_t src)
{
    if (count >= 32) return src >> 31;
    return src >> count;
}

/*
 * SHRDI: shift right, dividing. An arithmetic shift that rounds toward zero
 * rather than toward negative infinity, so it is an exact divide by a power
 * of two for negative values too. Sega's compiler uses it wherever a signed
 * value is divided by a constant. From i960.cpp case 0x59 sub-opcode 0xa.
 */
static inline uint32_t op_shrdi(uint32_t count, uint32_t src)
{
    if (count >= 32)
        return 0;
    if ((int32_t)src < 0) {
        if (src & ((1u << count) - 1u))
            return (uint32_t)((((int32_t)src) >> count) + 1);
        return (uint32_t)(((int32_t)src) >> count);
    }
    return src >> count;
}

static inline uint32_t op_rotate(uint32_t count, uint32_t src)
{
    count &= 31;
    return (src << count) | (src >> (32 - count));
}

/* ---- Compare ---- */

/* CMPI: signed compare, sets AC condition code */
static inline void op_cmpi(int32_t src1, int32_t src2)
{
    i960_cmp_s(src1, src2);
}

/* CMPO: unsigned compare */
static inline void op_cmpo(uint32_t src1, uint32_t src2)
{
    i960_cmp_u(src1, src2);
}

/* CMPR/CMPRL: floating-point compare, sets AC condition code.
 * Unordered (NaN) operands set the unordered condition. */
static inline void i960_cmp_d(double v1, double v2)
{
    if (v1 < v2)       I960_CC_SET(I960_AC_CC_LT);
    else if (v1 == v2) I960_CC_SET(I960_AC_CC_EQ);
    else if (v1 > v2)  I960_CC_SET(I960_AC_CC_GT);
    else               I960_CC_SET(I960_AC_CC_UN);  /* NaN */
}

/* ---- Conditional branches ---- */
/* These check AC condition code against a mask */

/* BE: branch if equal (mask=0x02) */
#define COND_E   0x02
/* BNE: branch if not equal (mask=0x05) */
#define COND_NE  0x05
/* BL: branch if less (mask=0x04) */
#define COND_L   0x04
/* BLE: branch if less or equal (mask=0x06) */
#define COND_LE  0x06
/* BG: branch if greater (mask=0x01) */
#define COND_G   0x01
/* BGE: branch if greater or equal (mask=0x03) */
#define COND_GE  0x03

/* ---- Load/Store ---- */

/* LD: load 32-bit word */
static inline uint32_t op_ld(uint32_t addr)
{
    return bus_read32(addr);
}

/* LDOB: load byte (zero-extend) */
static inline uint32_t op_ldob(uint32_t addr)
{
    return bus_read8(addr);
}

/* LDOS: load 16-bit (zero-extend) */
static inline uint32_t op_ldos(uint32_t addr)
{
    return bus_read16(addr);
}

/* LDIB: load byte (sign-extend) */
static inline int32_t op_ldib(uint32_t addr)
{
    return (int8_t)bus_read8(addr);
}

/* LDIS: load 16-bit (sign-extend) */
static inline int32_t op_ldis(uint32_t addr)
{
    return (int16_t)bus_read16(addr);
}

/* LDL: load 64-bit (two words) */
static inline void op_ldl(uint32_t addr, int reg_pair)
{
    g_i960.r[reg_pair]     = bus_read32(addr);
    g_i960.r[reg_pair + 1] = bus_read32(addr + 4);
}

/* LDT: load 96-bit (three words) */
static inline void op_ldt(uint32_t addr, int reg_triple)
{
    g_i960.r[reg_triple]     = bus_read32(addr);
    g_i960.r[reg_triple + 1] = bus_read32(addr + 4);
    g_i960.r[reg_triple + 2] = bus_read32(addr + 8);
}

/* LDQ: load 128-bit (four words) */
static inline void op_ldq(uint32_t addr, int reg_quad)
{
    g_i960.r[reg_quad]     = bus_read32(addr);
    g_i960.r[reg_quad + 1] = bus_read32(addr + 4);
    g_i960.r[reg_quad + 2] = bus_read32(addr + 8);
    g_i960.r[reg_quad + 3] = bus_read32(addr + 12);
}

/* ST: store 32-bit word */
static inline void op_st(uint32_t val, uint32_t addr)
{
    bus_write32(addr, val);
}

/* STOB: store byte */
static inline void op_stob(uint8_t val, uint32_t addr)
{
    bus_write8(addr, val);
}

/* STOS: store 16-bit */
static inline void op_stos(uint16_t val, uint32_t addr)
{
    bus_write16(addr, val);
}

/* STL: store 64-bit */
static inline void op_stl(int reg_pair, uint32_t addr)
{
    bus_write32(addr, g_i960.r[reg_pair]);
    bus_write32(addr + 4, g_i960.r[reg_pair + 1]);
}

/* STT: store 96-bit */
static inline void op_stt(int reg_triple, uint32_t addr)
{
    bus_write32(addr, g_i960.r[reg_triple]);
    bus_write32(addr + 4, g_i960.r[reg_triple + 1]);
    bus_write32(addr + 8, g_i960.r[reg_triple + 2]);
}

/* STQ: store 128-bit */
static inline void op_stq(int reg_quad, uint32_t addr)
{
    bus_write32(addr, g_i960.r[reg_quad]);
    bus_write32(addr + 4, g_i960.r[reg_quad + 1]);
    bus_write32(addr + 8, g_i960.r[reg_quad + 2]);
    bus_write32(addr + 12, g_i960.r[reg_quad + 3]);
}

/* ---- Bit operations ---- */

static inline uint32_t op_setbit(uint32_t bitnum, uint32_t src)
{
    return src | (1u << (bitnum & 31));
}

static inline uint32_t op_clrbit(uint32_t bitnum, uint32_t src)
{
    return src & ~(1u << (bitnum & 31));
}

static inline void op_chkbit(uint32_t bitnum, uint32_t src)
{
    if (src & (1u << (bitnum & 31)))
        I960_CC_SET(I960_AC_CC_EQ); /* bit set = "equal" condition */
    else
        I960_CC_SET(I960_AC_CC_LT); /* bit clear = "less" condition */
}

/* ---- Move ---- */

static inline uint32_t op_mov(uint32_t src)
{
    return src;
}

/* ---- Floating point ----
 *
 * Real operands live either in the four fp registers or, far more commonly, in
 * ordinary global/local registers holding an IEEE bit pattern - single
 * precision for the "real" ops, a register pair for the "long real" ops. The
 * REG mode bits pick which, so the lifter resolves that statically and calls
 * these to reinterpret the bits. */

static inline double i960_u2f(uint32_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return (double)f;
}

static inline uint32_t i960_f2u(double d)
{
    float f = (float)d;
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

static inline double i960_u2d(uint32_t lo, uint32_t hi)
{
    uint64_t bits = ((uint64_t)hi << 32) | lo;
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static inline void i960_d2u(double d, uint32_t *lo, uint32_t *hi)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    *lo = (uint32_t)bits;
    *hi = (uint32_t)(bits >> 32);
}

/* ROUNDR / CVTRI use the current rounding mode; round-to-nearest is the
 * i960 reset default and the only mode this game selects. */
static inline double i960_round(double d)
{
    return nearbyint(d);
}

/* ---- Test/Fault (conditional on AC) ---- */

/* TEST: set register based on condition */
static inline uint32_t op_teste(void)  { return i960_test_cc(COND_E)  ? 1 : 0; }
static inline uint32_t op_testne(void) { return i960_test_cc(COND_NE) ? 1 : 0; }
static inline uint32_t op_testl(void)  { return i960_test_cc(COND_L)  ? 1 : 0; }
static inline uint32_t op_testle(void) { return i960_test_cc(COND_LE) ? 1 : 0; }
static inline uint32_t op_testg(void)  { return i960_test_cc(COND_G)  ? 1 : 0; }
static inline uint32_t op_testge(void) { return i960_test_cc(COND_GE) ? 1 : 0; }

#endif /* MODEL2RECOMP_I960_OPS_H */
