/*
 * Model 2 video subsystem - stub implementation.
 *
 * TODO: Port MAME's model2_v.cpp rasterizer and System 24 tilemap engine.
 * For now, provides memory-mapped hardware stubs so recompiled code can run.
 *
 * Reference: MAME model2_v.cpp, segaic24.cpp (BSD-3-Clause)
 */

#include "model2recomp/video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Video state */
static uint8_t *s_framebuffer = NULL;   /* 496x384 RGBX8888 output */
static uint16_t *s_fbvramA = NULL;      /* 512x400 x 16bpp bank A */
static uint16_t *s_fbvramB = NULL;      /* 512x400 x 16bpp bank B */
static uint32_t *s_textureram0 = NULL;  /* 2MB texture RAM 0 */
static uint32_t *s_textureram1 = NULL;  /* 2MB texture RAM 1 */
static uint8_t  *s_lumaram = NULL;      /* 128KB luma RAM */
static uint16_t *s_palram = NULL;       /* 16KB palette RAM */
static uint16_t *s_colorxlat = NULL;    /* 48KB color translate RAM */
static uint8_t  *s_tile_ram = NULL;     /* 64KB System 24 tile RAM */
static uint8_t  *s_char_ram = NULL;     /* 512KB System 24 char RAM */

static uint32_t s_render_mode = 0;
static uint32_t s_videoctl = 0;
static uint32_t s_zclip = 0;

/* Geometry engine state */
static uint32_t s_geo_ram[4096];        /* Geo program RAM (16KB) */
static uint32_t s_geo_write_pos = 0;
static uint32_t s_geo_read_pos = 0;

/* Coprocessor state */
static uint32_t s_copro_ctl = 0;
static uint32_t s_copro_cnt = 0;

/* FIFO */
#define FIFO_SIZE 256
static uint32_t s_copro_fifo[FIFO_SIZE];
static int s_fifo_head = 0;
static int s_fifo_tail = 0;

#define FB_WIDTH  496
#define FB_HEIGHT 384

void video_init(void)
{
    s_framebuffer  = (uint8_t *)calloc(1, FB_WIDTH * FB_HEIGHT * 4);
    s_fbvramA      = (uint16_t *)calloc(1, 512 * 400 * 2);
    s_fbvramB      = (uint16_t *)calloc(1, 512 * 400 * 2);
    s_textureram0  = (uint32_t *)calloc(1, 0x200000);  /* 2MB */
    s_textureram1  = (uint32_t *)calloc(1, 0x200000);  /* 2MB */
    s_lumaram      = (uint8_t *)calloc(1, 0x20000);    /* 128KB */
    s_palram       = (uint16_t *)calloc(1, 0x4000);    /* 16KB */
    s_colorxlat    = (uint16_t *)calloc(1, 0xC000);    /* 48KB */
    s_tile_ram     = (uint8_t *)calloc(1, 0x10000);    /* 64KB */
    s_char_ram     = (uint8_t *)calloc(1, 0x80000);    /* 512KB */

    memset(s_geo_ram, 0, sizeof(s_geo_ram));

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

/* --- Geometry engine --- */

void geo_write(uint32_t offset, uint32_t data)
{
    if (offset < 4096)
        s_geo_ram[offset] = data;
}

uint32_t geo_read(uint32_t offset)
{
    if (offset < 4096)
        return s_geo_ram[offset];
    return 0;
}

void geo_prg_write(uint32_t data)
{
    if (s_geo_write_pos < 4096)
        s_geo_ram[s_geo_write_pos++] = data;
}

uint32_t geo_prg_read(uint32_t offset)
{
    if (offset < 4096)
        return s_geo_ram[offset];
    return 0;
}

void geo_ctl1_write(uint32_t data)
{
    s_geo_write_pos = 0;
    /* TODO: reset geometry engine state */
}

/* --- Coprocessor (TGP) --- */

void copro_function_port_write(uint32_t data)
{
    /* TODO: dispatch TGP function */
    s_copro_cnt++;
}

void copro_fifo_write(uint32_t data)
{
    int next = (s_fifo_head + 1) % FIFO_SIZE;
    if (next != s_fifo_tail) {
        s_copro_fifo[s_fifo_head] = data;
        s_fifo_head = next;
    }
}

uint32_t copro_fifo_read(void)
{
    if (s_fifo_head == s_fifo_tail)
        return 0;
    uint32_t val = s_copro_fifo[s_fifo_tail];
    s_fifo_tail = (s_fifo_tail + 1) % FIFO_SIZE;
    return val;
}

void copro_ctl1_write(uint32_t data)
{
    s_copro_ctl = data;
    if (data & 0x80000000) {
        s_copro_cnt = 0; /* Reset copro counter */
    }
}

uint32_t copro_ctl1_read(void)
{
    return s_copro_ctl;
}

uint32_t copro_status_read(void)
{
    return (s_copro_cnt == 0) ? 0xFFFFFFFF : 0;
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
    return 0; /* TODO */
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
    /* Bit 0: FIFO full, Bit 1: FIFO empty */
    int count = (s_fifo_head - s_fifo_tail + FIFO_SIZE) % FIFO_SIZE;
    uint32_t status = 0;
    if (count == 0) status |= 2;           /* empty */
    if (count >= FIFO_SIZE - 1) status |= 1; /* full */
    return status;
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

void tex0_write(uint32_t offset, uint32_t data)
{
    if (offset < 0x200000 / 4) s_textureram0[offset] = data;
}

void tex1_write(uint32_t offset, uint32_t data)
{
    if (offset < 0x200000 / 4) s_textureram1[offset] = data;
}

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

void tile_xhout_write(uint16_t data) { /* TODO */ }
void tile_xvout_write(uint16_t data) { /* TODO */ }

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
}

/* --- Luma RAM --- */

uint8_t lumaram_read(uint32_t offset)
{
    if (offset < 0x20000) return s_lumaram[offset];
    return 0;
}

void lumaram_write(uint32_t offset, uint8_t data)
{
    if (offset < 0x20000) s_lumaram[offset] = data;
}

/* --- Rendering --- */

void video_render_frame(void)
{
    /* TODO: Port MAME's rasterizer (model2_v.cpp / model2rd.ipp)
     *
     * For now, convert framebuffer A from Model 2 16bpp to RGBX8888.
     * Format: xGGGGGRRRRRBBBBB (little-endian 16-bit)
     */
    if (!s_framebuffer || !s_fbvramA) return;

    for (int y = 0; y < FB_HEIGHT && y < 400; y++) {
        for (int x = 0; x < FB_WIDTH && x < 512; x++) {
            uint16_t pixel = s_fbvramA[y * 512 + x];
            uint8_t r = ((pixel >> 0)  & 0x1F) << 3;
            uint8_t g = ((pixel >> 5)  & 0x1F) << 3;
            uint8_t b = ((pixel >> 10) & 0x1F) << 3;

            int dst = (y * FB_WIDTH + x) * 4;
            s_framebuffer[dst + 0] = r;
            s_framebuffer[dst + 1] = g;
            s_framebuffer[dst + 2] = b;
            s_framebuffer[dst + 3] = 0xFF;
        }
    }
}

const uint8_t *video_get_framebuffer(void)
{
    return s_framebuffer;
}
