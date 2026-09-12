import re
import sys
from pathlib import Path

def classify_log(text: str) -> dict:
    result = {
        "outcome": None,       # "clean" | "crash" | "hang"
        "esr": None,
        "elr": None,
        "sp": None,
        "submitted_cmd_ids": [],
        "data_ok_cmd_ids": [],
        "corrupt_sp_addr": None,
    }

    result["submitted_cmd_ids"] = [
        int(m) for m in re.findall(r"worker submitted cmd_id=(\d+)", text)
    ]
    result["data_ok_cmd_ids"] = [
        int(m) for m in re.findall(r"worker DATA OK, cmd_id=(\d+)", text)
    ]

    esr_match = re.search(r"SYNC EXCEPTION ESR=0x([0-9a-fA-F]+)", text)
    elr_match = re.search(r"SYNC EXCEPTION ELR=0x([0-9a-fA-F]+)", text)
    sp_match = re.search(r"SYNC EXCEPTION SP=0x([0-9a-fA-F]+)", text)
    corrupt_match = re.search(r"CORRUPT sp after \S+ switch_to, (0x[0-9a-fA-F]+)", text)

    if esr_match:
        result["outcome"] = "crash"
        result["esr"] = esr_match.group(1)
        result["elr"] = elr_match.group(1) if elr_match else None
        result["sp"] = sp_match.group(1) if sp_match else None
    elif corrupt_match:
        # 这类hang之前被stress_test.sh的正则误判成UNCLEAR，
        # 实际是真实失败：打印一行就卡进wfe死循环，被40s timeout杀掉
        result["outcome"] = "hang"
        result["corrupt_sp_addr"] = corrupt_match.group(1)
    elif len(result["data_ok_cmd_ids"]) == 3:
        result["outcome"] = "clean"
    else:
        result["outcome"] = "unknown"   # 真正意义不明的情况，需要人工看

    return result


def main():
    log_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "stress_logs")
    logs = sorted(log_dir.glob("run_*.log"), key=lambda p: int(re.search(r"\d+", p.stem).group()))

    counts = {"clean": 0, "crash": 0, "hang": 0, "unknown": 0}
    crash_signatures = []
    hang_addrs = []

    for log_path in logs:
        text = log_path.read_text()
        r = classify_log(text)
        counts[r["outcome"]] += 1
        if r["outcome"] == "crash":
            crash_signatures.append((log_path.name, r["esr"], r["elr"], r["sp"]))
        elif r["outcome"] == "hang":
            hang_addrs.append((log_path.name, r["corrupt_sp_addr"]))

    total = len(logs)
    real_failures = counts["crash"] + counts["hang"]
    print(f"Total runs: {total}")
    print(f"Clean: {counts['clean']}")
    print(f"Crash (formal ESR dump): {counts['crash']}")
    print(f"Hang (CORRUPT sp, timeout-killed): {counts['hang']}")
    print(f"Unknown (needs manual check): {counts['unknown']}")
    print(f"Real failure rate: {real_failures}/{total} ({100*real_failures/total:.1f}%)")

    if crash_signatures:
        print("\nCrash signatures (ESR/ELR/SP):")
        for name, esr, elr, sp in crash_signatures:
            print(f"  {name}: ESR=0x{esr} ELR=0x{elr} SP=0x{sp}")

    if hang_addrs:
        print("\nHang addresses:")
        for name, addr in hang_addrs:
            print(f"  {name}: {addr}")


if __name__ == "__main__":
    main()
