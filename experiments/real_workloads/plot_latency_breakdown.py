#!/usr/bin/env python3
import csv
import os
import re
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
POOL_LOG_DIR = SCRIPT_DIR / "logs_switch_pool"
REPLICATION_LOG_DIR = SCRIPT_DIR / "logs_switch_replication"
OUT_CSV = SCRIPT_DIR / "real_workload_latency_breakdown.csv"
OUT_PNG = SCRIPT_DIR / "real_workload_latency_breakdown.png"
OUT_PDF = SCRIPT_DIR / "real_workload_latency_breakdown.pdf"
FIXED_ONCHIP_CYCLES = float(os.environ.get("FIXED_ONCHIP_CYCLES", "25.0"))

STAT_RE = re.compile(r"^(stat\.[^=]+?)\s*=\s*([0-9.eE+-]+)\s*$", re.MULTILINE)
TRACE_RE = re.compile(r"^# TRACE_PATH:\s*(.+)\s*$", re.MULTILINE)
WORKLOAD_RE = re.compile(r"^(?P<id>\d+)\.(?P<name>[A-Za-z0-9]+)")


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
    return f"{match.group('id')}.{match.group('name')}"


def parse_stats(text: str) -> dict[str, float]:
    return {match.group(1): float(match.group(2)) for match in STAT_RE.finditer(text)}


def parse_run(path: Path, topology_label: str) -> dict[str, object] | None:
    text = path.read_text(errors="ignore")
    stats = parse_stats(text)
    trace_match = TRACE_RE.search(text)
    trace_path = Path(trace_match.group(1).strip()) if trace_match else path
    stem = sanitize_trace_name(trace_path)

    required = [
        "stat.node.0.exec.phase_cycles",
        "stat.node.0.exec.phase_time_us",
        "stat.node.0.amat.avg_load_issue_to_complete_lat",
        "stat.node.0.amat.load_store_ratio",
        "stat.node.0.amat.llc_avg_cxl_miss_lat",
        "stat.node.0.amat.cxl_avg_roundtrip_lat",
        "stat.node.0.amat.cxl_avg_queue_delay",
        "stat.node.0.amat.cxl_avg_access_service_time",
        "stat.node.0.amat.cxl_avg_interface_delay",
        "stat.node.0.cxl.aggregate_gbps",
    ]
    if any(key not in stats for key in required):
        return None

    queue_delay = stats["stat.node.0.amat.cxl_avg_queue_delay"]
    access_service = stats["stat.node.0.amat.cxl_avg_access_service_time"]
    interface_delay = stats["stat.node.0.amat.cxl_avg_interface_delay"]
    roundtrip = stats["stat.node.0.amat.cxl_avg_roundtrip_lat"]
    plotted_total = FIXED_ONCHIP_CYCLES + queue_delay + access_service + interface_delay

    return {
        "workload": stem,
        "workload_label": workload_label(stem),
        "topology": topology_label,
        "phase_cycles": stats["stat.node.0.exec.phase_cycles"],
        "phase_time_us": stats["stat.node.0.exec.phase_time_us"],
        "avg_load_issue_to_complete_lat": stats["stat.node.0.amat.avg_load_issue_to_complete_lat"],
        "load_store_ratio": stats["stat.node.0.amat.load_store_ratio"],
        "llc_avg_cxl_miss_lat": stats["stat.node.0.amat.llc_avg_cxl_miss_lat"],
        "cxl_avg_roundtrip_lat": roundtrip,
        "cxl_avg_queue_delay": queue_delay,
        "cxl_avg_access_service_time": access_service,
        "cxl_avg_interface_delay": interface_delay,
        "cxl_aggregate_gbps": stats["stat.node.0.cxl.aggregate_gbps"],
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

    fig, ax = plt.subplots(figsize=(12.0, 5.8), constrained_layout=True)
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
    ax.set_title(f"Real-Workload Remote-Miss Latency Breakdown (Fixed On-Chip = {FIXED_ONCHIP_CYCLES:.0f} cycles)")
    ax.grid(True, axis="y", alpha=0.3)

    component_handles = [
        Patch(facecolor=colors[name], edgecolor="black", label=name)
        for name, _ in components
    ]
    topology_handles = [
        Patch(facecolor="white", edgecolor="black", hatch=hatch_by_topology[name], label=name)
        for name in topology_order
    ]
    component_legend = ax.legend(handles=component_handles, loc="upper left", frameon=True)
    ax.add_artist(component_legend)
    ax.legend(handles=topology_handles, loc="upper right", frameon=True)

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
