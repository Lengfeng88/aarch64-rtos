/* M8: multi-core bring-up. Secondary CPUs are started through PSCI, record
 * themselves, and park with IRQ masked. Only CPU0 prints. Nothing here
 * touches the scheduler or the GIC. */
#include "percpu.h"
#include "sync.h"
cpu_local_t cpu_locals[MAX_CPUS];
static volatile unsigned long cpu_seen_id[MAX_CPUS];
#define CPU_STACK_SIZE 4096

extern int  psci_cpu_on(unsigned long target_cpu_mpidr, unsigned long entry_point_pa);
extern void secondary_start(void);
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void klog(const char *prefix, long val, int has_val, const char *suffix);
extern void gic_init_secondary(void);
extern void sched_register_on(unsigned long cpu, tcb_t *t);

/* One idle task per core: it stands for the boot context the core is already
   running on, so it needs no stack of its own. */
static tcb_t idle_tcb[MAX_CPUS];

extern void task_init(tcb_t *t, void (*entry)(void));

/* Step C: two independent, pinned CPU-bound test tasks per secondary core.
   They share nothing and print nothing; CPU0 reports their progress. */
static tcb_t test_tcb[MAX_CPUS][2];
static volatile unsigned long test_progress[MAX_CPUS][2];
static void test_task_a(void) { unsigned long c = this_cpu()->cpu_id; for (;;) test_progress[c][0]++; }
static void test_task_b(void) { unsigned long c = this_cpu()->cpu_id; for (;;) test_progress[c][1]++; }

/* Step D1: cross-CPU semaphore ping-pong. The ping task lives on CPU1, the pong
   task on CPU2 (needs -smp 3 or more); every wake-up crosses CPUs. */
#define PP_ROUNDS 2000UL
static sem_t pp_to_pong, pp_to_ping;            /* zero-initialised = count 0, unlocked */
static tcb_t pp_tcb[2];
static volatile unsigned long pp_rounds_done, pp_pongs;
static void pp_ping_task(void) {
    for (unsigned long i = 0; i < PP_ROUNDS; i++) {
        sem_post(&pp_to_pong);
        sem_wait(&pp_to_ping);
        pp_rounds_done = i + 1;
    }
    for (;;) __asm__ volatile("wfe");
}
static void pp_pong_task(void) {
    for (;;) {
        sem_wait(&pp_to_pong);
        pp_pongs++;
        sem_post(&pp_to_ping);
    }
}
extern void gic_enable_irq(unsigned int id);

/* boot.S computes each CPU's stack top as cpu_stacks + (id + 1) * 4096. */
unsigned char cpu_stacks[MAX_CPUS][CPU_STACK_SIZE] __attribute__((aligned(16)));
_Static_assert(sizeof(cpu_stacks[0]) == 4096, "boot.S assumes 4 KiB per-CPU stacks (lsl #12)");

static volatile unsigned char cpu_alive[MAX_CPUS];
static volatile unsigned long cpu_mpidr[MAX_CPUS];
static volatile unsigned long boot_vbar;

static void put_hex(unsigned long v) {
    static const char d[] = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) uart_putc(d[(v >> i) & 0xf]);
}

#ifdef SMP_SELFTEST
#include "spinlock.h"
#define SELFTEST_N 200000UL
static volatile unsigned long st_go;
static volatile unsigned long st_done[MAX_CPUS];
static volatile unsigned long st_plain, st_locked;
static spinlock_t st_lock = SPINLOCK_INIT;

/* Runs with IRQ masked on every participating core. */
static void selftest_body(unsigned long cpu) {
    for (unsigned long i = 0; i < SELFTEST_N; i++) st_plain++;      /* unprotected */
    for (unsigned long i = 0; i < SELFTEST_N; i++) {                /* protected   */
        spin_lock(&st_lock);
        st_locked++;
        spin_unlock(&st_lock);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    st_done[cpu] = 1;
}
#endif

/* Runs on CPUs 1..3, entered from boot.S with x0 = cpu id. */
void secondary_main(unsigned long cpu_id) {
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    if (cpu_id >= MAX_CPUS) { for (;;) __asm__ volatile("wfe"); }
    percpu_init(cpu_id);
    cpu_seen_id[cpu_id] = this_cpu()->cpu_id;

    unsigned long mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(boot_vbar) : "memory");

    cpu_mpidr[cpu_id] = mpidr;
    __asm__ volatile("dsb sy" ::: "memory");
    cpu_alive[cpu_id] = 1;
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");

#ifdef SMP_SELFTEST
    while (!st_go) { }
    selftest_body(cpu_id);
#endif
    /* M11 step B: this core's own run queue, holding only its idle task. */
    idle_tcb[cpu_id].name = "idle";
    idle_tcb[cpu_id].state = 0;
    current = &idle_tcb[cpu_id];
    sched_register_on(cpu_id, &idle_tcb[cpu_id]);
    for (int k = 0; k < 2; k++) {
        tcb_t *t = &test_tcb[cpu_id][k];
        t->name = "sectest";
        task_init(t, k == 0 ? test_task_a : test_task_b);
        sched_register_on(cpu_id, t);
    }
    if (cpu_id == 1) {
        pp_tcb[0].name = "ping";
        task_init(&pp_tcb[0], pp_ping_task);
        sched_register_on(cpu_id, &pp_tcb[0]);
    } else if (cpu_id == 2) {
        pp_tcb[1].name = "pong";
        task_init(&pp_tcb[1], pp_pong_task);
        sched_register_on(cpu_id, &pp_tcb[1]);
    }

    /* M9 step 2: this core's own GIC interface + physical timer. Its tick only
     * counts (secondary_irq in main.c); it never touches scheduler state. */
    gic_init_secondary();
    gic_enable_irq(30);
    {
        unsigned long f;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
        __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(f / 2000));
        __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(1UL));
    }
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    for (;;) __asm__ volatile("wfi");
}

