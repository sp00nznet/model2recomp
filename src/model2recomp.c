/*
 * model2recomp - Main lifecycle and frame loop.
 *
 * Initializes all Model 2 hardware subsystems, loads ROMs,
 * and provides the frame loop API.
 */

#include "model2recomp/model2recomp.h"
#include "model2recomp/i960.h"
#include "model2recomp/bus.h"
#include "model2recomp/func_table.h"
#include "model2recomp/video.h"
#include "model2recomp/sound.h"
#include "model2recomp/io.h"
#include "model2recomp/timer.h"
#include "model2recomp/eeprom.h"
#include "model2recomp/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_WIDTH  496
#define FB_HEIGHT 384

static model2_variant_t s_variant;
static bool s_initialized = false;

/* Audio buffer for one frame (~735 stereo samples at 44100/60) */
#define AUDIO_SAMPLES_PER_FRAME 735
static int16_t s_audio_buffer[AUDIO_SAMPLES_PER_FRAME * 2];

/* Frame timing (microseconds per frame at ~57.5 Hz) */
#define FRAME_US 17391

bool model2recomp_init(const char *window_title, int scale, model2_variant_t variant)
{
    s_variant = variant;

    printf("=== model2recomp v0.1.0 ===\n");
    printf("Variant: ");
    switch (variant) {
        case MODEL2_ORIGINAL: printf("Original Model 2\n"); break;
        case MODEL2A_CRX:     printf("Model 2A-CRX\n"); break;
        case MODEL2B_CRX:     printf("Model 2B-CRX\n"); break;
        case MODEL2C_CRX:     printf("Model 2C-CRX\n"); break;
    }

    /* Initialize subsystems */
    bus_init();
    i960_reset();
    func_table_init();
    video_init();
    sound_init();
    io_init();
    timer_init();
    eeprom_init();

    /* Initialize platform (SDL2) */
    if (!platform_init(window_title, FB_WIDTH, FB_HEIGHT, scale)) {
        fprintf(stderr, "[model2recomp] Platform init failed\n");
        return false;
    }

    s_initialized = true;
    printf("[model2recomp] Initialization complete\n");
    return true;
}

bool model2recomp_load_rom(const char *rom_dir)
{
    printf("[model2recomp] Loading ROMs from: %s\n", rom_dir);
    /* TODO: Implement ROM loading from MAME-format directory
     *
     * For Virtua Cop (original Model 2), expected files:
     *   Program ROM:  epr-17166?.ic? (4x 512KB = 2MB)
     *   Data ROM:     mpr-17164.ic? (8MB data)
     *   Texture ROM:  mpr-17148.ic? (texture data)
     *   Sound ROM:    epr-17168.ic? (68000 program)
     *   Sample ROM:   mpr-17149.ic? (MultiPCM samples)
     *   Copro ROM:    internal TGP microcode
     *
     * ROM files should be loaded into the appropriate bus memory regions.
     */
    printf("[model2recomp] ROM loading not yet implemented\n");
    return true; /* Don't fail - allow stub operation */
}

bool model2recomp_begin_frame(void)
{
    if (!s_initialized) return false;

    /* Poll platform events (input, quit) */
    if (!platform_poll_events())
        return false;

    /* Update input from mouse (lightgun) */
    int mx, my;
    bool mleft, mright;
    platform_get_mouse(&mx, &my, &mleft, &mright);

    /* Map mouse to lightgun coordinates */
    io_set_lightgun(0, (uint16_t)mx, (uint16_t)my, false);

    /* Map mouse buttons to triggers */
    uint8_t in1 = 0xFF;
    if (mleft)  in1 &= ~IN1_P1_TRIGGER;
    if (mright) in1 &= ~IN1_P2_TRIGGER;
    io_set_input(1, in1);

    /* Map keyboard to service/test/coin/start */
    uint8_t in0 = 0xFF;
    if (platform_key_pressed(SDL_SCANCODE_5))     in0 &= ~IN0_COIN1;
    if (platform_key_pressed(SDL_SCANCODE_6))     in0 &= ~IN0_COIN2;
    if (platform_key_pressed(SDL_SCANCODE_9))     in0 &= ~IN0_SERVICE;
    if (platform_key_pressed(SDL_SCANCODE_F2))    in0 &= ~IN0_TEST;
    if (platform_key_pressed(SDL_SCANCODE_1))     in0 &= ~IN0_START1;
    if (platform_key_pressed(SDL_SCANCODE_2))     in0 &= ~IN0_START2;
    io_set_input(0, in0);

    return true;
}

void model2recomp_end_frame(void)
{
    if (!s_initialized) return;

    /* Advance timers */
    timer_tick(FRAME_US);

    /* Run sound CPU and generate audio */
    sound_run_frame();
    sound_generate_samples(s_audio_buffer, AUDIO_SAMPLES_PER_FRAME);

    /* Render video */
    video_render_frame();

    /* Present to screen */
    const uint8_t *fb = video_get_framebuffer();
    if (fb) {
        platform_present_frame(fb, FB_WIDTH, FB_HEIGHT);
    }

    /* Queue audio */
    platform_queue_audio(s_audio_buffer, AUDIO_SAMPLES_PER_FRAME);

    /* Frame sync */
    platform_frame_sync();
}

void model2recomp_trigger_vblank(void)
{
    /* Fire VBlank interrupt (bit 0) */
    irq_raise(1);
}

const uint8_t *model2recomp_get_framebuffer(void)
{
    return video_get_framebuffer();
}

void model2recomp_shutdown(void)
{
    if (!s_initialized) return;

    printf("[model2recomp] Shutting down...\n");

    eeprom_shutdown();
    io_shutdown();
    sound_shutdown();
    video_shutdown();
    bus_shutdown();
    platform_shutdown();

    s_initialized = false;
    printf("[model2recomp] Shutdown complete\n");
}
