/*
 * model2recomp - Sega Model 2 arcade hardware library for static recompilation
 *
 * Provides Model 2 hardware as linkable C libraries:
 *   - i960 CPU context (registers, flags, call stack)
 *   - Memory bus (32-bit address space routing to hardware)
 *   - Geometry engine + 3D polygon rasterizer (modelled directly)
 *   - MB86233 "TGP" math coprocessor (emulated; runs game-uploaded microcode)
 *   - System 24 tilemaps, palette, colour translate and luma RAM
 *   - Sound (68000 + MultiPCM / SCSP) - stub, no audio
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

/*
 * Board variant.
 *
 * Only MODEL2_ORIGINAL is implemented. The others are accepted and named in
 * the startup log, but no code branches on them: 2B needs a SHARC core, 2C a
 * TGPx4, and everything from 2A onward needs SCSP rather than MultiPCM.
 */
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
 * Load flat, region-sized binary images from a directory. These are not ROM
 * files - a game project's tooling produces them from a ROM set, interleaving
 * the 16-bit halves into 32-bit words.
 *
 *   program.bin       required; the i960 program image
 *   data.bin          game data
 *   polygons.bin      3D models, read by the geometry engine
 *   textures.bin      texture sheets
 *   copro_tables.bin  coprocessor sin/cos, atan, 1/x and 1/sqrt(x) tables
 *   copro_data.bin    the coprocessor's external data socket (Daytona USA)
 *
 * Only program.bin is required; a missing image disables its subsystem rather
 * than failing the load.
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
 * Call the recompiled interrupt handler for the highest pending interrupt, if
 * any. Recompiled code has no interruptible instruction boundary, so this runs
 * at the field boundary. Called from model2recomp_field_sync().
 */
void model2recomp_dispatch_irq(void);

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
 * Write the current framebuffer to a binary PPM. Also happens automatically at
 * the frame limit when MODEL2_SCREENSHOT names a path.
 */
void model2recomp_save_ppm(const char *path);

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
