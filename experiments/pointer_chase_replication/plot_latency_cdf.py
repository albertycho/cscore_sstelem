#!/usr/bin/env python3
import ast
import re
from pathlib import Path

# use: python3 experiments/pointer_chase_replication/plot_latency_cdf.py

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs_cdf"

OUT_FILE_RE = re.compile(
    r"run_cdf_(?P<config>.+?)_load(?P<load_pct>\d{3})_bw(?P<req_bw>[0-9]+p[0-9]+)\.out$"
)
HIST_BIN_RE = re.compile(r"stat\.node\.(\d+)\.llc\.miss_lat_hist_bin_ns = (\d+)")
HIST_RE = re.compile(r"stat\.node\.(\d+)\.llc\.miss_lat_hist = (\[.*\])")


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


def main() -> int:
    try:
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:
        print(f"[FAIL] matplotlib not available: {exc}")
        return 1

    curves: list[tuple[str, list[float], list[float]]] = []
    for out_path in sorted(LOG_DIR.glob("run_cdf_*.out")):
        match = OUT_FILE_RE.match(out_path.name)
        if match is None:
            continue
        bin_ns, hist = parse_histogram(out_path)
        total = sum(hist)
        if total <= 0:
            continue
        cumulative: list[float] = []
        running = 0
        for value in hist:
            running += value
            cumulative.append(running / total)
        x_ns = [idx * bin_ns for idx in range(len(hist))]
        label = (
            f"{match.group('config')}, "
            f"load={int(match.group('load_pct'))}, "
            f"bw={match.group('req_bw').replace('p', '.')}"
        )
        curves.append((label, x_ns, cumulative))

    if not curves:
        print(f"[FAIL] No CDF logs found under {LOG_DIR}")
        return 1

    fig, ax = plt.subplots(figsize=(8.0, 5.0))
    for label, x_ns, cumulative in curves:
        ax.plot(x_ns, cumulative, linewidth=2.0, label=label)
    ax.set_xlabel("LLC Miss Latency (ns)")
    ax.set_ylabel("CDF")
    ax.set_ylim(0.0, 1.0)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower right", fontsize=9)
    fig.tight_layout()
    out_png = LOG_DIR / "pointer_chase_replication_latency_cdf.png"
    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
