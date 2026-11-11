# =====================================================================
#  interrupts.s — GDT/IDT ni yuklash va uzilishlar (interrupt) uchun
#  "stub" (kirish) funksiyalari.
#
#  Protsessor uzilish sodir bo'lganda IDT jadvalidagi manzilga sakraydi.
#  Har bir uzilish uchun kichik stub yozamiz: u uzilish raqamini stekka
#  qo'yadi va umumiy isr_common ga o'tadi. isr_common barcha
#  registrlarni saqlab, C dagi interrupt_handler() ni chaqiradi.
# =====================================================================

.section .text

# void gdt_flush(uint32_t gdt_ptr_manzili)
.global gdt_flush
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)               # yangi GDT ni protsessorga yuklaymiz
    mov $0x10, %ax            # 0x10 = GDT dagi kernel DATA segmenti
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ljmp $0x08, $1f           # 0x08 = kernel CODE segmenti (CS ni yangilash)
1:  ret

# void idt_load(uint32_t idt_ptr_manzili)
.global idt_load
idt_load:
    mov 4(%esp), %eax
    lidt (%eax)               # IDT ni protsessorga yuklaymiz
    ret

# --- Stub makroslari ---
# Ba'zi exceptionlar (8, 10-14, 17, 21, 29, 30) protsessor o'zi
# "error code" qo'yadi. Qolganlari uchun stekni bir xil qilish
# maqsadida o'zimiz 0 qo'yamiz.
.macro ISR_NOERR n
.global isr\n
isr\n:
    push $0                   # soxta error code
    push $\n                  # uzilish raqami
    jmp isr_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    push $\n                  # error code ni protsessor qo'ygan
    jmp isr_common
.endm

# 0-31: protsessor exceptionlari, 32-47: qurilma uzilishlari (IRQ 0-15)
.irp i, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
    ISR_NOERR \i
.endr
.irp i, 8,10,11,12,13,14,17,21,29,30
    ISR_ERR \i
.endr
.irp i, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    ISR_NOERR \i
.endr
# 0x81 (129): task_yield() — vazifa navbatni o'zi beradi (v0.3)
ISR_NOERR 129

# --- Umumiy ishlov beruvchi ---
isr_common:
    pusha                     # EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax            # kernel data segmentiga o'tamiz
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push %esp                 # registers_t* sifatida C ga uzatiladi
    cld
    call interrupt_handler
    # interrupt_handler qaysi vazifaning registrlarini tiklash kerakligini qaytaradi.
    # Boshqa vazifa bo'lsa — stek ko'rsatkichi o'sha vazifa stekiga o'tadi:
    # bu KONTEKST ALMASHTIRISH (v0.3).
    mov %eax, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp              # uzilish raqami va error code ni olib tashlaymiz
    iret                      # uzilishdan oldingi joyga qaytamiz

# --- Stub manzillari jadvali (idt.c shu jadvaldan foydalanadi) ---
.section .rodata
.global isr_stub_table
isr_stub_table:
.irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .long isr\i
.endr

.section .note.GNU-stack, "", @progbits
