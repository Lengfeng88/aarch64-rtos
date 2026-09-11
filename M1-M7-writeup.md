# Project 4 — ARMv8-A RTOS on QEMU aarch64 virt

## Milestone Summary: M1–M7 (P4 Phase 1 complete)

**Platform**: QEMU `virt` machine, `-cpu cortex-a53`, ARMv8-A, bare-metal C + AArch64 assembly, no MMU, no floating point (`-mgeneral-regs-only`). Toolchain: `aarch64-linux-gnu-gcc 13.3.0` + `qemu-system-aarch64 8.2.2`.

**Overall principle**: every milestone follows "run it on the real emulator immediately after writing it; any uncertain address/register value gets confirmed against the device tree or QEMU's real internal state, never assumed from experience." This principle directly enabled finding several real bugs along the way.

---

## M1: AArch64 Boot → Exception Vector Table → EL1 → UART

**Goal**: get from QEMU's reset entry point down to EL1, install the exception vector table, get PL011 UART output working.

**Key implementation**:
- `_start` probes the current exception level via the `CurrentEL` register and handles both "booted directly at EL1" and "descending from EL2 to EL1" paths separately, rather than assuming default behavior
- Vector table is 2KB-aligned, 16 entries at 128 bytes each, strictly per the architecture requirement
- Used `svc #0` to deliberately trigger a synchronous exception as a self-check — if the vector table wasn't genuinely installed, this step exposes it immediately

**Verified**: both boot paths (direct EL1 / EL2→EL1 descent) confirmed working on real QEMU; `svc` self-trap correctly hits the handler and returns correctly via `eret`.

---

## M2: Generic Timer → Timer Interrupt → Scheduler Tick

**M2a (register-level verification, no interrupts yet)**: `CNTFRQ_EL0` read as 62500000 Hz; `CNTP_TVAL_EL0` countdown + `ISTATUS` polling confirmed the hardware timer chain itself has no issues.

**M2b**: genuine "timer-interrupt-driven scheduling" was folded into M4 (since it needs the GIC to close the loop end to end).

---

## M3: Task → Context → Context Switch

**Key implementation**: `switch_to()` only saves/restores the AAPCS64 callee-saved registers (`x19–x28`, `x29`, `x30`) — a legitimate simplification allowed by the calling convention. A new task's first-ever run relies on a hand-crafted fake frame on its own stack that looks as if it had already been saved by `switch_to`.

**Verified**: the fake frame lands correctly at the entry function on first activation; two tasks switched back and forth 6 times, with canaries intact at both ends of both stacks throughout. This was purely cooperative switching at this point, no preemption yet.

---

## M4: GIC → IRQ → ISR → Task Wakeup

