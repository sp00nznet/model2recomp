# Getting Started with Model 2 Static Recompilation

This guide takes you from a Model 2 ROM dump to a native executable running the
game's own code. It assumes you have never done this before and explains why
each step exists.

If you only want to build and run an *existing* port, you are in the wrong
place — go to that port's own repository, for example
[virtuacop](https://github.com/sp00nznet/virtuacop), which has a shorter guide
for exactly that.

## What you need

- **A C17 compiler** — MSVC 2022 or GCC 12+, both tested
- **CMake ≥ 3.20**
- **SDL2** development libraries (vcpkg on Windows, your package manager
  elsewhere)
- **Python 3.10+** for the ROM and lifting tooling
- **A Model 2 ROM set you dumped or own.** None are distributed here and none
  ever will be.
- **MAME** — not as a dependency, as a reference. You will want to see what the
  game is supposed to look like.

Useful but optional:

- **A disassembler** you are comfortable in, for the i960 functions that fight
  back.

## Step 0: Understand the split

Two repositories, and it matters which is which.

- **`model2recomp/`** — this library. Building it produces `model2recomp.lib`
  (or `.a`) plus, optionally, an example and two test harnesses. It has no
  game `main()` and can never produce a playable executable.
- **`your-game/`** — *your* project. It holds the lifted C, a `main()`, and the
  ROM tooling; it links this library and produces the `.exe`.

```bash
git clone https://github.com/sp00nznet/model2recomp.git

# sanity-check it builds; your project will build it again via add_subdirectory
cmake -S model2recomp -B model2recomp/build \
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build model2recomp/build --config Release
```

If that produces a library, the rest of this guide is about your side.

## Step 1: Work out what board your game is

Only the **original Model 2** (1993) is implemented. Check your title:

| Board | Copro | Sound | Supported |
|---|---|---|---|
| Model 2 (1993) | MB86233 TGP | 68000 + MultiPCM | **Yes** |
| 2A-CRX (1994) | MB86233 TGP | 68000 + SCSP | No (sound differs) |
| 2B-CRX (1994) | ADSP-21062 SHARC | 68000 + SCSP | No (different DSP) |
| 2C-CRX (1996) | TGPx4 | 68000 + SCSP | No (different DSP) |

Virtua Cop, Daytona USA and Virtua Fighter 2 are original-board titles.

MAME's driver file for your game names the board; so does the MAME machine
description.

## Step 2: Turn the ROM set into flat binaries

Model 2 ROMs are 16-bit halves interleaved into 32-bit words, and MAME's
`ROM_LOAD32_WORD` entries describe exactly how. This library does not parse ROM
sets — it loads **flat, region-sized binary images** that your tooling
produces.

You need, at minimum:

| Image | From | Notes |
|---|---|---|
| `program.bin` | the `maincpu` region | The i960 program. Required. |
| `data.bin` | `main_data` | Game data |
| `polygons.bin` | `polygons` | 3D models |
| `textures.bin` | `textures` | Texture sheets |
| `copro_tables.bin` | `copro_tgp_tables` | sin/cos, atan, 1/x, 1/√x. **Do not skip this** — without it every transcendental returns zero and nothing rotates. |

The images are *region-sized*, not data-sized: a 16 MB region holding 4 MB of
data is a 16 MB file. Masking to the data extent instead folds high addresses
back onto real geometry and produces vertices with nonsense z values.

[virtuacop's `tools/rom_loader.py`](https://github.com/sp00nznet/virtuacop/blob/main/tools/rom_loader.py)
is a worked example: give it a MAME ZIP and an output directory and it writes
all of the above. Copy it and change the ROM table at the top.

## Step 3: Disassemble and find functions

The i960 is a RISC with four instruction formats (REG, COBR, CTRL, MEM), so a
disassembler is a weekend, not a month. What is harder is deciding where
functions *start*.

Three sources of function entry points, and you need all three:

1. **Call targets.** Every `call`, `callx` and `bal` displacement.
   **`bal` counts.** It is branch-and-link — a call that returns through `g14`.
   Treating it as a plain branch truncates the caller at the first `bal`.
2. **The interrupt table.** Handlers are never called or branched to by any
   instruction. Follow `SAT + 4` → PRCB → `PRCB + 0x14` → the table, and take
   the handler for each vector. Without this, your VBlank handler sits as
   unreachable trailing code inside whatever function precedes it, and the game
   does no per-frame work at all.
3. **Post-return prologues.** Code after a `ret` that looks like a function
   start. Watch for alignment padding between functions — a scan that stops at
   the first zero word will miss everything past it.

## Step 4: Lift to C

Translate each function to a C function operating on the shared i960 context.
The shape that works:

```c
void game_000029B0(void)
{
    I960_G(4) = op_ld(0x0050F364u);            /* ld 0x50F364, g4 */
    op_cmpi((int32_t)0, (int32_t)I960_G(4));
    if (i960_test_cc(COND_E)) goto L_000029C8;
    ...
    i960_do_ret();
    return;
}
```

Branches inside a function become `goto` to a label; calls become
`func_table_call(target)`; indirect branches become `func_table_call` on a
register.

Three things the i960 will punish you for getting wrong:

- **REG operand mode bits.** Bit 11 and bit 12 say whether `src1`/`src2` are
  registers or literals; bit 13 says whether the destination is a
  floating-point register. Ignore them and every floating-point operation
  silently reads the wrong operand — which produces a running game that draws
  garbage, the worst kind of bug.
- **Float literals.** In a real-mode instruction, literal `0x10` means `0.0`
  and literal `0x16` means `1.0`. A disassembler that does not know the
  instruction is a float op will print them as register names.
- **`bal` is a call.** See above.

MAME's `cpu/i960/i960.cpp` is the opcode table. Extract from it rather than
transcribing from the manual; the manual and the silicon disagree in places and
MAME follows the silicon.

[virtuacop's `tools/i960_lifter.py`](https://github.com/sp00nznet/virtuacop/blob/main/tools/i960_lifter.py)
is a working implementation of all of the above.

## Step 5: Create your game project

```cmake
cmake_minimum_required(VERSION 3.20)
project(mygame C)
set(CMAKE_C_STANDARD 17)

find_package(SDL2 REQUIRED)

set(MODEL2RECOMP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MODEL2RECOMP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
add_subdirectory(ext/model2recomp)

file(GLOB GAME_RECOMP_SOURCES src/recomp/game_code_*.c)
add_library(mygame_code STATIC src/recomp/game_register.c ${GAME_RECOMP_SOURCES})
target_include_directories(mygame_code PUBLIC include)
target_link_libraries(mygame_code PUBLIC model2recomp)

add_executable(mygame src/main/main.c)
target_link_libraries(mygame PRIVATE mygame_code model2recomp SDL2::SDL2main)
```

`find_package(SDL2)` at top level matters: SDL2's imported targets are
directory-scoped where they are found, and your executable needs
`SDL2::SDL2main` directly.

## Step 6: Write `main()`

```c
#include "model2recomp/model2recomp.h"
#include "model2recomp/func_table.h"
#include "model2recomp/i960.h"
#include "model2recomp/bus.h"

int main(int argc, char **argv)
{
    model2recomp_init("My Game", 2, MODEL2_ORIGINAL);
    model2recomp_load_rom(argc > 1 ? argv[1] : "roms");
    mygame_register_all();

    /* Follow the boot chain rather than hardcoding the firmware entry. */
    uint32_t ip = RESET_IP, prcb = RESET_PRCB;
    for (int hop = 0; ip && hop < 8; hop++) {
        I960_FP = bus_read32(prcb + 0x18);
        I960_SP = I960_FP + 0x40;
        if (!func_table_call(ip)) break;
        ip = bus_iac_take_reinit(&prcb);
    }

    /* Reaching here means init returned - the game did not enter its loop. */
    while (model2recomp_begin_frame()) {
        model2recomp_trigger_vblank();
        model2recomp_end_frame();
    }

    model2recomp_shutdown();
    return 0;
}
```

`RESET_IP` and `RESET_PRCB` come from the initial memory image at `0x00000000`
in your `program.bin`: the PRCB pointer is at offset `0x04` and the first IP at
offset `0x0C`.

**Read [Execution Model](technical/execution-model.md) now, not later.** The
frame loop above is a fallback. In a working port the guest never returns, and
the real frame boundary is inside `model2recomp_field_sync()`.

## Step 7: Boot it, and expect a hang

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
./build/Release/mygame.exe ./roms
```

The first run hangs. It always hangs. Work the list in
[Debugging](technical/debugging.md) — `MODEL2_TRACE=200`, look at the last
line, find out what the guest is waiting for.

The order things typically break in, from experience with Virtua Cop:

1. **Lifter bugs.** `bal` truncating functions; REG mode bits ignored; opcode
   mappings wrong. These show up as immediate crashes or nonsense.
2. **Handshakes.** The I/O board's command acknowledge; the coprocessor's
   FIFO-empty poll. These show up as clean hangs in a tight loop.
3. **The field sync.** If reads of `0x0098000C` are not routed to
   `model2recomp_field_sync()`, nothing ever advances.
4. **Missing interrupt handlers.** The game boots, the frame counter moves, and
   the game does nothing — because its VBlank handler was never lifted.
5. **The display list.** Both ports, opcodes and operands. Half of it parses as
   plausible nonsense.

## Step 8: Get a picture

Once the guest is in its own frame loop, add a frame limit and a screenshot so
you can iterate without watching:

```bash
MY_MAX_FRAMES=2000 MODEL2_SCREENSHOT=out.ppm ./build/Release/mygame.exe ./roms
```

Attract mode can take 20 seconds of game time to start drawing, so a short boot
test legitimately shows black. Sample across a run with
`MODEL2_SHOT_EVERY=100` rather than trusting one frame.

## Step 9: Beyond the picture

At this point you have the game running and drawing. What is left is the same
list as everyone else's:

- **Input** — the DPRAM protocol works, but nothing copies host input into it
  yet. This is the highest-value small job in the project.
- **Sound** — a 68000 core and MultiPCM.
- **Rasterizer fidelity** — bilinear, mipmaps, microtexture.

## Further reading

- [Execution Model](technical/execution-model.md) — the frame boundary,
  interrupts, boot
- [Hardware Overview](technical/hardware-overview.md) — the full memory map
- [Graphics Pipeline](technical/graphics-pipeline.md) — display list to pixel
- [TGP Coprocessor](technical/tgp-coprocessor.md) — the DSP and why it cannot
  be stubbed
- [Debugging](technical/debugging.md) — traces, screenshots, call-graph search
