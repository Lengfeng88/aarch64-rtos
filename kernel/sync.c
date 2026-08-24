#define STACK_WORDS 512

typedef struct {
    unsigned long sp;
    unsigned long stack[STACK_WORDS];
    const char *name;
    void (*entry)(void);
    int state;   /* 0 = READY, 1 = BLOCKED */
} tcb_t;

typedef struct {
    volatile int count;
    tcb_t *waiter;   /* single-waiter simplification: fine for a demo
                        with one producer and one consumer; a real
                        implementation would use a wait queue here */
} sem_t;

extern void switch_to(unsigned long *old_sp_ptr, unsigned long new_sp);
extern tcb_t *current;
extern tcb_t *pick_next_ready(void);

static inline unsigned long irq_disable_save(void) {
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    __asm__ volatile("msr daifset, #2");
    return flags;
}

static inline void irq_restore(unsigned long flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags));
}

void sem_init(sem_t *s, int initial_count) {
    s->count = initial_count;
    s->waiter = 0;
}

void sem_wait(sem_t *s) {
    unsigned long flags = irq_disable_save();
    if (s->count > 0) {
        s->count--;
        irq_restore(flags);
        return;
    }
    /* No permits available: block. We must not leave IRQ disabled across
       the switch_to below - PSTATE.I is live CPU state, not something
       switch_to saves per-task, so if we switched away with it still
       masked the task we switch into would also run with interrupts
       masked. Re-enable before switching, once our own state (BLOCKED +
       registered as the waiter) is consistent. */
    s->waiter = current;
    current->state = 1;
    irq_restore(flags);

    tcb_t *prev = current;
    tcb_t *next = pick_next_ready();
    if (next == prev) {
        /* Nobody else in the whole system is READY right now - every
           task is blocked on something. pick_next_ready()'s only
           option was to hand back the very task that just marked
           itself BLOCKED. Calling switch_to(&prev->sp, next->sp) here
           would be switch_to(&X->sp, X->sp): it overwrites X->sp with
           X's CURRENT (mid-sem_wait) stack pointer, then tries to
           resume from the value X->sp held BEFORE this call (an
           already-consumed earlier frame - e.g. the fake initial
           frame from task_init(), stale since this task's first
           activation) - restoring garbage and returning through a
           dead pointer. This never came up before M7's 3-worker demo
           because some other task was always still READY whenever
           one blocked; it's the first scenario where literally every
           task can be simultaneously BLOCKED with nothing to switch
           to. Instead of a context switch, just wait for a hardware
           interrupt to make us READY again (sem_post from an ISR is
           the only thing that can, since nothing else is running),
           re-checking our own state each time we wake. */
        while (current->state != 0) {
            __asm__ volatile("wfe");
        }
        return;
    }
    current = next;
    switch_to(&prev->sp, next->sp);
    /* Resumes here once sem_post() marks us READY again and the
       scheduler (preemptive or another voluntary yield) switches back. */
}

void sem_post(sem_t *s) {
    unsigned long flags = irq_disable_save();
    if (s->waiter) {
        tcb_t *w = s->waiter;
        s->waiter = 0;
        w->state = 0;   /* READY: hand the permit directly to the
                            waiter rather than incrementing count, so
                            wake-ups aren't lost between the count check
                            and the waiter registering itself */
    } else {
        s->count++;
    }
    irq_restore(flags);
}

typedef struct {
    volatile int locked;
    tcb_t *owner;
    tcb_t *waiter;   /* same single-waiter simplification as sem_t */
} mutex_t;

void mutex_init(mutex_t *m) {
    m->locked = 0;
    m->owner = 0;
    m->waiter = 0;
}

void mutex_lock(mutex_t *m) {
    unsigned long flags = irq_disable_save();
    if (!m->locked) {
        m->locked = 1;
        m->owner = current;
        irq_restore(flags);
        return;
    }
    m->waiter = current;
    current->state = 1;
    irq_restore(flags);

    tcb_t *next = pick_next_ready();
    tcb_t *prev = current;
    current = next;
    switch_to(&prev->sp, next->sp);
    /* Resumes here once mutex_unlock() hands ownership directly to us. */
}

void mutex_unlock(mutex_t *m) {
    unsigned long flags = irq_disable_save();
    if (m->waiter) {
        tcb_t *w = m->waiter;
        m->waiter = 0;
        m->owner = w;      /* ownership transfers directly to the
                               waiter - locked stays 1 throughout, so
                               there's no window where the mutex looks
                               free to a third task */
        w->state = 0;
    } else {
        m->locked = 0;
        m->owner = 0;
    }
    irq_restore(flags);
}

