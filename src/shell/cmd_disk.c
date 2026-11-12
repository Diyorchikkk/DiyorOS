/*
 * cmd_disk.c — disk bilan bog'liq shell buyruqlari (v0.6).
 *
 *   disk           disk va FAT32 haqida ma'lumot
 *   mkfs [nom]     diskni FAT32 da formatlash (hamma ma'lumot o'chadi!)
 *   mount          ulangan fayl tizimlari; "mount disk" — diskni /disk ga ulash
 *   umount /disk   diskni uzish
 */
#include <stdint.h>
#include <stdbool.h>
#include "shell.h"
#include "vfs.h"
#include "ata.h"
#include "kprintf.h"
#include "vga.h"
#include "drivers.h"
#include "string.h"

static void red(const char *msg)
{
    uint8_t old = vga_get_color();
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("%s", msg);
    vga_set_color(old & 0x0F, old >> 4);
}

/* Klaviaturadan qisqa javob o'qish (tasdiqlash uchun) */
static void ask(char *buf, int max)
{
    int n = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n' || c == KEY_CTRL_C) {
            if (c == KEY_CTRL_C)
                n = 0;
            vga_putc('\n');
            break;
        }
        if (c == '\b' && n > 0) {
            n--;
            vga_putc('\b');
        } else if (c >= 32 && c < 127 && n < max - 1) {
            buf[n++] = c;
            vga_putc(c);
        }
    }
    buf[n] = 0;
}

void cmd_disk(const char *args)
{
    (void)args;
    const ata_info_t *ai = ata_info();
    if (!ai->present) {
        red("Disk topilmadi. QEMU ni 'make run' bilan ishga tushiring (disk.img ulanadi).\n");
        return;
    }
    kprintf("Qurilma:        ATA (IDE), birlamchi kanal, master\n");
    kprintf("Model:          %s\n", ai->model);
    kprintf("Hajmi:          %u sektor x 512 bayt = %u MB\n", ai->sectors, ai->sectors / 2048);
    kprintf("Statistika:     %u sektor o'qildi, %u sektor yozildi\n", ai->reads, ai->writes);

    fat32_stats_t st;
    fat32_stats(&st);
    if (!st.mounted) {
        kprintf("Fayl tizimi:    ulanmagan (formatlash: mkfs, ulash: mount disk)\n");
        return;
    }
    uint32_t mb = 1024 * 1024 / st.cluster_bytes;
    kprintf("Fayl tizimi:    FAT32, nomi \"%s\", ID %x\n", st.label, st.volume_id);
    kprintf("Klaster:        %u bayt, jami %u ta\n", st.cluster_bytes, st.total_clusters);
    kprintf("FAT jadvali:    %u sektor (x2 nusxa)\n", st.fat_sectors);
    kprintf("Band / bo'sh:   %u KB / %u MB\n",
            (st.total_clusters - st.free_clusters) * (st.cluster_bytes / 512) / 2, st.free_clusters / mb);
    kprintf("Ulangan joyi:   /disk\n");
}

void cmd_mkfs(const char *args)
{
    while (*args == ' ')
        args++;
    const char *label = *args ? args : "DIYOROS";
    if (!ata_info()->present) {
        red("mkfs: disk topilmadi\n");
        return;
    }

    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("DIQQAT: diskdagi BARCHA ma'lumotlar o'chadi (%u MB).\n", ata_info()->sectors / 2048);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("Davom etish uchun 'ha' deb yozing: ");
    char answer[8];
    ask(answer, sizeof(answer));
    if (strcmp(answer, "ha") != 0) {
        kprintf("Bekor qilindi.\n");
        return;
    }

    fat32_stats_t st;
    fat32_stats(&st);
    if (st.mounted) {
        int err = vfs_unmount("/disk");
        if (err) {
            kprintf("mkfs: /disk ni uzib bo'lmadi: %s\n", vfs_strerror(err));
            return;
        }
    }

    kprintf("Formatlanmoqda...\n");
    int err = fat32_mkfs(label);
    if (err) {
        kprintf("mkfs: %s\n", err == VFS_ENOSPC ? "FAT32 uchun disk kamida 33 MB bo'lishi kerak"
                                                : vfs_strerror(err));
        return;
    }
    err = disk_mount();
    if (err) {
        kprintf("mkfs: formatlandi, lekin ulab bo'lmadi: %s\n", vfs_strerror(err));
        return;
    }
    fat32_stats(&st);
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("Tayyor: FAT32 \"%s\", %u MB bo'sh, /disk ga ulandi.\n",
            st.label, st.free_clusters / (1024 * 1024 / st.cluster_bytes));
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

void cmd_mount(const char *args)
{
    while (*args == ' ')
        args++;
    if (strcmp(args, "disk") == 0) {
        int err = disk_mount();
        if (err)
            kprintf("mount: %s\n", err == VFS_EINVAL ? "disk FAT32 da formatlanmagan (mkfs)"
                                                     : vfs_strerror(err));
        else
            kprintf("Disk /disk ga ulandi\n");
        return;
    }
    kprintf("Fayl tizimi  Joyi\n");
    const char *path, *fs;
    for (int i = 0; vfs_mount_info(i, &path, &fs) == VFS_OK; i++)
        kprintf("%-12s %s\n", fs, path);
    kprintf("(diskni ulash: mount disk)\n");
}

void cmd_umount(const char *args)
{
    while (*args == ' ')
        args++;
    if (!*args) {
        kprintf("Foydalanish: umount /disk\n");
        return;
    }
    int err = vfs_unmount(args);
    if (err)
        kprintf("umount: %s: %s\n", args, err == VFS_EINVAL ? "bu yerga hech narsa ulanmagan"
                                                            : vfs_strerror(err));
    else
        kprintf("%s uzildi\n", args);
}
