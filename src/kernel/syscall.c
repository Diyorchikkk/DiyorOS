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
 *
 * v0.8: syscall ichida uzilishlar YOQIQ (syscall_handler boshidagi sti).
 * Avval butun syscall uzilishlarsiz bajarilardi: dastur diskka yozayotganda
 * taymer ham, sichqoncha ham kutardi. Endi syscall o'rtasida ham boshqa
 * vazifaga o'tish mumkin — umumiy ma'lumotlar mutexlar bilan himoyalangan
 * (vfs, ata, wm). Bu "preemptible kernel" deyiladi.
 *
 * Standart kirish/chiqish (stdin/stdout): jarayonning fd_in / fd_out i
 * o'rnatilgan bo'lsa (pipe: "ls | wc"), fd 0 / fd 1 o'sha kanalga yo'naltiriladi.
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
#include "gui.h"
#include "sync.h"

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
        if (c == 0 && task_should_abort())
            return n ? (int32_t)n : VFS_EINTR;   /* Ctrl+C: kutish to'xtatildi */
        if (c == KEY_DELETE && n == 0)       /* Ctrl+D bo'sh qatorda — kiritish tugadi (EOF) */
            return 0;
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
    task_t *t = task_current();

    if (fd == 1 && t->fd_out >= 0) {          /* stdout kanalga (yoki faylga) yo'naltirilgan */
        int32_t r = vfs_write(t->fd_out, s, len);
        if (r == VFS_EPIPE) {
            /* O'qiydigan tomon yopilgan (masalan "ota | grep" da grep tugadi).
               Unix dagidek (SIGPIPE) jarayon jimgina to'xtatiladi */
            task_signal_kill(t->pid, EXIT_PIPE);
        }
        return r;
    }
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
    if (!vfs_fd_owned(fd, t->pid))
        return VFS_EBADF;                     /* boshqa jarayonning fayli */
    int32_t r = vfs_write(fd, s, len);
    if (r == VFS_EPIPE)
        task_signal_kill(t->pid, EXIT_PIPE);
    return r;
}

