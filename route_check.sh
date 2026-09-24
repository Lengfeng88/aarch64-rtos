#!/bin/bash
# Post-hoc routing check over archived logs.
# Strips the UART's \r and only accepts fully printed "DMA IRQ cK=<digits>" lines,
# so a report cut off by `timeout` falls back to the previous complete one.
cd "$(dirname "$0")"
for spec in irq1_smp4:1 irq1_smp1:0 irq2_smp4:2 irq0_smp4:0 default_smp4:0; do
  tag=${spec%%:*}; exp=${spec##*:}
  n=0; noreport=0; notexp=0; leaked=0
  for L in stress_logs_irq/$tag/run_*.log; do
    [ -f "$L" ] || continue
    n=$((n+1))
    txt=$(tr -d '\r' < "$L")
    if ! grep -qE "DMA IRQ c0=[0-9]+" <<< "$txt"; then noreport=$((noreport+1)); continue; fi
    for k in 0 1 2 3; do
      v=$(grep -oE "DMA IRQ c$k=[0-9]+" <<< "$txt" | tail -1 | sed 's/.*=//')
      c[$k]=${v:-0}
    done
    [ "${c[$exp]}" -eq 0 ] && notexp=$((notexp+1))
    for k in 0 1 2 3; do
      if [ "$k" -ne "$exp" ] && [ "${c[$k]}" -ne 0 ]; then leaked=$((leaked+1)); break; fi
    done
  done
  echo "$tag (expect cpu$exp): runs=$n  no-report=$noreport  no-irq-on-expected-cpu=$notexp  leaked-to-other-cpu=$leaked"
done
