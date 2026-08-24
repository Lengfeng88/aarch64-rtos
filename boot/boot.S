.section ".text.boot"
.global _start

_start:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    beq     from_el2
    cmp     x0, #1
    beq     el1_direct
    b       hang

el1_direct:
    mov     x9, #1
    b       from_el1

from_el2:
    mov     x9, #2
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    ldr     x0, =0x00C50838
    msr     sctlr_el1, x0

    mov     x0, #0x3c5
    msr     spsr_el2, x0
    adr     x0, from_el1
    msr     elr_el2, x0
    eret

from_el1:
    ldr     x0, =__stack_top
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
clear_bss:
    cmp     x0, x1
    b.ge    bss_done
    str     xzr, [x0], #8
    b       clear_bss
bss_done:

    mov     x0, x9
    bl      kernel_main
hang:
    wfe
    b       hang