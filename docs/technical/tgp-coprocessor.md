# The TGP Math Coprocessor

`src/copro.c` — a Fujitsu MB86233/86234 DSP emulator and the Model 2 board
wired around it. Ported from MAME's `cpu/mb86233/mb86233.cpp` and the
`model2_tgp_state` glue in `sega/model2.cpp`.

## Two things called TGP

Model 2 has two TGP-derived blocks, and confusing them costs a week:

| | Geometry engine | Math coprocessor |
|---|---|---|
| Where | `src/geometry.c` | `src/copro.c` |
| Ports | `0x00800000`, `0x00804000` | `0x00880000`, `0x00884000` |
| Microcode | fixed, Sega's | **uploaded by the game at boot** |
| Modelled as | the *result* — the display list is parsed directly | a real DSP, instruction by instruction |

The geometry engine can be modelled directly because its microcode never
changes; MAME does the same. The math coprocessor cannot, because the game
uploads its own program and then depends on exactly what that program computes.

## Why you cannot stub it

The game does not build its transformation matrices. It asks the coprocessor
to. A typical sequence from Virtua Cop's object loop:

```c
st  r12, 0x00880050     /* function 0x05 */
st  r13, 0x00880120     /* function 0x12 */
st  g4,  0x00884000     /* push x  */
st  g6,  0x00884000     /* push y  */
st  g7,  0x00884000     /* push z  */
st  g4,  0x00884000     /* push angle (integer) */
st  r3,  0x00884000     /* push scale x */
st  r3,  0x00884000     /* push scale y */
st  r3,  0x00884000     /* push scale z */
st  g14, 0x008000B0     /* geo command 0x0B: matrix write */
ld  0x00884000, r4      /* pop 12 matrix words... */
ld  0x00884000, r5
...
stq r4, 0x00804000      /* ...straight into the display list */
```

If the FIFO is a loopback, those twelve reads return the nine words that were
pushed plus whatever was stale, and the "matrix" the geometry engine gets is a
position, a raw integer angle, three `2.0`s and a NaN. Transformed points come
out with negative depths, a large fraction of polygons sort into the nearest
z bucket, and the scene collapses into a flat blob in front of everything.

That failure looks exactly like a broken floating-point implementation in the
recompiled CPU, which is what it was misdiagnosed as for a while. It is worth
knowing what it looks like.

## Ports

| Address | Direction | What |
|---|---|---|
| `0x00880000`–`0x00883FFF` | write | **Function port.** Pushes a command word into the input FIFO: `(data & 0x800FFFFF) | (((offset >> 2) & 0xFF) << 23)` — the function number comes from the *address*. |
| `0x00884000`–`0x00887FFF` | write | Push an argument into the input FIFO. While the upload gate is open, writes go to program memory instead. |
| `0x00884000`–`0x00887FFF` | read | Pop a result from the output FIFO. |
| `0x00980000` | write | Control. A `0 -> 1` transition on bit 31 opens the microcode upload gate and halts the DSP; `1 -> 0` boots it. |
| `0x00980004` | read | Bit 0 is 1 when the output FIFO is empty. **Polarity matters** — the game spins on this before uploading, so getting it backwards deadlocks the boot. |

Functions Virtua Cop uses, by frequency over 1600 fields: `0x11`, `0x06`,
`0x05`, `0x12`, `0x16`, `0x15`, `0x14` (about 30k calls each), then `0x10`,
`0x1A`, `0x07`, `0x03`, `0x13`, `0x09`, and a handful of `0x2D`, `0x0A`, `0x08`,
`0x1C`, `0x1B`. What they *mean* is defined by the microcode, not by us — which
is the point of emulating rather than HLE-ing it.

## The DSP

MB86233: 32-bit floating-point DSP, 4 KB of program RAM, two data banks
(`0x000`–`0x0FF` and `0x200`–`0x3FF`), a 16-entry register file, and an IO
space. Only floating-point mode is implemented — every Sega program switches to
it at startup and stays there.

Instruction classes, by top 6 bits of the opcode:

