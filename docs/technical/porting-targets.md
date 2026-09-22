# Porting Targets

Which Model 2 games this library can realistically run today, what each one
needs, and what the first port of a *second* title would teach us.

The short version: **Daytona USA is the obvious next target** — it is the same
board as the reference title, its program ROM is half the size, and it exercises
exactly one thing Virtua Cop never touched.

## The four boards, by how far they are from working

| Board | Coprocessor | Geometry + rasterizer | Distance from here |
|---|---|---|---|
| **Model 2** (1993) | MB86233 TGP | same | **Working.** This is what the library implements. |
| **2A-CRX** (1994) | MB86233 TGP — *identical* | same | **Close.** Different I/O chip and program-RAM map; sound differs but is stubbed either way. |
| **2B-CRX** (1994) | ADSP-21062 SHARC | same | **Far.** A different DSP core entirely. |
| **2C-CRX** (1996) | MB86235 "TGPx4" | same | **Far.** Another different DSP — 64-bit instruction words. |

The thing worth knowing, and the thing this library's own docs got wrong until
someone checked: **2A-CRX is not a new coprocessor.** In MAME both the original
board and 2A derive from `model2_tgp_state` and run the same MB86233. The
geometry engine and rasterizer are common to all four boards. So 2B and 2C are
DSP-emulator projects, and 2A is a memory-map project.

### What 2A-CRX actually needs

Diffing `model2o_mem` against `model2a_crx_mem` in MAME's `model2.cpp`:

| | Original | 2A-CRX |
|---|---|---|
| `0x00200000`–`0x0023FFFF` | 128 KB RAM, then the program ROM extension mirrored at `0x00220000` | 256 KB RAM, no ROM mirror |
| `0x01C00000` | Model 1 I/O Board 2 dual-port RAM, 4 KB | **Sega 315-5649 I/O chip**, 32 bytes |
| `0x01C80000` | i8251 UART | a serial register plus the UART status/control split across two halves |
| Sound | 68000 + MultiPCM | 68000 + SCSP |

Sound is stubbed on both, so it blocks nothing. The real work is the 315-5649,
and it is a register interface rather than a dual-port RAM protocol — arguably
simpler than what is already implemented.

## The games

What is sitting in the arcade directory, and what each would take.

### Daytona USA — `daytona` — **Model 2 original**

The obvious next port, and the one worth doing first.

| | |
|---|---|
| Board | `model2o_state` — same as Virtua Cop |
| Program ROM | **256 KB** (2 × 128 KB) — half of Virtua Cop's 512 KB |
| Sound board | Model 1 sound board (68000 + MultiPCM), stubbed either way |
| Blocker | **One.** See below. |

**The one blocker: the coprocessor data ROM.** Virtua Cop's `copro_data`
region is declared and empty. Daytona's holds **4 MB** (`mpr-16536`,
`mpr-16537`) — collision meshes, height maps and similar, which the MB86233
reads through its banked external window at `adr & 0x800000`. `tgp_memory_r()`
in `src/copro.c` currently returns 0 for that case, because on the reference
title there is nothing there:

```c
static uint32_t tgp_memory_r(uint32_t offset)
{
    uint32_t adr = (s_bank_reg & 0xFF0000) | offset;
    if (adr & 0x800000) return 0;                 /* <- copro data ROM, unimplemented */
    if (adr & 0x400000) return bus_bufferram_read32((adr & 0x7FFF) * 4);
    return 0;
}
```

Wiring it up is a ROM region, a load hook and two lines here — MAME masks the
address to the region size in dwords and indexes it. What makes it worth doing
on a real target rather than speculatively is that nothing currently in the
project can test it.

Two smaller things a Daytona loader has to get right:

- **`ROM_COPY` mirroring.** `main_data` `0x800000`–`0x8FFFFF` is copied to
  `0x900000`, `0xA00000` … `0xF00000`. The game reads those mirrors.
- **A gap in the texture region.** Textures load at `0x000000` and `0x800000`
  with nothing between. Region-sized images make this free; data-sized ones
  break it.

**What it would teach us.** Whether anything in model2recomp is accidentally
Virtua Cop-shaped. Daytona is a driving game — different geometry load,
different display-list usage, genuinely different lighting — on identical
silicon. It also has a link-play communication board the library does not
model, which is a good test of whether an unmodelled subsystem degrades or
deadlocks.

### Sega Rally Championship — `srallyc` — **Model 2A-CRX**

The natural second target, and the one that would land 2A support.

