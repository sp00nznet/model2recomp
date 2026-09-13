/*
 * Model 2 geometry engine and 3D rasterizer.
 *
 * Ported from MAME's model2_v.cpp (BSD-3-Clause). MAME models this hardware
 * directly in C++ rather than by running the TGP DSP's microcode, which is why
 * this can exist without an MB86233 emulator.
 *
 * The pipeline, once per field:
 *
 *   buffer RAM  -> geo_parse       walks a command stream the i960 pushed
 *               -> geo_* handlers  set matrices, lights, texture params
 *               -> geo_parse_*     transform vertices, compute lighting
 *               -> model2_3d_push  a word-at-a-time command FIFO
 *               -> process_polygon clip, cull, z-sort into the polygon list
 *               -> render_polygons project and rasterize, back to front
 *
 * The stream is a linked list of polygons rather than a list of triangles:
 * each entry supplies only the new vertices, and a link type says which of the
 * previous polygon's vertices to reuse. That is why the command buffer is
 * shuffled at the end of process_polygon instead of being cleared.
 */

#include "model2recomp/video.h"
#include "model2recomp/bus.h"
#include <math.h>
#include <float.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MODEL2_POLYCOUNT accounting: how many polygons the engine saw, and how many
 * it threw away. */
unsigned g_geo_seen, g_geo_culled, g_geo_clipped;

#define FB_WIDTH   496
#define FB_HEIGHT  384
#define FB_STRIDE  512      /* framebuffer VRAM is 512 pixels wide */

#define MAX_POLYGONS 32768
#define MAX_VERTS    8

/* ---- Bit-level float conversion, as the hardware stores them ---- */

