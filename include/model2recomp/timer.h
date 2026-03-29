/*
 * Model 2 timer and interrupt subsystem.
 *
 * 4 hardware timers that count down at 25 MHz.
 * When a timer reaches 0, it fires IRQ2.
 *
 * IRQ lines:
 *   Bit 0: VSYNC (VBlank)
 *   Bit 1: (reserved)
 *   Bit 2: Timer 0
 *   Bit 3: Timer 1
 *   Bit 4: Timer 2
 *   Bit 5: Timer 3
 *   Bit 6-9: (various)
 *   Bit 10: Sound (UART RxRDY/TxRDY)
 */

#ifndef MODEL2RECOMP_TIMER_H
#define MODEL2RECOMP_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize timer subsystem */
void timer_init(void);

/* --- Timer registers (0x00F00000-0x00F0000F) --- */
uint32_t timer_read(uint32_t offset);
void timer_write(uint32_t offset, uint32_t data);

/* --- IRQ registers (0x00E80000-0x00E80007) --- */
uint32_t irq_request_read(void);
void irq_ack_write(uint32_t data);
uint32_t irq_enable_read(void);
void irq_enable_write(uint32_t data);

/* Update IRQ state (called after ack/enable changes) */
void irq_update(void);

/* Fire a specific interrupt */
void irq_raise(uint32_t line_mask);

/* Advance timers by elapsed microseconds */
void timer_tick(uint32_t elapsed_us);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_TIMER_H */
