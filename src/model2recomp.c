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
#include <SDL.h>   /* SDL_SCANCODE_* used by the input polling below */
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
    geo_init();
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

/* Read an entire file into a freshly malloc'd buffer. Caller frees.
 * Returns NULL (and *size_out = 0) if the file can't be opened. */
static uint8_t *read_whole_file(const char *path, uint32_t *size_out)
{
    *size_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }

    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(buf); return NULL; }

    *size_out = (uint32_t)len;
    return buf;
}

/* Load one flat binary <rom_dir>/<name> into a bus region via loader().
 * required: if true, a missing/unreadable file makes the whole load fail. */
static bool load_region(const char *rom_dir, const char *name,
                        void (*loader)(const uint8_t *, uint32_t), bool required)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", rom_dir, name);

    uint32_t size = 0;
    uint8_t *data = read_whole_file(path, &size);
    if (!data) {
        fprintf(stderr, "[model2recomp] %s ROM file: %s\n",
                required ? "MISSING REQUIRED" : "optional (skipped)", path);
        return !required;
    }

    loader(data, size);
    free(data);
    return true;
}

bool model2recomp_load_rom(const char *rom_dir)
{
    printf("[model2recomp] Loading ROMs from: %s\n", rom_dir);

    /* The recompiled i960 code reads constants and tables out of program ROM,
     * so it must be present. Data/extra ROMs feed the (stubbed) renderer and
     * are optional for a boot test. These flat images are produced by
     * tools/rom_loader.py. */
    if (!load_region(rom_dir, "program.bin", bus_load_program_rom, true))
        return false;

    load_region(rom_dir, "data.bin",     bus_load_data_rom,   false);
    load_region(rom_dir, "polygons.bin", bus_load_extra_data, false);
    load_region(rom_dir, "textures.bin", bus_load_texture_rom, false);

    printf("[model2recomp] ROM loading complete\n");
    return true;
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

    /* Map keyboard to service/test/coin/start.
     * MODEL2_HOLD holds one of them down for headless runs, so an automated
     * boot test can reach the service menu and screenshot it. */
    static const char *hold = NULL;
    static bool hold_read = false;
    if (!hold_read) { hold = getenv("MODEL2_HOLD"); hold_read = true; }

    uint8_t in0 = 0xFF;
    if (hold) {
        if (!strcmp(hold, "test"))    in0 &= ~IN0_TEST;
        if (!strcmp(hold, "service")) in0 &= ~IN0_SERVICE;
        if (!strcmp(hold, "start1"))  in0 &= ~IN0_START1;
        if (!strcmp(hold, "coin1"))   in0 &= ~IN0_COIN1;
    }
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

    /* Render video: 3D first into its own bitmap, then composite */
    geo_render_polygons();
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

/*
 * Deliver a pending interrupt to the recompiled code.
 *
 * Recompiled functions are native C, so there is no instruction boundary to
 * interrupt. Instead the handler is called at the field boundary, which is
 * where the guest is already synchronising and where the real VBlank lands.
 *
 * The route to the handler is the one the processor takes: the Model 2
 * interrupt controller drives one of four external lines, the ICR maps that
 * line to a vector, and the interrupt table (PRCB+0x14) holds the handler for
 * each vector from 8 upwards. The handler acks the controller itself.
 *
 * Every pending line gets a turn, highest vector first, because the i960
 * priority is the vector divided by 8. Servicing only the first pending line
 * would starve every source but VBlank, which is asserted on line 0 in every
 * single field.
 *
 * ponytail: no nesting and no pre-emption - each line is serviced at most once
 * per field, in priority order. A source that needs to interrupt a handler
 * already running would need the interrupt table's pending-priority words.
 */
void model2recomp_dispatch_irq(void)
{
    /* Which controller bits drive which external line, per
     * model2_state::irq_update. */
    static const uint32_t line_mask[4] = { 0x001, 0x002, 0x3FC, 0xC00 };

    uint32_t int_tab = bus_read32(bus_i960_prcb() + 0x14);
    if (!int_tab) return;

    for (int line = 3; line >= 0; line--) {
        if (!(irq_request_read() & irq_enable_read() & line_mask[line]))
            continue;

        uint32_t vector = (bus_i960_icr() >> (line * 8)) & 0xFF;
        if (vector < 8)
            continue;   /* line is in IAC mode, which the hardware never uses here */

        uint32_t handler = bus_read32(int_tab + 36 + (vector - 8) * 4);
        if (handler)
            func_table_call(handler);
    }
}

void model2recomp_save_ppm(const char *path)
{
    const uint8_t *fb = video_get_framebuffer();
    if (!fb) return;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[model2recomp] Cannot write screenshot: %s\n", path);
        return;
    }

    fprintf(f, "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++)
        fwrite(&fb[i * 4], 1, 3, f);   /* RGBX -> RGB */
    fclose(f);

    printf("[model2recomp] Wrote %s (%dx%d)\n", path, FB_WIDTH, FB_HEIGHT);
}

/* --- Video field sync (see model2recomp.h) --- */

#define VIDEOCTL_FIELD 0x4   /* bit 2 of 0x0098000C toggles each field */

static long s_frame_limit = 0;
static long s_fields_done = 0;

void model2recomp_set_frame_limit(long fields)
{
    s_frame_limit = fields;
}

uint32_t model2recomp_field_sync(void)
{
    static uint32_t s_next_field_ms = 0;

    uint32_t now = SDL_GetTicks();
    if ((int32_t)(now - s_next_field_ms) >= 0) {
        s_next_field_ms = now + (FRAME_US / 1000);

        /* The geometry engine walks the stream the game submitted last field,
         * then the frame is drawn from the resulting polygon list, then the
         * VBlank interrupt lets the game build the next one. */
        /* Only walk a list the game has actually finished writing. */
        if (geo_take_list_ready())
            geo_parse();
        model2recomp_end_frame();
        model2recomp_trigger_vblank();
        model2recomp_dispatch_irq();

        bool quit = !model2recomp_begin_frame();
        if (s_frame_limit > 0 && ++s_fields_done >= s_frame_limit) {
            printf("[model2recomp] Frame limit (%ld) reached.\n", s_frame_limit);
            /* MODEL2_SCREENSHOT=path writes the final frame as a PPM. A boot
             * test that exits cleanly still tells you nothing about what was
             * on screen; this does. */
            const char *shot = getenv("MODEL2_SCREENSHOT");

            if (shot) model2recomp_save_ppm(shot);
            quit = true;
        }
        if (quit) {
            /* The guest is blocked in a busy-wait; there is no stack to unwind
             * back to the host, so stop here. */
            model2recomp_shutdown();
            exit(0);
        }

        videoctl_write(videoctl_read() ^ VIDEOCTL_FIELD);
    }

    return videoctl_read();
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
    geo_shutdown();
    video_shutdown();
    bus_shutdown();
    platform_shutdown();

    s_initialized = false;
    printf("[model2recomp] Shutdown complete\n");
}
