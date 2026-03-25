#!/usr/bin/env python3
import csv
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

TYPE_NAMES = {
    "0": "load",
    "1": "rfo",
    "2": "write",
    "3": "other",
}


def trace_tag_key(row: dict[str, str]) -> int:
    return int(row["trace_tag"])


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open() as f:
        return list(csv.DictReader(f))


def summarize(path: Path) -> None:
    rows = load_rows(path)
    by_trace_tag: dict[int, dict[str, int]] = defaultdict(dict)
    stage_rows: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        location = row["location"]
        stage = row["stage"]
        cycle = int(row["cycle"])
        full_stage = f"{location}:{stage}"
        trace_tag = trace_tag_key(row)
        if trace_tag != 0:
            if full_stage not in by_trace_tag[trace_tag] or cycle < by_trace_tag[trace_tag][full_stage]:
                by_trace_tag[trace_tag][full_stage] = cycle
        stage_rows[full_stage].append(row)

    ordered_stage_pairs = [
        ("node.0:node.request_issue", "pool.100:pool.request_accept", by_trace_tag),
        ("pool.100:pool.request_accept", "pool.100:pool.response_ready", by_trace_tag),
        ("pool.100:pool.response_ready", "pool.100:pool.response_send", by_trace_tag),
        ("pool.100:pool.response_send", "switch.port.pool.0:fabric.rx_enqueue", by_trace_tag),
        ("switch.port.pool.0:fabric.rx_enqueue", "switch.port.pool.0:fabric.ingress_release", by_trace_tag),
        ("switch.port.pool.0:fabric.ingress_release", "switch.port.node.0:fabric.tx_send", by_trace_tag),
        ("switch.port.node.0:fabric.tx_send", "node.0.remote_port:fabric.rx_enqueue", by_trace_tag),
        ("node.0.remote_port:fabric.rx_enqueue", "node.0.remote_port:fabric.ingress_release", by_trace_tag),
        ("node.0.remote_port:fabric.ingress_release", "node.0:node.response_receive", by_trace_tag),
    ]

    print(f"# {path}")
    for left, right, index in ordered_stage_pairs:
        samples = []
        for stages in index.values():
            if left in stages and right in stages:
                samples.append(stages[right] - stages[left])
        if not samples:
            continue
        print(
            f"{left} -> {right}: "
            f"count={len(samples)} avg={statistics.fmean(samples):.3f} "
            f"min={min(samples)} max={max(samples)}"
        )

    def gap_summary_values(rows_for_stage: list[dict[str, str]]) -> tuple[list[int], list[int]] | None:
        if len(rows_for_stage) < 2:
            return None
        gaps = [
            int(rows_for_stage[idx]["cycle"]) - int(rows_for_stage[idx - 1]["cycle"])
            for idx in range(1, len(rows_for_stage))
        ]
        pre_occ_values = [int(row["pre_queue_occ_bytes"]) for row in rows_for_stage]
        return gaps, pre_occ_values

    def print_gap_summary(stage_name: str) -> None:
        rows_for_stage = sorted(
            (row for row in stage_rows.get(stage_name, []) if trace_tag_key(row) != 0),
            key=lambda row: int(row["cycle"]),
        )
        summary = gap_summary_values(rows_for_stage)
        if summary is None:
            return
        gaps, pre_occ_values = summary
        gaps = [
            gap
            for gap in gaps
        ]
        mean = statistics.fmean(gaps)
        variance = statistics.fmean([(gap - mean) ** 2 for gap in gaps])
        print(
            f"{stage_name} gaps: "
            f"count={len(gaps)} avg={mean:.3f} stddev={math.sqrt(variance):.3f} "
            f"lt25_frac={sum(1 for gap in gaps if gap < 25) / len(gaps):.6f} "
            f"lt50_frac={sum(1 for gap in gaps if gap < 50) / len(gaps):.6f}"
        )
        print(
            f"{stage_name} pre_queue_occ_bytes: "
            f"avg={statistics.fmean(pre_occ_values):.3f} "
            f"nonzero_frac={sum(1 for value in pre_occ_values if value > 0) / len(pre_occ_values):.6f} "
            f"max={max(pre_occ_values)}"
        )

    def print_gap_summary_by_type(stage_name: str) -> None:
        rows_for_stage = [row for row in stage_rows.get(stage_name, []) if trace_tag_key(row) != 0]
        by_type: dict[str, list[dict[str, str]]] = defaultdict(list)
        for row in rows_for_stage:
            by_type[row["type"]].append(row)
        for type_id in sorted(by_type, key=int):
            rows_for_type = sorted(by_type[type_id], key=lambda row: int(row["cycle"]))
            summary = gap_summary_values(rows_for_type)
            if summary is None:
                continue
            gaps, _ = summary
            mean = statistics.fmean(gaps)
            variance = statistics.fmean([(gap - mean) ** 2 for gap in gaps])
            print(
                f"{stage_name} gaps[{TYPE_NAMES.get(type_id, type_id)}]: "
                f"count={len(gaps)} avg={mean:.3f} stddev={math.sqrt(variance):.3f} "
                f"lt25_frac={sum(1 for gap in gaps if gap < 25) / len(gaps):.6f} "
                f"lt50_frac={sum(1 for gap in gaps if gap < 50) / len(gaps):.6f}"
            )

    for stage_name in (
        "node.0.llc:llc.remote_candidate",
        "node.0.llc:llc.remote_mshr_merge",
        "node.0.llc:llc.remote_mshr_full",
        "node.0.llc:llc.remote_issue_success",
        "node.0.llc:llc.remote_issue_blocked",
        "node.0:node.request_issue",
        "pool.100:pool.request_accept",
        "pool.100:pool.response_ready",
        "pool.100:pool.response_send",
        "switch.port.pool.0:fabric.rx_enqueue",
    ):
        print_gap_summary(stage_name)
        print_gap_summary_by_type(stage_name)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: analyze_timeline.py <timeline.csv> [<timeline.csv> ...]", file=sys.stderr)
        return 1
    for arg in argv[1:]:
        summarize(Path(arg))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
