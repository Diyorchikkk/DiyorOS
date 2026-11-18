/*
 * selftest.c — kernelning o'z-o'zini tekshirishi (v0.11).
 *
 *   tekshir    — hamma guruhlar, faqat xatolar batafsil
 *   tekshir -v    — har bir tekshiruv
 *
 * Har bir guruh kernelning bitta qismini ALOHIDA tekshiradi (unit test): funksiya
 * chaqiriladi va natija kutilgan bilan solishtiriladi. Oxirida /bin/sinov ishga
 * tushadi — u syscall larni dastur tomondan tekshiradi (integratsion test).
 *
 * "make test" kernelni "test" yuklash satri bilan ishga tushiradi: testlar o'tadi,
 * "SELFTEST: OK" yoki "SELFTEST: XATO" chiqadi va kompyuter (QEMU) o'chadi. Shunday
 * qilib har bir o'zgarishdan keyin bitta buyruq bilan butun tizim tekshiriladi
 * (regressiya testlari).
 */
#include "tests.h"
#include "task.h"
#include "memory.h"
#include "vfs.h"
#include "sync.h"
#include "process.h"
#include "drivers.h"
#include "kprintf.h"
#include "kernel.h"
#include "string.h"
#include "irq.h"
#include "vga.h"
#include "tsc.h"
#include "config.h"

static bool verbose;
static int  total, failed, group_failed;
static const char *group_name;

