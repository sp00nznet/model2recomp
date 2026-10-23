/*
 * Model 2 sound board.
 *
 * Model 2A/2B/2C: a 68000 at 11.29 MHz (the SCSP clock halved) runs the
 * game's sound program from its own ROM, with 512 KB of RAM it shares with a
 * Yamaha SCSP that plays the samples the program copies in from 8 MB of
 * sample ROM. The i960 talks to it over an i8251 UART whose transmit line is
 * the SCSP's MIDI input and whose receive line is its MIDI output.
 *
 * The 68000 is Musashi (third_party/musashi), the SCSP a port of MAME's
 * (src/scsp.c). The original Model 2 board (68000 + YM3438 + two MultiPCM) is
 * not modelled: its games get a silent UART that always reports ready.
 */

#ifndef MODEL2RECOMP_SOUND_H
#define MODEL2RECOMP_SOUND_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void sound_init(void);
void sound_shutdown(void);

/* i8251 UART, as the i960 sees it: data at +0, control/status at +1. */
void uart_write(uint32_t offset, uint8_t data);
uint8_t uart_read(uint32_t offset);

/* The two sound-board ROM regions (sound_program.bin, samples.bin), in the
 * 68000's byte order. Copied; the caller keeps ownership of its buffer. */
void sound_load_program(const uint8_t *data, uint32_t size);
void sound_load_samples(const uint8_t *data, uint32_t size);

/* Run the board for num_samples of audio time (256 68000 cycles each) and
 * render that much stereo 44.1 kHz audio, interleaved L/R. Silence until a
 * sound program has been loaded. */
void sound_generate_samples(int16_t *buffer, int num_samples);

/* Kept for callers that paced the board per frame; the work is done by
 * sound_generate_samples now. */
void sound_run_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_SOUND_H */
