/*
 * Model 2 I/O board interface.
 *
 * Virtua Cop uses Model 1 I/O board 2 (837-11694).
 * Communication via dual-port RAM (MB8421).
 * Lightgun position reported via FPGA on I/O board.
 *
 * Reference: MAME model1io2.cpp (BSD-3-Clause)
 */

#include "model2recomp/io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Input port state */
static uint8_t s_input_ports[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

/* Lightgun state */
static lightgun_state_t s_lightgun[2];

/* DPRAM */
static uint8_t s_dpram[0x1000];

/*
 * DPRAM layout, as the board's Z80 firmware leaves it. These are DPRAM byte
 * offsets; the i960 reaches byte N at 0x01C00000 + N*2, because only two of
 * every four byte lanes are populated (see bus.c).
 *
 * The offsets were read out of the game rather than guessed: 0x00001300 reads
 * 0x08/0x09/0x0A/0x11, composes them into one word and inverts it, and
 * 0x000014F0 reads nine bytes at 0x80 as four little-endian coordinates plus a
 * status byte - which is exactly the layout of model1io2's lightgun FPGA.
 */
#define DPRAM_IN0     0x08  /* coin, service, test, start          (active low) */
#define DPRAM_IN1     0x09  /* player triggers                     (active low) */
#define DPRAM_IN2     0x0A  /* board DIPs, incl. "No Enemies"      (active low) */
#define DPRAM_IN3     0x11  /* fourth input byte, unused by this game */
#define DPRAM_CMD     0x20  /* command; board zeroes it when the command completes */
#define DPRAM_STATUS  0x21  /* board status, bit 6 = ready */
#define DPRAM_GUN     0x80  /* P1 Y, P1 X, P2 Y, P2 X, then offscreen flags */

/*
 * Lightgun calibration, from MAME's vcop input ports. The gun reports 10-bit
 * values over these ranges rather than 0..screen, and the game's crosshair
 * maths assumes them.
 */
#define GUN_X_MIN 0x083
#define GUN_X_MAX 0x276
#define GUN_Y_MIN 0x024
#define GUN_Y_MAX 0x1A9
#define GUN_BORDER 0.05f    /* fraction of range that counts as off-screen */

/* Lamp output */
static uint8_t s_lamp_state = 0;

void io_init(void)
{
    memset(s_input_ports, 0xFF, sizeof(s_input_ports));
    memset(&s_lightgun, 0, sizeof(s_lightgun));
    memset(s_dpram, 0xFF, sizeof(s_dpram));
    /* Board state: command register idle, status "ready" (bit 6). Virtua Cop's
     * NVRAM-restore path (0x2D248) waits on both before issuing command 3. */
    s_dpram[DPRAM_CMD] = 0x00;
    s_dpram[DPRAM_STATUS] = 0x40;
    s_dpram[DPRAM_IN0] = 0xFF;
    s_dpram[DPRAM_IN1] = 0xFF;
    s_dpram[DPRAM_IN2] = 0xFF;
    s_dpram[DPRAM_IN3] = 0xFF;
    s_lamp_state = 0;

    printf("[io] I/O board initialized\n");
}

void io_shutdown(void)
{
    /* Nothing to clean up */
}

/* --- DPRAM --- */

uint8_t dpram_read(uint32_t offset)
{
    if (offset < sizeof(s_dpram))
        return s_dpram[offset];
    return 0xFF;
}

/*
 * The board's Z80 firmware polls the command register, executes the command and
 * writes 0 back when done; the game busy-waits on it (Virtua Cop's 0x2928
 * stores the "SEGA" magic at 0x34..0x3A, raises command 1, then spins until
 * this reads back non-1). Nothing here runs asynchronously, so a command is
 * complete the moment it is issued.
 *
 * ponytail: no per-command semantics - every command acks instantly. Add a
 * switch here if a command has to leave a result in DPRAM before the ack.
 */
void dpram_write(uint32_t offset, uint8_t val)
{
    if (offset >= sizeof(s_dpram))
        return;

    s_dpram[offset] = val;

    if (offset == DPRAM_CMD && val != 0)
        s_dpram[DPRAM_CMD] = 0;
}

/* --- Input state --- */

void io_set_input(int port, uint8_t state)
{
    if (port >= 0 && port < 4)
        s_input_ports[port] = state;
}

uint8_t io_get_input(int port)
{
    if (port >= 0 && port < 4)
        return s_input_ports[port];
    return 0xFF;
}

/* --- Lightgun --- */

void io_set_lightgun(int player, uint16_t x, uint16_t y, bool offscreen)
{
    if (player >= 0 && player < 2) {
        s_lightgun[player].x = x;
        s_lightgun[player].y = y;
        s_lightgun[player].offscreen = offscreen;
    }
}

lightgun_state_t io_get_lightgun(int player)
{
    if (player >= 0 && player < 2)
        return s_lightgun[player];

    lightgun_state_t empty = {0, 0, true};
    return empty;
}

/*
 * Publish the host's input state into DPRAM, once per field.
 *
 * On real hardware the board's Z80 samples its ports and the lightgun FPGA and
 * copies the result here; with no Z80 emulated, this is that copy. Everything
 * the game reads about input comes from these bytes, so this is the whole of
 * the input path.
 */
static uint16_t gun_scale(uint16_t v, uint16_t range, uint16_t lo, uint16_t hi)
{
    if (range == 0) return lo;
    if (v >= range) v = (uint16_t)(range - 1);
    return (uint16_t)(lo + ((uint32_t)v * (hi - lo)) / (range - 1));
}

static bool gun_in_border(uint16_t x, uint16_t y)
{
    int bx = (int)((GUN_X_MAX - GUN_X_MIN) * GUN_BORDER);
    int by = (int)((GUN_Y_MAX - GUN_Y_MIN) * GUN_BORDER);

    return x <= GUN_X_MIN + bx || x >= GUN_X_MAX - bx ||
           y <= GUN_Y_MIN + by || y >= GUN_Y_MAX - by;
}

void io_update_dpram(uint16_t screen_w, uint16_t screen_h)
{
    s_dpram[DPRAM_IN0] = s_input_ports[0];
    s_dpram[DPRAM_IN1] = s_input_ports[1];
    s_dpram[DPRAM_IN2] = s_input_ports[2];
    s_dpram[DPRAM_IN3] = s_input_ports[3];

    uint8_t offscreen = 0xFC;   /* bits 0-1 are the per-player flags */

    for (int p = 0; p < 2; p++) {
        uint16_t gx = gun_scale(s_lightgun[p].x, screen_w, GUN_X_MIN, GUN_X_MAX);
        uint16_t gy = gun_scale(s_lightgun[p].y, screen_h, GUN_Y_MIN, GUN_Y_MAX);

        /* Shooting off-screen is how Virtua Cop reloads, so a forced
         * off-screen shot has to read as one: park the gun outside the
         * calibrated range rather than only setting the flag, because the
         * game cross-checks the coordinates against it. */
        if (s_lightgun[p].offscreen) {
            gx = GUN_X_MIN;
            gy = GUN_Y_MIN;
        }

        uint32_t base = DPRAM_GUN + p * 4;
        s_dpram[base + 0] = (uint8_t)gy;
        s_dpram[base + 1] = (uint8_t)(gy >> 8);
        s_dpram[base + 2] = (uint8_t)gx;
        s_dpram[base + 3] = (uint8_t)(gx >> 8);

        if (s_lightgun[p].offscreen || gun_in_border(gx, gy))
            offscreen |= (uint8_t)(1 << p);
    }

    s_dpram[DPRAM_GUN + 8] = offscreen;
}

void lamp_output_write(uint8_t data)
{
    s_lamp_state = data;
    /* Bits 0-1: coin counters, bits 2-7: lamps */
}

/* --------------------------------------------------------------------------
 * Sega 315-5649 I/O controller
 *
 * What the CRX boards have in place of the Model 1 I/O board's dual-port RAM:
 * a 32-byte register file rather than a command protocol, and identical on
 * 2A, 2B and 2C. Ported from MAME's sega/315_5649.cpp (Dirk Best).
 *
 * Ports A-G are bidirectional, one bit per port in the direction register
 * deciding which. A port configured as an input reads the host; one configured
 * as an output reads back what was last written to it, which is a thing the
 * games actually rely on.
 * ------------------------------------------------------------------------ */

static uint8_t s_5649_port[8];
static uint8_t s_5649_config;     /* 1 = input, 0 = output; all inputs at reset */
static uint8_t s_5649_mode;
static uint8_t s_5649_analog_ch;

void sega5649_reset(void)
{
    memset(s_5649_port, 0, sizeof(s_5649_port));
    s_5649_config = 0xFF;
    s_5649_mode = 0;
    s_5649_analog_ch = 0;
}

uint8_t sega5649_read(uint8_t offset)
{
    switch (offset & 0x1F) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06:
        /* Port G in counter mode reads four 16-bit counters; nothing here
         * drives them, so it falls through to the ordinary port read. */
        if (s_5649_config & (1u << offset))
            return io_get_input(offset < 4 ? offset : 0);
        return s_5649_port[offset];

    /* RS-422 receive. No satellite cabinet on the other end. */
    case 0x0B: case 0x0C:
        return 0xFF;

    /* RS-422 status. MAME hardcodes "receive buffers full, transmit buffers
     * empty" and the games are happy with it; a real link would need more. */
    case 0x0D:
        return 0x0C;

    /* Analog input, auto-incrementing through eight channels. Nothing here
     * has a wheel or a pedal yet, so every channel reads centred. */
    case 0x0F:
        s_5649_analog_ch = (uint8_t)((s_5649_analog_ch + 1) & 7);
        return 0x80;
    }
    return 0xFF;
}

void sega5649_write(uint8_t offset, uint8_t data)
{
    switch (offset & 0x1F) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06:
        s_5649_port[offset] = data;
        if (offset == 0)
            lamp_output_write(data);
        break;
    case 0x08: s_5649_config = data; break;      /* port direction */
    case 0x0E: s_5649_mode = data; break;        /* counter / RS-422 mode */
    case 0x0F: s_5649_analog_ch = (uint8_t)(data & 7); break;
    default: break;                              /* serial out, unmodelled */
    }
}
