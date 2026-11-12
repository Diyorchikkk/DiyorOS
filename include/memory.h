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
void     pmm_free_frame(uint32_t addr);
uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_memory_top(void);           /* ishlatsa bo'ladigan RAM ning oxirgi manzili */
int      pmm_region_count(void);
const mem_region_t *pmm_region(int i);

/* ----------------------------- Paging ---------------------------- */
#define PAGE_PRESENT 0x1    /* sahifa xotirada bor */
#define PAGE_RW      0x2    /* yozish mumkin */
#define PAGE_USER    0x4    /* foydalanuvchi dasturlari (ring 3) ham ko'ra oladi */

void     paging_init(void);
bool     paging_map(uint32_t virt, uint32_t phys, uint32_t flags);
void     paging_unmap(uint32_t virt);
bool     paging_translate(uint32_t virt, uint32_t *phys);
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
