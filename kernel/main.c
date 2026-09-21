extern void uart_puts(const char *s);
extern void vectors(void);
extern void gic_init(void);
extern void gic_enable_irq(unsigned int id);
extern unsigned int gic_ack(void);
extern void gic_eoi(unsigned int id);
extern unsigned int sched_debug_task_ewma(int i);
extern void sched_on_tick(void);
extern unsigned long sched_debug_select_count(int i);


#define TIMER_IRQ_ID 30
#define DMA_ACCEL_IRQ_ID 37
#define IRQ_DMA_DONE (1u << 0)
#define DMA_ACCEL_OK 0x00
#define NUM_WORKERS 3

#include "tcb.h"
#include "percpu.h"

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
extern int psci_cpu_on(unsigned long target_cpu_mpidr, unsigned long entry_point_pa);
extern void secondary_start(void);
extern void smp_boot_secondaries(void);
extern void smp_report_irq_counts(void);

/* Moved up from further down in the file so sync_exception_handler_full
   (which needs to inspect these for crash diagnostics) can see them -
   they're still defined exactly once, just earlier. */
static tcb_t taskWorker[NUM_WORKERS];
static tcb_t busy_task;

typedef struct {
    unsigned long cmd_id;
    sem_t *sem;
    dma_accel_completion_t result;
    int used;
    int done;
} pending_req_t;

static pending_req_t pending[NUM_WORKERS];

static void print_decline(const char *prefix, unsigned long v);
static int worker_id_of(tcb_t *t);

#define SWITCH_HIST_LEN 16
typedef struct {
    const char *site;
    int prev_id;   /* -1 = busy_task, -2 = unknown */
    int next_id;
} switch_hist_t;
static switch_hist_t switch_hist[SWITCH_HIST_LEN];
static int switch_hist_idx = 0;

void record_switch(const char *site, void *prev_p, void *next_p) {
    int pid = (prev_p == &busy_task) ? -1 : worker_id_of((tcb_t *)prev_p);
    int nid = (next_p == &busy_task) ? -1 : worker_id_of((tcb_t *)next_p);
    switch_hist[switch_hist_idx].site = site;
    switch_hist[switch_hist_idx].prev_id = pid;
    switch_hist[switch_hist_idx].next_id = nid;
    switch_hist_idx = (switch_hist_idx + 1) % SWITCH_HIST_LEN;
}

static void print_hexline(const char *prefix, unsigned long v);
void dump_switch_hist(void);

void check_sp_write(unsigned long *ptr, unsigned long value) {
    if (ptr == &taskWorker[1].sp) {
        unsigned long lo = (unsigned long)&taskWorker[1].stack[0];
        unsigned long hi = lo + STACK_WORDS * sizeof(unsigned long);
        if (value < lo || value >= hi) {
            uart_puts("CAUGHT WRITE: switch_to about to write BAD value into taskWorker[1].sp\r\n");
            print_hexline("  bad value about to be written=", value);
            dump_switch_hist();
            while (1) { __asm__ volatile("wfe"); }
        }
    }
}

void report_pre_switch_bad(const char *where, void *next_p, unsigned long bad_sp) {
    int nid = (next_p == (void *)&busy_task) ? -1 : worker_id_of((tcb_t *)next_p);
    uart_puts("PRE-SWITCH BAD: next->sp already invalid BEFORE switch_to, site=");
    uart_puts(where);
    print_decline("  next_id=", (unsigned long)(long)nid);
    print_hexline("  bad next->sp=", bad_sp);
    dump_switch_hist();
    while (1) { __asm__ volatile("wfe"); }
}

void dump_switch_hist(void) {
    uart_puts("CORRUPT sp: recent switch history (oldest first)\r\n");
    for (int i = 0; i < SWITCH_HIST_LEN; i++) {
        int idx = (switch_hist_idx + i) % SWITCH_HIST_LEN;
        if (!switch_hist[idx].site) continue;
        uart_puts(" site=");
        uart_puts(switch_hist[idx].site);
        print_decline("  prev_id=", (unsigned long)(long)switch_hist[idx].prev_id);
        print_decline("  next_id=", (unsigned long)(long)switch_hist[idx].next_id);
    }
}

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
    
    print_decline("P4 M7: current task = ", (unsigned long)(current == 0 ? 98 : current == &taskWorker[0] ? 0 : current == &taskWorker[1] ? 1 : current == &taskWorker[2] ? 2 : current == &busy_task ? 3 : 99));
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

static int worker_id_of(tcb_t *t);