MAME marks every `srallyc` set `MACHINE_NOT_WORKING`, which is worth knowing
before starting: if MAME cannot run it, the reference behaviour to compare
against does not exist. Take that as a reason to do Daytona first, not as a
reason to avoid it — "MAME cannot do this either" is a respectable place for a
recomp to end up.

### Dead or Alive — `doa` — **Model 2B-CRX** (2A clones exist)

Needs the SHARC. The merged set also carries the `doaa` / `doaab` **Model 2A**
clones, which would otherwise make it a 2A candidate — except that all DOA sets
run through a **Sega 315-5881 cryptographic device** (key 317-0229) that
decrypts data the game streams through it. That is a second, independent
obstacle on top of the board.

### Cyber Troopers Virtual-On — `von` — **Model 2B-CRX**

SHARC. `MACHINE_NOT_WORKING` in MAME.

### Over Rev — `overrev` — **Model 2B-CRX**

SHARC.

### The House of the Dead — `hotd` — **Model 2C-CRX**

MB86235 TGPx4. `MACHINE_NOT_WORKING` in MAME. A lightgun game like Virtua Cop,
so the I/O work would transfer — but the coprocessor is a from-scratch DSP
core.

### Wave Runner — `waverunr` — **Model 2C-CRX**

MB86235. `MACHINE_NOT_WORKING` in MAME.

## What the sweep actually found

`python tools/corpus.py all` runs every set through the whole pipeline and
writes [CORPUS.md](../../CORPUS.md). The numbers below are from that run, not
from reading MAME's driver, and they moved two things in this page from
"expected" to "measured".

**Desert Tank** is the third original-board title and nobody had tried it. It
boots on the generic launcher with no title-specific code and renders its
attract sequence - and MAME marks it `MACHINE_NOT_WORKING`.

**The CRX boards are not a wall.** The geometry engine and rasterizer are
common to all four boards, and it shows: *Gunblade NY* (2B) draws its warning
screen and title logo, *Manx TT* (2A) draws its motion-slider safety screen,
*Sonic Championship* (2B) gets a picture up. What is missing on those boards is
the math coprocessor, so what they cannot do is 3D - the tilemap and text paths
work today.

**Most sets stop for a reason the sweep can name.** Every one of the 35 sets on
hand lifts, compiles and boots. Of those that then draw nothing, the common
pattern is the runtime's own `no function at 0x...` - an entry point the game
computes and static analysis cannot see. `corpus.py discover` harvests those and
feeds them back, which is what moved Manx TT and Sonic Championship off blank.

It is not free: registering an address as an entry point splits the function
containing it, and a harvested address is not always a function start. Nine
hints took Desert Tank from a running attract sequence to a static screen, and
eleven took Virtua Cop from 2,654 colours on screen to 1,044. Discovery now
measures each round and rolls back one that costs more than it buys.

## The CRX boards, after the sweep

`corpus.py` put every set through the pipeline, and the results moved the CRX
work from "a DSP project" to a short list of concrete faults. The library now
takes the board variant (`model2recomp_init`'s third argument actually changes
the map, where before it only labelled a log line):

| | Original | CRX (2A / 2B / 2C) |
|---|---|---|
| `0x00200000` | 128 KB RAM, program ROM mirrored at `0x00220000` | **256 KB RAM**, no mirror |
| `0x01C00000` | Model 1 I/O Board 2, 4 KB of dual-port RAM | **Sega 315-5649**, a 32-byte register file |
| Texture RAM | `0x12000000` | **`0x11000000`**, two 1 MB banks |
| Luma RAM | `0x12800000`, one byte per dword | **`0x11400000`**, one byte per word |
| Serial | `0x01C80000` | `0x01C80000` (2A/2C), **`0x009C0000`** (2B) |

The 315-5649 is the same chip on all three CRX variants, which is why it was
worth doing once: it is not a 2A part.

### The one that was not a CRX bug at all

Sky Target spins on bit 0 of `0x01C80002` — the i8251 status register — and
never leaves. The UART sits on byte lanes 0 and 2 of the dword, exactly like
the dual-port RAM next to it, and `bus_read32` was returning only lane 0. A
16-bit read of the status register, which is how the games actually poll it,
therefore always came back zero.

That is a fault on the **original** board too. Virtua Cop and Daytona simply
never read it that way, so one game could not have found it and two did not.
It is the clearest argument for the sweep there is.

### What is left, after the board work

Two things the sweep can now say that it could not before.

