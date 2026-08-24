# ARMv8-A RTOS on QEMU aarch64 virt

A from-scratch, bare-metal RTOS kernel targeting `qemu-system-aarch64 -M virt`, built up milestone by milestone from a cold boot to a preemptive scheduler with synchronization primitives driving a real PCIe accelerator device over DMA.

No Linux, no existing RTOS base (not FreeRTOS/Zephyr/etc.) — every layer (boot, exception vectors, context switching, GIC, sync primitives, PCIe enumeration, DMA HAL) is written and verified against real hardware behavior in QEMU, not assumed from documentation.

## Platform

- QEMU `virt` machine, `-cpu cortex-a53`, ARMv8-A
- Bare-metal C + AArch64 assembly, no MMU, no floating point (`-mgeneral-regs-only`)
- Toolchain: `aarch64-linux-gnu-gcc` 13.3.0, `qemu-system-aarch64` 8.2.2
- M6/M7 require a custom `dma-accel` PCIe device model compiled into QEMU (see [Building QEMU with dma-accel](#building-qemu-with-dma-accel) below)

## Milestones

| # | What | Status |
|---|------|--------|
| M1 | Boot → exception vectors → EL1 → UART | ✅ |
| M2 | Generic Timer → timer interrupt → scheduler tick | ✅ |
| M3 | Task → Context → cooperative context switch | ✅ |
| M4 | GIC → IRQ → ISR → preemptive task wakeup | ✅ |
| M5 | Mutex / Semaphore / Event / Queue | ✅ |
| M6 | DMA → Accelerator HAL → MMIO (PCIe device) | ✅ |
| M7 | Full integration — concurrent tasks, real hardware DMA, IRQ-driven wakeup | ✅ (~90-95% reliable, one known open issue) |

Full write-up of what each milestone does, the bugs found along the way, and how they were diagnosed: see [`docs/M1-M7-writeup.md`](docs/M1-M7-writeup.md).

## Building and running

```bash
cd rtos
make run
```

`Makefile`'s `QEMU` variable points at your own `dma-accel`-enabled `qemu-system-aarch64` build (see below) — adjust the path if yours lives elsewhere. `make run` boots the current milestone's kernel image under QEMU with `-nographic`; output goes to your terminal over the emulated UART.

```bash
make clean   # remove build artifacts
```

## Building QEMU with dma-accel

M1–M5 run on stock `qemu-system-aarch64`. M6/M7 need a custom PCIe device model (`dma-accel`, vendor:device `1234:da00`) compiled in:

1. Drop `dma_accel.c` into `hw/misc/` of a QEMU source checkout
2. Add to `hw/misc/meson.build`:
   ```
   system_ss.add(when: 'CONFIG_DMA_ACCEL', if_true: files('dma_accel.c'))
   ```
3. Add to `hw/misc/Kconfig`:
   ```
   config DMA_ACCEL
       bool
       default y
       depends on PCI && MSI_NONBROKEN
   ```
4. Configure and build with `aarch64-softmmu` in the target list:
   ```bash
   ../configure --target-list=x86_64-softmmu,aarch64-softmmu   # add whichever targets you need
   ninja qemu-system-aarch64
   ```
5. Confirm it's in: `strings ./qemu-system-aarch64 | grep -c dma-accel` should return a positive count.

The device's register layout is defined in `dma_accel_regs.h` (BAR0 offsets, SQ/CQ descriptor formats, opcodes) — this is the source of truth for `kernel/pci.c` and `kernel/accel.c`.

## Known issues

- **`EC=0x0E` ("Illegal Execution state") crash**, ~7-10% of M7 runs with 3 concurrent workers. Happens *after* all completions have already been correctly dispatched (confirmed via register-dump + pending-table state at crash time), so it's a distinct issue from the three scheduler bugs already found and fixed during M7 bring-up — not yet root-caused. See the writeup for what's been ruled out so far.

## Code layout

```
boot/boot.S           — reset entry, EL2→EL1 drop, stack/BSS init
kernel/vectors.S       — exception vector table, full-register IRQ save/restore
kernel/switch.S        — cooperative context switch (callee-saved only)
kernel/uart.c          — PL011 polling driver
kernel/task.c          — TCB, stack frame construction, task_trampoline
kernel/sched.c         — pick_next_ready(), yield(), task registration
kernel/gic.c           — GICv2 distributor/CPU interface (PPI/SGI and SPI)
kernel/sync.c          — Semaphore / Mutex / Event / Queue
kernel/pci.c           — PCIe ECAM enumeration, BAR sizing/mapping, IRQ pin→SPI
kernel/accel.c         — dma-accel HAL: SQ/CQ registration, submission, IRQ-driven completion
kernel/main.c          — current milestone's test harness (replaced each milestone)
```

## Design notes worth knowing before reading the code

- **No MMU, no caches** — all addresses in the kernel are physical addresses. Simplifies everything but means there's no memory protection at all; a bad pointer anywhere can corrupt anything.
- **Cooperative vs. preemptive context switch are different code paths.** `switch_to()` (M3) only saves/restores AAPCS64 callee-saved registers, which is only valid at a real function-call boundary. The IRQ path (M4) saves *all* 31 general-purpose registers, because an interrupt can land anywhere.
- **A task's very first activation is not the same as a resume.** New tasks start via a hand-constructed fake stack frame (`task_init()`) that makes the first `switch_to()` land in a trampoline, which explicitly unmasks IRQ before jumping to the real entry point — otherwise PSTATE.I (set automatically on IRQ entry) never gets cleared for a task that started this way, silently killing preemption for it forever.
- **Struct layouts must match byte-for-byte across every `.c` file that touches them.** Several real bugs in this project came from a shared struct (`tcb_t`, `event_t`) being defined slightly differently in two files compiled separately — C won't catch this at compile time.
