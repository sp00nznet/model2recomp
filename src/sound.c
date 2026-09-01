/*
 * Model 2 sound subsystem - stub implementation.
 *
 * TODO: Integrate a 68000 interpreter + MultiPCM/SCSP emulation.
 * Candidates: Musashi (68000), MAME's multipcm.cpp / scsp.cpp
 *
 * For now, provides UART stubs so recompiled i960 code can run.
 */

#include "model2recomp/sound.h"
#include "model2recomp/timer.h"
#include <stdio.h>
#include <string.h>

/* UART state (i8251) */
static uint8_t s_uart_data = 0;
static uint8_t s_uart_status = 0x05; /* TxRDY=1, TxEmpty=1 */

/* Sound program/sample storage */
static uint8_t *s_sound_program = NULL;
static uint32_t s_sound_program_size = 0;
static uint8_t *s_sound_samples = NULL;
static uint32_t s_sound_samples_size = 0;

void sound_init(void)
{
    s_uart_data = 0;
    s_uart_status = 0x05;
    printf("[sound] Initialized (stub)\n");
}

void sound_shutdown(void)
{
    /* Program and sample data are owned by the ROM loader */
    s_sound_program = NULL;
    s_sound_samples = NULL;
}

void uart_write(uint32_t offset, uint8_t data)
{
    if (offset == 0) {
        /* Data register - send byte to sound CPU */
        s_uart_data = data;
        /* TODO: queue for 68000 sound CPU */

        /* The board raises the sound interrupt whenever the UART is ready to
         * transmit or has received a byte (model2_state::sound_ready_w). With
         * no 68000 to talk to, the transmitter is always ready, so sending a
         * command immediately makes it ready again. The game enables this
         * interrupt at boot and waits on it. */
        irq_raise(IRQ_SOUND);
    } else {
        /* Control/mode register */
        /* TODO: handle UART control */
    }
}

uint8_t uart_read(uint32_t offset)
{
    if (offset == 0) {
        /* Data register */
        return s_uart_data;
    } else {
        /* Status register: TxRDY | TxEmpty (always ready) */
        return s_uart_status;
    }
}

void sound_load_program(const uint8_t *data, uint32_t size)
{
    s_sound_program = (uint8_t *)data;
    s_sound_program_size = size;
    printf("[sound] Loaded program (%u bytes)\n", size);
}

void sound_load_samples(const uint8_t *data, uint32_t size)
{
    s_sound_samples = (uint8_t *)data;
    s_sound_samples_size = size;
    printf("[sound] Loaded samples (%u bytes)\n", size);
}

void sound_generate_samples(int16_t *buffer, int num_samples)
{
    /* TODO: Run 68000 + MultiPCM/SCSP and generate real audio */
    memset(buffer, 0, num_samples * 2 * sizeof(int16_t)); /* Stereo silence */
}

void sound_run_frame(void)
{
    /* TODO: Run 68000 sound CPU for one frame's worth of cycles */
}
