#!/usr/bin/env python3
import csv
import os
import re
import subprocess
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path("/shared/kshan/CXL_sst_traces")
LOG_ROOT = SCRIPT_DIR / "diag_logs"
CONFIG_PATH = SCRIPT_DIR / "cxl_config.csv"

SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen"
GEN_SRC = REPO_ROOT / "scripts" / "generate_synth_trace.cpp"

MPI_RANKS = 8
NUM_NODES = 8
POOL_NODE_ID_BASE = 100

LOAD_PCT = 10
MEM_PCTS = [10, 70, 100]
RUN_REPLICATION = True

NUM_INSTRS = 4_000_000
WARM_CACHE_INSTS = 200_000
CXL_PCT = 100
SEED = 0x12345678

SIM_NO_REP = SCRIPT_DIR / "pool_sweep.py"
SIM_REP = SCRIPT_DIR / "pool_sweep_replication.py"

STAT_RE = re.compile(r"^(stat\.[^=]+?)\s*=\s*(.+?)\s*$")


def status(msg: str) -> None:
    print(f"[DIAG] {msg}")


def build_generator() -> None:
    status("Building trace generator")
    GEN_BIN.parent.mkdir(parents=True, exist_ok=True)
    subprocess.check_call(
        [
            "g++",
            "-O3",
            "-std=c++17",
            "-I",
            str(REPO_ROOT / "inc"),
            str(GEN_SRC),
            "-o",
            str(GEN_BIN),
        ]
    )


def write_cxl_config(path: Path) -> None:
    status(f"Writing CXL config: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    cxl_base = 64 << 30
    cxl_size = 8 << 20
    with path.open("w") as f:
        f.write("# node_id,start,size,type,target\n")
        for node in range(NUM_NODES):
            f.write(f"{node},0x{cxl_base:x},0x{cxl_size:x},pool,{POOL_NODE_ID_BASE}\n")


def generate_trace(mem_pct: int) -> Path:
    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    trace_name = f"synth_diag_load{LOAD_PCT:03d}_mem{mem_pct:03d}.champsim.trace"
    trace_path = TRACE_ROOT / trace_name
    status(f"Generating trace {trace_name}")
    cmd = [
        str(GEN_BIN),
        "--out-dir", str(TRACE_ROOT),
        "--out-name", trace_name,
        "--num-instrs", str(NUM_INSTRS),
        "--warm-cache-instrs", str(WARM_CACHE_INSTS),
        "--mem-pct", str(mem_pct),
        "--load-pct", str(LOAD_PCT),
        "--cxl-pct", str(CXL_PCT),
        "--seed", hex(SEED),
    ]
    out = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True).stdout
    for line in out.splitlines():
        if line.startswith("main_loop_loads=") or line.startswith("Projected BW"):
            status(f"{trace_name}: {line}")
    return trace_path


def run_case(label: str, sim_script: Path, mem_pct: int, trace_path: Path) -> Path:
    case_dir = LOG_ROOT / f"{label}_mem{mem_pct:03d}"
    case_dir.mkdir(parents=True, exist_ok=True)
    out_path = case_dir / "stdout.log"
    err_path = case_dir / "stderr.log"

    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(CONFIG_PATH)
    env["LIGHTWEIGHT_OUTPUT"] = "1"
    env["PRINT_LAT_HIST"] = "1"
    env["FABRIC_DIAG_OUTPUT"] = "1"

    status(f"Running {label} mem_pct={mem_pct} -> {out_path}")
    with out_path.open("w") as out_f, err_path.open("w") as err_f:
        subprocess.check_call(
            ["mpirun", "-n", str(MPI_RANKS), SST_BIN, str(sim_script)],
            cwd=str(SCRIPT_DIR),
            env=env,
            stdout=out_f,
            stderr=err_f,
        )
    return out_path


def parse_stats(path: Path) -> dict:
    stats = {}
    with path.open() as f:
        for raw in f:
            m = STAT_RE.match(raw.strip())
            if not m:
                continue
            key, val = m.group(1), m.group(2)
            stats[key] = val
    return stats


def f64(stats: dict, key: str, default: float = 0.0) -> float:
    try:
        return float(stats.get(key, default))
    except ValueError:
        return default


def u64(stats: dict, key: str, default: int = 0) -> int:
    try:
        return int(float(stats.get(key, default)))
    except ValueError:
        return default


