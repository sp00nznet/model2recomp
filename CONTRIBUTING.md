# Contributing to model2recomp

This library is the Sega Model 2 board as C. Contributions that make it a more
accurate board, or that let it run a title it currently cannot, are all
welcome.

## Where the gaps are

These are real, scoped, and none of them need permission to start.

| Area | What is missing | Difficulty |
|---|---|---|
| **I/O inputs** | The DPRAM command protocol works and the platform layer collects keyboard and mouse state. Nothing copies one into the other, so buttons never reach the guest. | Small — and it has an obvious test |
| **Sound** | `src/sound.c` answers the UART handshake and nothing else. Needs a 68000 core plus MultiPCM (or SCSP for later boards). | Large |
| **Rasterizer fidelity** | Bilinear filtering, mipmap selection, trilinear blending, microtexture. LOD is already computed per polygon and simply unused. | Medium |
| **Board variants** | 2B-CRX (ADSP-21062 SHARC) and 2C-CRX (TGPx4) coprocessors; SCSP sound for 2A onward. | Large |
| **Another title** | The fastest way to find out what is accidentally Virtua Cop-specific in here. | Varies |

## Ground rules

**MAME is the reference.** When behaviour is in question, MAME's Model 2 driver
is right and we are wrong until proven otherwise. Port from it rather than
reimplementing from a datasheet — the datasheets and the silicon disagree in
places, and MAME follows the silicon.

**Credit what you port.** A port is a derivative work. If you bring in code
from MAME or anywhere else, add the file to [NOTICE](NOTICE) with the source it
came from and the copyright that source carries, and say so in the file header.
This is a licence obligation, not a formality.

**No ROMs, ever.** No ROM data, no ROM images, no "small test excerpts", no
microcode dumps. The `ref/` directory is git-ignored local scratch for reading
MAME sources and must stay that way.

**Match the surrounding style.** C17, four-space indent, `snake_case`. Comments
explain *why*, especially where the hardware is surprising — those comments are
the documentation for behaviour you cannot look up anywhere.

**Say what you measured.** "Fixes the flat scene" is not as useful as "the
matrix contained a NaN because the coprocessor FIFO was a loopback; here is the
value before and after." Half the value of a change to this library is the
explanation of what the hardware actually does.

## Before you open a pull request

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
               -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
./build/Release/model2recomp_test_tilemap
./build/Release/model2recomp_test_geometry
```

Both are self-checking and exit non-zero on failure. They need no ROMs, so
they run anywhere — please keep them passing, and add a check when you fix
something they would have caught.

If your change affects a game, say which one and show a before and after.

## Documentation

Docs live in `docs/`. Anything true of the *board* belongs here; anything true
of one *game* belongs in that game's repository. If you are unsure, ask whether
the sentence would still be true for a different Model 2 title.

## Getting help

**[sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu)** — ask there
before spending a weekend on something someone has already hit.
