/*
 * ahci.c — AHCI (SATA) disk drayveri (v0.9).
 *
 * Eski IDE kontrolleri (ata.c) bilan protsessor har bir 2 baytni o'zi
 * portdan o'qiydi (PIO). Zamonaviy kompyuterlarda IDE umuman yo'q: SATA
 * disklar AHCI kontrolleri orqali ishlaydi va ma'lumotni xotiraga O'ZI
 * ko'chiradi (DMA — Direct Memory Access). Protsessor faqat "nima qilish
 * kerak"ni yozib qo'yadi.
 *
 * Qanday ishlaydi:
 *   1. PCI dan kontrollerni topamiz (klass 01:06:01). Uning registrlari
 *      xotirada (MMIO) — BAR5 manzilida. Ularni kernel xotirasiga ulaymiz.
 *   2. Kontrollerda 32 tagacha "port" (har biriga bitta SATA disk). Har bir
 *      port uchun xotirada uchta tuzilma tayyorlaymiz:
 *        - buyruqlar ro'yxati (command list): 32 ta buyruq sarlavhasi;
 *        - qabul qilingan FIS lar: disk javoblarini kontroller shu yerga yozadi;
 *        - buyruq jadvali (command table): buyruqning o'zi (FIS) va
 *          PRDT — "ma'lumotni xotiraning qayeriga ko'chirish" ro'yxati.
 *   3. Buyruq berish: jadvalga READ DMA EXT (yoki WRITE) yozamiz, portning
 *      CI registrida 0-bitni yoqamiz. Kontroller bajarib, bitni o'chiradi.
 *      Biz shu bitni kutamiz (polling; uzilishlar ishlatilmaydi).
 *
 * DMA faqat FIZIK manzillarni tushunadi. Kernel bufferlari (heap) virtual
 * manzilda va fizik jihatdan uzluksiz emas, shuning uchun 64 KB lik
 * "oraliq bufer" (bounce buffer) ishlatiladi: 16 ta alohida fizik sahifa,
 * PRDT ning 16 ta yozuvi. Ma'lumot shu bufer orqali ko'chiriladi.
 */
#include "disk.h"
#include "pci.h"
#include "memory.h"
#include "string.h"
#include "klog.h"
#include "sync.h"
#include "io.h"
#include "irq.h"

/* ---- Kontroller (HBA) registrlari ---- */
#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_PI      0x0C     /* qaysi portlar mavjud (bitlar) */
#define HBA_VS      0x10
#define HBA_CAP2    0x24
#define HBA_BOHC    0x28     /* BIOS/OS egalik almashuvi */
#define GHC_AE      (1u << 31)

/* ---- Port registrlari (0x100 + port * 0x80) ---- */
#define PX_CLB      0x00     /* buyruqlar ro'yxati manzili */
#define PX_CLBU     0x04
#define PX_FB       0x08     /* qabul qilingan FIS lar manzili */
#define PX_FBU      0x0C
#define PX_IS       0x10     /* uzilish holati (xatolarni ko'rish uchun) */
#define PX_IE       0x14
#define PX_CMD      0x18
#define PX_TFD      0x20     /* diskning holat registri (BSY, DRQ, ERR) */
#define PX_SIG      0x24     /* qurilma turi */
#define PX_SSTS     0x28     /* SATA ulanish holati */
#define PX_SERR     0x30
#define PX_CI       0x38     /* buyruq berish: bit N — N-buyruq bajarilsin */

#define CMD_ST      (1u << 0)
#define CMD_SUD     (1u << 1)     /* diskni aylantirish (staggered spin-up) */
#define CMD_ICC_ACTIVE (1u << 28) /* ulanishni "faol" holatga o'tkazish */
#define CMD_FRE     (1u << 4)
#define CMD_FR      (1u << 14)
#define CMD_CR      (1u << 15)
#define TFD_BSY     0x80
#define TFD_DRQ     0x08
#define TFD_ERR     0x01
#define IS_TFES     (1u << 30)     /* disk xato qaytardi */

#define SIG_SATA    0x00000101     /* oddiy disk (CD-ROM — 0xEB140101) */

#define FIS_H2D     0x27           /* xostdan qurilmaga registrlar FIS i */
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_IDENTIFY      0xEC
#define ATA_FLUSH_EXT     0xEA

