#!/usr/bin/env python3
import csv
from pathlib import Path

# use: python3 experiments/pointer_chase_replication/plot_node_scaling.py

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs_node_scaling"


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def main() -> int:
    try:
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:
        print(f"[FAIL] matplotlib not available: {exc}")
        return 1

    csv_path = LOG_DIR / "node_scaling_summary.csv"
    if not csv_path.exists():
        print(f"[FAIL] Missing summary: {csv_path}")
        return 1

    rows = read_rows(csv_path)
    if not rows:
        print(f"[FAIL] Empty summary: {csv_path}")
        return 1

    title_map = {
        "no_rep": "No Replication (1 pool)",
        "rep2": "Replication (2 pools)",
    }
    configs = [config for config in ("no_rep", "rep2") if any(row["config"] == config for row in rows)]

    fig, ax = plt.subplots(figsize=(7.0, 4.5))
    for config in configs:
        series = sorted(
            [row for row in rows if row["config"] == config],
            key=lambda row: int(row["num_nodes"]),
        )
        x = [int(row["num_nodes"]) for row in series]
        y = [float(row["latency_cycles"]) for row in series]
        ax.plot(x, y, marker="o", linewidth=2.0, label=title_map.get(config, config))

    load_pct = rows[0].get("load_pct", "?")
    bw = rows[0].get("requested_request_gbps_per_node", "?")
    ax.set_xlabel("Node Count")
    ax.set_ylabel("Memory Access Latency (cycles)")
    ax.set_title(f"Node-Count Scaling (load={load_pct}%, {bw} Gbps/node)")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    fig.tight_layout()

    out_png = LOG_DIR / "node_scaling_latency.png"
    fig.savefig(out_png, dpi=220)
    plt.close(fig)
    print(f"[STATUS] Wrote plot: {out_png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
