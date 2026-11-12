/*
 * elf.h — ELF (Executable and Linkable Format) fayl tuzilmasi.
 * Linux va ko'pchilik Unix tizimlaridagi dasturlar shu formatda.
 *
 *   [ ELF sarlavha ][ dastur sarlavhalari (segmentlar) ] ... [ kod ] [ ma'lumot ] ...
 *
 * Bizga faqat PT_LOAD segmentlar kerak: "fayldagi shu baytlarni xotiraning
 * shu manziliga joylashtir" degan ko'rsatmalar.
 */
#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define ELF_MAGIC   0x464C457Fu   /* "\x7F" 'E' 'L' 'F' */
#define ELF_CLASS32 1
#define ELF_LSB     1             /* little-endian */
#define ET_EXEC     2             /* bajariladigan fayl */
#define EM_386      3             /* Intel 80386 */

#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4

typedef struct {
    uint32_t magic;
    uint8_t  class, data, version, osabi, abiversion, pad[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;       /* dastur qaysi manzildan boshlanadi */
    uint32_t phoff;       /* dastur sarlavhalari fayldagi joyi */
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;       /* nechta segment */
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf_header_t;

typedef struct {
    uint32_t type;        /* PT_LOAD — xotiraga yuklanadigan segment */
    uint32_t offset;      /* fayldagi joyi */
    uint32_t vaddr;       /* xotiradagi manzili */
    uint32_t paddr;
    uint32_t filesz;      /* fayldagi hajmi */
    uint32_t memsz;       /* xotiradagi hajmi (filesz dan katta bo'lsa — qolgani nol: .bss) */
    uint32_t flags;       /* PF_R / PF_W / PF_X */
    uint32_t align;
} __attribute__((packed)) elf_phdr_t;

#endif
