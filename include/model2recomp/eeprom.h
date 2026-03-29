/*
 * EEPROM / backup SRAM interface.
 *
 * Original Model 2 uses battery-backed SRAM (16KB).
 * Model 2A+ uses serial EEPROM (93C46).
 */

#ifndef MODEL2RECOMP_EEPROM_H
#define MODEL2RECOMP_EEPROM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize EEPROM/SRAM subsystem */
void eeprom_init(void);
void eeprom_shutdown(void);

/* Load/save backup SRAM from/to file */
bool eeprom_load(const char *path);
bool eeprom_save(const char *path);

/* Backup SRAM read/write (0x01D00000-0x01D03FFF) */
uint32_t backup_sram_read(uint32_t offset);
void backup_sram_write(uint32_t offset, uint32_t data);

#ifdef __cplusplus
}
#endif

#endif /* MODEL2RECOMP_EEPROM_H */
