/*
 * user/linker.ld — dasturlarning xotiradagi joylashuvi.
 * Har bir dastur o'zining alohida xotirasida 0x40000000 dan boshlanadi.
 * Kod (text) va ma'lumot (data) alohida sahifalarda: kod faqat o'qish uchun,
 * ma'lumot — o'qish va yozish uchun.
 */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = 0x40000000;

    .text   : { *(.text.start) *(.text*) } :text
    .rodata : { *(.rodata*) }               :text

    . = ALIGN(4096);
    .data   : { *(.data*) }                 :data
    .bss    : { *(COMMON) *(.bss*) }        :data

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
