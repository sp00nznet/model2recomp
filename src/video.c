/*
 * Model 2 video subsystem: video RAM, the geometry engine's host-side ports,
 * System 24 tilemap layers, and frame composition.
 *
 * The geometry engine and rasterizer themselves live in geometry.c; this file
 * owns the memory they read and the registers the i960 reaches them through.
 *
 * Ported from MAME's model2.cpp, model2_v.cpp and segaic24.cpp (BSD-3-Clause).
 * See ../NOTICE.
 */

#include "model2recomp/video.h"
#include "model2recomp/bus.h"
#include "model2recomp/copro.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* Video state */
static uint8_t *s_framebuffer = NULL;   /* 496x384 RGBX8888 output */
static uint16_t *s_fbvramA = NULL;      /* 512x400 x 16bpp bank A */
static uint16_t *s_fbvramB = NULL;      /* 512x400 x 16bpp bank B */
static uint32_t *s_textureram0 = NULL;  /* 2MB texture RAM 0 */
static uint32_t *s_textureram1 = NULL;  /* 2MB texture RAM 1 */
static uint8_t  *s_lumaram = NULL;      /* 32KB luma RAM (one byte per dword slot) */
static uint16_t *s_palram = NULL;       /* 16KB palette RAM */
static uint16_t *s_colorxlat = NULL;    /* 48KB color translate RAM */
static uint8_t  *s_tile_ram = NULL;     /* 64KB System 24 tile RAM */
static uint8_t  *s_char_ram = NULL;     /* 512KB System 24 char RAM */

static uint32_t s_render_mode = 0;
static uint32_t s_videoctl = 0;
static uint32_t s_zclip = 0;

#define FB_WIDTH  496
#define FB_HEIGHT 384

void video_init(void)
{
    s_framebuffer  = (uint8_t *)calloc(1, FB_WIDTH * FB_HEIGHT * 4);
    s_fbvramA      = (uint16_t *)calloc(1, 512 * 400 * 2);
    s_fbvramB      = (uint16_t *)calloc(1, 512 * 400 * 2);
    s_textureram0  = (uint32_t *)calloc(1, 0x200000);  /* 2MB */
    s_textureram1  = (uint32_t *)calloc(1, 0x200000);  /* 2MB */
    s_lumaram      = (uint8_t *)calloc(1, 0x8000);     /* 32KB */
    s_palram       = (uint16_t *)calloc(1, 0x4000);    /* 16KB */
    s_colorxlat    = (uint16_t *)calloc(1, 0xC000);    /* 48KB */
    s_tile_ram     = (uint8_t *)calloc(1, 0x10000);    /* 64KB */
    s_char_ram     = (uint8_t *)calloc(1, 0x80000);    /* 512KB */

    printf("[video] Initialized (%dx%d)\n", FB_WIDTH, FB_HEIGHT);
}

void video_shutdown(void)
{
    free(s_framebuffer);  s_framebuffer = NULL;
    free(s_fbvramA);      s_fbvramA = NULL;
    free(s_fbvramB);      s_fbvramB = NULL;
    free(s_textureram0);  s_textureram0 = NULL;
    free(s_textureram1);  s_textureram1 = NULL;
    free(s_lumaram);       s_lumaram = NULL;
    free(s_palram);       s_palram = NULL;
    free(s_colorxlat);    s_colorxlat = NULL;
    free(s_tile_ram);     s_tile_ram = NULL;
    free(s_char_ram);     s_char_ram = NULL;
}

/* --- Geometry engine ---
 *
 * The i960 does not talk to the geometry engine directly. It pushes a command
 * stream into buffer RAM through the geo program port, having first told the
 * engine where to write and where to read, and the engine walks that stream
 * once per field. Reference: MAME model2_state::geo_w / geo_prg_w / geo_parse.
 *
 * The port at 0x00804000 is dual purpose: while the control register's high
 * bit is set it is receiving TGP microcode, and otherwise it is pushing
 * geometry data. The microcode is discarded here - the geometry engine is
 * modelled directly rather than by running the DSP.
 */

static uint32_t s_geo_write_addr;    /* byte offset into buffer RAM */
static uint32_t s_geo_read_addr;
static uint32_t s_geoctl;
static uint32_t s_geo_upload_words;
static bool s_geo_list_ready;

