# EC=0x00 / ELR=0 Wild-Jump Investigation — Extensive, Inconclusive

**Status: known issue, unresolved. Not a regression of the fixed EC=0x0E bug — confirmed independent.**

---

## Background

After `EC=0x0E` ("Illegal Execution State") was root-caused and fixed (see
`EC-0x0E-writeup.md` / the corrected section of `M1-M7-writeup-en.md`), a
separate, pre-existing crash remained: `ESR=0x02000000` (`EC=0x00`, "Unknown
reason"), `ELR=0x0`. First logged during M5's Event demo, confirmed via
`checked_switch_to()` instrumentation to **never** go through `switch_to()`
at all — meaning its root cause lies somewhere else in the scheduler/context
path. This document records a full investigation session that produced solid
new evidence but did not reach a confirmed root cause.

## Tooling added

- **`stress_test.sh`**: runs the M7 3-worker demo N times (default 30) via
  `script` (to avoid QEMU's stdio buffering under `timeout`), classifies each
  run clean / crashed / unclear, logs everything under `stress_logs/`.
- **`trace_switch_resume()`** (`kernel/main.c` + two-line hook in
  `kernel/switch.S`, right after `mov sp, x1` and before any register pops):
  reads the about-to-be-restored frame's `x30` slot and only prints the full
  14-word frame if that value is below `0x40000000` (i.e. clearly not a
  valid code/data address in this kernel). Filtering is necessary — an
  earlier unfiltered version printed on every switch and caused a genuine
  livelock (UART output while IRQ is masked stretched every switch enough to
  change the race itself).

## Dead ends ruled out (in order)

1. **`vectors.S`'s `orig_sp` diagnostic field was never actually written.**
   The exception frame had an allocated-but-unfilled 8-byte slot at offset
   264; all "SP=0x40001720-range" values reported earlier in this
   investigation were stale leftover stack bytes, not real captured SP.
   Fixed with `add x2, sp, #272; str x2, [sp, #264]`. This invalidated a
   round of prior analysis that had been built on those values.

2. **"`current = next` runs before `switch_to()`'s own IRQ masking" theory**
   (all six blocking-primitive call sites — `yield`, `sem_wait`,
   `mutex_lock`, `event_wait`, `queue_send`, `queue_recv` — share this
   shape). Tried moving `irq_restore()` to after `current = next` in all
   six. Made things measurably **worse**: crash+unclear rate rose from
   ~2/30 to ~18/30, including genuine livelocks with zero worker
   submissions. Fully reverted.

3. **"`checked_switch_to()`'s post-switch bounds check runs with IRQ open"
   theory** ("window 3"). Tried in isolation. First batch: 30/30 clean.
   Three more batches: back to ~4-7% crashes. Not sufficient alone, though a
   marginal effect can't be ruled out from this sample size.

4. **"A `tcb_t*` is being mistaken for its own `->sp` value" theory.** Added
   a `next->sp` pre-switch bounds check to test this directly. The check
   **never fired** across a 30-run batch — theory disproven. Notably, adding
   this check (even though it never triggered) raised the crash rate to
   7/30, confirming the bug is extremely sensitive to the exact instruction
   count/timing on the `switch_to()` call path. Check removed again.

## What the evidence actually shows

With the filtered `trace_switch_resume()` + window-3 fix in place (current
repo state), four 30-run baseline batches (120 runs total) gave:

- **~5/120 (~4%) genuine crashes** — mostly `EC=0x00`/`ELR=0`, one `EC=0x0E`
  recurrence (see note below)
- **2/120 `"CORRUPT sp after irq_handler switch_to"`** hits — concluded to
  be a downstream victim of the same race rather than an independent bug
  (that code path runs fully IRQ-masked and can't itself be preempted
  mid-execution)
- A handful of `stress_test.sh` misclassifications (`grep -c 'DATA OK' -eq 3`
  sometimes doesn't match runs that manually verified as fully correct) —
  a test-script bug, not a kernel issue, not yet fixed

**One EC=0x0E recurrence** (`ESR=0x3a000000`) appeared in the 120 runs. This
sits within EC=0x0E's own historical 7-10% natural rate, and no changes were
made to `switch_to.S` in this session — treated as likely statistical
overlap, not a regression. Worth watching if it recurs in future batches.

**Two new, reproducible patterns** in the about-to-be-restored `x30` slot
across independent crashes:

1. **Exactly `0x3c0`** — the precise ARMv8 DAIF-all-masked encoding (bits
   9:6 all set). Seen in two separate crashes, once with the adjacent `x27`
   register holding the identical value too.
2. **Exactly equal to a worker's own `tcb_t` base address**
   (`&taskWorker[i]`, which equals `&taskWorker[i].sp` since `sp` is the
   struct's first field) — seen for all three different workers across
   different runs, always paired with `SP` correctly falling in that same
   worker's own `stack[]` range.

Both patterns are significant precisely because the substituted value is
**not random garbage** — it's a value that legitimately exists elsewhere in
the system (a valid DAIF encoding, or a real pointer). That points to a
genuine value-substitution bug in the context-switch path, not memory
corruption from an unrelated source.

`switch_to.S` was reviewed twice, line by line, specifically tracing
`x0`/`x1`/`x30` through the `trace_switch_resume` call, looking for a leak
mechanism for the TCB-base pattern. No conclusive mechanism was found — the
`bl trace_switch_resume` does clobber `x30` per AAPCS64, but the very next
instruction (`ldp x29, x30, [sp], #16`) unconditionally overwrites it from
the frame before any further use, so this specific call isn't the leak path.

## A note on an external review

A review document (not produced in this investigation) proposed that the
real root cause was architectural — `irq_handler()` calling `switch_to()`
directly instead of a unified exception-frame-based scheduler with an idle
task — and framed this as the explanation for "EC=0x0E". That framing is
**factually incorrect**: it conflates this investigation (`EC=0x00`/`ELR=0`)
with `EC=0x0E`, which is a different, already-fixed bug (see above). The
review's general hardening suggestions (missing `SPSR_EL1` dump, no idle
task, mixed cooperative/exception-frame switching) are a legitimate
direction for a future architectural pass, but are **not verified** as the
cause of the specific crashes chased here, and were not adopted mid-
investigation for that reason — logged as a candidate for M8 / Phase 2
instead.

## Conclusion

Investigation concluded without a confirmed root cause, due to time
invested rather than the trail going cold. Logged as a known,
**unresolved ~3-7% crash-rate scheduler context-switch race**, confirmed
independent of the fixed `EC=0x0E`. All temporary check-only diagnostics
were removed; the low-overhead filtered `trace_switch_resume()` is kept
permanently as a low-cost sanity check, the same way `checked_switch_to()`
was kept after the `EC=0x0E` investigation closed.

**Future entry points**, if picked back up:
- Chase the two `x30`-substitution patterns (`0x3c0` / TCB-base) — they are
  reproducible and specific enough to eventually triangulate a mechanism.
- Consider the unified exception-frame + idle-task + SVC-scheduler rewrite
  as a clean-slate architectural fix, but treat it as a new investigation
  (it invalidates the evidence gathered here) rather than a patch on top of
  the current architecture.
