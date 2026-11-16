/*
 * fb.c — grafik rejim drayveri (v0.7).
 *
 * MATN REJIMI VA GRAFIK REJIM
 * v0.1 dan beri ekran matn rejimida edi: videokartaga "shu katakda shu
 * belgi" deymiz, harfni piksellarga videokartaning o'zi aylantiradi
 * (shrift uning xotirasida). 80x25 belgi, 16 rang.
 *
 * Grafik rejimda esa har bir PIKSELni o'zimiz boshqaramiz: 1024x768 ekranda
 * 786 432 piksel, har biri 4 bayt (ko'k, yashil, qizil, bo'sh). Bu xotira
 * "framebuffer" deyiladi: unga yozilgan son darhol ekranda rang bo'lib chiqadi.
 * Harflarni ham endi o'zimiz chizamiz (shrift kernel ichida, gfx/font.c).
 *
 * BOCHS VBE (BGA)
 * QEMU, Bochs va UTM dagi standart videokarta "Bochs Graphics Adapter"
 * interfeysini qo'llab-quvvatlaydi: ikki port (0x1CE indeks, 0x1CF qiymat)
 * orqali o'lcham va rang chuqurligini yozamiz, xolos. BIOS kerak emas.
 * Framebuffer ning fizik manzilini videokartaning PCI BAR0 registri aytadi.
 *
 * MATN REJIMIGA QAYTISH
 * Grafik rejimdan chiqqanda VGA registrlarini 80x25 matn rejimi qiymatlariga
 * qaytarish kerak (BIOS buni qilmaydi — biz protected mode dagimiz). Bundan
 * tashqari framebuffer videokarta xotirasining boshidan joylashadi va u
 * yerdagi SHRIFTni (2-tekislik) ustidan yozib yuboradi. Shuning uchun grafik
 * rejimga o'tishdan oldin shriftni o'qib saqlaymiz, qaytganda joyiga yozamiz.
 */
#include "fb.h"
#include "pci.h"
#include "io.h"
#include "memory.h"
#include "string.h"
#include "klog.h"
#include "irq.h"

/* --- Bochs VBE registrlari --- */
#define VBE_INDEX   0x01CE
#define VBE_DATA    0x01CF
#define VBE_ID      0
#define VBE_XRES    1
#define VBE_YRES    2
#define VBE_BPP     3
#define VBE_ENABLE  4
#define VBE_VIRT_W  6
#define VBE_VIRT_H  7
#define VBE_X_OFF   8
#define VBE_Y_OFF   9
#define VBE_VRAM_64K 0x0A
#define VBE_DISABLED    0x00
#define VBE_ENABLED     0x01
#define VBE_LFB_ENABLED 0x40

static fb_info_t info;
static uint8_t saved_font[256 * 32];    /* VGA shrifti: 256 belgi x 32 bayt (16 tasi ishlatiladi) */

static void vbe_write(uint16_t index, uint16_t value)
{
    outw(VBE_INDEX, index);
    outw(VBE_DATA, value);
}

static uint16_t vbe_read(uint16_t index)
{
    outw(VBE_INDEX, index);
    return inw(VBE_DATA);
}

bool fb_detect(void)
{
    memset(&info, 0, sizeof(info));
    uint16_t id = vbe_read(VBE_ID);
    if (id < 0xB0C0 || id > 0xB0CF)
        return false;                           /* Bochs VBE yo'q (masalan, haqiqiy kompyuter) */

    /* QEMU std-vga: 1234:1111. Ba'zi muhitlarda boshqa ID bo'lishi mumkin — VGA sinfini qidiramiz */
    const pci_device_t *d = pci_find(0x1234, 0x1111);
    for (int i = 0; !d && i < pci_count(); i++)
        if (pci_get(i)->class_code == 0x03)
            d = pci_get(i);
    if (!d || !(d->bar[0] & ~0xFu))
        return false;

    info.phys = d->bar[0] & ~0xFu;              /* pastki 4 bit — BAR turi, manzil emas */
    info.bga_version = id;
    info.vram_bytes = (id >= 0xB0C2) ? (uint32_t)vbe_read(VBE_VRAM_64K) * 65536u : 0;
    if (info.vram_bytes == 0 || info.vram_bytes > FB_MAX_SIZE)
        info.vram_bytes = FB_MAX_SIZE;
    info.available = true;
    klog("fb: Bochs VBE 0x%x, framebuffer 0x%08x, %u MB video xotira",
         id, info.phys, info.vram_bytes / (1024 * 1024));
    return true;
}

const fb_info_t *fb_info(void)
{
    return &info;
}

/* ----------------------- VGA shriftini saqlash / tiklash ----------------------- */

/*
 * Matn rejimida shrift VGA xotirasining 2-tekisligida (plane 2) turadi va
 * protsessorga ko'rinmaydi. Ko'rinadigan qilish uchun registrlarni vaqtincha
 * "tekis" rejimga o'tkazamiz: 2-tekislik 0xA0000 manzilida ochiladi.
 */
