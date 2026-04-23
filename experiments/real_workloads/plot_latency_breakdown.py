#!/usr/bin/env python3
import csv
import os
import re
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
POOL_LOG_DIR = Path(os.environ.get("POOL_LOG_DIR", str(SCRIPT_DIR / "logs_switch_pool")))
REPLICATION_LOG_DIR = Path(os.environ.get("REPLICATION_LOG_DIR", str(SCRIPT_DIR / "logs_switch_replication")))
OUT_PREFIX = Path(os.environ.get("OUT_PREFIX", str(SCRIPT_DIR / "real_workload_latency_breakdown")))
OUT_CSV = OUT_PREFIX.with_suffix(".csv")
OUT_PNG = OUT_PREFIX.with_suffix(".png")
OUT_PDF = OUT_PREFIX.with_suffix(".pdf")
FIXED_ONCHIP_CYCLES = float(os.environ.get("FIXED_ONCHIP_CYCLES", "25.0"))

STAT_RE = re.compile(r"^(stat\.[^=]+?)\s*=\s*([0-9.eE+-]+)\s*$", re.MULTILINE)
TRACE_RE = re.compile(r"^# TRACE_PATH:\s*(.+)\s*$", re.MULTILINE)
WORKLOAD_RE = re.compile(r"^(?P<id>\d+)\.(?P<name>[A-Za-z0-9]+)")
NODE_ID_RE = re.compile(r"^stat\.node\.(\d+)\.", re.MULTILINE)


def sanitize_trace_name(path: Path) -> str:
    name = path.name
    for suffix in (".champsimtrace.gz", ".champsimtrace.xz", ".champsimtrace", ".gz", ".xz"):
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return path.stem


def workload_key(stem: str) -> tuple[int, str]:
    match = WORKLOAD_RE.match(stem)
    if match is None:
        return (1 << 30, stem)
    return (int(match.group("id")), match.group("name"))


def workload_label(stem: str) -> str:
    match = WORKLOAD_RE.match(stem)
    if match is None:
        return stem
    return match.group("name")


def parse_stats(text: str) -> dict[str, float]:
    return {match.group(1): float(match.group(2)) for match in STAT_RE.finditer(text)}


def collect_node_ids(stats: dict[str, float]) -> list[int]:
    node_ids = set()
    for key in stats:
        match = re.match(r"stat\.node\.(\d+)\.", key)
        if match is not None:
            node_ids.add(int(match.group(1)))
    return sorted(node_ids)


def require_node_values(stats: dict[str, float], node_ids: list[int], suffix: str) -> list[float] | None:
    values: list[float] = []
    for node_id in node_ids:
        key = f"stat.node.{node_id}.{suffix}"
        value = stats.get(key)
        if value is None:
            return None
        values.append(value)
    return values


def weighted_average(values: list[float], weights: list[float]) -> float:
    denom = sum(weights)
    if denom <= 0.0:
        return 0.0
    return sum(value * weight for value, weight in zip(values, weights)) / denom


