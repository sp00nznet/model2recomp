/*
 * Model 2 video subsystem.
 *
 * Two layers:
 *   1. 3D polygon renderer (geometry engine + rasterizer)
 *   2. System 24 tilemaps (4 layers, 2D overlay)
 *
 * The geometry engine receives commands via the geo FIFO from the i960.
 * The TGP coprocessor performs geometry transforms (matrix math, lighting).
 * The rasterizer draws textured polygons with Z-sort ordering.
 *
 * Resolution: 496x384 (Model 2 native)
 * Framebuffer: dual-banked 512x400 x 16bpp (xGGGGGRRRRRBBBBB)
 */

#ifndef MODEL2RECOMP_VIDEO_H
#define MODEL2RECOMP_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the video subsystem */
void video_init(void);
void video_shutdown(void);

/* --- Geometry engine interface --- */
/* Write to geometry engine (0x00800000-0x00803FFF) */
void geo_write(uint32_t offset, uint32_t data);
uint32_t geo_read(uint32_t offset);

/* Geo program RAM (0x00804000-0x00807FFF) */
void geo_prg_write(uint32_t data);
uint32_t geo_prg_read(uint32_t offset);

/* Byte offset in buffer RAM where the geometry command stream begins. */
uint32_t geo_read_start_address(void);

/* Consume the "a new display list has been published" flag. */
bool geo_take_list_ready(void);

/* CRT sync offsets, which position the 3D scene in the visible area. */
void video_get_crtc_offsets(int *x, int *y);

/* Direct access for the rasterizer. */
const uint16_t *video_get_palram(void);
const uint16_t *video_get_colorxlat(void);
const uint8_t  *video_get_lumaram(void);
const uint32_t *video_get_texture_ram(int bank);

/* --- Geometry engine / 3D rasterizer (geometry.c) --- */

void geo_init(void);
void geo_shutdown(void);

/* Walk one field's command stream out of buffer RAM, producing the polygon
 * list. Called at the field boundary, as the hardware does at VBlank. */
void geo_parse(void);

/* Project and rasterize the polygon list into the 3D bitmap. */
void geo_render_polygons(void);

/* The rendered 3D bitmap: 512 pixels per row, XRGB8888, 0 where nothing drawn. */
const uint32_t *geo_get_destmap(void);
const uint8_t *geo_get_gamma(void);

/* Polygons produced by the last geo_parse. */
uint32_t geo_polygon_count(void);

/* Master z-clip register (0xFF disables clipping). */
void geo_set_master_z_clip(uint32_t data);

/* Geo control */
void geo_ctl1_write(uint32_t data);

/* --- Coprocessor (TGP) interface --- */
/* Write function port (0x00880000) */

/* Copro FIFO (0x00884000) */

/* Copro control */

/* --- Rasterizer interface --- */
/* Render mode register (0x10000000) */
void render_mode_write(uint32_t data);
uint32_t render_mode_read(void);

/* Polygon count (0x10400000) */
uint32_t polygon_count_read(void);

/* Video control register (0x0098000C) */
void videoctl_write(uint32_t data);
uint32_t videoctl_read(void);

/* FIFO control (0x00980004) */
uint32_t fifo_control_read(void);

/* TGP identification (0x00980030) */
uint32_t tgpid_read(uint32_t offset);

/* --- Framebuffer access --- */
/* Framebuffer A (0x11600000-0x1167FFFF) - 512x400x16bpp */
uint16_t fbvram_bankA_read(uint32_t offset);
void fbvram_bankA_write(uint32_t offset, uint16_t data);

/* Framebuffer B (0x11680000-0x116FFFFF) */
uint16_t fbvram_bankB_read(uint32_t offset);
void fbvram_bankB_write(uint32_t offset, uint16_t data);

/* --- Texture RAM --- */
/* Texture RAM 0 (0x12000000-0x121FFFFF, 2MB) */
void tex0_write(uint32_t offset, uint32_t data);
/* Texture RAM 1 (0x12400000-0x125FFFFF, 2MB) */
void tex1_write(uint32_t offset, uint32_t data);

/* --- System 24 tilemaps --- */
/* Tile RAM (0x01000000-0x0100FFFF) */
uint16_t tile_read(uint32_t offset);
void tile_write(uint32_t offset, uint16_t data);

/* Char RAM (0x01080000-0x010FFFFF) */
uint16_t char_read(uint32_t offset);
void char_write(uint32_t offset, uint16_t data);

/* Sync registers */
void tile_xhout_write(uint16_t data);
void tile_xvout_write(uint16_t data);

/* --- Palette --- */
/* Palette RAM (0x01800000-0x01803FFF) */
uint16_t palette_read(uint32_t offset);
void palette_write(uint32_t offset, uint16_t data);

/* Color translate RAM (0x01810000-0x0181BFFF) */
uint16_t colorxlat_read(uint32_t offset);
void colorxlat_write(uint32_t offset, uint16_t data);

/* 3D Z clip (0x0181C000) */
void zclip_write(uint32_t data);

/* --- Luma RAM --- */
/* Polygon luminance RAM (0x12800000-0x1281FFFF) */
uint8_t lumaram_read(uint32_t offset);
void lumaram_write(uint32_t offset, uint8_t data);

/* --- Rendering --- */
/* Render current frame to the output framebuffer (called by end_frame) */
void video_render_frame(void);

/* Get the rendered output (496x384 RGBX8888) */
const uint8_t *video_get_framebuffer(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_VIDEO_H */
