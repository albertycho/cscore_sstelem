#!/usr/bin/env python3
import csv
import re
from collections import defaultdict
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
LOG_DIR = SCRIPT_DIR / "logs"
BW_CSV = LOG_DIR / "expected_bandwidths.csv"
OUT_CSV = LOG_DIR / "bw_latency_summary.csv"
OUT_PNG = LOG_DIR / "bw_latency_curves.png"
OUT_PNG_ACTUAL = LOG_DIR / "bw_latency_curves_actual_cxl_bw.png"
OUT_PNG_SWITCH_OBS = LOG_DIR / "bw_latency_curves_observed_switch_total_bw.png"
OUT_PNG_SWITCH_BOTTLENECK = LOG_DIR / "bw_latency_curves_observed_switch_bottleneck_bw.png"
OUT_PNG_POOL_REQ_LINK = LOG_DIR / "bw_latency_curves_observed_pool_req_link_bw.png"
OUT_PNG_SWITCH_POOL_LINK_TOTAL = LOG_DIR / "bw_latency_curves_observed_switch_pool_link_total_bw.png"
OUT_PNG_ISO_TOTAL_BALANCE = LOG_DIR / "bw_latency_iso_total_balance.png"
OUT_HIST_COMPARE = LOG_DIR / "bw_latency_hist_compare_load050_mem050.png"
OUT_PNG_LOAD_SWEEP = LOG_DIR / "bw_latency_vs_loadpct.png"
OUT_PNG_MEM_SWEEP = LOG_DIR / "bw_latency_vs_mempct.png"

RUN_RE = re.compile(r"^run_load(\d{3})_mem(\d{3})_(no_rep|rep)\.out$")
STAT_MISS_RE = re.compile(r"^stat\.node\.(\d+)\.llc\.cxl_miss\s*=\s*([0-9]+)\s*$")
STAT_LAT_RE = re.compile(r"^stat\.node\.(\d+)\.llc\.avg_cxl_lat\s*=\s*([0-9.eE+-]+)\s*$")
STAT_HIST_RE = re.compile(r"^stat\.node\.(\d+)\.llc\.miss_lat_hist\s*=\s*\[(.*)\]\s*$")
STAT_POOL_LINK_UTIL_RE = re.compile(r"^stat\.pool\.(\d+)\.util\.req_link_avg\s*=\s*([0-9.eE+-]+)\s*$")
STAT_SWITCH_POOL_INGRESS_UTIL_RE = re.compile(r"^stat\.switch\.util\.pool_ingress_avg\s*=\s*([0-9.eE+-]+)\s*$")
STAT_SWITCH_HOST_TO_RE = re.compile(r"^stat\.switch\.bw\.host_to_switch_gbps\s*=\s*([0-9.eE+-]+)\s*$")
STAT_SWITCH_TO_HOST_RE = re.compile(r"^stat\.switch\.bw\.switch_to_host_gbps\s*=\s*([0-9.eE+-]+)\s*$")
STAT_SWITCH_TOTAL_RE = re.compile(r"^stat\.switch\.bw\.host_link_total_gbps\s*=\s*([0-9.eE+-]+)\s*$")
SIM_TIME_RE = re.compile(r"^Simulation is complete, simulated time:\s*([0-9.eE+-]+)\s*([a-zA-Z]+)\s*$")

# The load/store utilization sweep config uses 64B transfers, 25 cycles per
# transfer, and a 2.4 GHz pool clock. Convert utilization into GB/s using the
# same byte-per-cycle accounting as the switch stats.
POOL_LINK_BYTES_PER_TRANSFER = 64.0
POOL_LINK_BW_CYCLES = 25.0
POOL_CLOCK_GHZ = 2.4
POOL_REQ_LINK_GBPS_PER_PORT = (POOL_LINK_BYTES_PER_TRANSFER / POOL_LINK_BW_CYCLES) * POOL_CLOCK_GHZ


