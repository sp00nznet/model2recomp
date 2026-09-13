# Model 2 Hardware Overview

What the board is, what is on it, and where everything lives in the i960's
address space. If you are wiring a lifted program to this library, this is the
map you are wiring it to.

## The board

Sega Model 2 (1993) came in four revisions. This library implements the
**original** board.

| | Original (1993) | 2A-CRX (1994) | 2B-CRX (1994) | 2C-CRX (1996) |
|---|---|---|---|---|
| Main CPU | i960KB @ 25 MHz | i960KB | i960KB | i960KB |
| Math coprocessor | MB86233 TGP | MB86233 TGP | ADSP-21062 SHARC | TGPx4 |
| Sound | 68000 + YM3438 + 2× MultiPCM | 68000 + SCSP | 68000 + SCSP | 68000 + SCSP |
| Titles | Virtua Cop, Daytona USA, Virtua Fighter 2 | Virtua Cop 2, Manx TT | Sega Rally, Virtual On | Dead or Alive, Over Rev |

`model2_variant_t` in the public API names all four, but only
`MODEL2_ORIGINAL` is implemented — the others currently change nothing but a
line of log output. 2B needs a SHARC core, 2C a TGPx4, and everything from 2A
onward needs SCSP instead of MultiPCM.

## Chips that matter to a recomp

```
        +-------------------+
        |   i960KB 25 MHz   |   <-- this is what you recompile.
        |   main program    |       Everything below is this library.
        +---------+---------+
                  | 32-bit bus
  +---------------+------------------------------------------+
  |               |              |              |            |
  v               v              v              v            v
MB86233        Geometry      Rasterizer    System 24      68000 board
math copro     engine        (custom)      tilemaps       sound
(emulated)     (modelled)                                 (stub)
  |               |              |              |
  |               +-----> polygons -----> framebuffer <--- composited
  |                                                        around the 3D
  +--- matrices, vectors, transformed points
```

The i960 does game logic and builds a **display list** in buffer RAM. The
geometry engine walks that list once per field and emits polygons. The
rasterizer fills them. The tilemap engine draws four scroll layers, split
around the 3D scene. The math coprocessor is a service the i960 calls into for
matrix and vector work — it is not in the display path.

See [graphics-pipeline.md](graphics-pipeline.md) for the display list and
[tgp-coprocessor.md](tgp-coprocessor.md) for the coprocessor.

## Memory map

Everything the bus decodes, in address order. `src/bus.c` is the authority; this
table is it in prose.

