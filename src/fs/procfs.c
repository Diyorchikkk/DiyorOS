/*
 * procfs.c — /proc: "virtual" fayl tizimi (Linux dagi kabi).
 *
 * Bu fayllar hech qayerda saqlanmaydi. "cat /proc/meminfo" qilinganda
 * mazmun shu zahoti kernel ma'lumotlaridan yaratiladi (generate funksiyasi).
 * Shuning uchun har safar o'qilganda yangi qiymat chiqadi.
 *
 * Bu VFS ning kuchini ko'rsatadi: cat buyrug'i oddiy fayl bilan /proc
 * faylini farqlamaydi — ikkalasi ham vfs_read orqali o'qiladi.
 */
#include "vfs.h"
#include "kprintf.h"
#include "memory.h"
#include "task.h"
#include "string.h"
#include "config.h"
#include "cpu.h"
#include "klog.h"

#define GEN_BUF_SIZE 4096

static int gen_version(char *b, uint32_t n)
{
    return ksnprintf(b, n, "%s %s (%s)\nMuallif: %s\nKompilyator: GCC, %s %s\n",
                     OS_NAME, OS_VERSION, OS_ARCH, OS_AUTHOR, __DATE__, __TIME__);
}

static int gen_uptime(char *b, uint32_t n)
{
    uint32_t s = timer_seconds();
    return ksnprintf(b, n, "%u soniya (%02u:%02u:%02u), %u tick\n",
                     s, s / 3600, (s / 60) % 60, s % 60, timer_ticks());
}

static int gen_meminfo(char *b, uint32_t n)
{
    heap_stats_t hs;
    heap_get_stats(&hs);
    uint32_t total = pmm_total_frames(), used = pmm_used_frames();
    return ksnprintf(b, n,
        "MemTotal:    %u KB\n"
        "MemFree:     %u KB\n"
        "MemUsed:     %u KB\n"
        "Frames:      %u (band %u)\n"
        "HeapSize:    %u KB\n"
        "HeapUsed:    %u bayt (%u blok)\n"
        "HeapFree:    %u bayt (%u blok)\n",
        total * 4, (total - used) * 4, used * 4, total, used,
        (hs.end - hs.start) / 1024, hs.used_bytes, hs.used_blocks,
        hs.free_bytes, hs.free_blocks);
}

/* CPUID — protsessor o'zi haqida ma'lumot beradigan buyruq */
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static int gen_cpuinfo(char *b, uint32_t n)
{
    uint32_t r[4];
    char vendor[13];
    cpuid(0, &r[0], &r[1], &r[2], &r[3]);
    memcpy(vendor, &r[1], 4);                /* EBX, EDX, ECX tartibida */
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;

    char brand[49] = "(noma'lum)";
    uint32_t max_ext, t;
    cpuid(0x80000000, &max_ext, &t, &t, &t);
    if (max_ext >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, &r[0], &r[1], &r[2], &r[3]);
            memcpy(brand + i * 16, r, 16);
        }
        brand[48] = 0;
    }
    const char *bp = brand;
    while (*bp == ' ')
        bp++;

    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    uint32_t family = (eax >> 8) & 0xF, model = (eax >> 4) & 0xF, stepping = eax & 0xF;

    return ksnprintf(b, n,
        "Ishlab chiqaruvchi: %s\n"
        "Model nomi:         %s\n"
        "Oila / model / stepping: %u / %u / %u\n"
        "Imkoniyatlar:%s%s%s%s%s%s\n",
        vendor, bp, family, model, stepping,
        (edx & (1 << 0))  ? " fpu"  : "",
        (edx & (1 << 4))  ? " tsc"  : "",
        (edx & (1 << 9))  ? " apic" : "",
        (edx & (1 << 23)) ? " mmx"  : "",
        (edx & (1 << 25)) ? " sse"  : "",
        (edx & (1 << 26)) ? " sse2" : "");
}

static int gen_tasks(char *b, uint32_t n)
{
    static const char *states[] = { "-", "tayyor", "ishlayapti", "uxlayapti", "kutyapti", "tugagan" };
    int len = ksnprintf(b, n, "PID  NOMI        HOLAT        CPU%%\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_get(i);
        if (t->state == TASK_UNUSED)
            continue;
        len += ksnprintf(b + len, len < (int)n ? n - len : 0, "%3u  %-10s  %-11s  %3u%%\n",
                         t->pid, t->name, states[t->state], t->cpu_percent);
    }
    return len;
}

