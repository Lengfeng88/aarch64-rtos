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
    __asm__ volatile("msr daifclr, #2");
    current->entry();
    while (1) { __asm__ volatile("wfe"); }
}

void task_init(tcb_t *t, void (*entry)(void)) {
    t->entry = entry;
    t->stack[0] = CANARY;
    t->stack[STACK_WORDS - 1] = CANARY;
    unsigned long top = (unsigned long)&t->stack[STACK_WORDS - 1];
    unsigned long *sp = (unsigned long *)(top - 96);
    for (int i = 0; i < 12; i++) sp[i] = 0;
    sp[1] = (unsigned long)task_trampoline;
    t->sp = (unsigned long)sp;
}