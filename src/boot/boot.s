# =====================================================================
#  boot.s — OS ning eng birinchi ishga tushadigan kodi
#
#  Kompyuter yoqilganda BIOS -> GRUB (yoki QEMU) bootloader ishlaydi.
#  Bootloader xotiradan "Multiboot header"ni qidiradi va uni topsa,
#  kernelni 1 MB manzilga yuklab, _start ga o'tadi.
#  Bu paytda protsessor allaqachon 32-bit "protected mode"da bo'ladi.
# =====================================================================

# --- Multiboot header (bootloader bizni taniy olishi uchun) ---
.set ALIGN,    1 << 0              # modullarni sahifa chegarasiga tekislash
.set MEMINFO,  1 << 1              # bizga xotira hajmi haqida ma'lumot ber
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002          # Multiboot standartidagi sehrli raqam
.set CHECKSUM, -(MAGIC + FLAGS)    # MAGIC + FLAGS + CHECKSUM = 0 bo'lishi shart

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

# --- Kernel steki (16 KB) ---
# C funksiyalar ishlashi uchun stek kerak. Uni .bss bo'limida ajratamiz.
# Stek pastga qarab o'sadi. Uning ostida bitta "qo'riqchi sahifa" (guard page)
# turadi: paging uni xaritadan chiqarib tashlaydi (v0.6.1). Stek to'lib ketsa,
# qo'shni ma'lumotni jimgina buzish o'rniga darhol exception bo'ladi.
.section .bss
.align 4096
.global boot_stack_guard
boot_stack_guard:
.skip 4096
.global boot_stack_bottom
boot_stack_bottom:
stack_bottom:
.skip 16384
stack_top:

# --- Kirish nuqtasi ---
.section .text
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp       # stek ko'rsatkichini o'rnatamiz

    # Bootloader EAX ga sehrli raqamni (0x2BADB002),
    # EBX ga esa multiboot ma'lumotlari manzilini qo'yib beradi.
    # Ularni kernel_main(magic, mbi) ga argument sifatida uzatamiz.
    push %ebx
    push %eax
    call kernel_main

    # kernel_main hech qachon qaytmasligi kerak. Qaytsa — to'xtaymiz.
    cli
1:  hlt
    jmp 1b
.size _start, . - _start

.section .note.GNU-stack, "", @progbits
