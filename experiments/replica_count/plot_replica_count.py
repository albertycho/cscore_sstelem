#!/usr/bin/env python3
import ast
import csv
import re
from pathlib import Path

# use: python3 experiments/replica_count/plot_replica_count.py

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs"
BLUE_CMAP = "Blues"

OUT_FILE_RE = re.compile(r"run_nodes(\d+)_replica(\d+)_(no_rep|rep)\.out$")
LAT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.avg_load_issue_to_complete_lat\s*=\s*([0-9.eE+-]+)")
LAT_COUNT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.load_issue_to_complete_count\s*=\s*([0-9.eE+-]+)")
HIST_BIN_RE = re.compile(r"stat\.node\.(\d+)\.llc\.miss_lat_hist_bin_ns = (\d+)")
HIST_RE = re.compile(r"stat\.node\.(\d+)\.llc\.miss_lat_hist = (\[.*\])")


def truncated_blue_cmap(plt_module):
    from matplotlib.colors import LinearSegmentedColormap
    base = plt_module.get_cmap(BLUE_CMAP)
    samples = [base(0.35 + 0.60 * (i / 255.0)) for i in range(256)]
    return LinearSegmentedColormap.from_list("Blues_truncated", samples)


def color_for_replica(cmap, norm, replica: int, min_replica: int):
    color = list(cmap(norm(replica)))
    if replica == min_replica:
        blend = 0.20
        color[0] = color[0] * (1.0 - blend) + blend
        color[1] = color[1] * (1.0 - blend) + blend
        color[2] = color[2] * (1.0 - blend) + blend
    return tuple(color)


def parse_run_metrics(path: Path) -> dict[str, float] | None:
    text = path.read_text(errors="ignore")
    lat_by_node = {int(m.group(1)): float(m.group(2)) for m in LAT_RE.finditer(text)}
    count_by_node = {int(m.group(1)): float(m.group(2)) for m in LAT_COUNT_RE.finditer(text)}
    if not lat_by_node or not count_by_node:
        return None

    weighted_lat_sum = 0.0
    weighted_lat_count = 0.0
    for node, lat in lat_by_node.items():
        count = count_by_node.get(node, 0.0)
        weighted_lat_sum += lat * count
        weighted_lat_count += count
    if weighted_lat_count <= 0.0:
        return None

    return {
        "weighted_avg_load_lat_cycles": weighted_lat_sum / weighted_lat_count,
        "total_load_samples": weighted_lat_count,
    }


def parse_histogram(path: Path) -> tuple[int, list[int]]:
    bin_ns = 10
    agg: list[int] = []
    with path.open("r", errors="ignore") as f:
        for line in f:
            m_bin = HIST_BIN_RE.search(line)
            if m_bin:
                bin_ns = int(m_bin.group(2))
                continue

            m_hist = HIST_RE.search(line)
            if not m_hist:
                continue
            hist = ast.literal_eval(m_hist.group(2))
            if len(agg) < len(hist):
                agg.extend([0] * (len(hist) - len(agg)))
            for idx, value in enumerate(hist):
                agg[idx] += int(value)
    return bin_ns, agg


def collect_runs(log_dir: Path) -> tuple[list[dict[str, float]], list[tuple[str, int, int, Path]]]:
    rows: list[dict[str, float]] = []
    artifacts: list[tuple[str, int, int, Path]] = []
    for out_path in sorted(log_dir.glob("run_nodes*_replica*_*.out")):
        match = OUT_FILE_RE.search(out_path.name)
        if not match:
            continue
        num_nodes = int(match.group(1))
        replicas = int(match.group(2))
        config = match.group(3)
        metrics = parse_run_metrics(out_path)
        if metrics is None:
            continue
        row = {"config": config, "num_nodes": num_nodes, "replicas": replicas}
        row.update(metrics)
        rows.append(row)
        artifacts.append((config, num_nodes, replicas, out_path))
    return rows, artifacts


def write_summary(rows: list[dict[str, float]], out_csv: Path) -> None:
    rows_sorted = sorted(rows, key=lambda row: (int(row["num_nodes"]), row["config"], int(row["replicas"])))
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows_sorted[0].keys()))
        writer.writeheader()
        writer.writerows(rows_sorted)
    print(f"[STATUS] Wrote summary: {out_csv}")


