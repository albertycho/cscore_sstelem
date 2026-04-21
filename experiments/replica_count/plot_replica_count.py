#!/usr/bin/env python3
import csv
import os
import re
from pathlib import Path
from typing import Dict

# use: python3 experiments/replica_count/plot_replica_count.py

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs"
NUM_NODES = int(os.environ.get("NUM_NODES", "8"))
ELBOW_LATENCY_THRESHOLD = float(os.environ.get("ELBOW_LATENCY_THRESHOLD", "1000.0"))
REPRESENTATIVE_LOAD_PCTS = [
    int(token)
    for token in os.environ.get("REPRESENTATIVE_LOAD_PCTS", "0,50,80,100").split(",")
    if token.strip()
]

OUT_FILE_RE = re.compile(
    r"run_replica(?P<replicas>\d{3})_load(?P<load_pct>\d{3})(?:_(?P<label>[^_]+))?_bw(?P<req_bw>[0-9]+p[0-9]+)\.out$"
)

LAT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.avg_load_issue_to_complete_lat\s*=\s*([0-9.eE+-]+)")
LAT_COUNT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.load_issue_to_complete_count\s*=\s*([0-9.eE+-]+)")
REQ_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.request_gbps\s*=\s*([0-9.eE+-]+)")
RESP_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.response_gbps\s*=\s*([0-9.eE+-]+)")
AGG_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.aggregate_gbps\s*=\s*([0-9.eE+-]+)")


def parse_scalars(pattern: re.Pattern[str], text: str) -> dict[int, float]:
    return {int(m.group(1)): float(m.group(2)) for m in pattern.finditer(text)}


def parse_req_bw_token(token: str) -> float:
    return float(token.replace("p", "."))


def parse_run(path: Path) -> dict[str, object] | None:
    match = OUT_FILE_RE.match(path.name)
    if match is None:
        return None

    text = path.read_text(errors="ignore")
    lat_by_node = parse_scalars(LAT_RE, text)
    lat_count_by_node = parse_scalars(LAT_COUNT_RE, text)
    req_bw_by_node = parse_scalars(REQ_BW_RE, text)
    resp_bw_by_node = parse_scalars(RESP_BW_RE, text)
    agg_bw_by_node = parse_scalars(AGG_BW_RE, text)
    if (
        len(lat_by_node) != NUM_NODES
        or len(lat_count_by_node) != NUM_NODES
        or len(req_bw_by_node) != NUM_NODES
        or len(resp_bw_by_node) != NUM_NODES
        or len(agg_bw_by_node) != NUM_NODES
    ):
        return None

    weighted_lat_sum = 0.0
    weighted_lat_count = 0.0
    for node, lat in lat_by_node.items():
        count = lat_count_by_node.get(node, 0.0)
        weighted_lat_sum += lat * count
        weighted_lat_count += count
    if weighted_lat_count <= 0.0:
        return None

    return {
        "replicas": int(match.group("replicas")),
        "load_pct": int(match.group("load_pct")),
        "requested_request_gbps_per_node": parse_req_bw_token(match.group("req_bw")),
        "request_gbps": sum(req_bw_by_node.values()),
        "response_gbps": sum(resp_bw_by_node.values()),
        "aggregate_bw_gbps": sum(agg_bw_by_node.values()),
        "latency_cycles": weighted_lat_sum / weighted_lat_count,
        "label": match.group("label") or "sample",
        "log_path": str(path),
    }


def collect_rows() -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for out_path in sorted(LOG_DIR.glob("run_replica*_load*_bw*.out")):
        row = parse_run(out_path)
        if row is not None:
            rows.append(row)
    return rows


def write_summary(rows: list[dict[str, object]], out_csv: Path) -> None:
    rows_sorted = sorted(rows, key=lambda row: (int(row["replicas"]), int(row["load_pct"]), float(row["aggregate_bw_gbps"])))
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "replicas",
                "load_pct",
                "aggregate_bw_gbps",
                "latency_cycles",
                "requested_request_gbps_per_node",
                "request_gbps",
                "response_gbps",
                "label",
                "log_path",
            ],
        )
        writer.writeheader()
        writer.writerows(rows_sorted)
    print(f"[STATUS] Wrote summary: {out_csv}")


