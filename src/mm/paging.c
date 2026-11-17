/*
 * paging.c — virtual xotira (paging).
 *
 * Paging yoqilgandan keyin protsessor ishlatadigan har bir manzil VIRTUAL
 * bo'ladi. MMU (protsessor ichidagi qism) uni jadvallar orqali FIZIK
 * manzilga aylantiradi. 32-bit manzil 3 qismga bo'linadi:
 *
 *   | 10 bit: Page Directory indeksi | 10 bit: Page Table indeksi | 12 bit: sahifa ichidagi joy |
 *
 *   Page Directory (1024 yozuv) -> Page Table (1024 yozuv) -> 4 KB lik fizik frame
 *   1024 * 1024 * 4 KB = 4 GB — butun 32-bit manzil maydoni.
 *
 * Nima uchun kerak?
 *   - Har bir dasturga alohida "o'z xotirasi"ni berish (keyingi bosqichlarda);
 *   - Fizik jihatdan tarqoq frame larni virtual jihatdan ketma-ket ko'rsatish (heap);
 *   - Himoya: xaritada yo'q manzilga murojaat -> Page Fault (exception #14).
 *
 * Bizning sxema:
 *   - 4 KB dan RAM oxirigacha: "identity mapping" (virtual = fizik).
 *     Shuning uchun kernel paging yoqilgandan keyin ham ishlashda davom etadi.
 *   - 0-sahifa (0x0–0xFFF) ataylab xaritaga kiritilmagan: NULL pointer
 *     ishlatilsa, darhol Page Fault bo'ladi va xato topiladi.
 *   - 0xD0000000 dan boshlab — kernel heap (kheap.c).
 *   - 0xE0000000 dan boshlab — framebuffer, ekran piksellari (v0.7, fb.c).
 *   - v0.6.1: kernelning KODI va o'zgarmas ma'lumotlari (.text, .rodata) FAQAT
 *     O'QISH uchun. Xato ko'rsatkich kernel kodini buzib qo'ya olmaydi —
 *     yozishga urinish darhol Page Fault beradi ("W^X" tamoyili: xotira yo
 *     yoziladi, yo bajariladi, ikkalasi birga emas).
 *
 * v0.5: HAR BIR JARAYONNING O'Z PAGE DIRECTORY SI
 *   - Kernel qismi (identity map va heap) barcha jarayonlarda BIR XIL:
 *     ularning Page Directory yozuvlari kernel dagidan nusxalanadi va
 *     o'sha Page Table larga ishora qiladi. PAGE_USER flagi yo'q — dastur
 *     (ring 3) kernel xotirasiga tegsa, darhol Page Fault bo'ladi.
 *   - Foydalanuvchi qismi (0x40000000 - 0xC0000000) har jarayonda O'ZINIKI.
 *     Ikki dastur bir xil virtual manzilni ishlatsa ham, ular har xil
 *     fizik frame larga tushadi — bir-birining xotirasini ko'ra olmaydi.
 *   - Jarayon almashganda CR3 registriga uning Page Directory si yoziladi.
 *
 * v0.10: COPY-ON-WRITE (CoW) — fork() ning asosi
 *   fork() jarayonning NUSXASINI yaratadi. To'g'ridan-to'g'ri yo'l — barcha
 *   sahifalarni nusxalash: 1 MB lik dastur uchun 256 ta frame va ko'p vaqt.
 *   Ko'pincha bola darhol exec() qiladi va bu nusxa bekorga ketadi.
 *   Yechim — "yozilganda nusxalash":
 *     1. fork: bola ota bilan BIR XIL frame larni oladi (faqat Page Table lar
 *        yangi). Yoziladigan har bir sahifa IKKALASIDA HAM "faqat o'qish" qilinadi
 *        va PAGE_COW belgisi qo'yiladi; frame hisoblagichi (pmm.c) +1.
 *     2. Kimdir shu sahifaga yozmoqchi bo'lsa — protsessor Page Fault beradi
 *        (sahifa bor, lekin faqat o'qish uchun). paging_cow_fault:
 *          - frame ni boshqa hech kim ishlatmasa — shunchaki yozishga ochamiz;
 *          - aks holda yangi frame ajratib, 4 KB ni nusxalaymiz va yozuvni
 *            yangi frame ga o'tkazamiz. Eski frame hisoblagichi -1.
 *     3. Buyruq qaytadan bajariladi — endi muvaffaqiyatli. Dastur hech narsa sezmaydi.
 *   Natija: fork bir zumda bajariladi, xotira faqat haqiqatan o'zgargan sahifalar
 *   uchun sarflanadi. Linux, BSD, Windows (fork emulatsiyasi) shunday ishlaydi.
 */
