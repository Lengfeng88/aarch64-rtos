# P4 AArch64 RTOS: Roadmap

Status as of 2026-09-23. This roadmap covers the P4 RTOS only.

## Ground rules

These are the project's own rules, and every item below is written to respect them:

1. **Verify before claiming.** A feature counts as done only after a regression run (30-run batches, `-smp 1` and `-smp 4` where relevant), and the README states exactly what was and was not covered.
2. **Don't design what won't be built.** Design-only work stays labeled as design-only.
3. **Tags record what was verified at the time.** Known-bad tags are kept as history and marked, not rewritten.
4. **Resume and README claims match demonstrated work.** For example, "IRQ affinity" means static single-bit routing verified at `-smp 4`, not general interrupt affinity.

## Where things stand

The single-core kernel (M1–M8 policy) and the SMP track (M8–M12, plus static DMA IRQ routing) are implemented and regression-tested. What remains is less "more features" and more three things:

- **Closing old open items** (two unconfirmed threads, one untested claim about the `pinned` filter).
- **Real-time semantics.** The kernel is called an RTOS but schedules with round-robin and EWMA policies, has no task priorities, no sleep API, and no measured latency numbers.
- **Validation gaps.** The MMU is off, and spinlocks are validated only under QEMU TCG.

## Overview

| Phase | Theme | Effort | Priority |
|---|---|---|---|
| 0 | Housekeeping and commits | S | Do first |
| 1 | Close open items | S–M | High |
| 2 | Wake path and stack safety | M | Medium |
| 3 | Real-time semantics | L | Highest value |
| 4 | MMU, caches, memory protection | L | Optional |
| 5 | Presentation | S–M | When stopping |

Effort: S = a session or less, M = a few sessions, L = a multi-week phase.

## Phase 0: Housekeeping (S)

- [ ] Commit the IRQ-affinity code, then the README, then `docs/P4-design-walkthrough.md` (separate commits); tag `p4-irq-affinity` on the code commit.
- [ ] Replace `stress_irq.sh` with the fixed version (routing metrics moved to `route_check.sh`).
- [ ] Record ELF hashes for the new builds in `docs/baseline_sha256.txt`, computed from the actual built ELFs.
- [ ] Update `EC-0x00-investigation.md` and the M1–M7 write-up, which still describe `EC=0x00` as open, and drop the outdated "Rebuilding pre-M8 tags" note.
- [ ] Merge `stress_test.sh`, `stress_irq.sh` and `route_check.sh` into one `tools/regress.sh` so a regression is a single command (ELF, `-smp`, tag, runs).

## Phase 1: Close the open items (S–M)

| Item | Method | Exit criterion |
|---|---|---|
| `cmd_id=3` `DATA MISMATCH` | Run the current default build at `-smp 1` for 100+ runs. If it reproduces: log src/dst contents and diff offsets, check buffer reuse before completion, and read the `dma-accel` model's copy path. | Root-caused and fixed, or "not reproduced in N runs". Never "fixed" without a cause. |
| CPU0 SP-corruption race | The detection hooks exist only in `DEBUG_HOOKS=1` builds, so clean default-build runs say little. Run a hooks build at `-smp 2` and `-smp 4`, including the configuration where CPU1 is never woken. | N clean hook-build runs, or a reproduced case with a cause. |
| `pinned` filter never forced | Build a scenario where `ping`/`pong` would be chosen as migration victims without the filter, and show that the filter prevents it. Include tasks that block and wake during migration, at `-smp 2/3/4`. | The filter is proven by a forcing test. |
| Load balancing default-on? | Decide from the evidence of the two rows above. | An explicit yes/no recorded in the README. |
| IRQ routing + load balancing | Regress `DMA_IRQ_CPU=N` together with `LOAD_BALANCE_SELFTEST=1`. | 30/30, or a found bug. |

## Phase 2: Wake path and stack safety (M)

- [ ] **Measure wake latency first.** Cross-core wakeups currently wait for the next 0.5 ms tick, because `sev` cannot wake a core idling in `wfi`. Record the ping/pong round-trip time before changing anything.
- [ ] **Replace it with an SGI reschedule IPI**, then compare against the baseline. Keep it only if the numbers justify the added complexity.
- [ ] **Stack high-water marks.** Secondary ISRs (now including the DMA path) run on 4 KiB task stacks with no guard. Fill stacks with a pattern and report the maximum depth per task; add an overflow check.
- [ ] *(Optional, time-boxed)* Test multi-bit `ITARGETSR` masks to see whether QEMU implements GICv2's "one of these cores" delivery. Skip if not needed.

## Phase 3: Real-time semantics (L)

This phase turns "a multicore kernel" into an RTOS.

1. **Latency and jitter measurement.** Interrupt-to-task latency and scheduling jitter under load, reported as histograms. QEMU TCG timing depends on the host, so state the limits of the measurement. A deterministic run would use `-icount`, which is incompatible with multi-threaded TCG and therefore single-core-only.
2. **Fixed-priority preemptive scheduling.** Per-priority ready queues on top of the existing per-CPU runqueues, keeping `sched_policy_t` as the extension point.
3. **Priority inheritance on the mutex**, with a priority-inversion test that fails without it and passes with it.
4. **A tick-based sleep/delay API**, since tasks currently cannot sleep for a duration.

**Exit criterion:** a demo in which a high-priority task's measured worst-case latency stays bounded while lower-priority load runs, with the numbers recorded in the docs.

## Phase 4: MMU, caches and memory protection (L, optional)

Turning the MMU on gives stack guard pages and memory protection, and it makes exclusive-access spinlocks meaningful on Normal memory. It also forces a real design decision about DMA coherence (non-cacheable buffers versus explicit cache maintenance). It would close the current "validated only under TCG" caveat, but it is a large change, so treat it as its own project phase.

## Phase 5: Presentation (S–M)

- [ ] A one-page case study: architecture diagram, the three or four most instructive bugs, and the verification method.
- [ ] Link the design walkthrough and this roadmap from the README.
- [ ] If feasible with the custom QEMU build, a nightly local regression run.

## Recommended order

Phase 0 → Phase 1 (one focused session) → Phase 3 items 1–2 → Phase 2 as needed for latency → Phase 4 only if the memory-protection story is wanted.

Stopping after Phase 1 is also a valid, honest endpoint: the current state is coherent, and the README already states what is not covered.

## What each phase supports claiming

| After | Supportable claim |
|---|---|
| Now | SMP RTOS on AArch64 with per-CPU scheduling, cross-core wakeups, task migration, and static DMA IRQ routing, regression-tested at 30-run batches |
| Phase 1 | Above, plus the open reliability items closed or explicitly bounded |
| Phase 3 | Measured real-time properties: bounded latency, fixed-priority preemption, priority inheritance |
| Phase 4 | Memory protection and cache-coherent DMA |
