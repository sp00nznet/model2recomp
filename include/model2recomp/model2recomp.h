/*
 * model2recomp - Sega Model 2 arcade hardware library for static recompilation
 *
 * Provides Model 2 hardware as linkable C libraries:
 *   - i960 CPU context (registers, flags, call stack)
 *   - Memory bus (32-bit address space routing to hardware)
 *   - TGP geometry coprocessor (MB86234 DSP)
 *   - Video renderer (3D polygon rasterizer + System 24 tilemaps)
 *   - Sound (68000 + MultiPCM / SCSP)
 *   - I/O (lightgun, buttons, coins via I/O board)
 *   - Timers, interrupts, EEPROM
 *
 * Reference: MAME model2.cpp (BSD-3-Clause)
 * Original hardware: Intel i960KB @ 25 MHz
 */

#ifndef MODEL2RECOMP_H
#define MODEL2RECOMP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Board variant */
typedef enum {
    MODEL2_ORIGINAL,    /* Original Model 2 (1993) - TGP, 68000+MultiPCM */
    MODEL2A_CRX,        /* Model 2A-CRX (1994) - TGP, 68000+SCSP */
    MODEL2B_CRX,        /* Model 2B-CRX (1994) - SHARC, 68000+SCSP */
    MODEL2C_CRX,        /* Model 2C-CRX (1996) - TGPx4, 68000+SCSP */
} model2_variant_t;

/*
 * Initialize the Model 2 hardware runtime.
 * Call before any other model2recomp function.
 */
bool model2recomp_init(const char *window_title, int scale, model2_variant_t variant);

/*
 * Load ROM set from a directory containing the MAME-format ZIP contents.
 * Expects: program ROMs, data ROMs, texture ROMs, sound ROMs, copro ROMs.
 */
bool model2recomp_load_rom(const char *rom_dir);

/*
 * Frame loop control.
 * begin_frame: polls input, returns false if user quit.
 * end_frame: renders 3D scene + tilemaps, presents to screen, outputs audio.
 */
bool model2recomp_begin_frame(void);
void model2recomp_end_frame(void);

/*
 * Trigger VBlank interrupt processing.
 * Called by recompiled code or frame loop when vertical blank occurs.
 */
void model2recomp_trigger_vblank(void);

/*
 * Video field sync - the frame boundary for a recompiled game.
 *
 * Recompiled game code owns its own main loop and busy-waits on the video
 * status register (0x0098000C, bit 2) for the next field, so it never returns
 * to a host frame loop. The bus routes reads of that register here: once a
 * field period has elapsed this presents the frame, pumps input, ticks timers
 * and flips the field bit, which is what lets the guest's wait terminate.
 *
 * Returns the video status register value.
 */
uint32_t model2recomp_field_sync(void);

/*
 * Stop after this many fields (0 = run until the window is closed). Used by
 * automated boot tests; the guest busy-wait gives no other place to stop.
 */
void model2recomp_set_frame_limit(long fields);

/*
 * Get the rendered framebuffer (496x384 RGBX8888).
 */
const uint8_t *model2recomp_get_framebuffer(void);

/*
 * Shut down and free all resources.
 */
void model2recomp_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_H */