#define BOUNCE_PAGES 16                          /* 64 KB = 128 sektor bir buyruqda */
#define MAX_SECTORS  (BOUNCE_PAGES * PAGE_SIZE / SECTOR_SIZE)
#define SPIN         2000000                     /* kutish chegarasi (taxminan 1-2 s) */

typedef struct {
    uint8_t  flags;          /* 0-4 bit: FIS uzunligi (dword), 6-bit: yozish */
    uint8_t  flags2;
    uint16_t prdtl;          /* PRDT yozuvlari soni */
    volatile uint32_t prdbc; /* ko'chirilgan baytlar (kontroller yozadi) */
    uint32_t ctba, ctbau;    /* buyruq jadvali manzili */
    uint32_t reserved[4];
} __attribute__((packed)) cmd_header_t;

typedef struct {
    uint32_t dba, dbau;      /* ma'lumot bufferi manzili */
    uint32_t reserved;
    uint32_t dbc;            /* bayt soni - 1 */
} __attribute__((packed)) prd_t;

typedef struct {
    uint8_t  cfis[64];       /* buyruq FIS i */
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    prd_t    prdt[BOUNCE_PAGES];
} __attribute__((packed)) cmd_table_t;

static volatile uint8_t *hba;    /* kontroller registrlari (MMIO) */
static volatile uint8_t *port;   /* tanlangan port registrlari */
static int               port_no = -1;
static cmd_header_t     *cmd_list;
static cmd_table_t      *cmd_table;
static uint8_t          *bounce[BOUNCE_PAGES];
static disk_info_t       info;
static mutex_t           lock;

static inline uint32_t hr(uint32_t off)              { return *(volatile uint32_t *)(hba + off); }
static inline void     hw(uint32_t off, uint32_t v)  { *(volatile uint32_t *)(hba + off) = v; }
static inline uint32_t pr(uint32_t off)              { return *(volatile uint32_t *)(port + off); }
static inline void     pw(uint32_t off, uint32_t v)  { *(volatile uint32_t *)(port + off) = v; }

/* Bitlar kerakli holatga kelguncha kutish (mask bitlari value ga teng bo'lguncha) */
static bool wait_port(uint32_t reg, uint32_t mask, uint32_t value)
{
    for (int i = 0; i < SPIN; i++) {
        if ((pr(reg) & mask) == value)
            return true;
        inb(0x80);                               /* ~1 mikrosekund */
    }
    return false;
}

/* Portni to'xtatish: buyruqlar va FIS qabul qilish o'chadi (sozlashdan oldin shart) */
static bool port_stop(void)
{
    pw(PX_CMD, pr(PX_CMD) & ~(CMD_ST | CMD_FRE));
    return wait_port(PX_CMD, CMD_CR | CMD_FR, 0);
}

static void port_start(void)
{
    wait_port(PX_CMD, CMD_CR, 0);
    pw(PX_CMD, pr(PX_CMD) | CMD_FRE);
    pw(PX_CMD, pr(PX_CMD) | CMD_ST);
}

/* Fizik sahifa olish (identity-map hududida: virtual = fizik), nollab */
static void *dma_page(void)
{
    uint32_t p = pmm_alloc_frame();
    if (p)
        memset((void *)p, 0, PAGE_SIZE);
    return (void *)p;
}

static void ata_string(char *dst, const uint16_t *src, int words)
{
    for (int i = 0; i < words; i++) {
        dst[i * 2]     = (char)(src[i] >> 8);
        dst[i * 2 + 1] = (char)(src[i] & 0xFF);
    }
    dst[words * 2] = 0;
    for (int i = words * 2 - 1; i >= 0 && dst[i] == ' '; i--)
        dst[i] = 0;
}

/*
 * Bitta buyruqni bajarish (0-slot). sectors — nechta sektor (bayt = sectors * 512),
 * ma'lumot bounce buferda. true — muvaffaqiyatli.
 */
