extern void uart_puts(const char *s);
extern void vectors(void);
extern void gic_init(void);
extern void gic_enable_irq(unsigned int id);
extern unsigned int gic_ack(void);
extern void gic_eoi(unsigned int id);

#define STACK_WORDS 512
#define TIMER_IRQ_ID 30
#define DMA_ACCEL_IRQ_ID 37
#define IRQ_DMA_DONE (1u << 0)
#define DMA_ACCEL_OK 0x00
#define NUM_WORKERS 3

typedef struct {
    unsigned long sp;
    unsigned long stack[STACK_WORDS];
    const char *name;
    void (*entry)(void);
    int state;
} tcb_t;

typedef struct {
    volatile int count;
    tcb_t *waiter;
} sem_t;

typedef struct {
    unsigned int bus, dev, func;
    unsigned long bar0_phys;
    unsigned int bar0_size;
    unsigned int bar0_readback;
    unsigned int gic_spi;
    int found;
} pci_dev_t;

typedef struct {
    unsigned long cmd_id;
    unsigned int status;
    unsigned int reserved;
} __attribute__((packed)) dma_accel_completion_t;

extern void task_init(tcb_t *t, void (*entry)(void));
extern void switch_to(unsigned long *old_sp_ptr, unsigned long new_sp);
extern void sched_register(tcb_t *t);
extern tcb_t *pick_next_ready(void);
extern void yield(void);
extern void sem_init(sem_t *s, int initial_count);
extern void sem_wait(sem_t *s);
extern void sem_post(sem_t *s);

extern pci_dev_t pci_find_dma_accel(void);
extern void accel_init(unsigned long bar0_phys);
extern void accel_setup_queues(void);
extern unsigned long accel_submit_copy(unsigned long src, unsigned long dst, unsigned int len);
extern int accel_drain_completions_multi(dma_accel_completion_t *out, int max_out);
extern unsigned int accel_irq_status(void);
extern void accel_irq_ack(unsigned int bits);
extern unsigned int accel_debug_cq_tail(void);
extern unsigned int accel_debug_cq_head_local(void);
extern int sched_debug_current_idx(void);
extern int sched_debug_num_tasks(void);
extern int sched_debug_task_state(int i);
extern void *sched_debug_task_ptr(int i);

/* Moved up from further down in the file so sync_exception_handler_full
   (which needs to inspect these for crash diagnostics) can see them -
   they're still defined exactly once, just earlier. */
static tcb_t taskWorker[NUM_WORKERS];
tcb_t *current;

typedef struct {
    unsigned long cmd_id;
    sem_t *sem;
    dma_accel_completion_t result;
    int used;
    int done;
} pending_req_t;

static pending_req_t pending[NUM_WORKERS];

static void print_hex_into(char *out, unsigned long v) {
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = hex[(v >> ((15 - i) * 4)) & 0xf];
    out[16] = 0;
}
static void print_dec_into(char *out, unsigned long v) {
    char tmp[21]; int i = 20; tmp[i] = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v > 0) { tmp[--i] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (tmp[i]) out[j++] = tmp[i++];
    out[j] = 0;
}
static void print_hexline(const char *prefix, unsigned long v) {
    char line[64]; int i = 0;
    while (prefix[i] && i < 40) { line[i] = prefix[i]; i++; }
    line[i++] = '0'; line[i++] = 'x';
    print_hex_into(&line[i], v); i += 16;
    line[i++] = '\r'; line[i++] = '\n'; line[i] = 0;
    uart_puts(line);
}
static void print_decline(const char *prefix, unsigned long v) {
    char line[64]; int i = 0;
    while (prefix[i] && i < 40) { line[i] = prefix[i]; i++; }
    char numbuf[21];
    print_dec_into(numbuf, v);
    int j = 0;
    while (numbuf[j]) line[i++] = numbuf[j++];
    line[i++] = '\r'; line[i++] = '\n'; line[i] = 0;
    uart_puts(line);
}