static inline float u2f(uint32_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

static inline uint32_t f2u(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    return v;
}

/* A vertex carries its screen position plus interpolated z and texture
 * coordinates. MAME calls these p[0..2]; named here for legibility. */
typedef struct {
    float x, y;
    float pz, pu, pv;
} vertex_t;

typedef struct {
    vertex_t normal;
    float    distance;
} plane_t;

typedef struct {
    float    diffuse;
    float    ambient;
    uint32_t specular_control;
    float    specular_scale;
} texparam_t;

typedef struct polygon_s {
    struct polygon_s *next;
    vertex_t v[MAX_VERTS];
    uint8_t  num_vertices;
    uint16_t z;
    uint16_t texheader[4];
    uint8_t  luma;
    int32_t  texlod;
    int16_t  viewport[4];
    int16_t  center[2];
    uint8_t  window;
} polygon_t;

/* ---- Rasterizer state ---- */

typedef struct {
    const uint16_t *texture_rom;
    uint32_t        texture_rom_mask;

    int16_t  viewport[4];
    int16_t  center[4][2];
    uint16_t center_sel;
    uint32_t reverse;
    int32_t  z_adjust;
    float    polygon_z;
    uint8_t  master_z_clip;

    uint32_t cur_command;
    uint32_t command_buffer[32];
    uint32_t command_index;

    polygon_t *poly_list;
    uint32_t   poly_list_index;
    polygon_t *poly_sorted_list[0x10000];
    uint16_t   min_z, max_z;

    uint16_t texture_ram[0x10000];
    uint8_t  log_ram[0x8000];
    uint8_t  cur_window;
    plane_t  clip_plane[4][4];
} raster_state_t;

/* ---- Geometry engine state ---- */

typedef struct {
    uint32_t        mode;               /* bit 0 = specular, bit 1 = no normals */
    const uint32_t *polygon_rom;
    uint32_t        polygon_rom_mask;
    float           matrix[12];
    vertex_t        focus;
    vertex_t        light;
    float           lod;
    float           coef_table[32];
    texparam_t      texture_parameters[32];
    uint32_t        polygon_ram0[0x8000];
    uint32_t        polygon_ram1[0x8000];
} geo_state_t;

static raster_state_t *s_raster;
static geo_state_t    *s_geo;
static bool            s_render_done;

/* The 3D output goes to its own bitmap, not to framebuffer VRAM. The game
 * writes that VRAM itself in render-test mode, and MAME likewise renders to a
 * separate destmap and composites. Non-zero pixels are the drawn ones, which
 * is why the shading path sets the top byte. */
static uint32_t *s_destmap;

/* CRT offsets. MAME's renderer applies these when projecting; they position
 * the 3D image inside the 496x384 visible area. */
#define CRTC_XOFFSET 90
#define CRTC_YOFFSET (-8)

/* ---- Generic 3D math (model2_v.cpp) ---- */

static inline void transform_point(vertex_t *p, const float *m)
{
    float tx = (p->x * m[0]) + (p->y * m[3]) + (p->pz * m[6]) + m[9];
    float ty = (p->x * m[1]) + (p->y * m[4]) + (p->pz * m[7]) + m[10];
    float tz = (p->x * m[2]) + (p->y * m[5]) + (p->pz * m[8]) + m[11];
    p->x = tx; p->y = ty; p->pz = tz;
}

static inline void transform_vector(vertex_t *v, const float *m)
{
    float tx = (v->x * m[0]) + (v->y * m[3]) + (v->pz * m[6]);
    float ty = (v->x * m[1]) + (v->y * m[4]) + (v->pz * m[7]);
    float tz = (v->x * m[2]) + (v->y * m[5]) + (v->pz * m[8]);
    v->x = tx; v->y = ty; v->pz = tz;
}

static inline void normalize_vector(vertex_t *v)
{
    float n = sqrtf((v->x * v->x) + (v->y * v->y) + (v->pz * v->pz));
    if (n != 0.0f) {
        float oon = 1.0f / n;
        v->x *= oon; v->y *= oon; v->pz *= oon;
    }
}

static inline float dot_product(const vertex_t *a, const vertex_t *b)
{
    return (a->x * b->x) + (a->y * b->y) + (a->pz * b->pz);
}

static inline void vector_cross3(vertex_t *dst, const vertex_t *v0,
                                 const vertex_t *v1, const vertex_t *v2)
{
    float p1x = v1->x - v0->x, p1y = v1->y - v0->y, p1z = v1->pz - v0->pz;
    float p2x = v2->x - v0->x, p2y = v2->y - v0->y, p2z = v2->pz - v0->pz;

    dst->x  = (p1y * p2z) - (p1z * p2y);
    dst->y  = (p1z * p2x) - (p1x * p2z);
    dst->pz = (p1x * p2y) - (p1y * p2x);
}

static inline void apply_focus(vertex_t *p)
{
    p->x *= s_geo->focus.x;
    p->y *= s_geo->focus.y;
}

/* 1.8.23 float to 4.12 float, for z-sorting. */
static uint16_t float_to_zval(float floatval, int32_t z_adjust)
{
    int32_t fpint = (int32_t)f2u(floatval);
    int32_t exponent = ((fpint >> 23) & 0xFF) - ((z_adjust >> 23) & 0xFF);
    uint32_t mantissa = (uint32_t)fpint & 0x7FFFFF;

    mantissa += 0x400;
    if (mantissa > 0x7FFFFF) {
        exponent++;
        mantissa = (mantissa & 0x7FFFFF) >> 1;
    }
    mantissa >>= 11;

    if (fpint < 0)      return 0x0000;
    if (exponent < -12) return 0x0000;
    if (exponent < 0)   return (uint16_t)((mantissa | 0x1000) >> -exponent);
    if (exponent < 15)  return (uint16_t)(((exponent + 1) << 12) | mantissa);
    return 0xFFFF;
}

static int32_t clip_polygon(const vertex_t *v, int32_t num_vertices,
                            vertex_t *vout, const plane_t *clip_plane)
{
    int32_t outcount = 0;
    const vertex_t *cur = v;

    float curdot = dot_product(cur, &clip_plane->normal);
    int32_t curin = (curdot >= clip_plane->distance) ? 1 : 0;

    for (int32_t i = 0; i < num_vertices; i++) {
        int32_t nextvert = (i + 1) % num_vertices;

        if (curin)
            vout[outcount++] = *cur;

        float nextdot = dot_product(&v[nextvert], &clip_plane->normal);
        int32_t nextin = (nextdot >= clip_plane->distance) ? 1 : 0;

        if ((curin != nextin) && !isnan(curdot) && !isnan(nextdot)) {
            float scale = (clip_plane->distance - curdot) / (nextdot - curdot);

            vout[outcount].x  = cur->x  + ((v[nextvert].x  - cur->x)  * scale);
            vout[outcount].y  = cur->y  + ((v[nextvert].y  - cur->y)  * scale);
            vout[outcount].pz = cur->pz + ((v[nextvert].pz - cur->pz) * scale);
            vout[outcount].pu = cur->pu + ((v[nextvert].pu - cur->pu) * scale);
            vout[outcount].pv = cur->pv + ((v[nextvert].pv - cur->pv) * scale);
            outcount++;
        }

        curdot = nextdot;
        curin = nextin;
        cur++;
    }

    return outcount;
}

static bool check_culling(uint32_t attr, float min_z, float max_z)
{
    /* Backface, unless the polygon is marked double sided. */
    if (((attr >> 17) & 1) == 0 && (s_raster->command_buffer[9] & 0x00800000))
        return true;

    /* Link type 0 terminates a strip rather than drawing. */
    if (((attr >> 8) & 3) == 0)
        return true;

    if (s_raster->master_z_clip != 0xFF && (int32_t)(1.0f / min_z) > s_raster->master_z_clip)
        return true;

    if (max_z < 0)
        return true;

    return false;
}

/* ---- Rasterizer command processing ---- */

static void model2_3d_process_polygon(uint32_t attr, int num_verts)
{
    raster_state_t *raster = s_raster;
    vertex_t v[4];
    uint16_t texheader[4];
    const uint16_t *tp, *th;
    uint8_t luma;
    int32_t texlod, tho;
    float zvalue, min_z, max_z;

    /* P0(n-1), P1(n-1) carried over from the previous polygon in the strip. */
    v[1].x  = u2f(raster->command_buffer[2] << 8);
    v[1].y  = u2f(raster->command_buffer[3] << 8);
    v[1].pz = u2f(raster->command_buffer[4] << 8);

    v[0].x  = u2f(raster->command_buffer[5] << 8);
    v[0].y  = u2f(raster->command_buffer[6] << 8);
    v[0].pz = u2f(raster->command_buffer[7] << 8);

    v[2].x  = u2f(raster->command_buffer[11] << 8);
    v[2].y  = u2f(raster->command_buffer[12] << 8);
    v[2].pz = u2f(raster->command_buffer[13] << 8);

    if (num_verts == 4) {
        v[3].x  = u2f(raster->command_buffer[14] << 8);
        v[3].y  = u2f(raster->command_buffer[15] << 8);
        v[3].pz = u2f(raster->command_buffer[16] << 8);
    } else {
        /* For a triangle the rope of P1(n) is P0(n-1), i.e. link type 3. */
        raster->command_buffer[14] = raster->command_buffer[11];
        raster->command_buffer[15] = raster->command_buffer[12];
        raster->command_buffer[16] = raster->command_buffer[13];
    }

    min_z = max_z = v[0].pz;
    for (int i = 1; i < num_verts; i++) {
        if (v[i].pz < min_z) min_z = v[i].pz;
        if (v[i].pz > max_z) max_z = v[i].pz;
    }

    /* Texture coordinates come from a separate stream indexed by the
     * "texture point address", which walks forward two words per vertex. */
    if ((raster->command_buffer[0] & 0x800000) || !raster->texture_rom)
        tp = &raster->texture_ram[raster->command_buffer[0] & 0xFFFF];
    else
        tp = &raster->texture_rom[raster->command_buffer[0] & raster->texture_rom_mask];

    for (int i = 0; i < num_verts; i++) {
        v[i].pv = *tp++;
        v[i].pu = *tp++;
    }
    raster->command_buffer[0] += num_verts * 2;

    if ((raster->command_buffer[1] & 0x800000) || !raster->texture_rom)
        th = &raster->texture_ram[raster->command_buffer[1] & 0xFFFF];
    else
        th = &raster->texture_rom[raster->command_buffer[1] & raster->texture_rom_mask];

    for (int i = 0; i < 4; i++)
        texheader[i] = th[i];

    /* The header offset is a signed 5-bit field. */
    tho = (int32_t)((attr >> 12) & 0x1F);
    if (tho & 0x10)
        tho |= -16;
    raster->command_buffer[1] += tho * 4;

    luma = (uint8_t)((raster->command_buffer[9] >> 15) & 0xFF);

    texlod = ((int32_t)(raster->command_buffer[10] >> 8) & 0x7F80) - 0x3F80;
    texlod += raster->log_ram[raster->command_buffer[10] & 0x7FFF];

    switch ((attr >> 10) & 3) {
        case 0:  zvalue = raster->polygon_z; break;   /* keep previous */
        case 1:  zvalue = min_z; break;
        case 2:  zvalue = max_z; break;
        default: zvalue = 1e10f; break;
    }
    raster->polygon_z = zvalue;

    g_geo_seen++;
    if (check_culling(attr, min_z, max_z)) g_geo_culled++;
    if (!check_culling(attr, min_z, max_z)) {
        vertex_t verts_in[MAX_VERTS], verts_out[MAX_VERTS];
        int32_t clipped_verts = num_verts;

        for (int i = 0; i < num_verts; i++)
            verts_in[i] = v[i];

        for (int i = 0; i < 4; i++) {
            clipped_verts = clip_polygon(verts_in, clipped_verts, verts_out,
                                         &raster->clip_plane[raster->center_sel][i]);
            for (int j = 0; j < clipped_verts; j++)
                verts_in[j] = verts_out[j];
        }

        if (clipped_verts <= 2) g_geo_clipped++;
        if (clipped_verts > 2 && raster->poly_list_index < MAX_POLYGONS) {
            uint16_t z = float_to_zval(zvalue, raster->z_adjust);
            polygon_t *poly = &raster->poly_list[raster->poly_list_index++];

            poly->z = z;
            memcpy(poly->texheader, texheader, sizeof(texheader));
            poly->luma = luma;
            poly->texlod = texlod;
            memcpy(poly->viewport, raster->viewport, sizeof(poly->viewport));
            poly->center[0] = raster->center[raster->center_sel][0];
            poly->center[1] = raster->center[raster->center_sel][1];
            poly->window = raster->cur_window;
            poly->num_vertices = (uint8_t)clipped_verts;

            for (int i = 0; i < clipped_verts; i++)
                poly->v[i] = verts_out[i];

            /* Bucket by z. Each bucket is a linked list, newest first. */
            poly->next = raster->poly_sorted_list[z];
            raster->poly_sorted_list[z] = poly;

            if (z < raster->min_z) raster->min_z = z;
            if (z > raster->max_z) raster->max_z = z;
        }
    }

    /* Carry vertices forward for the next polygon in the strip. */
    switch ((attr >> 8) & 3) {
        case 0:
        case 2:
            for (uint32_t i = 0; i < 6; i++)
                raster->command_buffer[2 + i] = raster->command_buffer[11 + i];
            break;
        case 1:
            for (uint32_t i = 0; i < 3; i++)
                raster->command_buffer[5 + i] = raster->command_buffer[11 + i];
            break;
        default:
            break;
    }
}

static void model2_3d_push(uint32_t input)
{
    raster_state_t *raster = s_raster;

    if (raster->cur_command == 0) {
        raster->cur_command = input & 0x0F;
        raster->command_index = 0;

        if (raster->cur_command == 1) {
            raster->reverse = (input >> 4) & 1;
            raster->center_sel = (input >> 6) & 3;
        }
        return;
    }

    if (raster->command_index >= 32)
        raster->command_index = 31;    /* never overrun the buffer */
    raster->command_buffer[raster->command_index++] = input;

    switch (raster->cur_command) {
    case 0x00:  /* NOP */
        break;

    case 0x01: { /* Polygon data */
        if (raster->command_index < 9)
            return;

        uint32_t attr = raster->command_buffer[8];

        if ((attr & 3) == 0) {
            raster->cur_command = 0;
            return;
        }

        if (attr & 1) {                         /* quad */
            if (raster->command_index < 17)
                return;
            model2_3d_process_polygon(attr, 4);
        } else {                                /* triangle */
            if (raster->command_index < 14)
                return;
            model2_3d_process_polygon(attr, 3);
        }
        raster->command_index = 8;              /* wait for the next link */
        break;
    }

    case 0x03: { /* Window data: viewport, per-eye centres, clip planes */
        if (raster->command_index < 6)
            return;

        /* Coordinates are 12-bit signed, packed two per word. */
        #define SEXT12(v) (((v) & 0x800) ? -(int16_t)(0x800 - ((v) & 0x7FF)) : (int16_t)(v))

        raster->viewport[0] = SEXT12((raster->command_buffer[0] >> 12) & 0xFFF);
        raster->viewport[1] = SEXT12(raster->command_buffer[0] & 0xFFF);
        raster->viewport[2] = SEXT12((raster->command_buffer[1] >> 12) & 0xFFF);
        raster->viewport[3] = SEXT12(raster->command_buffer[1] & 0xFFF);

        for (int i = 0; i < 4; i++) {
            raster->center[i][0] = SEXT12((raster->command_buffer[2 + i] >> 12) & 0xFFF);
            raster->center[i][1] = SEXT12(raster->command_buffer[2 + i] & 0xFFF);

            float left   = (float)(raster->center[i][0] - raster->viewport[0]);
            float right  = (float)(raster->viewport[2] - raster->center[i][0]);
            float top    = (float)(raster->viewport[3] - raster->center[i][1]);
            float bottom = (float)(raster->center[i][1] - raster->viewport[1]);

            raster->clip_plane[i][0].normal.x  =  1.0f / hypotf(1.0f, left);
            raster->clip_plane[i][0].normal.y  =  0.0f;
            raster->clip_plane[i][0].normal.pz = left / hypotf(1.0f, left);

            raster->clip_plane[i][1].normal.x  = -1.0f / hypotf(-1.0f, right);
            raster->clip_plane[i][1].normal.y  =  0.0f;
            raster->clip_plane[i][1].normal.pz = right / hypotf(-1.0f, right);

            raster->clip_plane[i][2].normal.x  =  0.0f;
            raster->clip_plane[i][2].normal.y  = -1.0f / hypotf(-1.0f, top);
            raster->clip_plane[i][2].normal.pz = top / hypotf(-1.0f, top);

            raster->clip_plane[i][3].normal.x  =  0.0f;
            raster->clip_plane[i][3].normal.y  =  1.0f / hypotf(1.0f, bottom);
            raster->clip_plane[i][3].normal.pz = bottom / hypotf(1.0f, bottom);
        }
        #undef SEXT12

        raster->cur_command = 0;
        break;
    }

    case 0x04: { /* Texture / log RAM write */
        if (raster->command_index < 2)
            return;

        if (raster->command_buffer[1] > 0 && raster->command_index >= 3) {
            uint32_t address = raster->command_buffer[0];

            if (address & 0x800000)
                raster->texture_ram[address & 0xFFFF] = (uint16_t)raster->command_buffer[2];
            else
                raster->log_ram[address & 0x7FFF] = (uint8_t)raster->command_buffer[2];

            raster->command_buffer[0]++;
            raster->command_buffer[1]--;
            raster->command_index--;    /* keep filling the same slot */
        }

        if (raster->command_buffer[1] == 0)
            raster->cur_command = 0;
        break;
    }

    case 0x08:  /* ZSort mode */
        raster->z_adjust = (int32_t)(raster->command_buffer[0] << 8);
        raster->cur_command = 0;
        break;

    default:
        /* Unknown command: drop it rather than wedging the FIFO. */
        raster->cur_command = 0;
        break;
    }
}
/* ---- Stream cursor ----
 *
 * Every command carries its operands inline, so a malformed or simply
 * unfinished stream will walk the read pointer off the end of buffer RAM or of
 * the polygon ROM. MAME gets away with raw pointers because its regions are
 * backed by allocations it controls; here the read is bounds-checked and
 * returns 0 past the end, which terminates the enclosing loop naturally.
 */
typedef struct {
    const uint32_t *p;
    const uint32_t *end;
} stream_t;

static inline uint32_t sread(stream_t *s)
{
    return (s->p < s->end) ? *s->p++ : 0;
}

static inline bool shas(const stream_t *s, uint32_t words)
{
    return (uint32_t)(s->end - s->p) >= words;
}

static inline void sskip(stream_t *s, uint32_t words)
{
    s->p = (s->p + words < s->end) ? s->p + words : s->end;
}

/* ---- Geometry engine: vertex paths ----
 *
 * MAME has four near-identical copies of this, selected by geo->mode: normals
 * present or computed, specular on or off. The input layout is the same in all
 * four - the no-normals forms still skip three words where the normal would be
 * - and only the lighting differs, so this is one function with two flags.
 */
static void geo_parse_polygons(stream_t *in, uint32_t count,
                               bool have_normals, bool specular)
{
    vertex_t point, normal, p0, p1, p2, p3;

    memset(&point, 0, sizeof(point));
    memset(&normal, 0, sizeof(normal));
    memset(&p0, 0, sizeof(p0));
    memset(&p1, 0, sizeof(p1));
    memset(&p2, 0, sizeof(p2));
    memset(&p3, 0, sizeof(p3));

    if (!shas(in, 6))
        return;

    /* First two points of the strip. */
    for (int n = 0; n < 2; n++) {
        point.x  = u2f(sread(in));
        point.y  = u2f(sread(in));
        point.pz = u2f(sread(in));
        transform_point(&point, s_geo->matrix);

        if (n == 0) p0 = point; else p1 = point;

        apply_focus(&point);
        model2_3d_push(f2u(point.x) >> 8);
        model2_3d_push(f2u(point.y) >> 8);
        model2_3d_push(f2u(point.pz) >> 8);
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!shas(in, 1))
            break;

        uint32_t attr = sread(in);
        model2_3d_push(attr & 0x0003FFFF);

        if ((attr & 3) == 0)
            break;      /* end of the strip */

        /* Normal (or the gap where it would be) plus one point, then a second
         * point for a quad or a skipped one for a triangle: 9 words either way. */
        if (!shas(in, 9))
            break;

        if (have_normals) {
            normal.x  = u2f(sread(in));
            normal.y  = u2f(sread(in));
            normal.pz = u2f(sread(in));
            transform_vector(&normal, s_geo->matrix);
        } else {
            sskip(in, 3);   /* the normal is computed from the face instead */
        }

        point.x  = u2f(sread(in));
        point.y  = u2f(sread(in));
        point.pz = u2f(sread(in));
        transform_point(&point, s_geo->matrix);
        p2 = point;

        if (!have_normals) {
            vector_cross3(&normal, &p0, &p1, &p2);
            normalize_vector(&normal);
        }

        float dotl = dot_product(&normal, &s_geo->light);
        float dotp = dot_product(&normal, &point);

        apply_focus(&point);

        /* Front or back face, which the rasterizer reads out of the luma. */
        float face = (dotp >= 0) ? 0.0f : 256.0f;

        const texparam_t *tex = &s_geo->texture_parameters[(attr >> 18) & 0x1F];

        float luminance = ((dotl * dotp) < 0) ? 0.0f : fabsf(dotl);

        if (specular) {
            float spec = ((2 * dotl) * normal.pz) - s_geo->light.pz;
            if (spec < 0) spec = 0;
            if (tex->specular_control == 0) spec = 0;
            if ((tex->specular_control >> 1) != 0) spec *= spec;
            if ((tex->specular_control >> 2) != 0) spec *= spec;
            if (((tex->specular_control + 1) >> 3) != 0) spec *= spec;
            spec *= tex->specular_scale;
            luminance = (luminance * tex->diffuse) + tex->ambient + spec;
        } else {
            luminance = (luminance * tex->diffuse) + tex->ambient;
        }

        if (!(luminance > 0.0f))  luminance = 0.0f;   /* also catches NaN */
        if (luminance > 255.0f)   luminance = 255.0f;

        int32_t luma = (int32_t)luminance + (int32_t)face;

        float coef = s_geo->coef_table[(attr >> 27) & 0x1F];
        float distance = coef * fabsf(dotp) * s_geo->lod;

        model2_3d_push((uint32_t)luma << 15);
        model2_3d_push(f2u(distance) >> 8);
        model2_3d_push(f2u(point.x) >> 8);
        model2_3d_push(f2u(point.y) >> 8);
        model2_3d_push(f2u(point.pz) >> 8);

        if (attr & 1) {                 /* quad: one more point */
            point.x  = u2f(sread(in));
            point.y  = u2f(sread(in));
            point.pz = u2f(sread(in));
            transform_point(&point, s_geo->matrix);
            p3 = point;

            apply_focus(&point);
            model2_3d_push(f2u(point.x) >> 8);
            model2_3d_push(f2u(point.y) >> 8);
            model2_3d_push(f2u(point.pz) >> 8);
        } else {                        /* triangle: skip the unused point */
            sskip(in, 3);
            p3 = p2;
        }

        /* Carry vertices forward the way the link type says. */
        switch ((attr >> 8) & 3) {
            case 0: case 2: p0 = p2; p1 = p3; break;
            case 1:         p1 = p2;          break;
            case 3:         p0 = p3;          break;
        }
    }

    model2_3d_push(0);
}

