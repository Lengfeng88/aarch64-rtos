#define MAX_TASKS 8

#include "tcb.h"
#include "percpu.h"
#include "spinlock.h"

extern void switch_to(unsigned long *old_sp_ptr, unsigned long new_sp);
extern void report_corrupt_sp(const char *where, unsigned long sp);
extern int smp_online;
extern void klog(const char *prefix, long val, int has_val, const char *suffix);

/* Per-CPU run queue. Only the owning CPU touches its own queue for now
   (no locking yet); tasks are still all registered on CPU0. */
typedef struct {
    tcb_t *tasks[MAX_TASKS];
    int num_tasks;
    int current_idx;
    unsigned long select_count[MAX_TASKS];
    spinlock_t lock;   /* protects tasks[], task state changes and current_idx */
} runqueue_t;

static runqueue_t runqueues[MAX_CPUS];

static inline runqueue_t *this_rq(void) {
    return &runqueues[this_cpu()->cpu_id];
}

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

void sched_register_on(unsigned long cpu, tcb_t *t) {
    runqueue_t *rq = &runqueues[cpu];
    unsigned long f = spin_lock_irqsave(&rq->lock);
    if (rq->num_tasks < MAX_TASKS) {
        rq->tasks[rq->num_tasks] = t;
        t->cpu = (unsigned int)cpu;
        if (cpu == this_cpu()->cpu_id && t == current) rq->current_idx = rq->num_tasks;
        rq->num_tasks++;
    }
    spin_unlock_irqrestore(&rq->lock, f);
}

void sched_register(tcb_t *t) {
    sched_register_on(this_cpu()->cpu_id, t);
}

/* Finds the next READY task after the current one, wrapping around.
   Both the timer ISR (preemption) and sem_wait (voluntary block) go
   through this single path, so a task that's BLOCKED is never handed
   the CPU by either mechanism. */
static tcb_t *roundrobin_select_next(void) {
    runqueue_t *rq = this_rq();
    for (int i = 1; i <= rq->num_tasks; i++) {
        int idx = (rq->current_idx + i) % rq->num_tasks;
        if (rq->tasks[idx]->state == 0) {
            rq->current_idx = idx;
            rq->select_count[idx]++;
            return rq->tasks[idx];
        }
    }
    /* Nobody else runnable - staying on current. current_idx must still
       be resynced here. */
    for (int i = 0; i < rq->num_tasks; i++) {
        if (rq->tasks[i] == current) {
            rq->current_idx = i;
            break;
        }
    }
    return current;
}

