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
 * v0.10: har jarayonning O'Z deskriptor jadvali (task_t.fd). fd 0/1/2 endi
 * oddiy deskriptorlar: odatda "konsol" (klaviatura/ekran), lekin pipe yoki
 * faylga ulangan bo'lishi mumkin ("ls | wc", "dastur > fayl", dup2). Dastur
 * faqat o'z jadvalidagi raqamlarni ishlata oladi — boshqa jarayonning fayliga
 * yetib borishning yo'li yo'q (avval buning uchun egasini tekshirish kerak edi).
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

/* Ko'rsatkich jarayonning o'z xotirasidami? */
static bool user_ok(uint32_t ptr, uint32_t len, bool write)
{
    if (ptr < USER_BASE || ptr >= USER_TOP || len > USER_TOP - ptr)
        return false;
    uint32_t *dir = task_current()->page_dir;
    /* v0.10: kernel bu buferga yozadi. fork dan keyingi "copy-on-write" sahifalar
       hozircha faqat o'qish uchun — nusxasini oldindan olamiz (aks holda kernel
       o'zi Page Fault ga tushardi) */
    if (write && !paging_make_writable(dir, ptr, len))
        return false;
    return paging_check_range(dir, ptr, len, PAGE_PRESENT | PAGE_USER | (write ? PAGE_RW : 0));
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

/* Ekranga jarayonning O'Z rangida yozish (v0.8: rang har bir jarayonniki — ikki dastur
   bir vaqtda yozsa ham bir-birining rangini buzmaydi) */
static void console_write(const task_t *t, const char *s, uint32_t len, bool is_error)
{
    uint8_t color = is_error ? (VGA_LIGHT_RED | (VGA_BLACK << 4)) : t->color;
    for (uint32_t i = 0; i < len; i++)
        vga_putc_color(s[i], color);
}

/* fd 0: klaviaturadan bitta qator (yozilganlar ekranda ko'rinadi, Backspace ishlaydi) */
static int32_t read_line(char *buf, uint32_t len)
{
    uint8_t color = task_current()->color;
    uint32_t n = 0;
    while (n < len) {
        char c = keyboard_getchar();
        if (c == 0 && task_should_abort())
            return n ? (int32_t)n : VFS_EINTR;   /* Ctrl+C: kutish to'xtatildi */
        if (c == KEY_DELETE && n == 0)       /* Ctrl+D bo'sh qatorda — kiritish tugadi (EOF) */
            return 0;
        if (c == '\n') {
            vga_putc_color('\n', color);
            buf[n++] = '\n';
            break;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                vga_putc_color('\b', color);
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            buf[n++] = c;
            vga_putc_color(c, color);
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
    int kind = vfs_fd_kind((int)fd);
    if (kind == VFS_EBADF)
        return VFS_EBADF;
    if (kind == FD_CONSOLE) {                 /* ekran; fd 2 (xatolar) — qizil */
        console_write(t, s, len, fd == 2);
        return (int32_t)len;
    }

    int32_t r = vfs_write((int)fd, s, len);
    if (r == VFS_EPIPE) {
        /* O'qiydigan tomon yopilgan (masalan "ota | grep" da grep tugadi).
           Unix dagidek (SIGPIPE) jarayon jimgina to'xtatiladi */
        task_signal_kill(t->pid, EXIT_PIPE);
    } else if (fd == 1 && r != VFS_EINTR && r < (int32_t)len && !t->out_failed && !task_should_abort()) {
            /* "dastur > /disk/fayl": yozib bo'lmadi (odatda diskda joy tugagan). Dasturlar
           printf natijasini tekshirmaydi — foydalanuvchi bexabar qolmasligi uchun
           bir marta ekranga aytamiz */
        t->out_failed = true;
        char msg[96];
        int n = ksnprintf(msg, sizeof(msg), "[PID %u] %s: chiqishni yozib bo'lmadi: %s\n",
                          t->pid, t->name, vfs_strerror(r < 0 ? r : VFS_ENOSPC));
        console_write(t, msg, (uint32_t)n, true);
    }
    return r;
}

static int32_t sys_read(uint32_t fd, uint32_t buf, uint32_t len)
{
    if (len > IO_MAX)
        len = IO_MAX;
    if (!user_ok(buf, len, true))
        return EFAULT;
    task_t *t = task_current();
    int kind = vfs_fd_kind((int)fd);
    if (kind == VFS_EBADF)
        return VFS_EBADF;
    if (kind == FD_CONSOLE) {                 /* klaviatura */
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
    return vfs_read((int)fd, (void *)buf, len);   /* fayl yoki kanal */
}

static int32_t sys_open(uint32_t upath, uint32_t flags)
{
    char path[VFS_PATH_MAX];
    if (!copy_string(path, upath, sizeof(path)))
        return EFAULT;
    /* Jadval to'lsa (TASK_MAX_FDS) — VFS_EMFILE: bitta dastur tizimdagi barcha ochiq
       fayl yozuvlarini egallab ololmaydi */
    return vfs_open(path, flags & (VFS_O_READ | VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC | VFS_O_APPEND));
}

static int32_t sys_close(uint32_t fd)
{
    return vfs_close((int)fd);                /* faqat o'z jadvalidagi raqam */
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
            /* v0.10: fork dan keyin bu frame aka-ukasi bilan umumiy bo'lishi mumkin —
               avval o'z nusxasini olamiz, aks holda ikkalasining xotirasi tozalanardi */
            if (!paging_make_writable(t->page_dir, page, PAGE_SIZE))
                return -1;
            paging_translate_in(t->page_dir, page, &phys, 0);
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
    si->stdio = (vfs_fd_kind(0) != FD_CONSOLE ? SI_STDIN_PIPE : 0) |
                (vfs_fd_kind(1) != FD_CONSOLE ? SI_STDOUT_PIPE : 0);
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
    if (task_count_children(t->pid) >= TASK_MAX_CHILDREN)
        return PROC_ETASKS;                   /* "fork bomb" bo'lmasin: bittada ko'pi bilan 4 bola */

    /* io dagi raqamlar — shu jarayonning o'z deskriptorlari; bola ularning nusxasini oladi */
    spawn_opts_t o = { .fd_in = io[0], .fd_out = io[1], .pgid = t->pgid };
    return process_spawn_ex(path, args, &o);
}

/*
 * wait(pid, &kod) — bolaning tugashini kutish. v0.10: pid = 0 (yoki -1) — istalgan
 * bola. Tugagan bolaning PID ini qaytaradi; bolasi yo'q bo'lsa — VFS_ENOENT.
 */
static int32_t sys_wait(uint32_t pid, uint32_t ucode)
{
    if (ucode && !user_ok(ucode, 4, true))
        return EFAULT;
    int32_t code = 0;
    int32_t cpid = task_wait_child((int32_t)pid, &code);
    if (cpid == SYNC_EINTR)
        return VFS_EINTR;
    if (cpid < 0)
        return VFS_ENOENT;                    /* bunday bola yo'q */
    task_reap_pid((uint32_t)cpid);            /* zombi bolani tozalaymiz */
    if (ucode)
        *(int32_t *)ucode = code;
    return cpid;
}

static int32_t sys_pipe(uint32_t ufds)
{
    if (!user_ok(ufds, 8, true))
        return EFAULT;
    int fds[2];
    int err = vfs_pipe(fds);                  /* joy bo'lmasa — VFS_EMFILE */
    if (err)
        return err;
    memcpy((void *)ufds, fds, sizeof(fds));
    return 0;
}

/* ------------------------- fork, exec, dup (v0.10) ------------------------- */

static int32_t sys_fork(registers_t *r)
{
    task_t *t = task_current();
    if (task_count_children(t->pid) >= TASK_MAX_CHILDREN)
        return PROC_ETASKS;
    int32_t pid = task_fork(r);
    if (pid > 0)
        klog("fork: PID %u (%s) -> PID %d", t->pid, t->name, pid);
    return pid;
}

/* exec(yo'l, argumentlar) — muvaffaqiyatda qaytmaydi (dastur almashadi) */
static int32_t sys_exec(registers_t *r, uint32_t upath, uint32_t uargs)
{
    char path[VFS_PATH_MAX], args[128];
    if (!copy_string(path, upath, sizeof(path)))
        return EFAULT;
    args[0] = 0;
    if (uargs && !copy_string(args, uargs, sizeof(args)))
        return EFAULT;
    return process_exec(r, path, args);
}

static int32_t sys_chdir(uint32_t upath)
{
    char path[VFS_PATH_MAX];
    if (!copy_string(path, upath, sizeof(path)))
        return EFAULT;
    return vfs_chdir(path);                   /* faqat SHU jarayonning papkasi o'zgaradi */
}

static int32_t sys_getcwd(uint32_t ubuf, uint32_t size)
{
    char path[VFS_PATH_MAX];
    int err = vfs_get_path(vfs_cwd(), path, sizeof(path));
    if (err)
        return err;
    uint32_t len = strlen(path);
    if (len + 1 > size)
        return VFS_EINVAL;                    /* bufer kichik */
    if (!user_ok(ubuf, len + 1, true))
        return EFAULT;
    memcpy((void *)ubuf, path, len + 1);
    return (int32_t)len;
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
    case SYS_SETCOLOR: task_current()->color = a & 0x0F; ret = 0; break;   /* fon — qora */
    case SYS_CLEAR:    vga_clear(); ret = 0; break;
    case SYS_WIN_CREATE: ret = sys_win_create(a, b, c); break;
    case SYS_WIN_UPDATE: ret = sys_win_update(a, b); break;
    case SYS_WIN_EVENT:  ret = sys_win_event(a, b); break;
    case SYS_WIN_CLOSE:  ret = gui_sys_close((int)a); break;
    case SYS_SPAWN:      ret = sys_spawn(a, b, c); break;
    case SYS_WAIT:       ret = sys_wait(a, b); break;
    case SYS_PIPE:       ret = sys_pipe(a); break;
    case SYS_FORK:       ret = sys_fork(r); break;
    case SYS_EXEC:       ret = sys_exec(r, a, b); break;
    case SYS_DUP:        ret = vfs_dup((int)a); break;
    case SYS_DUP2:       ret = vfs_dup2((int)a, (int)b); break;
    case SYS_CHDIR:      ret = sys_chdir(a); break;
    case SYS_GETCWD:     ret = sys_getcwd(a, b); break;
    default:
        klog("PID %u (%s): noma'lum syscall %u", task_current()->pid, task_current()->name, r->eax);
        ret = ENOSYS;
        break;
    }
    __asm__ volatile ("cli");                 /* qaytish yo'li (iret) uzilishlarsiz */
    r->eax = (uint32_t)ret;                   /* natija dasturga EAX orqali qaytadi */
}