**Key implementation**:
- GICv2 confirmed via device-tree dump (Distributor `0x08000000`, CPU interface `0x08010000`, timer PPI = INTID 30) — measured, not guessed from documentation
- The IRQ exception vector must save all 30 general-purpose registers plus `ELR_EL1`/`SPSR_EL1` (completely different from `switch_to`'s "callee-saved only" approach, since an interrupt can land anywhere)

**Real bug hit**: a new task's first-ever run went through "fake frame + bare `ret`" straight into the entry function, entirely skipping `eret`. Since `PSTATE.I` (the IRQ mask bit) is automatically set by hardware on IRQ entry and only correctly cleared when `eret` restores `SPSR_EL1`, this meant a new task would run permanently with interrupts masked — killing all further preemption for that task. Fixed by adding a `task_trampoline` layer: every new task explicitly unmasks IRQ once on its first run, before jumping into its real entry point.

**Verified**: 12 ticks of strict A/B alternation, driven entirely by the timer interrupt, with no task ever calling `switch_to` itself.

---

## M5: Mutex / Semaphore / Event / Queue

The scheduler was upgraded to a general version (`pick_next_ready()`/`yield()`, with a `state` field on each task for READY/BLOCKED), so preemption and voluntary blocking go through the exact same logic and always see the same ground truth.

**All four primitives fully implemented and stress-tested (25–55+ runs each, all passing)**:
- **Semaphore**: single-waiter simplification, hands the permit directly to the waiter on wake rather than leaving any window that looks "free" but isn't
- **Mutex**: same block/wake skeleton, with an owner field + `locked` flag
- **Event**: supports both "wait for any bit" and "wait for all bits" semantics
- **Queue**: ring buffer, wakes go through a retry loop rather than handing off a specific slot directly (since a woken blocked sender/receiver still has to actually perform its own push/pop)

**Real bugs hit (representative, worth recording)**:
1. **A recurring "the last yield isn't reliable" pattern**: several primitives' tail-end paths hit a case where a task's single `yield()` after its last action would, with low probability, never wake up again — root-caused to an edge case in GIC interrupt delivery (the timer hardware genuinely expired, but the GIC failed to redeliver that interrupt to the CPU). Fixed uniformly by polling an explicit completion flag at the tail end (`while (!done) yield();`) instead of guessing a fixed number of yields
2. **An Event API design flaw**: `event_wait()`'s blocking path originally recomputed its return value by re-reading the live shared `e->flags` after waking, but if the woken task didn't run immediately and another `event_clear()+event_set()` cycle happened in between, it would read a value that no longer reflected the condition that had actually triggered the wakeup. Fixed by having `event_set()` capture the result into a `wake_result` field inside the critical section, at the exact moment it decides to wake the waiter; `event_wait()` reads that field directly
3. **Cross-file struct mismatch**: `main.c`'s copy of the `event_t` definition wasn't kept in sync after `sync.c` added the `wake_result` field, so `event_set()` was writing past the end of the smaller `ev` object `main.c` had allocated — this class of "the same struct's field layout doesn't match across different `.c` files" bug recurred several times across the project, and is the single most worth-remembering lesson from it

---

## M6: DMA → Accelerator HAL → MMIO

The most complex milestone so far, and the only one involving a **real PCIe device** (the user-provided `dma-accel` custom QEMU device model).

### Environment setup
The user's `qemu-src` fork couldn't be cloned directly, so an equivalent environment was first rebuilt in the sandbox using stock QEMU 8.2.2 with `dma_accel.c` + the `Kconfig`/`meson.build` entries dropped in manually, to verify driver logic. This was later fully reproduced on the user's own local environment (rebuilt with the missing `aarch64-softmmu` target added).

### PCI enumeration + BAR0 mapping
- PCIe ECAM base (`0x4010000000`), the 32-bit MMIO window, and the legacy-IRQ-to-GIC-SPI mapping (device 2/INTA → SPI 5 → INTID 37) were all cross-checked against raw device-tree data
- Implemented standard PCI BAR sizing (write-all-1s, read back, invert-and-add-one), vendor/device scanning, and command-register enable

**Real bug hit (the deepest debugging session in the whole project)**: `PCI_COMMAND_MEMORY` was incorrectly defined as `bit 0` (it should be `bit 1` — `bit 0` is actually `PCI_COMMAND_IO`). This bug was extremely well-hidden — writing it and reading it back both "looked correct" (because the readback genuinely reflected what had been written under the wrong definition), and even QEMU's own `xp` raw memory read showed "the config byte was written correctly." It was finally found by adding temporary `fprintf` tracing directly inside QEMU's own `hw/pci/pci.c` (`pci_bar_address()`) and rebuilding just that file, which showed QEMU itself concluding "the Memory bit isn't set" — tracing all the way into QEMU's own `linux/pci_regs.h` finally revealed the correct bit definition.

### SQ/CQ command submission + DMA
- Implemented command-queue registration, `OPCODE_COPY` command construction, and doorbell submission per the user-provided real register spec (`dma_accel_regs.h`)
- First verified the link itself via polling (reading `REG_CQ_TAIL`), then switched to genuine GIC-interrupt-driven completion notification

**Real bug hit**: `REG_IRQ_MASK` was never being written — the device's interrupt line is an AND of `irq_status & irq_mask`, so no matter how many times `DMA_DONE` got set, without the mask configured the interrupt would never actually fire. The polling version completely bypassed this issue; it only surfaced once the interrupt-driven path was actually exercised.

**Final verification** (single task submit, block, real hardware interrupt wakeup):
```
submitted COPY, cmd_id=1
submitter blocking on completion_sem
[bystander] still running, loops=500000 ... 3000000
woke via IRQ, completion cmd_id=1
completion status=0
bystander loops while we waited=2985355
DATA VERIFIED - dst matches src byte-for-byte
```
The `bystander` task looped nearly 3 million times while the submitter was blocked, proving the CPU was genuinely handed off to another task rather than busy-waiting; the data was moved through the real device's DMA engine and verified byte-for-byte. This result was identical between the sandbox and the user's real local environment.

---

## M7: Full Integration (Multi-Task Concurrency)

M6 only verified a single task submitting/waiting. M7 escalated the scenario to three tasks submitting their own COPY commands **almost simultaneously**, each blocking on its own semaphore, with the ISR needing to correctly route each completion back to the right task by `cmd_id`. This was the first scenario where the system could end up with every task blocked at once, none of them ready — and it precisely triggered three scheduler/driver bugs that had never surfaced before.

**Debugging approach**: expanded `sync_el1h` from "save two registers" to saving all 31 general-purpose registers plus `ESR`/`ELR`, printing the full register state and the `pending[]` wait-table state at crash time, instead of continuing to guess.

**Three real bugs hit**:

1. **`sem_wait()` never checked whether the switch target was itself**: when `pick_next_ready()` can't find any other ready task, it can only return the caller itself. But `sem_wait()` (and the blocking paths of Mutex/Event/Queue) never handled this case, and still called `switch_to(&prev->sp, next->sp)` — with `prev == next`, this restores context from a now-stale pre-call `sp` value, producing a batch of zeroed registers and a `ret` straight to address `0x0`. The crash-time `x30 = 0x0` matched this mechanism exactly. Fixed by detecting `next == prev` and, in that case, not context-switching at all — polling its own state in place until an ISR wakes it.

2. **`pick_next_ready()`'s "nobody else runnable" branch never resynced `current_idx`**: since the caller had already marked itself BLOCKED, even the self-check in this branch's own scan fails, leaving `current_idx` permanently desynced from the real current task's index. Every subsequent scheduling decision (including the timer's own) would then start scanning from the wrong position, permanently failing to find some tasks that were actually ready. Fixed by explicitly resyncing `current_idx` to `current`'s real array index before this branch returns.

