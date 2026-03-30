#!/usr/bin/env python3
import csv
import re
from pathlib import Path
from typing import Dict, List, Optional

# use: python3 experiments/pointer_chase_replication/plot_bw_latency.py

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs"

OUT_FILE_RE = re.compile(
    r"run_(?P<config>.+?)_load(?P<load_pct>\d{3})(?:_(?P<label>[^_]+))?_bw(?P<req_bw>[0-9]+p[0-9]+)\.out$"
)

LAT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.avg_load_issue_to_complete_lat\s*=\s*([0-9.eE+-]+)")
LAT_COUNT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.load_issue_to_complete_count\s*=\s*([0-9.eE+-]+)")
REQ_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.request_gbps\s*=\s*([0-9.eE+-]+)")
RESP_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.response_gbps\s*=\s*([0-9.eE+-]+)")
AGG_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.aggregate_gbps\s*=\s*([0-9.eE+-]+)")


def parse_scalars(pattern: re.Pattern[str], text: str) -> Dict[int, float]:
    return {int(m.group(1)): float(m.group(2)) for m in pattern.finditer(text)}


def parse_req_bw_token(token: str) -> float:
    return float(token.replace("p", "."))


def derive_class_bandwidths(load_pct: int, request_gbps: float, response_gbps: float) -> Dict[str, float]:
    load_frac = load_pct / 100.0
    avg_request_bytes = 64.0 - (56.0 * load_frac)
    load_request_frac = 0.0 if avg_request_bytes <= 0.0 else (8.0 * load_frac) / avg_request_bytes
    load_request_gbps = request_gbps * load_request_frac
    store_gbps = max(0.0, request_gbps - load_request_gbps)
    load_gbps = load_request_gbps + response_gbps
    aggregate_gbps = request_gbps + response_gbps
    return {
        "load_bw_gbps": load_gbps,
        "store_bw_gbps": store_gbps,
        "aggregate_bw_gbps": aggregate_gbps,
    }


def parse_run(path: Path) -> Optional[Dict[str, object]]:
    match = OUT_FILE_RE.match(path.name)
    if match is None:
        return None

    config_name = match.group("config")
    load_pct = int(match.group("load_pct"))
    label = match.group("label") or "sample"
    requested_bw_gbps = parse_req_bw_token(match.group("req_bw"))

    text = path.read_text(errors="ignore")
    lat_by_node = parse_scalars(LAT_RE, text)
    lat_count_by_node = parse_scalars(LAT_COUNT_RE, text)
    req_bw_by_node = parse_scalars(REQ_BW_RE, text)
    resp_bw_by_node = parse_scalars(RESP_BW_RE, text)
    agg_bw_by_node = parse_scalars(AGG_BW_RE, text)
    if not lat_by_node or not lat_count_by_node or not req_bw_by_node or not resp_bw_by_node or not agg_bw_by_node:
        return None

    weighted_lat_sum = 0.0
    weighted_lat_count = 0.0
    for node, lat in lat_by_node.items():
        count = lat_count_by_node.get(node, 0.0)
        weighted_lat_sum += lat * count
        weighted_lat_count += count
    if weighted_lat_count <= 0.0:
        return None

    total_request_gbps = sum(req_bw_by_node.values())
    total_response_gbps = sum(resp_bw_by_node.values())
    total_aggregate_gbps = sum(agg_bw_by_node.values())

    row: Dict[str, object] = {
        "config": config_name,
        "load_pct": float(load_pct),
        "requested_request_gbps_per_node": requested_bw_gbps,
        "request_gbps": total_request_gbps,
        "response_gbps": total_response_gbps,
        "aggregate_bw_gbps": total_aggregate_gbps,
        "latency_cycles": weighted_lat_sum / weighted_lat_count,
    }
    row.update(derive_class_bandwidths(load_pct, total_request_gbps, total_response_gbps))
    row["label"] = label
    row["log_path"] = str(path)
    return row


def collect_rows(log_dir: Path) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    for out_path in sorted(log_dir.glob("run_*_load*_bw*.out")):
        row = parse_run(out_path)
        if row is not None:
            rows.append(row)
    return rows


def write_summary(rows: List[Dict[str, object]], out_csv: Path) -> None:
    rows_sorted = sorted(
        rows,
        key=lambda r: (str(r["config"]), int(r["load_pct"]), float(r["aggregate_bw_gbps"])),
    )
    fieldnames = [
        "config",
        "load_pct",
        "load_bw_gbps",
        "store_bw_gbps",
        "aggregate_bw_gbps",
        "latency_cycles",
        "requested_request_gbps_per_node",
        "request_gbps",
        "response_gbps",
        "label",
        "log_path",
    ]
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows_sorted)
    print(f"[STATUS] Wrote summary: {out_csv}")


def plot_bw_latency(rows: List[Dict[str, object]], out_png: Path) -> None:
    import matplotlib.pyplot as plt
    from matplotlib import colors
    from matplotlib.cm import ScalarMappable

    title_map = {
        "no_rep": "No Replication (1 pool)",
        "rep2": "Replication (2 pools)",
    }
    load_pcts = sorted({int(row["load_pct"]) for row in rows})
    configs = [config for config in ("no_rep", "rep2") if any(str(row["config"]) == config for row in rows)]
    norm = colors.Normalize(vmin=min(load_pcts), vmax=max(load_pcts))
    cmap = plt.get_cmap("viridis")

    fig, axes = plt.subplots(1, len(configs), figsize=(12.0, 5.2), sharey=True, constrained_layout=True)
    if len(configs) == 1:
        axes = [axes]

    for ax, config_name in zip(axes, configs):
        for load_pct in load_pcts:
            series = sorted(
                [
                    row for row in rows
                    if str(row["config"]) == config_name and int(row["load_pct"]) == load_pct
                ],
                key=lambda r: float(r["aggregate_bw_gbps"]),
            )
            if not series:
                continue
            x = [float(row["aggregate_bw_gbps"]) for row in series]
            y = [float(row["latency_cycles"]) for row in series]
            color = cmap(norm(load_pct))
            ax.plot(
                x,
                y,
                linewidth=1.8,
                linestyle="-",
                color=color,
            )
        ax.set_xlabel("Aggregate Bandwidth (Gbps)")
        ax.set_title(title_map.get(config_name, config_name))
        ax.set_ylim(0, 3000)
        ax.grid(True, alpha=0.3)

    axes[0].set_ylabel("Memory Access Latency (cycles)")
    fig.suptitle("8-Node Pointer-Chase Bandwidth-Latency Curves")

    cbar = fig.colorbar(ScalarMappable(norm=norm, cmap=cmap), ax=axes)
    cbar.set_label("Load %")

    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")


def main() -> int:
    rows = collect_rows(LOG_DIR)
    if not rows:
        print(f"[FAIL] No pointer-chase replication logs with complete stats found under {LOG_DIR}")
        return 1

    out_csv = LOG_DIR / "pointer_chase_replication_bw_latency.csv"
    write_summary(rows, out_csv)

    try:
        plot_bw_latency(rows, LOG_DIR / "pointer_chase_replication_bw_latency.png")
    except ModuleNotFoundError as exc:
        print(f"[FAIL] matplotlib not available: {exc}")
        return 1

    print("[STATUS] Pointer-chase replication post-processing complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
