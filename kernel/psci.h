#ifndef PSCI_H
#define PSCI_H

#define PSCI_CPU_ON  0xC4000003UL

int psci_cpu_on(unsigned long target_cpu_mpidr, unsigned long entry_point_pa);

#endif
