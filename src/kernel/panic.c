/*
 * panic.c — kernel panic: tuzatib bo'lmaydigan xato (v0.6.1).
 *
 * Kernel o'z ichida xato topsa (noto'g'ri xotiraga murojaat, buzilgan heap,
 * to'lib ketgan stek...), ishlashda davom etish XAVFLI: buzilgan ma'lumot
 * diskka yozilib ketishi mumkin. Shuning uchun tizim darhol to'xtatiladi va
 * ekranga (hamda serial portga) sabab chiqariladi. Windows dagi "ko'k ekran"
 * (BSOD) va Linux dagi "kernel panic" aynan shu.
 *
 * BACKTRACE
 * Har bir funksiya chaqirilganda stekka qaytish manzili va oldingi EBP
 * saqlanadi — ular zanjir hosil qiladi:
 *
 *     EBP -> [ oldingi EBP ][ qaytish manzili ]
 *                 |
 *                 v
 *            [ oldingi EBP ][ qaytish manzili ] ...
 *
 * Shu zanjir bo'ylab yurib, "xatoga qaysi funksiyalar orqali kelindi" ni
 * chiqaramiz. Manzilni funksiya nomiga aylantirish (Mac terminalida):
 *     i686-elf-addr2line -f -e build/diyoros.bin 0x00101234
 */
#include "klog.h"
#include "kprintf.h"
#include "vga.h"
#include "memory.h"
#include "kernel.h"
#include "task.h"
#include "drivers.h"

static bool in_panic = false;

static void __attribute__((noreturn)) halt_forever(void)
{
    __asm__ volatile ("cli");
    for (;;)
        __asm__ volatile ("hlt");
}

static bool readable(uint32_t addr)
{
    uint32_t phys;
    if (!paging_kernel_directory())
        return false;                       /* paging hali yoqilmagan */
    return addr >= 0x1000 && paging_translate(addr, &phys) && paging_translate(addr + 7, &phys);
}

static void backtrace(uint32_t eip, uint32_t ebp)
{
    kprintf("  Backtrace: 0x%08x", eip);
    int printed = 1;
    for (int depth = 0; depth < 9; depth++) {
        if ((ebp & 3) || !readable(ebp))
            break;
        uint32_t next = ((uint32_t *)ebp)[0];
        uint32_t ret  = ((uint32_t *)ebp)[1];
        if (ret < (uint32_t)kernel_start || ret >= (uint32_t)kernel_data_start)
            break;                          /* kernel kodidan tashqari — zanjir tugadi */
        if (printed++ % 5 == 0)
            kprintf("\n         ");        /* 80 ustunga sig'ishi uchun qatorga 5 tadan */
        kprintf(" <- 0x%08x", ret);
        if (next <= ebp)
            break;                          /* stek yuqoriga o'sadi: teskari bo'lsa — buzilgan */
        ebp = next;
    }
    kprintf("\n");
}

static void panic_header(void)
{
    __asm__ volatile ("cli");
    if (in_panic)                           /* panic ichida yana xato: cheksiz tsiklga tushmaymiz */
        halt_forever();
    in_panic = true;

    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n\n  *** KERNEL PANIC ***                                                          \n");
}

static void __attribute__((noreturn)) panic_footer(void)
{
    if (sched_active()) {
        task_t *t = task_current();
        kprintf("  Vazifa: PID %u (%s)   Ishlash vaqti: %u s\n", t->pid, t->name, timer_seconds());
    }
    kprintf("  Tizim to'xtatildi. Qayta yoqing.\n");
    halt_forever();
}

void panic(const char *fmt, ...)
{
    uint32_t ebp;
    __asm__ volatile ("mov %%ebp, %0" : "=r"(ebp));
    panic_header();

    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    kprintf("  %s\n", msg);
    klog("PANIC: %s", msg);

    /* panic() ni chaqirgan joydan boshlaymiz (o'z kadrimizni tashlab) */
    backtrace((uint32_t)__builtin_return_address(0), readable(ebp) ? ((uint32_t *)ebp)[0] : 0);
    panic_footer();
}

/* Kernelning o'zida (ring 0) exception: idt.c chaqiradi */
void panic_exception(registers_t *r, const char *name)
{
    panic_header();
    kprintf("  Exception #%u: %s\n", r->int_no, name);
    kprintf("  Error code: 0x%x   EIP: 0x%08x   CS: 0x%x   EFLAGS: 0x%x\n",
            r->err_code, r->eip, r->cs, r->eflags);
    kprintf("  EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("  ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n", r->esi, r->edi, r->ebp, (uint32_t)r);
    klog("PANIC: exception #%u (%s), EIP 0x%08x", r->int_no, name, r->eip);

    if (r->int_no == 14) {
        /* Page Fault: CR2 registrida xato bergan manzil saqlanadi */
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  Sahifa xatosi manzili (CR2): 0x%08x\n", cr2);
        kprintf("  Sabab: %s, %s, %s rejimida\n",
                (r->err_code & 1) ? "ruxsat buzildi" : "sahifa xaritada yo'q",
                (r->err_code & 2) ? "yozishda" : "o'qishda",
                (r->err_code & 4) ? "foydalanuvchi" : "kernel");
        if (cr2 < 0x1000)
            kprintf("  (NULL pointer ishlatilgan bo'lishi mumkin)\n");
        else if ((r->err_code & 3) == 3 && cr2 >= (uint32_t)kernel_start && cr2 < (uint32_t)kernel_data_start)
            kprintf("  (kernel kodi faqat o'qish uchun: unga yozib bo'lmaydi)\n");
    }
    backtrace(r->eip, r->ebp);
    panic_footer();
}

/*
 * Double Fault (#8): protsessor shu funksiyaga "task gate" orqali, alohida
 * toza stekda keladi (gdt.c). Eng ko'p uchraydigan sabab — kernel stekining
 * to'lib ketishi: stek qo'riqchi sahifaga yetgan va protsessor exception
 * kadrini yoza olmagan.
 */
void double_fault_entry(void)
{
    uint32_t eip, esp, ebp;
    tss_saved_state(&eip, &esp, &ebp);

    panic_header();
    kprintf("  Exception #8: Double Fault\n");
    kprintf("  EIP: 0x%08x   ESP: 0x%08x   EBP: 0x%08x\n", eip, esp, ebp);

    bool overflow = false;
    if (sched_active()) {
        task_t *t = task_current();
        if (t->stack_guard && esp < (uint32_t)t->stack_guard + 512 &&
            esp + PAGE_SIZE > (uint32_t)t->stack_guard)
            overflow = true;
    } else if (esp < (uint32_t)boot_stack_bottom + 512 && esp + PAGE_SIZE > (uint32_t)boot_stack_bottom) {
        overflow = true;
    }
    if (overflow)
        kprintf("  Sabab: KERNEL STEKI TO'LIB KETDI (ESP qo'riqchi sahifaga yetdi)\n");
    klog("PANIC: double fault, EIP 0x%08x ESP 0x%08x%s", eip, esp,
         overflow ? " (stek to'lib ketgan)" : "");
    backtrace(eip, ebp);
    panic_footer();
}