/* ---- Geometry engine: command handlers ---- */

static void geo_object_data(uint32_t opcode, stream_t *in)
{
    uint32_t tpa = sread(in);   /* texture point address */
    uint32_t tha = sread(in);   /* texture header address */
    uint32_t oba = sread(in);   /* object address */
    uint32_t obc = sread(in);   /* object count */
    stream_t src;

    model2_3d_push(opcode >> 23);
    model2_3d_push(tpa);
    model2_3d_push(tha);

    if ((oba & 0x00800000) && !s_geo->polygon_rom)
        return;     /* polygon ROM not loaded */

    if (oba & 0x01000000) {
        src.p = &s_geo->polygon_ram1[oba & 0x7FFF];
        src.end = &s_geo->polygon_ram1[0x8000];
    } else if (oba & 0x00800000) {
        uint32_t off = oba & s_geo->polygon_rom_mask;
        src.p = &s_geo->polygon_rom[off];
        src.end = &s_geo->polygon_rom[s_geo->polygon_rom_mask + 1];
    } else {
        src.p = &s_geo->polygon_ram0[oba & 0x7FFF];
        src.end = &s_geo->polygon_ram0[0x8000];
    }

    /* A count of zero rolls over to the maximum. */
    if (obc == 0)
        obc = 0xFFFFF;

    /* mode bit 1 selects "no normals in the stream", bit 0 selects specular. */
    geo_parse_polygons(&src, obc, (s_geo->mode & 2) == 0, (s_geo->mode & 1) != 0);
}

