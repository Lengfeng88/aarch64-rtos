# P4 AArch64 RTOS: Design Walkthrough (M1–M8 single-core, M8–M12 SMP)

This document explains what each milestone of the RTOS is for and how it was built. It complements the README (status, build flags, known issues) and the M1–M7 write-up (bug investigations).

## Numbering note

The project has **two milestones named M8**:

- **M8 (policy)**, the last single-core milestone: a pluggable scheduling-policy interface with EWMA load prediction.
- **M8 (SMP)**, the first multi-core milestone: waking the secondary cores through PSCI.

Git tags and regression logs (`p4-m8` … `p4-m12-*`) refer to the SMP track. In the README, the single-core one is labeled "M8 (policy)".

## Goal and working method

The goal is a from-scratch, bare-metal AArch64 RTOS on QEMU `virt` (`-cpu cortex-a53`, no MMU, no floating point) that drives a real PCIe accelerator, a custom `dma-accel` device model, rather than only running toy tasks. Each layer is therefore verified against actual device behavior instead of assumed from documentation.

Three practices run through the whole project:

1. **Small increments, each with a regression.** Every milestone is checked with 30 runs of `stress_test.sh` (40 s per run; a run is clean when it prints 3 `DATA OK` lines and no `SYNC EXCEPTION`). SMP steps are run at both `-smp 1` and `-smp 4`.
2. **`-smp 1` must degrade to the previous behavior**, so every step stays comparable to the one before it. New features sit behind build flags, and the default build stays behavior-identical.
3. **Git tags record what was verified at the time.** History is not rewritten, so some old tags contain known bugs, and the README says which.

## Part 1: Single-core track (M1–M8 policy)

### M1: Boot

**Purpose:** get a known execution environment before anything else.
**Method:** `boot.S` drops from EL2 to EL1, initializes the stack and BSS, and installs the exception vector table. A polled PL011 UART provides output for all later debugging.

### M2: Timer

**Purpose:** a time base for the scheduler.
**Method:** the EL1 physical timer (PPI 30) fires every `tick_freq/2000`, i.e. every 0.5 ms.

### M3: Cooperative context switch

**Purpose:** run more than one task.
**Method:** each task has a `tcb_t` whose first field is the saved stack pointer. `switch_to()` saves and restores only the AAPCS64 callee-saved registers, which is valid only at a real function-call boundary. A new task's first run goes through a hand-built fake stack frame that lands in a trampoline; the trampoline explicitly unmasks IRQ before jumping to the task entry. Without that, a task started this way would never have preemption enabled.

### M4: GIC and preemption

**Purpose:** interrupt-driven preemption.
**Method:** GICv2 is brought up. The IRQ vector saves **all 31 general-purpose registers**, because an interrupt can land on any instruction. This is a different code path from M3's callee-saved-only switch, and keeping the two apart matters for everything that follows.

### M5: Synchronization primitives

**Purpose:** let tasks block and wake each other.
**Method:** Mutex, Semaphore, Event and Queue, scoped to a single waiter, with critical sections protected by masking IRQ. This "single core, masking IRQ is enough" assumption is exactly what the SMP track later stress-tests.

### M6: DMA and the accelerator HAL

**Purpose:** talk to a real device.
**Method:**

- PCIe ECAM enumeration, BAR sizing and mapping.
- INTx pin to SPI mapping (`dma-accel` uses SPI 37).
- SQ/CQ queues, doorbell submission, IRQ-driven completion.

The deepest bug here was `PCI_COMMAND_MEMORY` defined as bit 0 instead of bit 1. Finding it required temporary tracing inside QEMU's own `hw/pci/pci.c`.

### M7: Multi-task integration

**Purpose:** several workers submitting real DMA concurrently, woken by interrupts.
**Bugs found and fixed:**

- Blocking paths did not handle `pick_next_ready()` returning the caller itself, causing a stale stack-pointer restore.
- The "nobody else runnable" branch never resynced `current_idx`, permanently desynchronizing the scheduler.
- A race window between the doorbell (`accel_submit_copy()`) and `pending_register()` could drop a completion. It was closed by putting both in one IRQ-disabled critical section.

`dispatch_available_completions()` was also added as a polling fallback against occasional GIC redelivery failures.

### M8 (policy): Pluggable scheduling

**Purpose:** change scheduling decisions without touching call sites.
**Method:** `sched_policy_t` has two function pointers, `select_next` and `on_tick`. Three policies exist:

