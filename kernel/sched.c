#define MAX_TASKS 4

#include "tcb.h"
#include "percpu.h"

extern void switch_to(unsigned long *old_sp_ptr, unsigned long new_sp);
extern void report_corrupt_sp(const char *where, unsigned long sp);

static tcb_t *all_tasks[MAX_TASKS];
static int num_tasks = 0;
static int current_idx = 0;
static unsigned long select_count[MAX_TASKS];

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
static tcb_t *roundrobin_select_next(void) {
    for (int i = 1; i <= num_tasks; i++) {
        int idx = (current_idx + i) % num_tasks;
        if (all_tasks[idx]->state == 0) {
            current_idx = idx;
            select_count[idx]++;
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

static tcb_t *load_aware_select_next(void) {
    tcb_t *best = 0;
    int best_idx = -1;
    for (int i = 1; i <= num_tasks; i++) {
        int idx = (current_idx + i) % num_tasks;
        if (all_tasks[idx]->state == 0) {
            if (best == 0 || all_tasks[idx]->ewma_load < best->ewma_load) {
                best = all_tasks[idx];
                best_idx = idx;
            }
        }
    }
    if (best) {
        current_idx = best_idx;
        select_count[best_idx]++;
        return best;
    }
    /* 跟roundrobin_select_next()一样的fallback，state同步逻辑不变 */
    for (int i = 0; i < num_tasks; i++) {
        if (all_tasks[i] == current) {
            current_idx = i;
            break;
        }
    }
    return current;
}

typedef tcb_t* (*select_next_fn)(void);
typedef void (*on_tick_fn)(tcb_t *cur);
typedef struct {
    select_next_fn select_next;
    on_tick_fn on_tick;
} sched_policy_t;

static sched_policy_t policy_roundrobin = { roundrobin_select_next, 0 };

#define EWMA_ALPHA_NUM 3
#define EWMA_ALPHA_DEN 10
#define EWMA_SCALE 1000

static void ewma_on_tick(tcb_t *cur) {
    cur->ewma_load = (EWMA_ALPHA_NUM * EWMA_SCALE 
                     + (EWMA_ALPHA_DEN - EWMA_ALPHA_NUM) * cur->ewma_load) 
                     / EWMA_ALPHA_DEN;
}

static sched_policy_t policy_ewma = { roundrobin_select_next, ewma_on_tick };
static sched_policy_t policy_load_aware = { load_aware_select_next, ewma_on_tick };
static sched_policy_t *active_policy = &policy_load_aware;

tcb_t *pick_next_ready(void) {
    return active_policy->select_next();
}

void sched_on_tick(void) {
    if (active_policy->on_tick) {
        active_policy->on_tick(current);
    }
}

/* Debug-only accessors for tracking down the M7 scheduling issue. */
int sched_debug_current_idx(void) { return current_idx; }
int sched_debug_num_tasks(void) { return num_tasks; }
int sched_debug_task_state(int i) { return all_tasks[i]->state; }
void *sched_debug_task_ptr(int i) { return (void *)all_tasks[i]; }
unsigned long sched_debug_select_count(int i) { return select_count[i]; }

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
extern void record_switch(const char *site, void *prev_p, void *next_p);

extern void report_pre_switch_bad(const char *where, void *next_p, unsigned long bad_sp);

#ifdef DEBUG_HOOKS
static inline void checked_switch_to(tcb_t *prev, tcb_t *next, const char *where) {
    record_switch(where, prev, next);
    {
        unsigned long nlo = (unsigned long)&next->stack[0];
        unsigned long nhi = nlo + STACK_WORDS * sizeof(unsigned long);
        if (next->sp < nlo || next->sp >= nhi) {
            report_pre_switch_bad(where, next, next->sp);
        }
    }
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
#else
static inline void checked_switch_to(tcb_t *prev, tcb_t *next, const char *where) {
    (void)where;
    switch_to(&prev->sp, next->sp);
}
#endif

/* Voluntary yield: give up the CPU to another READY task without
   blocking. Back to its original, pre-window-1 form. */
void yield(void) {
    unsigned long flags = irq_disable_save();
    tcb_t *next = pick_next_ready();
    if (next != current) {
        tcb_t *prev = current;
        current = next;
        checked_switch_to(prev, next, "CORRUPT sp after yield() switch_to, sp=");
    }
    irq_restore(flags);
}