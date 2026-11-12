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
 */
#include "memory.h"
#include "string.h"

static uint32_t *kernel_directory = 0;
static uint32_t *current_directory = 0;
static uint32_t identity_limit = 0;

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
    memset(kernel_directory, 0, PAGE_SIZE);

    /* RAM oxirini 4 MB ga yaxlitlaymiz (bitta Page Table 4 MB ni qamraydi) */
    identity_limit = (pmm_memory_top() + 0x3FFFFF) & ~0x3FFFFFu;

    for (uint32_t addr = PAGE_SIZE; addr < identity_limit; addr += PAGE_SIZE)
        paging_map(addr, addr, PAGE_RW);

    /*
     * Heap hududi uchun Page Table larni OLDINDAN yaratamiz. Sababi: jarayonlar
     * kernel ning Page Directory yozuvlarini nusxalab oladi. Agar keyin heap
     * uchun yangi Page Table yaratilsa, eski jarayonlar uni ko'rmay qoladi.
     * Oldindan yaratilsa — yozuvlar o'zgarmaydi, Page Table lar esa umumiy.
     */
    for (uint32_t addr = HEAP_START; addr < HEAP_MAX; addr += 0x400000)
        get_table(kernel_directory, addr, true, 0);

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

/* Jarayon tugaganda: uning barcha user sahifalari, Page Table lari va directory si */
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
                pmm_free_frame(table[j] & ~0xFFFu);
        pmm_free_frame((uint32_t)table);
    }
    pmm_free_frame((uint32_t)dir);
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

uint32_t paging_identity_limit(void)
{
    return identity_limit;
}

uint32_t paging_directory_address(void)
{
    return (uint32_t)current_directory;
}