| Policy | Behavior |
|---|---|
| `policy_roundrobin` | The original round-robin, now behind the interface |
| `policy_ewma` | Round-robin selection, plus a per-task EWMA of recent CPU occupancy |
| `policy_load_aware` | Picks the READY task with the lowest `ewma_load` |

The EWMA is fixed-point on a 0–1000 scale with α = 0.3. A non-yielding `busy_task` converged to about 997, while mostly-blocked workers stayed at 0–510, so the estimator does separate load levels.

A workload-modeling bug turned up along the way: finished workers never set themselves to BLOCKED, so their EWMA climbed back toward saturation and `policy_load_aware` silently degenerated into round-robin. Setting `state = BLOCKED` when a worker finishes fixed it.

### Two large bugs in the single-core phase

- **`EC=0x0E` (illegal execution state).** `switch_to()` had no protection against interrupts landing mid-switch. Fix: mask IRQ for the whole switch and save each task's own DAIF as part of its context.
- **`EC=0x00` (`ELR=0` wild jump).** IRQ was open between `current = next` and `switch_to()`. Fix: `yield()` and `block_current_and_switch()` now do pick-next, set `current`, and switch, all with IRQ masked. After this the kernel ran 300/300 clean. That the original `ELR=0` crash shared this cause is inferred from the result, not proven.

## Part 2: SMP track (M8–M12)

**Guiding principle: correctness first.** Get the cores running, then give each its own state, then add locks, then split the scheduler, and only then do migration and balancing.

### M8 (SMP): Wake the secondaries

**Purpose:** bring CPU1–3 up.
**Method:**

- PSCI `CPU_ON` starts each secondary, each with its own stack; a secondary masks IRQ, sets its vector table and parks in `wfe`.
- QEMU `virt` without EL3 implements PSCI at EL2, so the call must be `hvc #0`. Using `smc` raises an Undefined Instruction exception.
- `tcb_t` was consolidated into a single `tcb.h` with size and offset static asserts, because duplicate definitions across `.c` files had caused real bugs.

### M9: Per-CPU state and per-CPU interrupts

**Purpose:** give each core its own identity and interrupts before it shares anything.
**Method:**

- `percpu.h` defines `cpu_local_t`. `TPIDR_EL1` holds a pointer to it, so `this_cpu()` is one register read.
- `current` becomes a macro for `this_cpu()->curr`, so existing code needed no changes.
- GIC split: the distributor is initialized once by CPU0. The CPU interface (`GICC_PMR/CTLR`) is banked per core, and PPI 30 is enabled per core.
- A secondary's interrupt handler only counts, re-arms the timer and does EOI. It never touches scheduler state, so interrupts are proven on secondaries before they share anything.

### M10: Spinlocks

**Purpose:** mutual exclusion across cores.
**Method:** `LDAXR/STXR` with `WFE`, plus IRQ-saving variants (`spin_lock_irqsave`). The self-test (`SMP_SELFTEST=1`) has every core increment a plain counter and a locked counter 200,000 times. The locked counter comes out exact, while the plain one loses updates. That both proves the lock and shows what happens without it.

**Caveat:** the MMU is off and data accesses are Device memory, so exclusive access has only been validated under QEMU TCG.

### M11: Per-CPU runqueues

**Purpose:** let each core schedule its own tasks, and let tasks wake each other across cores.
**Method, in four steps:**

1. **Refactor only.** `runqueue_t runqueues[MAX_CPUS]`, all tasks still on CPU0, behavior unchanged.
2. **Idle task per secondary**, registered on its own runqueue.
3. **Pinned test tasks on CPU1–3** with real context switching in the secondary's ISR.
4. **Cross-core correctness (D0–D2):**
   - `sem_t` moves into a single `sync.h`; `tcb_t` gains a `cpu` field (its home core).
   - Each runqueue gets a spinlock.
   - `sched_wake()` locks only the target task's home runqueue.
   - Semaphores become locked. A CPU1↔CPU2 ping/pong test runs 2000 rounds to verify cross-core wakeups.
   - `uart_lock` and `accel_lock` are added. `accel_lock` covers submit plus pending registration and completion drain plus match plus post, closing the window where a completion could be consumed before its waiter registered.
   - Workers are spread across cores (worker *i* goes to core `i % online`).

**Lock order:** `accel_lock` → semaphore lock → runqueue lock.

