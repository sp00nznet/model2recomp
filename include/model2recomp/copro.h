/*
 * Model 2 math coprocessor: Fujitsu MB86233/86234 TGP.
 *
 * This is the DSP the game offloads its matrix and vector math to - not the
 * geometry engine (that lives in geometry.c and is modelled directly). The
 * game uploads its own microcode at boot, so the only thing needed here is a
 * faithful CPU core plus the address spaces the Model 2 board wires around it.
 *
 * Core ported from MAME's cpu/mb86233/mb86233.cpp; the surrounding memory maps
 * from model2_tgp_state in mame/sega/model2.cpp.
 */

#ifndef MODEL2RECOMP_COPRO_H
#define MODEL2RECOMP_COPRO_H

#include <stdint.h>
#include <stdbool.h>

void copro_load_tables(const uint8_t *data, uint32_t size);

/* i960-side ports */
void     copro_ctl_write(uint32_t data);     /* 0x00980000: upload gate / boot */
uint32_t copro_ctl_read(void);
void     copro_function_write(uint32_t offset, uint32_t data); /* 0x00880000 */
void     copro_fifo_write(uint32_t data);    /* 0x00884000 */
uint32_t copro_fifo_read(void);
bool     copro_output_empty(void);           /* 0x00980004 */

#endif /* MODEL2RECOMP_COPRO_H */
