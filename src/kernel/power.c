/*
 * power.c — kompyuterni o'chirish va qayta yuklash (v0.7).
 * Avval shell ichida edi; endi grafik menyu ham chaqiradi.
 */
#include "kernel.h"
#include "io.h"
#include "vfs.h"
#include "klog.h"
#include "task.h"

void power_reboot(void)
{
    klog("qayta yuklash");
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++)   /* klaviatura kontrolleri tayyor bo'lguncha */
        ;
    outb(0x64, 0xFE);            /* CPU ga reset signali */
    for (;;)
        __asm__ volatile ("hlt");
}

void power_off(void)
{
    klog("tizimni o'chirish");
    /* v0.10: har vazifaning o'z joriy papkasi bor — birortasi /disk ichida qolsa, disk uzilmasdi */
    for (int i = 0; i < MAX_TASKS; i++)
        task_get(i)->cwd = vfs_root();
    vfs_unmount("/disk");        /* disk toza uziladi: FSInfo ga bo'sh joy soni yoziladi */
    outw(0x604, 0x2000);         /* QEMU (yangi versiyalar) ACPI o'chirish porti */
    outw(0xB004, 0x2000);        /* Bochs / eski QEMU */
    __asm__ volatile ("cli");
}
