/*
 * fbcon.c — framebuffer konsoli (v0.9).
 *
 * MUAMMO. Eski kompyuterlarda BIOS ekranni VGA matn rejimida qoldiradi:
 * 0xB8000 manziliga belgi kodini yozsak, videokartaning o'zi uni harfga
 * aylantiradi. UEFI li zamonaviy kompyuterlarda bu rejim UMUMAN YO'Q —
 * faqat piksellar massivi (framebuffer) bor. U yerda harfni ham piksellab
 * o'zimiz chizishimiz kerak (Linux da bu "fbcon" deyiladi).
 *
 * YECHIM. Bootloader (GRUB) grafik rejimni yoqib, framebuffer manzili,
 * o'lchami va piksel formatini multiboot ma'lumotida beradi. Konsol
 * avvalgidek 80x25 katakli massivda yashaydi (vga.c ning soya massivi) —
 * shell, muharrir, dasturlar hech narsani sezmaydi. Bu fayl massivni
 * kuzatib turadi va O'ZGARGAN kataklarni shrift bilan piksellarga chizadi:
 *
 *     kprintf -> vga.c (80x25 katak, xotirada) -> fbcon.c -> framebuffer
 *
 * Kim va qachon chizadi:
 *   - yuklanishda (scheduler hali yo'q) — har bir yangi qatordan keyin darhol;
 *   - keyin — alohida "fbcon" kernel oqimi har 20 ms da (o'zgarish bo'lsa);
 *   - panic da — hech narsani kutmasdan, uzilishlarsiz.
 * Grafik interfeys (gui) ishlayotganda konsol ekranni unga bo'shatib beradi.
 *
 * Ekran katta bo'lsa, matn butun songa kattalashtiriladi (1920x1080 da 2x)
 * va o'rtaga joylashtiriladi.
 */
#include "fbcon.h"
#include "fb.h"
#include "vga.h"
#include "gfx.h"
#include "task.h"
#include "sync.h"
#include "string.h"
#include "kprintf.h"

#define COLS      VGA_WIDTH
#define ROWS      VGA_HEIGHT
#define CELLS     (COLS * ROWS)
#define MAX_SCALE 4

static bool      enabled = false;
static volatile bool suspended = false;
static volatile bool need_full = true;    /* keyingi chizishda butun ekranni tozalab chizish */
static bool      thread_on = false;
static uint8_t  *fbmem;                   /* framebuffer boshi (avval fizik, paging dan keyin FB_VIRT) */
static uint32_t  width, height, pitch, bytespp, visible_h;
static int       scale = 1, ox, oy;
static uint32_t  pal[16];                 /* 16 rang — ekranning piksel formatida */
static uint16_t  drawn[CELLS];            /* ekranda hozir chizilgan holat */
static int       drawn_cursor = -1;
static uint32_t  last_gen;
static mutex_t   lock;                    /* chizish va to'xtatish bir vaqtda bo'lmasin */

/* 0x00RRGGBB -> ekranning formati (masalan, ba'zi kartalarda ko'k va qizil joyi teskari) */
static uint32_t pack(uint32_t rgb)
{
    const fb_info_t *fi = fb_info();
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    if (bytespp == 2)                     /* 16 bit: 5-6-5 */
        return ((r >> 3) << fi->red_pos) | ((g >> 2) << fi->green_pos) | ((b >> 3) << fi->blue_pos);
    return (r << fi->red_pos) | (g << fi->green_pos) | (b << fi->blue_pos);
}

/* Bitta qator piksellarni framebuffer ga yozish */
static void write_line(uint8_t *dst, const uint32_t *px, int n)
{
    if (bytespp == 4) {
        memcpy(dst, px, (uint32_t)n * 4);
    } else if (bytespp == 3) {
        for (int i = 0; i < n; i++) {
            dst[i * 3] = (uint8_t)px[i];
            dst[i * 3 + 1] = (uint8_t)(px[i] >> 8);
            dst[i * 3 + 2] = (uint8_t)(px[i] >> 16);
        }
    } else {
        for (int i = 0; i < n; i++)
            ((uint16_t *)dst)[i] = (uint16_t)px[i];
    }
}

static void draw_cell(int pos, uint16_t cell, bool cursor)
{
    int cx = pos % COLS, cy = pos / COLS;
    uint8_t ch = (uint8_t)cell, attr = (uint8_t)(cell >> 8);
    uint32_t fg = pal[attr & 0x0F], bg = pal[(attr >> 4) & 0x0F];
    const uint8_t *glyph = font8x16[ch < 128 ? ch : '?'];
    uint32_t line[FONT_W * MAX_SCALE];
    uint32_t top = (uint32_t)(oy + cy * FONT_H * scale);
    uint8_t *base = fbmem + top * pitch + (uint32_t)(ox + cx * FONT_W * scale) * bytespp;

    for (int gy = 0; gy < FONT_H; gy++) {
        uint8_t bits = glyph[gy];
        if (cursor && gy >= FONT_H - 2)
            bits = 0xFF;                      /* kursor — katak ostidagi chiziq */
        for (int gx = 0; gx < FONT_W; gx++) {
            uint32_t px = (bits & (0x80 >> gx)) ? fg : bg;
            for (int s = 0; s < scale; s++)
                line[gx * scale + s] = px;
        }
        for (int sy = 0; sy < scale; sy++) {
            uint32_t y = top + (uint32_t)(gy * scale + sy);
            if (y < visible_h)
                write_line(base + (uint32_t)(gy * scale + sy) * pitch, line, FONT_W * scale);
        }
    }
}