/* Called from sched.c/sync.c's checked_switch_to() when a task's sp,
   right after switch_to() returns, is found NOT to fall within that
   task's own stack[] array - i.e. the corruption we're hunting
   actually happened during THIS specific switch_to() call, at THIS
   specific call site. Prints where + the bad value and halts, so the
   very first hit pins down both the call site and the bad value
   without any further guessing. */
void report_corrupt_sp(const char *where, unsigned long sp) {
    print_hexline(where, sp);
    while (1) { __asm__ volatile("wfe"); }
}
void trace_switch_resume(unsigned long *frame) {
    unsigned long x30_slot = frame[3];   // offset 24 / 8 = index 3
    /* Cheap check first, before any UART output: valid code/data
       addresses in this kernel are all >= 0x40000000. Anything below
       that (like the 0x3c0 we've been chasing) is suspicious and
       worth the (expensive) print. Skip silently otherwise - this
       keeps the overhead near zero on the many thousands of routine,
       healthy switches. */
    if (x30_slot >= 0x40000000UL) {
        return;
    }
    print_hexline("SUSPICIOUS x30 slot, frame=", (unsigned long)frame);
    print_hexline(" x30_slot=", x30_slot);
    for (int i = 0; i < 14; i++) {
        print_hexline(" w=", frame[i]);
    }
}

/* Full register dump at fault time. regs[] layout, per vectors.S's
   sync_el1h: x0..x30 (31 regs, 8 bytes each = 248 bytes) then esr,elr
   (16 more bytes) = 264 bytes total, at the pointer passed in via x0. */
void sync_exception_handler_full(unsigned long *regs) {
    unsigned long esr = regs[31];
    unsigned long elr = regs[32];
    unsigned long orig_sp = regs[33];   // NEW - the offset-264 slot vectors.S now fills in
    print_hexline("SYNC EXCEPTION ESR=", esr);
    print_hexline("SYNC EXCEPTION ELR=", elr);
    print_hexline("SYNC EXCEPTION SP=", orig_sp);   // NEW

    // NEW: does this sp fall inside any worker's own stack array?
    int matched_worker = -1;
    for (int i = 0; i < NUM_WORKERS; i++) {
        unsigned long lo = (unsigned long)&taskWorker[i].stack[0];
        unsigned long hi = lo + STACK_WORDS * sizeof(unsigned long);
        if (orig_sp >= lo && orig_sp < hi) {
            matched_worker = i;
            break;
        }
    }
    print_decline("SYNC EXCEPTION SP belongs to worker (-1=none)=", (unsigned long)matched_worker);

    const char *names[31] = {
        "x0","x1","x2","x3","x4","x5","x6","x7","x8","x9",
        "x10","x11","x12","x13","x14","x15","x16","x17","x18","x19",
        "x20","x21","x22","x23","x24","x25","x26","x27","x28","x29","x30"
    };
    for (int i = 0; i < 31; i++) {
            char label[8];
            int li = 0;
            label[li++] = ' ';
            label[li++] = names[i][0];
            if (names[i][1]) label[li++] = names[i][1];
            if (names[i][2]) label[li++] = names[i][2];
            label[li] = 0;
            print_hexline(label, regs[i]);
    }
    
    print_decline("P4 M7: current task = ", (unsigned long)(current == &taskWorker[0] ? 0 : current == &taskWorker[1] ? 1 : current == &taskWorker[2] ? 2 : 99));
    for (int i = 0; i < NUM_WORKERS; i++) {
        print_decline("P4 M7: pending[i].used=", (unsigned long)pending[i].used);
        print_decline("P4 M7: pending[i].done=", (unsigned long)pending[i].done);
        print_decline("P4 M7: pending[i].cmd_id=", pending[i].cmd_id);
        print_hexline("P4 M7: pending[i].sem ptr=", (unsigned long)pending[i].sem);
    }
    while (1) { __asm__ volatile("wfe"); }


}

