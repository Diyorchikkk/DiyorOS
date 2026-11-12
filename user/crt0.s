# =====================================================================
#  crt0.s — har bir dasturning eng birinchi ishga tushadigan kodi.
#
#  Kernel dasturni ishga tushirganda stek quyidagicha bo'ladi:
#     ESP   -> argc
#     ESP+4 -> argv (satrlar massivi)
#  crt0 ularni main(argc, argv) ga uzatadi, main qaytsa — exit(natija).
# =====================================================================
.section .text.start
.global _start
_start:
    mov (%esp), %eax        # argc
    mov 4(%esp), %ebx       # argv
    push %ebx
    push %eax
    call main
    push %eax               # main qaytargan qiymat — chiqish kodi
    call exit
1:  jmp 1b

.section .note.GNU-stack, "", @progbits