typedef struct {
    volatile unsigned int flags;
    tcb_t *waiter;
    unsigned int wait_mask;
    int wait_all;   /* 0 = wake on ANY bit in wait_mask, 1 = wake only
                        once ALL bits in wait_mask are set */
    unsigned int wake_result;   /* captured inside the critical section
                                    at the exact moment we decide to
                                    wake the waiter - the woken task
                                    reads this instead of re-reading
                                    live e->flags later, which could by
                                    then have been changed again by
                                    someone else (e.g. a second
                                    event_clear()+event_set() cycle
                                    that runs before the woken task
                                    actually gets scheduled) */
} event_t;

void event_init(event_t *e) {
    e->flags = 0;
    e->waiter = 0;
    e->wait_mask = 0;
    e->wait_all = 0;
    e->wake_result = 0;
}

static int event_satisfied(event_t *e) {
    if (e->wait_all) return (e->flags & e->wait_mask) == e->wait_mask;
    return (e->flags & e->wait_mask) != 0;
}

void event_set(event_t *e, unsigned int bits) {
    unsigned long flags = irq_disable_save();
    e->flags |= bits;
    if (e->waiter && event_satisfied(e)) {
        tcb_t *w = e->waiter;
        e->waiter = 0;
        e->wake_result = e->flags & e->wait_mask;   /* lock in the
                                                         result now,
                                                         while we know
                                                         it's correct */
        w->state = 0;
    }
    irq_restore(flags);
}

void event_clear(event_t *e, unsigned int bits) {
    unsigned long flags = irq_disable_save();
    e->flags &= ~bits;
    irq_restore(flags);
}

unsigned int event_wait(event_t *e, unsigned int mask, int wait_all) {
    unsigned long flags = irq_disable_save();
    e->wait_mask = mask;
    e->wait_all = wait_all;
    if (event_satisfied(e)) {
        unsigned int result = e->flags & mask;
        irq_restore(flags);
        return result;
    }
    e->waiter = current;
    current->state = 1;
    irq_restore(flags);

    tcb_t *next = pick_next_ready();
    tcb_t *prev = current;
    current = next;
    switch_to(&prev->sp, next->sp);
    /* Resumes here once event_set() finds the wait condition satisfied
       and marks us READY again. Read the result event_set() captured
       at wake time - NOT live e->flags, which may have moved on by
       now (see the struct comment on wake_result). */
    return e->wake_result;
}

#define QUEUE_CAP 4

typedef struct {
    unsigned long buf[QUEUE_CAP];
    int head, tail, count;
    tcb_t *send_waiter;   /* blocked producer, waiting for a free slot */
    tcb_t *recv_waiter;   /* blocked consumer, waiting for an item */
} queue_t;

void queue_init(queue_t *q) {
    q->head = q->tail = q->count = 0;
    q->send_waiter = 0;
    q->recv_waiter = 0;
}

/* Unlike sem/mutex/event's one-shot wake, a blocked sender or receiver
   here still has to actually perform its own push/pop after waking -
   waking it just means "a slot might be available now", not "here is
   your slot". A retry loop is simpler to get right than trying to hand
   off a specific queue slot directly. */
void queue_send(queue_t *q, unsigned long item) {
    while (1) {
        unsigned long flags = irq_disable_save();
        if (q->count < QUEUE_CAP) {
            q->buf[q->tail] = item;
            q->tail = (q->tail + 1) % QUEUE_CAP;
            q->count++;
            if (q->recv_waiter) {
                tcb_t *w = q->recv_waiter;
                q->recv_waiter = 0;
                w->state = 0;
            }
            irq_restore(flags);
            return;
        }
        q->send_waiter = current;
        current->state = 1;
        irq_restore(flags);

        tcb_t *next = pick_next_ready();
        tcb_t *prev = current;
        current = next;
        switch_to(&prev->sp, next->sp);
        /* Woken because a slot was freed - loop back and retry rather
           than assume our specific slot is still there (a third task
           could in principle have taken it first in a more general
           system than this single-producer/single-consumer demo). */
    }
}

unsigned long queue_recv(queue_t *q) {
    while (1) {
        unsigned long flags = irq_disable_save();
        if (q->count > 0) {
            unsigned long item = q->buf[q->head];
            q->head = (q->head + 1) % QUEUE_CAP;
            q->count--;
            if (q->send_waiter) {
                tcb_t *w = q->send_waiter;
                q->send_waiter = 0;
                w->state = 0;
            }
            irq_restore(flags);
            return item;
        }
        q->recv_waiter = current;
        current->state = 1;
        irq_restore(flags);

        tcb_t *next = pick_next_ready();
        tcb_t *prev = current;
        current = next;
        switch_to(&prev->sp, next->sp);
    }
}