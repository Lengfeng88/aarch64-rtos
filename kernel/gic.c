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

/* Works for both PPIs/SGIs (id<32, via ISENABLER0 as before) and SPIs
   (id>=32, e.g. dma-accel's legacy INTx on SPI 37) - the enable-set
   register repeats every 32 IDs, and SPIs additionally need a target
   CPU set via ITARGETSR (byte-per-interrupt; GICD_ITARGETSR for
   PPIs/SGIs is banked per-CPU and read-only, so only touch it for
   id>=32, where it's genuinely writable). */
void gic_enable_irq(unsigned int id) {
    GICD_ISENABLER(id) = (1U << (id % 32));
    GICD_IPRIORITYR(id) = 0x80;
    if (id >= 32) {
        GICD_ITARGETSR(id) = 0x01;   /* route to CPU0 - the only CPU we have */
    }
}

unsigned int gic_ack(void) {
    return GICC_IAR;
}

void gic_eoi(unsigned int id) {
    GICC_EOIR = id;
}
