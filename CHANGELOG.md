# Changelog

Follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Nothing is released yet. Virtua Cop boots, plays and draws against this
runtime, but the 3D output is visibly wrong, so there is no version worth
tagging. Everything below is unreleased.

## [Unreleased]

### Added

- The Model 2 memory map, interrupt controller, and the execution model the
  guest actually relies on: IAC reinitialisation, I/O acknowledgement, and the
  field-sync read the game busy-waits on. The frame boundary lives inside that
  wait loop, which is where presentation, input and interrupt dispatch happen.
- The System 24 tilemap layers, with the pair mask at `tile_ram` 0x6000/0x6800,
  window and split modes, and the real palette path — palram through 32
  `colorxlat` ramps per channel, indexed by luma, then gamma.
- The geometry engine and the Lockheed-Martin rasterizer, ported from MAME.
- The MB86233 "TGP" math coprocessor. Before it existed, the scene was flat,
  and that was misattributed to the recompiled floating point for two commits.
- Texture sampling: wrapping, bilinear filtering, mipmaps and the microtexture.
- Translucency in all three of the mechanisms the hardware has.
- Diagnostics: `MODEL2_SHADE` (how one polygon resolved its colour),
  `MODEL2_PROBE` (what drew one pixel), `MODEL2_MATRIX` (check the
  coprocessor's output), `MODEL2_RAMDUMP`, watchpoint call paths, culling
  counters, and a display-list command histogram.
- Host input plumbed through to the guest's DPRAM.

### Fixed

- The window tilemap is the other half of a pair, not a layer of its own;
  drawing it as a layer overdrew the screen.
- Tile pens are indices, not colours.
- Back-pass tilemap opacity now matches MAME, and unclipped window halves are
  skipped rather than drawn twice.
- The whole window rendered red: the texture format carries alpha in the red
  channel, and the SDL surface was `RGBX8888` where the data is `ABGR8888`.
- A quad fills its rectangle; it was filling half of it.
- The texture origin was off, and unpublished display lists were being parsed.
- Stale display lists were re-rendered after the guest had moved on.
- A guest frame leak that presented as an interrupt-frame problem.
- Every interrupt line is serviced, including the sound interrupt.

### Known issues

Geometry renders but is wrong — walls as triangles, flat surfaces as slabs,
gaps in bodies. Virtua Cop's display list matches MAME's to 32,765 of 32,768
dwords, so the commands reaching this runtime are very nearly right and the
fault is downstream of the guest.
