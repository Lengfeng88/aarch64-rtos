#define STACK_WORDS 512
#define MAX_TASKS 4

typedef struct {
    unsigned long sp;
    unsigned long stack[STACK_WORDS];
    const char *name;
    void (*entry)(void);
    int state;
} tcb_t;

extern tcb_t *current;
extern void switch_to(unsigned long *old_sp_ptr, unsigned long new_sp);
extern void report_corrupt_sp(const char *where, unsigned long sp);

static tcb_t *all_tasks[MAX_TASKS];
static int num_tasks = 0;
static int current_idx = 0;

/* NEW (window-3 fix only): needed so checked_switch_to() can mask IRQ
   around its own post-switch bounds check. Not used anywhere else in
   this file - yield() itself is back to its original, unmodified
   form below. */
static inline unsigned long irq_disable_save(void) {
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    __asm__ volatile("msr daifset, #2");
    return flags;
}

static inline void irq_restore(unsigned long flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags));
}

void sched_register(tcb_t *t) {
    all_tasks[num_tasks] = t;
    if (t == current) current_idx = num_tasks;
    num_tasks++;
}

/* Finds the next READY task after the current one, wrapping around.
   Both the timer ISR (preemption) and sem_wait (voluntary block) go
   through this single path, so a task that's BLOCKED is never handed
   the CPU by either mechanism. */
tcb_t *pick_next_ready(void) {
    for (int i = 1; i <= num_tasks; i++) {
        int idx = (current_idx + i) % num_tasks;
        if (all_tasks[idx]->state == 0) {
            current_idx = idx;
            return all_tasks[idx];
        }
    }
    /* Nobody else runnable - staying on current. See sem_wait()'s
       comment for why current_idx must still be resynced here. */
    for (int i = 0; i < num_tasks; i++) {
        if (all_tasks[i] == current) {
            current_idx = i;
            break;
        }
    }
    return current;
}

/* Debug-only accessors for tracking down the M7 scheduling issue. */
int sched_debug_current_idx(void) { return current_idx; }
int sched_debug_num_tasks(void) { return num_tasks; }
int sched_debug_task_state(int i) { return all_tasks[i]->state; }
void *sched_debug_task_ptr(int i) { return (void *)all_tasks[i]; }

/* Wraps switch_to() with a post-switch sanity check. Once control
   returns here (prev has been switched back in - possibly much later,
   by a completely different call site), prev->sp should hold a value
   that will make sense the NEXT time prev is switched away/in again.
   If it doesn't fall inside prev's own stack[] array, the corruption
   happened during THIS switch_to() call - which pins down both the
   call site (via `where`) and the moment.

   WINDOW-3 FIX: switch_to() returns with IRQ unmasked (it restores
   the resumed task's own saved daif right before its ret). That
   means the bounds check below used to run with IRQ open - a nested
   timer IRQ landing in the middle of it could preempt away before
   the check (and its report_corrupt_sp call, if bad) ever completes,
   letting the corruption go unreported or interleaving with another
   task's own switch_to() call. Masking IRQ around just the check
   (not the switch_to() call itself, which already manages its own
   masking internally) closes that gap without changing when the
   actual task switch happens. */
static inline void checked_switch_to(tcb_t *prev, tcb_t *next, const char *where) {
    switch_to(&prev->sp, next->sp);
    unsigned long flags = irq_disable_save();
    unsigned long lo = (unsigned long)&prev->stack[0];
    unsigned long hi = lo + STACK_WORDS * sizeof(unsigned long);
    int bad = (prev->sp < lo || prev->sp >= hi);
    unsigned long bad_sp = prev->sp;
    irq_restore(flags);
    if (bad) {
        report_corrupt_sp(where, bad_sp);
    }
}

/* Voluntary yield: give up the CPU to another READY task without
   blocking. Back to its original, pre-window-1 form. */
void yield(void) {
    tcb_t *next = pick_next_ready();
    if (next != current) {
        tcb_t *prev = current;
        current = next;
        checked_switch_to(prev, next, "CORRUPT sp after yield() switch_to, sp=");
    }
}