| Opcode | Class |
|---|---|
| `0x00` | `lab` — load A and B from two memory operands |
| `0x07` | `ld` / `mov` — every combination of memory, IO, program and register |
| `0x0D` | `stm` — mode register (float mode, rounding) |
| `0x0E` | `lipl` / `lia` / `lib` / `lid` — load 24-bit immediate |
| `0x0F` | `rep` / `clr` / `set` |
| `0x10`–`0x1F` | `ldi` — load immediate into a named register |
| `0x2F`, `0x3F` | conditional branch, call, return, and `ldif` |

Every instruction can carry an ALU operation in parallel: `fadd`, `fsbd`,
`fml`, `fmsd`, `fmrd`, `fdvd`, `fabd`, `fned`, integer add/sub/and/or/xor/not,
shifts, and float↔int conversion. Integer results land immediately; float
results land *after* the transfer, which is a real hardware behaviour and
matters when an instruction reads and writes the same register.

## The board around it

### IO space

| Address | What |
|---|---|
| `0x0020`–`0x0023` | sin/cos — write sets the angle base, read returns the value |
| `0x0024`–`0x0027` | atan — four base words; the read combines them with sign and quadrant handling |
| `0x0028`–`0x0029` | 1/x |
| `0x002A`–`0x002B` | 1/√x |
| `0x0000`–`0xFFFF` | when the bank register is enabled, the whole IO space is a window onto buffer RAM instead |

All four transcendentals are **table lookups in a ROM on the CPU board** —
`opr-14742a.45` and `opr-14743a.46`, interleaved into a 256 KB image. Load it
as `copro_tables.bin`. Without it every lookup returns zero, which is not a
crash and not obviously wrong until nothing rotates.

The bank window shadows the math ports when enabled, which is not a bug: the
microcode turns the bank off before using them.

### Register file

Sixteen entries, of which four are wired to the board:

| Index | Access | What |
|---|---|---|
| `0` | write | LEDs / busy flag (ignored) |
| `1` | read | Pop the input FIFO |
| `2` | write | Push the output FIFO |
| `3` | write | Bank register — bits 22–23 enable the external window, bits 16–23 supply its high address bits |

The external window reaches buffer RAM at `adr & 0x400000`, and the copro data
ROM socket at `adr & 0x800000`.

**That socket is not implemented.** `tgp_memory_r()` returns 0 for it, which is
correct on Virtua Cop — its `copro_data` region is declared and empty — and
wrong on any title that fills it. Daytona USA puts 4 MB of collision and
height-map data there. Wiring it up is a ROM region, a load hook and two lines;
it is left undone because nothing in the project can currently test it. See
[porting-targets.md](porting-targets.md).

## Scheduling: how a DSP runs without a scheduler

MAME interleaves the i960 and the DSP and uses FIFO flow control to halt
whichever side is ahead. A recompiled i960 cannot be halted — there is no
scheduler and no instruction boundary.

The DSP is a pure slave: its program loops reading the input FIFO. So it is run
**on demand**, inside the bus access that needs its output:

```c
static void copro_pump(void)
{
    if (!booted) return;
    if (input_empty && output_nonempty) return;   /* nothing to gain */
    starved = false;
    copro_execute(2000000);                       /* until it starves, or the budget ends */
}
```

`copro_execute` returns as soon as the DSP reads an empty input FIFO — that is
the only thing that can stall it, and it means there is no more work. Both
`copro_fifo_read()` and `copro_output_empty()` pump first.

The consequences of this design:

- **The i960 half never blocks.** MAME's FIFOs are 8 deep and halt the pusher;
  these are 8192 deep and never fill in practice. Order is preserved, so the
  game reads the same values in the same sequence.
- **Work can run ahead.** Pushing ten commands and then reading one result
  executes all ten. That is a pipeline, not a reordering — nothing observable
  changes.
- **The budget is a safety net.** Two million instructions is far more than any
  command needs; hitting it means the microcode is in a loop that neither
  outputs nor starves, and the warning it prints is the signal to go look.

Measured cost on Virtua Cop: about 5,800 DSP instructions per field. It is not
the bottleneck; the rasterizer is.

## Microcode upload

`0x00980000` bit 31 gates it. Rising edge: reset the counter, halt the DSP.
While high, writes to `0x00884000` go to program memory. Falling edge: reset
the core and start running. Virtua Cop uploads 2,024 words.
