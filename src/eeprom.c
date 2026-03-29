/*
 * EEPROM / backup SRAM.
 *
 * Original Model 2 uses 16KB battery-backed SRAM.
 */

#include "model2recomp/eeprom.h"
#include <stdio.h>
#include <string.h>

static uint8_t s_sram[0x4000]; /* 16KB */

void eeprom_init(void)
{
    memset(s_sram, 0xFF, sizeof(s_sram));
    printf("[eeprom] SRAM initialized (16KB)\n");
}

void eeprom_shutdown(void)
{
    /* Nothing to clean up */
}

bool eeprom_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(s_sram, 1, sizeof(s_sram), f);
    fclose(f);
    printf("[eeprom] Loaded %zu bytes from %s\n", n, path);
    return true;
}

bool eeprom_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(s_sram, 1, sizeof(s_sram), f);
    fclose(f);
    printf("[eeprom] Saved to %s\n", path);
    return true;
}

uint32_t backup_sram_read(uint32_t offset)
{
    uint32_t byte_offset = offset * 4;
    if (byte_offset + 3 < sizeof(s_sram)) {
        return (uint32_t)s_sram[byte_offset]
             | ((uint32_t)s_sram[byte_offset + 1] << 8)
             | ((uint32_t)s_sram[byte_offset + 2] << 16)
             | ((uint32_t)s_sram[byte_offset + 3] << 24);
    }
    return 0xFFFFFFFF;
}

void backup_sram_write(uint32_t offset, uint32_t data)
{
    uint32_t byte_offset = offset * 4;
    if (byte_offset + 3 < sizeof(s_sram)) {
        s_sram[byte_offset]     = (uint8_t)(data);
        s_sram[byte_offset + 1] = (uint8_t)(data >> 8);
        s_sram[byte_offset + 2] = (uint8_t)(data >> 16);
        s_sram[byte_offset + 3] = (uint8_t)(data >> 24);
    }
}