def parse_run(path: Path, topology_label: str) -> dict[str, object] | None:
    text = path.read_text(errors="ignore")
    stats = parse_stats(text)
    trace_match = TRACE_RE.search(text)
    trace_path = Path(trace_match.group(1).strip()) if trace_match else path
    stem = sanitize_trace_name(trace_path)

    node_ids = collect_node_ids(stats)
    if not node_ids:
        return None

    miss_weights = require_node_values(stats, node_ids, "llc.cxl_miss")
    phase_cycles = require_node_values(stats, node_ids, "exec.phase_cycles")
    phase_time_us = require_node_values(stats, node_ids, "exec.phase_time_us")
    llc_avg_cxl_miss_lat = require_node_values(stats, node_ids, "amat.llc_avg_cxl_miss_lat")
    roundtrip_values = require_node_values(stats, node_ids, "amat.cxl_avg_roundtrip_lat")
    queue_values = require_node_values(stats, node_ids, "amat.cxl_avg_queue_delay")
    access_service_values = require_node_values(stats, node_ids, "amat.cxl_avg_access_service_time")
    interface_values = require_node_values(stats, node_ids, "amat.cxl_avg_interface_delay")
    aggregate_bw_values = require_node_values(stats, node_ids, "cxl.aggregate_gbps")
    load_lat_values = require_node_values(stats, node_ids, "amat.avg_load_issue_to_complete_lat")
    load_lat_counts = require_node_values(stats, node_ids, "cpu.0.load_issue_to_complete_count")
    retired_load_ops = require_node_values(stats, node_ids, "amat.retired_load_ops")
    retired_store_ops = require_node_values(stats, node_ids, "amat.retired_store_ops")

    required_groups = [
        miss_weights,
        phase_cycles,
        phase_time_us,
        llc_avg_cxl_miss_lat,
        roundtrip_values,
        queue_values,
        access_service_values,
        interface_values,
        aggregate_bw_values,
        load_lat_values,
        load_lat_counts,
        retired_load_ops,
        retired_store_ops,
    ]
    if any(group is None for group in required_groups):
        return None

    assert miss_weights is not None
    assert phase_cycles is not None
    assert phase_time_us is not None
    assert llc_avg_cxl_miss_lat is not None
    assert roundtrip_values is not None
    assert queue_values is not None
    assert access_service_values is not None
    assert interface_values is not None
    assert aggregate_bw_values is not None
    assert load_lat_values is not None
    assert load_lat_counts is not None
    assert retired_load_ops is not None
    assert retired_store_ops is not None

    queue_delay = weighted_average(queue_values, miss_weights)
    access_service = weighted_average(access_service_values, miss_weights)
    interface_delay = weighted_average(interface_values, miss_weights)
    roundtrip = weighted_average(roundtrip_values, miss_weights)
    plotted_total = FIXED_ONCHIP_CYCLES + queue_delay + access_service + interface_delay
    total_loads = sum(retired_load_ops)
    total_stores = sum(retired_store_ops)

    return {
        "workload": stem,
        "workload_label": workload_label(stem),
        "topology": topology_label,
        "node_count": len(node_ids),
        "phase_cycles": max(phase_cycles),
        "phase_time_us": max(phase_time_us),
        "avg_load_issue_to_complete_lat": weighted_average(load_lat_values, load_lat_counts),
        "load_store_ratio": (total_loads / total_stores) if total_stores > 0 else 0.0,
        "llc_avg_cxl_miss_lat": weighted_average(llc_avg_cxl_miss_lat, miss_weights),
        "cxl_avg_roundtrip_lat": roundtrip,
        "cxl_avg_queue_delay": queue_delay,
        "cxl_avg_access_service_time": access_service,
        "cxl_avg_interface_delay": interface_delay,
        "cxl_aggregate_gbps": sum(aggregate_bw_values),
        "fixed_onchip_cycles": FIXED_ONCHIP_CYCLES,
        "plotted_total_cycles": plotted_total,
        "roundtrip_minus_sum_cycles": roundtrip - (queue_delay + access_service + interface_delay),
        "log_path": str(path),
    }


def collect_rows(log_dir: Path, topology_label: str) -> dict[str, dict[str, object]]:
    rows: dict[str, dict[str, object]] = {}
    for out_path in sorted(log_dir.glob("*.out")):
        row = parse_run(out_path, topology_label)
        if row is None:
            continue
        rows[str(row["workload"])] = row
    return rows


def write_summary(rows: list[dict[str, object]], out_csv: Path) -> None:
    fieldnames = [
        "workload",
        "workload_label",
        "topology",
        "node_count",
        "phase_cycles",
        "phase_time_us",
        "avg_load_issue_to_complete_lat",
        "load_store_ratio",
        "llc_avg_cxl_miss_lat",
        "cxl_avg_roundtrip_lat",
        "cxl_avg_queue_delay",
        "cxl_avg_access_service_time",
        "cxl_avg_interface_delay",
        "fixed_onchip_cycles",
        "plotted_total_cycles",
        "roundtrip_minus_sum_cycles",
        "cxl_aggregate_gbps",
        "log_path",
    ]
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    print(f"[STATUS] Wrote summary: {out_csv}")


