/*
 * 93C46 serial EEPROM self-check.
 *
 * Eleven of the thirty-five sets in the corpus bit-bang this device through the
 * 315-5649's port A and read it back on port B, thousands of times a run, so a
 * protocol error here is a protocol error in eleven games at once - and it
 * would look exactly like "the game is stuck", which is not a symptom that
 * points anywhere.
 *
 * The device is driven the way the games drive it: clock a command in one bit
 * at a time on the rising edge of port A bit 7, then clock the answer out.
 */

#include "model2recomp/io.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

/* Port A: bit 0 ctrlmode, bit 5 DI, bit 6 CS, bit 7 CLK. */
#define PA_CTRL 0x01
#define PA_DI   0x20
#define PA_CS   0x40
#define PA_CLK  0x80

static uint8_t s_pa = PA_CTRL;      /* ctrlmode on throughout; CS starts low */

static void pa(uint8_t bits)
{
    s_pa = (uint8_t)(PA_CTRL | bits);
    eeprom93c46_port_a(s_pa);
}

/* One bit in, on a rising clock edge, with CS held. */
static void shift_in(int bit)
{
    pa(PA_CS | (bit ? PA_DI : 0));
    pa(PA_CS | (bit ? PA_DI : 0) | PA_CLK);
}

/* The data-out line, as the game sees it: port B bit 5 while in ctrlmode. */
static int data_out(void)
{
    return (eeprom93c46_port_b(0xFF) & 0x20) ? 1 : 0;
}

static void deselect(void)
{
    pa(0);
}

static void command(int op, int addr)
{
    deselect();
    shift_in(1);                        /* start bit */
    shift_in((op >> 1) & 1);
    shift_in(op & 1);
    for (int i = 5; i >= 0; i--)
        shift_in((addr >> i) & 1);
}

static void write_word(int addr, uint16_t value)
{
    command(1, addr);                   /* WRITE */
    for (int i = 15; i >= 0; i--)
        shift_in((value >> i) & 1);
    deselect();
}

static uint16_t read_word(int addr)
{
    command(2, addr);                   /* READ */
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        shift_in(0);                    /* clocking; DI is ignored */
        v = (uint16_t)((v << 1) | data_out());
    }
    deselect();
    return v;
}

static void ewen(void)  { command(0, 0x30); deselect(); }   /* write enable  */
static void ewds(void)  { command(0, 0x00); deselect(); }   /* write disable */

int main(void)
{
    printf("93C46 self-check\n");
    eeprom93c46_reset();

    /* A blank part reads all ones - that is what an erased cell is, and a game
     * checksumming it needs to see it rather than zeros. */
    check(read_word(0) == 0xFFFF, "a blank part reads 0xFFFF");

    /* Writes do nothing until the write-enable latch is set. This is the bug
     * that shipped for one commit: WRITE decoded, then stored nothing, so a
     * game saving its settings read them back blank forever. */
    write_word(5, 0x1234);
    check(read_word(5) == 0xFFFF, "a write without EWEN is ignored");

    ewen();
    write_word(5, 0x1234);
    check(read_word(5) == 0x1234, "a write after EWEN is stored and read back");

    write_word(0x3F, 0xBEEF);
    check(read_word(0x3F) == 0xBEEF, "the last word of the array is reachable");
    check(read_word(5) == 0x1234, "...without disturbing an earlier one");

    ewds();
    write_word(5, 0x0000);
    check(read_word(5) == 0x1234, "EWDS stops further writes");

    /* ERASE sets a word back to all ones, and also needs the latch. */
    ewen();
    command(3, 5);
    deselect();
    check(read_word(5) == 0xFFFF, "ERASE returns a word to 0xFFFF");

    /* Out of ctrlmode the port is the cabinet's inputs, untouched. */
    eeprom93c46_port_a(0x00);
    check(eeprom93c46_port_b(0x5A) == 0x5A,
          "out of ctrlmode port B is the inputs, not the EEPROM");

    printf(failures ? "%d check(s) FAILED\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
