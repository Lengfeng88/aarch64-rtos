ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text.boot : { *(.text.boot) }
    .text.vectors : { *(.text.vectors) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    __bss_end = .;

    . = ALIGN(16);
    . += 0x10000;
    __stack_top = .;
}
