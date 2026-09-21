/*
 * Generic corpus launcher.
 *
 * One driver for every Model 2 set, so the whole library can be swept at once
 * instead of one hand-written main() per title. It knows nothing about any
 * game: the lifted code is linked in under a fixed prefix, the ROM images come
 * from a directory, and the boot sequence below is the board's, not a title's.
 *
 *   corpus_<set> <rom_dir> [title]
 *
 * MODEL2_MAX_FRAMES caps the run so an automated sweep terminates; the game
 * owns the frame loop and never returns, so the cap is enforced at the field
 * boundary. MODEL2_SHOT_EVERY / MODEL2_SCREENSHOT (model2recomp) capture what
 * it drew, MODEL2_FAST runs it as quickly as the host can.
 *
 * A per-title port wants its own main() eventually - that is where input
 * mapping and cabinet wiring live. This one exists to answer "how far does
 * this set get with no help at all", for all of them, in one run.
 */

#include "model2recomp/model2recomp.h"
#include "model2recomp/i960.h"
#include "model2recomp/bus.h"
#include "model2recomp/func_table.h"
#include "game/functions.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char *argv[])
{
    const char *rom_dir = (argc > 1) ? argv[1] : "roms";
    const char *title   = (argc > 2) ? argv[2] : "Model 2 corpus";

    /* Unbuffered, because the sweep kills whatever is still running when the
     * timeout expires - and a game that had to be killed is precisely the one
     * whose output says why. Redirected to a file, stdout is fully buffered by
     * default, so everything printed before the kill dies with the process and
     * every stuck set reads as having never started. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* The board variant is not a parameter yet: nothing in the library
     * branches on it, so passing MODEL2_ORIGINAL for every set is honest
     * rather than lazy. When 2A lands, this reads it from the catalog. */
    if (!model2recomp_init(title, 2, MODEL2_ORIGINAL)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    if (!model2recomp_load_rom(rom_dir)) {
        fprintf(stderr, "no ROMs in %s\n", rom_dir);
        model2recomp_shutdown();
        return 1;
    }

    game_register_all();

    const char *cap = getenv("MODEL2_MAX_FRAMES");
    model2recomp_set_frame_limit(cap ? strtol(cap, NULL, 10) : 0);

    /*
     * Boot the i960 the way the hardware does. Reset takes its IP and its
     * frame pointer from the initialization boot record in the program ROM -
     * word 1 points at the PRCB, word 3 is the first instruction, and the
     * PRCB's stack-pointer field is at +0x18. The reset stub then relocates
     * the PRCB and the interrupt table into work RAM and issues an IAC
     * "reinitialize processor", which is what hands control to the real
     * firmware entry. Follow that chain; do not guess where it lands.
     */
    uint32_t prcb = bus_read32(0x00000004);
    uint32_t ip   = bus_read32(0x0000000C);

    for (int hop = 0; ip != 0 && hop < 8; hop++) {
        I960_FP = bus_read32(prcb + 0x18);
        I960_SP = I960_FP + 0x40;
        g_i960.IP = ip;

        printf("[corpus] boot %d: IP 0x%08X PRCB 0x%08X FP 0x%08X\n",
               hop, ip, prcb, I960_FP);
        if (!func_table_call(ip)) {
            fprintf(stderr, "[corpus] entry 0x%08X not registered\n", ip);
            break;
        }
        ip = bus_iac_take_reinit(&prcb);
    }

    /* Only reached if the guest's own frame loop never started. Keep the
     * board running so a screenshot still samples whatever it managed. */
    printf("[corpus] guest returned; idling to the frame limit\n");
    while (model2recomp_begin_frame()) {
        model2recomp_trigger_vblank();
        model2recomp_end_frame();
    }

    model2recomp_shutdown();
    return 0;
}