static int32_t sys_read(uint32_t fd, uint32_t buf, uint32_t len)
{
    if (len > IO_MAX)
        len = IO_MAX;
    if (!user_ok(buf, len, true))
        return EFAULT;
    task_t *t = task_current();
    if (fd == 0 && t->fd_in >= 0)             /* stdin kanaldan */
        return vfs_read(t->fd_in, (void *)buf, len);
    if (fd == 0) {
        /*
         * Klaviatura bitta: uni faqat OLDINGI PLANDAGI dastur o'qiy oladi.
         * Fon jarayoni ("dastur &") o'qimoqchi bo'lsa, u shell bilan tugmalarni
         * talashib qolardi. Unix da bunday jarayonga SIGTTIN yuboriladi; biz
         * uni xabar bilan to'xtatamiz. v0.8: oldingi planda butun jarayon
         * guruhi (pipeline) turadi, shuning uchun pgid bilan solishtiramiz.
         */
        if (task_foreground() != t->pgid) {
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
    si->stdio = (task_current()->fd_in >= 0 ? SI_STDIN_PIPE : 0) |
                (task_current()->fd_out >= 0 ? SI_STDOUT_PIPE : 0);
    return 0;
}

/* ------------------------- Grafik oynalar (v0.7) ------------------------- */

static int32_t sys_win_create(uint32_t utitle, uint32_t w, uint32_t h)
{
    char title[40];
    if (!copy_string(title, utitle, sizeof(title)))
        return EFAULT;
    if (w > 4096 || h > 4096)
        return -22;
    return gui_sys_create(title, (int)w, (int)h);
}

static int32_t sys_win_update(uint32_t id, uint32_t upixels)
{
    int w, h;
    if (!gui_sys_window_size((int)id, &w, &h))
        return -2;
    /* Dastur bergan bufer butunligicha o'zining xotirasida bo'lishi shart */
    if (!user_ok(upixels, (uint32_t)w * (uint32_t)h * 4, false))
        return EFAULT;
    return gui_sys_update((int)id, (const uint32_t *)upixels);
}

static int32_t sys_win_event(uint32_t id, uint32_t uev)
{
    if (!user_ok(uev, sizeof(struct gui_event), true))
        return EFAULT;
    return gui_sys_event((int)id, (struct gui_event *)uev);
}

/* ---------------------- Jarayonlar va kanallar (v0.8) ---------------------- */

/*
 * Bolaning stdin/stdout i uchun deskriptor tayyorlash:
 *   want >= 0 — ota bergan o'z deskriptori (masalan, pipe uchi);
 *   want < 0  — otaning o'zidagi yo'naltirish meros qoladi (bo'lsa).
 * Har doim NUSXA (dup) olinadi: ota o'z deskriptorini o'zi yopadi.
 */
static int child_fd(int want, int inherited, uint32_t pid, int *out)
{
    *out = -1;
    int src = want >= 0 ? want : inherited;
    if (src < 0)
        return 0;
    if (want >= 0 && !vfs_fd_owned(want, pid))
        return VFS_EBADF;
    int fd = vfs_dup(src, pid);
    if (fd < 0)
        return fd;
    *out = fd;
    return 0;
}

/* spawn(yo'l, argumentlar, io) — io: 0 yoki int[2] = {stdin, stdout}, -1 — meros */
static int32_t sys_spawn(uint32_t upath, uint32_t uargs, uint32_t uio)
{
    char path[VFS_PATH_MAX], args[128];
    if (!copy_string(path, upath, sizeof(path)))
        return EFAULT;
    args[0] = 0;
    if (uargs && !copy_string(args, uargs, sizeof(args)))
        return EFAULT;
    int io[2] = { -1, -1 };
    if (uio) {
        if (!user_ok(uio, sizeof(io), false))
            return EFAULT;
        memcpy(io, (const void *)uio, sizeof(io));
    }

    task_t *t = task_current();
    int children = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *k = task_get(i);
        if (k->state != TASK_UNUSED && k->parent_pid == t->pid)
            children++;
    }
    if (children >= 4)
        return PROC_ETASKS;                   /* "fork bomb" bo'lmasin: bittada ko'pi bilan 4 bola */

    spawn_opts_t o = { .fd_in = -1, .fd_out = -1, .pgid = t->pgid };
    int err = child_fd(io[0], t->fd_in, t->pid, &o.fd_in);
    if (!err)
        err = child_fd(io[1], t->fd_out, t->pid, &o.fd_out);
    if (!err) {
        int32_t pid = process_spawn_ex(path, args, &o);
        if (pid > 0)
            return pid;                       /* deskriptorlar endi bolaniki */
        err = pid;
    }
    if (o.fd_in >= 0)
        vfs_close(o.fd_in);
    if (o.fd_out >= 0)
        vfs_close(o.fd_out);
    return err;
}

/* wait(pid) — o'z bolasining tugashini kutish; chiqish kodini qaytaradi */
static int32_t sys_wait(uint32_t pid, uint32_t ucode)
{
    task_t *t = task_current();
    task_t *k = task_find(pid);
    if (!k || k->parent_pid != t->pid || k == t)
        return VFS_ENOENT;                    /* bunday bola yo'q */
    if (ucode && !user_ok(ucode, 4, true))
        return EFAULT;
    int32_t code = task_wait(pid);
    if (code == SYNC_EINTR && task_should_abort())
        return VFS_EINTR;
    task_reap_pid(pid);                       /* zombi bolani tozalaymiz */
    if (ucode)
        *(int32_t *)ucode = code;
    return 0;
}

static int32_t sys_pipe(uint32_t ufds)
{
    if (!user_ok(ufds, 8, true))
        return EFAULT;
    if (vfs_fd_count(task_current()->pid) + 2 > FDS_PER_PROCESS)
        return VFS_EMFILE;
    int fds[2];
    int err = vfs_pipe(fds);
    if (err)
        return err;
    memcpy((void *)ufds, fds, sizeof(fds));
    return 0;
}

void syscall_handler(registers_t *r)
{
    uint32_t a = r->ebx, b = r->ecx, c = r->edx;
    int32_t ret;

    /* v0.8: syscall davomida uzilishlar yoqiq — kernel "preemptible" */
    __asm__ volatile ("sti");

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
    case SYS_WIN_CREATE: ret = sys_win_create(a, b, c); break;
    case SYS_WIN_UPDATE: ret = sys_win_update(a, b); break;
    case SYS_WIN_EVENT:  ret = sys_win_event(a, b); break;
    case SYS_WIN_CLOSE:  ret = gui_sys_close((int)a); break;
    case SYS_SPAWN:      ret = sys_spawn(a, b, c); break;
    case SYS_WAIT:       ret = sys_wait(a, b); break;
    case SYS_PIPE:       ret = sys_pipe(a); break;
    default:
        klog("PID %u (%s): noma'lum syscall %u", task_current()->pid, task_current()->name, r->eax);
        ret = ENOSYS;
        break;
    }
    __asm__ volatile ("cli");                 /* qaytish yo'li (iret) uzilishlarsiz */
    r->eax = (uint32_t)ret;                   /* natija dasturga EAX orqali qaytadi */
}
