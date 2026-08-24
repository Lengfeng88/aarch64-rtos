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

static tcb_t *all_tasks[MAX_TASKS];
static int num_tasks = 0;
static int current_idx = 0;

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
    /* Nobody else runnable - staying on current. This branch is only
       reached when current itself just became BLOCKED (so even the
       self-check in the loop above failed), which means current_idx
       never got updated to match current's own real index above -
       every future call (from the timer, or another task blocking)
       would then scan starting from a stale, wrong position, silently
       skipping whoever's actually running. Sync current_idx to
       current's real index before returning, so the bookkeeping stays
       correct for whatever calls this next. */
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

/* Voluntary yield: give up the CPU to another READY task without
   blocking. Safe to call directly from task context (interrupts are
   already enabled here, so unlike sem_wait there's no DAIF-leak risk
   to worry about - this is the same trick M3 used before preemption
   existed at all). */
void yield(void) {
    tcb_t *next = pick_next_ready();
    if (next != current) {
        tcb_t *prev = current;
        current = next;
        switch_to(&prev->sp, next->sp);
    }
}