/* Number of CPUs that came up (including CPU0); kernel_main uses it for task placement. */
int smp_online = 1;

/* Runs on CPU0, IRQ still masked, before the first task is started. */
void smp_boot_secondaries(void) {
    unsigned long mpidr, vbar;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
    boot_vbar = vbar;
    cpu_mpidr[0] = mpidr;
    cpu_alive[0] = 1;
    __asm__ volatile("dsb sy" ::: "memory");

    int online = 1;
    for (unsigned long cpu = 1; cpu < MAX_CPUS; cpu++) {
        int rc = psci_cpu_on(cpu, (unsigned long)&secondary_start);
        klog("CPU0: psci_cpu_on target=", (long)cpu, 1, rc < 0 ? " rc=-" : " rc=");
        klog("", (long)(rc < 0 ? -rc : rc), 1, "\r\n");
        if (rc != 0) continue;

        unsigned long spins = 0;
        while (!cpu_alive[cpu] && ++spins < 50000000UL) { }
        if (cpu_alive[cpu]) {
            online++;
            klog("CPU", (long)cpu, 1, ": alive, MPIDR_EL1=");
            put_hex(cpu_mpidr[cpu]);
            klog(" this_cpu()->cpu_id=", (long)cpu_seen_id[cpu], 1, "\r\n");
        } else {
            klog("CPU", (long)cpu, 1, ": did NOT come up (timeout)\r\n");
        }
    }
    klog("CPUs online: ", (long)online, 1, "\r\n");
    smp_online = online;

#ifdef SMP_SELFTEST
    st_go = 1;
    __asm__ volatile("dsb sy" ::: "memory");
    selftest_body(0);
    for (unsigned long c = 1; c < MAX_CPUS; c++)
        if (cpu_alive[c]) while (!st_done[c]) { }
    {
        unsigned long expected = (unsigned long)online * SELFTEST_N;
        klog("SELFTEST cpus=", (long)online, 1, "");
        klog(" expected=", (long)expected, 1, "");
        klog(" plain=", (long)st_plain, 1, "");
        klog(" locked=", (long)st_locked, 1, "");
        klog(" LOCK_OK=", (long)(st_locked == expected), 1, "\r\n");
    }
#endif
}

/* CPU0 only. Rate-limited: one line every 64 calls. */
void smp_report_irq_counts(void) {
    static unsigned long calls;
    if (++calls % 64) return;
    klog("IRQ counts: cpu0=", (long)cpu_locals[0].irq_count, 1, "");
    klog(" cpu1=", (long)cpu_locals[1].irq_count, 1, "");
    klog(" cpu2=", (long)cpu_locals[2].irq_count, 1, "");
    klog(" cpu3=", (long)cpu_locals[3].irq_count, 1, "\r\n");
    {
        unsigned long calls = 0, bad = 0;
        for (unsigned long c = 1; c < MAX_CPUS; c++) {
            calls += cpu_locals[c].sched_calls;
            bad   += cpu_locals[c].unexpected_switch;
        }
        klog("SCHED secondaries: calls=", (long)calls, 1, "");
        klog(" unexpected=", (long)bad, 1, "\r\n");
    }
    {
        unsigned long sw = 0;
        for (unsigned long c = 1; c < MAX_CPUS; c++) sw += cpu_locals[c].context_switches;
        klog("SECTEST switches=", (long)sw, 1, "");
        for (unsigned long c = 1; c < MAX_CPUS; c++) {
            klog(" cpu", (long)c, 1, "=");
            klog("", (long)test_progress[c][0], 1, ",");
            klog("", (long)test_progress[c][1], 1, "");
        }
        klog("", 0, 0, "\r\n");
    }
    klog("PINGPONG rounds=", (long)pp_rounds_done, 1, "");
    klog(" pongs=", (long)pp_pongs, 1, "\r\n");
}
