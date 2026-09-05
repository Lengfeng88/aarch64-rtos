.section ".text"
.global switch_to
// void switch_to(unsigned long *old_sp_ptr, unsigned long new_sp)
//
// switch_to masks IRQs for its own entire duration (entry to ret),
// and treats DAIF as part of each task's saved context rather than a
// CPU-global value the caller has to manage. This closes the window
// where a timer interrupt landing mid-switch (between the old task's
// sp being saved and the new task's sp being loaded, or during the
// register push/pop itself) could corrupt whichever tcb_t->sp is
// being written at that moment.
switch_to:
    mrs     x3, daif          // capture caller's (old task's) current daif
    msr     daifset, #2       // mask IRQ for the whole switch - restored
                               // to the NEW task's own saved value only
                               // at the very end, right before ret

    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x27, x28, [sp, #-16]!
    stp     x29, x30, [sp, #-16]!
    str     x3,       [sp, #-16]!   // old task's original daif, saved as
                                      // part of ITS context (16-byte slot
                                      // to keep sp 16-byte aligned)

    mov     x2, sp
    str     x2, [x0]          // *old_sp_ptr = old task's fully-saved sp

    mov     sp, x1            // switch to new task's stack

    mov     x0, x1
    bl      trace_switch_resume

    ldr     x3,       [sp], #16     // new task's saved daif
    ldp     x29, x30, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16

    msr     daif, x3          // restore IRQ mask state to whatever the
                               // NEW task had when IT was last switched
                               // away - not the caller's, so nothing
                               // leaks between tasks
    ret
