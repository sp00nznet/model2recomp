/*
 * Model 2 timer and interrupt subsystem.
 *
 * 4 timers counting down at 25 MHz. IRQ on reaching 0.
 * Reference: MAME model2.cpp (BSD-3-Clause)
 */

#include "model2recomp/timer.h"
#include <stdio.h>
#include <string.h>

/* Timer state */
static uint32_t s_timer_vals[4];
static uint32_t s_timer_orig[4];
static int      s_timer_running[4];

/* IRQ state */
static uint32_t s_irq_request = 0;
static uint32_t s_irq_enable = 0;

void timer_init(void)
{
    memset(s_timer_vals, 0, sizeof(s_timer_vals));
    memset(s_timer_orig, 0, sizeof(s_timer_orig));
    memset(s_timer_running, 0, sizeof(s_timer_running));
    s_irq_request = 0;
    s_irq_enable = 0;

    printf("[timer] Initialized (4 timers @ 25 MHz)\n");
}

uint32_t timer_read(uint32_t offset)
{
    if (offset < 4) {
        /* If running, return current countdown value */
        /* For now, return stored value (no real-time tracking yet) */
        return s_timer_vals[offset];
    }
    return 0;
}

void timer_write(uint32_t offset, uint32_t data)
{
    if (offset < 4) {
        s_timer_vals[offset] = data;
        s_timer_orig[offset] = data;
        s_timer_running[offset] = 1;
    }
}

uint32_t irq_request_read(void)
{
    return s_irq_request;
}

void irq_ack_write(uint32_t data)
{
    /* Writing bits clears the corresponding IRQ request */
    s_irq_request &= ~data;
}

uint32_t irq_enable_read(void)
{
    return s_irq_enable;
}

void irq_enable_write(uint32_t data)
{
    /* Delay IRQ mask update by 2 cycles (vcop2 needs this per MAME) */
    s_irq_enable = data;
    irq_update();
}

void irq_update(void)
{
    /* Check if any enabled IRQ is pending */
    uint32_t active = s_irq_request & s_irq_enable;
    (void)active;
    /* TODO: Signal i960 interrupt when active != 0 */
}

void irq_raise(uint32_t line_mask)
{
    s_irq_request |= line_mask;
    irq_update();
}

void timer_tick(uint32_t elapsed_us)
{
    /* Convert microseconds to 25 MHz ticks */
    uint32_t ticks = elapsed_us * 25;

    for (int i = 0; i < 4; i++) {
        if (!s_timer_running[i]) continue;

        if (s_timer_vals[i] <= ticks) {
            /* Timer expired */
            s_timer_vals[i] = 0xFFFFF; /* Reset to default */
            s_timer_running[i] = 0;

            /* Fire IRQ: timer i maps to bit (i + 2) */
            uint32_t line = 1 << (i + 2);
            if (s_irq_enable & line) {
                irq_raise(line);
            }
        } else {
            s_timer_vals[i] -= ticks;
        }
    }
}