static void set_vbar(void) {
    unsigned long v = (unsigned long)&vectors;
    __asm__ volatile("msr vbar_el1, %0" :: "r"(v));
}
static inline unsigned long read_cntfrq(void) {
    unsigned long v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
static inline void timer_rearm(unsigned long ticks) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(ticks));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(1UL));
}
static inline unsigned long irq_disable_save(void) {
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    __asm__ volatile("msr daifset, #2");
    return flags;
}
static inline void irq_restore(unsigned long flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags));
}

static unsigned long tick_freq;

static void pending_register(unsigned long cmd_id, sem_t *sem) {
    /* Same race as accel_submit_copy: without this, two workers could
       both see the same slot as free (checked-but-not-yet-claimed) if
       one gets preempted between the check and the write, and the
       second one's registration would silently clobber the first's -
       whose completion would then never find a match and it would
       hang forever waiting on a semaphore nobody posts. */
    unsigned long flags = irq_disable_save();
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (!pending[i].used) {
            pending[i].cmd_id = cmd_id;
            pending[i].sem = sem;
            pending[i].used = 1;
            pending[i].done = 0;
            break;
        }
    }
    irq_restore(flags);
}

static int worker_loops_seen[NUM_WORKERS];
static sem_t worker_sem[NUM_WORKERS];

#define TEST_LEN 128
static char src_buf[NUM_WORKERS][TEST_LEN] __attribute__((aligned(64)));
static char dst_buf[NUM_WORKERS][TEST_LEN] __attribute__((aligned(64)));

/* Shared between the real ISR and a periodic fallback poll: drains
   whatever completions are currently available and dispatches each to
   its registered waiter. Safe to call from either context since it
   only touches shared state that's already protected the same way
   (accel.c's own internals disable IRQ around the ring pointers, and
   pending[] here is only mutated under irq_disable in
   pending_register(); reading/matching it here from task context with
   IRQ disabled is equally safe). */
static void dispatch_available_completions(void) {
    unsigned long flags = irq_disable_save();
    dma_accel_completion_t comps[NUM_WORKERS];
    int n = accel_drain_completions_multi(comps, NUM_WORKERS);
    for (int c = 0; c < n; c++) {
        for (int i = 0; i < NUM_WORKERS; i++) {
            if (pending[i].used && !pending[i].done &&
                pending[i].cmd_id == comps[c].cmd_id) {
                pending[i].result = comps[c];
                pending[i].done = 1;
                sem_post(pending[i].sem);
                break;
            }
        }
    }
    irq_restore(flags);
}

void irq_handler(void) {
    unsigned int id = gic_ack();

    if (id == TIMER_IRQ_ID) {
        timer_rearm(tick_freq / 2000);
        tcb_t *next = pick_next_ready();
        gic_eoi(id);
        if (next != current) {
            tcb_t *prev = current;
            current = next;
            switch_to(&prev->sp, next->sp);
            unsigned long lo = (unsigned long)&prev->stack[0];
            unsigned long hi = lo + STACK_WORDS * sizeof(unsigned long);
            if (prev->sp < lo || prev->sp >= hi) {
                print_hexline("CORRUPT sp after irq_handler switch_to, sp=", prev->sp);
                while (1) { __asm__ volatile("wfe"); }
            }

        }
        return;
    }

    if (id == DMA_ACCEL_IRQ_ID) {
        unsigned int status = accel_irq_status();
        if (status & IRQ_DMA_DONE) {
            accel_irq_ack(IRQ_DMA_DONE);
            dispatch_available_completions();
        }
        gic_eoi(id);
        return;
    }

    gic_eoi(id);
}

static int worker_id_of(tcb_t *t) {
    for (int i = 0; i < NUM_WORKERS; i++) if (&taskWorker[i] == t) return i;
    return -1;
}

