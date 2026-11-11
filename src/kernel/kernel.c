/*
 * kernel.c — kernelning asosiy funksiyasi.
 *
 * boot.s dan keyin shu yerga kelamiz. Ketma-ketlik:
 *   1. Ekranni tayyorlash (VGA)
 *   2. GDT — xotira segmentlari
 *   3. IDT + PIC — uzilishlar
 *   4. Taymer va klaviatura drayverlari
 *   5. Uzilishlarni yoqish (sti)
 *   6. Shell (buyruqlar qatori) ni ishga tushirish
 */
#include <stdint.h>
#include "config.h"
#include "multiboot.h"
#include "kernel.h"
#include "vga.h"
#include "kprintf.h"
#include "cpu.h"
#include "drivers.h"
#include "memory.h"
#include "task.h"

uint32_t mem_lower_kb = 0;
uint32_t mem_upper_kb = 0;

static void boot_ok(const char *msg)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  [ ");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("OK");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf(" ] %s\n", msg);
}

/* 0-qatordagi status bar: OS nomi va ishlash vaqti. Har sekundda taymer chaqiradi. */
static void status_bar_update(uint32_t seconds)
{
    char line[VGA_WIDTH + 1];
    const char *left = " " OS_NAME " v" OS_VERSION;
    int i = 0;

    for (; i < VGA_WIDTH; i++)
        line[i] = ' ';
    line[VGA_WIDTH] = 0;

    for (int j = 0; left[j]; j++)
        line[j] = left[j];

    /* Tarix varaqlanayotgan bo'lsa — o'rtada ko'rsatkich */
    int offset = vga_view_offset();
    if (offset > 0) {
        char mid[] = "[ Tarix: -000 qator ]";
        mid[10] = '0' + (offset / 100) % 10;
        mid[11] = '0' + (offset / 10) % 10;
        mid[12] = '0' + offset % 10;
        int m = (VGA_WIDTH - (int)(sizeof(mid) - 1)) / 2;
        for (int j = 0; mid[j]; j++)
            line[m + j] = mid[j];
    }

    uint32_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
    char right[] = "Ishlash vaqti: 00:00:00 ";
    int p = 15;   /* "Ishlash vaqti: " uzunligi */
    right[p]     = '0' + (h / 10) % 10;
    right[p + 1] = '0' + h % 10;
    right[p + 3] = '0' + m / 10;
    right[p + 4] = '0' + m % 10;
    right[p + 6] = '0' + s / 10;
    right[p + 7] = '0' + s % 10;

    int start = VGA_WIDTH - (int)(sizeof(right) - 1);
    for (int j = 0; right[j]; j++)
        line[start + j] = right[j];

    /* 16-28 ustunlar fon vazifalari (spinner) uchun ajratilgan — ularga tegmaymiz */
    char head[STATUS_TASK_COL + 1];
    for (int j = 0; j < STATUS_TASK_COL; j++)
        head[j] = line[j];
    head[STATUS_TASK_COL] = 0;
    vga_write_at(0, 0, head, VGA_WHITE, VGA_BLUE);
    vga_write_at(STATUS_TASK_END, 0, line + STATUS_TASK_END, VGA_WHITE, VGA_BLUE);
}

void status_bar_refresh(void)
{
    status_bar_update(timer_seconds());
}

static void print_banner(void)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("\n");
    kprintf("   ____  _                  ___  ____  \n");
    kprintf("  |  _ \\(_)_   _  ___  _ __/ _ \\/ ___| \n");
    kprintf("  | | | | | | | |/ _ \\| '__| | | \\___ \\ \n");
    kprintf("  | |_| | | |_| | (_) | |  | |_| |___) |\n");
    kprintf("  |____/|_|\\__, |\\___/|_|   \\___/|____/ \n");
    kprintf("           |___/                        \n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  %s v%s  |  %s\n\n", OS_NAME, OS_VERSION, OS_ARCH);
}

void kernel_main(uint32_t magic, multiboot_info_t *mbi)
{
    vga_init();
    status_bar_update(0);

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("%s yuklanmoqda...\n\n", OS_NAME);

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        if (mbi->flags & 1) {
            mem_lower_kb = mbi->mem_lower;
            mem_upper_kb = mbi->mem_upper;
        }
        boot_ok("Multiboot bootloader aniqlandi");
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("  [XATO] Multiboot magic noto'g'ri: 0x%x. Xotira haqida ma'lumot yo'q.\n", magic);
        for (;;)
            __asm__ volatile ("cli; hlt");
    }

    gdt_init();
    boot_ok("GDT (segmentlar jadvali) o'rnatildi");

    idt_init();
    boot_ok("IDT va PIC (uzilishlar) o'rnatildi");

    /* Xotira: IDT dan keyin, chunki paging xatolari (Page Fault) ushlanishi kerak */
    pmm_init(mbi);
    kprintf("  [ ");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("OK");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf(" ] Fizik xotira menejeri: %u MB, %u ta frame (4 KB)\n",
            pmm_memory_top() / (1024 * 1024), pmm_total_frames());

    paging_init();
    boot_ok("Paging yoqildi (virtual xotira)");

    heap_init();
    boot_ok("Kernel heap tayyor (kmalloc / kfree)");

    timer_init(TIMER_HZ);
    timer_on_second(status_bar_update);
    boot_ok("PIT taymer ishga tushdi (100 Hz)");

    keyboard_init();
    boot_ok("PS/2 klaviatura drayveri ulandi");

    if (mouse_init())
        boot_ok("PS/2 sichqoncha (g'ildirak bilan) ulandi");
    else
        boot_ok("PS/2 sichqoncha ulandi (g'ildiraksiz)");

    __asm__ volatile ("sti");    /* uzilishlarni yoqamiz — endi IRQ lar keladi */
    boot_ok("Uzilishlar yoqildi");

    sched_init();                /* shu oqim "shell" vazifasiga aylanadi, idle yaratiladi */
    boot_ok("Scheduler ishga tushdi (Round-Robin, kvant 50 ms)");

    print_banner();
    shell_run();                 /* bu funksiyadan hech qachon qaytilmaydi */
}
