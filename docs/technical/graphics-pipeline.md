# Graphics Pipeline

From the i960 writing a register to a lit pixel on screen. All of this lives in
`src/geometry.c` and `src/video.c`, ported from MAME's `model2_v.cpp`,
`model2rd.ipp` and `segaic24.cpp`.

## Per field

```
1.  geo_parse()             walk the display list the game pushed into buffer RAM,
                            producing a z-sorted polygon list
2.  geo_render_polygons()   project and rasterize it into a private 496x384 bitmap
3.  video_render_frame()    draw the back tilemaps, composite the 3D bitmap over
                            them, then draw the front tilemaps
4.  VBlank                  the game builds the next field's list
```

All four happen inside `model2recomp_field_sync()`. See
[execution-model.md](execution-model.md).

## The display list, and the two ports that build it

The list lives in buffer RAM (`0x00900000`). The game tells the engine where to
write (`0x00801008`) and where to read (`0x00803008`), and writing the read
address is how it publishes a finished list.

Words reach the list through **two different ports**, and you need both:

| Write to | Appends | Encoding |
|---|---|---|
| `0x00800000`–`0x00800FFF` | an **opcode** | function number from the register address (`(addr >> 4) & 0x3F`) in bits 23–28, value in bits 0–19 |
| `0x00804000` | an **operand** | the value, verbatim |

Implementing only the program port leaves a stream of operands with no opcodes,
which parses as plausible nonsense — plausible enough to produce polygons, which
is what makes it an expensive mistake to debug.

A word with bit 31 set is a **jump** to another offset in buffer RAM rather than
a command.

### Commands

Opcode is bits 23–27. The `0x10`–`0x1F` half largely repeats `0x00`–`0x0F`.

| Op | Name | Operands | What |
|---|---|---|---|
| `0x00` | nop | 0 | |
| `0x01` | object data | 4 + | Texture point address, texture header address, object address, count. Reads model data from polygon ROM or polygon RAM and emits transformed, lit polygons. |
| `0x02` | direct data | varies | Polygons supplied inline instead of from ROM |
| `0x03` | window data | 6 | Viewport, per-eye centres, clip planes |
| `0x04` | texture data | varies | Upload into texture RAM |
| `0x05` | polygon data | varies | Upload into polygon RAM |
| `0x06` | texture parameters | varies | Per-texture ambient, diffuse, specular |
| `0x07` | mode | 1 | Selects the lighting path: normals present or computed, specular on or off |
| `0x08` | z-sort mode | 1 | |
| `0x09` | focus | 2 | X and Y projection scale |
| `0x0A` | light vector | 3 | |
| `0x0B` | **matrix write** | 12 | 3×3 rotation then 3 translation. The values come from the coprocessor. |
| `0x0C` | translate write | 3 | Just the translation, `matrix[9..11]` |
| `0x0D` | DSP RAM push | 2 | Not supported |
| `0x0E` | test | varies | |
| `0x0F` | end | 0 | |
| `0x14` | log data | varies | |
| `0x16` | LOD | 1 | |
| `0x1D`/`0x1E` | code upload / jump | 2 / 1 | Geometry microcode — not supported, and not needed, because the engine is modelled rather than emulated |

**Command `0x0B` is where a stubbed coprocessor shows up.** The game does not
compute those twelve floats; it asks the MB86233 for them. See
[tgp-coprocessor.md](tgp-coprocessor.md).

## Geometry

For each polygon in an object:

1. **Transform** each vertex by the current matrix.
2. **Transform the normal** — or compute one from the face, if the current mode
   says normals are absent.
3. **Light**: `dotl = normal · light`, `dotp = normal · point`. Luminance is
   `|dotl|`, zeroed when `dotl * dotp < 0`, then scaled by the texture's
   diffuse term and offset by its ambient. Specular, when the mode selects it,
   adds `(2·dotl·normal.z − light.z)` raised to a power the texture's specular
   control chooses.
4. **Face bit**: `dotp >= 0` is front-facing. It rides in bit 8 of the luma.
5. **Focus**: multiply x and y by the projection scale.
6. **Clip** against the four window planes.
7. **Z-sort**: the polygon's z is converted from IEEE float to the hardware's
   4.12 format and used as an index into 65536 buckets, each a linked list.

Then the whole list is walked back to front, bucket by bucket.

## Rasterizing

`geo_render_polygons()` projects each polygon and fills it. Projection is
destructive — it rewrites `v[i].x` and `v[i].y` in place, and for textured
polygons replaces `pz` with `1/z` and scales `pu`/`pv` by it — so a polygon list
can only be rendered **once**. The `render_done` flag exists for exactly this:
a field where the game did not submit a new list keeps the bitmap it already
has instead of re-projecting an already-projected list.

Texture coordinates interpolate as `u/z`, `v/z` and `1/z`, which are affine in
screen space; that is what makes the mapping perspective correct.

### The colour path

Nothing on this hardware picks an RGB value directly:

