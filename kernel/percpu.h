#ifndef PERCPU_H
#define PERCPU_H
#include "tcb.h"

#define MAX_CPUS 4

typedef struct {
    unsigned long cpu_id;
    tcb_t *curr;
    volatile unsigned long irq_count;
} __attribute__((aligned(64))) cpu_local_t;
_Static_assert(sizeof(cpu_local_t) == 64, "cpu_local_t must be exactly one cache line");

extern cpu_local_t cpu_locals[MAX_CPUS];

static inline cpu_local_t *this_cpu(void) {
    cpu_local_t *p;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}

/* Runs on the CPU itself, before that CPU touches `current`. */
static inline void percpu_init(unsigned long cpu_id) {
    cpu_locals[cpu_id].cpu_id = cpu_id;
    __asm__ volatile("msr tpidr_el1, %0\n\tisb" :: "r"(&cpu_locals[cpu_id]) : "memory");
}

/* `current` = the running CPU's current task. Must stay AFTER the struct. */
#define current (this_cpu()->curr)
#endif
