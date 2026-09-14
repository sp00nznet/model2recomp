/*
 * i960 CPU context for static recompilation.
 *
 * Intel i960KB register architecture:
 *   - 16 local registers (r0-r15), saved/restored on call/return
 *   - 16 global registers (g0-g15), persistent across calls
 *   - 4 floating-point registers (fp0-fp3)
 *   - Arithmetic Controls (AC) register with condition codes
 *   - Process Controls (PC) register
 *   - Instruction Pointer (IP)
 *
 * Special registers:
 *   r0 = PFP (Previous Frame Pointer)
 *   r1 = SP  (Stack Pointer)
 *   r2 = RIP (Return Instruction Pointer)
 *   g15 = FP (Frame Pointer)
 *
 * The i960 uses a register cache (4 frames of 16 local regs) for fast
 * call/return. When the cache is full, frames spill to the stack.
 */

#ifndef MODEL2RECOMP_I960_H
#define MODEL2RECOMP_I960_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I960_RCACHE_SIZE 4

typedef struct I960Context {
    /* 32 registers: r0-r15 (local) and g0-g15 (global) */
    uint32_t r[32];

    /* Register cache for call/return (4 frames x 16 local regs) */
    uint32_t rcache[I960_RCACHE_SIZE][16];
    uint32_t rcache_frame_addr[I960_RCACHE_SIZE];
    int32_t  rcache_pos;

    /* Floating-point registers */
    double fp[4];

    /* System registers */
    uint32_t SAT;       /* System Address Table */
    uint32_t PRCB;      /* Processor Control Block */
    uint32_t PC;        /* Process Controls */
    uint32_t AC;        /* Arithmetic Controls (condition codes) */
    uint32_t IP;        /* Instruction Pointer */
    uint32_t PIP;       /* Previous Instruction Pointer */
    uint32_t ICR;       /* Interrupt Control Register */

    /* Cycle counter for timing synchronization */
    int32_t  cycles;
    int32_t  cycles_target;
} I960Context;

/* Global CPU context - all recompiled functions operate on this */
extern I960Context g_i960;

/* Return address of the bal in progress; see i960.c. */
extern uint32_t g_bal_link;

/* --- Register accessors --- */

/* Local registers r0-r15 */
#define I960_R(n)       (g_i960.r[n])
#define I960_PFP        (g_i960.r[0])       /* Previous Frame Pointer */
#define I960_SP         (g_i960.r[1])       /* Stack Pointer */
#define I960_RIP        (g_i960.r[2])       /* Return Instruction Pointer */

/* Global registers g0-g15 */
#define I960_G(n)       (g_i960.r[16 + (n)])
#define I960_FP         (g_i960.r[31])      /* Frame Pointer = g15 */

/* --- Arithmetic Controls (AC register) --- */
/*
 * Condition code is in bits 0-2 of AC:
 *   bit 0 = unordered (invalid comparison)
 *   bit 1 = greater than
 *   bit 2 = equal
 * So: 0b000 = less than, 0b010 = greater, 0b100 = equal
 */
#define I960_AC_CC_MASK     0x00000007
#define I960_AC_CC_LT       0x04    /* Less than */
#define I960_AC_CC_EQ       0x02    /* Equal */
#define I960_AC_CC_GT       0x01    /* Greater than */
#define I960_AC_CC_UN       0x00    /* Unordered */

#define I960_CC_SET(cc)     (g_i960.AC = (g_i960.AC & ~I960_AC_CC_MASK) | ((cc) & I960_AC_CC_MASK))
#define I960_CC_GET()       (g_i960.AC & I960_AC_CC_MASK)

/* Overflow flag in AC */
#define I960_AC_OF          (1 << 8)
#define I960_AC_IF          (1 << 12)   /* Integer overflow flag */
#define I960_AC_NIF         (1 << 15)   /* No imprecise faults */

/* --- Condition code helpers --- */

/* Compare signed integers, set AC condition code */
static inline void i960_cmp_s(int32_t v1, int32_t v2)
{
    if (v1 < v2)      I960_CC_SET(I960_AC_CC_LT);
    else if (v1 == v2) I960_CC_SET(I960_AC_CC_EQ);
    else               I960_CC_SET(I960_AC_CC_GT);
}

/* Compare unsigned integers, set AC condition code */
static inline void i960_cmp_u(uint32_t v1, uint32_t v2)
{
    if (v1 < v2)      I960_CC_SET(I960_AC_CC_LT);
    else if (v1 == v2) I960_CC_SET(I960_AC_CC_EQ);
    else               I960_CC_SET(I960_AC_CC_GT);
}

/* Test condition code against mask (for branch instructions) */
static inline bool i960_test_cc(int mask)
{
    return (I960_CC_GET() & mask) != 0;
}

/* --- Call/Return support --- */

/*
 * Save current local registers to cache or stack.
 * Used by CALL instruction.
 */
void i960_do_call(uint32_t target_addr, uint32_t return_addr);

/*
 * Restore local registers from cache or stack.
 * Used by RET instruction.
 */
void i960_do_ret(void);

/*
 * Reset CPU to power-on state.
 */
void i960_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_I960_H */
