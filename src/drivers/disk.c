/*
 * disk.c — blok qurilma qatlami (v0.9).
 *
 * Fayl tizimi "disk_read(lba, count, buf)" deydi — qaysi drayver bajarishi
 * shu yerda yuklanishda bir marta tanlanadi. Bu "abstraksiya qatlami": yangi
 * disk turi (masalan, NVMe yoki USB) qo'shilsa, fat32.c o'zgarmaydi.
 *
 * Tartib: avval AHCI (zamonaviy SATA), topilmasa — eski IDE (ATA).
 */
#include "disk.h"
#include "ata.h"

static enum { NONE, ATA, AHCI } backend = NONE;
static disk_info_t none;

bool disk_init(void)
{
    if (ahci_init())
        backend = AHCI;
    else if (ata_init())
        backend = ATA;
    else
        backend = NONE;
    return backend != NONE;
}

const disk_info_t *disk_info(void)
{
    if (backend == AHCI)
        return ahci_info();
    if (backend == ATA)
        return ata_info();
    return &none;
}

bool disk_read(uint32_t lba, uint32_t count, void *buf)
{
    if (backend == AHCI)
        return ahci_read(lba, count, buf);
    if (backend == ATA)
        return ata_read(lba, count, buf);
    return false;
}

bool disk_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (backend == AHCI)
        return ahci_write(lba, count, buf);
    if (backend == ATA)
        return ata_write(lba, count, buf);
    return false;
}
