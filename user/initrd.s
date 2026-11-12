# =====================================================================
#  initrd.s — dasturlarni kernel fayliga "yopishtirish".
#
#  Diskimiz hali yo'q (v0.6), shuning uchun yig'ilgan dasturlar (ELF fayllar)
#  .incbin bilan kernel ichiga joylanadi. Yuklanishda kernel ularni
#  /bin papkasiga oddiy fayl sifatida yozadi (process.c: initrd_install).
#
#  Yangi dastur qo'shish:
#    1. user/<nom>.c yozing
#    2. Makefile dagi USER_PROGS ga <nom> ni qo'shing
#    3. Pastdagi ikkala ro'yxatga ham bitta qator qo'shing
# =====================================================================

.macro ENTRY name
    .long name_\name, start_\name, end_\name
.endm

.macro BLOB name
name_\name:
    .asciz "\name"
    .balign 4
start_\name:
    .incbin "build/user/\name"
end_\name:
.endm

.section .rodata
.balign 4
.global initrd_table
initrd_table:
    ENTRY hello
    ENTRY guess
    ENTRY primes
    ENTRY hack
    ENTRY isolate
    ENTRY kundalik
    .long 0, 0, 0              # ro'yxat oxiri

    BLOB hello
    BLOB guess
    BLOB primes
    BLOB hack
    BLOB isolate
    BLOB kundalik

.section .note.GNU-stack, "", @progbits