static void check(bool ok, const char *what)
{
    total++;
    if (!ok) {
        failed++;
        group_failed++;
    }
    if (ok && !verbose)
        return;
    vga_set_color(ok ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    kprintf(ok ? "    [ OK ] " : "    [XATO] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("%s: %s\n", group_name, what);
}

static void group_begin(const char *name)
{
    group_name = name;
    group_failed = 0;
}

static void group_end(int checks_before)
{
    int n = total - checks_before;
    if (n == 0) {
        kprintf("[ -- ] %s: o'tkazib yuborildi\n", group_name);
        return;
    }
    vga_set_color(group_failed ? VGA_LIGHT_RED : VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf(group_failed ? "[XATO] " : "[ OK ] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    if (group_failed)
        kprintf("%s: %d / %d xato\n", group_name, group_failed, n);
    else
        kprintf("%s (%d tekshiruv)\n", group_name, n);
}

/* ------------------------------ Satrlar ------------------------------ */

static void test_strings(void)
{
    char b[32];
    ksnprintf(b, sizeof(b), "%d|%u|%x|%s", -42, 7u, 0xBEEFu, "ok");
    check(strcmp(b, "-42|7|BEEF|ok") == 0, "ksnprintf %d %u %x %s (hex - katta harf)");
    ksnprintf(b, sizeof(b), "[%5s][%-4s]", "ab", "c");
    check(strcmp(b, "[   ab][c   ]") == 0, "ksnprintf kenglik");
    int n = ksnprintf(b, 4, "123456");
    check(n == 6 && strcmp(b, "123") == 0, "ksnprintf bufer chegarasi");
    check(strlcpy(b, "salom dunyo", 6) == 11 && strcmp(b, "salom") == 0, "strlcpy");
    char m[8] = "abcdefg";
    memmove(m + 1, m, 4);
    check(memcmp(m, "aabcdfg", 7) == 0, "memmove ustma-ust");
    check(udiv64(10000000000ULL, 7) == 1428571428ULL, "udiv64 (64 bitli bo'lish)");
}

/* ------------------------------- PMM ------------------------------- */

static void test_pmm(void)
{
    uint32_t used0 = pmm_used_frames();
    uint32_t f = pmm_alloc_frame();
    check(f && (f & 0xFFF) == 0, "frame ajratildi, 4 KB ga tekislangan");
    check(pmm_used_frames() == used0 + 1, "band frame lar +1");
    check(pmm_frame_refs(f) == 1, "hisoblagich = 1");
    pmm_frame_ref(f);
    check(pmm_frame_refs(f) == 2, "ref -> 2");
    memset((void *)f, 0x5A, PAGE_SIZE);
    uint32_t flags = irq_save();
    pmm_free_frame_zero(f);                       /* 2 -> 1: tozalanmasligi kerak */
    bool kept = *(volatile uint8_t *)f == 0x5A;
    pmm_free_frame_zero(f);                       /* 1 -> 0: tozalanadi va bo'shaydi */
    bool zeroed = *(volatile uint32_t *)f == 0;
    irq_restore(flags);
    check(kept, "boshqa egasi bor - mazmun saqlandi");
    check(zeroed, "oxirgi egasi - nol bilan tozalandi");
    check(pmm_used_frames() == used0, "band frame lar avvalgiday");
}

/* ------------------------------- Heap ------------------------------ */

static void test_heap(void)
{
    heap_stats_t a, b;
    heap_get_stats(&a);
    uint8_t *p[40];
    bool ok = true;
    for (int i = 0; i < 40; i++) {
        uint32_t size = 16 + (uint32_t)i * 97;
        p[i] = kmalloc(size);
        if (!p[i]) {
            ok = false;
            break;
        }
        memset(p[i], i, size);
    }
    check(ok, "40 ta har xil blok ajratildi");
    for (int i = 0; ok && i < 40; i += 2) {
        kfree(p[i]);
        p[i] = 0;
    }
    for (int i = 1; ok && i < 40; i += 2) {
        uint32_t size = 16 + (uint32_t)i * 97;
        for (uint32_t k = 0; k < size; k++)
            if (p[i][k] != i)
                ok = false;
    }
    check(ok, "qo'shni bloklar bo'shatilgandan keyin ma'lumot butun");
    for (int i = 1; i < 40; i += 2)
        kfree(p[i]);
    heap_get_stats(&b);
    check(a.used_bytes == b.used_bytes, "hammasi qaytarildi (band baytlar avvalgiday)");
}

/* ------------------------- Paging va CoW ------------------------- */

static void test_paging(void)
{
    uint32_t used0 = pmm_used_frames();
    const uint32_t va = 0x40000000;
    uint32_t *d = paging_create_directory();
    uint32_t f = pmm_alloc_frame();
    check(d && f, "katalog va frame");
    if (!d || !f)
        return;
    memset((void *)f, 0xAB, PAGE_SIZE);
    check(paging_map_in(d, va, f, PAGE_USER | PAGE_RW), "sahifani ulash");
    uint32_t phys, fl;
    check(paging_translate_in(d, va + 0x123, &phys, &fl) && phys == f + 0x123 && (fl & PAGE_RW),
          "virtual -> fizik tarjima");

    uint32_t *c = paging_fork_directory(d);
    check(c != 0, "fork: katalog nusxasi");
    if (!c)
        return;
    uint32_t pp, cp, pf, cf;
    paging_translate_in(d, va, &pp, &pf);
    paging_translate_in(c, va, &cp, &cf);
    check(pp == cp, "fork dan keyin: bitta fizik frame");
    check(!(pf & PAGE_RW) && !(cf & PAGE_RW) && (pf & PAGE_COW) && (cf & PAGE_COW),
          "ikkalasi ham faqat o'qish + COW");
    check(pmm_frame_refs(f) == 2, "frame hisoblagichi = 2");

    check(paging_cow_fault(c, va) == 1, "bola yozdi: CoW ishlovchisi");
    paging_translate_in(c, va, &cp, &cf);
    check(cp != f && (cf & PAGE_RW) && !(cf & PAGE_COW), "bola yangi frame oldi (RW)");
    check(*(uint8_t *)cp == 0xAB && memcmp((void *)cp, (void *)f, PAGE_SIZE) == 0,
          "nusxa mazmuni bir xil");
    check(pmm_frame_refs(f) == 1, "eski frame hisoblagichi = 1");
    check(paging_cow_fault(d, va) == 1, "ota yozdi: nusxasiz ochildi");
    paging_translate_in(d, va, &pp, &pf);
    check(pp == f && (pf & PAGE_RW), "ota o'z frame ida qoldi");
    check(paging_cow_fault(d, va) == 0, "CoW bo'lmagan sahifa - haqiqiy xato");
    check(paging_cow_fault(d, va + 0x10000) == 0, "xaritada yo'q sahifa - haqiqiy xato");

    paging_free_directory(c);
    paging_free_directory(d);
    check(pmm_used_frames() == used0, "hamma frame lar qaytarildi");
}

/* --------------------------------- VFS --------------------------------- */

static void test_vfs(void)
{
    char buf[32];
    vfs_remove("/st", true);                      /* oldingi to'xtatilgan sinovdan qolgan bo'lsa */
    check(vfs_mkdir("/st") == VFS_OK, "papka /st yaratildi");
    int fd = vfs_open("/st/a.txt", VFS_O_WRITE | VFS_O_CREATE);
    check(fd >= 3, "yangi fayl: fd >= 3 (0-2 konsol)");
    check(vfs_write(fd, "salom dunyo", 11) == 11, "11 bayt yozildi");
    vfs_close(fd);

    fd = vfs_open("/st/a.txt", VFS_O_READ);
    int fd2 = vfs_dup(fd);
    check(fd2 >= 0 && fd2 != fd, "dup");
    int32_t n = vfs_read(fd, buf, 5);
    n += vfs_read(fd2, buf + 5, 6);
    buf[11] = 0;
    check(n == 11 && strcmp(buf, "salom dunyo") == 0, "dup: pozitsiya umumiy");
    check(vfs_dup2(fd, 10) == 10 && vfs_fd_kind(10) == 0, "dup2(fd, 10)");
    check(vfs_seek(10, 6) == VFS_OK && vfs_read(fd, buf, 5) == 5 && memcmp(buf, "dunyo", 5) == 0,
          "seek bitta nusxada - boshqasida ham ko'rinadi");
    vfs_close(fd);
    vfs_close(fd2);
    vfs_close(10);
    check(vfs_close(fd) == VFS_EBADF, "yopilgan fd -> EBADF");

    check(vfs_open("/st/yoq.txt", VFS_O_READ) == VFS_ENOENT, "yo'q fayl -> ENOENT");
    check(vfs_open("/st", VFS_O_READ) == VFS_EISDIR, "papka -> EISDIR");
    check(vfs_rename("/st/a.txt", "/st/b.txt") == VFS_OK, "rename");
    vfs_node_t *node;
    check(vfs_lookup("/st/b.txt", &node) == VFS_OK && node->size == 11, "yangi nom, hajm 11");

    int list[TASK_MAX_FDS], opened = 0, err = 0;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        int r = vfs_open("/st/b.txt", VFS_O_READ);
        if (r < 0) {
            err = r;
            break;
        }
        list[opened++] = r;
    }
    check(err == VFS_EMFILE && opened == TASK_MAX_FDS - 3, "jadval to'ldi -> EMFILE");
    check(vfs_remove("/st/b.txt", false) == VFS_EBUSY, "ochiq faylni o'chirib bo'lmaydi");
    for (int i = 0; i < opened; i++)
        vfs_close(list[i]);

    /* Joriy papka */
    char old[VFS_PATH_MAX];
    vfs_get_path(vfs_cwd(), old, sizeof(old));
    check(vfs_chdir("/st") == VFS_OK, "chdir /st");
    fd = vfs_open("c.txt", VFS_O_WRITE | VFS_O_CREATE);
    vfs_close(fd);
    check(vfs_lookup("/st/c.txt", &node) == VFS_OK, "nisbiy yo'l joriy papkadan");
    check(vfs_remove("/st", true) == VFS_EBUSY, "joriy papkani o'chirib bo'lmaydi");
    vfs_chdir(old);
    check(vfs_remove("/st", true) == VFS_OK, "ochir -r /st (rekursiv)");

    fd = vfs_open("/proc/version", VFS_O_READ);
    check(fd >= 0 && vfs_read(fd, buf, sizeof(buf)) > 0, "/proc/version o'qiladi");
    vfs_close(fd);
}

/* -------------------------------- Pipe -------------------------------- */

static void test_pipe(void)
{
    int p[2];
    char buf[8];
    check(vfs_pipe(p) == VFS_OK, "pipe yaratildi");
    check(vfs_write(p[1], "abcd", 4) == 4, "4 bayt yozildi");
    check(vfs_read(p[0], buf, 8) == 4 && memcmp(buf, "abcd", 4) == 0, "4 bayt o'qildi");
    vfs_close(p[1]);
    check(vfs_read(p[0], buf, 8) == 0, "yozish uchi yopildi -> EOF");
    vfs_close(p[0]);

    check(vfs_pipe(p) == VFS_OK, "ikkinchi pipe");
    int w2 = vfs_dup(p[1]);
    vfs_close(p[1]);
    check(vfs_write(w2, "x", 1) == 1, "dup qilingan uch ishlaydi (havola qoldi)");
    vfs_close(p[0]);
    check(vfs_write(w2, "x", 1) == VFS_EPIPE, "o'quvchi yo'q -> EPIPE");
    vfs_close(w2);
}

/* ------------------------- Sinxronizatsiya ------------------------- */

static mutex_t     st_mutex;
static semaphore_t st_sem;
static volatile uint32_t st_counter;

static void counter_thread(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        mutex_lock(&st_mutex);
        uint32_t v = st_counter;
        if ((i & 63) == 0)
            task_yield();                         /* poygani ataylab qo'zg'atamiz */
        st_counter = v + 1;
        mutex_unlock(&st_mutex);
    }
}

static void producer_thread(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        sem_up(&st_sem);
}

static void test_sync(void)
{
    static bool ready = false;
    if (!ready) {                                 /* mutex ro'yxatga bir marta qo'shiladi */
        mutex_init(&st_mutex, "selftest", false);
        sem_init(&st_sem, "selftest", 0);
        ready = true;
    }
    check(mutex_lock(&st_mutex) == 0 && mutex_owned(&st_mutex), "mutex olindi");
    check(!mutex_trylock(&st_mutex), "band mutex - trylock rad etadi");
    mutex_unlock(&st_mutex);
    check(!mutex_owned(&st_mutex), "mutex bo'shatildi");

    st_counter = 0;
    uint32_t a = task_create("st-mutex", counter_thread, 3000);
    uint32_t b = task_create("st-mutex", counter_thread, 3000);
    task_wait(a);
    task_wait(b);
    task_reap_pid(a);
    task_reap_pid(b);
    check(st_counter == 6000, "2 oqim x 3000 (yield bilan) - poyga yo'q");

    uint32_t pr = task_create("st-sem", producer_thread, 50);
    int got = 0;
    for (int i = 0; i < 50; i++)
        if (sem_down(&st_sem) == 0)
            got++;
    task_wait(pr);
    task_reap_pid(pr);
    check(got == 50 && !sem_trydown(&st_sem), "semafor: 50 ta signal, ortiqchasi yo'q");
}

/* ----------------------------- Scheduler ----------------------------- */

static void exit_thread(uint32_t code)
{
    task_exit_code((int32_t)code);
}

static void spin_thread(uint32_t ticks)
{
    volatile uint32_t *mine = &task_current()->cpu_ticks;
    uint32_t start = *mine;
    while (*mine - start < ticks)
        ;
}

static void test_sched(void)
{
    uint32_t pid = task_create("st-exit", exit_thread, 42);
    check(pid && task_wait(pid) == 42, "oqim yaratildi, chiqish kodi 42");
    task_reap_pid(pid);

    uint32_t t0 = timer_ticks();
    task_sleep_ticks(5);
    uint32_t dt = timer_ticks() - t0;
    check(dt >= 5 && dt <= 7, "kutish (50 ms) aniqligi");

    sched_policy_t old = sched_policy();

    /* PRIO: yuqori ustuvorlikli vazifa past ustuvorlikliga navbat bermaydi */
    sched_set_policy(SCHED_PRIO);
    uint32_t flags = irq_save();
    uint32_t lo = task_create("st-past", spin_thread, 10);
    uint32_t hi = task_create("st-yuqori", spin_thread, 10);
    task_set_prio(lo, 3);
    task_set_prio(hi, 9);
    irq_restore(flags);
    task_wait(lo);
    task_wait(hi);
    task_t *tl = task_find(lo), *th = task_find(hi);
    check(tl && th && th->end_tick <= tl->first_run_tick, "PRIO: past ustuvorlik yuqorisi tugagach boshlandi");
    task_reap_pid(lo);
    task_reap_pid(hi);

    /* MLFQ: protsessorni ko'p ishlatgan vazifa eng pastki darajaga tushadi */
    sched_set_policy(SCHED_MLFQ);
    uint32_t m = task_create("st-mlfq", spin_thread, 12);
    task_wait(m);
    task_t *tm = task_find(m);
    check(tm && tm->level == MLFQ_LEVELS - 1, "MLFQ: hisob vazifasi pastki darajada");
    task_reap_pid(m);

    sched_set_policy(old);
    check(sched_policy() == old, "algoritm qaytarildi");
}

/* -------------------------------- Disk -------------------------------- */

static void test_disk(void)
{
    fat32_stats_t st;
    fat32_stats(&st);
    if (!st.mounted)
        return;                                   /* disk yo'q yoki formatlanmagan */
    uint32_t free0 = st.free_clusters;
    const uint32_t size = 64 * 1024;
    uint8_t *data = kmalloc(size), *back = kmalloc(size);
    check(data && back, "bufer");
    if (!data || !back)
        return;
    for (uint32_t i = 0; i < size; i++)
        data[i] = (uint8_t)(i * 7 + (i >> 9));
    int fd = vfs_open("/disk/_selftest.bin", VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    check(fd >= 0 && vfs_write(fd, data, size) == (int32_t)size, "64 KB diskka yozildi");
    vfs_close(fd);
    fd = vfs_open("/disk/_selftest.bin", VFS_O_READ);
    uint32_t got = 0;
    int32_t n;
    while (fd >= 0 && got < size && (n = vfs_read(fd, back + got, size - got)) > 0)
        got += (uint32_t)n;
    vfs_close(fd);
    check(got == size && memcmp(data, back, size) == 0, "o'qilgani yozilgani bilan bayt-ma-bayt bir xil");
    check(vfs_remove("/disk/_selftest.bin", false) == VFS_OK, "fayl o'chirildi");
    fat32_stats(&st);
    check(st.free_clusters == free0, "bo'sh klasterlar soni avvalgiday");
    kfree(data);
    kfree(back);
}

/* --------------------------- User dasturlar --------------------------- */

static void test_user(void)
{
    int32_t pid = process_spawn("/bin/sinov", verbose ? "-v" : "");
    check(pid > 0, "/bin/sinov ishga tushdi");
    if (pid <= 0)
        return;
    int32_t code = task_wait((uint32_t)pid);
    task_reap_pid((uint32_t)pid);
    check(code == 0, "sinov: barcha syscall testlari o'tdi");
}

/* --------------------------------------------------------------------- */

int selftest_run(bool v)
{
    verbose = v;
    total = failed = 0;
    uint32_t t0 = timer_ticks();
    kprintf("DiyorOS %s - o'z-o'zini tekshirish\n", OS_VERSION);

    struct { const char *name; void (*fn)(void); } groups[] = {
        { "satrlar va kprintf",         test_strings },
        { "fizik xotira (PMM)",         test_pmm     },
        { "kernel heap",                test_heap    },
        { "paging va copy-on-write",    test_paging  },
        { "VFS, deskriptorlar, papka",  test_vfs     },
        { "kanallar (pipe)",            test_pipe    },
        { "mutex va semafor",           test_sync    },
        { "scheduler va algoritmlar",   test_sched   },
        { "disk (FAT32)",               test_disk    },
        { "user dasturlar (sinov)",     test_user    },
    };
    for (unsigned i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        int before = total;
        group_begin(groups[i].name);
        groups[i].fn();
        group_end(before);
    }
    uint32_t ms = (timer_ticks() - t0) * (1000 / TIMER_HZ);
    vga_set_color(failed ? VGA_LIGHT_RED : VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("Natija: %d / %d tekshiruv o'tdi, %d ta xato (%u.%u s)\n",
            total - failed, total, failed, ms / 1000, ms % 1000 / 100);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    return failed;
}

/* Yuklash satri: "test" — testlar, "bench" — o'lchovlar; keyin kompyuter o'chadi */
void auto_run(void)
{
    if (kernel_cmdline_has("test")) {
        int fails = selftest_run(kernel_cmdline_has("-v"));
        kprintf("SELFTEST: %s\n", fails ? "XATO" : "OK");
    }
    if (kernel_cmdline_has("bench")) {
        kprintf("\n=== Rejalashtirish algoritmlari (3 marta takror) ===\n");
        schedbench_compare(3, true);
        kprintf("\n=== Protsessor ulushi ===\n");
        schedbench_share(true);
        kprintf("\n=== Mikro-o'lchovlar ===\n");
        int32_t pid = process_spawn("/bin/olchov", "");
        if (pid > 0) {
            task_wait((uint32_t)pid);
            task_reap_pid((uint32_t)pid);
        }
        kprintf("BENCH: TAYYOR\n");
    }
    power_off();                                  /* QEMU o'chadi; haqiqiy kompyuterda shell ochiladi */
}