def load_bandwidth_table(path: Path):
    table = {}
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            load_pct = int(row["load_pct"])
            mem_pct = int(row["mem_pct"])
            load_cxl = float(row.get("load_cxl_gbps", 0.0))
            store_cxl = float(row.get("store_cxl_gbps", 0.0))
            host_to_switch = float(row.get("host_to_switch_gbps", 0.0))
            switch_to_host = float(row.get("switch_to_host_gbps", 0.0))
            host_link_total = float(row.get("host_link_total_gbps", 0.0))
            total_gbps = float(row.get("total_gbps", 0.0))
            if total_gbps <= 0.0:
                total_gbps = host_link_total if host_link_total > 0.0 else (load_cxl + store_cxl)
            cxl_total = load_cxl + store_cxl
            if cxl_total <= 0.0:
                cxl_total = host_link_total if host_link_total > 0.0 else total_gbps
            table[(load_pct, mem_pct)] = {
                "total_gbps": total_gbps,
                "cxl_total_gbps": cxl_total,
                "host_to_switch_gbps": host_to_switch,
                "switch_to_host_gbps": switch_to_host,
                "host_link_total_gbps": host_link_total,
            }
    return table


def to_seconds(value: float, unit: str) -> float:
    unit = unit.strip().lower()
    if unit in ("s", "sec", "secs", "second", "seconds"):
        return value
    if unit in ("ms",):
        return value * 1e-3
    if unit in ("us",):
        return value * 1e-6
    if unit in ("ns",):
        return value * 1e-9
    if unit in ("ps",):
        return value * 1e-12
    return 0.0


def parse_node_stats(out_path: Path):
    node_miss = {}
    node_lat = {}
    pool_req_link_util = {}
    sim_time_s = 0.0
    switch_bw = {
        "pool_ingress_avg_util": 0.0,
        "host_to_switch_gbps": 0.0,
        "switch_to_host_gbps": 0.0,
        "host_link_total_gbps": 0.0,
    }
    with out_path.open() as f:
        for raw in f:
            line = raw.strip()
            mm = STAT_MISS_RE.match(line)
            if mm:
                node = int(mm.group(1))
                node_miss[node] = int(mm.group(2))
                continue
            ml = STAT_LAT_RE.match(line)
            if ml:
                node = int(ml.group(1))
                node_lat[node] = float(ml.group(2))
                continue
            mu = STAT_POOL_LINK_UTIL_RE.match(line)
            if mu:
                pool_id = int(mu.group(1))
                pool_req_link_util[pool_id] = float(mu.group(2))
                continue
            ms_pool_util = STAT_SWITCH_POOL_INGRESS_UTIL_RE.match(line)
            if ms_pool_util:
                switch_bw["pool_ingress_avg_util"] = float(ms_pool_util.group(1))
                continue
            ms_h2s = STAT_SWITCH_HOST_TO_RE.match(line)
            if ms_h2s:
                switch_bw["host_to_switch_gbps"] = float(ms_h2s.group(1))
                continue
            ms_s2h = STAT_SWITCH_TO_HOST_RE.match(line)
            if ms_s2h:
                switch_bw["switch_to_host_gbps"] = float(ms_s2h.group(1))
                continue
            ms_tot = STAT_SWITCH_TOTAL_RE.match(line)
            if ms_tot:
                switch_bw["host_link_total_gbps"] = float(ms_tot.group(1))
                continue
            mt = SIM_TIME_RE.match(line)
            if mt:
                sim_time_s = to_seconds(float(mt.group(1)), mt.group(2))
    return node_miss, node_lat, pool_req_link_util, sim_time_s, switch_bw


def parse_run_name(name: str):
    m = RUN_RE.match(name)
    if m:
        return int(m.group(1)), int(m.group(2)), m.group(3)
    # Backward/forward compatibility with prefixed names:
    # e.g., run_peak020_load050_mem050_rep.out
    m2 = re.search(r"load(\d{3})_mem(\d{3})_(no_rep|rep)\.out$", name)
    if not m2:
        return None
    return int(m2.group(1)), int(m2.group(2)), m2.group(3)


def weighted_avg_latency(node_miss, node_lat):
    numer = 0.0
    denom = 0
    for node, miss in node_miss.items():
        lat = node_lat.get(node)
        if lat is None:
            continue
        numer += lat * miss
        denom += miss
    if denom == 0:
        return 0.0, 0
    return numer / float(denom), denom


