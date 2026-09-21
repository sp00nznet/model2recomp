# Model 2 corpus

Every Model 2 set MAME knows, run through the same pipeline: read the
ROM layout out of the driver, build the region images, lift the i960
program to C, build it against this library with the generic launcher,
and boot it headless. Regenerate the whole table with
`python tools/corpus.py all`.

No ROM data, and no code lifted from one, is committed here. The table
records what happened on a machine that had the sets.

| | |
|---|---|
| Sets in the sweep | **36** |
| ROM set on hand | **35** |
| Region images built | **35** |
| Program lifts to C | **35** |
| That C builds | **35** |
| Boots | **35** |
| Draws something | **7** |
| Reaches attract (still changing at the last sample) | **3** |
| Blocked on a Sega crypto device regardless of the board | **5** |

Read the board heading before the result. The memory map is now variant-aware, so the CRX boards get their own program RAM, I/O chip and texture windows - but 2B and 2C still have no math coprocessor, and without one no polygon can be transformed. A CRX title that draws only tilemaps and text is at the ceiling of what this library can currently give it, not failing. See [porting-targets.md](docs/technical/porting-targets.md).

**Furthest** is how far the set got:

- `ATTRACT` — drew a picture, and at least a tenth of it changed between samples - the attract sequence is running
- `STATIC` — drew a picture and stayed on it
- `FLAT-FILL` — lit every pixel but only one colour: a framebuffer cleared to a background with nothing drawn on it
- `BOOTS-BLANK` — reached the field boundary, drew nothing
- `NO-FRAMES` — never reached the first sample field before the timeout
- `NO-BOOT` — the reset vector did not dispatch

**Hints** counts the entry points `corpus.py discover` harvested from the runtime's own dispatch misses: addresses the game computes at run time, which static analysis cannot see.

## Model 2 (3)

The original 1993 board — the one this library implements. These results are the library's own.

| Set | Game | Year | ROM | Extract | Lifts | Builds | Boots | Renders | Motion | Furthest | Funcs | Hints | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `daytona` | Daytona USA (Revision A) | 1994 | yes | ok | ok | ok | yes | yes | frozen | STATIC | 1657 | 0 |  |
| `desert` | Desert Tank | 1994 | yes | ok | ok | ok | yes | yes | advancing | ATTRACT | 1387 | 0 | MAME cannot run this set either. |
| `vcop` | Virtua Cop (Revision B) | 1994 | yes | ok | ok | ok | yes | yes | advancing | ATTRACT | 2300 | 0 |  |

## 2A-CRX (9)

Same MB86233 coprocessor, same geometry engine and rasterizer; a different I/O chip (Sega 315-5649 in place of the Model 1 dual-port RAM) and a different program-RAM map. A memory-map job rather than a DSP one, and the largest single win available: nine titles.

| Set | Game | Year | ROM | Extract | Lifts | Builds | Boots | Renders | Motion | Furthest | Funcs | Hints | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `vf2` | Virtua Fighter 2 (Version 2.1) | 1994 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 6227 | 1 |  |
| `manxtt` | Manx TT Superbike - DX/Twin (Revision D) | 1995 | yes | ok | ok | ok | yes | yes | frozen | STATIC | 2349 | 2 | MAME cannot run this set either. |
| `skytargt` | Sky Target | 1995 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 3597 | 0 | MAME cannot run this set either. |
| `srallyc` | Sega Rally Championship - Twin/DX (Revision C) | 1995 | yes | ok | ok | ok | yes | no | blank | NO-FRAMES | 14262 | 1 | MAME cannot run this set either. |
| `vcop2` | Virtua Cop 2 | 1995 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 11518 | 7 |  |
| `airwlkrs` | Air Walkers | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 7558 | 1 | MAME cannot run this set either. |
| `motoraid` | Motor Raid - Twin | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 4327 | 3 |  |
| `dynamcop` | Dynamite Cop (Export, Model 2A) | 1998 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 17877 | 0 | MAME cannot run this set either. Behind a Sega 315-5881/317-0229 cryptographic device; the data it streams is not decryp |
| `hpyagu98` | Hanguk Pro Yagu 98 | 1998 | no | no-rom | no-rom | no-code | - | - | - | - | 0 | 0 | MAME cannot run this set either. |

