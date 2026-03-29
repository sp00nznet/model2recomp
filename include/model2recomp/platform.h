/*
 * Platform abstraction layer (SDL2).
 *
 * Provides windowing, rendering, audio output, and input polling.
 */

#ifndef MODEL2RECOMP_PLATFORM_H
#define MODEL2RECOMP_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize SDL2 window and audio */
bool platform_init(const char *title, int width, int height, int scale);

/* Shut down SDL2 */
void platform_shutdown(void);

/* Poll SDL2 events. Returns false if quit requested. */
bool platform_poll_events(void);

/* Present a framebuffer to the window (RGBX8888, width*height*4 bytes) */
void platform_present_frame(const uint8_t *framebuffer, int width, int height);

/* Queue audio samples for playback (stereo int16, interleaved L/R) */
void platform_queue_audio(const int16_t *samples, int num_samples);

/* Frame sync (~57.5 Hz for Model 2, ~60 Hz NTSC) */
void platform_frame_sync(void);

/* Get mouse position and button state (for lightgun) */
void platform_get_mouse(int *x, int *y, bool *left_button, bool *right_button);

/* Get keyboard state */
bool platform_key_pressed(int scancode);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_PLATFORM_H */