/* Push one word onto the command stream at the current write address. */
unsigned g_geo_pushes, g_geo_publishes, g_geo_opcodes;
static void geo_push(uint32_t data)
{
    g_geo_pushes++;
    bus_bufferram_write32(s_geo_write_addr, data);
    s_geo_write_addr += 4;
}

void geo_write(uint32_t offset, uint32_t data)
{
    uint32_t address = offset * 4;

    if (address < 0x1000) {
        /*
         * This is how commands are issued: the register address carries the
         * function number and the written value carries its parameter, and the
         * two are combined into one word appended to the stream. The parser
         * later reads that function number back out as bits 23-28.
         *
         * Missing this leaves the stream as operands with no opcodes, which
         * parses as noise.
         */
        uint32_t function = (address >> 4) & 0x3F;

        if (data & 0x80000000) {
            geo_push((data & 0x800FFFFF) | (function << 23));
        } else if ((address & 0xF) == 0) {
            uint32_t r = (data & 0x000FFFFF) | (function << 23);

            /* Function 1 (object data) in the high register window also
             * carries the eye mode in the address. */
            if (((address >> 4) & 0xC0) && function == 1)
                r |= ((address >> 10) & 3) << 29;

            geo_push(r);
            g_geo_opcodes++;
        }
        return;
    }

    if (address == 0x1008) {
        s_geo_write_addr = data & 0xFFFFF;
    } else if (address == 0x3008) {
        /*
         * Writing the read address is the game publishing a finished display
         * list. Parsing on an arbitrary field boundary instead races the
         * construction of the next one: the game reads the video status
         * register more than once per field, so the parse could land midway
         * through the list being written and pick up a half-updated matrix.
         */
        s_geo_read_addr = data & 0xFFFFF;
        s_geo_list_ready = true;
        g_geo_publishes++;
    }
}

uint32_t geo_read(uint32_t offset)
{
    uint32_t address = offset * 4;

    if (address == 0x2008) return s_geo_write_addr;
    if (address == 0x3008) return s_geo_read_addr;
    return 0;
}

void geo_prg_write(uint32_t data)
{
    if (s_geoctl & 0x80000000) {
        s_geo_upload_words++;   /* TGP microcode; not executed */
        return;
    }

    geo_push(data);
}

uint32_t geo_prg_read(uint32_t offset)
{
    return 0xFFFFFFFF;   /* the real port reads back as open bus */
}

void geo_ctl1_write(uint32_t data)
{
    /* A high-bit transition brackets a microcode upload. */
    if ((data ^ s_geoctl) == 0x80000000 && (data & 0x80000000))
        s_geo_upload_words = 0;

    s_geoctl = data;
}

uint32_t geo_read_start_address(void)
{
    return s_geo_read_addr;
}

/* True once the game has published a new display list since the last parse. */
bool geo_take_list_ready(void)
{
    bool ready = s_geo_list_ready;
    s_geo_list_ready = false;
    return ready;
}

/* --- Rasterizer --- */

void render_mode_write(uint32_t data)
{
    s_render_mode = data;
}

uint32_t render_mode_read(void)
{
    return s_render_mode;
}

uint32_t polygon_count_read(void)
{
    return geo_polygon_count();
}

void videoctl_write(uint32_t data)
{
    s_videoctl = data;
}

uint32_t videoctl_read(void)
{
    return s_videoctl;
}

uint32_t fifo_control_read(void)
{
    /* Bit 0 reports the copro -> i960 output FIFO as empty (MAME
     * model2_state::fifo_control_r). The game spins on this before uploading a
     * TGP program, so getting the polarity wrong deadlocks the boot. */
    return copro_output_empty() ? 1 : 0;
}

uint32_t tgpid_read(uint32_t offset)
{
    /* TGP identification - returns chip ID for Model 2 original */
    static const uint32_t tgp_id[] = { 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000 };
    return (offset < 4) ? tgp_id[offset] : 0;
}

/* --- Framebuffer --- */

uint16_t fbvram_bankA_read(uint32_t offset)
{
    if (offset < 512 * 400) return s_fbvramA[offset];
    return 0;
}

