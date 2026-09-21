#ifndef SPINLOCK_H
#define SPINLOCK_H
/* Test-and-set spinlock on AArch64 exclusives (LDAXR/STXR + WFE, the same
 * shape as the Linux arm64 spinlock). Unlock is a store-release, which
 * clears the exclusive monitors of waiting cores and wakes their WFE. */
typedef struct { volatile unsigned int locked; } spinlock_t;
#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l) {
    unsigned int tmp;
    __asm__ volatile(
        "   sevl\n"
        "1: wfe\n"
        "2: ldaxr %w0, [%1]\n"
        "   cbnz  %w0, 1b\n"
        "   stxr  %w0, %w2, [%1]\n"
        "   cbnz  %w0, 2b\n"
        : "=&r"(tmp)
        : "r"(&l->locked), "r"(1u)
        : "memory");
}

static inline void spin_unlock(spinlock_t *l) {
    __asm__ volatile("stlr wzr, [%0]" :: "r"(&l->locked) : "memory");
}

/* Local IRQ mask AND the lock: the mask keeps this core's own ISR from
 * re-entering the critical section, the lock keeps the other cores out. */
static inline unsigned long spin_lock_irqsave(spinlock_t *l) {
    unsigned long f;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #2" : "=r"(f) :: "memory");
    spin_lock(l);
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, unsigned long f) {
    spin_unlock(l);
    __asm__ volatile("msr daif, %0" :: "r"(f) : "memory");
}
#endif
