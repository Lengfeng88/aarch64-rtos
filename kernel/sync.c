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
extern void report_corrupt_sp(const char *where, unsigned long sp);

static inline unsigned long irq_disable_save(void) {
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    __asm__ volatile("msr daifset, #2");
    return flags;
}

static inline void irq_restore(unsigned long flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags));
}

/* WINDOW-3 FIX (same as sched.c's checked_switch_to - see that file's
   comment for the full rationale): mask IRQ around the post-switch
   bounds check itself, not just the switch_to() call. */
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

void sem_init(sem_t *s, int initial_count) {
    s->count = initial_count;
    s->waiter = 0;
}

/* Back to its original, pre-window-1 form. */
void sem_wait(sem_t *s) {
    unsigned long flags = irq_disable_save();
    if (s->count > 0) {
        s->count--;
        irq_restore(flags);
        return;
    }
    s->waiter = current;
    current->state = 1;
    irq_restore(flags);

    tcb_t *prev = current;
    tcb_t *next = pick_next_ready();
    if (next == prev) {
        while (current->state != 0) {
            __asm__ volatile("wfe");
        }
        return;
    }
    current = next;
    checked_switch_to(prev, next, "CORRUPT sp after sem_wait() switch_to, sp=");
    /* Resumes here once sem_post() marks us READY again and the
       scheduler (preemptive or another voluntary yield) switches back. */
}

void sem_post(sem_t *s) {
    unsigned long flags = irq_disable_save();
    if (s->waiter) {
        tcb_t *w = s->waiter;
        s->waiter = 0;
        w->state = 0;
    } else {
        s->count++;
    }
    irq_restore(flags);
}

typedef struct {
    volatile int locked;
    tcb_t *owner;
    tcb_t *waiter;
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
    checked_switch_to(prev, next, "CORRUPT sp after mutex_lock() switch_to, sp=");
}

void mutex_unlock(mutex_t *m) {
    unsigned long flags = irq_disable_save();
    if (m->waiter) {
        tcb_t *w = m->waiter;
        m->waiter = 0;
        m->owner = w;
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
    int wait_all;
    unsigned int wake_result;
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
        e->wake_result = e->flags & e->wait_mask;
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
    checked_switch_to(prev, next, "CORRUPT sp after event_wait() switch_to, sp=");
    return e->wake_result;
}

#define QUEUE_CAP 4

typedef struct {
    unsigned long buf[QUEUE_CAP];
    int head, tail, count;
    tcb_t *send_waiter;
    tcb_t *recv_waiter;
} queue_t;

void queue_init(queue_t *q) {
    q->head = q->tail = q->count = 0;
    q->send_waiter = 0;
    q->recv_waiter = 0;
}

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
        checked_switch_to(prev, next, "CORRUPT sp after queue_send() switch_to, sp=");
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
        checked_switch_to(prev, next, "CORRUPT sp after queue_recv() switch_to, sp=");
    }
}