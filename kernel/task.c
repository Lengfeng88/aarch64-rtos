#define STACK_WORDS 512
#define CANARY 0xC0FFEEDEADBEEFULL

typedef struct {
    unsigned long sp;
    unsigned long stack[STACK_WORDS];
    const char *name;
    void (*entry)(void);
    int state;
} tcb_t;

extern void switch_to(unsigned long *old_sp_ptr, unsigned long new_sp);
extern tcb_t *current;

void task_trampoline(void) {
    /* Kept as a harmless extra safety net - now redundant with
       task_init()'s fake frame already setting daif=0 (IRQ unmasked)
       for a task's first-ever activation, but doesn't hurt to leave
       it as a second line of defense. */
    __asm__ volatile("msr daifclr, #2");
    current->entry();
    while (1) { __asm__ volatile("wfe"); }
}

void task_init(tcb_t *t, void (*entry)(void)) {
    t->entry = entry;
    t->stack[0] = CANARY;
    t->stack[STACK_WORDS - 1] = CANARY;
    unsigned long top = (unsigned long)&t->stack[STACK_WORDS - 1];

    /* NEW frame size: 112 bytes (14 words), up from 96 - switch_to.S
       now pushes/pops an extra 16-byte slot for the saved daif value,
       laid out (low address = popped first) as:
         sp[0]  = daif   (only low 8 bytes used, sp[1] is alignment padding)
         sp[2]  = x29
         sp[3]  = x30   <- return address, must be task_trampoline
         sp[4..5]   = x27,x28
         sp[6..7]   = x25,x26
         sp[8..9]   = x23,x24
         sp[10..11] = x21,x22
         sp[12..13] = x19,x20
       This must exactly match switch_to.S's pop order or a task's
       first activation will restore garbage into these registers. */
    unsigned long *sp = (unsigned long *)(top - 112);
    for (int i = 0; i < 14; i++) sp[i] = 0;
    sp[0] = 0;                              /* initial daif: 0 = IRQ unmasked */
    sp[3] = (unsigned long)task_trampoline; /* x30 */
    t->sp = (unsigned long)sp;
}