def plot(rows: list[dict[str, object]], out_png: Path, out_pdf: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch

    workloads = sorted({str(row["workload"]) for row in rows}, key=workload_key)
    by_key = {(str(row["workload"]), str(row["topology"])): row for row in rows}
    topology_order = ["No Replication (1 pool)", "Replication (2 pools)"]
    hatch_by_topology = {
        "No Replication (1 pool)": "",
        "Replication (2 pools)": "//",
    }
    colors = {
        "Access Service Time": "#4c9f70",
        "On-Chip Time (fixed)": "#d9d9d9",
        "Interface Delay": "#7f6bb2",
        "Queue Delay": "#4f8bc9",
    }
    components = [
        ("Access Service Time", "cxl_avg_access_service_time"),
        ("On-Chip Time (fixed)", "fixed_onchip_cycles"),
        ("Interface Delay", "cxl_avg_interface_delay"),
        ("Queue Delay", "cxl_avg_queue_delay"),
    ]

    fig, ax = plt.subplots(figsize=(13.6, 5.8), constrained_layout=True)
    x_positions = list(range(len(workloads)))
    width = 0.34
    offsets = {
        "No Replication (1 pool)": -width / 2,
        "Replication (2 pools)": width / 2,
    }

    for topology in topology_order:
        bottoms = [0.0] * len(workloads)
        for component_name, key in components:
            heights: list[float] = []
            for workload in workloads:
                row = by_key.get((workload, topology))
                heights.append(float(row[key]) if row is not None else 0.0)
            ax.bar(
                [x + offsets[topology] for x in x_positions],
                heights,
                width=width,
                bottom=bottoms,
                color=colors[component_name],
                edgecolor="black",
                linewidth=0.8,
                hatch=hatch_by_topology[topology],
            )
            bottoms = [bottom + height for bottom, height in zip(bottoms, heights)]

    for topology in topology_order:
        for idx, workload in enumerate(workloads):
            row = by_key.get((workload, topology))
            if row is None:
                continue
            total = float(row["plotted_total_cycles"])
            ax.text(
                x_positions[idx] + offsets[topology],
                total + 8.0,
                f"{total:.0f}",
                ha="center",
                va="bottom",
                fontsize=8,
                rotation=90,
            )

    ax.set_xticks(x_positions)
    ax.set_xticklabels([by_key[(workload, topology_order[0])]["workload_label"] for workload in workloads], rotation=0)
    ax.set_ylabel("Latency (cycles)")
    node_count = int(rows[0]["node_count"]) if rows else 0
    ax.set_title(f"Real-Workload Remote-Miss Latency Breakdown ({node_count} nodes)")
    ax.grid(True, axis="y", alpha=0.3)

    component_handles = [
        Patch(facecolor=colors[name], edgecolor="black", label=name)
        for name, _ in components
    ]
    topology_handles = [
        Patch(facecolor="white", edgecolor="black", hatch=hatch_by_topology[name], label=name)
        for name in topology_order
    ]
    component_legend = ax.legend(
        handles=component_handles,
        loc="upper left",
        bbox_to_anchor=(1.01, 1.0),
        borderaxespad=0.0,
        frameon=True,
    )
    ax.add_artist(component_legend)
    ax.legend(
        handles=topology_handles,
        loc="upper left",
        bbox_to_anchor=(1.01, 0.55),
        borderaxespad=0.0,
        frameon=True,
    )

    fig.savefig(out_png, dpi=220)
    fig.savefig(out_pdf)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")
    print(f"[STATUS] Wrote plot: {out_pdf}")


def main() -> int:
    pool_rows = collect_rows(POOL_LOG_DIR, "No Replication (1 pool)")
    rep_rows = collect_rows(REPLICATION_LOG_DIR, "Replication (2 pools)")
    common_workloads = sorted(set(pool_rows) & set(rep_rows), key=workload_key)
    if not common_workloads:
        print("[FAIL] No common workloads found across the two real-workload log directories.")
        return 1

    rows: list[dict[str, object]] = []
    for workload in common_workloads:
        rows.append(pool_rows[workload])
        rows.append(rep_rows[workload])

    write_summary(rows, OUT_CSV)
    plot(rows, OUT_PNG, OUT_PDF)
    print("[STATUS] Real-workload latency breakdown post-processing complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
