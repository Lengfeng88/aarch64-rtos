/* M8: multi-core bring-up. Secondary CPUs are started through PSCI, record
 * themselves, and park with IRQ masked. Only CPU0 prints. Nothing here
 * touches the scheduler or the GIC. */
#define MAX_CPUS       4
#define CPU_STACK_SIZE 4096

extern int  psci_cpu_on(unsigned long target_cpu_mpidr, unsigned long entry_point_pa);
extern void secondary_start(void);
extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern void klog(const char *prefix, long val, int has_val, const char *suffix);

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

/* Runs on CPUs 1..3, entered from boot.S with x0 = cpu id. */
void secondary_main(unsigned long cpu_id) {
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    if (cpu_id >= MAX_CPUS) { for (;;) __asm__ volatile("wfe"); }

    unsigned long mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("msr vbar_el1, %0\n\tisb" :: "r"(boot_vbar) : "memory");

    cpu_mpidr[cpu_id] = mpidr;
    __asm__ volatile("dsb sy" ::: "memory");
    cpu_alive[cpu_id] = 1;
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");

    for (;;) __asm__ volatile("wfe");
}

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
            uart_puts("\r\n");
        } else {
            klog("CPU", (long)cpu, 1, ": did NOT come up (timeout)\r\n");
        }
    }
    klog("CPUs online: ", (long)online, 1, "\r\n");
}