static int gen_mounts(char *b, uint32_t n)
{
    int len = 0;
    const char *path, *fs;
    for (int i = 0; vfs_mount_info(i, &path, &fs) == VFS_OK; i++)
        len += ksnprintf(b + len, len < (int)n ? n - len : 0, "%-8s %s\n", fs, path);
    return len;
}

/* Har bir qurilma uzilishi (IRQ) necha marta kelgani — Linux dagi /proc/interrupts */
static int gen_interrupts(char *b, uint32_t n)
{
    static const char *names[16] = {
        "taymer (PIT)", "klaviatura", "kaskad (slave PIC)", "COM2", "COM1", "LPT2", "floppy", "LPT1",
        "CMOS soati", "-", "-", "-", "sichqoncha (PS/2)", "FPU", "ATA birlamchi", "ATA ikkilamchi"
    };
    int len = ksnprintf(b, n, "IRQ  SONI        QURILMA\n");
    for (int i = 0; i < 16; i++)
        if (irq_count(i))
            len += ksnprintf(b + len, len < (int)n ? n - len : 0, "%3d  %-10u  %s\n",
                             i, irq_count(i), names[i]);
    len += ksnprintf(b + len, len < (int)n ? n - len : 0, "Soxta (spurious): %u\n",
                     irq_spurious_count());
    return len;
}

/* Kernel jurnali (dmesg bilan bir xil; oxirgi ~4 KB) */
static int gen_kmsg(char *b, uint32_t n)
{
    uint32_t size = klog_size();
    uint32_t off = size > n - 1 ? size - (n - 1) : 0;
    uint32_t got = klog_read(off, b, n - 1);
    b[got] = 0;
    return (int)got;
}

/* ------------------------------ Fayl tizimi ------------------------------ */

static const vfs_ops_t procfs_ops;

static vfs_node_t proc_root;
static vfs_node_t proc_files[] = {
    { .name = "cpuinfo",    .generate = gen_cpuinfo    },
    { .name = "interrupts", .generate = gen_interrupts },
    { .name = "kmsg",       .generate = gen_kmsg       },
    { .name = "meminfo", .generate = gen_meminfo },
    { .name = "mounts",  .generate = gen_mounts  },
    { .name = "tasks",   .generate = gen_tasks   },
    { .name = "uptime",  .generate = gen_uptime  },
    { .name = "version", .generate = gen_version },
};
#define PROC_FILE_COUNT (sizeof(proc_files) / sizeof(proc_files[0]))

static int32_t procfs_read(vfs_node_t *node, uint32_t off, uint32_t len, uint8_t *buf)
{
    char *text = kmalloc(GEN_BUF_SIZE);
    if (!text)
        return VFS_ENOMEM;
    int total = node->generate(text, GEN_BUF_SIZE);
    if (total > GEN_BUF_SIZE - 1)
        total = GEN_BUF_SIZE - 1;

    int32_t count = 0;
    if (off < (uint32_t)total) {
        count = total - off;
        if ((uint32_t)count > len)
            count = len;
        memcpy(buf, text + off, count);
    }
    kfree(text);
    return count;
}

static vfs_node_t *procfs_readdir(vfs_node_t *dir, uint32_t index)
{
    (void)dir;
    return index < PROC_FILE_COUNT ? &proc_files[index] : 0;
}

static vfs_node_t *procfs_finddir(vfs_node_t *dir, const char *name)
{
    (void)dir;
    for (uint32_t i = 0; i < PROC_FILE_COUNT; i++)
        if (strcmp(proc_files[i].name, name) == 0)
            return &proc_files[i];
    return 0;
}

/* Yozish, yaratish, o'chirish yo'q — /proc faqat o'qish uchun */
static const vfs_ops_t procfs_ops = {
    .read    = procfs_read,
    .readdir = procfs_readdir,
    .finddir = procfs_finddir,
};

vfs_node_t *procfs_create_root(void)
{
    proc_root.type    = VFS_DIR;
    proc_root.fs_name = "procfs";
    proc_root.ops     = &procfs_ops;
    proc_root.inode   = 1;
    rtc_read(&proc_root.ctime);
    proc_root.mtime = proc_root.ctime;

    for (uint32_t i = 0; i < PROC_FILE_COUNT; i++) {
        vfs_node_t *f = &proc_files[i];
        f->type    = VFS_FILE;
        f->fs_name = "procfs";
        f->ops     = &procfs_ops;
        f->inode   = i + 2;
        f->parent  = &proc_root;
        f->ctime   = proc_root.ctime;
        f->mtime   = proc_root.ctime;
    }
    return &proc_root;
}