void fbvram_bankA_write(uint32_t offset, uint16_t data)
{
    if (offset < 512 * 400) s_fbvramA[offset] = data;
}

uint16_t fbvram_bankB_read(uint32_t offset)
{
    if (offset < 512 * 400) return s_fbvramB[offset];
    return 0;
}

void fbvram_bankB_write(uint32_t offset, uint16_t data)
{
    if (offset < 512 * 400) s_fbvramB[offset] = data;
}

/* --- Texture RAM --- */

/*
 * Texture RAM writes carry 16 bits each, and two consecutive CPU words pack
 * into one storage dword - even offsets into the low half, odd into the high
 * (MAME model2_tgp_state::tex0_w). Storing a full 32-bit word per dword instead
 * leaves every upper half zero, which blanks half of every texture and makes
 * surfaces render as flat colour.
 */
static void texram_write(uint32_t *ram, uint32_t offset, uint32_t data)
{
    uint32_t index = offset >> 1;
    if (index >= 0x200000 / 4)
        return;

    if (offset & 1)
        ram[index] = (ram[index] & 0x0000FFFF) | ((data & 0xFFFF) << 16);
    else
        ram[index] = (ram[index] & 0xFFFF0000) | (data & 0xFFFF);
}

void tex0_write(uint32_t offset, uint32_t data) { texram_write(s_textureram0, offset, data); }
void tex1_write(uint32_t offset, uint32_t data) { texram_write(s_textureram1, offset, data); }

/* --- System 24 tilemaps --- */

uint16_t tile_read(uint32_t offset)
{
    if (offset * 2 < 0x10000)
        return *(uint16_t *)(s_tile_ram + offset * 2);
    return 0;
}

void tile_write(uint32_t offset, uint16_t data)
{
    if (offset * 2 < 0x10000)
        *(uint16_t *)(s_tile_ram + offset * 2) = data;
}

uint16_t char_read(uint32_t offset)
{
    if (offset * 2 < 0x80000)
        return *(uint16_t *)(s_char_ram + offset * 2);
    return 0;
}

void char_write(uint32_t offset, uint16_t data)
{
    if (offset * 2 < 0x80000)
        *(uint16_t *)(s_char_ram + offset * 2) = data;
}

/*
 * CRT horizontal/vertical sync position. The System 24 tile device forwards
 * these, and the 3D projection uses them to place the scene inside the visible
 * area (model2_state::horizontal_sync_w / vertical_sync_w). The constants are
 * MAME's; the defaults below are the renderer's initial offsets, used until the
 * game programs the CRTC.
 */
static int s_crtc_xoffset = 90;
static int s_crtc_yoffset = -8;

void tile_xhout_write(uint16_t data) { s_crtc_xoffset = 84 + (int16_t)data; }
void tile_xvout_write(uint16_t data) { s_crtc_yoffset = 130 + (int16_t)data; }

void video_get_crtc_offsets(int *x, int *y)
{
    if (x) *x = s_crtc_xoffset;
    if (y) *y = s_crtc_yoffset;
}

/* Direct access for the rasterizer, which reads these every pixel. */
const uint16_t *video_get_palram(void)     { return s_palram; }
const uint16_t *video_get_colorxlat(void)  { return s_colorxlat; }
const uint8_t  *video_get_lumaram(void)    { return s_lumaram; }
const uint32_t *video_get_texture_ram(int bank) { return bank ? s_textureram1 : s_textureram0; }

/* --- Palette --- */

uint16_t palette_read(uint32_t offset)
{
    if (offset < 0x4000 / 2) return s_palram[offset];
    return 0;
}

void palette_write(uint32_t offset, uint16_t data)
{
    if (offset < 0x4000 / 2) s_palram[offset] = data;
}

uint16_t colorxlat_read(uint32_t offset)
{
    if (offset < 0xC000 / 2) return s_colorxlat[offset];
    return 0;
}

void colorxlat_write(uint32_t offset, uint16_t data)
{
    if (offset < 0xC000 / 2) s_colorxlat[offset] = data;
}

void zclip_write(uint32_t data)
{
    s_zclip = data;
    geo_set_master_z_clip(data);
}