**The CRX memory map is complete for the titles that get that far.**
`MODEL2_UNMAPPED=1` reports every address the guest touches that the map does
not cover. Virtua Cop, Sky Target and Motor Raid touch none. So a 2A title that
still draws nothing is not waiting on a register this library lacks - the
remaining faults are in the coprocessor, in game logic, or in the lift.

**Six sets are behind a Sega cryptographic device** - the 315-5881, or the
317-0229 on Dead or Alive - and stream their data through it. `doa`,
`dynamcop`, `pltkids`, `sgt24h`, `zerogun` and `powsled` cannot draw whatever
else is fixed, because what they read back is not yet decrypted. That is a
blocker in its own right and the corpus table now names it rather than lumping
those titles in with "the coprocessor is missing".

### The call-depth cap was strangling twenty of the thirty-five sets

`func_table_call` refused to dispatch past 500 nested calls. That is a recursion
guard, not a model of anything - the i960's own depth is bounded by its stack -
and 500 turned out to be far too low, for a reason that has nothing to do with
how deep these games nest.

The lifter sometimes splits a routine into **one-instruction fragments that
tail-call each other**. Virtua Cop's colour-ramp generator is a single function
with `goto` loops; Virtua Cop 2's equivalent is a chain of functions four bytes
apart, each doing one instruction and calling the next. A loop in that shape is
*recursion*, so a fill of a few thousand iterations exhausts the counter and is
abandoned part-way through.

Twenty of the thirty-five sets were hitting it. Manx TT hit it 3,570 times in a
single run; Top Skater 180, Rail Chase 2 168, Sega Ski Super G 39, Over Rev 33.

Virtua Cop 2 was the clearest case. Its ramp fill stopped after 228 of 24,576
entries - two runs ending at `0x3F` and `0x13C`, where the hardware reads `0x40`
and `0x140`, one and four entries short - and stayed at exactly 228 whether the
run was 900 fields, 3,000 or 12,000. With the cap raised it finishes the fill
and **reaches attract mode**: the warning screen, then the lock-on demo in
colour.

The cap is now 20,000 by default and `MODEL2_CALLDEPTH` overrides it. It is
still host stack and not free - 200,000 overflows a default 1 MB thread stack
and kills the process before the first field - so the corpus launcher links with
a 256 MB stack *reserve*. Raising the cap without that just moves the failure
from a diagnostic to a crash.

The real fix is upstream: stop the lifter splitting functions into fragments, so
a loop stays a loop. That is a bigger and riskier change, and this project's own
notes record three previous attempts at function splitting that each regressed
rendering badly. The cap is the cheap half.

### The SHARC is not what is stopping most 2B titles

This page - and this project's own summary of it - said the 2B sets need an
ADSP-21062 before they can do anything. The sweep disproves it.

**Gunblade NY reaches attract mode on 2B with no coprocessor at all.** Its
framebuffer holds 440,095 lit pixels and the 3D composite contributed exactly
zero of them: the whole attract sequence is tilemaps and text, and that path is
shared across all four boards. So the SHARC blocks 3D, not attract.

Which means a blank 2B set is stopped by something else, and dumping three of
them says what:

| | Name table | Char RAM | Colour ramps (of 32/channel) | Lit pixels |
|---|---|---|---|---|
| Gunblade NY | 2.6% | 55% | **31** | 440,095 |
| Virtua Striker | **18.8%** | 72% | **31** | **0** |
| Virtual-On | 0.5% | 62% | **2** | 0 |

Virtual-On is the Virtua Cop 2 fault again - the ramps are unwritten, so
whatever it draws is black.

Virtua Striker turned out not to be a renderer fault at all, and it is worth
recording how that was established because the method generalises.

Its name table holds 12,288 entries. 12,269 of them are tile `0x20` at palette
0 - a cleared screen. The other 19 spell, at row 25:

    SOUND initialize...

The game is waiting for the sound board. `src/sound.c` is a UART handshake and
nothing else: no 68000, no SCSP. Virtua Striker prints that line, polls a work
RAM flag that only the sound interrupt would set, and stays there. Nothing about
the renderer is wrong, and the "more tilemap content than Gunblade" figure that
first pointed here was 12,269 copies of a blank tile.

What it wants is a *reply*, not readiness. The serial registers it touches are
`0x009C0000` (data, written once) and `0x009C0004` (control, written six
times); it never reads either back, so it is waiting on the sound interrupt to
carry a response. Re-asserting that interrupt every field - which is what a
permanently-ready transmitter looks like, and what MAME's `sound_ready_w`
condition amounts to with nothing on the other end - was tried and changes
nothing: the game is past readiness and waiting for the board to answer. That
needs the 68000, which makes Virtua Striker a sound-board target rather than a
renderer or coprocessor one.