static tcb_t *load_aware_select_next(void) {
    runqueue_t *rq = this_rq();
    tcb_t *best = 0;
    int best_idx = -1;
    for (int i = 1; i <= rq->num_tasks; i++) {
        int idx = (rq->current_idx + i) % rq->num_tasks;
        if (rq->tasks[idx]->state == 0) {
            if (best == 0 || rq->tasks[idx]->ewma_load < best->ewma_load) {
                best = rq->tasks[idx];
                best_idx = idx;
            }
        }
    }
    if (best) {
        rq->current_idx = best_idx;
        rq->select_count[best_idx]++;
        return best;
    }
    /* Same fallback as roundrobin_select_next(): resync current_idx. */
    for (int i = 0; i < rq->num_tasks; i++) {
        if (rq->tasks[i] == current) {
            rq->current_idx = i;
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
    runqueue_t *rq = this_rq();
    unsigned long f = spin_lock_irqsave(&rq->lock);
    tcb_t *n = active_policy->select_next();
    spin_unlock_irqrestore(&rq->lock, f);
    return n;
}

/* Mark a task READY from any CPU. Tasks do not migrate: the task stays on its
   home CPU's run queue, so only that queue's lock is needed. */
void sched_wake(tcb_t *t) {
    runqueue_t *rq = &runqueues[t->cpu];
    unsigned long f = spin_lock_irqsave(&rq->lock);
    t->state = 0;
    spin_unlock_irqrestore(&rq->lock, f);
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");
}

/* M12: migrate a READY task from its current home CPU to dst_cpu.
   Caller must NOT hold either rq's lock. Only migrates tasks that are
   actually READY and sitting in their rq's tasks[] array - a task
   that is `current` on some CPU, or BLOCKED waiting on a semaphore,
   is never touched (BLOCKED tasks still rely on sched_wake's
   home-CPU-never-changes invariant).
   Lock order: always lock the lower CPU id's rq first, to avoid a new
   deadlock ordering against any future cross-rq path.
   dst_cpu must be a secondary (1..MAX_CPUS-1) for now - CPU0's own
   wfe/idle path as a migration target hasn't been verified yet. */
int sched_migrate(tcb_t *t, unsigned long dst_cpu) {
    unsigned long src_cpu = t->cpu;
    if (dst_cpu == src_cpu || dst_cpu == 0) return -1;

    unsigned long lo = src_cpu < dst_cpu ? src_cpu : dst_cpu;
    unsigned long hi = src_cpu < dst_cpu ? dst_cpu : src_cpu;
    runqueue_t *rq_lo = &runqueues[lo];
    runqueue_t *rq_hi = &runqueues[hi];

    unsigned long f_lo = spin_lock_irqsave(&rq_lo->lock);
    spin_lock(&rq_hi->lock); /* nested, same core, IRQ already masked by f_lo */

    runqueue_t *src = &runqueues[src_cpu];
    runqueue_t *dst = &runqueues[dst_cpu];
    int ok = 0;

    /* Same bug/fix as sched_load_balance_pass's victim scan: `current`
       is the CALLER's per-CPU curr, not src_cpu's. A task genuinely
       executing on src_cpu must never be pulled out from under it -
       compare against cpu_locals[src_cpu].curr, not the caller's own
       current. Defends sched_migrate() itself even if some future
       caller doesn't already filter this the way
       sched_load_balance_pass does. */
    if (t->state == 0 && t != cpu_locals[src_cpu].curr) {
        int idx = -1;
        for (int i = 0; i < src->num_tasks; i++) {
            if (src->tasks[i] == t) { idx = i; break; }
        }
        if (idx >= 0 && dst->num_tasks < MAX_TASKS) {
            src->tasks[idx] = src->tasks[src->num_tasks - 1];
            src->num_tasks--;
            if (src->current_idx >= src->num_tasks) src->current_idx = 0;

            dst->tasks[dst->num_tasks] = t;
            t->cpu = (unsigned int)dst_cpu;
            dst->num_tasks++;
            ok = 1;
        }
    }

    spin_unlock(&rq_hi->lock);
    spin_unlock_irqrestore(&rq_lo->lock, f_lo);
    if (ok) {
        __asm__ volatile("dsb sy\n\tsev" ::: "memory");
        klog("LB: migrated task, src=", (long)src_cpu, 1, "");
        klog(" dst=", (long)dst_cpu, 1, "\r\n");
    }
    return ok ? 0 : -1;
}

/* M12: very simple first cut - scan CPU1..smp_online-1's num_tasks, if
   busiest/idlest differ enough, move one READY task busiest -> idlest.
   Not wired to any automatic trigger yet; called manually under
   LOAD_BALANCE_SELFTEST to validate sched_migrate() in isolation. */
void sched_load_balance_pass(void) {
    int busiest = -1, idlest = -1;
    int busiest_n = -1, idlest_n = 1 << 30;
    for (int cpu = 1; cpu < smp_online; cpu++) {
        int n = runqueues[cpu].num_tasks;
        if (n > busiest_n) { busiest_n = n; busiest = cpu; }
        if (n < idlest_n)  { idlest_n = n; idlest = cpu; }
    }
    if (busiest < 0 || idlest < 0 || busiest == idlest) return;
    if (busiest_n - idlest_n < 2) return;

    /* BUG FIX (found via 30x regression, run 20/30 crashed with a
       SYNC EXCEPTION / unresolved-SP fault): `current` is a per-CPU
       macro (this_cpu()->curr) - on CPU0, comparing against it here
       only ever matches CPU0's own running task, never the task
       actually executing on the busiest CPU (e.g. CPU1). That let a
       task genuinely running on CPU1 be selected as a migration
       victim and yanked into CPU3's rq mid-execution, corrupting its
       context. Must compare against the busiest CPU's OWN current
       task, read directly from cpu_locals[] (a plain global array,
       safe to read cross-core here - only ever written by the
       owning CPU itself via percpu_init/switch_to). */
    unsigned long f = spin_lock_irqsave(&runqueues[busiest].lock);
    tcb_t *victim = 0;
    for (int i = 0; i < runqueues[busiest].num_tasks; i++) {
        tcb_t *t = runqueues[busiest].tasks[i];
        if (t->state == 0 && t != cpu_locals[busiest].curr && !t->pinned) { victim = t; break; }
    }
    spin_unlock_irqrestore(&runqueues[busiest].lock, f);

    if (victim) sched_migrate(victim, (unsigned long)idlest);
}

void sched_on_tick(void) {
    if (active_policy->on_tick) {
        active_policy->on_tick(current);
    }
}

/* Debug-only accessors (CPU0's run queue; all tasks live there for now). */
int sched_debug_current_idx(void) { return runqueues[0].current_idx; }
int sched_debug_num_tasks(void) { return runqueues[0].num_tasks; }
int sched_debug_task_state(int i) { return (i < 0 || i >= runqueues[0].num_tasks) ? -1 : runqueues[0].tasks[i]->state; }
void *sched_debug_task_ptr(int i) { return (i < 0 || i >= runqueues[0].num_tasks) ? (void *)0 : (void *)runqueues[0].tasks[i]; }
unsigned long sched_debug_select_count(int i) { return runqueues[0].select_count[i]; }

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