/* --- Luma RAM --- */

uint8_t lumaram_read(uint32_t offset)
{
    if (offset < 0x8000) return s_lumaram[offset];
    return 0;
}

void lumaram_write(uint32_t offset, uint8_t data)
{
    if (offset < 0x8000) s_lumaram[offset] = data;
}

/* --- System 24 tilemap rendering ---
 *
 * Four 64x64 tilemaps of 8x8 4bpp characters. Reference: MAME segaic24.cpp
 * (segas24_tile_device) for the layout, model2_v.cpp for the draw order.
 *
 * Tile RAM (word offsets):
 *   0x0000/0x1000/0x2000/0x3000  name tables for layers 0..3, 64x64 entries
 *   0x5000 + L                   horizontal scroll for layer L
 *   0x5004 + L                   vertical scroll; bit 15 disables the layer
 *
 * A name-table word is: tile index in the low bits, colour in bits 7-14, and
 * bit 15 selecting which pass draws it - clear means behind the 3D scene, set
 * means in front of it.
 *
 * Char RAM holds 32 bytes per tile: 8 rows of 8 pixels packed 4bpp. MAME reads
 * it as 16-bit words with the byte order swapped on little-endian hosts, which
 * works out to simply taking the four nibbles of each word from MSB to LSB.
 * Pen 0 is transparent.
 */

#define TILE_NAME_TABLE(L) (0x1000u * (L))
#define TILE_HSCROLL       0x5000u
#define TILE_VSCROLL       0x5004u
/* Name-table bits that index char RAM. Model 2 wires this to 0x3FFF (MAME:
 * S24TILE(config, m_tiles, 0, 0x3fff)), which is the whole 0x4000-tile char
 * RAM. Note the colour field at bits 7-14 overlaps it - that is the hardware,
 * not a decode mistake: a tile's index partly determines its palette bank. */
#define TILE_MASK          0x3FFFu

/* Model 2 palette entries are 15-bit, red in the low bits.
 *
 * ponytail: the real path runs this through the colour-translate RAM and a
 * gamma table (model2_v.cpp) before the DAC. Straight 5-to-8 bit expansion
 * until there is something on screen to compare against. */
/*
 * A palette entry is not a colour. Each of its three 5-bit fields selects one
 * of 32 ramps in colour-translate RAM, and the tilemaps read those ramps at
 * luma 0x40 - fully lit - before the gamma curve. Expanding the 5 bits to 8
 * directly skips the game's own colour setup: on the warning screen every pen
 * came out within a shade of white, so white text sat invisibly on a white
 * background. From model2_v.cpp screen_update_model2.
 */
#define XLAT_TILE_R 0x0040
#define XLAT_TILE_G 0x2040
#define XLAT_TILE_B 0x4040

