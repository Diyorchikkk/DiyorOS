/*
 * syscall.c — tizim chaqiruvlari (system calls), int 0x80.
 *
 * Ring 3 dagi dastur portlarga, ekran xotirasiga, kernel ma'lumotlariga
 * to'g'ridan-to'g'ri tega olmaydi. Biror narsa kerak bo'lsa (ekranga yozish,
 * fayl ochish, uxlash), u "int 0x80" bilan kernelga murojaat qiladi:
 *
 *   dastur:  EAX = SYS_WRITE, EBX = 1, ECX = matn, EDX = uzunlik; int 0x80
 *   kernel:  ring 0 ga o'tadi -> syscall_handler -> ekranga yozadi -> EAX = natija
 *   dastur:  int 0x80 dan keyingi buyruqdan davom etadi
 *
 * XAVFSIZLIK: dastur bergan har bir ko'rsatkich tekshiriladi — u dasturning
 * O'Z xotirasida bo'lishi va sahifalari mavjud bo'lishi shart. Aks holda
 * dastur kernelni o'zi uchun boshqa xotirani o'qishga yoki yozishga majbur
 * qilishi mumkin edi.
 */
#include "process.h"
#include "syscall_nums.h"
#include "task.h"
#include "memory.h"
#include "vfs.h"
#include "vga.h"
#include "drivers.h"
#include "string.h"
#include "config.h"
#include "kprintf.h"
#include "klog.h"

#define EFAULT     (-14)          /* noto'g'ri ko'rsatkich */
#define ENOSYS     (-38)          /* bunday syscall yo'q */
#define IO_MAX     (64 * 1024)    /* bitta read/write uchun maksimum */
#define HEAP_LIMIT (16 * 1024 * 1024)
#define FDS_PER_PROCESS 8         /* bitta dastur ochishi mumkin bo'lgan fayllar */

/* Ko'rsatkich jarayonning o'z xotirasidami? */
static bool user_ok(uint32_t ptr, uint32_t len, bool write)
{
    if (ptr < USER_BASE || ptr >= USER_TOP || len > USER_TOP - ptr)
        return false;
    return paging_check_range(task_current()->page_dir, ptr, len,
                              PAGE_PRESENT | PAGE_USER | (write ? PAGE_RW : 0));
}

/* Dasturdagi satrni kernel buferiga xavfsiz nusxalash */
static bool copy_string(char *dst, uint32_t src, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++) {
        if ((i == 0 || ((src + i) & 0xFFF) == 0) && !user_ok(src + i, 1, false))
            return false;                     /* har yangi sahifada tekshiramiz */
        dst[i] = *(const char *)(src + i);
        if (!dst[i])
            return true;
    }
    return false;                             /* juda uzun */
}

/* fd 0: klaviaturadan bitta qator (yozilganlar ekranda ko'rinadi, Backspace ishlaydi) */
static int32_t read_line(char *buf, uint32_t len)
{
    uint32_t n = 0;
    while (n < len) {
        char c = keyboard_getchar();
        if (c == '\n') {
            vga_putc('\n');
            buf[n++] = '\n';
            break;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                vga_putc('\b');
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            buf[n++] = c;
            vga_putc(c);
        }
    }
    return (int32_t)n;
}

static int32_t sys_write(uint32_t fd, uint32_t buf, uint32_t len)
{
    if (len > IO_MAX)
        len = IO_MAX;
    if (!user_ok(buf, len, false))
        return EFAULT;
    const char *s = (const char *)buf;

    if (fd == 1 || fd == 2) {
        uint8_t old = vga_get_color();
        if (fd == 2)
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        for (uint32_t i = 0; i < len; i++)
            vga_putc(s[i]);
        if (fd == 2)
            vga_set_color(old & 0x0F, old >> 4);
        return (int32_t)len;
    }
    if (!vfs_fd_owned(fd, task_current()->pid))
        return VFS_EBADF;                     /* boshqa jarayonning fayli */
    return vfs_write(fd, s, len);
}

static int32_t sys_read(uint32_t fd, uint32_t buf, uint32_t len)
{
    if (len > IO_MAX)
        len = IO_MAX;
    if (!user_ok(buf, len, true))
        return EFAULT;
    if (fd == 0) {
        /*
         * Klaviatura bitta: uni faqat OLDINGI PLANDAGI dastur o'qiy oladi.
         * Fon jarayoni ("dastur &") o'qimoqchi bo'lsa, u shell bilan tugmalarni
         * talashib qolardi. Unix da bunday jarayonga SIGTTIN yuboriladi; biz
         * uni xabar bilan to'xtatamiz.
         */
        task_t *t = task_current();
        if (task_foreground() != t->pid) {
            uint8_t old = vga_get_color();
            vga_set_color(VGA_DARK_GREY, VGA_BLACK);
            kprintf("\n[PID %u] %s: fon jarayoni klaviaturadan o'qiy olmaydi - to'xtatildi\n",
                    t->pid, t->name);
            vga_set_color(old & 0x0F, old >> 4);
            task_exit_code(EXIT_TTIN);         /* qaytmaydi */
        }
        return read_line((char *)buf, len);
    }
    if (!vfs_fd_owned(fd, task_current()->pid))
        return VFS_EBADF;
    return vfs_read(fd, (void *)buf, len);
}

