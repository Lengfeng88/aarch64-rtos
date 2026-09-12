# M8 RL Scheduling — Feasibility Note

**Status:** Design / feasibility evaluation only. No kernel changes. No RL code written yet.
**Scope boundary:** Everything below happens in a standalone Python simulation. Nothing in this note is implemented in the RTOS kernel (`~/projects/aarch64-rtos`).

---

## 1. Goal

Evaluate whether a learned scheduling policy could improve on the scheduler behavior already implemented and verified in M8 (round-robin baseline, EWMA-based load-aware heuristic), using only the state actually observable in the current system — not a hypothetical richer state space.

This is explicitly a **research spike**, not a claim of "implemented an RL scheduler." The outcome may be "RL isn't warranted yet" — that is itself a valid, useful result.

## 2. Current System Boundary

- The RL environment is a **Python simulation** of scheduling decisions, built around the same state/action shape the real M8 kernel scheduler already uses.
- It does **not** touch `kernel/sched.c`, `kernel/sync.c`, `kernel/main.c`, or `kernel/task.c`.
- No new `tcb_t` fields, no new kernel milestones, no new stress-test risk to the timing-sensitive `switch_to()` path.
- If this spike later justifies moving further, the natural next step (not part of this note) would be distilling a learned policy back into the kernel's existing `sched_policy_t` interface for direct comparison against `policy_roundrobin` / `policy_load_aware` — but that is a future decision, not an assumption baked into this design.

## 3. Available State

Only fields that exist and have been verified in the real M8 implementation:

| Field | Source | Verified? |
|---|---|---|
| `ewma_load` | `tcb_t.ewma_load`, updated by `ewma_on_tick()` | Yes — busy_task converges to ~997/1000, workers observed at 0/300/510 |
| `state` (READY=0 / BLOCKED=1) | `tcb_t.state` | Yes — existing scheduler semantics, exercised since M4/M5 |
| recent scheduling history | `select_count[]` (added this session via `sched_debug_select_count()`) | Yes — confirmed distinguishing busy task (thousands of selections) from completed/blocked workers (frozen low counts) |

**Not included:** task priority, deadline, remaining work, arrival/completion timestamps. None of these exist in the current `tcb_t` or scheduler. See Section 8.

### Note on `select_count`

`select_count_i` is a raw service-history counter, not a derived fairness label. It should not be called "starvation" directly — starvation is a judgment about outcome, not a raw observation. From a windowed version of `select_count_i(window)`, a fairness metric can be derived, e.g.:

```
fairness_imbalance = variance(select_count over window)
```

or a normalized index such as Jain's fairness index. This keeps the observed quantity (service history) and the property being optimized for (fairness) distinct.

## 4. Action Space

- Discrete: choose the next READY task to run.
- Action count = number of currently-READY candidate tasks (matches the real scheduler's `select_next()` contract — it also only chooses among `state == 0` tasks).
- No change to this shape is needed to stay aligned with the real system.

## 5. Reward Candidates

Split explicitly into what can be computed today vs. what would require state the system doesn't have yet.

**Computable now, from real M8 data:**
- CPU utilization (busy vs. idle time, derivable from `select_count` and tick counts)
- Fairness (variance or Jain's index over `select_count`)

Candidate reward function for the initial spike:

```
R = α × CPU_utilization − β × fairness_imbalance
```

**Not computable now — do not fake these:**
- Deadline miss rate — no deadline concept exists anywhere in the scheduler or `tcb_t`.
- Response latency — no explicit task arrival/completion timestamp model exists; DMA completion timing exists in principle (IRQ-driven) but isn't currently captured as a per-task latency metric.

These stay listed as future extensions (Section 8), not as placeholder/synthetic values in the reward function. Inventing numbers for them would make the simulation's results unfalsifiable against the real system.

## 6. Baselines

The comparison this spike is actually trying to run:

- **Round Robin** — `roundrobin_select_next()`, already implemented and stress-tested (~10% real-failure baseline established across multiple 30-run batches, unrelated to this RL work).
- **EWMA-based load-aware heuristic** — `load_aware_select_next()`, already implemented and stress-tested (~10% baseline, same order as RR — no regression attributed to it).

Both baselines already exist as real, verified kernel code. The RL policy is evaluated against them in simulation using the same state fields listed in Section 3.

## 7. RL Extension

The learned policy is the **last** thing this note is about, not the center of it. The actual research question is:

> Can a learned scheduling policy improve upon the existing RR and EWMA-based load-aware heuristic, under the *same* observable state (`ewma_load`, `state`, service history) already available in the real system?

Valid outcomes, all useful:

- `RL > heuristics` — learned policy finds structure the heuristics miss. Worth a deeper writeup.
- `RL ≈ heuristics` — the heuristic already captures what's learnable from this state. Still a legitimate, reportable result.
- `RL < heuristics` — most likely explanation: at this state dimensionality (`ewma_load` + `state` + service history only), there isn't enough signal for a learned policy to beat a simple, well-tuned heuristic. This is not a failure of the spike; it's information about how much the current state space actually supports.

No algorithm choice is fixed yet (tabular Q-learning / contextual bandit are the lightest options consistent with the low-dimensional state above); that decision is deferred until after the state/action/reward design here is validated against real M8 data traces.

## 8. Future State Extensions (explicitly NOT part of the current spike)

These would make the RL problem richer and are natural next steps *if* this spike's results justify continuing — but none of them exist in the system today, and none should be assumed as available state in Sections 3–5 above:

- **Priority** — would require a new `tcb_t` field and changes to `select_next()`'s candidate-selection logic in both `roundrobin_select_next()` and `load_aware_select_next()`.
- **Deadline / deadline slack** — would require a deadline concept that doesn't exist anywhere in the current scheduler.
- **Remaining work** — would require a work-quantity model; the current demo workload (submit one DMA, wait, verify) doesn't have a notion of partial progress.
- **Explicit arrival/completion timestamps** — would require a timestamped event model beyond the current UART-log-based observation approach, to support real latency/deadline-miss reward terms.

Adding any of these means touching the kernel again (new fields synchronized across all four `tcb_t` definitions, updated scheduling logic, a fresh stress-test regression pass) — i.e., it re-enters kernel work with the same risk profile as the M8 changes already made this session. That is a deliberate future decision, not a default extension of this spike.

## 9. Go / No-Go Criteria for This Spike

Proceed past the design stage only if, once real M8 log data (EWMA traces, `select_count` traces from stress runs) is fed into the Section 3 state definition:

- The state actually discriminates between task behaviors in a way a policy could exploit (i.e., `ewma_load` + `state` + service history aren't degenerate/constant across the real traces).
- A reward computed from Section 5's computable terms produces a meaningful, non-trivial signal (not flat, not dominated by noise from the same ~10% crash-rate variance already characterized in the M8 stress-test investigation).

If either check fails, stop here. The existing M8 deliverable (policy interface, verified EWMA predictor, load-aware heuristic, Python benchmarking/analysis tooling) already stands on its own regardless of this spike's outcome.
