/*
 * cmd_hw.c — "hw": kompyuter haqida qisqa hisobot (v0.9).
 *
 * Haqiqiy kompyuterda sinaganda shu ekranni suratga olish kifoya: qaysi
 * protsessor, qancha xotira, ekran qanday olingan, disk qaysi drayver
 * bilan ishlayapti, klaviatura/sichqoncha bormi, PCI da nimalar bor.
 */
#include <stdint.h>
#include <stdbool.h>
#include "shell.h"
#include "kprintf.h"
#include "vga.h"
#include "vfs.h"
#include "memory.h"
#include "kernel.h"
#include "disk.h"
#include "fb.h"
#include "fbcon.h"
#include "pci.h"
#include "drivers.h"
#include "string.h"

static void title(const char *t)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("%s\n", t);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cpu_lines(void)
{
    char buf[512];
    int fd = vfs_open("/proc/cpuinfo", VFS_O_READ);
    if (fd < 0)
        return;
    int n = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    for (char *line = buf; *line; ) {        /* har qatorni ikki bo'sh joy bilan */
        char *end = line;
        while (*end && *end != '\n')
            end++;
        char save = *end;
        *end = 0;
        kprintf("  %s\n", line);
        if (!save)
            break;
        line = end + 1;
    }
}

void cmd_hw(const char *args)
{
    (void)args;
    title("Protsessor");
    cpu_lines();

    title("Xotira va yuklanish");
    kprintf("  RAM:           %u MB (ishlatiladi: %u MB)\n", pmm_ram_total_mb(),
            pmm_memory_top() / (1024 * 1024));
    kprintf("  Bootloader:    %s\n", boot_loader_name());
    if (fbcon_enabled()) {
        char d[48];
        fbcon_describe(d, sizeof(d));
        kprintf("  Ekran:         grafik framebuffer %s (UEFI GOP / VBE)\n", d);
    } else {
        kprintf("  Ekran:         VGA matn rejimi 80x25%s\n",
                fb_info()->available ? ", grafik: Bochs VBE" : "");
    }

    title("Qurilmalar");
    const disk_info_t *di = disk_info();
    if (di->present)
        kprintf("  Disk:          %s, %u MB - %s\n", di->model, di->sectors / 2048, di->driver);
    else
        kprintf("  Disk:          topilmadi (IDE ham, AHCI/SATA ham yo'q)\n");
    for (int i = 0; i < pci_count(); i++)
        if (pci_get(i)->class_code == 0x01 && pci_get(i)->subclass == 0x04)
            kprintf("  Diqqat:        disk RAID rejimida - BIOS da SATA rejimini AHCI qiling\n");
    kprintf("  Klaviatura:    %s\n", ps2_present() ? "PS/2" : "PS/2 kontrolleri yo'q");

    int usb = 0, nvme = 0, net = 0;
    for (int i = 0; i < pci_count(); i++) {
        const pci_device_t *d = pci_get(i);
        if (d->class_code == 0x0C && d->subclass == 0x03)
            usb++;
        if (d->class_code == 0x01 && d->subclass == 0x08)
            nvme++;
        if (d->class_code == 0x02)
            net++;
    }
    kprintf("  PCI:           %d ta qurilma (batafsil: pci)\n", pci_count());
    if (usb)
        kprintf("  USB:           %d ta kontroller - drayveri yo'q (BIOS \"USB Legacy\" ishlatiladi)\n", usb);
    if (nvme)
        kprintf("  NVMe disk:     %d ta - drayveri yo'q (fayllar faqat xotirada saqlanadi)\n", nvme);
    if (net)
        kprintf("  Tarmoq:        %d ta karta - drayveri yo'q\n", net);
}