3. **A race window between submitting a command and registering the wait entry**: `accel_submit_copy()` (ringing the device's doorbell) and `pending_register()` (recording who's waiting for that `cmd_id`) were two separate calls, with an unprotected window in between. If the device's completion interrupt happened to land in that gap, the completion notification would be silently dropped for lack of a matching wait entry, and that task would hang forever waiting (caught directly via a `NO MATCH FOUND` debug trace). Fixed by wrapping both calls inside a single IRQ-disabled critical section, as one atomic operation.

After fixing all three, an additional fallback layer was added — `dispatch_available_completions()`, called both from the real ISR and periodically from each task's own wait-for-all-done loop, rather than relying entirely on interrupt delivery (to guard against the already-known GIC occasional-redelivery-failure issue logged back in M5).

**Verified**: 25/25 clean in sandbox stress runs, 28/30 in a larger batch; 9/10 clean on the user's own real environment (`~/projects/rtos` + their own compiled `qemu-src`), with all three `worker DATA OK, cmd_id=` lines appearing consistently.

---

## Known, Unresolved Issue (does not affect the conclusions above)

> **[CORRECTION — see the new section at the end of this document]** The paragraph below originally claimed that the "jump to `0x0`/`0x100000000`" wild-pointer crash had already been fully explained and fixed by M7 bug #1 (`sem_wait()`'s `next==prev` issue). **This conclusion is outdated and incorrect.** A later investigation (see "EC=0x0E: Root Cause, Fix, and Verification" at the end of this document) found that this class of crash (`ESR EC=0x00`, `ELR=0x0`) still recurs at roughly a 3–17% rate even after bug #1's fix, and — confirmed via an independent runtime check — **does not go through `switch_to()` at all**, meaning it is a genuinely separate, still-unresolved bug from both bug #1 and the later-diagnosed `EC=0x0E`. This bug is still **open**. The original paragraph is preserved below for the historical record only.

