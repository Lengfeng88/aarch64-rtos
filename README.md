# ARMv8-A RTOS on QEMU aarch64 virt

A from-scratch, bare-metal RTOS kernel targeting `qemu-system-aarch64 -M virt`, built up milestone by milestone from a cold boot to a preemptive scheduler with synchronization primitives driving a real PCIe accelerator device over DMA, and then to a 4-core SMP kernel with per-CPU runqueues, cross-CPU wakeups, task migration and optional routing of the DMA completion interrupt to a secondary core.

No Linux, no existing RTOS base (not FreeRTOS/Zephyr/etc.) — every layer (boot, exception vectors, context switching, GIC, sync primitives, PCIe enumeration, DMA HAL, PSCI SMP bring-up, spinlocks, per-CPU scheduling) is written and verified against real hardware behavior in QEMU, not assumed from documentation.

## Platform

- QEMU `virt` machine, `-cpu cortex-a53`, ARMv8-A, `-smp 1` to `-smp 4`
- Bare-metal C + AArch64 assembly, no MMU, no floating point (`-mgeneral-regs-only`)
- Toolchain: `aarch64-linux-gnu-gcc` 13.3.0, `qemu-system-aarch64` 8.2.2
- M6/M7 and everything after require a custom `dma-accel` PCIe device model compiled into QEMU (see [Building QEMU with dma-accel](#building-qemu-with-dma-accel) below)

## Milestones

| # | What | Status |
|---|------|--------|
| M1 | Boot → exception vectors → EL1 → UART | ✅ |
| M2 | Generic Timer → timer interrupt → scheduler tick | ✅ |
| M3 | Task → Context → cooperative context switch | ✅ |
| M4 | GIC → IRQ → ISR → preemptive task wakeup | ✅ |
| M5 | Mutex / Semaphore / Event / Queue | ✅ |
| M6 | DMA → Accelerator HAL → MMIO (PCIe device) | ✅ |
| M7 | Full integration — concurrent tasks, real hardware DMA, IRQ-driven wakeup | ✅ (~93-96% reliable at the time; later single-core fixes brought it to 300/300 clean, see [Known issues](#known-issues)) |
| M8 (policy) | Pluggable scheduling policy interface + EWMA CPU load prediction | ✅ |

The SMP track below continues from here. **Numbering note:** git tags and regression logs (`p4-m8` … `p4-m12-*`, `docs/regression_logs/`) use M8–M12 for the SMP track. The scheduling-policy work above predates it and is called "M8 (policy)" to avoid confusion.

### SMP track

| # | What | Status |
|---|------|--------|
| M8 | PSCI (HVC) `CPU_ON` for CPU1–3, per-CPU boot stacks, secondaries park in `wfe` | ✅ |
| M9 | Per-CPU state (`TPIDR_EL1` → `this_cpu()`, `current` is per-CPU), per-CPU timer/GIC init, count-only tick path on secondaries | ✅ |
| M10 | `LDAXR`/`STXR` + `WFE` spinlocks (`kernel/spinlock.h`), 4-core shared-counter self-test | ✅ |
| M11 | Per-CPU runqueues, per-secondary idle, pinned tasks with real context switching on CPU1–3 | ✅ |
| D0–D2 | Single shared `sem_t` (`kernel/sync.h`), cross-CPU `sched_wake`, locked semaphores, UART and accelerator locks, DMA workers spread across CPUs | ✅ |
| M12 | Task migration (`sched_migrate`), load-balance pass, `pinned` flag, periodic trigger from CPU0's tick | ✅ (gated behind `LOAD_BALANCE_SELFTEST`) |
| IRQ affinity | `gic_enable_irq_target(id, mask)`, opt-in `DMA_IRQ_CPU=N` static routing of the DMA completion IRQ to a secondary, DMA branch in `secondary_irq`, per-CPU DMA IRQ counters | ✅ (opt-in build option; the default build still routes to CPU0) |

Each step was checked against the 30-run stress regression (`stress_test.sh`: 40 s per run, clean = 3 `DATA OK` and no `SYNC EXCEPTION`). M8 through D2a were regressed at both `-smp 1` and `-smp 4`; the M12 steps were regressed at `-smp 4`. Regression logs are archived in `docs/regression_logs/`.

The IRQ-affinity work was checked with five 30-run batches (`stress_irq.sh`), every one 30/30 clean: `DMA_IRQ_CPU=1` at `-smp 4` and `-smp 1`, `DMA_IRQ_CPU=2` at `-smp 4`, `DMA_IRQ_CPU=0` at `-smp 4` as a control, and the default build at `-smp 4`. `route_check.sh` then confirmed from the logs that in every run the DMA interrupts were counted only on the expected core. Logs: `docs/regression_logs/irq_affinity/`.

**Tags.** Build from `p4-irq-affinity`, which includes M12 step 3 and the debug-print fix in `7cfdb97`. `p4-m12-step1` and `p4-m12-step2` are kept as historical records of what was verified at the time; they predate the fix in `cd4fbd1` (`p4-m12-step2-fixed1`) and can crash under `-smp 4` (the migration guard compared against the calling CPU's `current` instead of the source CPU's). Do not build from them. Other tags: `p4-m12-step3`, `p4-single-core-baseline`, `p4-single-core-maskfix-300`, `p4-m8`, `p4-align`, `p4-m9a`, `p4-m10a`, `p4-m11b`, `p4-d2a`.

For a walkthrough of each milestone's purpose and method (M1–M8 single-core, M8–M12 SMP, IRQ affinity), see [`docs/P4-design-walkthrough.md`](docs/P4-design-walkthrough.md). Full write-up of what each M1–M7 milestone does, the bugs found along the way, and how they were diagnosed: see [`docs/M1-M7-writeup.md`](docs/M1-M7-writeup.md) (including the resolved `EC=0x0E` investigation) and [`EC-0x00-investigation.md`](EC-0x00-investigation.md) (the `EC=0x00` / `ELR=0` investigation, now believed fixed).

## Building and running

```bash
cd rtos
make run
```

`Makefile`'s `QEMU` variable points at your own `dma-accel`-enabled `qemu-system-aarch64` build (see below) — adjust the path if yours lives elsewhere. `make run` boots the current kernel image under QEMU with `-nographic`; output goes to your terminal over the emulated UART.

To run with multiple cores, invoke QEMU directly:

```bash
$QEMU -M virt -cpu cortex-a53 -smp 4 -nographic -device dma-accel -kernel build/kernel.elf
```

At `-smp 1` the kernel falls back to a single-CPU configuration (all workers on CPU0).

Build flags (all off by default; the default build carries no debug hooks):

| Flag | Effect |
|------|--------|
| `make DEBUG_HOOKS=1` | Keeps the switch/SP-corruption debug hooks (`check_sp_write`, `trace_switch_resume`, post-switch bounds checks) |
| `make SMP_SELFTEST=1` | Boot-time 4-core shared-counter test (plain vs. locked counter) |
| `make LOAD_BALANCE_SELFTEST=1` | Enables the periodic load-balance / task-migration pass |
| `make GIC_PROBE=1` | Boot-time `ITARGETSR` readback experiment on an unused SPI (prints `PROBE mask=` / `PROBE readback=`) |
| `make DMA_IRQ_CPU=N` | Routes the DMA completion IRQ to CPU N (1–3) instead of CPU0; falls back to CPU0 if CPU N is not online |

Changing any of these flags requires `make clean` first: dependency tracking only follows header changes, not `CFLAGS`, so stale objects would otherwise be reused.

```bash
make clean   # remove build artifacts
```

## Building QEMU with dma-accel

M1–M5 run on stock `qemu-system-aarch64`. M6 onward needs a custom PCIe device model (`dma-accel`, vendor:device `1234:da00`) compiled in:

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

## M8 (policy): Scheduling policy interface and load prediction

Added a pluggable `sched_policy_t` interface (`select_next` + `on_tick` function pointers) so the scheduler can swap decision logic without touching call sites in `main.c`/`sync.c`. Three policies implemented:

- `policy_roundrobin` — the original M1-M7 round-robin logic, unchanged, now behind the interface
- `policy_ewma` — round-robin selection, but tracks a fixed-point EWMA (`ewma_load`, 0-1000 scale, α=0.3) of each task's recent CPU occupancy via `on_tick`
- `policy_load_aware` — selects the READY task with the lowest `ewma_load` instead of pure round-robin

Verified with a dedicated non-yielding `busy_task` alongside the normal DMA workers: `ewma_load` converges to ~997/1000 for the busy task vs. 0-510 for the mostly-blocked workers, confirming the estimator distinguishes real load levels. Found and fixed a real workload-modeling bug in the process: completed worker tasks never set their own state to `BLOCKED`, so they stayed in the scheduling candidate pool indefinitely and their `ewma_load` re-climbed toward saturation over time — silently degenerating `policy_load_aware` back into round-robin once workers finished. Fixed by setting `state=1` when a worker's work is done.

`ewma_load` is only compared within a single runqueue; it has not been validated as comparable across cores, so the M12 balancer uses per-runqueue task counts instead.

See [`tools/`](tools/) for the Python-based log parsing/analysis tooling built alongside this work, and [`docs/M8-RL-Scheduling-Feasibility-Note.md`](docs/M8-RL-Scheduling-Feasibility-Note.md) for a design-only feasibility evaluation of extending this into a reinforcement-learning scheduling research track (no kernel changes, simulation only, not yet implemented).

## Known issues

Status as of 2026-09-23.

**Limitations**

- Load balancing is gated behind `LOAD_BALANCE_SELFTEST` and is not default-on; it has been verified in one scenario at `-smp 4`, and running it continuously is a bigger exposure than that covers. Migration targets are secondaries only (CPU1 to `smp_online-1`).
- The DMA completion IRQ (SPI 37) is routed to CPU0 by default (`gic_enable_irq(id)` is `gic_enable_irq_target(id, 0x01)`), so by default CPU0 is the chokepoint for every accelerator completion regardless of where a worker runs. `make DMA_IRQ_CPU=N` (N = 1..3) statically routes it to CPU N instead, and if CPU N is not online it falls back to CPU0. That clamp is done in software because QEMU stores `ITARGETSR` bits for CPUs that do not exist (observed at `-smp 2`), which would silently route the interrupt nowhere.
- IRQ routing has only been verified for single-bit targets: CPU1 and CPU2 at `-smp 4` (30 runs each), and the fallback at `-smp 1` (30 runs) and `-smp 2` (one smoke run). Multi-bit masks are accepted by QEMU but their delivery semantics are untested, and routing has not been combined with `LOAD_BALANCE_SELFTEST`. At `-smp 1`, `ITARGETSR` reads back 0 for every mask (uniprocessor behaviour), so the readback cannot verify routing there; the regressions check the per-CPU `DMA IRQ c0..c3` counters that the busy task prints instead.
- A migrated task is picked up by its new core on that core's next timer tick (~0.5 ms); `sched_wake`'s `sev` cannot wake a core idling in `wfi`.
- MMU is off (Device-type memory accesses), so the exclusive-access spinlocks are only validated under QEMU TCG.
- No memory protection at all (see design notes).

**Open, unconfirmed against current code**

- A CPU0-side SP-corruption race first seen during early SMP bring-up (reproduced even with CPU1 never woken). Probably subsumed by the later IRQ-masking fixes and the many clean regressions since, but never explicitly closed out.
- A `cmd_id=3` `DATA MISMATCH` reproduced on pure single-core in an earlier investigation, logged as not root-caused, and not revisited since.

### Resolved

- **`EC=0x0E` ("Illegal Execution state") crash** — root-caused: `switch_to()` had no self-protection against interrupts landing mid-switch. Fixed by having `switch_to()` mask IRQ for its entire duration and treat DAIF as per-task saved context. See the writeup's `EC=0x0E` section for the full root-cause/fix/verification.
- **`EC=0x00` ("Unknown reason") / `ELR=0x0` wild-jump crash**, previously ~3-13% of stress runs — believed fixed by two single-core changes: (1) IRQ was open between `current = next` and `switch_to()`, fixed by doing pick-next, `current = next` and `switch_to()` with IRQ masked (`yield()` and `block_current_and_switch()`); (2) in the `DEBUG_HOOKS` build, the `check_sp_write` hook clobbered `new_sp`. After the fix the kernel ran 300/300 clean (30/30 first). That the original `ELR=0` crash shared the IRQ-window cause is inferred from this result, not proven.
- **SMP migration crash** — `sched_migrate` / the balance pass checked "is this task running" against the calling CPU's `current`, so a task actually executing on another core could be pulled off it. Fixed in `cd4fbd1` (`p4-m12-step2-fixed1`), re-verified 30/30 at `-smp 4`.

## Code layout

```
boot/boot.S            — reset entry, EL2→EL1 drop, stack/BSS init, per-CPU secondary stacks
kernel/vectors.S       — exception vector table, full-register IRQ save/restore
kernel/switch.S        — cooperative context switch (callee-saved only)
kernel/uart.c          — PL011 polling driver (spinlock-protected)
kernel/task.c          — TCB, stack frame construction, task_trampoline
kernel/tcb.h           — the single tcb_t definition (size/offset static asserts)
kernel/percpu.h        — cpu_local_t, this_cpu() via TPIDR_EL1, per-CPU `current`
kernel/smp.c           — PSCI secondary bring-up, secondary_main, SMP self-tests
kernel/spinlock.h      — LDAXR/STXR + WFE spinlocks, irqsave variants
kernel/sched.c         — pluggable sched_policy_t, per-CPU runqueues, sched_wake, sched_migrate, load-balance pass
kernel/gic.c           — GICv2 distributor/CPU interface (PPI/SGI and SPI); SPIs default to CPU0, `gic_enable_irq_target` picks a core
kernel/sync.h          — the single sem_t definition
kernel/sync.c          — Semaphore (locked, cross-CPU safe) / Mutex / Event / Queue (IRQ-mask only, CPU0 use)
kernel/pci.c           — PCIe ECAM enumeration, BAR sizing/mapping, IRQ pin→SPI
kernel/accel.c         — dma-accel HAL: SQ/CQ registration, submission, IRQ-driven completion
kernel/main.c          — test harness (workers, busy task, per-CPU tests, IRQ handler)
tools/                 — Python log parsing (parse_log.py) and cross-batch stress-test summary (summarize_batches.py)
stress_irq.sh          — 30-run regression of one ELF at a given `-smp` (clean = 3 `DATA OK`, no `SYNC EXCEPTION`/`CORRUPT`)
route_check.sh         — reads the archived logs and checks that DMA IRQs were counted only on the expected core
docs/                  — writeups, baseline ELFs/hashes, archived regression logs
```

## Design notes worth knowing before reading the code

- **No MMU, no caches** — all addresses in the kernel are physical addresses. Simplifies everything but means there's no memory protection at all; a bad pointer anywhere can corrupt anything.
- **Cooperative vs. preemptive context switch are different code paths.** `switch_to()` (M3) only saves/restores AAPCS64 callee-saved registers, which is only valid at a real function-call boundary. The IRQ path (M4) saves *all* 31 general-purpose registers, because an interrupt can land anywhere.
- **A task's very first activation is not the same as a resume.** New tasks start via a hand-constructed fake stack frame (`task_init()`) that makes the first `switch_to()` land in a trampoline, which explicitly unmasks IRQ before jumping to the real entry point — otherwise PSTATE.I (set automatically on IRQ entry) never gets cleared for a task that started this way, silently killing preemption for it forever.
- **Struct layouts must match byte-for-byte across every `.c` file that touches them.** Several real bugs in this project came from a shared struct (`tcb_t`, `event_t`) being defined slightly differently in two files compiled separately — C won't catch this at compile time. `tcb_t` and `sem_t` now each live in one header (`tcb.h`, `sync.h`) with static asserts on size and offsets.
- **Tasks have a home CPU, and pinned tasks never move.** `sched_wake()` locks only the runqueue of the task's `cpu` field, so a task must not change CPU while it can be a semaphore waiter. Cross-core cooperating tasks (the ping/pong pair) are marked `pinned`, and the balancer skips them. Lock order: `accel_lock` → semaphore lock → runqueue lock; cross-runqueue operations lock the lower CPU id first.
- **Secondaries idle in `wfi`, not `wfe`.** The per-secondary idle `tcb_t` is a placeholder; the real idle loop is the secondary's own C function. Wakeups therefore land on the next timer tick.
- **The IRQ-masking window matters more than it looks.** Any point between choosing the next task and actually switching to it must run with IRQ masked; leaving it open was the root cause of the intermittent corruption the single-core regressions chased.
- **SPI routing is static and decided at boot.** `ITARGETSR` is per-interrupt, not per-event, so the DMA IRQ can be pinned to one core but not balanced. When `DMA_IRQ_CPU` is set, the enable runs after `smp_boot_secondaries()` because the clamp needs `smp_online`; the default build keeps the original enable before secondary bring-up. Any core that can receive the DMA IRQ needs a handler branch that acks the device before EOI, otherwise the level-triggered line storms; `secondary_irq` has one.
