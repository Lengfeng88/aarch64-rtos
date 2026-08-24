.section ".text.vectors"
.align 11
.global vectors
vectors:
    .align 7
    b   hang_vec
    .align 7
    b   hang_vec
    .align 7
    b   hang_vec
    .align 7
    b   hang_vec

    .align 7
    b   sync_el1h
    .align 7
    b   irq_el1h
    .align 7
    b   hang_vec
    .align 7
    b   hang_vec

    .align 7
    b   hang_vec
    .align 7
    b   hang_vec
    .align 7
    b   hang_vec
    .align 7
    b   hang_vec

    .align 7
    b   hang_vec
    .align 7
    b   hang_vec
    .align 7
    b   hang_vec
    .align 7
    b   hang_vec

.global sync_el1h
sync_el1h:
    // Full register dump for deep debugging - unlike the earlier
    // minimal version (x0/x1 + esr/elr only), we need to see EVERY
    // register's value at fault time to find which one held the bad
    // address that caused a wild branch (ELR pointing at an invalid
    // address like 0x0 means something jumped there - the interesting
    // question is which register held that 0).
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    stp     x0, x1, [sp, #248]
    mov     x0, sp
    bl      sync_exception_handler_full
hang_after_sync:
    wfe
    b       hang_after_sync

.global irq_el1h
irq_el1h:
    // Save the full interrupted context. An IRQ can land anywhere, not
    // just at a function-call boundary, so unlike switch_to we cannot
    // rely on the AAPCS64 callee-saved convention here — every general
    // register plus ELR_EL1/SPSR_EL1 must be saved explicitly.
    sub     sp, sp, #272            // 34 * 8 bytes, 16-byte aligned
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]

    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x30, x0, [sp, #240]
    str     x1, [sp, #256]

    bl      irq_handler

    ldp     x30, x0, [sp, #240]
    ldr     x1, [sp, #256]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #272
    eret

hang_vec:
    wfe
    b       hang_vec