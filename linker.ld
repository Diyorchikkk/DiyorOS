/*
 * linker.ld — kernel faylining xotiradagi joylashuvi.
 * Kernel 1 MB (0x100000) manzildan boshlanadi: undan pastda BIOS
 * va VGA xotirasi kabi maxsus hududlar bor.
 */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    kernel_start = .;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)      /* Multiboot header birinchi 8 KB ichida bo'lishi shart */
        *(.text*)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data*)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss*)
    }

    kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