void worker_entry(void) {
    int id = worker_id_of(current);
    /* current can move between reading it and using it below only via
       preemption, which is fine here since 'id' is computed once and
       everything after uses the captured value, not 'current' again. */

    for (int i = 0; i < TEST_LEN; i++) {
        src_buf[id][i] = (char)((i + id * 17) ^ 0xA5);
        dst_buf[id][i] = 0;
    }

    unsigned long src_phys = (unsigned long)src_buf[id];
    unsigned long dst_phys = (unsigned long)dst_buf[id];

    sem_init(&worker_sem[id], 0);

    /* The gap between accel_submit_copy() ringing the doorbell and
       pending_register() recording who's waiting is NOT safe to leave
       open: if the device (or even just a preemption reordering us
       behind another worker) lets the completion IRQ land in that
       gap, the completion arrives with no pending[] entry to match,
       gets silently dropped, and this worker never wakes up. Wrap
       both calls in one critical section so the doorbell ring and the
       registration are atomic as a pair. */
    unsigned long flags = irq_disable_save();
    unsigned long cmd_id = accel_submit_copy(src_phys, dst_phys, TEST_LEN);
    pending_register(cmd_id, &worker_sem[id]);
    irq_restore(flags);

    print_decline("P4 M7: worker submitted cmd_id=", cmd_id);
    sem_wait(&worker_sem[id]);

    int slot = -1;
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (pending[i].used && pending[i].cmd_id == cmd_id) { slot = i; break; }
    }
    dma_accel_completion_t comp = pending[slot].result;

    if (comp.cmd_id != cmd_id || comp.status != DMA_ACCEL_OK) {
        uart_puts("P4 M7: !!! worker got wrong/bad completion !!!\r\n");
    }

    int data_ok = 1;
    for (int i = 0; i < TEST_LEN; i++) {
        if (dst_buf[id][i] != src_buf[id][i]) { data_ok = 0; break; }
    }
    print_decline(data_ok ? "P4 M7: worker DATA OK, cmd_id="
                          : "P4 M7: worker DATA MISMATCH, cmd_id=", cmd_id);

    worker_loops_seen[id] = 1;

    /* Don't just idle here hoping the timer eventually preempts its
       way to whichever workers are still suspended - same lesson as
       M5/M6's tail-stall fixes: the timer's own interrupt can silently
       stop being redelivered (the known GIC issue). Proactively yield
       until everyone's done, so forward progress is driven by
       voluntary scheduling, not a passive tick that might never come
       again. */
    int all_done;
    do {
        all_done = 1;
        for (int i = 0; i < NUM_WORKERS; i++) {
            if (!worker_loops_seen[i]) { all_done = 0; break; }
        }
        if (!all_done) {
            /* Fallback safety net for the known GIC redelivery flake
               (see the M5/M6 writeups): don't rely purely on the
               interrupt actually arriving. Directly poll for
               completions the ISR might have missed, in addition to
               yielding - either mechanism alone can miss a wakeup,
               together they can't both miss the same completion. */
            dispatch_available_completions();
            yield();
        }
    } while (!all_done);

    while (1) { __asm__ volatile("wfe"); }
}

void kernel_main(unsigned long boot_path) {
    (void)boot_path;
    set_vbar();
    uart_puts("P4 M7: full integration test boot OK\r\n");

    pci_dev_t d = pci_find_dma_accel();
    if (!d.found) {
        uart_puts("P4 M7: dma-accel NOT FOUND\r\n");
        while (1) { __asm__ volatile("wfe"); }
    }
    accel_init(d.bar0_phys);
    accel_setup_queues();

    tick_freq = read_cntfrq();

    for (int i = 0; i < NUM_WORKERS; i++) {
        taskWorker[i].name = "worker";
        task_init(&taskWorker[i], worker_entry);
    }
    current = &taskWorker[0];
    for (int i = 0; i < NUM_WORKERS; i++) {
        sched_register(&taskWorker[i]);
    }

    gic_init();
    gic_enable_irq(TIMER_IRQ_ID);
    gic_enable_irq(DMA_ACCEL_IRQ_ID);
    timer_rearm(tick_freq / 2000);

    unsigned long dummy_sp;
    switch_to(&dummy_sp, taskWorker[0].sp);

    uart_puts("SHOULD NEVER PRINT\r\n");
    while (1) { __asm__ volatile("wfe"); }
}
