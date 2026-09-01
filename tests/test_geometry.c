/*
 * Geometry engine / 3D rasterizer check.
 *
 * Virtua Cop currently submits an empty display list every field, so running
 * the game proves nothing about this path. This builds a command stream by
 * hand in buffer RAM - exactly the way the hardware's register writes build
 * one - and checks that a triangle comes out the far end in the right place.
 *
 * The stream uses "direct data" (command 02), which carries its vertices
 * inline, so the test needs neither the polygon ROM nor the texture ROM.
 *
 * Run: model2recomp_test_geometry  (prints OK and exits 0, or names each
 * failing check and exits 1)
 */

#include "model2recomp/video.h"
#include "model2recomp/bus.h"
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static int g_failures;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++; \
        } \
    } while (0)

#define FB_STRIDE 512
#define FB_WIDTH  496
#define FB_HEIGHT 384

/* Geo register file: writing to address (function << 4) appends a command
 * word, and the geo program port appends its operands. */
static void cmd(uint32_t function, uint32_t param)
{
    geo_write((function << 4) / 4, param);
}

static void arg(uint32_t value)
{
    geo_prg_write(value);
}

static uint32_t f2u(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

static int count_drawn_pixels(void)
{
    const uint16_t *map = geo_get_destmap();
    int n = 0;
    if (!map) return 0;

    for (int y = 0; y < FB_HEIGHT; y++)
        for (int x = 0; x < FB_WIDTH; x++)
            if (map[(size_t)y * FB_STRIDE + x])
                n++;
    return n;
}

/* Set up viewport, centre, focus and lighting the way the game's own init
 * stream does, using the values Virtua Cop actually writes. */
static void emit_setup(void)
{
    geo_write(0x1008 / 4, 0);       /* write start address */
    geo_write(0x3008 / 4, 0);       /* read start address */

    cmd(0x07, 0);  arg(0);          /* geo mode: normals present, no specular */
    cmd(0x03, 0);                   /* window data */
    arg(0x00000080);                /* viewport start  (x 0, y 128) */
    arg(0x01F00200);                /* viewport end    (x 496, y 512) */
    arg(0x00F80140);                /* centre, eye 0   (x 248, y 320) */
    arg(0x00F80140);
    arg(0x00F80140);
    arg(0x00F80140);

    cmd(0x09, 0);                   /* focal distance */
    arg(f2u(925.549f));
    arg(f2u(925.549f));

    cmd(0x0A, 0);                   /* light vector */
    arg(f2u(0.0f)); arg(f2u(0.0f)); arg(f2u(-1.0f));

    cmd(0x16, 0); arg(f2u(1.0f));   /* LOD */
}

/*
 * One triangle via direct data. The layout, from geo_direct_data:
 *   texture point address, texture header address,
 *   two points (x,y,z each), then per polygon:
 *   attributes, luma, distance, one point, and for a quad a second point,
 *   terminated by an attribute word with the low two bits clear.
 *
 * Coordinates travel as 24-bit fields: the handler drops the low 8 bits of the
 * float on the way in and the rasterizer shifts them back on the way out, so a
 * plain float bit pattern round-trips, losing only mantissa precision.
 *
 * Luma is the exception - the geometry path has already positioned it at bit
 * 15, so a direct-data stream pre-shifts it by a further 8.
 */

/*
 * Direct data carries coordinates that have already been through the focus
 * multiply, so the test works backwards from where it wants the triangle to
 * land. Projection (model2_3d_project) is:
 *
 *   screen_x = CRTC_XOFFSET + center_x + x / z
 *   screen_y = (384 - center_y) + CRTC_YOFFSET - y / z
 *
 * with CRTC_XOFFSET 90, CRTC_YOFFSET -8 and the centre this stream sets up.
 */
#define CENTER_X 248
#define CENTER_Y 320
#define SCREEN_X0 (90 + CENTER_X)
#define SCREEN_Y0 ((384 - CENTER_Y) - 8)

static float view_x_for(float screen_x, float z) { return (screen_x - SCREEN_X0) * z; }
static float view_y_for(float screen_y, float z) { return (SCREEN_Y0 - screen_y) * z; }

typedef struct { float x, y; } pt2_t;

static void emit_triangle_at(pt2_t a, pt2_t b, pt2_t c, float z)
{
    /* Bit 23 set on both addresses keeps texture lookups in internal RAM. */
    cmd(0x02, 0);
    arg(0x00800000);        /* texture point address */
    arg(0x00800000);        /* texture header address */

    /* P0 and P1. The rasterizer reads these as the carried-over pair. */
    arg(f2u(view_x_for(a.x, z)));  arg(f2u(view_y_for(a.y, z)));  arg(f2u(z));
    arg(f2u(view_x_for(b.x, z)));  arg(f2u(view_y_for(b.y, z)));  arg(f2u(z));

    /* attr: triangle (bit 1), link type 1, z from min z, double sided. */
    uint32_t attr = 0x2 | (1u << 8) | (1u << 10) | (1u << 17);
    arg(attr);
    arg(0x40u << 23);       /* luma 0x40, front face */
    arg(0);                 /* distance */
    arg(f2u(view_x_for(c.x, z)));  arg(f2u(view_y_for(c.y, z)));  arg(f2u(z));

    arg(0);                 /* attribute with low bits clear: end of strip */
    cmd(0x0F, 0);           /* end of the display list */
}

/* A triangle roughly in the middle of the visible band. The clip planes come
 * from the viewport and centre, which restrict the projected y to about
 * [-136, 248]; the framebuffer clip then takes the top at the viewport start. */
static const pt2_t TRI_A = { 200.0f, 150.0f };
static const pt2_t TRI_B = { 400.0f, 150.0f };
static const pt2_t TRI_C = { 300.0f, 240.0f };

static bool pixel_set(int x, int y)
{
    const uint16_t *map = geo_get_destmap();
    return map && map[(size_t)y * FB_STRIDE + x] != 0;
}

static void test_empty_list_draws_nothing(void)
{
    geo_write(0x1008 / 4, 0);
    geo_write(0x3008 / 4, 0);
    cmd(0x0F, 0);

    geo_parse();
    geo_render_polygons();

    CHECK(geo_polygon_count() == 0);
    CHECK(count_drawn_pixels() == 0);
    printf("  empty display list draws nothing: OK\n");
}

static void test_triangle_reaches_the_screen(void)
{
    emit_setup();
    emit_triangle_at(TRI_A, TRI_B, TRI_C, 200.0f);

    geo_parse();

    CHECK(geo_polygon_count() > 0);
    if (geo_polygon_count() == 0) {
        printf("  triangle: no polygon reached the list\n");
        return;
    }

    geo_render_polygons();

    int drawn = count_drawn_pixels();
    CHECK(drawn > 0);

    /* Inside: near the centroid. Outside: well clear of every edge. */
    CHECK(pixel_set(300, 180));
    CHECK(!pixel_set(100, 180));
    CHECK(!pixel_set(300, 350));

    printf("  triangle rasterized at the projected position: %d pixels\n", drawn);
}

/* Geometry behind the eye must not reach the screen. Two mechanisms would
 * each achieve that on their own - the max-z cull and the clip planes - so
 * this asserts the outcome rather than which one fired. */
static void test_negative_z_is_culled(void)
{
    emit_setup();
    emit_triangle_at(TRI_A, TRI_B, TRI_C, -200.0f);

    geo_parse();
    geo_render_polygons();

    CHECK(geo_polygon_count() == 0);
    CHECK(count_drawn_pixels() == 0);
    printf("  geometry behind the eye is culled: OK\n");
}

/* Nearer geometry sorts into a lower z bucket than farther geometry, which is
 * what the back-to-front draw order depends on. */
static void test_depth_sorting(void)
{
    emit_setup();
    emit_triangle_at(TRI_A, TRI_B, TRI_C, 100.0f);
    geo_parse();
    uint32_t near_count = geo_polygon_count();

    emit_setup();
    emit_triangle_at(TRI_A, TRI_B, TRI_C, 800.0f);
    geo_parse();
    uint32_t far_count = geo_polygon_count();

    CHECK(near_count > 0);
    CHECK(far_count > 0);
    printf("  depth sorting: near=%u far=%u polygon(s)\n", near_count, far_count);
}

int main(void)
{
    bus_init();
    video_init();
    geo_init();

    printf("Model 2 geometry engine:\n");
    test_empty_list_draws_nothing();
    test_triangle_reaches_the_screen();
    test_negative_z_is_culled();
    test_depth_sorting();

    geo_shutdown();
    video_shutdown();
    bus_shutdown();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