static void geo_direct_data(uint32_t opcode, stream_t *in)
{
    uint32_t attr;

    model2_3d_push((opcode >> 23) - 1);
    model2_3d_push(sread(in));  /* texture point address */
    model2_3d_push(sread(in));  /* texture header address */

    for (int i = 0; i < 6; i++)
        model2_3d_push(sread(in) >> 8);

    while (shas(in, 1) && ((attr = sread(in)) & 3) != 0) {
        if (!shas(in, (attr & 1) ? 8 : 5))
            break;

        model2_3d_push(attr & 0x00FFFFFF);
        model2_3d_push(sread(in) >> 8);      /* luma */
        model2_3d_push(sread(in) >> 8);      /* distance */

        for (int i = 0; i < 3; i++)
            model2_3d_push(sread(in) >> 8);

        if (attr & 1)
            for (int i = 0; i < 3; i++)
                model2_3d_push(sread(in) >> 8);
    }

    model2_3d_push(0);
}

static void geo_window_data(uint32_t opcode, stream_t *in)
{
    model2_3d_push(opcode >> 23);
    s_raster->cur_window++;

    /* Six coordinate pairs: viewport start and end, then one vanishing point
     * per eye mode. Repacked from XXX0YYY to 00XXXYYY for the rasterizer. */
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t y = sread(in);
        uint32_t x = (y & 0x0FFF0000) >> 4;
        y &= 0xFFF;
        model2_3d_push(x | y);
    }
}