```
  polygon's texheader[3]  ->  colorbase (10 bits)
                              |
                              v
                     palram[0x1000 + colorbase]   a 15-bit value: 5 bits per channel
                              |
              +---------------+---------------+
              v               v               v
       colorxlat R      colorxlat G      colorxlat B      each 5 bits selects one of
       ramp[0..31]      ramp[0..31]      ramp[0..31]      32 ramps of 256 entries
              |               |               |
              +-------- indexed by luma ------+           6 bits, 0..63
                              |
                              v
                        gamma table
                              |
                              v
                           RGB pixel
```

Where the luma comes from depends on the polygon:

- **Untextured**: the polygon's own luma, `>> 2`.
- **Textured**: the 4-bit texel indexes **luma RAM** at the texture header's
  luma base, and that byte is scaled by the polygon's luma:
  `luma = lumaram[lumabase + (texel << 3)] * poly_luma / 256`, clamped to `0x3F`.

So a polygon that draws black usually means its palette entry is zero, not that
the rasterizer is wrong.

### Texture sheets

Texture RAM is addressed as 2048×1024 but stored as 1024×2048: the right half
wraps into the lower half with the y bit flipped. Two 4-bit texels per byte,
four per 16-bit unit, read as 32-bit words. `get_texel()` in `src/geometry.c`
does the unpacking.

### Wrapping is not optional

A texture coordinate **always** wraps modulo the texture size:
`model2rd.ipp` masks with `tex_width - 1` unconditionally. The header's
"smooth wrap" bits (`texheader[0]` bits 6 and 7) do not decide whether the
texture repeats - they only pick how the bilinear filter behaves at the seam.
Clamping to the edge texel instead smears it across everything past the
texture, which turns a tiled wall or a ground plane into one stretched streak.

Mirroring (bits 8 and 9) reflects with period `2 * size` and disables smooth
wrapping on that axis.

### Translucency

There is no alpha blend. Three separate mechanisms, from `texheader[0]`:

| Bits | Meaning |
|---|---|
| 14 | textured |
| 13 | translucent |
| 15 | checkerboard |

- **Translucent and textured**: texel `0xF` is the transparent index and the
  pixel is discarded. This is how every sprite-like polygon gets its shape -
  clouds, muzzle flashes, the HUD's revolver cylinder. Draw them without it
  and each one is an opaque rectangle of its texture's background colour.
- **Translucent and untextured**: nothing is drawn at all.
- **Checkerboard**: a 50% stipple on the pixel grid - `(x ^ y) & 1` selects
  which pixels survive. The hardware's only "half transparent".

### Filtering

Bilinear, with mipmaps and trilinear blending between levels, following
`model2rd.ipp`.

The level comes from the depth, not from anything the game says directly:

```
mml   = -polygon's texlod + log2(z)      log2 to 7 fractional bits
level = clamp(mml >> 7, 0, max_level)    max_level bottoms out at 2x2
```

The fractional part of `mml` blends into the next level down. Mip levels
**alternate between the two texture sheets** - level 0 from the sheet
`texheader[2]` bit 12 selects, level 1 from the other, and so on - and each
level's origin is `((texx - 2048) >> level) & 2047`, which is the unchanged
origin at level 0.

Below level 0 a polygon with the microtexture bit blends in a fixed 128x128
detail sheet instead, up to almost half, with the strength coming from how far
past level 0 the pixel is.

`log2` is computed from the float's exponent plus a 128-entry table on the top
mantissa bits - MAME takes this from `voodoo_render.cpp`. It runs once per
pixel, which is too often for `logf`.

Filtering is not cosmetic here. Without mipmaps a minified texture aliases into
noise, and the ground planes and distant walls in Virtua Cop's wharf are almost
entirely minified.

## Tilemaps

Four System 24 layers, 8×8 tiles, 4 bits per pixel, from character RAM at
`0x01080000` with layer maps in tile RAM at `0x01000000`. They draw in two
passes — back layers, then the 3D bitmap composited over them, then front
layers — so the HUD sits over the scene and the sky sits behind it. Which pass
a *tile* belongs to is bit 15 of its name-table word, not a property of the
layer, so one layer contributes to both.

In the back pass, tilemaps 3 and 2 draw opaque (pen 0 included) and 1 and 0
transparent, which is what clears the screen.

**Not implemented: the window/split modes.** The four tilemaps are two pairs, a
"screen" half and a "window" half, and when the control word at the even
half's vertical-scroll register has bits 13-14 set, the window half is drawn
clipped inside the screen half rather than on its own. Here the window half is
simply skipped in that case - drawing it unclipped paints the whole screen.
Per-line scroll (horizontal-scroll bit 15) is missing for the same reason. Both
are in segaic24.cpp draw_common.

The 3D bitmap is composited by skipping zero pixels: zero means the rasterizer
never touched that pixel, so the tilemap below shows through.

The CRTC origin moves with the horizontal and vertical sync registers
(`0x01040000`, `0x01060000`), and both the rasterizer's viewport clipping and
the tilemap draw honour it.
