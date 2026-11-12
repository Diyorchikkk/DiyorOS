/*
 * process.c — user dasturlarni ishga tushirish (v0.5).
 *
 * "hello" deb yozilganda nima bo'ladi:
 *   1. /bin/hello fayli VFS orqali xotiraga o'qiladi.
 *   2. ELF sarlavhasi tekshiriladi (sehrli raqam, 32-bit, x86, bajariladigan).
 *   3. Jarayon uchun YANGI Page Directory yaratiladi (alohida xotira).
 *   4. Har bir PT_LOAD segment uchun frame lar ajratilib, 0x40000000 dan
 *      boshlab xaritaga qo'shiladi va fayldagi kod/ma'lumot nusxalanadi.
 *      Kod segmenti faqat O'QISH uchun — dastur o'z kodini buzolmaydi.
 *   5. Stek (64 KB) 0xC0000000 dan pastda ajratiladi, unga argc/argv yoziladi.
 *   6. Ring 3 vazifa yaratiladi — scheduler uni navbat bilan ishga tushiradi.
 */
#include "process.h"
#include "elf.h"
#include "task.h"
#include "memory.h"
#include "vfs.h"
#include "string.h"
#include "kprintf.h"
#include "vga.h"
#include "klog.h"
#include "irq.h"

#define MAX_ARGS      16
#define EXEC_MAX      (1024 * 1024)
#define ELF_MAX_PHDRS 16

static inline uint32_t page_down(uint32_t a) { return a & ~0xFFFu; }
static inline uint32_t page_up(uint32_t a)   { return (a + 0xFFF) & ~0xFFFu; }

/*
 * Boshqa jarayonning xotirasiga yozish. Jarayon hali ishlamagan, uning
 * Page Directory si CR3 da emas. Lekin har bir frame identity-map da,
 * shuning uchun virtual manzilni fizikka aylantirib, to'g'ridan-to'g'ri yozamiz.
 */
static bool copy_to_dir(uint32_t *dir, uint32_t vaddr, const void *src, uint32_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint32_t phys;
        if (!paging_translate_in(dir, vaddr, &phys, 0))
            return false;
        uint32_t chunk = PAGE_SIZE - (vaddr & 0xFFF);
        if (chunk > len)
            chunk = len;
        memcpy((void *)phys, s, chunk);
        vaddr += chunk;
        s += chunk;
        len -= chunk;
    }
    return true;
}

/* [start, end) oralig'iga sahifalar ajratish (allaqachon bor bo'lsa — ruxsatini kengaytirish) */
static bool map_user_range(uint32_t *dir, uint32_t start, uint32_t end, bool writable, uint32_t *pages)
{
    for (uint32_t page = page_down(start); page < end; page += PAGE_SIZE) {
        uint32_t phys, flags;
        if (paging_translate_in(dir, page, &phys, &flags)) {
            if (writable && !(flags & PAGE_RW))
                paging_map_in(dir, page, phys, flags | PAGE_RW);
            continue;
        }
        uint32_t frame = pmm_alloc_frame();
        if (!frame)
            return false;
        memset((void *)frame, 0, PAGE_SIZE);   /* boshqa jarayonning eski ma'lumoti qolmasin */
        if (!paging_map_in(dir, page, frame, PAGE_USER | (writable ? PAGE_RW : 0))) {
            pmm_free_frame(frame);
            return false;
        }
        (*pages)++;
    }
    return true;
}