static void geo_texture_data(uint32_t opcode, stream_t *in)
{
    model2_3d_push(opcode >> 23);
    model2_3d_push(sread(in));          /* start address / dsp id */

    uint32_t count = sread(in);
    model2_3d_push(count);

    for (uint32_t i = 0; i < count && shas(in, 1); i++)
        model2_3d_push(sread(in));
}

static void geo_polygon_data(uint32_t opcode, stream_t *in)
{
    uint32_t address = sread(in);
    uint32_t *p = (address & 0x01000000)
        ? &s_geo->polygon_ram1[address & 0x7FFF]
        : &s_geo->polygon_ram0[address & 0x7FFF];

    uint32_t count = sread(in);
    uint32_t room = 0x8000 - (address & 0x7FFF);
    if (count > room) count = room;

    for (uint32_t i = 0; i < count && shas(in, 1); i++)
        *p++ = sread(in);

    (void)opcode;
}

static void geo_texture_parameters(uint32_t opcode, stream_t *in)
{
    uint32_t index = sread(in) >> 2;
    uint32_t count = sread(in);

    for (uint32_t i = 0; i < count && shas(in, 2); i++) {
        uint32_t param = sread(in);

        index &= 0x1F;
        s_geo->texture_parameters[index].diffuse = (float)(param & 0xFF);
        s_geo->texture_parameters[index].ambient = (float)((param >> 8) & 0xFF);
        s_geo->texture_parameters[index].specular_control = (param >> 24) & 0xFF;
        s_geo->texture_parameters[index].specular_scale = (float)((param >> 16) & 0xFF);

        s_geo->coef_table[index] = u2f(sread(in));

        index = (index + 1) & 0x1F;
    }

    (void)opcode;
}

static void geo_zsort_mode(uint32_t opcode, stream_t *in)
{
    model2_3d_push(opcode >> 23);
    model2_3d_push(sread(in) >> 8);
}

static void geo_log_data(uint32_t opcode, stream_t *in)
{
    model2_3d_push(opcode >> 23);
    model2_3d_push(sread(in));

    uint32_t count = sread(in);
    model2_3d_push(count << 2);

    for (uint32_t i = 0; i < count && shas(in, 1); i++) {
        uint32_t data = sread(in);
        model2_3d_push(data & 0xFF);
        model2_3d_push((data >> 8) & 0xFF);
        model2_3d_push((data >> 16) & 0xFF);
        model2_3d_push((data >> 24) & 0xFF);
    }
}

static void geo_test(uint32_t opcode, stream_t *in)
{
    /* FIFO walking-ones test, then polygon ROM checksums. Nothing acts on the
     * result here; the words still have to be consumed to stay in step. */
    sskip(in, 32);

    uint32_t blocks = sread(in);
    for (uint32_t i = 0; i < blocks && shas(in, 3); i++)
        sskip(in, 3);       /* address, count, checksum */

    (void)opcode;
}

/*
 * Dispatch. The opcode is the top 5 bits; the 0x10-0x1F half largely repeats
 * the 0x00-0x0F half.
 */