**Wake latency:** cross-core wakeups use no IPI. Secondaries idle in `wfi`, which `sev` cannot wake, so a wakeup takes effect on the target core's next 0.5 ms tick.

**Process lesson:** at one point a patch believed to be applied had not been written to disk, and `DATA OK` fell from 3 to 1. Since then every edit goes through an anchor-checked script that fails on zero or multiple matches.

### M12: Migration and load balancing

**Purpose:** move tasks between cores.
**Method, in three steps:**

1. **`sched_migrate()`** moves only READY, non-running tasks. It takes two runqueue locks, always the lower CPU id first. A task that could be a semaphore waiter must not change `t->cpu`, or `sched_wake()` could lock the wrong queue.
2. **`sched_load_balance_pass()`** decides by per-queue **task count**, not EWMA: EWMA was only ever compared within a single queue and was never validated as comparable across cores. A `pinned` flag on `tcb_t` keeps cooperating tasks such as ping/pong from being moved.
3. **Trigger:** moved into CPU0's own tick path, once every 200 ticks (about 100 ms).

**Bug found by regression:** the "is this task running?" check compared against the *calling* core's `current` instead of the core the task was on, so a task executing elsewhere could occasionally be pulled away. The fix reads the correct core's `cpu_locals[...].curr`. Because of this, the tags `p4-m12-step1` and `p4-m12-step2` are known-bad and kept only as history.

M12 is still gated behind `LOAD_BALANCE_SELFTEST` and is not default-on.

### IRQ affinity (extension after M12)

**Purpose:** the DMA completion interrupt was hardcoded to CPU0 (`ITARGETSR = 0x01`), so every completion passed through CPU0 regardless of where the task ran.
**Method:**

- `gic_enable_irq_target(id, mask)` writes the target byte; `gic_enable_irq(id)` remains as a wrapper for CPU0.
- `make DMA_IRQ_CPU=N` statically routes the DMA IRQ to CPU N.
- `secondary_irq` gained a DMA branch that acks the device before EOI. Without it, the level-triggered line would storm.
- The enable moves after `smp_boot_secondaries()` (the clamp needs `smp_online`), and routing falls back to CPU0 if the target core is not online.
- The clamp is done in software because QEMU stores `ITARGETSR` bits for cores that do not exist (observed at `-smp 2`). At `-smp 1`, `ITARGETSR` reads back 0 for every mask (uniprocessor behavior).
- Per-core DMA interrupt counters make routing observable, and `route_check.sh` verifies it from the archived logs.

## How the pieces fit together

- **"Each core works alone" comes before "cores share state".** Secondaries only counted in M9, got locks in M10, and touched the scheduler in M11.
- **Probe, don't assume.** PSCI needing `hvc`, `ITARGETSR` behavior at `-smp 1` and `-smp 2`, and the PCI command-register bit were all found by measurement.
- **Migration and balancing are deliberately conservative.** They target secondaries only, use task counts, protect cooperating tasks with `pinned`, and stay off by default because the verified scenarios are narrow.

## Verification snapshot

| Step | Regression |
|---|---|
| Single-core mask fix | 300/300 clean |
| SMP M8 through D2a | 30/30 clean at `-smp 1` and `-smp 4` (per step) |
| M12 steps (after the migration fix) | 30/30 clean at `-smp 4` |
| IRQ affinity | Five 30-run batches, all 30/30: `DMA_IRQ_CPU=1` at `-smp 4` and `-smp 1`, `DMA_IRQ_CPU=2` at `-smp 4`, `DMA_IRQ_CPU=0` at `-smp 4` (control), default build at `-smp 4`; `route_check.sh` found every DMA interrupt on the expected core |

## What is not covered

- The MMU is off, so there is no memory protection, and spinlocks are validated only under QEMU TCG.
- Load balancing has been exercised in one scenario at `-smp 4` and is not default-on.
- IRQ routing is verified only for single-bit targets (CPU1 and CPU2 at `-smp 4`, fallback at `-smp 1` and, as a single smoke run, `-smp 2`). Multi-bit masks are accepted by QEMU but their delivery semantics are untested, and routing has not been combined with load balancing.
- Two older items are unconfirmed against the current code: a CPU0-side SP-corruption race first seen during early SMP bring-up (probably subsumed by later fixes but never explicitly closed), and a `cmd_id=3` `DATA MISMATCH` reproduced on pure single-core and never root-caused.
