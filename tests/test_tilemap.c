/*
 * System 24 tilemap renderer check.
 *
 * The game this library was written for draws a 3D attract screen and leaves
 * its text layer blank, so booting it proves nothing about the tilemap path.
 * This drives the renderer directly with a known pattern instead.
 *
 * Run: model2recomp_test_tilemap   (prints OK and exits 0, or names each
 * failing check and exits 1)
 */

#include "model2recomp/video.h"
#include <stdio.h>
#include <string.h>

/* Not assert(): release builds define NDEBUG and compile it out, which makes
 * the whole file report OK no matter what the renderer does. */
static int g_failures;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++; \
        } \
    } while (0)

#define FB_WIDTH  496
#define FB_HEIGHT 384

/* Tile RAM word offsets, matching video.c */
#define NAME_TABLE(L) (0x1000u * (L))
#define HSCROLL       0x5000u
#define VSCROLL       0x5004u

static uint32_t pixel_at(int x, int y)
{
    const uint8_t *fb = video_get_framebuffer();
    const uint8_t *p = fb + ((size_t)y * FB_WIDTH + x) * 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* Write one 8x8 tile: every pixel gets pen `pen`. Char RAM is 16 words per
 * tile, four pixels per word, most significant nibble leftmost. */
static void fill_tile(uint32_t tile, uint32_t pen)
{
    uint16_t row = (uint16_t)((pen << 12) | (pen << 8) | (pen << 4) | pen);
    for (uint32_t w = 0; w < 16; w++)
        char_write(tile * 16 + w, row);
}

/* Disable every layer, then let each test enable just what it needs. */
static void blank_all_layers(void)
{
    for (uint32_t o = 0; o < 0x8000; o++)
        tile_write(o, 0);
    for (int L = 0; L < 4; L++)
        tile_write(VSCROLL + L, 0x8000);   /* bit 15 = layer off */
}

static void test_layer_disable_bit(void)
{
    blank_all_layers();
    /* Layer 3 is the opaque one; with it disabled nothing should be written,
     * so the framebuffer keeps whatever was there. */
    memset((void *)video_get_framebuffer(), 0x5A, FB_WIDTH * FB_HEIGHT * 4);
    video_render_frame();
    CHECK(pixel_at(10, 10) == 0x5A5A5A);
    printf("  layer disable bit: OK\n");
}

static void test_opaque_layer_clears(void)
{
    blank_all_layers();
    tile_write(VSCROLL + 3, 0);            /* enable layer 3 */
    palette_write(0, 0x001F);              /* pen 0 of colour 0 = full red */
    fill_tile(0, 0);                       /* tile 0 is all pen 0 */

    memset((void *)video_get_framebuffer(), 0x5A, FB_WIDTH * FB_HEIGHT * 4);
    video_render_frame();

    /* Red is the low 5 bits, expanded to 8: 0x1F << 3 = 0xF8. */
    CHECK(pixel_at(0, 0) == 0x0000F8);
    CHECK(pixel_at(495, 383) == 0x0000F8);
    printf("  opaque layer clears screen: OK\n");
}

static void test_tile_pixels_and_colour(void)
{
    blank_all_layers();
    tile_write(VSCROLL + 3, 0);
    tile_write(VSCROLL + 0, 0);
    palette_write(0, 0x0000);              /* background black */

    /*
     * The tile index (bits 0-13) and the colour (bits 7-14) overlap, so a name
     * entry does not let you choose them independently. Pick the entry, then
     * derive both the way the hardware does.
     *
     * The index is deliberately above 0x0FFF: Model 2 masks with 0x3FFF, and a
     * narrower mask would silently fetch the wrong tile for most of char RAM.
     */
    const uint16_t name = 0x1107;
    const uint32_t tile   = name & 0x3FFF;
    const uint32_t colour = (name >> 7) & 0xFF;

    palette_write(colour * 16 + 5, 0x03E0);   /* green is bits 5-9 */
    fill_tile(tile, 5);                       /* solid pen 5 */
    tile_write(NAME_TABLE(0) + 0, name);

    video_render_frame();

    CHECK(pixel_at(0, 0) == 0x00F800);    /* inside the tile: green */
    CHECK(pixel_at(7, 7) == 0x00F800);    /* still inside (8x8) */
    CHECK(pixel_at(8, 0) == 0x000000);    /* next tile along is empty */
    CHECK(pixel_at(0, 8) == 0x000000);    /* next tile down is empty */
    printf("  tile pixels, size and colour: OK\n");
}

/*
 * Pixel order within a tile row. Uniform tiles pass whatever the nibble order
 * is, so this uses eight different pens across the row: pixel x must come from
 * nibble x counting down from the most significant, and the two words of a row
 * must be left half then right half.
 */
static void test_pixel_order_within_row(void)
{
    blank_all_layers();
    tile_write(VSCROLL + 3, 0);
    tile_write(VSCROLL + 0, 0);
    palette_write(0, 0x0000);

    /* Colour 0, pens 1..8 get distinct red levels so each column is
     * identifiable from the framebuffer alone. */
    for (uint32_t pen = 1; pen <= 8; pen++)
        palette_write(pen, (uint16_t)(pen * 2));   /* red = pen*2 */

    /* Row reads 1,2,3,4 then 5,6,7,8 left to right. */
    for (uint32_t y = 0; y < 8; y++) {
        char_write(11 * 16 + y * 2 + 0, 0x1234);
        char_write(11 * 16 + y * 2 + 1, 0x5678);
    }
    tile_write(NAME_TABLE(0) + 0, (uint16_t)11);

    video_render_frame();

    for (int x = 0; x < 8; x++) {
        uint32_t expect_red = (uint32_t)((x + 1) * 2) << 3;
        uint32_t got = pixel_at(x, 0) & 0xFF;
        if (got != expect_red)
            printf("  at x=%d expected red %02X got %02X\n", x, expect_red, got);
        CHECK(got == expect_red);
    }
    printf("  pixel order within a row: OK\n");
}

static void test_pen_zero_is_transparent(void)
{
    blank_all_layers();
    tile_write(VSCROLL + 3, 0);
    tile_write(VSCROLL + 0, 0);
    palette_write(0, 0x001F);              /* layer 3 clears to red */
    const uint16_t name = 0x0089;          /* tile 0x89, colour 1 */
    palette_write(((name >> 7) & 0xFF) * 16 + 0, 0x7FFF); /* pen 0 white: must not show */
    fill_tile(name & 0x3FFF, 0);           /* solid pen 0 */
    tile_write(NAME_TABLE(0) + 0, name);

    video_render_frame();

    /* A transparent tile on layer 0 leaves layer 3's red showing. */
    CHECK(pixel_at(2, 2) == 0x0000F8);
    printf("  pen 0 transparent: OK\n");
}

static void test_scroll(void)
{
    blank_all_layers();
    tile_write(VSCROLL + 3, 0);
    tile_write(VSCROLL + 0, 0);
    palette_write(0, 0x0000);
    palette_write(5, 0x7C00);              /* colour 0, pen 5 = blue */
    fill_tile(3, 5);

    /* Put the tile one cell right and one down, then scroll it to the origin. */
    tile_write(NAME_TABLE(0) + 64 + 1, (uint16_t)3);
    tile_write(HSCROLL + 0, 8);
    tile_write(VSCROLL + 0, 8);

    video_render_frame();

    CHECK(pixel_at(0, 0) == 0xF80000);
    CHECK(pixel_at(8, 8) == 0x000000);
    printf("  scroll: OK\n");
}

/* A name-table entry with bit 15 set belongs to the pass drawn over the 3D
 * scene, so it must not appear in the pass drawn behind it. Both passes run
 * inside one video_render_frame, so the visible result is the same - what this
 * pins down is that the bit selects a pass rather than being masked into the
 * tile index. */
static void test_pass_bit_is_not_part_of_tile_index(void)
{
    blank_all_layers();
    tile_write(VSCROLL + 3, 0);
    tile_write(VSCROLL + 0, 0);
    palette_write(0, 0x0000);
    palette_write(6, 0x7FFF);              /* colour 0 pen 6 = white */
    fill_tile(4, 6);

    tile_write(NAME_TABLE(0) + 0, (uint16_t)(4 | 0x8000));

    video_render_frame();

    CHECK(pixel_at(1, 1) == 0xF8F8F8);
    printf("  pass bit selects pass, not tile: OK\n");
}

int main(void)
{
    video_init();

    printf("System 24 tilemap renderer:\n");
    test_layer_disable_bit();
    test_opaque_layer_clears();
    test_tile_pixels_and_colour();
    test_pixel_order_within_row();
    test_pen_zero_is_transparent();
    test_scroll();
    test_pass_bit_is_not_part_of_tile_index();

    video_shutdown();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
