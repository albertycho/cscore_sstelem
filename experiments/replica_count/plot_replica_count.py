#!/usr/bin/env python3
import csv
import os
import re
from pathlib import Path

# use: python3 experiments/replica_count/plot_replica_count.py

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs"
NUM_NODES = int(os.environ.get("NUM_NODES", "8"))

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

    ncols = 4
    nrows = (len(replica_counts) + ncols - 1) // ncols
    fig, axes = plt.subplots(
        nrows,
        ncols,
        figsize=(4.2 * ncols, 3.8 * nrows),
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


def main() -> int:
    rows = collect_rows()
    if not rows:
        print(f"[FAIL] No replica-sweep logs with complete stats found under {LOG_DIR}")
        return 1

    out_csv = LOG_DIR / "replica_sweep_bw_latency.csv"
    write_summary(rows, out_csv)

    try:
        plot_bw_latency(rows, LOG_DIR / "replica_sweep_bw_latency.png")
    except ModuleNotFoundError as exc:
        print(f"[FAIL] matplotlib not available: {exc}")
        return 1

    print("[STATUS] Replica-sweep post-processing complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