static int32_t load_elf(uint32_t *dir, const uint8_t *file, uint32_t size,
                        uint32_t *entry, uint32_t *image_end, uint32_t *pages)
{
    const elf_header_t *h = (const elf_header_t *)file;
    if (size < sizeof(*h) || h->magic != ELF_MAGIC || h->class != ELF_CLASS32 ||
        h->data != ELF_LSB || h->type != ET_EXEC || h->machine != EM_386)
        return PROC_ENOEXEC;
    /*
     * Fayldagi har bir son SHUBHALI deb qaraladi: buzilgan yoki ataylab
     * tayyorlangan ELF kernelni fayl chegarasidan tashqarini o'qishga majbur
     * qilmasligi kerak. "a + b > size" ko'rinishidagi tekshiruv yetarli emas:
     * a + b 32 bitdan toshib, kichik son bo'lib qolishi mumkin (integer
     * overflow). Shuning uchun ayirish bilan tekshiramiz: "b > size - a".
     */
    if (h->phentsize != sizeof(elf_phdr_t) || h->phnum == 0 || h->phnum > ELF_MAX_PHDRS)
        return PROC_ENOEXEC;
    if (h->phoff > size || (uint32_t)h->phnum * sizeof(elf_phdr_t) > size - h->phoff)
        return PROC_ENOEXEC;
    if (h->entry < USER_BASE || h->entry >= USER_STACK_TOP - USER_STACK_SIZE)
        return PROC_ENOEXEC;

    uint32_t limit = USER_STACK_TOP - USER_STACK_SIZE;
    *image_end = USER_BASE;
    bool entry_ok = false;

    for (int i = 0; i < h->phnum; i++) {
        const elf_phdr_t *ph = (const elf_phdr_t *)(file + h->phoff + i * sizeof(elf_phdr_t));
        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        /* Segment faqat user hududida bo'lishi shart — kernel ga yozdirmaymiz */
        if (ph->vaddr < USER_BASE || ph->vaddr >= limit || ph->memsz > limit - ph->vaddr ||
            ph->filesz > ph->memsz ||
            ph->offset > size || ph->filesz > size - ph->offset)
            return PROC_ENOEXEC;
        if ((ph->flags & PF_X) && h->entry >= ph->vaddr && h->entry - ph->vaddr < ph->memsz)
            entry_ok = true;                   /* kirish nuqtasi bajariladigan segment ichida */

        if (!map_user_range(dir, ph->vaddr, ph->vaddr + ph->memsz, ph->flags & PF_W, pages))
            return PROC_ENOMEM;
        if (!copy_to_dir(dir, ph->vaddr, file + ph->offset, ph->filesz))
            return PROC_ENOMEM;
        /* filesz..memsz oralig'i (.bss) — frame lar allaqachon nol bilan to'ldirilgan */

        if (ph->vaddr + ph->memsz > *image_end)
            *image_end = ph->vaddr + ph->memsz;
    }
    if (!entry_ok)
        return PROC_ENOEXEC;
    *entry = h->entry;
    return 0;
}

/*
 * Stekka argc va argv ni joylash (C dagi main(argc, argv) uchun):
 *
 *   0xC0000000  ->  [ "hello\0" "abc\0" ... ]   satrlar
 *                   [ argv[0] argv[1] ... NULL ]
 *                   [ argv ko'rsatkichi ]
 *   ESP         ->  [ argc ]
 */
static int32_t setup_stack(uint32_t *dir, const char *name, const char *args, uint32_t *esp_out)
{
    char buf[256];
    strncpy(buf, args ? args : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    const char *argv[MAX_ARGS + 1];
    int argc = 0;
    argv[argc++] = name;
    for (char *p = buf; *p && argc < MAX_ARGS;) {
        while (*p == ' ')
            *p++ = 0;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
    }

    uint32_t sp = USER_STACK_TOP;
    uint32_t addrs[MAX_ARGS + 1];
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = strlen(argv[i]) + 1;
        sp -= len;
        if (!copy_to_dir(dir, sp, argv[i], len))
            return PROC_ENOMEM;
        addrs[i] = sp;
    }
    addrs[argc] = 0;
    sp &= ~3u;

    sp -= (argc + 1) * 4;
    uint32_t argv_addr = sp;
    if (!copy_to_dir(dir, sp, addrs, (argc + 1) * 4))
        return PROC_ENOMEM;

    uint32_t frame[2] = { (uint32_t)argc, argv_addr };
    sp -= sizeof(frame);
    if (!copy_to_dir(dir, sp, frame, sizeof(frame)))
        return PROC_ENOMEM;

    *esp_out = sp;
    return 0;
}

