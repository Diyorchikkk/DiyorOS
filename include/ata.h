/* ata.h — ATA (IDE) qattiq disk drayveri, PIO rejimi (v0.6) */
#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>
#include "disk.h"

typedef disk_info_t ata_info_t;   /* v0.9: umumiy blok qurilma ma'lumoti (disk.h) */

bool              ata_init(void);
const ata_info_t *ata_info(void);
/* count ta sektorni o'qish / yozish. true — muvaffaqiyatli */
bool              ata_read(uint32_t lba, uint32_t count, void *buf);
bool              ata_write(uint32_t lba, uint32_t count, const void *buf);

#endif
