#!/usr/bin/env python3
import ast
import csv
import re
from pathlib import Path
from typing import Dict, List, Tuple

# use: python3 experiments/replica_count/plot_replica_count.py

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs"
CLOCK_GHZ = 2.4
BLUE_CMAP = "Blues"


def truncated_blue_cmap(plt_module):
    from matplotlib.colors import LinearSegmentedColormap
    base = plt_module.get_cmap(BLUE_CMAP)
    # Skip the very light end so the smallest replica count is still visible.
    samples = [base(0.35 + 0.60 * (i / 255.0)) for i in range(256)]
    return LinearSegmentedColormap.from_list("Blues_truncated", samples)


def color_for_replica(cmap, norm, replica: int, min_replica: int):
    color = list(cmap(norm(replica)))
    if replica == min_replica:
        # Make the smallest replica line/marker slightly lighter for readability.
        blend = 0.20
        color[0] = color[0] * (1.0 - blend) + blend
        color[1] = color[1] * (1.0 - blend) + blend
        color[2] = color[2] * (1.0 - blend) + blend
    return tuple(color)

OUT_FILE_RE = re.compile(r"run_replica(\d+)_(no_rep|rep)\.out$")

WALL_RE = re.compile(r"stat\.node\.(\d+)\.walltime_s = ([0-9.eE+-]+)")
CXL_LAT_RE = re.compile(r"stat\.node\.(\d+)\.llc\.avg_cxl_lat = ([0-9.eE+-]+)")
CXL_MISS_RE = re.compile(r"stat\.node\.(\d+)\.llc\.cxl_miss = ([0-9.eE+-]+)")
HIST_BIN_RE = re.compile(r"stat\.node\.(\d+)\.llc\.miss_lat_hist_bin_ns = (\d+)")
HIST_RE = re.compile(r"stat\.node\.(\d+)\.llc\.miss_lat_hist = (\[.*\])")


def parse_run_metrics(path: Path) -> Dict[str, float]:
    txt = path.read_text(errors="ignore")
    walls = [float(m.group(2)) for m in WALL_RE.finditer(txt)]
    cxl_lat = {int(m.group(1)): float(m.group(2)) for m in CXL_LAT_RE.finditer(txt)}
    cxl_miss = {int(m.group(1)): float(m.group(2)) for m in CXL_MISS_RE.finditer(txt)}

    weighted_cxl_lat = 0.0
    total_cxl_miss = 0.0
    for node, miss in cxl_miss.items():
        lat = cxl_lat.get(node, 0.0)
        weighted_cxl_lat += miss * lat
        total_cxl_miss += miss

    avg_cxl_lat_cycles = (weighted_cxl_lat / total_cxl_miss) if total_cxl_miss > 0 else 0.0
    return {
        "max_node_walltime_s": max(walls) if walls else 0.0,
        "weighted_avg_cxl_lat_cycles": avg_cxl_lat_cycles,
        "weighted_avg_cxl_lat_ns": (avg_cxl_lat_cycles / CLOCK_GHZ) if avg_cxl_lat_cycles > 0 else 0.0,
        "total_cxl_miss": total_cxl_miss,
    }


def parse_histogram(path: Path) -> Tuple[int, List[int]]:
    bin_ns = 10
    agg: List[int] = []
    with path.open("r", errors="ignore") as f:
        for line in f:
            m_bin = HIST_BIN_RE.search(line)
            if m_bin:
                try:
                    bin_ns = int(m_bin.group(2))
                except ValueError:
                    pass
                continue

            m_hist = HIST_RE.search(line)
            if not m_hist:
                continue
            try:
                hist = ast.literal_eval(m_hist.group(2))
            except (ValueError, SyntaxError):
                continue
            if not isinstance(hist, list):
                continue
            if len(agg) < len(hist):
                agg.extend([0] * (len(hist) - len(agg)))
            for i, val in enumerate(hist):
                try:
                    agg[i] += int(val)
                except (TypeError, ValueError):
                    pass
    return bin_ns, agg


def collect_runs(log_dir: Path) -> Tuple[List[Dict[str, float]], List[Tuple[str, int, Path]]]:
    rows: List[Dict[str, float]] = []
    artifacts: List[Tuple[str, int, Path]] = []
    for out_path in sorted(log_dir.glob("run_replica*_*.out")):
        m = OUT_FILE_RE.search(out_path.name)
        if not m:
            continue
        replicas = int(m.group(1))
        config = m.group(2)
        metrics = parse_run_metrics(out_path)
        row = {"config": config, "replicas": replicas}
        row.update(metrics)
        rows.append(row)
        artifacts.append((config, replicas, out_path))
    return rows, artifacts