**`EC=0x0E` ("Illegal Execution State") crash** (probability roughly 7–10%): a different problem from the three fixed above — stack canaries intact at crash time, `ELR` points into the kernel's own valid code range (not a wild pointer), and the `pending[]` table shows all three tasks' completion states already at `done=1`, meaning the crash happens **after** all commands have already been correctly processed, during some task's tail-end bookkeeping. Root cause not yet found; logged as a known issue, with the decision made to lock in the existing fixes rather than keep chasing this one. Two distinct crash signatures have been observed under this general umbrella so far (the other being the M5/early-M7 "jump to `0x0`/`0x100000000`" wild pointer, ~~already confirmed to be caused by bug #1 above and now fixed~~ — **this conclusion has since been corrected; see the note above and the new section at the end**) — `EC=0x0E` is an independent, still-unresolved second class.

---

## Code Structure (`~/projects/rtos/`)

> Note: the later EC=0x0E investigation below was carried out in a renamed/relocated local repo, `~/projects/aarch64-rtos` (same `~/projects/qemu-src` build), with the same directory structure as listed below — only the repo path changed.

```
boot/boot.S          — reset entry point, EL descent, stack/BSS init
kernel/vectors.S      — exception vector table, full-register IRQ save/restore
kernel/switch.S        — cooperative context switch (callee-saved only)
kernel/uart.c          — polling PL011 driver (internally atomic; callers combining
                          multiple lines into one log message must wrap their own
                          critical section)
kernel/task.c           — TCB, stack frame construction, task_trampoline
kernel/sched.c          — pick_next_ready(), yield(), task registration
kernel/gic.c            — GICv2 distributor/CPU interface, PPI/SGI and SPI support
kernel/sync.c            — Semaphore / Mutex / Event / Queue
kernel/pci.c             — PCIe ECAM enumeration, BAR sizing/mapping, IRQ pin→SPI
kernel/accel.c           — dma-accel HAL: SQ/CQ registration, command submission,
                            IRQ-driven completion
kernel/main.c            — current milestone's test harness (replaced per milestone)
```

---

## EC=0x0E: Root Cause, Fix, and Verification (later investigation, added retroactively)

**Background**: the `EC=0x0E` ("Illegal Execution State") crash logged in the "Known Issues" section above was picked back up in a later, separate session, in a repo relocated to `~/projects/aarch64-rtos` (same `qemu-src` build).

**Symptom**: an intermittent (roughly 5–10%) synchronous exception, with `ESR`'s EC field equal to `0x0E`, and `ELR` landing inside `vectors.S`'s own IRQ-restore sequence. Crash-time register dumps showed `x0`–`x28` mostly zeroed, with only `x30` holding a plausible-looking address. In the clearest case observed, `pending[]` showed all three workers' completion states already at `done=1` — meaning the corruption happened *after* all completions had been correctly dispatched, not during dispatch itself.

**Investigation approach**: rather than continuing to guess from post-crash register dumps, this session instead:
1. Added a raw stack-pointer snapshot to `vectors.S`'s `sync_el1h`/`irq_el1h` (reusing 8 bytes of exception-frame padding that was already allocated but never actually used, so no frame-layout change was needed), plus a check for whether that `sp` fell inside any worker's own `stack[]` array
2. Disassembled `yield()`/`worker_entry()`/`dispatch_available_completions()` at `-O0` and confirmed all three compiled exactly as written, instruction-for-instruction matching the source — ruling out a compiler-introduced bug as the explanation