static void font_plane_open(void)
{
    outw(0x3C4, 0x0402);   /* sequencer: faqat 2-tekislikka yozish */
    outw(0x3C4, 0x0704);   /* sequencer: ketma-ket manzillash (odd/even o'chiq) */
    outw(0x3CE, 0x0204);   /* graphics: 2-tekislikdan o'qish */
    outw(0x3CE, 0x0005);   /* graphics: odd/even o'chiq */
    outw(0x3CE, 0x0406);   /* graphics: xotira 0xA0000 dan, 64 KB */
}

static void font_plane_close(void)
{
    outw(0x3C4, 0x0302);   /* 0- va 1-tekislik (belgi va rang) */
    outw(0x3C4, 0x0304);
    outw(0x3CE, 0x0004);
    outw(0x3CE, 0x1005);
    outw(0x3CE, 0x0E06);   /* xotira 0xB8000 dan — matn rejimi */
}

static void font_save(void)
{
    font_plane_open();
    memcpy(saved_font, (void *)0xA0000, sizeof(saved_font));
    font_plane_close();
}

static void font_restore(void)
{
    font_plane_open();
    memcpy((void *)0xA0000, saved_font, sizeof(saved_font));
    font_plane_close();
}

/* --------------------- 80x25 matn rejimining VGA registrlari --------------------- */

static const uint8_t mode3_seq[5]  = { 0x03, 0x00, 0x03, 0x00, 0x02 };
static const uint8_t mode3_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF
};
static const uint8_t mode3_gc[9]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF };
static const uint8_t mode3_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};

static void vga_write_mode3(void)
{
    outb(0x3C2, 0x67);                          /* Miscellaneous Output */

    for (uint8_t i = 0; i < 5; i++) {           /* Sequencer */
        outb(0x3C4, i);
        outb(0x3C5, mode3_seq[i]);
    }

    /* CRTC registrlari yozishdan himoyalangan bo'lishi mumkin — qulfni ochamiz */
    outb(0x3D4, 0x03);
    outb(0x3D5, inb(0x3D5) | 0x80);
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & 0x7F);
    for (uint8_t i = 0; i < 25; i++) {
        uint8_t v = mode3_crtc[i];
        if (i == 0x03) v |= 0x80;
        if (i == 0x11) v &= 0x7F;
        outb(0x3D4, i);
        outb(0x3D5, v);
    }

    for (uint8_t i = 0; i < 9; i++) {           /* Graphics Controller */
        outb(0x3CE, i);
        outb(0x3CF, mode3_gc[i]);
    }

    for (uint8_t i = 0; i < 21; i++) {          /* Attribute Controller (o'qish uni "indeks" holatiga qaytaradi) */
        (void)inb(0x3DA);
        outb(0x3C0, i);
        outb(0x3C0, mode3_ac[i]);
    }
    (void)inb(0x3DA);
    outb(0x3C0, 0x20);                          /* palitrani qulflab, ekranni yoqish */
}

/* ------------------------------ Rejim almashtirish ------------------------------ */

bool fb_set_mode(uint32_t w, uint32_t h)
{
    if (!info.available || info.active)
        return false;
    uint32_t size = w * h * 4;
    if (size > info.vram_bytes)
        return false;

    /* Framebuffer ni kernel xotirasiga ulash. Sahifa jadvallari oldindan bor,
       shuning uchun bu sahifalar barcha jarayonlarda ham ko'rinadi */
    for (uint32_t off = 0; off < size; off += PAGE_SIZE)
        if (!paging_map(FB_VIRT + off, info.phys + off, PAGE_RW))
            return false;

    uint32_t flags = irq_save();
    font_save();
    vbe_write(VBE_ENABLE, VBE_DISABLED);
    vbe_write(VBE_XRES, (uint16_t)w);
    vbe_write(VBE_YRES, (uint16_t)h);
    vbe_write(VBE_BPP, 32);
    vbe_write(VBE_VIRT_W, (uint16_t)w);
    vbe_write(VBE_X_OFF, 0);
    vbe_write(VBE_Y_OFF, 0);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
    irq_restore(flags);

    if (vbe_read(VBE_XRES) != w || vbe_read(VBE_YRES) != h) {
        fb_text_mode();                         /* adapter bu o'lchamni qabul qilmadi */
        return false;
    }

    info.width = w;
    info.height = h;
    info.bpp = 32;
    info.pitch = w * 4;
    info.pixels = (uint32_t *)FB_VIRT;
    info.active = true;
    klog("fb: grafik rejim %ux%u, 32 bit", w, h);
    return true;
}

void fb_text_mode(void)
{
    uint32_t flags = irq_save();
    vbe_write(VBE_ENABLE, VBE_DISABLED);
    vga_write_mode3();
    font_restore();
    bool was_active = info.active;
    info.active = false;
    irq_restore(flags);
    if (was_active)
        klog("fb: matn rejimiga qaytildi");
}

/* Kernel panic: grafik rejim ishlayotgan bo'lsa, xabar ko'rinishi uchun matnga o'tamiz.
   Hech qanday qulf va xotira ajratish yo'q — tizim har qanday holatda bo'lishi mumkin */
void fb_emergency_text(void)
{
    if (!info.active)
        return;
    vbe_write(VBE_ENABLE, VBE_DISABLED);
    vga_write_mode3();
    font_restore();
    info.active = false;
}
