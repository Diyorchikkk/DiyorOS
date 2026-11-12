/*
 * ata.c — ATA (IDE) qattiq disk drayveri, PIO rejimi.
 *
 * Disk 512 baytlik SEKTORlardan iborat. Har bir sektorning raqami bor —
 * LBA (Logical Block Address): 0, 1, 2, ... Disk bilan 0x1F0-0x1F7 portlar
 * orqali gaplashamiz:
 *
 *   0x1F0  ma'lumot (16-bit, bitta sektor = 256 ta so'z)
 *   0x1F2  nechta sektor
 *   0x1F3-0x1F5  LBA ning 0-23 bitlari
 *   0x1F6  disk tanlash + LBA ning 24-27 bitlari
 *   0x1F7  buyruq (yozishda) / holat (o'qishda): BSY, DRQ, ERR bitlari
 *
 * PIO (Programmed I/O): protsessor har bir so'zni o'zi portdan o'qiydi.
 * Sodda, lekin sekin — zamonaviy drayverlar DMA ishlatadi (disk xotiraga
 * o'zi yozadi). Biz holat registrini kutib turamiz (polling), uzilish yo'q.
 */
#include "ata.h"
#include "io.h"
#include "irq.h"
#include "string.h"

#define ATA_DATA      0x1F0
#define ATA_ERROR     0x1F1
#define ATA_COUNT     0x1F2
#define ATA_LBA0      0x1F3
#define ATA_LBA1      0x1F4
#define ATA_LBA2      0x1F5
#define ATA_DRIVE     0x1F6
#define ATA_CMD       0x1F7      /* yozish: buyruq */
#define ATA_STATUS    0x1F7      /* o'qish: holat */
#define ATA_CONTROL   0x3F6

#define ST_ERR   0x01            /* xato */
#define ST_DRQ   0x08            /* ma'lumot tayyor (o'qish/yozish mumkin) */
#define ST_DF    0x20            /* disk nosozligi */
#define ST_BSY   0x80            /* disk band */

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENTIFY 0xEC

#define TIMEOUT 1000000

static ata_info_t info;

static inline void insw(uint16_t port, void *buf, uint32_t count)
{
    __asm__ volatile ("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buf, uint32_t count)
{
    __asm__ volatile ("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}

/* Diskka 400 ns vaqt berish (holat registrini 4 marta o'qish) */
static void delay400(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA_CONTROL);
}

static bool wait_not_busy(void)
{
    for (int i = 0; i < TIMEOUT; i++)
        if (!(inb(ATA_STATUS) & ST_BSY))
            return true;
    return false;
}

/* Ma'lumot tayyor bo'lguncha kutish (yoki xato) */
static bool wait_drq(void)
{
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_BSY)
            continue;
        if (s & (ST_ERR | ST_DF))
            return false;
        if (s & ST_DRQ)
            return true;
    }
    return false;
}

/* IDENTIFY javobidagi satrlar har 2 baytda almashtirilgan bo'ladi */
static void ata_string(char *dst, const uint16_t *src, int words)
{
    for (int i = 0; i < words; i++) {
        dst[i * 2]     = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    dst[words * 2] = 0;
    for (int i = words * 2 - 1; i >= 0 && dst[i] == ' '; i--)   /* oxirgi bo'sh joylar */
        dst[i] = 0;
}

bool ata_init(void)
{
    memset(&info, 0, sizeof(info));
    outb(ATA_CONTROL, 0x02);              /* nIEN: disk uzilishlarini o'chirish (polling) */

    outb(ATA_DRIVE, 0xA0);                /* birlamchi kanal, master disk */
    delay400();
    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, CMD_IDENTIFY);

    uint8_t s = inb(ATA_STATUS);
    if (s == 0 || s == 0xFF)
        return false;                     /* disk yo'q */
    if (!wait_not_busy())
        return false;
    if (inb(ATA_LBA1) || inb(ATA_LBA2))
        return false;                     /* ATA emas (masalan, CD-ROM) */
    if (!wait_drq())
        return false;

    uint16_t id[256];
    insw(ATA_DATA, id, 256);

    ata_string(info.serial, id + 10, 10);
    ata_string(info.model, id + 27, 20);
    info.sectors = id[60] | ((uint32_t)id[61] << 16);   /* LBA28 sektorlar soni */
    info.present = info.sectors > 0;
    return info.present;
}

const ata_info_t *ata_info(void)
{
    return &info;
}

/* Disk buyrug'ini yuborish: qaysi sektordan, nechta, nima qilish */
static bool issue(uint32_t lba, uint8_t count, uint8_t cmd)
{
    if (!wait_not_busy())
        return false;
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* LBA rejimi, master */
    outb(ATA_COUNT, count);
    outb(ATA_LBA0, (uint8_t)lba);
    outb(ATA_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_LBA2, (uint8_t)(lba >> 16));
    outb(ATA_CMD, cmd);
    delay400();                           /* disk holat registrini yangilashga ulgursin */
    return true;
}

bool ata_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!info.present || lba + count > info.sectors || lba + count < lba)
        return false;

    uint32_t f = irq_save();              /* bir vaqtda faqat bitta vazifa diskka murojaat qiladi */
    uint8_t *p = buf;
    bool ok = true;
    while (count && ok) {
        uint32_t n = count > 255 ? 255 : count;
        ok = issue(lba, (uint8_t)n, CMD_READ);
        for (uint32_t i = 0; i < n && ok; i++) {
            ok = wait_drq();
            if (ok) {
                insw(ATA_DATA, p, 256);
                p += SECTOR_SIZE;
            }
        }
        lba += n;
        count -= n;
        info.reads += n;
    }
    if (!ok)
        info.errors++;
    irq_restore(f);
    return ok;
}

bool ata_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!info.present || lba + count > info.sectors || lba + count < lba)
        return false;

    uint32_t f = irq_save();
    const uint8_t *p = buf;
    bool ok = true;
    while (count && ok) {
        uint32_t n = count > 255 ? 255 : count;
        ok = issue(lba, (uint8_t)n, CMD_WRITE);
        for (uint32_t i = 0; i < n && ok; i++) {
            ok = wait_drq();
            if (ok) {
                outsw(ATA_DATA, p, 256);
                p += SECTOR_SIZE;
            }
        }
        lba += n;
        count -= n;
        info.writes += n;
    }
    if (ok)
        ok = wait_not_busy() && !(inb(ATA_STATUS) & (ST_ERR | ST_DF));   /* oxirgi sektor yozib bo'lindimi */
    if (ok) {                             /* disk keshidagi ma'lumotni plastinaga yozish */
        outb(ATA_CMD, CMD_FLUSH);
        delay400();
        ok = wait_not_busy() && !(inb(ATA_STATUS) & (ST_ERR | ST_DF));
    }
    if (!ok)
        info.errors++;
    irq_restore(f);
    return ok;
}