static bool issue(uint8_t command, uint64_t lba, uint32_t sectors, bool write)
{
    uint32_t bytes = sectors * SECTOR_SIZE;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    memset(cmd_table, 0, sizeof(*cmd_table));
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t chunk = bytes - i * PAGE_SIZE;
        if (chunk > PAGE_SIZE)
            chunk = PAGE_SIZE;
        cmd_table->prdt[i].dba = (uint32_t)bounce[i];
        cmd_table->prdt[i].dbc = chunk - 1;
    }

    uint8_t *f = cmd_table->cfis;                 /* Register H2D FIS */
    f[0] = FIS_H2D;
    f[1] = 0x80;                                  /* bu — buyruq (registrlarni yangilash emas) */
    f[2] = command;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 1 << 6;                                /* LBA rejimi */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)sectors;
    f[13] = (uint8_t)(sectors >> 8);

    cmd_header_t *h = &cmd_list[0];
    h->flags = 5 | (write ? (1 << 6) : 0);        /* FIS = 5 dword (20 bayt) */
    h->flags2 = 0;
    h->prdtl = (uint16_t)pages;
    h->prdbc = 0;
    h->ctba = (uint32_t)cmd_table;
    h->ctbau = 0;

    if (!wait_port(PX_TFD, TFD_BSY | TFD_DRQ, 0))  /* disk band bo'lsa — kutamiz */
        return false;
    pw(PX_IS, 0xFFFFFFFF);                        /* eski holatlarni tozalash */
    pw(PX_CI, 1);                                 /* bajar! */

    for (int i = 0; i < SPIN; i++) {
        if (pr(PX_IS) & IS_TFES)
            return false;                         /* disk xato qaytardi */
        if (!(pr(PX_CI) & 1))
            return !(pr(PX_TFD) & TFD_ERR);       /* tugadi */
        inb(0x80);
    }
    return false;                                 /* javob kelmadi */
}

/* Kontrollerni BIOS/UEFI dan "olish" (ba'zi haqiqiy kompyuterlarda shart) */
static void bios_handoff(void)
{
    if (!(hr(HBA_CAP2) & 1))
        return;
    hw(HBA_BOHC, hr(HBA_BOHC) | (1u << 1));       /* OS egalikni so'raydi */
    for (int i = 0; i < SPIN && (hr(HBA_BOHC) & 1); i++)
        inb(0x80);
}

static bool setup_port(int n)
{
    port = hba + 0x100 + (uint32_t)n * 0x80;
    if (!port_stop())
        return false;
    cmd_list = dma_page();                        /* 1 KB kerak, 1 KB ga tekislangan */
    uint8_t *fis = dma_page();                    /* 256 bayt kerak */
    cmd_table = dma_page();                       /* 128 ga tekislangan */
    if (!cmd_list || !fis || !cmd_table)
        return false;
    for (int i = 0; i < BOUNCE_PAGES; i++)
        if (!(bounce[i] = dma_page()))
            return false;
    pw(PX_CLB, (uint32_t)cmd_list);
    pw(PX_CLBU, 0);
    pw(PX_FB, (uint32_t)fis);
    pw(PX_FBU, 0);
    pw(PX_SERR, 0xFFFFFFFF);
    pw(PX_IS, 0xFFFFFFFF);
    pw(PX_IE, 0);                                 /* uzilishlar kerak emas (polling) */
    port_start();
    return true;
}

