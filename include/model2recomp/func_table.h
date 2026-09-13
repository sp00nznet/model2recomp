/*
 * Function dispatch table.
 *
 * Maps i960 code addresses to native C function pointers.
 * Used for indirect calls (function pointers, jump tables, interrupts).
 */

#ifndef MODEL2RECOMP_FUNC_TABLE_H
#define MODEL2RECOMP_FUNC_TABLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
void func_table_dump_ring(void);

#endif

/* All recompiled functions take no args and return nothing.
 * They operate on the global g_i960 context and call bus_read/write. */
typedef void (*i960_func_t)(void);

/* Initialize the function table (clear all entries). */
void func_table_init(void);

/* Register a recompiled function at its original i960 address. */
void func_table_register(uint32_t i960_addr, i960_func_t func);

/* Look up and call a function by its original address.
 * Returns true if found and called, false if not registered. */
bool func_table_call(uint32_t i960_addr);

/* Look up without calling. Returns NULL if not found. */
i960_func_t func_table_lookup(uint32_t i960_addr);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_FUNC_TABLE_H */
