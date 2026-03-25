#!/usr/bin/env python3
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def key_for(row: dict[str, str]) -> int:
    instr_id = int(row["instr_id"])
    if instr_id != 0:
        return instr_id
    return int(row["trace_tag"])


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open() as f:
        return list(csv.DictReader(f))


def summarize(path: Path) -> None:
    rows = load_rows(path)
    by_key: dict[int, dict[str, int]] = defaultdict(dict)
    for row in rows:
        key = key_for(row)
        location = row["location"]
        stage = row["stage"]
        cycle = int(row["cycle"])
        full_stage = f"{location}:{stage}"
        if full_stage not in by_key[key] or cycle < by_key[key][full_stage]:
            by_key[key][full_stage] = cycle

    ordered_stage_names = [
        "node.0:node.request_issue",
        "pool.100:pool.request_accept",
        "pool.100:pool.response_ready",
        "pool.100:pool.response_send",
        "switch.port.pool.0:fabric.rx_enqueue",
        "switch.port.pool.0:fabric.ingress_release",
        "switch.port.node.0:fabric.tx_send",
        "node.0.remote_port:fabric.rx_enqueue",
        "node.0.remote_port:fabric.ingress_release",
        "node.0:node.response_receive",
    ]

    print(f"# {path}")
    for left, right in zip(ordered_stage_names, ordered_stage_names[1:]):
        samples = []
        for stages in by_key.values():
            if left in stages and right in stages:
                samples.append(stages[right] - stages[left])
        if not samples:
            continue
        print(
            f"{left} -> {right}: "
            f"count={len(samples)} avg={statistics.fmean(samples):.3f} "
            f"min={min(samples)} max={max(samples)}"
        )


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: analyze_timeline.py <timeline.csv> [<timeline.csv> ...]", file=sys.stderr)
        return 1
    for arg in argv[1:]:
        summarize(Path(arg))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