`tools/screen_text.py` reads that text out of a `MODEL2_RAMDUMP` tilemap dump.
Most of these boards boot through a self-test that says what it is doing, and it
says so in the name table long before the colour path works - so a set that
looks blank on screen may still be telling you exactly what it wants.


### The CRX boards have a serial EEPROM, and it was answering nonsense

Every CRX board hangs a 93C46 (64x16) serial EEPROM off the 315-5649's port A
and reads it back on port B, and every CRX game reads its settings out of it
before it will do anything else. `MODEL2_HOTREADS` found it: Over Rev reads
`0x01C00000` 12,159 times in 900 fields, and writes it 9,788 times in 300 - it
is bit-banging a serial protocol and getting a data-out line stuck high, which
is not a value any command can produce.

The wiring, from MAME's `model2a_state::model2a`:

| Port | Direction | What |
|---|---|---|
| A | out | EEPROM: bit 0 ctrlmode, bit 5 DI, bit 6 CS, bit 7 CLK |
| B | in | `in0_r` - coins/start/test, or the EEPROM's DO in ctrlmode |
| C, D | in | cabinet inputs |
| E | out | billboard |
| F | out | lamps and coin counters |
| G | in | DIP switches |

Two things were wrong. There was no EEPROM at all, and `lamp_output_write` was
being called for port **A** - the EEPROM's control lines - rather than port F.
Both are fixed, and the device now answers a real read.

**It does not move any title yet, and the reason is worth knowing.** The
contents come up blank (`0xFFFF`), and these games checksum what they read.
MAME's own driver says so for Hanguk Pro Yagu 98: "requires certain values to be
set in the EEPROM and backup RAM, otherwise it fails with Error #1", and ships
"partly handcrafted EEPROM and backup RAM files ... to allow the game to boot".

So the next step for this family is *contents*, not code: a valid settings image
per title, or enough of the game's own setup path to let it write one. That is a
different kind of work from everything else on this page, and it is now the
named blocker rather than a guess.

### Fourteen sets overflow the dispatch table, and the obvious fix is worse

`func_table` is 8,192 slots, chosen against Virtua Cop's 2,300 functions.
Fourteen of the thirty-five sets have more: Power Sled 21,131, Dynamite Cop
17,878, Dynamite Baseball 17,678, Pilot Kids 16,150, Super GT 24h 14,371, Sega
Rally 14,262, Indy 500 12,176, Virtual-On 11,925, Last Bronx 11,772, Virtua Cop
2 11,518, Top Skater 11,446, Over Rev 9,350, Sega Ski Super G 8,573, House of
the Dead 8,259.

Those games register the first 8,192 and every dispatch to the rest misses. The
runtime said so all along - "Table full!" - but once per function, thousands of
times, so the line that mattered scrolled away behind itself. It says it once
now.

**Raising the table does not fix it.** Tried at 65,536: Power Sled starts
drawing, and Virtua Cop 2 **segfaults**. With a complete table the dispatches
that used to miss now resolve, and some of them resolve to functions the lifter
produced from stretches of data rather than code. Truncating the table was
accidentally shielding the game from its own bad entries, and Virtua Cop 2's
attract mode was partly standing on that.

So this is the second half of a fix. The first half is the lifter not emitting
those functions - the same over-discovery that produces one-instruction
fragments, measurable with `I960_DISCOVERY_STATS=1`. Do that first and this
number can go up; do it in the other order and you trade one title's attract
mode for another's.

## Suggested order

1. **Daytona USA.** Same board, smaller program, one well-understood gap. It
   is the cheapest possible answer to "is this library actually
   title-agnostic?"
2. **The coprocessor data ROM**, driven by (1) rather than written blind.
3. **2A-CRX support** — the 315-5649 I/O chip and the program-RAM map — then
   Sega Rally.
4. **A SHARC or MB86235 core**, if someone wants 2B or 2C. Both are large,
   self-contained, and testable in isolation, which makes them good projects
   for someone who wants a well-defined thing to build.

## A note on ROM set vintage

Merged sets from older MAME builds use older filenames — `mpr-16537.28` where
current MAME's `ROM_START` says `mpr-16537.ic28`, `e19310aa.12` where it now
says `epr-19310a.12`. Same chips, same checksums, different strings. A loader
matches on the name in *your* ZIP, so read the ZIP rather than transcribing
MAME's current table.