static int32_t sys_open(uint32_t upath, uint32_t flags)
{
    char path[VFS_PATH_MAX];
    if (!copy_string(path, upath, sizeof(path)))
        return EFAULT;
    if (vfs_fd_count(task_current()->pid) >= FDS_PER_PROCESS)
        return VFS_EMFILE;                    /* bitta dastur butun jadvalni to'ldirib qo'ymasin */
    return vfs_open(path, flags & (VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC | VFS_O_APPEND));
}

static int32_t sys_close(uint32_t fd)
{
    if (!vfs_fd_owned(fd, task_current()->pid))
        return VFS_EBADF;
    return vfs_close(fd);
}

/* Dastur heap ini kattalashtirish: yangi sahifalar ajratib, xaritaga qo'shamiz */
static int32_t sys_sbrk(int32_t increment)
{
    task_t *t = task_current();
    uint32_t old = t->brk;
    if (increment == 0)
        return (int32_t)old;
    uint32_t new_brk = old + (uint32_t)increment;
    if (increment < 0) {
        /* new_brk > old — ayirish 0 dan pastga "aylanib" ketgan (integer underflow) */
        if (new_brk > old || new_brk < t->brk_start)
            return -1;
        t->brk = new_brk;                     /* soddalik uchun sahifalarni qaytarmaymiz */
        return (int32_t)old;
    }
    if (new_brk < old || new_brk > t->brk_start + HEAP_LIMIT ||
        new_brk > USER_STACK_TOP - USER_STACK_SIZE)
        return -1;

    for (uint32_t page = (old + 0xFFF) & ~0xFFFu; page < new_brk; page += PAGE_SIZE) {
        /* Heap avval kichraytirilgan bo'lsa, sahifa hali xaritada turibdi: qayta
           ajratsak, eski frame yo'qolib qolardi (xotira oqishi). Uni tozalab ishlatamiz */
        uint32_t phys;
        if (paging_translate_in(t->page_dir, page, &phys, 0)) {
            memset((void *)(phys & ~0xFFFu), 0, PAGE_SIZE);
            continue;
        }
        uint32_t frame = pmm_alloc_frame();
        if (!frame)
            return -1;
        memset((void *)frame, 0, PAGE_SIZE);
        if (!paging_map_in(t->page_dir, page, frame, PAGE_USER | PAGE_RW)) {
            pmm_free_frame(frame);
            return -1;
        }
        t->user_pages++;
    }
    t->brk = new_brk;
    return (int32_t)old;
}

static int32_t sys_sysinfo(uint32_t ptr)
{
    if (!user_ok(ptr, sizeof(struct sysinfo), true))
        return EFAULT;
    struct sysinfo *si = (struct sysinfo *)ptr;
    memset(si, 0, sizeof(*si));
    si->mem_total_kb = pmm_total_frames() * 4;
    si->mem_free_kb  = (pmm_total_frames() - pmm_used_frames()) * 4;
    si->uptime_ms    = timer_ticks() * (1000 / TIMER_HZ);
    si->pid          = task_current()->pid;
    si->user_pages   = task_current()->user_pages;
    for (int i = 0; i < MAX_TASKS; i++)
        if (task_get(i)->state != TASK_UNUSED)
            si->task_count++;
    strncpy(si->os_name, OS_NAME, sizeof(si->os_name) - 1);
    strncpy(si->os_version, OS_VERSION, sizeof(si->os_version) - 1);
    return 0;
}

void syscall_handler(registers_t *r)
{
    uint32_t a = r->ebx, b = r->ecx, c = r->edx;
    int32_t ret;

    switch (r->eax) {
    case SYS_EXIT:     task_exit_code((int32_t)a); return;   /* qaytmaydi */
    case SYS_WRITE:    ret = sys_write(a, b, c); break;
    case SYS_READ:     ret = sys_read(a, b, c); break;
    case SYS_OPEN:     ret = sys_open(a, b); break;
    case SYS_CLOSE:    ret = sys_close(a); break;
    case SYS_SLEEP:    timer_sleep_ms(a); ret = 0; break;
    case SYS_GETPID:   ret = (int32_t)task_current()->pid; break;
    case SYS_UPTIME:   ret = (int32_t)(timer_ticks() * (1000 / TIMER_HZ)); break;
    case SYS_YIELD:    task_yield(); ret = 0; break;
    case SYS_SBRK:     ret = sys_sbrk((int32_t)a); break;
    case SYS_SYSINFO:  ret = sys_sysinfo(a); break;
    case SYS_SETCOLOR: vga_set_color(a & 0x0F, VGA_BLACK); ret = 0; break;
    case SYS_CLEAR:    vga_clear(); ret = 0; break;
    default:
        klog("PID %u (%s): noma'lum syscall %u", task_current()->pid, task_current()->name, r->eax);
        ret = ENOSYS;
        break;
    }
    r->eax = (uint32_t)ret;                   /* natija dasturga EAX orqali qaytadi */
}
