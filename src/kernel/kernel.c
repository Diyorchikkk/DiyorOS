/*
 * kernel.c — kernelning asosiy funksiyasi.
 *
 * boot.s dan keyin shu yerga kelamiz. Ketma-ketlik muhim — har bir qism
 * o'zidan oldingisiga tayanadi:
 *   1. Serial port va ekran (VGA)      — xabar chiqara olish
 *   2. GDT, TSS                        — segmentlar, ring 0 / ring 3
 *   3. IDT + PIC                       — uzilishlar va exceptionlar
 *   4. PMM -> paging -> heap           — xotiraning uch qatlami
 *   5. FPU, ATA disk, fayl tizimlari
 *   6. Taymer, klaviatura, sichqoncha
 *   7. Uzilishlarni yoqish (sti), scheduler
 *   8. Shell
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
#include "vfs.h"
#include "ata.h"
#include "klog.h"
#include <stdarg.h>
#include <stdbool.h>

uint32_t mem_lower_kb = 0;
uint32_t mem_upper_kb = 0;

/*
 * Yuklanish bosqichi haqida xabar: ekranga "[ OK ] ..." yoki "[ -- ] ...",
 * shu bilan birga kernel jurnaliga (dmesg) ham yoziladi.
 */
static void boot_msg(bool ok, const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  [ ");
    vga_set_color(ok ? VGA_LIGHT_GREEN : VGA_YELLOW, VGA_BLACK);
    kprintf(ok ? "OK" : "--");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf(" ] %s\n", msg);
    klog_quiet("%s", msg);              /* serial portga ekran nusxasi orqali allaqachon chiqdi */
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
        mid[10] = (char)('0' + (offset / 100) % 10);
        mid[11] = (char)('0' + (offset / 10) % 10);
        mid[12] = (char)('0' + offset % 10);
        int m = (VGA_WIDTH - (int)(sizeof(mid) - 1)) / 2;
        for (int j = 0; mid[j]; j++)
            line[m + j] = mid[j];
    }

    uint32_t h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
    char right[] = "Ishlash vaqti: 00:00:00 ";
    int p = 15;   /* "Ishlash vaqti: " uzunligi */
    right[p]     = (char)('0' + (h / 10) % 10);
    right[p + 1] = (char)('0' + h % 10);
    right[p + 3] = (char)('0' + m / 10);
    right[p + 4] = (char)('0' + m % 10);
    right[p + 6] = (char)('0' + s / 10);
    right[p + 7] = (char)('0' + s % 10);

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
    serial_init();               /* eng birinchi: keyingi har bir xabar serial portga ham chiqadi */
    vga_init();
    status_bar_update(0);

    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("%s v%s yuklanmoqda...\n\n", OS_NAME, OS_VERSION);
    klog_quiet("%s v%s, yig'ilgan: %s %s", OS_NAME, OS_VERSION, __DATE__, __TIME__);

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("Multiboot magic noto'g'ri: 0x%x (bootloader xotira haqida ma'lumot bermadi)", magic);
    if (mbi->flags & MULTIBOOT_FLAG_MEM) {
        mem_lower_kb = mbi->mem_lower;
        mem_upper_kb = mbi->mem_upper;
    }
    boot_msg(true, "Multiboot bootloader aniqlandi%s",
             serial_present() ? ", serial konsol (COM1) ulandi" : "");

    gdt_init();
    boot_msg(true, "GDT (ring 0 / ring 3 segmentlari) va TSS o'rnatildi");

    idt_init();
    boot_msg(true, "IDT, PIC va syscall (int 0x80) o'rnatildi");

    /* Xotira: IDT dan keyin, chunki paging xatolari (Page Fault) ushlanishi kerak */
    pmm_init(mbi);
    boot_msg(true, "Fizik xotira menejeri: %u MB, %u ta frame (4 KB)",
             pmm_memory_top() / (1024 * 1024), pmm_total_frames());

    paging_init();
    boot_msg(true, "Paging yoqildi; kernel kodi faqat o'qish uchun (%u KB)",
             (uint32_t)(kernel_data_start - kernel_start) / 1024);

    heap_init();
    boot_msg(true, "Kernel heap tayyor (kmalloc / kfree)");

    fpu_init();

    bool have_disk = ata_init();
    if (have_disk)
        boot_msg(true, "ATA disk: %s, %u MB", ata_info()->model, ata_info()->sectors / 2048);

    rootfs_init();
    boot_msg(true, "Fayl tizimi: VFS, ramfs (/), procfs (/proc), dasturlar (/bin)");

    if (!have_disk) {
        boot_msg(false, "Disk topilmadi (faqat xotiradagi fayl tizimi)");
    } else if (disk_mount() == VFS_OK) {
        fat32_stats_t st;
        fat32_stats(&st);
        boot_msg(true, "/disk: FAT32 \"%s\", %u MB bo'sh", st.label,
                 st.free_clusters / (1024 * 1024 / st.cluster_bytes));
    } else {
        boot_msg(false, "Disk FAT32 da formatlanmagan. Formatlash: mkfs");
    }

    timer_init(TIMER_HZ);
    timer_on_second(status_bar_update);
    boot_msg(true, "PIT taymer ishga tushdi (%u Hz)", TIMER_HZ);

    keyboard_init();
    boot_msg(true, "PS/2 klaviatura drayveri ulandi");

    if (mouse_init())
        boot_msg(true, "PS/2 sichqoncha (g'ildirak bilan) ulandi");
    else
        boot_msg(true, "PS/2 sichqoncha ulandi (g'ildiraksiz)");

    __asm__ volatile ("sti");    /* uzilishlarni yoqamiz — endi IRQ lar keladi */
    boot_msg(true, "Uzilishlar yoqildi");

    sched_init();                /* shu oqim "shell" vazifasiga aylanadi, idle yaratiladi */
    boot_msg(true, "Scheduler ishga tushdi (Round-Robin, kvant %u ms)",
             SCHED_QUANTUM * 1000 / TIMER_HZ);

    print_banner();
    shell_run();                 /* bu funksiyadan hech qachon qaytilmaydi */
    panic("shell_run qaytdi");
}
