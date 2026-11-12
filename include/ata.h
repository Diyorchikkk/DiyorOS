/* ata.h — ATA (IDE) qattiq disk drayveri, PIO rejimi (v0.6) */
#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

#define SECTOR_SIZE 512

typedef struct {
    bool     present;
    char     model[41];       /* disk modeli (IDENTIFY dan) */
    char     serial[21];
    uint32_t sectors;         /* jami sektorlar (LBA28) */
    uint32_t reads, writes;   /* statistika: o'qilgan / yozilgan sektorlar */
    uint32_t errors;          /* muvaffaqiyatsiz amallar soni */
} ata_info_t;

bool              ata_init(void);
const ata_info_t *ata_info(void);
/* count ta sektorni o'qish / yozish. true — muvaffaqiyatli */
bool              ata_read(uint32_t lba, uint32_t count, void *buf);
bool              ata_write(uint32_t lba, uint32_t count, const void *buf);

#endif
