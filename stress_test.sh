#!/bin/bash
# stress_test.sh - run the M7 3-worker demo N times, count crashes.
# Uses the exact same QEMU binary + flags as `make run`.
#
# IMPORTANT: QEMU's -nographic stdio is line-buffered when attached to
# a real tty, but switches to full buffering when stdout is redirected
# to a plain file. `timeout` kills it with SIGTERM, and anything still
# sitting in that buffer never gets flushed - so a plain
# `qemu ... > file 2>&1` under timeout can produce an EMPTY log even on
# a run that completed fine. `script` fakes a pty so QEMU keeps
# line-buffering, avoiding this.
set -u

QEMU=/home/kelvin/projects/qemu-src/build/qemu-system-aarch64
KERNEL=build/kernel.elf
N=30
CRASH_COUNT=0
CLEAN_COUNT=0
UNCLEAR_COUNT=0
LOGDIR=stress_logs
mkdir -p "$LOGDIR"

for i in $(seq 1 "$N"); do
    echo "=== Run $i/$N ==="
    script -qec "timeout 40 $QEMU -M virt -cpu cortex-a53 -nographic -device dma-accel -kernel $KERNEL" \
        "$LOGDIR/run_$i.log" < /dev/null > /dev/null 2>&1

    if grep -q "SYNC EXCEPTION" "$LOGDIR/run_$i.log"; then
        CRASH_COUNT=$((CRASH_COUNT + 1))
        echo "  -> CRASHED"
        grep "SYNC EXCEPTION" "$LOGDIR/run_$i.log"
    elif [ "$(grep -c 'DATA OK' "$LOGDIR/run_$i.log")" -eq 3 ]; then
        CLEAN_COUNT=$((CLEAN_COUNT + 1))
        echo "  -> clean (3/3 workers OK)"
    else
        UNCLEAR_COUNT=$((UNCLEAR_COUNT + 1))
        echo "  -> UNCLEAR (check log manually: $LOGDIR/run_$i.log)"
    fi
done

echo ""
echo "=== Summary ==="
echo "Total runs:  $N"
echo "Crashed:     $CRASH_COUNT"
echo "Clean (3/3): $CLEAN_COUNT"
echo "Unclear:     $UNCLEAR_COUNT"
