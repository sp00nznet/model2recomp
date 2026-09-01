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
#include <string.h>

/* Input port state */
static uint8_t s_input_ports[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

/* Lightgun state */
static lightgun_state_t s_lightgun[2];

/* Lightgun mux register */
static uint8_t s_lightgun_mux = 0;

/* DPRAM */
static uint8_t s_dpram[0x1000];

/* I/O board DPRAM registers */
#define DPRAM_CMD    0x40   /* command; board zeroes it when the command completes */
#define DPRAM_STATUS 0x42   /* board status, bit 6 = ready */

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
    s_lightgun_mux = 0;
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
 * Lightgun data read.
 * Port order: P1_Y(0), P1_X(1), P2_Y(2), P2_X(3)
 * Each is 10-bit, read as two bytes (low, high).
 */
uint8_t lightgun_data_read(uint32_t offset)
{
    uint16_t data;
    int port = offset >> 1;

    switch (port) {
        case 0: data = s_lightgun[0].y; break;  /* P1_Y */
        case 1: data = s_lightgun[0].x; break;  /* P1_X */
        case 2: data = s_lightgun[1].y; break;  /* P2_Y */
        case 3: data = s_lightgun[1].x; break;  /* P2_X */
        default: data = 0; break;
    }

    return (offset & 1) ? (uint8_t)(data >> 8) : (uint8_t)data;
}

uint8_t lightgun_mux_read(void)
{
    if (s_lightgun_mux < 8)
        return lightgun_data_read(s_lightgun_mux);
    else
        return lightgun_offscreen_read();
}

void lightgun_mux_write(uint8_t data)
{
    s_lightgun_mux = data;
}

uint8_t lightgun_offscreen_read(void)
{
    uint8_t data = 0xFC; /* bits 0-1 are offscreen flags */

    /* 5% border detection */
    #define BORDER_SIZE 0.05f
    #define MAX_GUN_X 319
    #define MAX_GUN_Y 239

    int border_x = (int)(MAX_GUN_X * BORDER_SIZE);
    int border_y = (int)(MAX_GUN_Y * BORDER_SIZE);

    /* Player 1 */
    if (s_lightgun[0].x <= border_x || s_lightgun[0].x >= MAX_GUN_X - border_x ||
        s_lightgun[0].y <= border_y || s_lightgun[0].y >= MAX_GUN_Y - border_y ||
        s_lightgun[0].offscreen) {
        data |= 1;
    }

    /* Player 2 */
    if (s_lightgun[1].x <= border_x || s_lightgun[1].x >= MAX_GUN_X - border_x ||
        s_lightgun[1].y <= border_y || s_lightgun[1].y >= MAX_GUN_Y - border_y ||
        s_lightgun[1].offscreen) {
        data |= 2;
    }

    return data;
}

void lamp_output_write(uint8_t data)
{
    s_lamp_state = data;
    /* Bits 0-1: coin counters, bits 2-7: lamps */
}
