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
 */
#include "memory.h"
#include "string.h"

static uint32_t *page_directory = 0;
static uint32_t identity_limit = 0;

static inline void invlpg(uint32_t addr)
{
    /* TLB (manzillar keshi) dan shu sahifaning eski yozuvini o'chirish */
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

/* virt manzil uchun Page Table ni topish. Yo'q bo'lsa va create=true — yaratish */
static uint32_t *get_table(uint32_t virt, bool create)
{
    uint32_t pd_index = virt >> 22;

    if (!(page_directory[pd_index] & PAGE_PRESENT)) {
        if (!create)
            return 0;
        uint32_t frame = pmm_alloc_frame();
        if (!frame)
            return 0;
        memset((void *)frame, 0, PAGE_SIZE);   /* frame identity-map da: to'g'ridan-to'g'ri yozsa bo'ladi */
        page_directory[pd_index] = frame | PAGE_PRESENT | PAGE_RW;
    }
    return (uint32_t *)(page_directory[pd_index] & ~0xFFFu);
}

bool paging_map(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t *table = get_table(virt, true);
    if (!table)
        return false;
    table[(virt >> 12) & 0x3FF] = (phys & ~0xFFFu) | (flags & 0xFFF) | PAGE_PRESENT;
    invlpg(virt);
    return true;
}

void paging_unmap(uint32_t virt)
{
    uint32_t *table = get_table(virt, false);
    if (!table)
        return;
    table[(virt >> 12) & 0x3FF] = 0;
    invlpg(virt);
}

/* MMU qiladigan ishni dasturiy takrorlash: virtual -> fizik */
bool paging_translate(uint32_t virt, uint32_t *phys)
{
    uint32_t *table = get_table(virt, false);
    if (!table)
        return false;
    uint32_t entry = table[(virt >> 12) & 0x3FF];
    if (!(entry & PAGE_PRESENT))
        return false;
    *phys = (entry & ~0xFFFu) | (virt & 0xFFF);
    return true;
}

void paging_init(void)
{
    page_directory = (uint32_t *)pmm_alloc_frame();
    memset(page_directory, 0, PAGE_SIZE);

    /* RAM oxirini 4 MB ga yaxlitlaymiz (bitta Page Table 4 MB ni qamraydi) */
    identity_limit = (pmm_memory_top() + 0x3FFFFF) & ~0x3FFFFFu;

    for (uint32_t addr = PAGE_SIZE; addr < identity_limit; addr += PAGE_SIZE)
        paging_map(addr, addr, PAGE_RW);

    /* CR3 — Page Directory manzili */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory));

    /* CR0: 31-bit (PG) — paging ni yoqish, 16-bit (WP) — faqat o'qiladigan sahifalarni himoya qilish */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

uint32_t paging_identity_limit(void)
{
    return identity_limit;
}

uint32_t paging_directory_address(void)
{
    return (uint32_t)page_directory;
}
