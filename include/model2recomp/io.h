/*
 * Model 2 I/O board interface.
 *
 * Virtua Cop uses the Model 1 I/O board 2 (837-11694, Z80-based with FPGA).
 * Communication via dual-port RAM (MB8421, 2Kx8).
 *
 * Input ports:
 *   IN0: Service, test, coin, start buttons
 *   IN1: Game-specific buttons (trigger, etc.)
 *   IN2: Additional buttons (lightgun games)
 *   Lightgun: P1_X, P1_Y, P2_X, P2_Y (10-bit each)
 *
 * The I/O board handles lightgun position detection via its FPGA
 * and reports coordinates through the dual-port RAM.
 */

#ifndef MODEL2RECOMP_IO_H
#define MODEL2RECOMP_IO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Input port bit definitions - IN0, from MAME's model2 port table.
 * Bit 2 is the service-mode (test) switch and bit 3 is the service button
 * that adds a credit; they were the wrong way round here until 2026-09-13. */
#define IN0_COIN1       (1 << 0)
#define IN0_COIN2       (1 << 1)
#define IN0_TEST        (1 << 2)   /* service-mode switch, held */
#define IN0_SERVICE     (1 << 3)   /* service button, momentary */
#define IN0_START1      (1 << 4)
#define IN0_START2      (1 << 5)

/* Input port bit definitions - IN1 (Virtua Cop) */
#define IN1_P1_TRIGGER  (1 << 0)
#define IN1_P2_TRIGGER  (1 << 1)

/* Lightgun state */
typedef struct {
    uint16_t x;     /* 0-319 screen X */
    uint16_t y;     /* 0-239 screen Y */
    bool offscreen; /* true if pointing off screen */
} lightgun_state_t;

/* Initialize I/O subsystem */
void io_init(void);
void io_shutdown(void);

/* --- DPRAM interface (i960 side, 0x01C00000-0x01C00FFF) --- */
uint8_t dpram_read(uint32_t offset);
void dpram_write(uint32_t offset, uint8_t val);

/* --- Input state (set by platform layer from SDL events) --- */
void io_set_input(int port, uint8_t state);
uint8_t io_get_input(int port);

/* --- Lightgun --- */
void io_set_lightgun(int player, uint16_t x, uint16_t y, bool offscreen);
lightgun_state_t io_get_lightgun(int player);

/* Lightgun data read (0x00E80040+) */
/* Copy the current input and lightgun state into DPRAM where the game reads
 * it. Call once per field, after the platform layer has sampled the host.
 * Lightgun coordinates go in as screen pixels and are scaled to the gun's
 * calibrated 10-bit range here. */
void io_update_dpram(uint16_t screen_w, uint16_t screen_h);

/* --- Lamp/coin counter outputs --- */
void lamp_output_write(uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_IO_H */
