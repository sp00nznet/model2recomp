/*
 * Model 2 sound subsystem.
 *
 * Original Model 2: 68000 @ 10 MHz + YM3438 (OPN2C) + 2x MultiPCM (YMW-258-F)
 * Model 2A/B/C:     68000 @ 11.3 MHz + SCSP (YMF292)
 *
 * Communication between i960 and sound CPU via UART (i8251).
 * Sound data transmitted at 31.25 kHz (standard MIDI rate).
 *
 * For recompilation, we run the 68000 sound CPU as an interpreter
 * (or potentially also recompile it separately).
 */

#ifndef MODEL2RECOMP_SOUND_H
#define MODEL2RECOMP_SOUND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize sound subsystem */
void sound_init(void);
void sound_shutdown(void);

/* --- UART interface (i960 side) --- */
/* UART data/control at 0x01C80000-0x01C80003 */
void uart_write(uint32_t offset, uint8_t data);
uint8_t uart_read(uint32_t offset);

/* --- Sound board ROM loading --- */
void sound_load_program(const uint8_t *data, uint32_t size);
void sound_load_samples(const uint8_t *data, uint32_t size);

/* --- Sound generation --- */
/* Generate audio samples for one frame (~735 samples at 44100 Hz / 60 fps) */
void sound_generate_samples(int16_t *buffer, int num_samples);

/* Run sound CPU for given number of cycles */
void sound_run_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_SOUND_H */