#include "memory.h"
#include "string.h"
#include "kernel.h"
#include "klog.h"
#include "cpu.h"
#include "fb.h"
#include "irq.h"

static uint32_t *kernel_directory = 0;
static uint32_t *current_directory = 0;
static uint32_t identity_limit = 0;
static uint32_t cow_copies = 0;      /* v0.10: CoW tufayli nusxalangan sahifalar */
static uint32_t cow_reused = 0;      /* ...nusxa kerak bo'lmagan (oxirgi egasi) holatlar */

static inline void invlpg(uint32_t addr)
{
    /* TLB (manzillar keshi) dan shu sahifaning eski yozuvini o'chirish */
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

/* dir dagi virt manzil uchun Page Table ni topish. Yo'q bo'lsa va create=true — yaratish */
static uint32_t *get_table(uint32_t *dir, uint32_t virt, bool create, uint32_t flags)
{
    uint32_t pd_index = virt >> 22;

    if (!(dir[pd_index] & PAGE_PRESENT)) {
        if (!create)
            return 0;
        uint32_t frame = pmm_alloc_frame();
        if (!frame)
            return 0;
        memset((void *)frame, 0, PAGE_SIZE);   /* frame identity-map da: to'g'ridan-to'g'ri yozsa bo'ladi */
        dir[pd_index] = frame | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
    }
    return (uint32_t *)(dir[pd_index] & ~0xFFFu);
}

bool paging_map_in(uint32_t *dir, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t *table = get_table(dir, virt, true, flags);
    if (!table)
        return false;
    table[(virt >> 12) & 0x3FF] = (phys & ~0xFFFu) | (flags & 0xFFF) | PAGE_PRESENT;
    if (dir == current_directory)
        invlpg(virt);
    return true;
}

bool paging_translate_in(uint32_t *dir, uint32_t virt, uint32_t *phys, uint32_t *flags)
{
    uint32_t *table = get_table(dir, virt, false, 0);
    if (!table)
        return false;
    uint32_t entry = table[(virt >> 12) & 0x3FF];
    if (!(entry & PAGE_PRESENT))
        return false;
    if (phys)
        *phys = (entry & ~0xFFFu) | (virt & 0xFFF);
    if (flags)
        *flags = entry & 0xFFF;
    return true;
}

static void table_entry_clear(uint32_t virt)
{
    uint32_t *table = get_table(kernel_directory, virt, false, 0);
    if (table)
        table[(virt >> 12) & 0x3FF] = 0;
}

/* Kernel xaritasi bilan ishlash (v0.2 dagi funksiyalar) */
bool paging_map(uint32_t virt, uint32_t phys, uint32_t flags)
{
    return paging_map_in(kernel_directory, virt, phys, flags);
}

void paging_unmap(uint32_t virt)
{
    uint32_t *table = get_table(kernel_directory, virt, false, 0);
    if (!table)
        return;
    table[(virt >> 12) & 0x3FF] = 0;
    invlpg(virt);
}

/* MMU qiladigan ishni dasturiy takrorlash: virtual -> fizik */
bool paging_translate(uint32_t virt, uint32_t *phys)
{
    return paging_translate_in(kernel_directory, virt, phys, 0);
}

void paging_init(void)
{
    kernel_directory = (uint32_t *)pmm_alloc_frame();
    if (!kernel_directory)
        panic("paging_init: bo'sh xotira yo'q");
    memset(kernel_directory, 0, PAGE_SIZE);

    /* RAM oxirini 4 MB ga yaxlitlaymiz (bitta Page Table 4 MB ni qamraydi) */
    identity_limit = (pmm_memory_top() + 0x3FFFFF) & ~0x3FFFFFu;

    uint32_t ro_start = (uint32_t)kernel_start, ro_end = (uint32_t)kernel_data_start;
    for (uint32_t addr = PAGE_SIZE; addr < identity_limit; addr += PAGE_SIZE) {
        bool read_only = addr >= ro_start && addr < ro_end;     /* kernel kodi va .rodata */
        if (!paging_map(addr, addr, read_only ? 0 : PAGE_RW))
            panic("paging_init: sahifa jadvallari uchun xotira yetmadi");
    }

    /*
     * Heap hududi uchun Page Table larni OLDINDAN yaratamiz. Sababi: jarayonlar
     * kernel ning Page Directory yozuvlarini nusxalab oladi. Agar keyin heap
     * uchun yangi Page Table yaratilsa, eski jarayonlar uni ko'rmay qoladi.
     * Oldindan yaratilsa — yozuvlar o'zgarmaydi, Page Table lar esa umumiy.
     */
    for (uint32_t addr = HEAP_START; addr < HEAP_MAX; addr += 0x400000)
        if (!get_table(kernel_directory, addr, true, 0))
            panic("paging_init: heap jadvallari uchun xotira yetmadi");

    /* v0.7: framebuffer hududi (16 MB) uchun ham xuddi shunday — grafik rejim keyin
       yoqilsa ham, oldin yaratilgan jarayonlar ekran xotirasini "ko'radi" */
    for (uint32_t addr = FB_VIRT; addr < FB_VIRT + FB_MAX_SIZE; addr += 0x400000)
        if (!get_table(kernel_directory, addr, true, 0))
            panic("paging_init: framebuffer jadvallari uchun xotira yetmadi");

    /* v0.9: qurilma registrlari (AHCI va h.k.) hududi — xuddi shu sababdan oldindan */
    for (uint32_t addr = MMIO_VIRT; addr < MMIO_VIRT + MMIO_SIZE; addr += 0x400000)
        if (!get_table(kernel_directory, addr, true, 0))
            panic("paging_init: MMIO jadvallari uchun xotira yetmadi");

    /* Shell steki ostidagi qo'riqchi sahifa: xaritada yo'q — stek to'lsa darhol xato */
    table_entry_clear((uint32_t)boot_stack_guard);
    tss_set_df_cr3((uint32_t)kernel_directory);

    paging_switch(kernel_directory);

    /* CR0: 31-bit (PG) — paging ni yoqish, 16-bit (WP) — faqat o'qiladigan sahifalarni himoya qilish */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

/* ------------------------ Jarayonlar uchun (v0.5) ------------------------ */

uint32_t *paging_kernel_directory(void)
{
    return kernel_directory;
}

uint32_t *paging_current_directory(void)
{
    return current_directory;
}

/* CR3 ga yangi Page Directory ni yozish: butun virtual xotira "almashadi" */
void paging_switch(uint32_t *dir)
{
    if (dir == current_directory)
        return;
    current_directory = dir;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(dir) : "memory");
}

/* Yangi jarayon uchun Page Directory: kernel qismi nusxalanadi, user qismi bo'sh */
uint32_t *paging_create_directory(void)
{
    uint32_t *dir = (uint32_t *)pmm_alloc_frame();
    if (!dir)
        return 0;
    memset(dir, 0, PAGE_SIZE);
    for (int i = 0; i < 1024; i++) {
        uint32_t virt = (uint32_t)i << 22;
        if (virt >= USER_BASE && virt < USER_TOP)
            continue;
        dir[i] = kernel_directory[i];
    }
    return dir;
}

/*
 * Jarayon tugaganda: uning barcha user sahifalari, Page Table lari va directory si.
 *
 * v0.10: frame boshqa jarayon bilan umumiy (fork, CoW) bo'lsa — faqat hisoblagich
 * kamayadi, frame o'sha jarayonda qoladi. Oxirgi egasi chiqqanda bo'shatiladi.
 *
 * v0.9: har bir sahifa bo'shatishdan oldin NOL bilan tozalanadi:
 *   1) xavfsizlik — frame keyingi egasiga (boshqa jarayon yoki kernel) oldingi
 *      jarayonning ma'lumotini (parollar, fayl mazmuni) ko'rsatmasligi kerak;
 *   2) emulyatorlar (QEMU, UTM) bajarilgan kod sahifalarini kuzatadi: kod turgan
 *      sahifa keyin oddiy xotira sifatida ishlatilsa, unga har bir yozish sekin yo'ldan
 *      o'tardi — "sinx poyga" biror dasturdan keyin 30-40 marta sekinlashardi.
 *      Butun sahifani yozib chiqish bu belgini bir marta, shu yerning o'zida olib tashlaydi.
 */
void paging_free_directory(uint32_t *dir)
{
    if (!dir || dir == kernel_directory)
        return;
    for (uint32_t i = USER_BASE >> 22; i < (USER_TOP >> 22); i++) {
        if (!(dir[i] & PAGE_PRESENT))
            continue;
        uint32_t *table = (uint32_t *)(dir[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++)
            if (table[j] & PAGE_PRESENT)
                pmm_free_frame_zero(table[j] & ~0xFFFu);   /* oxirgi egasi bo'lsak — tozalab bo'shatadi */
        pmm_free_frame((uint32_t)table);
    }
    pmm_free_frame((uint32_t)dir);
}

/* ------------------------- fork va copy-on-write (v0.10) ------------------------- */

/* TLB ni to'liq tozalash: CR3 ni qayta yozish. Joriy jarayonning ko'p yozuvi o'zgarganda */
void paging_flush_tlb(void)
{
    uint32_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}

/*
 * Ota jarayon xotirasining copy-on-write nusxasi (fork). Bola uchun yangi Page
 * Directory va yangi Page Table lar yaratiladi, lekin ular OTANING frame lariga
 * ishora qiladi. Ikkala tomonda ham yoziladigan sahifalar "faqat o'qish + CoW".
 * Xotira yetmasa — 0 (ota xotirasi buzilmaydi: CoW belgisi qolgan sahifalar
 * birinchi yozishda shunchaki qayta ochiladi).
 */
uint32_t *paging_fork_directory(uint32_t *src)
{
    uint32_t *dir = paging_create_directory();
    if (!dir)
        return 0;
    for (uint32_t i = USER_BASE >> 22; i < (USER_TOP >> 22); i++) {
        if (!(src[i] & PAGE_PRESENT))
            continue;
        uint32_t *table = (uint32_t *)pmm_alloc_frame();
        if (!table) {
            paging_free_directory(dir);
            if (src == current_directory)
                paging_flush_tlb();
            return 0;
        }
        uint32_t *from = (uint32_t *)(src[i] & ~0xFFFu);
        /* Jadval bo'linmas nusxalanadi: shu payt boshqa jarayon (aka-uka) umumiy
           frame da CoW ni hal qilib, hisoblagichni o'zgartirishi mumkin */
        uint32_t flags = irq_save();
        for (int j = 0; j < 1024; j++) {
            uint32_t e = from[j];
            if (e & PAGE_PRESENT) {
                if (e & PAGE_RW) {
                    e = (e & ~(uint32_t)PAGE_RW) | PAGE_COW;
                    from[j] = e;                   /* ota ham endi yozganda nusxa oladi */
                }
                pmm_frame_ref(e & ~0xFFFu);
            }
            table[j] = e;
        }
        irq_restore(flags);
        dir[i] = (uint32_t)table | (src[i] & 0xFFF);
    }
    if (src == current_directory)
        paging_flush_tlb();                        /* otaning eski "yozish mumkin" yozuvlari TLB da qolmasin */
    return dir;
}

/*
 * Page Fault: sahifa bor, lekin faqat o'qish uchun va dastur yozmoqchi. Bu CoW
 * sahifami? Bo'lsa — nusxa olamiz (yoki oxirgi egasi bo'lsak — shunchaki ochamiz).
 * Qaytaradi: 1 — hal qilindi (buyruq qayta bajariladi), 0 — CoW emas (haqiqiy
 * xato), -1 — nusxa uchun xotira yo'q.
 */
int paging_cow_fault(uint32_t *dir, uint32_t virt)
{
    if (!dir || dir == kernel_directory || virt < USER_BASE || virt >= USER_TOP)
        return 0;
    uint32_t flags = irq_save();
    uint32_t *table = get_table(dir, virt, false, 0);
    uint32_t idx = (virt >> 12) & 0x3FF;
    if (!table || !(table[idx] & PAGE_PRESENT) || !(table[idx] & PAGE_COW)) {
        irq_restore(flags);
        return 0;
    }
    uint32_t e = table[idx];
    uint32_t frame = e & ~0xFFFu;
    uint32_t keep = (e & 0xFFF & ~(uint32_t)PAGE_COW) | PAGE_RW;
    if (pmm_frame_refs(frame) <= 1) {
        table[idx] = frame | keep;                 /* qolgan yagona egasi — nusxa shart emas */
        cow_reused++;
    } else {
        uint32_t copy = pmm_alloc_frame();
        if (!copy) {
            irq_restore(flags);
            return -1;
        }
        memcpy((void *)copy, (const void *)frame, PAGE_SIZE);
        table[idx] = copy | keep;
        pmm_free_frame(frame);                     /* hisoblagich -1: frame boshqasida qoladi */
        cow_copies++;
    }
    if (dir == current_directory)
        invlpg(virt & ~0xFFFu);
    irq_restore(flags);
    return 1;
}

void paging_cow_stats(uint32_t *copies, uint32_t *reused)
{
    *copies = cow_copies;
    *reused = cow_reused;
}

/*
 * Kernel dastur xotirasiga yozishidan OLDIN (read() buferi, wait() natijasi):
 * oraliqdagi CoW sahifalarni oldindan ajratib qo'yamiz. Aks holda kernel o'zi
 * Page Fault ga tushardi. Xotira yetmasa — false.
 */
bool paging_make_writable(uint32_t *dir, uint32_t virt, uint32_t len)
{
    if (len == 0)
        return true;
    uint32_t end = virt + len;
    if (end < virt)
        return false;
    for (uint32_t page = virt & ~0xFFFu; page < end; page += PAGE_SIZE) {
        if (paging_cow_fault(dir, page) < 0)
            return false;
        if (page + PAGE_SIZE < page)
            break;
    }
    return true;
}

/* dir dagi [virt, virt+len) oralig'idagi har bir sahifa mavjud va kerakli ruxsatga egami */
bool paging_check_range(uint32_t *dir, uint32_t virt, uint32_t len, uint32_t need_flags)
{
    if (len == 0)
        return true;
    uint32_t end = virt + len;
    if (end < virt)
        return false;                          /* 32-bit dan oshib ketdi */
    for (uint32_t page = virt & ~0xFFFu; page < end; page += PAGE_SIZE) {
        uint32_t flags;
        if (!paging_translate_in(dir, page, 0, &flags))
            return false;
        if ((flags & need_flags) != need_flags)
            return false;
        if (page + PAGE_SIZE < page)
            break;
    }
    return true;
}

/*
 * v0.9: qurilmaning fizik registrlar manzilini (masalan, AHCI kontrolleri —
 * 0xFEBF1000) kernelning virtual xotirasiga ulash. Kesh o'chiriladi: protsessor
 * registr qiymatini eslab qolmasligi, har safar qurilmaning o'zidan o'qishi kerak.
 * Faqat yuklanishda chaqiriladi (oddiy "ko'tariluvchi" ajratuvchi, qaytarilmaydi).
 */
void *paging_map_mmio(uint32_t phys, uint32_t size)
{
    static uint32_t next = MMIO_VIRT;
    uint32_t base = phys & ~(PAGE_SIZE - 1), extra = phys - base;
    uint32_t pages = (size + extra + PAGE_SIZE - 1) / PAGE_SIZE;
    if (next + pages * PAGE_SIZE > MMIO_VIRT + MMIO_SIZE)
        return 0;
    uint32_t virt = next;
    for (uint32_t i = 0; i < pages; i++)
        if (!paging_map(virt + i * PAGE_SIZE, base + i * PAGE_SIZE, PAGE_RW | PAGE_PCD))
            return 0;
    next += pages * PAGE_SIZE;
    return (void *)(virt + extra);
}

uint32_t paging_identity_limit(void)
{
    return identity_limit;
}

uint32_t paging_directory_address(void)
{
    return (uint32_t)current_directory;
}