static inline uint32_t palette_rgbx(uint16_t entry)
{
    const uint8_t *gam = geo_get_gamma();
    uint32_t c = entry & 0x7FFF;
    uint32_t r = gam[s_colorxlat[XLAT_TILE_R + (((c >> 0)  & 0x1F) << 8)] & 0xFF];
    uint32_t g = gam[s_colorxlat[XLAT_TILE_G + (((c >> 5)  & 0x1F) << 8)] & 0xFF];
    uint32_t b = gam[s_colorxlat[XLAT_TILE_B + (((c >> 10) & 0x1F) << 8)] & 0xFF];
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static inline uint16_t tile_word(uint32_t word_offset)
{
    return *(const uint16_t *)(s_tile_ram + word_offset * 2);
}

/*
 * Draw one tilemap layer. `pass` selects name-table entries by bit 15, so the
 * same layer contributes to both the behind-3D and in-front-of-3D passes.
 * `opaque` writes pen 0 as well, which is how the bottom layer clears.
 */
static void tilemap_draw_layer(int layer, int pass, int opaque)
{
    uint16_t hscr = tile_word(TILE_HSCROLL + layer);
    uint16_t vscr = tile_word(TILE_VSCROLL + layer);

    if (vscr & 0x8000)
        return; /* layer disabled */

    {   /* MODEL2_NOLAYER=<bitmask> drops layers, to see who paints what. */
        const char *nl = getenv("MODEL2_NOLAYER");
        if (nl && (strtol(nl, NULL, 0) & (1 << layer)))
            return;
    }

    /*
     * The four tilemaps are two pairs: a "screen" half (even) and a "window"
     * half (odd). The window half is not a layer of its own. In normal mode
     * segaic24.cpp's draw_common never touches it at all - it draws only
     * tile_layer[layer] - and only when the pair's control word selects a
     * split (bits 13-14) does it draw both halves, each clipped to its own
     * region of the screen.
     *
     * Drawing the window half unconditionally is what put a solid fill over
     * everything: Virtua Cop's warning screen fills tilemap 1 with one solid
     * tile, and the whole screen - text, 3D and all - vanished behind it.
     *
     * ponytail: the odd half is skipped in both modes, which loses whatever
     * belongs inside a window but keeps everything else. Implementing the
     * split means draw_common's four cases plus per-line scroll off hscr
     * bit 15.
     */
    if (layer & 1)
        return;

    /* ponytail: plain scroll only. Per-line scroll (hscr bit 15) is in
     * draw_common too. */
    int scroll_x = hscr & 0x1FF;
    int scroll_y = vscr & 0x1FF;

    const uint32_t names = TILE_NAME_TABLE(layer);

    for (int y = 0; y < FB_HEIGHT; y++) {
        /* The tilemap is 64x64 tiles = 512x512 pixels, and wraps. */
        int src_y = (y + scroll_y) & 0x1FF;
        int tile_row = src_y >> 3;
        int fine_y = src_y & 7;
        uint8_t *dst_row = s_framebuffer + (size_t)y * FB_WIDTH * 4;

        for (int x = 0; x < FB_WIDTH; x++) {
            int src_x = (x + scroll_x) & 0x1FF;
            uint16_t name = tile_word(names + tile_row * 64 + (src_x >> 3));

            if (((name >> 15) & 1) != pass)
                continue;

            /* 32 bytes per tile, 2 words per row of 8 pixels. */
            uint32_t row_word = (name & TILE_MASK) * 16u + (uint32_t)fine_y * 2u;
            int fine_x = src_x & 7;
            uint16_t bits = *(const uint16_t *)(s_char_ram + (row_word + (fine_x >> 2)) * 2);
            uint32_t pen = (bits >> (12 - 4 * (fine_x & 3))) & 0xF;

            if (pen == 0 && !opaque)
                continue;

            uint32_t pal_index = (((name >> 7) & 0xFF) * 16u + pen) & 0x1FFF;
            *(uint32_t *)(dst_row + x * 4) = palette_rgbx(s_palram[pal_index]);
        }
    }
}

/* --- Rendering --- */

void video_render_frame(void)
{
    if (!s_framebuffer) return;

    /*
     * Frame composition, following model2_v.cpp: the tilemaps split around the
     * 3D scene. Layers draw back to front, layer 3 lowest.
     */
    for (int layer = 3; layer >= 0; layer--)
        tilemap_draw_layer(layer, 0, layer >= 2);

    /*
     * The 3D scene, over the back tilemaps and under the front ones. Only
     * pixels the rasterizer actually touched are copied; zero means nothing
     * was drawn there, so the tilemap below shows through.
     */
    const uint32_t *scene = geo_get_destmap();
    if (scene) {
        for (int y = 0; y < FB_HEIGHT; y++) {
            const uint32_t *src = scene + (size_t)y * 512;
            uint8_t *dst = s_framebuffer + (size_t)y * FB_WIDTH * 4;

            for (int x = 0; x < FB_WIDTH; x++) {
                uint32_t p = src[x];
                if (!p) continue;
                /* The rasterizer already produced 8-bit RGB. */
                dst[x * 4 + 0] = (uint8_t)(p >> 16);
                dst[x * 4 + 1] = (uint8_t)(p >> 8);
                dst[x * 4 + 2] = (uint8_t)p;
                dst[x * 4 + 3] = 0xFF;
            }
        }
    }

    for (int layer = 3; layer >= 0; layer--)
        tilemap_draw_layer(layer, 1, 0);

}

const uint8_t *video_get_framebuffer(void)
{
    return s_framebuffer;
}