int32_t process_spawn(const char *path, const char *args)
{
    vfs_node_t *node;
    int err = vfs_lookup(path, &node);
    if (err)
        return err;
    if (node->type == VFS_DIR)
        return VFS_EISDIR;
    if (node->size == 0 || node->size > EXEC_MAX)
        return PROC_ENOEXEC;

    /* 1. Faylni xotiraga o'qish */
    uint32_t size = node->size;
    uint8_t *file = kmalloc(size);
    if (!file)
        return PROC_ENOMEM;
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        kfree(file);
        return fd;
    }
    uint32_t got = 0;
    int32_t n = 0;
    while (got < size && (n = vfs_read(fd, file + got, size - got)) > 0)
        got += n;
    vfs_close(fd);
    if (n < 0) {                               /* o'qishda xato (masalan, disk) */
        kfree(file);
        return n;
    }

    /* 2-5. Alohida xotira, segmentlar, stek */
    uint32_t *dir = paging_create_directory();
    if (!dir) {
        kfree(file);
        return PROC_ENOMEM;
    }
    uint32_t entry = 0, image_end = 0, pages = 0, esp = 0;
    int32_t result = load_elf(dir, file, got, &entry, &image_end, &pages);
    kfree(file);

    if (!result && !map_user_range(dir, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, true, &pages))
        result = PROC_ENOMEM;

    const char *name = vfs_basename(path);
    if (!result)
        result = setup_stack(dir, name, args, &esp);

    if (result) {
        paging_free_directory(dir);
        return result;
    }

    /* 6. Ring 3 vazifa */
    uint32_t pid = task_create_user(name, dir, entry, esp, page_up(image_end), pages);
    if (!pid) {
        paging_free_directory(dir);
        return PROC_ETASKS;
    }
    klog("exec: PID %u %s (%u sahifa, entry 0x%x)", pid, path, pages, entry);
    return (int32_t)pid;
}

const char *process_strerror(int32_t err)
{
    switch (err) {
    case PROC_ENOEXEC: return "bajariladigan fayl emas (ELF format noto'g'ri)";
    case PROC_ENOMEM:  return "xotira yetmadi";
    case PROC_ETASKS:  return "vazifalar jadvali to'la";
    default:           return vfs_strerror(err);
    }
}

/*
 * Dastur (ring 3) exception qildi: 0 ga bo'lish, NULL pointer, kernel
 * xotirasiga tegish, cli kabi taqiqlangan buyruq...
 * v0.4 gacha bunday xato butun tizimni to'xtatardi (kernel panic).
 * Endi faqat shu jarayon o'ldiriladi — kernel va boshqa dasturlar ishlayveradi.
 */
void process_fault(registers_t *r, const char *exception_name)
{
    task_t *t = task_current();
    uint8_t old = vga_get_color();
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("\n[PID %u] %s: Segmentation fault - %s\n", t->pid, t->name, exception_name);
    if (r->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  Manzil 0x%08x: %s, %s\n", cr2,
                (r->err_code & 1) ? "ruxsat yo'q" : "sahifa yo'q",
                (r->err_code & 2) ? "yozishda" : "o'qishda");
        if (cr2 < USER_BASE || cr2 >= USER_TOP)
            kprintf("  (bu manzil dastur hududidan tashqarida: 0x40000000 - 0xBFFFFFFF)\n");
    } else if (r->int_no == 13) {
        kprintf("  Taqiqlangan buyruq yoki segment (EIP 0x%08x)\n", r->eip);
    }
    kprintf("  Jarayon to'xtatildi. Kernel va boshqa dasturlar ishlashda davom etadi.\n");
    vga_set_color(old & 0x0F, old >> 4);
    klog("PID %u (%s): %s, EIP 0x%08x - jarayon to'xtatildi", t->pid, t->name, exception_name, r->eip);
    task_exit_code(EXIT_FAULT);
}

/* ---------------------------- initrd: /bin ---------------------------- */

/*
 * Dasturlar kernel bilan birga yig'iladi va kernel fayliga "yopishtiriladi"
 * (user/initrd.s, .incbin). Yuklanishda ular /bin ga oddiy fayl sifatida
 * yoziladi. Shundan keyin ular VFS orqali o'qiladi — boshqa fayllar kabi
 * (cp bilan nusxalasa, mv bilan ko'chirsa ham bo'ladi).
 */
typedef struct {
    const char    *name;
    const uint8_t *start;
    const uint8_t *end;
} initrd_entry_t;

extern const initrd_entry_t initrd_table[] __attribute__((weak));

int initrd_install(const char *dir)
{
    if (!initrd_table)
        return 0;                      /* dasturlarsiz yig'ilgan (eski Makefile) */
    int count = 0;
    char path[VFS_PATH_MAX];
    for (const initrd_entry_t *e = initrd_table; e->name; e++) {
        ksnprintf(path, sizeof(path), "%s/%s", dir, e->name);
        int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (fd < 0)
            continue;
        vfs_write(fd, e->start, (uint32_t)(e->end - e->start));
        vfs_close(fd);
        count++;
    }
    return count;
}
