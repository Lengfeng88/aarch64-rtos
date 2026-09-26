#!/usr/bin/env bash
# tools/regress.sh — single-command regression runner for the P4 AArch64 RTOS.
#
# Replaces stress_test.sh + stress_irq.sh + route_check.sh with one script.
# A run is "clean" iff it printed 3x "DATA OK" and no "SYNC EXCEPTION" /
# "CORRUPT" / "CAUGHT" marker. If DMA_IRQ_CPU is set, each run is also
# checked for correct IRQ routing (mirrors route_check.sh's logic).
#
# Usage:
#   tools/regress.sh <elf> <smp> <tag> [runs] [DMA_IRQ_CPU]
#
# Examples:
#   tools/regress.sh build/kernel.elf 4 p4-m12-step3            # 30 runs, no IRQ check
#   tools/regress.sh build/kernel_irq1.elf 4 irq1_smp4 30 1      # + expects DMA IRQ on cpu1

set -u

ELF="${1:?usage: regress.sh <elf> <smp> <tag> [runs] [DMA_IRQ_CPU]}"
SMP="${2:?missing -smp value}"
TAG="${3:?missing tag}"
RUNS="${4:-30}"
EXPECT_CPU="${5:-}"

QEMU=/home/kelvin/projects/qemu-src/build/qemu-system-aarch64
LOGDIR="stress_logs_${TAG}"
TIMEOUT_S=40

mkdir -p "$LOGDIR"

clean=0
bad=0
no_report=0
misrouted=0
leaked=0
routed_ok=0

for i in $(seq 1 "$RUNS"); do
  log="$LOGDIR/run_${i}.log"
  # Run under `script` (pty) so QEMU's stdout isn't fully block-buffered;
  # a plain redirect loses everything if `timeout` SIGTERMs before a flush
  # (this is why the project's original stress_test.sh used `script`).
  script -qc "timeout ${TIMEOUT_S}s $QEMU -M virt -cpu cortex-a53 -nographic -device dma-accel -kernel $ELF -smp $SMP" "$log" >/dev/null

  # normalize: strip \r that corrupted stress_irq.sh's old inline metrics
  sed -i 's/\r$//' "$log"

  data_ok=$(grep -c 'DATA OK' "$log")
  bad_marker=$(grep -Ec 'SYNC EXCEPTION|CORRUPT|CAUGHT' "$log")

  if [ "$data_ok" -ge 3 ] && [ "$bad_marker" -eq 0 ]; then
    clean=$((clean + 1))
  else
    bad=$((bad + 1))
    echo "  [bad] run $i: DATA OK x$data_ok, markers=$bad_marker ($log)"
  fi

  if [ -n "$EXPECT_CPU" ]; then
    # skip route check on truncated (timeout-cut) report lines
    if ! grep -q 'DMA IRQ' "$log"; then
      no_report=$((no_report + 1))
      continue
    fi
    if grep -qE "DMA IRQ.*cpu${EXPECT_CPU}\b" "$log"; then
      routed_ok=$((routed_ok + 1))
    else
      misrouted=$((misrouted + 1))
    fi
    # any nonzero count on an unexpected cpu = leak
    for c in 0 1 2 3; do
      [ "$c" = "$EXPECT_CPU" ] && continue
      if grep -qE "cpu${c}=[1-9]" "$log"; then
        leaked=$((leaked + 1))
        break
      fi
    done
  fi
done

{
  echo "tag=$TAG elf=$ELF smp=$SMP runs=$RUNS"
  echo "clean=$clean/$RUNS bad=$bad"
  if [ -n "$EXPECT_CPU" ]; then
    echo "routing: expect_cpu=$EXPECT_CPU routed_ok=$routed_ok misrouted=$misrouted leaked=$leaked no_report=$no_report"
  fi
} | tee "$LOGDIR/summary.txt"
