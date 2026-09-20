#include "psci.h"

static inline long psci_call(unsigned long func_id, unsigned long arg0,
                              unsigned long arg1, unsigned long arg2)
{
    register unsigned long x0 asm("x0") = func_id;
    register unsigned long x1 asm("x1") = arg0;
    register unsigned long x2 asm("x2") = arg1;
    register unsigned long x3 asm("x3") = arg2;

    asm volatile("hvc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x3)
                 : "memory");

    return (long)x0;
}

int psci_cpu_on(unsigned long target_cpu_mpidr, unsigned long entry_point_pa)
{
    return (int)psci_call(PSCI_CPU_ON, target_cpu_mpidr, entry_point_pa, 0);
}