def plot_latency_vs_count(rows: list[dict[str, float]], out_png: Path) -> None:
    import matplotlib.pyplot as plt

    node_counts = sorted({int(row["num_nodes"]) for row in rows})
    if not node_counts:
        raise RuntimeError("No rows found for replica-count plot.")

    fig, ax = plt.subplots(figsize=(7.4, 4.8))
    for num_nodes in node_counts:
        series = sorted(
            [
                (
                    int(row["replicas"]),
                    float(row["weighted_avg_load_lat_cycles"]),
                )
                for row in rows
                if int(row["num_nodes"]) == num_nodes
            ],
            key=lambda item: item[0],
        )
        if not series:
            continue
        x = [point[0] for point in series]
        y = [point[1] for point in series]
        ax.plot(x, y, marker="o", linewidth=2.0, label=f"{num_nodes} nodes")

    ax.set_xlabel("Replica Count")
    ax.set_ylabel("Weighted Avg Memory Access Latency (cycles)")
    ax.set_title("Pointer-Chase Latency Across Node and Replica Counts")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    fig.tight_layout()
    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def plot_cdf_overlay(artifacts: list[tuple[str, int, int, Path]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors

    max_node_count = max(num_nodes for _, num_nodes, _, _ in artifacts)
    rep_artifacts = sorted(
        [
            artifact for artifact in artifacts
            if artifact[0] == "rep" and artifact[1] == max_node_count
        ],
        key=lambda item: item[2],
    )
    if not rep_artifacts:
        raise RuntimeError("No replication runs found for CDF overlay plot.")

    baseline_artifacts = [
        artifact for artifact in artifacts
        if artifact[0] == "no_rep" and artifact[1] == max_node_count
    ]

    replica_values = [replicas for _, _, replicas, _ in rep_artifacts]
    min_replica = min(replica_values)
    norm = colors.Normalize(vmin=min(replica_values), vmax=max(replica_values))
    cmap = truncated_blue_cmap(plt)

    fig, ax = plt.subplots(figsize=(8.0, 5.0))
    if baseline_artifacts:
        _, _, _, out_path = baseline_artifacts[0]
        bin_ns, hist = parse_histogram(out_path)
        total = sum(hist)
        if total > 0:
            cumulative = []
            running = 0
            for value in hist:
                running += value
                cumulative.append(running / total)
            x = [idx * bin_ns for idx in range(len(hist))]
            ax.plot(x, cumulative, linewidth=2.2, color="black", linestyle="--", label="no_rep")

    for _, _, replicas, out_path in rep_artifacts:
        bin_ns, hist = parse_histogram(out_path)
        total = sum(hist)
        if total <= 0:
            continue
        cumulative = []
        running = 0
        for value in hist:
            running += value
            cumulative.append(running / total)
        x = [idx * bin_ns for idx in range(len(hist))]
        color = color_for_replica(cmap, norm, replicas, min_replica)
        ax.plot(x, cumulative, linewidth=2.0, alpha=0.95, color=color, label=f"rep, r={replicas}")

    ax.set_xlabel("LLC Miss Latency (ns)")
    ax.set_ylabel("CDF")
    ax.set_ylim(0.0, 1.0)
    ax.set_title(f"Pointer-Chase Latency CDF vs Replica Count ({max_node_count} nodes)")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower right")
    fig.tight_layout()
    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def main() -> int:
    rows, artifacts = collect_runs(LOG_DIR)
    if not rows:
        print(f"[FAIL] No replica-count logs found under {LOG_DIR}")
        return 1

    write_summary(rows, LOG_DIR / "replica_count_summary.csv")

    try:
        plot_latency_vs_count(rows, LOG_DIR / "replica_count_latency_vs_count.png")
        plot_cdf_overlay(artifacts, LOG_DIR / "replica_count_latency_cdf_overlay.png")
    except ModuleNotFoundError as exc:
        print(f"[FAIL] matplotlib not available: {exc}")
        return 1

    print("[STATUS] Replica-count post-processing complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