def collect_rows(log_dir: Path, bw_table):
    rows = []
    for out_path in sorted(log_dir.glob("run_*.out")):
        parsed = parse_run_name(out_path.name)
        if parsed is None:
            continue
        load_pct, mem_pct, mode = parsed

        bw = bw_table.get((load_pct, mem_pct))
        if bw is None:
            continue

        node_miss, node_lat, pool_req_link_util, sim_time_s, switch_bw = parse_node_stats(out_path)
        avg_lat, miss_sum = weighted_avg_latency(node_miss, node_lat)
        actual_cxl_demand_gbps = 0.0
        if sim_time_s > 0.0:
            actual_cxl_demand_gbps = (float(miss_sum) * 64.0) / sim_time_s / 1e9
        observed_h2s = switch_bw["host_to_switch_gbps"]
        observed_s2h = switch_bw["switch_to_host_gbps"]
        observed_total = switch_bw["host_link_total_gbps"]
        observed_bottleneck = max(observed_h2s, observed_s2h)
        observed_balance_ratio = 0.0
        if observed_bottleneck > 0.0:
            observed_balance_ratio = min(observed_h2s, observed_s2h) / observed_bottleneck
        avg_pool_req_link_util = (
            sum(pool_req_link_util.values()) / float(len(pool_req_link_util))
            if pool_req_link_util
            else 0.0
        )
        pool_links_seen = len(pool_req_link_util)
        observed_pool_req_link_total_gbps = sum(pool_req_link_util.values()) * POOL_REQ_LINK_GBPS_PER_PORT
        observed_pool_req_link_avg_gbps = avg_pool_req_link_util * POOL_REQ_LINK_GBPS_PER_PORT
        observed_switch_pool_ingress_total_gbps = (
            switch_bw["pool_ingress_avg_util"] * float(pool_links_seen) * POOL_REQ_LINK_GBPS_PER_PORT
        )
        observed_switch_pool_link_total_gbps = (
            observed_pool_req_link_total_gbps + observed_switch_pool_ingress_total_gbps
        )
        rows.append(
            {
                "mode": mode,
                "load_pct": load_pct,
                "store_pct": 100 - load_pct,
                "mem_pct": mem_pct,
                "total_gbps": bw["total_gbps"],
                "cxl_total_gbps": bw["cxl_total_gbps"],
                "projected_host_to_switch_gbps": bw["host_to_switch_gbps"],
                "projected_switch_to_host_gbps": bw["switch_to_host_gbps"],
                "projected_host_link_total_gbps": bw["host_link_total_gbps"],
                "actual_cxl_demand_gbps": actual_cxl_demand_gbps,
                "observed_switch_host_to_switch_gbps": observed_h2s,
                "observed_switch_to_host_gbps": observed_s2h,
                "observed_switch_host_link_total_gbps": observed_total,
                "observed_switch_bottleneck_gbps": observed_bottleneck,
                "observed_switch_balance_ratio": observed_balance_ratio,
                "avg_pool_req_link_util": avg_pool_req_link_util,
                "observed_pool_req_link_total_gbps": observed_pool_req_link_total_gbps,
                "observed_pool_req_link_avg_gbps": observed_pool_req_link_avg_gbps,
                "observed_switch_pool_ingress_total_gbps": observed_switch_pool_ingress_total_gbps,
                "observed_switch_pool_link_total_gbps": observed_switch_pool_link_total_gbps,
                "pool_links_seen": pool_links_seen,
                "simulated_time_s": sim_time_s,
                "weighted_avg_cxl_lat_cycles": avg_lat,
                "cxl_miss_sum": miss_sum,
                "nodes_seen": len(node_miss),
                "log_file": out_path.name,
            }
        )
    rows.sort(key=lambda r: (r["mode"], r["load_pct"], r["total_gbps"]))
    return rows


def write_csv(rows, out_csv: Path):
    if not rows:
        return False
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    return True