bool ahci_init(void)
{
    memset(&info, 0, sizeof(info));
    const pci_device_t *d = 0;
    for (int i = 0; i < pci_count() && !d; i++) {
        const pci_device_t *c = pci_get(i);
        if (c->class_code == 0x01 && c->subclass == 0x06 && c->prog_if == 0x01)
            d = c;
    }
    if (!d || !(d->bar[5] & ~0xFu) || (d->bar[5] & 1))
        return false;                             /* AHCI kontrolleri yo'q */

    pci_enable_dma(d);
    hba = paging_map_mmio(d->bar[5] & ~0xFu, 0x1100);
    if (!hba)
        return false;
    bios_handoff();
    hw(HBA_GHC, hr(HBA_GHC) | GHC_AE);            /* AHCI rejimi */

    uint32_t pi = hr(HBA_PI);
    for (int n = 0; n < 32 && port_no < 0; n++) {
        if (!(pi & (1u << n)))
            continue;
        volatile uint8_t *p = hba + 0x100 + (uint32_t)n * 0x80;
        volatile uint32_t *cmd = (volatile uint32_t *)(p + PX_CMD);
        /* Haqiqiy kompyuterlarda disk "uxlab" yoki aylanmay turgan bo'lishi mumkin:
           uyg'otamiz va ulanish o'rnatilishini qisqa kutamiz (~10 ms) */
        *cmd |= CMD_SUD;
        *cmd = (*cmd & 0x0FFFFFFFu) | CMD_ICC_ACTIVE;
        uint32_t ssts = 0;
        for (int i = 0; i < 10000; i++) {
            ssts = *(volatile uint32_t *)(p + PX_SSTS);
            if ((ssts & 0x0F) == 3)
                break;
            inb(0x80);
        }
        uint32_t sig = *(volatile uint32_t *)(p + PX_SIG);
        if ((ssts & 0x0F) != 3)
            continue;                             /* qurilma ulanmagan */
        if (sig != SIG_SATA)
            continue;                             /* CD-ROM va boshqalar */
        if (setup_port(n))
            port_no = n;
    }
    if (port_no < 0) {
        klog("ahci: kontroller bor (%04x:%04x), lekin SATA disk topilmadi", d->vendor, d->device);
        return false;
    }

    /* IDENTIFY: disk modeli va hajmi */
    if (!issue(ATA_IDENTIFY, 0, 1, false)) {
        klog("ahci: port %d IDENTIFY ga javob bermadi", port_no);
        return false;
    }
    const uint16_t *id = (const uint16_t *)bounce[0];
    ata_string(info.serial, id + 10, 10);
    ata_string(info.model, id + 27, 20);
    uint64_t sectors = (id[83] & (1 << 10))       /* LBA48 qo'llab-quvvatlanadimi */
        ? ((uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32))
        : ((uint64_t)id[60] | ((uint64_t)id[61] << 16));
    info.sectors = sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)sectors;   /* 2 TB gacha */
    info.present = info.sectors > 0;
    info.driver = "AHCI (SATA, DMA)";
    mutex_init(&lock, "ahci", false);
    klog("ahci: %04x:%04x, AHCI %u.%u, port %d: %s, %u MB", d->vendor, d->device,
         hr(HBA_VS) >> 16, (hr(HBA_VS) >> 8) & 0xFF, port_no, info.model, info.sectors / 2048);
    return info.present;
}

const disk_info_t *ahci_info(void)
{
    return &info;
}

bool ahci_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!info.present || lba + count > info.sectors || lba + count < lba)
        return false;
    mutex_lock(&lock);
    bool ok = true;
    uint8_t *dst = buf;
    while (count && ok) {
        uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        ok = issue(ATA_READ_DMA_EXT, lba, n, false);
        if (ok) {
            for (uint32_t done = 0; done < n * SECTOR_SIZE; done += PAGE_SIZE) {
                uint32_t chunk = n * SECTOR_SIZE - done;
                memcpy(dst + done, bounce[done / PAGE_SIZE], chunk > PAGE_SIZE ? PAGE_SIZE : chunk);
            }
            info.reads += n;
            lba += n;
            count -= n;
            dst += n * SECTOR_SIZE;
        }
    }
    if (!ok)
        info.errors++;
    mutex_unlock(&lock);
    return ok;
}

bool ahci_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!info.present || lba + count > info.sectors || lba + count < lba)
        return false;
    mutex_lock(&lock);
    bool ok = true;
    const uint8_t *src = buf;
    while (count && ok) {
        uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        for (uint32_t done = 0; done < n * SECTOR_SIZE; done += PAGE_SIZE) {
            uint32_t chunk = n * SECTOR_SIZE - done;
            memcpy(bounce[done / PAGE_SIZE], src + done, chunk > PAGE_SIZE ? PAGE_SIZE : chunk);
        }
        ok = issue(ATA_WRITE_DMA_EXT, lba, n, true);
        if (ok) {
            info.writes += n;
            lba += n;
            count -= n;
            src += n * SECTOR_SIZE;
        }
    }
    if (ok)
        ok = issue(ATA_FLUSH_EXT, 0, 0, false);   /* diskning o'z keshidan yozib qo'ysin */
    if (!ok)
        info.errors++;
    mutex_unlock(&lock);
    return ok;
}
