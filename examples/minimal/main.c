/*
 * Minimal Model 2 recomp example.
 *
 * Initializes the hardware runtime and runs an empty frame loop.
 * Demonstrates the API usage pattern for game-specific projects.
 */

#include "model2recomp/model2recomp.h"
#include "model2recomp/i960.h"
#include "model2recomp/bus.h"
#include "model2recomp/func_table.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* 1. Initialize hardware */
    if (!model2recomp_init("Model 2 Minimal Example", 2, MODEL2_ORIGINAL)) {
        fprintf(stderr, "Failed to initialize model2recomp\n");
        return 1;
    }

    /* 2. Load ROM (optional for this example) */
    /* model2recomp_load_rom("roms/vcop"); */

    /* 3. Register recompiled functions */
    /* func_table_register(0x00000100, my_reset_vector); */

    /* 4. Main frame loop */
    printf("Running frame loop (press ESC to quit)...\n");
    while (model2recomp_begin_frame()) {

        /* Call recompiled game code here:
         *   func_table_call(0x00000100);   // reset vector
         *   func_table_call(0x000001A0);   // main loop
         */

        /* Trigger VBlank at end of frame */
        model2recomp_trigger_vblank();

        /* Render and present */
        model2recomp_end_frame();
    }

    /* 5. Shutdown */
    model2recomp_shutdown();
    return 0;
}
