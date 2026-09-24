#!/bin/bash
# usage: stress_irq.sh <elf> <smp> <tag> [runs=30]   (routing check: ./route_check.sh)
ELF=$1; SMP=$2; TAG=$3; N=${4:-30}
QEMU=/home/kelvin/projects/qemu-src/build/qemu-system-aarch64
D=stress_logs_irq/$TAG; mkdir -p $D
clean=0
for i in $(seq 1 $N); do
  L=$D/run_$i.log
  timeout 40 $QEMU -M virt -cpu cortex-a53 -smp $SMP -nographic -device dma-accel -kernel $ELF > $L 2>&1
  ok=$(grep -c 'DATA OK' $L)
  bad=$(grep -cE 'SYNC EXCEPTION|CORRUPT' $L)
  if [ "$ok" -eq 3 ] && [ "$bad" -eq 0 ]; then clean=$((clean+1)); else echo "run $i FAIL: DATA OK=$ok markers=$bad"; fi
done
echo "$TAG: clean=$clean/$N"
