/*
 * SDL2 platform layer for Model 2 recompilation.
 *
 * Provides windowing, rendering, audio output, input polling,
 * and frame synchronization.
 */

#include "model2recomp/platform.h"

#ifdef _MSC_VER
#pragma comment(lib, "SDL2.lib")
#pragma comment(lib, "SDL2main.lib")
#endif

#include <SDL.h>
#include <stdio.h>

static SDL_Window   *s_window = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture = NULL;
static SDL_AudioDeviceID s_audio_dev = 0;

static int s_width = 0;
static int s_height = 0;

static uint64_t s_frame_start = 0;
static const double FRAME_TIME_MS = 1000.0 / 57.5; /* Model 2 runs at ~57.5 Hz */

bool platform_init(const char *title, int width, int height, int scale)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[platform] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    s_width = width;
    s_height = height;

    s_window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width * scale, height * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!s_window) {
        fprintf(stderr, "[platform] Window creation failed: %s\n", SDL_GetError());
        return false;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        fprintf(stderr, "[platform] Renderer creation failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_RenderSetLogicalSize(s_renderer, width, height);

    /*
     * The framebuffer holds red, green, blue, 0xFF in that byte order, which
     * is the 32-bit value (0xFF << 24) | (b << 16) | (g << 8) | r - and SDL
     * names packed formats from the most significant byte down, so that is
     * ABGR8888.
     *
     * It was RGBX8888, which reads the same value as red 0xFF, green from our
     * blue and blue from our green. Every pixel came out with red at full:
     * on screen that is a solid red pane with the game showing through it,
     * tinted. Screenshots were unaffected because the PPM writer takes bytes
     * 0, 1 and 2 straight off the framebuffer, so the two disagreed and only
     * the window was wrong.
     */
    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
        width, height);
    if (!s_texture) {
        fprintf(stderr, "[platform] Texture creation failed: %s\n", SDL_GetError());
        return false;
    }

    /*
     * Ask SDL what that format means and check it against how the framebuffer
     * is packed. Getting this wrong is invisible in screenshots - they are
     * written straight from the framebuffer - so the window can be wrong on
     * its own, and stay wrong.
     */
    {
        int bpp;
        Uint32 rm, gm, bm, am;

        if (!SDL_PixelFormatEnumToMasks(SDL_PIXELFORMAT_ABGR8888, &bpp,
                                        &rm, &gm, &bm, &am) ||
            bpp != 32 || rm != 0x000000FFu || gm != 0x0000FF00u ||
            bm != 0x00FF0000u) {
            fprintf(stderr, "[platform] ERROR: texture format does not match "
                            "the framebuffer (r=%08X g=%08X b=%08X)\n",
                    rm, gm, bm);
            return false;
        }
    }

    /* Audio setup: 44100 Hz stereo int16 */
    SDL_AudioSpec want = {0};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = NULL; /* Push mode */

    SDL_AudioSpec have;
    s_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_audio_dev > 0) {
        SDL_PauseAudioDevice(s_audio_dev, 0);
    }

    s_frame_start = SDL_GetPerformanceCounter();

    printf("[platform] SDL2 initialized: %dx%d @ %dx\n", width, height, scale);
    return true;
}

void platform_shutdown(void)
{
    if (s_audio_dev > 0) {
        SDL_CloseAudioDevice(s_audio_dev);
        s_audio_dev = 0;
    }
    if (s_texture)  { SDL_DestroyTexture(s_texture);   s_texture = NULL; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
    if (s_window)   { SDL_DestroyWindow(s_window);     s_window = NULL; }
    SDL_Quit();
}

bool platform_poll_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE)
                    return false;
                break;
        }
    }
    return true;
}

void platform_present_frame(const uint8_t *framebuffer, int width, int height)
{
    if (!s_texture || !s_renderer || !framebuffer) return;

    SDL_UpdateTexture(s_texture, NULL, framebuffer, width * 4);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

void platform_queue_audio(const int16_t *samples, int num_samples)
{
    if (s_audio_dev > 0 && samples) {
        SDL_QueueAudio(s_audio_dev, samples, num_samples * 2 * sizeof(int16_t));
    }
}

void platform_frame_sync(void)
{
    uint64_t now = SDL_GetPerformanceCounter();
    double elapsed = (double)(now - s_frame_start) / SDL_GetPerformanceFrequency() * 1000.0;

    if (elapsed < FRAME_TIME_MS) {
        SDL_Delay((uint32_t)(FRAME_TIME_MS - elapsed));
    }

    s_frame_start = SDL_GetPerformanceCounter();
}

void platform_get_mouse(int *x, int *y, bool *left_button, bool *right_button,
                        bool *middle_button)
{
    int mx, my;
    uint32_t buttons = SDL_GetMouseState(&mx, &my);

    /* With the window unfocused the pointer is wherever the desktop left it,
     * which aims the lightgun somewhere arbitrary - and off-screen means
     * "reload" to this game. Park it in the centre instead, so a headless run
     * is repeatable. */
    if (!(SDL_GetWindowFlags(s_window) & SDL_WINDOW_MOUSE_FOCUS)) {
        if (x) *x = s_width / 2;
        if (y) *y = s_height / 2;
        if (left_button) *left_button = false;
        if (right_button) *right_button = false;
        if (middle_button) *middle_button = false;
        return;
    }

    /* Scale mouse coordinates to game resolution */
    int ww, wh;
    SDL_GetWindowSize(s_window, &ww, &wh);

    if (x) *x = (mx * s_width) / ww;
    if (y) *y = (my * s_height) / wh;
    if (left_button)   *left_button   = (buttons & SDL_BUTTON_LMASK) != 0;
    if (right_button)  *right_button  = (buttons & SDL_BUTTON_RMASK) != 0;
    if (middle_button) *middle_button = (buttons & SDL_BUTTON_MMASK) != 0;
}

bool platform_key_pressed(int scancode)
{
    const uint8_t *state = SDL_GetKeyboardState(NULL);
    return state[scancode] != 0;
}