static void geo_process_command(uint32_t opcode, stream_t *in, bool *end_code)
{
    switch ((opcode >> 23) & 0x1F) {
    case 0x00: model2_3d_push(opcode >> 23);            break;  /* nop */
    case 0x01:
    case 0x11: geo_object_data(opcode, in);             break;
    case 0x02:
    case 0x12: geo_direct_data(opcode, in);             break;
    case 0x03:
    case 0x13: geo_window_data(opcode, in);             break;
    case 0x04: geo_texture_data(opcode, in);            break;
    case 0x05:
    case 0x15: geo_polygon_data(opcode, in);            break;
    case 0x06: geo_texture_parameters(opcode, in);      break;
    case 0x07:
    case 0x17: s_geo->mode = sread(in);                 break;
    case 0x08:
    case 0x18: geo_zsort_mode(opcode, in);              break;
    case 0x09:
    case 0x19:
        s_geo->focus.x = u2f(sread(in));
        s_geo->focus.y = u2f(sread(in));
        break;
    case 0x0A:
    case 0x1A:
        s_geo->light.x  = u2f(sread(in));
        s_geo->light.y  = u2f(sread(in));
        s_geo->light.pz = u2f(sread(in));
        break;
    case 0x0B:
    case 0x1B:
        for (int i = 0; i < 12; i++)
            s_geo->matrix[i] = u2f(sread(in));
        break;
    case 0x0C:
    case 0x1C:
        for (int i = 0; i < 3; i++)
            s_geo->matrix[i + 9] = u2f(sread(in));
        break;
    case 0x0D: sskip(in, 2);                            break;  /* DSP RAM push: unsupported */
    case 0x0E: geo_test(opcode, in);                    break;
    case 0x10: sskip(in, 1);                            break;  /* dummy read */
    case 0x14: geo_log_data(opcode, in);                break;
    case 0x16: s_geo->lod = u2f(sread(in));             break;
    case 0x1D: sskip(in, 2);                            break;  /* code upload: unsupported */
    case 0x1E: sskip(in, 1);                            break;  /* code jump: unsupported */
    case 0x0F:
    case 0x1F:
        model2_3d_push(0xFF000000);
        *end_code = true;
        break;
    }
}

/* ---- Frame ---- */

void geo_frame_start(void)
{
    raster_state_t *raster = s_raster;

    raster->poly_list_index = 0;
    memset(raster->poly_sorted_list, 0, sizeof(raster->poly_sorted_list));
    raster->min_z = 0xFFFF;
    raster->max_z = 0;
    /* Some titles set a background with "previous z" mode as the first entry
     * in the list, so this has to start large. */
    raster->polygon_z = 1e10f;
    raster->cur_window = 0;

    /* The FIFO is word-at-a-time and a truncated stream can leave a command
     * half-assembled; starting a field mid-command would misread the next one. */
    raster->cur_command = 0;
    raster->command_index = 0;

    s_render_done = false;
}

/*
 * The polygon and texture ROMs are loaded after the subsystems are
 * initialized, so bind them on first use rather than at init.
 */
static bool geo_bind_roms(void)
{
    /*
     * The mask is the region size, not the size of the data in it. Virtua Cop
     * declares both regions as 16MB and fills the first 4MB from two 2MB ROMs
     * interleaved as 32-bit words, leaving the rest zero - so the images
     * tools/rom_loader.py writes are already region-sized and the file length
     * is the right thing to mask with. Masking to the *data* extent instead
     * folds high addresses back onto real geometry and produces vertices with
     * nonsense z values.
     */
    if (s_geo->polygon_rom)
        return true;

    uint32_t poly_words = 0, tex_words = 0;
    const uint32_t *poly = bus_get_polygon_rom(&poly_words);
    const uint16_t *tex = bus_get_texture_rom(&tex_words);

    if (!poly || !tex || !poly_words || !tex_words) {
        static bool warned;
        if (!warned) {
            fprintf(stderr, "[geo] Polygon or texture ROM missing or short; 3D disabled\n");
            warned = true;
        }
        return false;
    }

    s_geo->polygon_rom = poly;
    s_geo->polygon_rom_mask = poly_words - 1;
    s_raster->texture_rom = tex;
    s_raster->texture_rom_mask = tex_words - 1;

    printf("[geo] Bound polygon and texture ROMs\n");
    return true;
}

void geo_parse(void)
{
    if (!s_geo || !s_raster)
        return;

    /* Only object data reads the ROMs, so a missing ROM disables that command
     * rather than the whole engine - direct data still draws. */
    geo_bind_roms();

    const uint32_t *base = (const uint32_t *)bus_get_buffer_ram();
    if (!base)
        return;

    stream_t in;
    in.end = base + (0x20000 / 4);
    in.p = base + ((geo_read_start_address() & 0x1FFFF) / 4);

    uint32_t op_count = 0;
    bool end_code = false;

    geo_frame_start();

    while (!end_code && shas(&in, 1) && op_count++ < 0x8000) {
        uint32_t opcode = sread(&in);

        /* The high bit makes it a jump rather than a command. */
        if (opcode & 0x80000000) {
            in.p = base + ((opcode & 0x1FFFF) / 4);
            continue;
        }

        geo_process_command(opcode, &in, &end_code);
    }
}

uint32_t geo_polygon_count(void)
{
    return s_raster ? s_raster->poly_list_index : 0;
}

/* ---- Projection and rasterization ---- */

static void model2_3d_project(polygon_t *poly)
{
    int xoff, yoff;
    video_get_crtc_offsets(&xoff, &yoff);

    for (int i = 0; i < poly->num_vertices; i++) {
        poly->v[i].x = xoff + poly->center[0]
                     + (poly->v[i].x / (poly->v[i].pz + FLT_MIN));
        poly->v[i].y = ((384 - poly->center[1]) + yoff)
                     - (poly->v[i].y / (poly->v[i].pz + FLT_MIN));
    }
}

/*
 * Colour.
 *
 * Nothing on this hardware picks an RGB value directly. A polygon names a
 * palette entry, which selects one of 32 ramps per channel in the colour
 * translate RAM, and the pixel's luma indexes along that ramp. Textured pixels
 * derive their luma from the texel through the luma RAM; untextured ones use
 * the polygon's own luma. Then everything goes through a gamma curve.
 *
 * From model2rd.ipp draw_scanline_solid / draw_scanline_tex.
 */