static void clear_screen(void)
{
    for (uint32_t y = 0; y < visible_h; y++)
        memset(fbmem + y * pitch, 0, width * bytespp);
}

/* O'zgargan kataklarni chizish. full — hammasini (ekran boshqa narsa bilan buzilgan) */
static void render(bool full)
{
    static uint16_t now[CELLS];
    int cur;
    bool vis;
    vga_snapshot(now, &cur, &vis);
    int cpos = vis ? cur : -1;
    if (full)
        clear_screen();
    for (int i = 0; i < CELLS; i++) {
        bool c_here = i == cpos, c_was = i == drawn_cursor;
        if (!full && now[i] == drawn[i] && c_here == c_was)
            continue;
        draw_cell(i, now[i], c_here);
        drawn[i] = now[i];
    }
    drawn_cursor = cpos;
}

/* Yuklanish paytida vga.c har yangi qatordan keyin chaqiradi */
static void early_flush(void)
{
    if (!enabled || suspended || thread_on)
        return;                               /* oqim ishga tushgan — u chizadi */
    bool full = need_full;
    need_full = false;
    render(full);
}

bool fbcon_early(const multiboot_info_t *mbi)
{
    if (!(mbi->flags & MULTIBOOT_FLAG_FB) || mbi->framebuffer_type != MULTIBOOT_FB_RGB)
        return false;                         /* VGA matn rejimi (yoki ekran ma'lumoti yo'q) */
    if (mbi->framebuffer_addr >= 0x100000000ull)
        return false;                         /* 4 GB dan yuqorida — 32 bitli kernel yeta olmaydi */
    uint32_t bpp = mbi->framebuffer_bpp;
    if (bpp != 16 && bpp != 24 && bpp != 32)
        return false;
    if (mbi->framebuffer_width < COLS * FONT_W || mbi->framebuffer_height < ROWS * FONT_H)
        return false;

    fb_boot_setup((uint32_t)mbi->framebuffer_addr, mbi->framebuffer_width, mbi->framebuffer_height,
                  mbi->framebuffer_pitch, bpp, mbi->red_pos, mbi->green_pos, mbi->blue_pos);
    width = mbi->framebuffer_width;
    height = mbi->framebuffer_height;
    pitch = mbi->framebuffer_pitch;
    bytespp = bpp / 8;
    visible_h = height;
    fbmem = (uint8_t *)fb_info()->pixels;     /* paging hali o'chiq: fizik manzil */

    uint32_t sx = width / (COLS * FONT_W), sy = height / (ROWS * FONT_H);
    scale = (int)(sx < sy ? sx : sy);
    if (scale > MAX_SCALE)
        scale = MAX_SCALE;
    ox = (int)(width - (uint32_t)(COLS * FONT_W * scale)) / 2;
    oy = (int)(height - (uint32_t)(ROWS * FONT_H * scale)) / 2;
    for (int i = 0; i < 16; i++)
        pal[i] = pack(vga_palette_rgb[i]);

    vga_use_shadow();                         /* konsol endi xotirada, 0xB8000 ishlatilmaydi */
    vga_set_flush_hook(early_flush);
    enabled = true;
    need_full = true;
    return true;
}

void fbcon_after_paging(void)
{
    if (!enabled)
        return;
    if (!fb_boot_map()) {
        enabled = false;                      /* ekranni ulab bo'lmadi: faqat serial port qoladi */
        return;
    }
    fbmem = (uint8_t *)fb_info()->pixels;
    uint32_t mapped = FB_MAX_SIZE / pitch;    /* juda katta ekranda faqat ulangan qismi */
    if (visible_h > mapped)
        visible_h = mapped;
}

static void fbcon_thread(uint32_t arg)
{
    (void)arg;
    for (;;) {
        mutex_lock(&lock);
        uint32_t gen = vga_generation();
        if (!suspended && (need_full || gen != last_gen)) {
            bool full = need_full;
            need_full = false;
            last_gen = gen;
            render(full);
        }
        mutex_unlock(&lock);
        task_sleep_ticks(2);                  /* 20 ms: sezilmaydi, protsessorni band qilmaydi */
    }
}

void fbcon_start(void)
{
    if (!enabled)
        return;
    mutex_init(&lock, "fbcon", false);
    if (task_create("fbcon", fbcon_thread, 0))
        thread_on = true;
}

bool fbcon_enabled(void)
{
    return enabled;
}

void fbcon_suspend(void)
{
    if (!enabled)
        return;
    mutex_lock(&lock);                        /* chizish o'rtasida bo'lsa — tugashini kutamiz */
    suspended = true;
    mutex_unlock(&lock);
}

void fbcon_resume(void)
{
    if (!enabled)
        return;
    need_full = true;                         /* grafik interfeys ekranni bo'yab ketgan */
    suspended = false;
}

/* Panic: uzilishlar o'chiq, boshqa hech kim ishlamaydi — to'g'ridan-to'g'ri chizamiz */
void fbcon_panic(void)
{
    if (!enabled)
        return;
    suspended = false;
    render(true);
}

void fbcon_describe(char *buf, int size)
{
    ksnprintf(buf, (uint32_t)size, "%ux%u, %u bit, matn %ux", width, height, bytespp * 8, scale);
}
