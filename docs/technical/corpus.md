# The Corpus Sweep

There are about thirty Model 2 titles. That is small enough to stop treating a
port as a bespoke project and start treating the whole library as a corpus: put
every set through the same pipeline, and let a table say where each one is.

```bash
python tools/corpus.py all            # every stage, every set
python tools/corpus.py run vcop       # one stage, one set
```

Output lands in `corpus/` — region images, lifted C, screenshots — which is
gitignored, because all of it is derived from ROMs this project does not
distribute. The only thing committed is the report, [CORPUS.md](../../CORPUS.md).

## Why a corpus at all

One game cannot exercise a board. Everything this library does was verified
against *Virtua Cop* first, and the moment a second title was pointed at it,
eight bugs fell out of shared code — `movl`/`movt`/`movq` ignoring the literal
bit, unaligned word accesses being masked rather than split, a jump table
dispatched through `callx` going unharvested. None of those were Daytona bugs.
They were Model 2 bugs that Virtua Cop happened not to trip.

Thirty titles is thirty chances to trip the rest, and the sweep is what makes
that cheap enough to run.

## The stages

Each is independently re-runnable and takes an optional list of sets.

| Stage | What it does | Where it lands |
|---|---|---|
| `catalog` | Reads every `ROM_START` in MAME's driver: board variant, region layout, which sets are on hand | `corpus/state.json` |
| `extract` | Set ZIP → the flat region images `model2recomp_load_rom` reads | `corpus/<set>/roms/` |
| `lift` | `program.bin` → C, via `tools/i960_lifter.py` | `corpus/<set>/recomp/` |
| `build` | That C → one executable per set, on the shared launcher | `build_corpus/corpus/` |
| `run` | Boots each headless, samples the screen, classifies | `corpus/<set>/shots/` |
| `report` | The table | `CORPUS.md` |

### The ROM layout comes from MAME, not from a hand-written table

Every title lays its ROMs out differently — which chips make up the i960
program, where the data ROM's mirrors go, whether the coprocessor's data socket
is filled. That table already exists and is already right: it is the `ROM_START`
block in `sega/model2.cpp`.

The two existing ports each transcribed their title's block into a
`rom_loader.py` by hand. That does not scale to thirty: it is thirty
transcriptions and thirty chances to put a chip at the wrong offset and get a
region full of plausible-looking garbage. So `tools/mame_romset.py` reads the
driver instead. Point it at a local copy (`ref/model2.cpp`, local scratch, never
redistributed) and it yields every set's layout.

It was checked the only way that means anything: its output for `vcop` and
`daytona` is **byte-identical** to what those two hand-written loaders produce.

### Chip names drift; part numbers do not

MAME has renamed these chips repeatedly, and sets in the wild carry whatever
spelling was current when they were built. `mpr-16537.28` became
`mpr-16537.ic28`. Virtua Striker's `epr-18068a.15` is `ep18068a.15` in a
merged set from an older build. Desert Tank's `mpr-16964` moved socket from
`.21` to `.20`.

Matching only the name the current driver lists yields a region of zeros, and
the game then fails much later for no visible reason. So the loader falls back
to the part number, which is what actually identifies the chip — and reports
every chip it matched loosely, so a substitution is never silent.

Merged sets also frequently hold a *clone* rather than the parent: `doa.zip`
carries `doab`'s chips. The sweep tries a title's whole family and keeps the
layout the ZIP satisfies most completely, rather than the first one that does
not fail outright.

### One launcher for every set

`examples/corpus/main.c` is the whole game project, for all of them. It knows
nothing about any title: the lifted code links in under a fixed `game_` prefix,
the ROM images come from a directory, and the boot sequence is the board's —
initialization boot record, PRCB, reinitialize IAC, and then the guest's own
frame loop, which never returns.

A real port still wants its own `main()` eventually; that is where input
mapping and cabinet wiring live. This one answers a narrower question for all
thirty at once: **how far does this set get with no help at all?**

### How "did it draw" is decided

The run stage boots each set under `SDL_VIDEODRIVER=dummy` with `MODEL2_FAST=1`
and `MODEL2_SHOT_EVERY`, then reads the PPMs it left behind.

- **Drew** — more than a few thousand non-zero subpixels in any sample. Read
  off the pixels, not the file size: a PPM is uncompressed, so its size says
  nothing about what is in it.
- **Advancing** — at least 10% of the sampled picture differs between two
  consecutive samples. A game sitting on one screen with a flashing "INSERT
  COIN" changes a few per cent; one cycling its attract sequence changes most
  of it. Counting the difference rather than hashing the frame is what keeps
  those apart.

The field-0 sample is excluded from that comparison. It is taken before the
guest has drawn anything, and counting it makes every game that ever puts a
pixel up look like it is cycling.

The sweep runs with **no input** by default, because attract mode is what these
games do *before* a credit — a coin is the one thing that ends the state being
measured. `--input coin1,start1` surveys what happens past the title screen
instead.

## What the sweep cannot tell you

It measures how far a set gets on *this* board implementation, which is the
original 1993 Model 2. A 2B-CRX title that draws nothing is not a lifter
failure or a renderer failure; it is a title whose coprocessor
([ADSP-21062 SHARC](porting-targets.md)) does not exist here yet. Read the
board column before reading the result column.

Likewise, several of these sets are `MACHINE_NOT_WORKING` in MAME. A set MAME
cannot run has no reference behaviour to compare a port against; that is worth
knowing before spending a week on one, and the report flags it.

## Sequential on purpose

Recompiling and building thirty lifted programs in parallel is a good way to
run a machine out of memory partway through and then spend the evening working
out which of the failures were real. Every stage runs one set at a time, and
every subprocess has a timeout, so one pathological set cannot cost the other
twenty-nine.

On Windows the build also forces the 64-bit hosted compiler (`-T host=x64`) and
disables MSBuild node reuse. Neither is tuning: the largest lifted translation
units exhaust the 32-bit host's address space outright, and the worker nodes
MSBuild keeps alive for reuse accumulate across thirty-odd invocations until
`cl` cannot load `c2.dll`.