#define COLORXLAT_R 0x0000
#define COLORXLAT_G 0x2000      /* 0x4000 bytes / 2 */
#define COLORXLAT_B 0x4000      /* 0x8000 bytes / 2 */

static uint8_t s_gamma[256];

static void build_gamma_table(void)
{
    /* MAME's colour-space conversion; a real cabinet's monitor calibration
     * varied per game. */
    for (int i = 0; i < 256; i++) {
        double v = ((double)i - 64.0) * 255.0 / 191.0;
        s_gamma[i] = (uint8_t)(v < 0.0 ? 0.0 : v);
    }
}

/* Everything the rasterizer needs for one polygon, resolved once up front. */
typedef struct {
    const uint16_t *ramp_r, *ramp_g, *ramp_b;   /* already offset by colour */
    const uint8_t  *lumaram;
    uint32_t        lumabase;
    uint32_t        poly_luma;

    bool            textured;
    const uint32_t *sheet;
    uint32_t        texx, texy;
    uint32_t        texwidth, texheight;
    uint8_t         wrapx, wrapy, mirrorx, mirrory;
} shading_t;

static uint32_t shade(const shading_t *sh, uint32_t luma)
{
    if (luma > 0x3F) luma = 0x3F;

    uint32_t r = s_gamma[sh->ramp_r[luma] & 0xFF];
    uint32_t g = s_gamma[sh->ramp_g[luma] & 0xFF];
    uint32_t b = s_gamma[sh->ramp_b[luma] & 0xFF];

    return (r << 16) | (g << 8) | b;
}

/*
 * One 4-bit texel. Texture sheets are addressed as 2048x1024 but stored as
 * 1024x2048, so the right half wraps into the lower half with the y bit
 * flipped. Two texels per byte, four per 16-bit unit, and the sheet is read as
 * 32-bit words.
 */
static uint32_t get_texel(uint32_t base_x, uint32_t base_y, int x, int y,
                          const uint32_t *sheet)
{
    int x2 = (int)base_x + x;
    int y2 = (int)base_y + y;

    if (x2 >= 1024) {
        x2 -= 1024;
        y2 ^= 1024;
    }

    uint32_t offset = (((uint32_t)y2 / 2) * 512) + ((uint32_t)x2 / 2);
    uint32_t texel = sheet[offset >> 1];

    if (offset & 1) texel >>= 16;
    if ((y & 1) == 0) texel >>= 8;
    if ((x & 1) == 0) texel >>= 4;

    return texel & 0x0F;
}

/* Apply the header's wrap/mirror rules to a texture coordinate. */
static int wrap_coord(int c, uint32_t size, uint8_t wrap, uint8_t mirror)
{
    if (mirror) {
        uint32_t period = size * 2;
        uint32_t m = (uint32_t)c & (period - 1);
        return (int)(m < size ? m : period - 1 - m);
    }
    if (wrap)
        return (int)((uint32_t)c & (size - 1));

    if (c < 0) return 0;
    if ((uint32_t)c >= size) return (int)size - 1;
    return c;
}

/*
 * Flat or textured triangle fill.
 *
 * ponytail: point sampling, no bilinear filter, no mipmaps and no
 * microtexture. Texture coordinates are perspective correct - u/z, v/z and 1/z
 * interpolate linearly in screen space, which is what the geometry stage
 * already prepared them for - so the mapping itself is right; what is missing
 * is only the filtering MAME applies on top.
 */
static void fill_triangle(const vertex_t *a, const vertex_t *b, const vertex_t *c,
                          const shading_t *sh, const int16_t *viewport)
{
    uint32_t *fb = s_destmap;
    if (!fb) return;

    int min_x = (int)floorf(fminf(fminf(a->x, b->x), c->x));
    int max_x = (int)ceilf (fmaxf(fmaxf(a->x, b->x), c->x));
    int min_y = (int)floorf(fminf(fminf(a->y, b->y), c->y));
    int max_y = (int)ceilf (fmaxf(fmaxf(a->y, b->y), c->y));

    /* The viewport is in the projected space, y measured from the bottom. */
    int xoff, yoff;
    video_get_crtc_offsets(&xoff, &yoff);

    int vx0 = viewport[0] + xoff, vx1 = viewport[2] + xoff;
    int vy0 = (384 - viewport[3]) + yoff, vy1 = (384 - viewport[1]) + yoff;
    if (vx1 <= vx0 || vy1 <= vy0) { vx0 = 0; vy0 = 0; vx1 = FB_WIDTH; vy1 = FB_HEIGHT; }

    if (min_x < vx0) min_x = vx0;
    if (min_y < vy0) min_y = vy0;
    if (max_x > vx1) max_x = vx1;
    if (max_y > vy1) max_y = vy1;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x > FB_WIDTH)  max_x = FB_WIDTH;
    if (max_y > FB_HEIGHT) max_y = FB_HEIGHT;
    if (min_x >= max_x || min_y >= max_y) return;

    float ax = a->x, ay = a->y, bx = b->x, by = b->y, cx = c->x, cy = c->y;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (!(fabsf(area) > 1e-6f)) return;
    float inv_area = 1.0f / area;

    /* Untextured polygons are one colour for the whole surface. */
    uint32_t flat = sh->textured ? 0 : shade(sh, sh->poly_luma >> 2);

    for (int y = min_y; y < max_y; y++) {
        float py = (float)y + 0.5f;
        uint32_t *row = fb + (size_t)y * FB_STRIDE;

        for (int x = min_x; x < max_x; x++) {
            float px = (float)x + 0.5f;

            /* Barycentric weights: w1 and w2 for b and c, the rest for a. */
            float w1 = ((px - ax) * (cy - ay) - (py - ay) * (cx - ax)) * inv_area;
            float w2 = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) * inv_area;

            if (w1 < 0.0f || w2 < 0.0f || (w1 + w2) > 1.0f)
                continue;

            if (row[x])
                continue;       /* already covered by nearer geometry */

            if (!sh->textured) {
                    row[x] = flat | 0xFF000000u;
                continue;
            }

            float w0 = 1.0f - w1 - w2;

            /* These three are affine in screen space, which is what makes the
             * texture mapping perspective correct. */
            float ooz = w0 * a->pz + w1 * b->pz + w2 * c->pz;
            float uoz = w0 * a->pu + w1 * b->pu + w2 * c->pu;
            float voz = w0 * a->pv + w1 * b->pv + w2 * c->pv;

            if (ooz <= 0.0f)
                continue;

            float z = 1.0f / ooz;
            int u = (int)(uoz * z);
            int v = (int)(voz * z);

            u = wrap_coord(u, sh->texwidth,  sh->wrapx, sh->mirrorx);
            v = wrap_coord(v, sh->texheight, sh->wrapy, sh->mirrory);

            uint32_t texel = get_texel(sh->texx, sh->texy, u, v, sh->sheet);

            /* The texel picks an entry in the luma translation window, scaled
             * by the polygon's own luma. */
            uint32_t t = texel << 4;
            uint32_t luma = (uint32_t)sh->lumaram[(sh->lumabase + (t >> 1)) & 0x7FFF]
                          * sh->poly_luma / 256;

            row[x] = shade(sh, luma) | 0xFF000000u;
        }
    }
}

