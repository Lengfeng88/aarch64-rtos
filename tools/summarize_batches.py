# tools/summarize_batches.py
import re
import sys
from pathlib import Path
from parse_log import classify_log   # 复用你已经写好的分类函数


def summarize_one_batch(log_dir: Path) -> dict:
    logs = sorted(log_dir.glob("run_*.log"))
    counts = {"clean": 0, "crash": 0, "hang": 0, "unknown": 0}
    for log_path in logs:
        r = classify_log(log_path.read_text())
        counts[r["outcome"]] += 1
    total = len(logs)
    real_failures = counts["crash"] + counts["hang"]
    return {
        "batch": log_dir.name,
        "total": total,
        "clean": counts["clean"],
        "crash": counts["crash"],
        "hang": counts["hang"],
        "unknown": counts["unknown"],
        "failure_rate": round(100 * real_failures / total, 1) if total else 0,
    }


def main():
    # 用法: python3 summarize_batches.py stress_logs_m8_policy stress_logs_m8_ewma stress_logs_m8_loadaware
    batch_dirs = [Path(p) for p in sys.argv[1:]]
    rows = [summarize_one_batch(d) for d in batch_dirs]

    print(f"{'Batch':<30} {'Total':<7} {'Clean':<7} {'Crash':<7} {'Hang':<7} {'Fail%':<7}")
    for r in rows:
        print(f"{r['batch']:<30} {r['total']:<7} {r['clean']:<7} {r['crash']:<7} {r['hang']:<7} {r['failure_rate']:<7}")


if __name__ == "__main__":
    main()