/*
 * memory.h — xotirani boshqarish: 3 qatlam
 *
 *   1. PMM  (Physical Memory Manager) — RAM ni 4 KB lik "frame"larga bo'lib,
 *           qaysi biri bo'sh, qaysi biri band ekanini kuzatadi.
 *   2. Paging — virtual manzillarni fizik manzillarga bog'laydi
 *           (page directory + page table).
 *   3. Heap — kernel uchun ixtiyoriy hajmdagi xotira: kmalloc / kfree.
 */
#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "multiboot.h"

#define PAGE_SIZE 4096

/* ------------------------------ PMM ------------------------------ */
typedef struct {
    uint32_t base;
    uint32_t length;
    uint32_t type;
} mem_region_t;

void     pmm_init(multiboot_info_t *mbi);
uint32_t pmm_alloc_frame(void);          /* bo'sh frame ning fizik manzili, 0 — xotira tugagan */
void     pmm_free_frame(uint32_t addr);       /* v0.10: hisoblagichni kamaytiradi, 0 da bo'shatadi */
void     pmm_free_frame_zero(uint32_t addr);  /* oxirgi egasi bo'lsa — nol bilan tozalab bo'shatish */
void     pmm_frame_ref(uint32_t addr);        /* frame ni yana bitta jarayon ishlatadi (fork) */
uint32_t pmm_frame_refs(uint32_t addr);
uint32_t pmm_shared_frames(void);
uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_ram_total_mb(void);         /* v0.9: kompyuterdagi butun RAM */
uint32_t pmm_memory_top(void);           /* ishlatsa bo'ladigan RAM ning oxirgi manzili */
int      pmm_region_count(void);
const mem_region_t *pmm_region(int i);

/* ----------------------------- Paging ---------------------------- */
#define PAGE_PRESENT 0x1    /* sahifa xotirada bor */
#define PAGE_RW      0x2    /* yozish mumkin */
#define PAGE_USER    0x4    /* foydalanuvchi dasturlari (ring 3) ham ko'ra oladi */
#define PAGE_PCD     0x10   /* keshlanmasin: qurilma registrlari (MMIO) uchun shart (v0.9) */
#define PAGE_COW     0x200  /* v0.10: "copy-on-write" — protsessor e'tibor bermaydigan bo'sh bit (AVL).
                               Sahifa vaqtincha faqat o'qish uchun; yozilganda nusxa olinadi */

/* v0.9: qurilma registrlari (MMIO) uchun virtual hudud — framebuffer dan keyin, 4 MB */
#define MMIO_VIRT    0xE4000000u
#define MMIO_SIZE    0x400000u

void     paging_init(void);
bool     paging_map(uint32_t virt, uint32_t phys, uint32_t flags);
void     paging_unmap(uint32_t virt);
bool     paging_translate(uint32_t virt, uint32_t *phys);
void    *paging_map_mmio(uint32_t phys, uint32_t size);   /* v0.9: qurilma registrlarini ulash */
uint32_t paging_identity_limit(void);
uint32_t paging_directory_address(void);

/* v0.5: har bir jarayonning o'z Page Directory si */
#define USER_BASE       0x40000000u   /* dasturlar shu manzildan yuklanadi */
#define USER_TOP        0xC0000000u   /* user hududining oxiri */
#define USER_STACK_TOP  0xC0000000u
#define USER_STACK_SIZE (64 * 1024)

uint32_t *paging_kernel_directory(void);
uint32_t *paging_current_directory(void);
uint32_t *paging_create_directory(void);
void      paging_free_directory(uint32_t *dir);
void      paging_switch(uint32_t *dir);
bool      paging_map_in(uint32_t *dir, uint32_t virt, uint32_t phys, uint32_t flags);
bool      paging_translate_in(uint32_t *dir, uint32_t virt, uint32_t *phys, uint32_t *flags);
bool      paging_check_range(uint32_t *dir, uint32_t virt, uint32_t len, uint32_t need_flags);

/* v0.10: fork va copy-on-write */
uint32_t *paging_fork_directory(uint32_t *src);            /* ota xotirasining CoW nusxasi */
int       paging_cow_fault(uint32_t *dir, uint32_t virt);   /* 1 — nusxa olindi, 0 — CoW emas, -1 — xotira yo'q */
bool      paging_make_writable(uint32_t *dir, uint32_t virt, uint32_t len);   /* oraliqdagi CoW ni oldindan ajratish */
void      paging_flush_tlb(void);
void      paging_cow_stats(uint32_t *copies, uint32_t *reused);

/* ------------------------------ Heap ----------------------------- */
#define HEAP_START 0xD0000000u
#define HEAP_MAX   0xE0000000u   /* heap 256 MB dan oshmaydi */

typedef struct {
    uint32_t start, end;       /* heap egallagan virtual hudud */
    uint32_t used_bytes, free_bytes;
    uint32_t used_blocks, free_blocks;
} heap_stats_t;

void  heap_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void  heap_get_stats(heap_stats_t *s);
void  heap_dump(void);           /* barcha bloklarni ekranga chiqarish */

#endif