def _plot_curves(rows, out_png: Path, x_key: str, x_label: str, title_suffix: str):
    try:
        import matplotlib.pyplot as plt
        import matplotlib as mpl
    except ModuleNotFoundError as exc:
        return f"Plot skipped: {exc}"

    grouped = defaultdict(lambda: defaultdict(list))
    for row in rows:
        grouped[row["mode"]][row["load_pct"]].append(row)

    all_load_pcts = sorted({row["load_pct"] for row in rows})
    cmap = plt.get_cmap("viridis")
    norm = mpl.colors.Normalize(vmin=min(all_load_pcts), vmax=max(all_load_pcts))
    color_for_load = {lp: cmap(norm(lp)) for lp in all_load_pcts}

    modes = ["no_rep", "rep"]
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), sharey=True, constrained_layout=True)

    for ax, mode in zip(axes, modes):
        data_by_load = grouped.get(mode, {})
        for load_pct in sorted(data_by_load.keys()):
            pts = sorted(data_by_load[load_pct], key=lambda r: r[x_key])
            x = [p[x_key] for p in pts]
            y = [p["weighted_avg_cxl_lat_cycles"] for p in pts]
            ax.plot(
                x,
                y,
                marker="o",
                linewidth=1.6,
                markersize=4,
                color=color_for_load[load_pct],
                label=f"L{load_pct}/S{100-load_pct}",
            )

        ax.set_title(f"{mode}: {title_suffix}")
        ax.set_xlabel(x_label)
        ax.grid(True, linestyle=":", linewidth=0.8, alpha=0.8)

    axes[0].set_ylabel("Weighted avg CXL miss latency (cycles)")

    sm = mpl.cm.ScalarMappable(norm=norm, cmap=cmap)
    sm.set_array([])
    cbar = fig.colorbar(sm, ax=axes, fraction=0.03, pad=0.02)
    cbar.set_label("Load ratio (%)")

    fig.savefig(out_png, dpi=180)
    plt.close(fig)
    return f"Wrote {out_png}"


def _plot_mode_panels(rows, out_png: Path, x_key: str, curve_key: str, x_label: str, title_suffix: str, curve_label: str):
    try:
        import matplotlib.pyplot as plt
        import matplotlib as mpl
    except ModuleNotFoundError as exc:
        return f"Plot skipped: {exc}"

    grouped = defaultdict(lambda: defaultdict(list))
    for row in rows:
        grouped[row["mode"]][int(row[curve_key])].append(row)

    curve_vals = sorted({int(row[curve_key]) for row in rows})
    cmap = plt.get_cmap("viridis")
    norm = mpl.colors.Normalize(vmin=min(curve_vals), vmax=max(curve_vals))
    color_for_curve = {cv: cmap(norm(cv)) for cv in curve_vals}

    modes = ["no_rep", "rep"]
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), sharey=True, constrained_layout=True)
    for ax, mode in zip(axes, modes):
        curves = grouped.get(mode, {})
        for cv in sorted(curves.keys()):
            pts = sorted(curves[cv], key=lambda r: float(r[x_key]))
            x = [float(p[x_key]) for p in pts]
            y = [float(p["weighted_avg_cxl_lat_cycles"]) for p in pts]
            ax.plot(
                x,
                y,
                marker="o",
                linewidth=1.6,
                markersize=4,
                color=color_for_curve[cv],
                label=f"{curve_label}{cv}",
            )

        ax.set_title(f"{mode}: {title_suffix}")
        ax.set_xlabel(x_label)
        ax.grid(True, linestyle=":", linewidth=0.8, alpha=0.8)

    axes[0].set_ylabel("Weighted avg CXL miss latency (cycles)")
    sm = mpl.cm.ScalarMappable(norm=norm, cmap=cmap)
    sm.set_array([])
    cbar = fig.colorbar(sm, ax=axes, fraction=0.03, pad=0.02)
    cbar.set_label(f"{curve_label.strip()} value")

    fig.savefig(out_png, dpi=180)
    plt.close(fig)
    return f"Wrote {out_png}"


def plot(
    rows,
    out_png_projected: Path,
    out_png_actual: Path,
    out_png_switch_obs: Path,
    out_png_switch_bottleneck: Path,
    out_png_pool_req_link: Path,
    out_png_switch_pool_link_total: Path,
):
    msg1 = _plot_curves(
        rows,
        out_png_projected,
        x_key="total_gbps",
        x_label="Projected total bandwidth (GB/s)",
        title_suffix="CXL Latency vs Projected Total Bandwidth",
    )
    msg2 = _plot_curves(
        rows,
        out_png_actual,
        x_key="actual_cxl_demand_gbps",
        x_label="Observed CXL demand bandwidth (GB/s)",
        title_suffix="CXL Latency vs Observed CXL Demand Bandwidth",
    )
    msg3 = _plot_curves(
        rows,
        out_png_switch_obs,
        x_key="observed_switch_host_link_total_gbps",
        x_label="Observed aggregate switch host-link BW (GB/s)",
        title_suffix="CXL Latency vs Observed Aggregate Switch Host-Link Bandwidth",
    )
    msg4 = _plot_curves(
        rows,
        out_png_switch_bottleneck,
        x_key="observed_switch_bottleneck_gbps",
        x_label="Observed directional bottleneck BW (GB/s)",
        title_suffix="CXL Latency vs Observed Directional Bottleneck Bandwidth",
    )
    msg5 = _plot_curves(
        rows,
        out_png_pool_req_link,
        x_key="observed_pool_req_link_total_gbps",
        x_label="Observed aggregate pool request-link BW use (GB/s)",
        title_suffix="CXL Latency vs Observed Aggregate Pool Request-Link Bandwidth",
    )
    msg6 = _plot_curves(
        rows,
        out_png_switch_pool_link_total,
        x_key="observed_switch_pool_link_total_gbps",
        x_label="Observed aggregate switch-pool link BW use (GB/s)",
        title_suffix="CXL Latency vs Observed Aggregate Switch-Pool Link Bandwidth",
    )
    return msg1, msg2, msg3, msg4, msg5, msg6


