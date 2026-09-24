#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL

#define GICD_CTLR       (*(volatile unsigned int *)(GICD_BASE + 0x000))
#define GICD_ISENABLER0 (*(volatile unsigned int *)(GICD_BASE + 0x100))
#define GICD_ISENABLER(n) (*(volatile unsigned int *)(GICD_BASE + 0x100 + 4*((n)/32)))
#define GICD_IPRIORITYR(n) (*(volatile unsigned char *)(GICD_BASE + 0x400 + (n)))
#define GICD_ITARGETSR(n)  (*(volatile unsigned char *)(GICD_BASE + 0x800 + (n)))

#define GICC_CTLR (*(volatile unsigned int *)(GICC_BASE + 0x000))
#define GICC_PMR  (*(volatile unsigned int *)(GICC_BASE + 0x004))
#define GICC_IAR  (*(volatile unsigned int *)(GICC_BASE + 0x00C))
#define GICC_EOIR (*(volatile unsigned int *)(GICC_BASE + 0x010))

void gic_init(void) {
    GICD_CTLR = 1;          /* enable distributor, group 0 (secure/simple mode as QEMU virt exposes) */
    GICC_PMR  = 0xFF;       /* priority mask: allow all priorities through */
    GICC_CTLR = 1;          /* enable CPU interface */
}

/* Enable an interrupt and (for SPIs) choose which CPU(s) it is routed to.
   Works for both PPIs/SGIs (id<32, via ISENABLER0 as before) and SPIs
   (id>=32, e.g. dma-accel's legacy INTx on SPI 37) - the enable-set
   register repeats every 32 IDs.

   target_mask is the GICv2 ITARGETSR byte: bit n = CPU n (0x01=CPU0,
   0x02=CPU1, 0x04=CPU2, 0x08=CPU3). Only meaningful for id>=32
   (ITARGETSR for PPIs/SGIs is banked per-CPU and read-only). Callers
   should pass a single bit for a CPU that is actually online; multi-bit
   masks are NOT relied on (QEMU virt's multi-target behaviour is
   unverified). The target is written BEFORE the enable so the interrupt
   can never become deliverable to a stale target. */
void gic_enable_irq_target(unsigned int id, unsigned char target_mask) {
    if (id >= 32) {
        GICD_ITARGETSR(id) = target_mask;
    }
    GICD_IPRIORITYR(id) = 0x80;
    GICD_ISENABLER(id) = (1U << (id % 32));
}

/* Default routing: CPU0. Behaviour-identical to the pre-change function,
   so existing call sites (timer PPI, DMA SPI) need no edits. */
void gic_enable_irq(unsigned int id) {
    gic_enable_irq_target(id, 0x01);
}

/* Read back the current routing byte of an SPI (id>=32). */
unsigned int gic_get_target(unsigned int id) {
    return GICD_ITARGETSR(id);
}

/* Experiment helper: write `mask` into ITARGETSR(id), read it back, then
   restore the old value. Does NOT enable the interrupt. Use it on an
   SPI nothing uses (id>=32) to learn what QEMU actually stores for a
   given mask (e.g. whether bits for CPUs that are not online are cleared). */
unsigned int gic_probe_target(unsigned int id, unsigned char mask) {
    unsigned char old = GICD_ITARGETSR(id);
    unsigned int rb;
    GICD_ITARGETSR(id) = mask;
    rb = GICD_ITARGETSR(id);
    GICD_ITARGETSR(id) = old;
    return rb;
}

unsigned int gic_ack(void) {
    return GICC_IAR;
}

void gic_eoi(unsigned int id) {
    GICC_EOIR = id;
}

/* GICC (CPU interface) registers are banked per CPU: every secondary core
   must set up its own. The distributor is enabled once, by CPU0. */
void gic_init_secondary(void) {
    GICC_PMR  = 0xFF;
    GICC_CTLR = 1;
}