def write_summary(rows: List[Dict[str, float]], out_csv: Path) -> None:
    rows_sorted = sorted(rows, key=lambda r: (r["config"], int(r["replicas"])))
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows_sorted[0].keys()))
        writer.writeheader()
        writer.writerows(rows_sorted)
    print(f"[STATUS] Wrote summary: {out_csv}")


def plot_latency_vs_count(rows: List[Dict[str, float]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors

    rep_points = sorted(
        [
            (int(row["replicas"]), float(row["weighted_avg_cxl_lat_ns"]))
            for row in rows
            if str(row["config"]) == "rep"
        ],
        key=lambda x: x[0],
    )
    if not rep_points:
        raise RuntimeError("No replication (rep) runs found for latency-vs-count plot.")

    x = [p[0] for p in rep_points]
    y = [p[1] for p in rep_points]
    min_replica = min(x)
    norm = colors.Normalize(vmin=min(x), vmax=max(x))
    cmap = truncated_blue_cmap(plt)
    point_colors = [color_for_replica(cmap, norm, replica, min_replica) for replica in x]

    fig, ax = plt.subplots(figsize=(7.0, 4.5))
    ax.plot(x, y, linewidth=1.2, color="0.4", alpha=0.8)
    sc = ax.scatter(
        x, y, c=point_colors, s=90, zorder=3,
        edgecolors="black", linewidths=0.6
    )
    avg_y = sum(y) / len(y)
    ax.axhline(
        avg_y,
        linestyle=":",
        linewidth=2.0,
        color="black",
        label=f"Average latency = {avg_y:.2f} ns",
        zorder=2,
    )
    ax.set_xlabel("Replica Count")
    ax.set_ylabel("Weighted Avg CXL Latency (ns)")
    ax.set_title("CXL Latency vs Replica Count (Replication Only)")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    sm = plt.cm.ScalarMappable(norm=norm, cmap=cmap)
    sm.set_array([])
    cbar = fig.colorbar(sm, ax=ax)
    cbar.set_label("Replica Count")
    fig.tight_layout()
    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def plot_cdf_overlay(artifacts: List[Tuple[str, int, Path]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors

    fig, ax = plt.subplots(figsize=(8.0, 5.0))
    plotted = 0
    rep_artifacts = sorted(
        [a for a in artifacts if a[0] == "rep"],
        key=lambda t: t[1],
    )
    if not rep_artifacts:
        plt.close(fig)
        raise RuntimeError("No replication (rep) runs found for CDF overlay plot.")

    replica_values = [replicas for _, replicas, _ in rep_artifacts]
    min_replica = min(replica_values)
    norm = colors.Normalize(vmin=min(replica_values), vmax=max(replica_values))
    cmap = truncated_blue_cmap(plt)

    for idx, (config, replicas, out_path) in enumerate(rep_artifacts):
        bin_ns, hist = parse_histogram(out_path)
        total = sum(hist)
        if total <= 0:
            continue
        cumulative = []
        running = 0
        weighted_sum_ns = 0.0
        for v in hist:
            running += v
            cumulative.append(running / total)
        for i, v in enumerate(hist):
            weighted_sum_ns += float(i * bin_ns) * float(v)
        mean_ns = weighted_sum_ns / float(total)
        x = [i * bin_ns for i in range(len(hist))]
        color = color_for_replica(cmap, norm, replicas, min_replica)
        ax.plot(x, cumulative, linewidth=2.0, alpha=0.95, color=color, label=f"rep, r={replicas}")
        ax.axvline(
            mean_ns,
            linestyle=":",
            linewidth=1.8,
            alpha=0.95,
            color=color,
            label="_nolegend_",
        )
        label_y = 0.03 + 0.07 * (idx % 6)
        ax.text(
            mean_ns + 8.0,
            label_y,
            f"avg r={replicas}",
            color=color,
            fontsize=8,
            va="bottom",
            ha="left",
            rotation=90,
            clip_on=True,
        )
        plotted += 1

    if plotted == 0:
        plt.close(fig)
        raise RuntimeError("No valid latency histograms found in logs.")

    ax.set_xlabel("LLC Miss Latency (ns)")
    ax.set_ylabel("CDF")
    ax.set_title("Overlayed Latency CDFs Across Replica Counts (Replication Only)")
    ax.set_xlim(left=200)
    ax.set_ylim(0.0, 1.0)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower right", fontsize=8, ncol=2)
    sm = plt.cm.ScalarMappable(norm=norm, cmap=cmap)
    sm.set_array([])
    cbar = fig.colorbar(sm, ax=ax)
    cbar.set_label("Replica Count")
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
    except RuntimeError as exc:
        print(f"[FAIL] {exc}")
        return 1

    print("[STATUS] Replica-count plotting complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