/* Resolve a polygon's texture header into everything the fill loop needs. */
static void setup_shading(const polygon_t *poly, shading_t *sh)
{
    const uint16_t *palram = video_get_palram();
    const uint16_t *xlat = video_get_colorxlat();

    /* bit 14 selects textured, bit 13 translucent. */
    uint32_t renderer = (poly->texheader[0] >> 13) & 3;

    uint32_t colorbase = (poly->texheader[3] >> 6) & 0x3FF;
    uint32_t colour = palram[(colorbase + 0x1000) & 0x1FFF] & 0x7FFF;

    sh->ramp_r = xlat + COLORXLAT_R + (((colour >> 0)  & 0x1F) << 8);
    sh->ramp_g = xlat + COLORXLAT_G + (((colour >> 5)  & 0x1F) << 8);
    sh->ramp_b = xlat + COLORXLAT_B + (((colour >> 10) & 0x1F) << 8);

    sh->lumaram   = video_get_lumaram();
    sh->lumabase  = (poly->texheader[1] & 0xFF) << 7;
    sh->poly_luma = poly->luma;
    sh->textured  = (renderer & 2) != 0;

    if (!sh->textured)
        return;

    sh->mirrorx = (poly->texheader[0] >> 8) & 1;
    sh->mirrory = (poly->texheader[0] >> 9) & 1;
    /* Smooth wrapping is disabled when mirroring is on. */
    sh->wrapx = ((poly->texheader[0] >> 6) & 1) & ~sh->mirrorx;
    sh->wrapy = ((poly->texheader[0] >> 7) & 1) & ~sh->mirrory;

    sh->sheet = video_get_texture_ram((poly->texheader[2] & 0x1000) ? 1 : 0);

    sh->texwidth  = 32u << ((poly->texheader[0] >> 0) & 0x7);
    sh->texheight = 32u << ((poly->texheader[0] >> 3) & 0x7);
    sh->texx = 32u * ((poly->texheader[2] >> 0) & 0x3F);
    sh->texy = 32u * ((poly->texheader[2] >> 6) & 0x1F);
}

void geo_render_polygons(void)
{
    raster_state_t *raster = s_raster;
    if (!raster || !s_destmap)
        return;

    /* The game submits a display list every other field. Rendering is
     * destructive - projection rewrites each vertex in place - so a field with
     * no new list keeps the bitmap it already has, as MAME's render_polygons
     * does when m_render_done is still set. */
    if (s_render_done)
        return;

    memset(s_destmap, 0, (size_t)FB_STRIDE * FB_HEIGHT * sizeof(uint32_t));

    if (raster->poly_list_index == 0)
        return;

    for (int window = raster->cur_window; window >= 0; window--) {
        for (int32_t z = raster->min_z; z <= raster->max_z; z++) {
            polygon_t *poly = raster->poly_sorted_list[z];

            while (poly) {
                if (poly->window == window) {
                    shading_t sh;
                    memset(&sh, 0, sizeof(sh));
                    setup_shading(poly, &sh);

                    model2_3d_project(poly);

                    /* Textured polygons interpolate u/z, v/z and 1/z. The
                     * eighth is the hardware's fixed texture coordinate scale
                     * (model2_3d_render). */
                    if (sh.textured) {
                        for (int i = 0; i < poly->num_vertices; i++) {
                            poly->v[i].pz = 1.0f / (poly->v[i].pz + FLT_MIN);
                            poly->v[i].pu = poly->v[i].pu * poly->v[i].pz * (1.0f / 8.0f);
                            poly->v[i].pv = poly->v[i].pv * poly->v[i].pz * (1.0f / 8.0f);
                        }
                    }

                    for (int i = 1; i + 1 < poly->num_vertices; i++)
                        fill_triangle(&poly->v[0], &poly->v[i], &poly->v[i + 1],
                                      &sh, poly->viewport);
                }
                poly = poly->next;
            }
        }
    }

    s_render_done = true;
}

/* ---- Lifecycle ---- */

void geo_init(void)
{
    s_raster = (raster_state_t *)calloc(1, sizeof(raster_state_t));
    s_geo    = (geo_state_t *)calloc(1, sizeof(geo_state_t));
    if (!s_raster || !s_geo) {
        fprintf(stderr, "[geo] Out of memory\n");
        return;
    }

    s_destmap = (uint32_t *)calloc((size_t)FB_STRIDE * FB_HEIGHT, sizeof(uint32_t));
    s_raster->poly_list = (polygon_t *)calloc(MAX_POLYGONS, sizeof(polygon_t));
    if (!s_raster->poly_list || !s_destmap) {
        fprintf(stderr, "[geo] Out of memory for the polygon list\n");
        return;
    }

    /* The ROMs are loaded after init, so geo_bind_roms picks them up on the
     * first parse rather than here. */
    build_gamma_table();
    s_raster->master_z_clip = 0xFF;   /* z-clip disabled until the game sets it */

    printf("[geo] Geometry engine initialized\n");
}

void geo_shutdown(void)
{
    if (s_raster) free(s_raster->poly_list);
    free(s_destmap); s_destmap = NULL;
    free(s_raster);  s_raster = NULL;
    free(s_geo);     s_geo = NULL;
}

/* The rendered 3D bitmap, 512 pixels per row, 0 where nothing was drawn. */
const uint32_t *geo_get_destmap(void)
{
    return s_destmap;
}

void geo_set_master_z_clip(uint32_t data)
{
    if (s_raster)
        s_raster->master_z_clip = (uint8_t)data;
}