| Range | Size | What |
|---|---|---|
| `0x00000000`–`0x001FFFFF` | 2 MB | **Program ROM.** The i960 program. Lifted to C; the image is still mapped because code reads constants and tables out of it. |
| `0x00200000`–`0x0021FFFF` | 128 KB | Program RAM (original board only) |
| `0x00220000`–`0x0023FFFF` | 128 KB | Program ROM extension, mapped from ROM offset `0x20000` |
| `0x00500000`–`0x005FFFFF` | 1 MB | **Work RAM.** Where the game lives. Fast-pathed in the bus. |
| `0x00800000`–`0x00803FFF` | 16 KB | **Geometry registers.** A write below `0x1000` appends an *opcode* word to the display list, built from the register address and the value written. |
| `0x00804000`–`0x00807FFF` | 16 KB | **Geometry program port.** A write appends an *operand* word to the display list. |
| `0x00880000`–`0x00883FFF` | 16 KB | **Copro function port.** A write pushes a command into the coprocessor's input FIFO, the function number taken from the address. |
| `0x00884000`–`0x00887FFF` | 16 KB | **Copro data FIFO.** Write pushes an argument; read pops a result. |
| `0x00900000`–`0x0091FFFF` | 128 KB | **Buffer RAM.** The display list itself, and the coprocessor's banked window onto it. Mirrored to `0x0097FFFF`. |
| `0x00980000` | 4 | Copro control — the microcode upload gate and boot trigger |
| `0x00980004` | 4 | FIFO control — bit 0 reads 1 when the copro output FIFO is empty |
| `0x00980008` | 4 | Geo control — the geometry microcode upload gate |
| `0x0098000C` | 4 | **Video control / field status.** Bit 2 is the field flag. **Reading this is the frame boundary** — see [execution-model.md](execution-model.md). |
| `0x00980030`–`0x0098003F` | 16 | TGP identification |
| `0x00E00000`–`0x00E00037` | 56 | CPU control (wait states) |
| `0x00E80000` | 4 | Interrupt request (read) / acknowledge (write) |
| `0x00E80004` | 4 | Interrupt enable |
| `0x00F00000`–`0x00F0000F` | 16 | Four countdown timers off the 25 MHz clock |
| `0x01000000`–`0x0100FFFF` | 64 KB | System 24 tile RAM (layer maps) |
| `0x01040000` / `0x01060000` | 2 each | Horizontal / vertical sync registers — these move the CRTC origin, and the rasterizer honours them |
| `0x01080000`–`0x010FFFFF` | 512 KB | System 24 character RAM (8×8 4bpp tiles) |
| `0x01800000`–`0x01803FFF` | 16 KB | **Palette RAM.** `0x0000`–`0x0FFF` (u16 index) is tilemap colour; `0x1000`–`0x13FF` is polygon colour. |
| `0x01810000`–`0x0181BFFF` | 48 KB | **Colour translate RAM.** 32 ramps × 256 entries × 3 channels. |
| `0x0181C000` | 4 | 3D master z-clip |
| `0x01C00000`–`0x01C00FFF` | 4 KB | **I/O board dual-port RAM** (byte lanes `0x00FF00FF`) |
| `0x01C80000`–`0x01C80003` | 4 | UART to the sound board |
| `0x01D00000`–`0x01D03FFF` | 16 KB | Backup SRAM (high scores, settings) |
| `0x02000000`–`0x03FFFFFF` | 32 MB | **Data ROM.** Game data. |
| `0x06000000`–`0x06FFFFFF` | 16 MB | **Extra data / polygon ROM.** 3D models, read by the geometry engine. |
| `0x10000000`–`0x101FFFFF` | — | Render mode |
| `0x10400000`–`0x105FFFFF` | — | Polygon count (read) |
| `0x11600000`–`0x116FFFFF` | 2× 512 KB | Framebuffer VRAM banks A and B |
| `0x12000000`–`0x123FFFFF` | 2 MB | Texture RAM sheet 0 (mirrored) |
| `0x12400000`–`0x127FFFFF` | 2 MB | Texture RAM sheet 1 (mirrored) |
| `0x12800000`–`0x1281FFFF` | 128 KB | Luma RAM — one byte per 32-bit slot, 32 KB of data |
| `0xFF000000`–`0xFF0000FF` | — | i960 IAC (inter-agent communication) messages |

Two things surprise people:

- **The display list is built through two different ports.** Register writes
  below `0x00800FFF` append *opcodes*; writes to `0x00804000` append
  *operands*. Implement only the second and you get a stream of operands with
  no opcodes, which parses as plausible nonsense.
- **Luma RAM is byte-wide in a dword-spaced window.** `0x12800000`–`0x1281FFFF`
  is 128 KB of address space holding 32 KB of data, one byte in each dword.

## ROM regions

`model2recomp_load_rom(dir)` loads flat binaries out of a directory. They are
not ROM files — they are the interleaved, region-sized images a game project's
tooling produces from a ROM set (Model 2 ROMs are 16-bit halves interleaved
into 32-bit words).

| File | Loaded to | Needed for |
|---|---|---|
| `program.bin` | `0x00000000` | **Required.** The i960 program image. |
| `data.bin` | `0x02000000` | Game data |
| `polygons.bin` | `0x06000000` | 3D models — without it the geometry engine's object-data command does nothing |
| `textures.bin` | texture sheets | Textured polygons |
| `copro_tables.bin` | coprocessor | sin/cos, atan, 1/x, 1/√x tables from the CPU board. **Without it the coprocessor computes zero for every transcendental.** |

Only `program.bin` is required; everything else degrades a subsystem rather
than failing the load.

## Interrupts

The Model 2 interrupt controller has 12 lines. `0x00E80000` reads pending and
writes acknowledge (the written value is ANDed off); `0x00E80004` is the
enable mask.

The i960 reaches a handler by a fixed path: the interrupt control register
selects a vector per external line, and the vector indexes the interrupt table
at `PRCB + 0x14`. Virtua Cop's ICR is `0x0F0E0D0C`, which makes VBlank vector
12.

Recompiled code has no instruction boundary for an interrupt to land on, so
this library dispatches at the field boundary — which is where VBlank arrives
on real hardware anyway. See [execution-model.md](execution-model.md).

## What is not modelled

- **Sound.** `src/sound.c` answers the UART handshake so the game does not
  wait forever. There is no 68000, no YM3438, no MultiPCM, and no audio.
- **Host input reaching the game.** `io_set_input` and `io_set_lightgun` store
  state, and the I/O board's DPRAM command protocol works, but nothing copies
  the former into the latter — so buttons do not reach the guest.
- **Board variants** other than the original.
- **Rasterizer filtering.** Point sampling only: no bilinear, no mipmaps, no
  microtexture.
