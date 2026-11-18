/*
 * apps.c — "DiyorOS haqida" oynasi va /bin dagi dasturlarni ishga tushirish (v0.7).
 */
#include "gui.h"
#include "process.h"
#include "memory.h"
#include "drivers.h"
#include "fb.h"
#include "pci.h"
#include "string.h"
#include "kprintf.h"
#include "config.h"

#define W 470
#define H 330

/* Dastur ring 3 da, o'z xotirasida ishga tushadi. Grafik dasturlar (mandel, ilon)
   o'zi oyna ochadi, matnli dasturlar (hello, primes) natijani terminalga chiqaradi */
void app_run_program(const char *path)
{
    int32_t pid = process_spawn(path, "");
    if (pid > 0) {
        wm_track_process((uint32_t)pid);
        return;
    }
    kprintf("grafik: %s - %s\n", path, process_strerror(pid));
}

static void about_draw(window_t *w)
{
    surface_t *s = &w->surf;
    char buf[96];
    gfx_fill(s, 0, 0, W, H, RGB(255, 255, 255));
    gfx_gradient_v(s, 0, 0, W, 96, COL_TITLE_ACTIVE, COL_TITLE_ACTIVE2);

    /* Logotip va nom */
    ui_logo(s, 24, 20, 56);
    gfx_text_scaled(s, 96, 18, OS_NAME, 3, RGB(255, 255, 255));
    gfx_text(s, 98, 70, "Versiya " OS_VERSION " - noldan yozilgan OS",
             RGB(200, 220, 250), GFX_TRANSPARENT);

    int y = 112;
    const char *labels[] = { "Muallif", "Arxitektura", "Til", "Ekran", "Xotira", "Qurilmalar", "Ishlash vaqti" };
    for (int i = 0; i < 7; i++)
        gfx_text_bold(s, 24, y + i * 22, labels[i], COL_TEXT_DIM);
    gfx_text(s, 150, y, OS_AUTHOR, COL_TEXT, GFX_TRANSPARENT);
    gfx_text(s, 150, y + 22, OS_ARCH, COL_TEXT, GFX_TRANSPARENT);
    gfx_text(s, 150, y + 44, "C va GNU Assembly (kutubxonalarsiz)", COL_TEXT, GFX_TRANSPARENT);
    ksnprintf(buf, sizeof(buf), "%ux%u, 32 bit rang (%s)", fb_info()->width, fb_info()->height,
              fb_info()->boot ? "bootloader ekrani" : "Bochs VBE");
    gfx_text(s, 150, y + 66, buf, COL_TEXT, GFX_TRANSPARENT);
    ksnprintf(buf, sizeof(buf), "%u MB RAM, %u MB bo'sh", pmm_total_frames() * 4 / 1024,
              (pmm_total_frames() - pmm_used_frames()) * 4 / 1024);
    gfx_text(s, 150, y + 88, buf, COL_TEXT, GFX_TRANSPARENT);
    ksnprintf(buf, sizeof(buf), "%d ta PCI qurilma", pci_count());
    gfx_text(s, 150, y + 110, buf, COL_TEXT, GFX_TRANSPARENT);
    uint32_t up = timer_seconds();
    ksnprintf(buf, sizeof(buf), "%u soat %u daqiqa %u soniya", up / 3600, (up / 60) % 60, up % 60);
    gfx_text(s, 150, y + 132, buf, COL_TEXT, GFX_TRANSPARENT);

    gfx_hline(s, 24, H - 40, W - 48, RGB(220, 224, 230));
    gfx_text(s, 24, H - 30, "Yadro: xotira, jarayonlar, VFS, FAT32, syscall, grafika",
             COL_TEXT_DIM, GFX_TRANSPARENT);
    wm_invalidate(w);
}

static const window_ops_t about_ops = {
    .tick = about_draw,
};

void app_about_open(void)
{
    window_t *w = wm_create("DiyorOS haqida", W, H, &about_ops, 0);
    if (w)
        about_draw(w);
}
