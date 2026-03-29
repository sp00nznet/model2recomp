# model2recomp

Sega Model 2 arcade hardware runtime library for static recompilation.

## Overview

Provides Model 2 hardware as a linkable C library, replacing the i960 CPU with statically recompiled native code while keeping all other hardware (TGP, video, sound, I/O) functional.

## Hardware Coverage

| Component | Status | Notes |
|-----------|--------|-------|
| i960 CPU context | Done | Registers, call/return cache, condition codes |
| Memory bus | Done | Full 32-bit address space routing |
| Function dispatch | Done | Hash table for indirect calls |
| TGP geometry DSP | Stub | FIFO + control registers wired |
| 3D rasterizer | Stub | Framebuffer conversion only |
| System 24 tilemaps | Stub | RAM read/write only |
| Palette | Stub | RAM read/write only |
| Sound (68000+MultiPCM) | Stub | UART wired, no audio generation |
| I/O board | Done | Lightgun, buttons, coins, DPRAM |
| Timers | Done | 4x 25 MHz countdown timers |
| IRQ system | Done | Request/ack/enable |
| EEPROM/SRAM | Done | 16KB backup with save/load |
| Platform (SDL2) | Done | Window, rendering, audio, input |

## Supported Variants

- **Original Model 2** (1993) - Virtua Cop, Daytona USA, etc.
- **Model 2A-CRX** (1994) - Virtua Cop 2, Manx TT, etc.
- **Model 2B-CRX** (1994) - Sega Rally, Virtual On, etc.
- **Model 2C-CRX** (1996) - Dead or Alive, etc.

## Building

```bash
cmake -B build
cmake --build build --config Release
```

Requires SDL2 development libraries.

## Usage

Game projects embed model2recomp as a subdirectory:

```cmake
add_subdirectory(ext/model2recomp)
target_link_libraries(my_game PRIVATE model2recomp)
```

See `examples/minimal/main.c` for the API pattern.

## Reference

Hardware documentation derived from MAME (BSD-3-Clause).
Original MAME Model 2 driver by R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels.