def plot_iso_total_balance(rows, out_png: Path, bin_width_gbps: float = 0.25):
    try:
        import matplotlib.pyplot as plt
        import matplotlib as mpl
    except ModuleNotFoundError as exc:
        return f"Plot skipped: {exc}"

    modes = ["no_rep", "rep"]
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), sharey=True, constrained_layout=True)

    for ax, mode in zip(axes, modes):
        mode_rows = [r for r in rows if r["mode"] == mode]
        buckets = defaultdict(list)
        for r in mode_rows:
            total = float(r["observed_switch_host_link_total_gbps"])
            if total <= 0.0:
                continue
            b = int(total / bin_width_gbps)
            buckets[b].append(r)

        xs = []
        ys = []
        cs = []
        for b in sorted(buckets.keys()):
            bucket = buckets[b]
            if len(bucket) < 2:
                continue
            center = (b + 0.5) * bin_width_gbps
            for r in bucket:
                h2s = float(r["observed_switch_host_to_switch_gbps"])
                s2h = float(r["observed_switch_to_host_gbps"])
                denom = max(h2s, s2h)
                if denom <= 0.0:
                    continue
                balance = min(h2s, s2h) / denom
                xs.append(balance)
                ys.append(float(r["weighted_avg_cxl_lat_cycles"]))
                cs.append(center)

        if xs:
            vmin = min(cs)
            vmax = max(cs)
            if vmax <= vmin:
                vmax = vmin + 1e-9
            norm = mpl.colors.Normalize(vmin=vmin, vmax=vmax)
            sc = ax.scatter(xs, ys, c=cs, cmap="viridis", norm=norm, s=32, alpha=0.9)
            cbar = fig.colorbar(sc, ax=ax, fraction=0.03, pad=0.02)
            cbar.set_label("Aggregate BW bin center (GB/s)")

        ax.set_title(f"{mode}: Latency vs Balance (Same Aggregate BW Bins)")
        ax.set_xlabel("Directional balance min(h2s,s2h)/max(h2s,s2h)")
        ax.grid(True, linestyle=":", linewidth=0.8, alpha=0.8)

    axes[0].set_ylabel("Weighted avg CXL miss latency (cycles)")
    fig.savefig(out_png, dpi=180)
    plt.close(fig)
    return f"Wrote {out_png}"


def _sum_hist(dst, src):
    if len(dst) < len(src):
        dst.extend([0] * (len(src) - len(dst)))
    for i, v in enumerate(src):
        dst[i] += v


def parse_aggregated_miss_hist(out_path: Path):
    agg = []
    with out_path.open() as f:
        for raw in f:
            m = STAT_HIST_RE.match(raw.strip())
            if not m:
                continue
            payload = m.group(2).strip()
            if not payload:
                hist = []
            else:
                hist = [int(x.strip()) for x in payload.split(",") if x.strip()]
            _sum_hist(agg, hist)
    return agg


def _cdf_from_hist(hist):
    total = float(sum(hist))
    if total <= 0:
        return [0.0 for _ in hist]
    run = 0.0
    out = []
    for v in hist:
        run += float(v)
        out.append(run / total)
    return out


