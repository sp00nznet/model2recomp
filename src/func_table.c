/*
 * Function dispatch table.
 *
 * Hash table mapping i960 addresses to native function pointers.
 * Uses open addressing with linear probing.
 */

#include "model2recomp/func_table.h"
#include "model2recomp/i960.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABLE_SIZE 8192  /* Power of 2, must be > number of functions */
#define TABLE_MASK (TABLE_SIZE - 1)

#define MAX_CALL_DEPTH 500
#define MAX_MISS_LOG   20

typedef struct {
    uint32_t    addr;
    i960_func_t func;
    bool        occupied;
} table_entry_t;

static table_entry_t s_table[TABLE_SIZE];
static int s_call_depth = 0;
uint32_t g_cur_func = 0;   /* debug: MODEL2_WATCH */
static int s_miss_count = 0;

static inline uint32_t hash_addr(uint32_t addr)
{
    /* MurmurHash-like mixing */
    addr ^= addr >> 16;
    addr *= 0x45d9f3b;
    addr ^= addr >> 16;
    return addr & TABLE_MASK;
}

void func_table_init(void)
{
    memset(s_table, 0, sizeof(s_table));
    s_call_depth = 0;
    s_miss_count = 0;
    printf("[func_table] Initialized (%d slots)\n", TABLE_SIZE);
}

void func_table_register(uint32_t i960_addr, i960_func_t func)
{
    uint32_t idx = hash_addr(i960_addr);

    for (int i = 0; i < TABLE_SIZE; i++) {
        uint32_t probe = (idx + i) & TABLE_MASK;
        if (!s_table[probe].occupied || s_table[probe].addr == i960_addr) {
            s_table[probe].addr = i960_addr;
            s_table[probe].func = func;
            s_table[probe].occupied = true;
            return;
        }
    }

    fprintf(stderr, "[func_table] ERROR: Table full! Cannot register 0x%08X\n", i960_addr);
}

i960_func_t func_table_lookup(uint32_t i960_addr)
{
    uint32_t idx = hash_addr(i960_addr);

    for (int i = 0; i < TABLE_SIZE; i++) {
        uint32_t probe = (idx + i) & TABLE_MASK;
        if (!s_table[probe].occupied)
            return NULL;
        if (s_table[probe].addr == i960_addr)
            return s_table[probe].func;
    }

    return NULL;
}

/* MODEL2_TRACE=N prints the first N dispatches, indented by call depth.
 * The last line before a hang names the function that is spinning. */
static long s_trace_left = -1;

bool func_table_call(uint32_t i960_addr)
{
    if (s_trace_left < 0) {
        const char *e = getenv("MODEL2_TRACE");
        s_trace_left = e ? strtol(e, NULL, 10) : 0;
    }
    if (s_trace_left > 0) {
        s_trace_left--;
        printf("%*s0x%08X\n", s_call_depth, "", i960_addr);
        fflush(stdout);
    }

    i960_func_t func = func_table_lookup(i960_addr);
    if (!func) {
        if (s_miss_count < MAX_MISS_LOG) {
            printf("[func_table] MISS: no function at 0x%08X\n", i960_addr);
            s_miss_count++;
        }
        return false;
    }

    if (s_call_depth >= MAX_CALL_DEPTH) {
        fprintf(stderr, "[func_table] ERROR: Max call depth (%d) exceeded at 0x%08X\n",
                MAX_CALL_DEPTH, i960_addr);
        return false;
    }

    s_call_depth++;
    uint32_t prev = g_cur_func; g_cur_func = i960_addr;
    uint32_t sp_in = g_i960.r[1];

    func();

    /*
     * MODEL2_LEAK names functions that return with the guest stack higher than
     * they found it. Only the innermost is reported, since a leak shows up in
     * every caller above it too; that innermost one is the function whose
     * generated C has a path that never reaches its "ret".
     *
     * Function discovery is what produces them: a data table following a "ret"
     * looks like an entry point, splits the real function in two, and a path
     * through the far half falls off the end. One leaked frame per field is
     * enough for the stack to climb into the PRCB within a minute.
     */
    static int s_child_leaked;
    int child_leaked = s_child_leaked;
    s_child_leaked = 0;
    if (g_i960.r[1] > sp_in) {
        s_child_leaked = 1;
        static int budget = 40;
        if (!child_leaked && budget > 0 && getenv("MODEL2_LEAK")) {
            budget--;
            fprintf(stderr, "[leak] %08X left sp %08X -> %08X\n",
                    i960_addr, sp_in, g_i960.r[1]);
        }
    }

    g_cur_func = prev;
    s_call_depth--;
    return true;
}