def summarize_one(label: str, mem_pct: int, stats: dict) -> tuple[list[dict], dict]:
    node_rows = []
    node_ids = []
    for i in range(NUM_NODES):
        k = f"stat.node.{i}.llc.cxl_miss"
        if k in stats:
            node_ids.append(i)

    total_cxl_miss = 0
    weighted_cxl_lat = 0.0
    total_miss = 0
    weighted_miss_lat = 0.0
    total_outstanding = 0

    for i in node_ids:
        prefix = f"stat.node.{i}."
        cxl_miss = u64(stats, prefix + "llc.cxl_miss")
        tmiss = u64(stats, prefix + "llc.total_miss")
        avg_cxl = f64(stats, prefix + "llc.avg_cxl_lat")
        avg_miss = f64(stats, prefix + "llc.avg_miss_lat")
        outst = u64(stats, prefix + "llc.pool_outstanding")
        fw_in = f64(stats, prefix + "fabric.ingress_wait_avg_cycles")
        fw_out = f64(stats, prefix + "fabric.egress_wait_avg_cycles")
        fw_in_max = u64(stats, prefix + "fabric.ingress_wait_max_cycles")
        fw_out_max = u64(stats, prefix + "fabric.egress_wait_max_cycles")

        node_rows.append(
            {
                "case": label,
                "mem_pct": mem_pct,
                "node": i,
                "cxl_miss": cxl_miss,
                "total_miss": tmiss,
                "avg_cxl_lat_cycles": avg_cxl,
                "avg_miss_lat_cycles": avg_miss,
                "pool_outstanding": outst,
                "fabric_ingress_wait_avg_cycles": fw_in,
                "fabric_egress_wait_avg_cycles": fw_out,
                "fabric_ingress_wait_max_cycles": fw_in_max,
                "fabric_egress_wait_max_cycles": fw_out_max,
            }
        )

        total_cxl_miss += cxl_miss
        weighted_cxl_lat += avg_cxl * cxl_miss
        total_miss += tmiss
        weighted_miss_lat += avg_miss * tmiss
        total_outstanding += outst

    agg = {
        "case": label,
        "mem_pct": mem_pct,
        "nodes_with_stats": len(node_ids),
        "cxl_miss_sum": total_cxl_miss,
        "total_miss_sum": total_miss,
        "avg_cxl_lat_weighted_cycles": (weighted_cxl_lat / total_cxl_miss) if total_cxl_miss else 0.0,
        "avg_miss_lat_weighted_cycles": (weighted_miss_lat / total_miss) if total_miss else 0.0,
        "pool_outstanding_sum": total_outstanding,
        "switch_pool_ingress_util_avg": f64(stats, "stat.switch.util.pool_ingress_avg"),
        "switch_node_ingress_util_avg": f64(stats, "stat.switch.util.node_ingress_avg"),
        "switch_pool_ingress_wait_avg_cycles": f64(stats, "stat.switch.fabric.pool_ingress_wait_avg_cycles"),
        "switch_pool_egress_wait_avg_cycles": f64(stats, "stat.switch.fabric.pool_egress_wait_avg_cycles"),
        "switch_pool_ingress_wait_max_cycles": u64(stats, "stat.switch.fabric.pool_ingress_wait_max_cycles"),
        "switch_pool_egress_wait_max_cycles": u64(stats, "stat.switch.fabric.pool_egress_wait_max_cycles"),
    }
    return node_rows, agg


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    LOG_ROOT.mkdir(parents=True, exist_ok=True)
    build_generator()
    write_cxl_config(CONFIG_PATH)

    cases = [("no_rep", SIM_NO_REP)]
    if RUN_REPLICATION:
        cases.append(("rep", SIM_REP))

    per_node_rows = []
    agg_rows = []

    for mem_pct in MEM_PCTS:
        trace_path = generate_trace(mem_pct)
        for label, sim_script in cases:
            out_path = run_case(label, sim_script, mem_pct, trace_path)
            stats = parse_stats(out_path)
            rows, agg = summarize_one(label, mem_pct, stats)
            per_node_rows.extend(rows)
            agg_rows.append(agg)
            status(
                f"{label} mem={mem_pct}: "
                f"weighted_avg_cxl_lat={agg['avg_cxl_lat_weighted_cycles']:.3f} "
                f"cxl_miss_sum={agg['cxl_miss_sum']} "
                f"pool_outstanding_sum={agg['pool_outstanding_sum']}"
            )

    per_node_csv = LOG_ROOT / "diag_per_node.csv"
    agg_csv = LOG_ROOT / "diag_aggregate.csv"
    write_csv(per_node_csv, per_node_rows)
    write_csv(agg_csv, agg_rows)
    status(f"Wrote {per_node_csv}")
    status(f"Wrote {agg_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