/* Secondary cores: count the tick, re-arm this core's timer, EOI. No scheduler
   state (all_tasks/current_idx/...) may be touched from here yet. */
static void secondary_irq(unsigned int id) {
    if (id >= 1020) return;            /* spurious: nothing to EOI */
    if (id == TIMER_IRQ_ID) {
        this_cpu()->irq_count++;
        timer_rearm(tick_freq / 2000);
    }
    gic_eoi(id);
}

void irq_handler(void) {
    unsigned int id = gic_ack();
    if (this_cpu()->cpu_id != 0) { secondary_irq(id); return; }

    if (id == TIMER_IRQ_ID) {
        this_cpu()->irq_count++;
#ifdef DEBUG_HOOKS
        /* Full sweep, every tick, regardless of whether a switch is
           about to happen - to catch the exact tick where some task's
           sp flips from valid to invalid while it's NOT the one being
           switched in/out. */
        /* Ring log of worker2's own sp value at every single tick,
           regardless of whether it looks valid yet - so once it DOES
           go bad, we can see the exact last-known-good value and the
           first-bad value side by side, and cross-reference against
           the switch history for what ran in between. */
        static unsigned long w_sp_log[NUM_WORKERS][SWITCH_HIST_LEN];
        static int w_sp_log_idx[NUM_WORKERS];
        for (int wi = 0; wi < NUM_WORKERS; wi++) {
            w_sp_log[wi][w_sp_log_idx[wi]] = taskWorker[wi].sp;
            w_sp_log_idx[wi] = (w_sp_log_idx[wi] + 1) % SWITCH_HIST_LEN;
        }

        for (int wi = 0; wi < NUM_WORKERS; wi++) {
            unsigned long wlo = (unsigned long)&taskWorker[wi].stack[0];
            unsigned long whi = wlo + STACK_WORDS * sizeof(unsigned long);
            unsigned long s = taskWorker[wi].sp;
            if (s != 0 && (s < wlo || s >= whi)) {
                uart_puts("W SP LOG for the bad worker (oldest first):\r\n");
                for (int li = 0; li < SWITCH_HIST_LEN; li++) {
                    int idx = (w_sp_log_idx[wi] + li) % SWITCH_HIST_LEN;
                    print_hexline("  ", w_sp_log[wi][idx]);
                }
                uart_puts("TICK-SWEEP: worker sp went bad while NOT being switched, id=");
                char idbuf[4]; idbuf[0] = '0' + wi; idbuf[1] = '\r'; idbuf[2] = '\n'; idbuf[3] = 0;
                uart_puts(idbuf);
                print_hexline("  bad sp=", s);
                dump_switch_hist();
                while (1) { __asm__ volatile("wfe"); }
            }
        }

#endif
        timer_rearm(tick_freq / 2000);
        sched_on_tick(); 
        tcb_t *next = pick_next_ready();
        gic_eoi(id);
        if (next != current) {
            tcb_t *prev = current;
            current = next;
#ifdef DEBUG_HOOKS
            record_switch("irq_handler", prev, next);
            {
                unsigned long nlo = (unsigned long)&next->stack[0];
                unsigned long nhi = nlo + STACK_WORDS * sizeof(unsigned long);
                if (next->sp < nlo || next->sp >= nhi) {
                    report_pre_switch_bad("irq_handler", next, next->sp);
                }
            }
#endif
            switch_to(&prev->sp, next->sp);
#ifdef DEBUG_HOOKS
            unsigned long lo = (unsigned long)&prev->stack[0];
            unsigned long hi = lo + STACK_WORDS * sizeof(unsigned long);
            if (prev->sp < lo || prev->sp >= hi) {
                unsigned long mpidr;
                __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
                print_hexline("CORRUPT sp after irq_handler switch_to, sp=", prev->sp);
                print_hexline("CORRUPT sp: MPIDR_EL1 of offending core=", mpidr);
                print_decline("CORRUPT sp: prev worker id=", (unsigned long)worker_id_of(prev));

                unsigned long canary_lo = prev->stack[0];
                unsigned long canary_hi = prev->stack[STACK_WORDS - 1];
                print_hexline("CORRUPT sp: prev->stack[0] (low canary)=", canary_lo);
                print_hexline("CORRUPT sp: prev->stack[top] (high canary)=", canary_hi);
                print_decline("CORRUPT sp: low canary intact (1=yes)=",
                              (unsigned long)(canary_lo == 0xC0FFEEDEADBEEFULL));
                print_decline("CORRUPT sp: high canary intact (1=yes)=",
                              (unsigned long)(canary_hi == 0xC0FFEEDEADBEEFULL));

                /* Does the bad sp value actually fall inside some OTHER
                   task's own valid stack range? If so, this isn't
                   corruption at all - it's a prev/next mixup: prev->sp
                   ended up holding a different task's legitimate sp. */
                for (int wi = 0; wi < NUM_WORKERS; wi++) {
                    unsigned long wlo = (unsigned long)&taskWorker[wi].stack[0];
                    unsigned long whi = wlo + STACK_WORDS * sizeof(unsigned long);
                    if (prev->sp >= wlo && prev->sp < whi) {
                        print_decline("CORRUPT sp: MATCH worker=", (unsigned long)wi);
                    }
                }
                print_decline("CORRUPT sp: next worker id=", (unsigned long)worker_id_of(next));
                unsigned long blo = (unsigned long)&busy_task.stack[0];
                unsigned long bhi = blo + STACK_WORDS * sizeof(unsigned long);
                if (prev->sp >= blo && prev->sp < bhi) {
                    uart_puts("CORRUPT sp: matches busy_task's valid range\r\n");
                }
                print_hexline("CORRUPT sp: next->sp for comparison=", next->sp);
                print_hexline("CORRUPT sp: &prev->stack[0]=", (unsigned long)&prev->stack[0]);
                print_hexline("CORRUPT sp: &next->stack[0]=", (unsigned long)&next->stack[0]);
                dump_switch_hist();

                while (1) { __asm__ volatile("wfe"); }
            }

#endif
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
    if (id < 0) {
        uart_puts("FATAL: worker_id_of(current) returned -1 in worker_entry!\r\n");
        print_hexline("  current ptr=", (unsigned long)current);
        while (1) { __asm__ volatile("wfe"); }
    }
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
#if DISABLE_REAL_DMA
    /* Control experiment: fake an instant completion, no real hardware
       DMA submitted at all, to test whether the device's DMA engine
       writing to RAM is what's corrupting tcb_t.sp. */
    static unsigned long fake_cmd_id_counter = 0;
    fake_cmd_id_counter++;
    unsigned long cmd_id = fake_cmd_id_counter;
    for (int i = 0; i < TEST_LEN; i++) dst_buf[id][i] = src_buf[id][i];
    pending_register(cmd_id, &worker_sem[id]);
    pending[id].result.cmd_id = cmd_id;
    pending[id].result.status = 0;
    pending[id].done = 1;
    sem_post(&worker_sem[id]);
#else
    unsigned long cmd_id = accel_submit_copy(src_phys, dst_phys, TEST_LEN);
    pending_register(cmd_id, &worker_sem[id]);
#endif
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
    print_decline("WORKER ewma=", (unsigned long)current->ewma_load);

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

static void busy_task_entry(void) {
    unsigned long counter = 0;
    while (1) {
        counter++;
        if ((counter & 0xFFFFF) == 0) {
            print_decline("BUSY TASK ewma=", (unsigned long)busy_task.ewma_load);
            smp_report_irq_counts();
            print_decline("SELECT busy=", sched_debug_select_count(3));   // busy_task是第4个注册的，index=3
            print_decline("SELECT w0=", sched_debug_select_count(0));
            print_decline("SELECT w1=", sched_debug_select_count(1));
            print_decline("SELECT w2=", sched_debug_select_count(2));
}
        /* 故意不yield() —— 只靠timer抢占它 */
    }
}

void secondary_entry_c(void) {
    unsigned long el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    el = (el >> 2) & 0x3;
    print_decline("CPU1: CurrentEL=", el);

    unsigned long daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    print_hexline("CPU1: DAIF on entry=", daif);

    uart_puts("CPU1 alive\r\n");
    while (1) { __asm__ volatile("wfe"); }
}

void kernel_main(unsigned long boot_path) {
    percpu_init(0);
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

    busy_task.name = "busy";
    task_init(&busy_task, busy_task_entry); 

    current = &taskWorker[0];
    for (int i = 0; i < NUM_WORKERS; i++) {
        sched_register(&taskWorker[i]);
    }

    sched_register(&busy_task); 

    gic_init();
    gic_enable_irq(TIMER_IRQ_ID);
    gic_enable_irq(DMA_ACCEL_IRQ_ID);
    timer_rearm(tick_freq / 2000);

    unsigned long mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    print_hexline("CPU0: MPIDR_EL1=", mpidr);

    smp_boot_secondaries();

    unsigned long dummy_sp;
    switch_to(&dummy_sp, taskWorker[0].sp);

    uart_puts("SHOULD NEVER PRINT\r\n");
    while (1) { __asm__ volatile("wfe"); }
}