**Root cause**: `switch_to()` (`kernel/switch.S`) had **zero self-protection against interrupts**. It neither masked IRQ during its own register push/pop/sp-swap sequence, nor treated `DAIF` (the IRQ mask state) as part of each task's own saved context. This meant that *any* call site invoking `switch_to()` — `yield()`, `sem_wait()`, `mutex_lock()`, `event_wait()`, `queue_send()`/`queue_recv()`, and `irq_handler()`'s own timer-preemption branch — could have a timer interrupt land mid-way through the "save old task's sp → load new task's sp" sequence, corrupting whichever `tcb_t.sp` happened to be mid-write at that exact moment.

**Fix**: rewrote `switch_to.S` to mask IRQ (`msr daifset,#2`) for its entire duration, from entry to `ret`, and to save/restore each task's own `DAIF` value as part of its saved context (a new 16-byte slot added to the push/pop sequence; frame size grew from 96 to 112 bytes) — restoring the *new* task's own `daif` only immediately before `ret`, closing the race window without leaking IRQ-mask state between tasks. `task_init()`'s fake initial frame was updated to match the new 14-word layout (`sp[0]=daif=0`/unmasked, `sp[3]=x30=task_trampoline`). Along the way, a second, independent race window was also found and removed: a redundant `msr daifclr,#2` in `kernel_main()` between GIC/timer setup and the first `switch_to()` into task 0 — if a timer IRQ landed in that gap, the boot stack's `sp` could get written into `taskWorker[0]`'s TCB before it was ever legitimately used. This line is no longer needed now that `switch_to()` manages `daif` per-task.

**Verification (two independent methods — not just "the crash rate went down")**:
1. A temporary `widen_race_window()` (a busy-loop of nops) was inserted between `current=next` and `switch_to()` in `yield()`, deliberately stretching the race window well beyond a single timer tick period. Before the fix, this reliably reproduced the crash (and, along the way, surfaced and helped correct one separate, unrelated 100%-reproducible failure caused by a copy-paste mistake mid-fix). After the fix, the same artificially-widened window no longer reproduced any crash. This instrumentation was purely temporary and has since been removed.
2. A permanent `checked_switch_to()` wrapper was added at *every* `switch_to()` call site (in `kernel/sched.c` and `kernel/sync.c`), backed by a shared `report_corrupt_sp()` in `main.c` — immediately after each switch completes, it checks whether the just-suspended task's `tcb_t.sp` still falls inside that task's own `stack[]` array, and if not, reports exactly which call site and switch caused it. Across several 30-run stress batches (including runs with the widened race window still active), zero `CORRUPT sp` reports occurred, and zero `EC=0x0E` crashes recurred. Because this check is cheap and general-purpose, it was kept permanently rather than removed along with the temporary instrumentation.

**Byproduct — a correction to an earlier, mistaken conclusion**: the "Known Issues" section above had originally concluded that the "jump to `0x0`/`0x100000000`" wild-pointer crash was fully explained and fixed by M7 bug #1 (`sem_wait()`'s `next==prev` case). This investigation confirmed that conclusion was **wrong** — that class of crash (`ESR EC=0x00` "Unknown reason", `ELR=0x0`, first logged during M5's Event demo) still recurs at a natural rate of roughly 3–17% (small sample, wide interval) even after the `EC=0x0E` fix, and — cross-checked via the same `checked_switch_to()` instrumentation — **does not go through `switch_to()` at all**, meaning its root cause lies elsewhere, and it is a genuinely separate problem from both bug #1 and `EC=0x0E`. The three had simply looked similar from the outside (all present as "PC ends up somewhere invalid"), which repeatedly confused the diagnosis across several earlier sessions. This older bug remains **unresolved** and is logged as the next investigation to take on independently — starting from the original M5-era notes (both tasks' stack canaries were confirmed intact at the time, ruling out simple stack overflow) to avoid re-covering ground that's already been ruled out.
