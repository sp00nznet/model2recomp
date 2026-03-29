/*
 * Function dispatch table.
 *
 * Hash table mapping i960 addresses to native function pointers.
 * Uses open addressing with linear probing.
 */

#include "model2recomp/func_table.h"
#include <stdio.h>
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

bool func_table_call(uint32_t i960_addr)
{
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
    func();
    s_call_depth--;
    return true;
}