## 2B-CRX (15)

Needs an ADSP-21062 SHARC for the math coprocessor. The geometry engine and rasterizer are shared, which is why some of these still put their tilemap screens up.

| Set | Game | Year | ROM | Extract | Lifts | Builds | Boots | Renders | Motion | Furthest | Funcs | Hints | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `rchase2` | Rail Chase 2 (Revision A) | 1994 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 4901 | 3 |  |
| `vstriker` | Virtua Striker (Revision A) | 1994 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 2525 | 16 |  |
| `fvipers` | Fighting Vipers (Revision D) | 1995 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 3593 | 3 | MAME cannot run this set either. |
| `gunblade` | Gunblade NY (Revision A) | 1995 | yes | ok | ok | ok | yes | yes | advancing | ATTRACT | 5813 | 5 |  |
| `indy500` | INDY 500 Twin (Revision A, Newer) | 1995 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 12176 | 2 | MAME cannot run this set either. |
| `von` | Cyber Troopers Virtual-On - Twin (Export) | 1995 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 11925 | 2 | MAME cannot run this set either. |
| `doa` | Dead or Alive (Model 2B, Revision C) | 1996 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 8054 | 4 | Behind a Sega 315-5881/317-0229 cryptographic device; the data it streams is not decrypted here. |
| `dynabb` | Dynamite Baseball | 1996 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 17676 | 0 | MAME cannot run this set either. |
| `lastbrnx` | Last Bronx (Export, Revision A) | 1996 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 11772 | 2 | MAME cannot run this set either. |
| `powsled` | Power Sled (Slave, Revision A) | 1996 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 21128 | 4 | MAME cannot run this set either. |
| `schamp` | Sonic Championship (USA) | 1996 | yes | ok | ok | ok | yes | yes | twitching | STATIC | 8162 | 12 | MAME cannot run this set either. |
| `sgt24h` | Super GT 24h | 1996 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 14371 | 1 | MAME cannot run this set either. Behind a Sega 315-5881/317-0229 cryptographic device; the data it streams is not decryp |
| `dynabb97` | Dynamite Baseball 97 (Revision A) | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 5203 | 0 | MAME cannot run this set either. |
| `zerogun` | Zero Gunner (Export, Model 2B) | 1997 | yes | ok | ok | ok | yes | no | blank | NO-FRAMES | 6992 | 11 | Behind a Sega 315-5881/317-0229 cryptographic device; the data it streams is not decrypted here. |
| `pltkids` | Pilot Kids (Model 2B, Revision A) | 1998 | yes | ok | ok | ok | yes | no | blank | NO-FRAMES | 16150 | 1 | Behind a Sega 315-5881/317-0229 cryptographic device; the data it streams is not decrypted here. |

## 2C-CRX (9)

Needs an MB86235 "TGPx4". Same story as 2B: shared rasterizer, absent coprocessor.

| Set | Game | Year | ROM | Extract | Lifts | Builds | Boots | Renders | Motion | Furthest | Funcs | Hints | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `skisuprg` | Sega Ski Super G | 1996 | yes | ok | ok | ok | yes | yes | twitching | STATIC | 8573 | 2 | MAME cannot run this set either. |
| `stcc` | Sega Touring Car Championship (newer) | 1996 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 5695 | 0 | MAME cannot run this set either. |
| `waverunr` | Wave Runner (Japan, Revision A) | 1996 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 2802 | 2 | MAME cannot run this set either. |
| `bel` | Behind Enemy Lines | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 2268 | 1 | MAME cannot run this set either. |
| `hotd` | The House of the Dead (Revision A) | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 8259 | 0 | MAME cannot run this set either. |
| `overrev` | Over Rev (Model 2C, Revision A) | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 9350 | 1 | MAME cannot run this set either. |
| `rascot2` | Royal Ascot II | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 4502 | 0 | MAME cannot run this set either. |
| `segawski` | Sega Water Ski (Japan, Revision A) | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 4213 | 0 | MAME cannot run this set either. |
| `topskatr` | Top Skater (Export, Revision A) | 1997 | yes | ok | ok | ok | yes | no | blank | BOOTS-BLANK | 11445 | 9 | MAME cannot run this set either. |
