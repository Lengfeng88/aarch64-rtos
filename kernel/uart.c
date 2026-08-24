#define UART0_BASE 0x09000000UL
#define UART_DR    (*(volatile unsigned int *)(UART0_BASE + 0x00))
#define UART_FR    (*(volatile unsigned int *)(UART0_BASE + 0x18))
#define UART_FR_TXFF (1 << 5)

static inline unsigned long irq_save(void) {
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    __asm__ volatile("msr daifset, #2");
    return flags;
}

static inline void irq_restore(unsigned long flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags));
}

void uart_putc(char c) {
    while (UART_FR & UART_FR_TXFF) {}
    UART_DR = c;
}

void uart_puts(const char *s) {
    unsigned long flags = irq_save();
    while (*s) uart_putc(*s++);
    irq_restore(flags);
}

static void uart_print_dec(unsigned long v) {
    char buf[21];
    int i = 20;
    buf[i] = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    while (buf[i]) uart_putc(buf[i++]);
}

/* klog: atomic multi-part log line. Wraps prefix + optional decimal value +
 * suffix in a single IRQ-masked critical section, so a compound line can
 * never be interleaved with another task's/ISR's output — unlike calling
 * uart_puts() three times back to back, which drops the mask between calls. */
void klog(const char *prefix, long val, int has_val, const char *suffix) {
    unsigned long flags = irq_save();
    const char *s = prefix;
    while (*s) uart_putc(*s++);
    if (has_val) uart_print_dec((unsigned long)val);
    s = suffix;
    while (*s) uart_putc(*s++);
    irq_restore(flags);
}