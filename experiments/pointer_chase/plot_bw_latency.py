#!/usr/bin/env python3
import csv
import re
from pathlib import Path

# use: python3 experiments/pointer_chase/plot_bw_latency.py

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs"

OUT_FILE_RE = re.compile(
    r"run_load(?P<load_pct>\d{3})(?:_(?P<label>[^_]+))?_bw(?P<req_bw>[0-9]+p[0-9]+)\.out$"
)
LAT_RE = re.compile(r"stat\.node\.0\.cpu\.0\.avg_load_issue_to_complete_lat\s*=\s*([0-9.eE+-]+)")
REQ_BW_RE = re.compile(r"stat\.node\.0\.injector\.request_gbps\s*=\s*([0-9.eE+-]+)")
RESP_BW_RE = re.compile(r"stat\.node\.0\.injector\.response_gbps\s*=\s*([0-9.eE+-]+)")
AGG_BW_RE = re.compile(r"stat\.node\.0\.injector\.aggregate_gbps\s*=\s*([0-9.eE+-]+)")


def parse_req_bw_token(token: str) -> float:
    return float(token.replace("p", "."))


def parse_run(path: Path) -> dict[str, object] | None:
    match = OUT_FILE_RE.match(path.name)
    if match is None:
        return None

    text = path.read_text(errors="ignore")
    lat = LAT_RE.search(text)
    req_bw = REQ_BW_RE.search(text)
    resp_bw = RESP_BW_RE.search(text)
    agg_bw = AGG_BW_RE.search(text)
    if lat is None or req_bw is None or resp_bw is None or agg_bw is None:
        return None

    return {
        "load_pct": int(match.group("load_pct")),
        "requested_request_gbps_per_node": parse_req_bw_token(match.group("req_bw")),
        "request_gbps": float(req_bw.group(1)),
        "response_gbps": float(resp_bw.group(1)),
        "aggregate_bw_gbps": float(agg_bw.group(1)),
        "latency_cycles": float(lat.group(1)),
        "label": match.group("label") or "sample",
        "log_path": str(path),
    }


def collect_rows() -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for out_path in sorted(LOG_DIR.glob("run_load*_bw*.out")):
        row = parse_run(out_path)
        if row is not None:
            rows.append(row)
    return rows


def write_summary(rows: list[dict[str, object]], out_csv: Path) -> None:
    rows_sorted = sorted(rows, key=lambda row: (int(row["load_pct"]), float(row["aggregate_bw_gbps"])))
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
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
    norm = colors.Normalize(vmin=min(load_pcts), vmax=max(load_pcts))
    cmap = plt.get_cmap("viridis")

    fig, ax = plt.subplots(figsize=(7.0, 4.8), constrained_layout=True)
    for load_pct in load_pcts:
        series = sorted(
            [row for row in rows if int(row["load_pct"]) == load_pct],
            key=lambda row: float(row["aggregate_bw_gbps"]),
        )
        x = [float(row["aggregate_bw_gbps"]) for row in series]
        y = [float(row["latency_cycles"]) for row in series]
        ax.plot(x, y, linewidth=1.8, color=cmap(norm(load_pct)))

    ax.set_xlabel("Aggregate Bandwidth (Gbps)")
    ax.set_ylabel("Memory Access Latency (cycles)")
    ax.set_title("Single-Node Pointer-Chase Bandwidth-Latency Curves")
    ax.grid(True, alpha=0.3)
    cbar = fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=ax)
    cbar.set_label("Load %")

    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def main() -> int:
    rows = collect_rows()
    if not rows:
        print(f"[FAIL] No pointer-chase logs with complete stats found under {LOG_DIR}")
        return 1
    write_summary(rows, LOG_DIR / "pointer_chase_bw_latency.csv")
    plot_bw_latency(rows, LOG_DIR / "pointer_chase_bw_latency.png")
    print("[STATUS] Pointer-chase post-processing complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
