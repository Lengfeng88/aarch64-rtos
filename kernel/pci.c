/* PCIe config space access via the generic ECAM host bridge on QEMU virt.
 * Verified against a real device-tree dump of this exact machine (not
 * assumed from generic docs):
 *   compatible = "pci-host-ecam-generic"
 *   reg = <0x40 0x10000000  0x00 0x10000000>   -> ECAM base 0x4010000000
 *   32-bit MMIO window (for BAR assignment): CPU addr 0x10000000
 */

#define PCIE_ECAM_BASE   0x4010000000UL
#define PCIE_MMIO32_BASE 0x10000000UL

#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_BAR0         0x10
#define PCI_INTERRUPT_PIN 0x3D

#define PCI_COMMAND_IO         (1u << 0)
#define PCI_COMMAND_MEMORY     (1u << 1)   /* NOT bit 0 - verified against
                                               QEMU's own linux/pci_regs.h
                                               after a long trace-driven
                                               debug session; bit 0 is
                                               actually PCI_COMMAND_IO */
#define PCI_COMMAND_BUS_MASTER (1u << 2)

static inline unsigned long ecam_addr(unsigned int bus, unsigned int dev,
                                       unsigned int func, unsigned int reg) {
    return PCIE_ECAM_BASE
         | ((unsigned long)bus  << 20)
         | ((unsigned long)dev  << 15)
         | ((unsigned long)func << 12)
         | reg;
}

static inline unsigned int pci_cfg_read32(unsigned int bus, unsigned int dev,
                                           unsigned int func, unsigned int reg) {
    return *(volatile unsigned int *)ecam_addr(bus, dev, func, reg);
}

static inline unsigned short pci_cfg_read16(unsigned int bus, unsigned int dev,
                                             unsigned int func, unsigned int reg) {
    return *(volatile unsigned short *)ecam_addr(bus, dev, func, reg);
}

static inline unsigned char pci_cfg_read8(unsigned int bus, unsigned int dev,
                                           unsigned int func, unsigned int reg) {
    return *(volatile unsigned char *)ecam_addr(bus, dev, func, reg);
}

static inline void pci_cfg_write32(unsigned int bus, unsigned int dev,
                                    unsigned int func, unsigned int reg,
                                    unsigned int val) {
    *(volatile unsigned int *)ecam_addr(bus, dev, func, reg) = val;
}

static inline void pci_cfg_write16(unsigned int bus, unsigned int dev,
                                    unsigned int func, unsigned int reg,
                                    unsigned short val) {
    *(volatile unsigned short *)ecam_addr(bus, dev, func, reg) = val;
}

typedef struct {
    unsigned int bus, dev, func;
    unsigned long bar0_phys;   /* CPU physical address BAR0 was assigned */
    unsigned int bar0_size;    /* determined via the standard PCI BAR-sizing
                                   trick (write all 1s, read back, invert+1),
                                   not assumed from the device model source -
                                   a real driver can't see the QEMU source */
    unsigned int bar0_readback; /* what we read back from the BAR0 config
                                    register immediately after writing it -
                                    should match bar0_phys if the write
                                    actually took */
    unsigned int gic_spi;      /* legacy INTx target, computed from the
                                   device's own Interrupt Pin register and
                                   the swizzling formula verified against
                                   this machine's actual interrupt-map */
    int found;
} pci_dev_t;

/* Standard PCI BAR-sizing: write all 1s, read back the size mask, restore
   the original value, then decode. Only handles 32-bit non-prefetchable
   memory BARs, which is what dma-accel uses. */
static unsigned int pci_bar0_size(unsigned int bus, unsigned int dev, unsigned int func) {
    unsigned int orig = pci_cfg_read32(bus, dev, func, PCI_BAR0);
    pci_cfg_write32(bus, dev, func, PCI_BAR0, 0xFFFFFFFFu);
    unsigned int mask = pci_cfg_read32(bus, dev, func, PCI_BAR0);
    pci_cfg_write32(bus, dev, func, PCI_BAR0, orig);
    /* low 4 bits are type/flags for a memory BAR, not part of the size */
    mask &= ~0xFu;
    if (mask == 0) return 0;
    return (~mask) + 1;
}

/* Scan bus 0 only (device 2 is where dma-accel actually landed on this
   machine, but we don't hardcode that - a real driver scans for the
   vendor/device match rather than assuming a slot). */
pci_dev_t pci_find_dma_accel(void) {
    pci_dev_t out;
    out.found = 0;

    for (unsigned int dev = 0; dev < 32; dev++) {
        unsigned int func = 0;
        unsigned short vendor = pci_cfg_read16(0, dev, func, PCI_VENDOR_ID);
        if (vendor == 0xFFFF) continue;   /* no device in this slot */
        unsigned short device = pci_cfg_read16(0, dev, func, PCI_DEVICE_ID);
        if (vendor != 0x1234 || device != 0xda00) continue;

        out.bus = 0;
        out.dev = dev;
        out.func = func;
        out.found = 1;

        /* Disable memory decode while we size and reassign BAR0 - some
           real hardware requires this even though QEMU's emulation is
           lenient about it; doing it properly costs nothing here. */
        unsigned short cmd0 = pci_cfg_read16(0, dev, func, PCI_COMMAND);
        pci_cfg_write16(0, dev, func, PCI_COMMAND, cmd0 & ~PCI_COMMAND_MEMORY);

        unsigned int size = pci_bar0_size(0, dev, func);
        out.bar0_size = size;

        /* Assign a BAR address ourselves within the 32-bit MMIO window -
           a real firmware/bootloader normally does this, but bare metal
           with no such firmware has to do it itself. Since we're the
           only PCI device we expect to matter here, a fixed base is
           fine; a general-purpose allocator would need to track
           already-assigned regions. */
        unsigned long assigned = PCIE_MMIO32_BASE;
        pci_cfg_write32(0, dev, func, PCI_BAR0, (unsigned int)assigned);
        out.bar0_phys = assigned;

        /* Read back immediately, through the exact same ECAM path, to
           separate two different possible failures: did the config
           write itself not take, or did it take but the BAR's memory
           region not actually get mapped into the address space? */
        out.bar0_readback = pci_cfg_read32(0, dev, func, PCI_BAR0);

        /* Enable memory space + bus mastering (needed for the device's
           own DMA engine to actually read/write system RAM later). */
        unsigned short cmd = pci_cfg_read16(0, dev, func, PCI_COMMAND);
        cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
        pci_cfg_write16(0, dev, func, PCI_COMMAND, cmd);

        unsigned char pin = pci_cfg_read8(0, dev, func, PCI_INTERRUPT_PIN);
        /* Swizzling formula verified against this exact machine's
           device-tree interrupt-map table (not assumed from generic
           PCI docs): SPI = 3 + ((device + pin - 1) mod 4). pin is
           1-based (1=INTA..4=INTD); pin=0 means "uses no legacy
           interrupt" per the PCI spec, which would be a real problem
           for us since we don't implement MSI yet. */
        if (pin >= 1 && pin <= 4) {
            out.gic_spi = 3 + ((dev + pin - 1) % 4);
        } else {
            out.gic_spi = 0xFFFFFFFFu;   /* sentinel: no legacy IRQ available */
        }

        return out;
    }

    return out;
}
