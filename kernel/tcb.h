#ifndef TCB_H
#define TCB_H
/* Single definition of the task control block. Do not redeclare tcb_t in .c
 * files: every file must see the same layout. */

#define STACK_WORDS 512

typedef struct {
    unsigned long sp;               /* saved SP; must stay first (offset 0) */
    unsigned long stack[STACK_WORDS];
    const char *name;
    void (*entry)(void);
    int state;                      /* 0 = READY, 1 = BLOCKED */
    unsigned int ewma_load;
    unsigned int cpu;               /* home CPU: the run queue that owns this task */
} __attribute__((aligned(16))) tcb_t;

/* Offsets taken from the built binary (task_init, task_trampoline, and the
 * stack range checks in main.c). */
_Static_assert(__builtin_offsetof(tcb_t, sp)    == 0,      "tcb_t.sp must be at offset 0");
_Static_assert(__builtin_offsetof(tcb_t, stack) == 8,      "tcb_t.stack must start at offset 8");
_Static_assert(__builtin_offsetof(tcb_t, entry) == 0x1010, "tcb_t.entry must be at offset 0x1010");
_Static_assert(sizeof(tcb_t) == 0x1030,                    "sizeof(tcb_t) changed");
_Static_assert(__builtin_offsetof(tcb_t, cpu) == 0x1020,        "tcb_t.cpu must be at offset 0x1020");
_Static_assert(_Alignof(tcb_t) >= 16,                     "tcb_t must be 16-byte aligned (SP alignment)");

#endif