def plot_hist_compare(log_dir: Path, out_png: Path, load_pct: int = 50, mem_pct: int = 50):
    try:
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:
        return f"Histogram plot skipped: {exc}"

    no_rep_path = None
    rep_path = None
    for p in sorted(log_dir.glob("run_*")):
        parsed = parse_run_name(p.name)
        if parsed is None:
            continue
        lp, mp, mode = parsed
        if lp == load_pct and mp == mem_pct:
            if mode == "no_rep":
                no_rep_path = p
            elif mode == "rep":
                rep_path = p

    if no_rep_path is None or rep_path is None:
        return (
            "Histogram plot skipped: missing run files "
            f"(load={load_pct}, mem={mem_pct})"
        )

    no_rep_hist = parse_aggregated_miss_hist(no_rep_path)
    rep_hist = parse_aggregated_miss_hist(rep_path)
    max_len = max(len(no_rep_hist), len(rep_hist))
    if max_len == 0:
        return "Histogram plot skipped: no miss_lat_hist entries found in selected runs"
    if len(no_rep_hist) < max_len:
        no_rep_hist.extend([0] * (max_len - len(no_rep_hist)))
    if len(rep_hist) < max_len:
        rep_hist.extend([0] * (max_len - len(rep_hist)))

    x_ns = [i * 10 for i in range(max_len)]
    no_rep_cdf = _cdf_from_hist(no_rep_hist)
    rep_cdf = _cdf_from_hist(rep_hist)

    fig, axes = plt.subplots(1, 2, figsize=(14, 5), constrained_layout=True)

    x_min_ns = 200

    axes[0].step(x_ns, no_rep_hist, where="mid", label="no_rep", linewidth=1.8)
    axes[0].step(x_ns, rep_hist, where="mid", label="rep", linewidth=1.8)
    axes[0].set_title(f"Latency Histogram (Load={load_pct}, Mem={mem_pct})")
    axes[0].set_xlabel("Miss latency bin (ns)")
    axes[0].set_ylabel("Miss count (sum over all nodes)")
    axes[0].set_xlim(left=x_min_ns)
    axes[0].grid(True, linestyle=":", linewidth=0.8, alpha=0.8)
    axes[0].legend(loc="best")

    axes[1].plot(x_ns, no_rep_cdf, label="no_rep", linewidth=1.8)
    axes[1].plot(x_ns, rep_cdf, label="rep", linewidth=1.8)
    axes[1].set_title(f"Latency CDF (Load={load_pct}, Mem={mem_pct})")
    axes[1].set_xlabel("Miss latency bin (ns)")
    axes[1].set_ylabel("CDF")
    axes[1].set_xlim(left=x_min_ns)
    axes[1].set_ylim(0.0, 1.0)
    axes[1].grid(True, linestyle=":", linewidth=0.8, alpha=0.8)
    axes[1].legend(loc="best")

    fig.savefig(out_png, dpi=180)
    plt.close(fig)
    return f"Wrote {out_png}"


def main():
    if not BW_CSV.exists():
        print(f"Missing bandwidth CSV: {BW_CSV}")
        return 1

    bw_table = load_bandwidth_table(BW_CSV)
    rows = collect_rows(LOG_DIR, bw_table)
    if not rows:
        print(f"No matching run .out files found in {LOG_DIR}")
        return 1

    write_csv(rows, OUT_CSV)
    print(f"Wrote {OUT_CSV}")
    msg1, msg2, msg3, msg4, msg5, msg6 = plot(
        rows,
        OUT_PNG,
        OUT_PNG_ACTUAL,
        OUT_PNG_SWITCH_OBS,
        OUT_PNG_SWITCH_BOTTLENECK,
        OUT_PNG_POOL_REQ_LINK,
        OUT_PNG_SWITCH_POOL_LINK_TOTAL,
    )
    print(msg1)
    print(msg2)
    print(msg3)
    print(msg4)
    print(msg5)
    print(msg6)
    print(plot_iso_total_balance(rows, OUT_PNG_ISO_TOTAL_BALANCE))
    print(
        _plot_mode_panels(
            rows,
            OUT_PNG_LOAD_SWEEP,
            x_key="observed_switch_bottleneck_gbps",
            curve_key="mem_pct",
            x_label="Observed directional bottleneck BW (GB/s)",
            title_suffix="CXL Latency vs Directional Bottleneck BW (curves by Mem Ratio)",
            curve_label="M",
        )
    )
    print(
        _plot_mode_panels(
            rows,
            OUT_PNG_MEM_SWEEP,
            x_key="mem_pct",
            curve_key="load_pct",
            x_label="Mem ratio (%)",
            title_suffix="CXL Latency vs Mem Ratio (curves by Load Ratio)",
            curve_label="L",
        )
    )
    print(plot_hist_compare(LOG_DIR, OUT_HIST_COMPARE, load_pct=50, mem_pct=50))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
