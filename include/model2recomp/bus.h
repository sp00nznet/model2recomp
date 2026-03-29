/*
 * Model 2 memory bus.
 *
 * 32-bit address space routed to hardware:
 *
 *   0x00000000-0x001FFFFF  Program ROM (2MB)
 *   0x00200000-0x0021FFFF  Program RAM (128KB, Model 2 original)
 *   0x00220000-0x0023FFFF  Program ROM extension (128KB, Model 2 original)
 *   0x00500000-0x005FFFFF  Work RAM (1MB)
 *   0x00800000-0x00803FFF  Geometry engine
 *   0x00804000-0x00807FFF  Geo program RAM
 *   0x00880000-0x00883FFF  Copro function port
 *   0x00884000-0x00887FFF  Copro FIFO
 *   0x00900000-0x0091FFFF  Buffer RAM (128KB, mirrored)
 *   0x00980000-0x0098003F  System control (copro, geo, video, FIFO, TGP ID)
 *   0x00E00000-0x00E00037  CPU control (wait states)
 *   0x00E80000-0x00E80007  IRQ request/ack/enable
 *   0x00F00000-0x00F0000F  Timers (4x, count down at 25 MHz)
 *   0x01000000-0x010FFFFF  System 24 tile/char RAM
 *   0x01800000-0x0181BFFF  Palette + color translate
 *   0x0181C000-0x0181C003  3D Z clip
 *   0x01A00000-0x01A04FFF  Comm board
 *   0x01C00000-0x01C00FFF  DPRAM (I/O board)
 *   0x01C80000-0x01C80003  UART (sound communication)
 *   0x01D00000-0x01D03FFF  Backup SRAM (16KB)
 *   0x02000000-0x03FFFFFF  Main data ROM (32MB)
 *   0x06000000-0x06FFFFFF  Extra data ROM (16MB)
 *   0x10000000-0x101FFFFF  Render mode
 *   0x10400000-0x105FFFFF  Polygon count
 *   0x11600000-0x116FFFFF  Framebuffer A/B
 *   0x12000000-0x125FFFFF  Texture RAM 0/1
 *   0x12800000-0x1281FFFF  Luma RAM
 */

#ifndef MODEL2RECOMP_BUS_H
#define MODEL2RECOMP_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Bus read/write (32-bit flat address space) --- */
uint8_t  bus_read8(uint32_t addr);
uint16_t bus_read16(uint32_t addr);
uint32_t bus_read32(uint32_t addr);

void bus_write8(uint32_t addr, uint8_t val);
void bus_write16(uint32_t addr, uint16_t val);
void bus_write32(uint32_t addr, uint32_t val);

/* --- Direct Work RAM access (faster for stack/locals) --- */
/* Work RAM is 1MB at 0x00500000-0x005FFFFF */
uint8_t  bus_workram_read8(uint32_t offset);
uint16_t bus_workram_read16(uint32_t offset);
uint32_t bus_workram_read32(uint32_t offset);
void     bus_workram_write8(uint32_t offset, uint8_t val);
void     bus_workram_write16(uint32_t offset, uint16_t val);
void     bus_workram_write32(uint32_t offset, uint32_t val);
uint8_t *bus_get_workram(void);

/* --- Direct Program RAM access --- */
/* Program RAM is 128KB at 0x00200000-0x0021FFFF */
uint8_t *bus_get_program_ram(void);

/* --- ROM access --- */
const uint8_t *bus_get_program_rom(uint32_t *size_out);
const uint8_t *bus_get_data_rom(uint32_t *size_out);

/* --- Buffer RAM access --- */
/* Buffer RAM is 128KB at 0x00900000-0x0091FFFF */
uint8_t *bus_get_buffer_ram(void);

/* --- Backup SRAM access --- */
/* Backup SRAM is 16KB at 0x01D00000-0x01D03FFF */
uint8_t *bus_get_backup_sram(void);

/* --- VBlank callback --- */
typedef void (*bus_vblank_callback_t)(void);
void bus_set_vblank_callback(bus_vblank_callback_t cb);

/* --- Initialize bus (called by model2recomp_init) --- */
void bus_init(void);
void bus_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_BUS_H */
