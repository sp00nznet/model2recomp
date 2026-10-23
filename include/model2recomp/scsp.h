/*
 * Yamaha YMF292-F "SCSP" sound processor - the Model 2A/2B/2C sound chip.
 *
 * 32 PCM slots with envelope, pitch and amplitude LFOs, FM through a ring
 * buffer, a 128-step effects DSP, three timers, a MIDI port and a DMA engine,
 * all driven by the sound board's 68000 through a 4 KB register window. The
 * core is a C port of MAME's sound/scsp.cpp and scspdsp.cpp (ElSemi,
 * R. Belmont, kingshriek; BSD-3-Clause), which the Model 2 driver uses.
 *
 * One instance. The chip's sample memory is the sound board's RAM, handed in
 * at init as a big-endian byte array the 68000 also sees.
 */

#ifndef MODEL2RECOMP_SCSP_H
#define MODEL2RECOMP_SCSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* irq_cb(level, state): raise (state != 0) or drop a 68000 interrupt line.
 * midi_out_cb(byte): a byte the sound program sent out of the MIDI port. */
void scsp_init(uint8_t *ram, uint32_t ram_size,
               void (*irq_cb)(int level, int state),
               void (*midi_out_cb)(uint8_t byte));
void scsp_reset(void);

/* Register window: 16-bit access at a byte offset within 0x1000. mem_mask
 * says which byte lanes the CPU drove (0xFF00 high, 0x00FF low, 0xFFFF both);
 * reading the MIDI input buffer only takes a byte when its lane is read. */
uint16_t scsp_read16(uint32_t offset, uint16_t mem_mask);
void     scsp_write16(uint32_t offset, uint16_t data, uint16_t mem_mask);

/* A byte arriving at the MIDI input (from the main CPU's UART). */
void scsp_midi_in(uint8_t byte);

/* Bytes waiting in the 32-byte MIDI input FIFO. */
int  scsp_midi_in_pending(void);

/* Render count stereo frames at 44.1 kHz into out (L, R interleaved). */
void scsp_generate(int16_t *out, int count);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_SCSP_H */