def plot_bw_latency(rows: list[dict[str, object]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors
    from matplotlib.cm import ScalarMappable

    load_pcts = sorted({int(row["load_pct"]) for row in rows})
    replica_counts = sorted({int(row["replicas"]) for row in rows})
    norm = colors.Normalize(vmin=min(load_pcts), vmax=max(load_pcts))
    cmap = plt.get_cmap("viridis")

    ncols = 2
    nrows = (len(replica_counts) + ncols - 1) // ncols
    fig, axes = plt.subplots(
        nrows,
        ncols,
        figsize=(6.0 * ncols, 3.6 * nrows),
        sharex=True,
        sharey=True,
        constrained_layout=True,
    )
    axes_list = list(axes.flat) if hasattr(axes, "flat") else [axes]

    for idx, replica_count in enumerate(replica_counts):
        ax = axes_list[idx]
        for load_pct in load_pcts:
            series = sorted(
                [
                    row for row in rows
                    if int(row["replicas"]) == replica_count and int(row["load_pct"]) == load_pct
                ],
                key=lambda row: float(row["aggregate_bw_gbps"]),
            )
            if not series:
                continue
            x = [float(row["aggregate_bw_gbps"]) for row in series]
            y = [float(row["latency_cycles"]) for row in series]
            ax.plot(x, y, linewidth=1.8, color=cmap(norm(load_pct)))
        ax.set_title(f"Replica Count = {replica_count}")
        ax.set_ylim(0, 3000)
        ax.grid(True, alpha=0.3)

    for idx in range(len(replica_counts), len(axes_list)):
        axes_list[idx].set_visible(False)

    for ax in axes_list[:len(replica_counts)]:
        ax.set_xlabel("Aggregate Bandwidth (Gbps)")
    for row_idx in range(nrows):
        axes_list[row_idx * ncols].set_ylabel("Memory Access Latency (cycles)")

    fig.suptitle("8-Node Pointer-Chase Bandwidth-Latency Curves Across Replica Counts")
    cbar = fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=axes_list[:len(replica_counts)])
    cbar.set_label("Load %")

    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def plot_selected_loads(rows: list[dict[str, object]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors
    from matplotlib.cm import ScalarMappable

    load_pcts = sorted({int(row["load_pct"]) for row in rows})
    selected_loads = [load for load in REPRESENTATIVE_LOAD_PCTS if load in load_pcts]
    replica_counts = sorted({int(row["replicas"]) for row in rows})
    norm = colors.Normalize(vmin=min(replica_counts), vmax=max(replica_counts))
    cmap = plt.get_cmap("plasma")

    ncols = 2
    nrows = (len(selected_loads) + ncols - 1) // ncols
    fig, axes = plt.subplots(
        nrows,
        ncols,
        figsize=(5.4 * ncols, 4.0 * nrows),
        sharex=True,
        sharey=True,
        constrained_layout=True,
    )
    axes_list = list(axes.flat) if hasattr(axes, "flat") else [axes]

    for idx, load_pct in enumerate(selected_loads):
        ax = axes_list[idx]
        for replica_count in replica_counts:
            series = sorted(
                [
                    row for row in rows
                    if int(row["load_pct"]) == load_pct and int(row["replicas"]) == replica_count
                ],
                key=lambda row: float(row["aggregate_bw_gbps"]),
            )
            if not series:
                continue
            x = [float(row["aggregate_bw_gbps"]) for row in series]
            y = [float(row["latency_cycles"]) for row in series]
            ax.plot(x, y, linewidth=1.8, color=cmap(norm(replica_count)))
        ax.set_title(f"Load = {load_pct}%")
        ax.set_ylim(0, 3000)
        ax.grid(True, alpha=0.3)

    for idx in range(len(selected_loads), len(axes_list)):
        axes_list[idx].set_visible(False)

    for ax in axes_list[:len(selected_loads)]:
        ax.set_xlabel("Aggregate Bandwidth (Gbps)")
    for row_idx in range(nrows):
        axes_list[row_idx * ncols].set_ylabel("Memory Access Latency (cycles)")

    fig.suptitle("8-Node Pointer-Chase Curves at Representative Load Ratios")
    cbar = fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=axes_list[:len(selected_loads)])
    cbar.set_label("Replica Count")

    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def plot_elbow_bandwidth(rows: list[dict[str, object]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors
    from matplotlib.cm import ScalarMappable

    load_pcts = sorted({int(row["load_pct"]) for row in rows})
    replica_counts = sorted({int(row["replicas"]) for row in rows})
    norm = colors.Normalize(vmin=min(load_pcts), vmax=max(load_pcts))
    cmap = plt.get_cmap("viridis")

    fig, ax = plt.subplots(figsize=(7.4, 5.0), constrained_layout=True)
    for load_pct in load_pcts:
        x: list[int] = []
        y: list[float] = []
        for replica_count in replica_counts:
            series = [
                row for row in rows
                if int(row["load_pct"]) == load_pct and int(row["replicas"]) == replica_count
            ]
            feasible = [
                float(row["aggregate_bw_gbps"])
                for row in series
                if float(row["latency_cycles"]) <= ELBOW_LATENCY_THRESHOLD
            ]
            if not feasible:
                continue
            x.append(replica_count)
            y.append(max(feasible))
        if x:
            ax.plot(x, y, linewidth=1.8, marker="o", markersize=4, color=cmap(norm(load_pct)))

    ax.set_xlabel("Replica Count")
    ax.set_ylabel("Max Aggregate Bandwidth (Gbps)")
    ax.set_title(f"Bandwidth Sustained Below {ELBOW_LATENCY_THRESHOLD:.0f} Cycles")
    ax.set_xticks(replica_counts)
    ax.grid(True, alpha=0.3)
    cbar = fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=ax)
    cbar.set_label("Load %")

    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def build_elbow_table(rows: list[dict[str, object]]) -> Dict[int, Dict[int, float]]:
    load_pcts = sorted({int(row["load_pct"]) for row in rows})
    replica_counts = sorted({int(row["replicas"]) for row in rows})
    elbow_by_load: Dict[int, Dict[int, float]] = {}

    for load_pct in load_pcts:
        elbow_by_load[load_pct] = {}
        for replica_count in replica_counts:
            series = [
                row for row in rows
                if int(row["load_pct"]) == load_pct and int(row["replicas"]) == replica_count
            ]
            feasible = [
                float(row["aggregate_bw_gbps"])
                for row in series
                if float(row["latency_cycles"]) <= ELBOW_LATENCY_THRESHOLD
            ]
            if feasible:
                elbow_by_load[load_pct][replica_count] = max(feasible)

    return elbow_by_load


def write_elbow_summary(elbow_by_load: Dict[int, Dict[int, float]], out_csv: Path) -> None:
    replica_counts = sorted({replica for row in elbow_by_load.values() for replica in row})
    with out_csv.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["load_pct", "replica_count", "max_aggregate_bw_gbps_below_threshold", "latency_threshold_cycles"])
        for load_pct in sorted(elbow_by_load):
            for replica_count in replica_counts:
                value = elbow_by_load[load_pct].get(replica_count)
                if value is None:
                    continue
                writer.writerow([load_pct, replica_count, value, ELBOW_LATENCY_THRESHOLD])
    print(f"[STATUS] Wrote elbow summary: {out_csv}")


def plot_elbow_heatmap(elbow_by_load: Dict[int, Dict[int, float]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    import numpy as np

    load_pcts = sorted(elbow_by_load)
    replica_counts = sorted({replica for row in elbow_by_load.values() for replica in row})
    matrix = np.full((len(load_pcts), len(replica_counts)), np.nan, dtype=float)

    for load_idx, load_pct in enumerate(load_pcts):
        for replica_idx, replica_count in enumerate(replica_counts):
            value = elbow_by_load[load_pct].get(replica_count)
            if value is not None:
                matrix[load_idx, replica_idx] = value

    fig, ax = plt.subplots(figsize=(7.2, 5.6), constrained_layout=True)
    image = ax.imshow(matrix, origin="lower", aspect="auto", cmap="viridis")
    ax.set_xticks(range(len(replica_counts)))
    ax.set_xticklabels(replica_counts)
    ax.set_yticks(range(len(load_pcts)))
    ax.set_yticklabels(load_pcts)
    ax.set_xlabel("Replica Count")
    ax.set_ylabel("Load %")
    ax.set_title(f"Max Aggregate Bandwidth Sustained Below {ELBOW_LATENCY_THRESHOLD:.0f} Cycles")
    cbar = fig.colorbar(image, ax=ax)
    cbar.set_label("Aggregate Bandwidth (Gbps)")

    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def plot_normalized_gain(elbow_by_load: Dict[int, Dict[int, float]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors
    from matplotlib.cm import ScalarMappable

    load_pcts = sorted(elbow_by_load)
    replica_counts = sorted({replica for row in elbow_by_load.values() for replica in row})
    norm = colors.Normalize(vmin=min(load_pcts), vmax=max(load_pcts))
    cmap = plt.get_cmap("viridis")

    fig, ax = plt.subplots(figsize=(7.4, 5.0), constrained_layout=True)
    for load_pct in load_pcts:
        baseline = elbow_by_load[load_pct].get(replica_counts[0])
        if baseline is None or baseline <= 0.0:
            continue
        x: list[int] = []
        y: list[float] = []
        for replica_count in replica_counts:
            value = elbow_by_load[load_pct].get(replica_count)
            if value is None:
                continue
            x.append(replica_count)
            y.append(value / baseline)
        if x:
            ax.plot(x, y, linewidth=1.8, marker="o", markersize=4, color=cmap(norm(load_pct)))

    ax.set_xlabel("Replica Count")
    ax.set_ylabel("Bandwidth Relative to 1 Replica")
    ax.set_title(f"Normalized Bandwidth Sustained Below {ELBOW_LATENCY_THRESHOLD:.0f} Cycles")
    ax.set_xticks(replica_counts)
    ax.grid(True, alpha=0.3)
    cbar = fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=ax)
    cbar.set_label("Load %")

    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def plot_marginal_gain(elbow_by_load: Dict[int, Dict[int, float]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors
    from matplotlib.cm import ScalarMappable

    load_pcts = sorted(elbow_by_load)
    replica_counts = sorted({replica for row in elbow_by_load.values() for replica in row})
    norm = colors.Normalize(vmin=min(load_pcts), vmax=max(load_pcts))
    cmap = plt.get_cmap("viridis")

    fig, ax = plt.subplots(figsize=(7.4, 5.0), constrained_layout=True)
    for load_pct in load_pcts:
        x: list[int] = []
        y: list[float] = []
        for replica_count in replica_counts[1:]:
            current = elbow_by_load[load_pct].get(replica_count)
            previous = elbow_by_load[load_pct].get(replica_count - 1)
            if current is None or previous is None:
                continue
            x.append(replica_count)
            y.append(current - previous)
        if x:
            ax.plot(x, y, linewidth=1.8, marker="o", markersize=4, color=cmap(norm(load_pct)))

    ax.set_xlabel("Replica Count")
    ax.set_ylabel("Bandwidth Gain from Previous Replica (Gbps)")
    ax.set_title(f"Incremental Bandwidth Gain Below {ELBOW_LATENCY_THRESHOLD:.0f} Cycles")
    ax.set_xticks(replica_counts[1:])
    ax.grid(True, alpha=0.3)
    cbar = fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=ax)
    cbar.set_label("Load %")

    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def main() -> int:
    rows = collect_rows()
    if not rows:
        print(f"[FAIL] No replica-sweep logs with complete stats found under {LOG_DIR}")
        return 1

    out_csv = LOG_DIR / "replica_sweep_bw_latency.csv"
    write_summary(rows, out_csv)
    elbow_by_load = build_elbow_table(rows)
    write_elbow_summary(elbow_by_load, LOG_DIR / "replica_sweep_elbow_summary.csv")

    try:
        plot_bw_latency(rows, LOG_DIR / "replica_sweep_bw_latency.png")
        plot_selected_loads(rows, LOG_DIR / "replica_sweep_bw_latency_selected_loads.png")
        plot_elbow_heatmap(elbow_by_load, LOG_DIR / "replica_sweep_elbow_heatmap.png")
    except ModuleNotFoundError as exc:
        print(f"[FAIL] matplotlib not available: {exc}")
        return 1

    print("[STATUS] Replica-sweep post-processing complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
