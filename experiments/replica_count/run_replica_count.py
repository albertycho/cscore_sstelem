#!/usr/bin/env python3
import concurrent.futures
import csv
import os
import re
import subprocess
from pathlib import Path
from typing import Dict, List, Tuple

# use: python3 experiments/replica_count/run_replica_count.py

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path("/shared/kshan/CXL_sst_traces")
OUTPUT_ROOT = SCRIPT_DIR / "logs"
CONFIG_PATH = SCRIPT_DIR / "cxl_config.csv"
SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen"
GEN_SRC = REPO_ROOT / "scripts" / "generate_synth_trace.cpp"

SIM_NO_REP = SCRIPT_DIR / "pool_replica_count_no_rep.py"
SIM_REP = SCRIPT_DIR / "pool_replica_count_rep.py"

MPI_RANKS = 8
NUM_NODES = 8
POOL_NODE_ID_BASE = 100

MAX_CORE_BUDGET = 160
MAX_PARALLEL = max(1, MAX_CORE_BUDGET // MPI_RANKS)

REPLICA_COUNTS = [1, 2, 4, 8]

# Fixed 50/50 configuration
LOAD_PCT = 50
MEM_PCT = 50
CXL_PCT = 100
NUM_INSTRS = 4_000_000
WARM_CACHE_INSTS = 200_000
WARMUP_MAIN_INSTS = 100_000
SIM_INSTS = 2_000_000
SEED = 0x12345678

LIGHTWEIGHT_OUTPUT = "1"
PRINT_LAT_HIST = "1"

TRACE_NAME = f"synth_load{LOAD_PCT:03d}_mem{MEM_PCT:03d}.champsim.trace"
TRACE_PATH = TRACE_ROOT / TRACE_NAME


def build_generator() -> None:
    print("[STATUS] Building trace generator...")
    GEN_BIN.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "g++",
        "-O3",
        "-std=c++17",
        "-I",
        str(REPO_ROOT / "inc"),
        str(GEN_SRC),
        "-o",
        str(GEN_BIN),
    ]
    subprocess.check_call(cmd)


def generate_trace() -> None:
    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    print(f"[STATUS] Generating fixed trace {TRACE_NAME} (load={LOAD_PCT} mem={MEM_PCT})")
    cmd = [
        str(GEN_BIN),
        "--out-dir",
        str(TRACE_ROOT),
        "--out-name",
        TRACE_NAME,
        "--num-instrs",
        str(NUM_INSTRS),
        "--warm-cache-instrs",
        str(WARM_CACHE_INSTS),
        "--mem-pct",
        str(MEM_PCT),
        "--load-pct",
        str(LOAD_PCT),
        "--cxl-pct",
        str(CXL_PCT),
        "--seed",
        hex(SEED),
    ]
    result = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True)
    for line in result.stdout.splitlines():
        if line.startswith("main_loop_loads=") or line.startswith("projected_bandwidth_gbps"):
            print(f"[STATUS] {line}")


def write_cxl_config(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"[STATUS] Writing CXL config: {path}")
    cxl_base = 64 << 30
    cxl_ws = 8 << 20
    with path.open("w") as f:
        f.write("# node_id,start,size,type,target\n")
        for node in range(NUM_NODES):
            # Route pool-backed region; switch policy decides pool port.
            f.write(f"{node},0x{cxl_base:x},0x{cxl_ws:x},pool,{POOL_NODE_ID_BASE}\n")


def run_sst(sim_script: Path, replicas: int, out_path: Path, err_path: Path) -> Tuple[int, Path]:
    env = os.environ.copy()
    env["TRACE_PATH"] = str(TRACE_PATH)
    env["CXL_CONFIG_PATH"] = str(CONFIG_PATH)
    env["NUM_NODES"] = str(NUM_NODES)
    env["NUM_REPLICAS"] = str(replicas)
    env["POOL_NODE_ID_BASE"] = str(POOL_NODE_ID_BASE)
    env["LIGHTWEIGHT_OUTPUT"] = LIGHTWEIGHT_OUTPUT
    env["PRINT_LAT_HIST"] = PRINT_LAT_HIST
    env["WARM_CACHE_INSTS"] = str(WARM_CACHE_INSTS)
    env["WARMUP_MAIN_INSTS"] = str(WARMUP_MAIN_INSTS)
    env["SIM_INSTS"] = str(SIM_INSTS)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)

    print(f"[STATUS] Launching replicas={replicas} {sim_script.name} -> {out_path.name}")
    with out_path.open("w") as out_f, err_path.open("w") as err_f:
        proc = subprocess.run(
            ["mpirun", "-n", str(MPI_RANKS), SST_BIN, str(sim_script)],
            cwd=str(SCRIPT_DIR),
            env=env,
            stdout=out_f,
            stderr=err_f,
        )
    return proc.returncode, out_path


WALL_RE = re.compile(r"stat\.node\.(\d+)\.walltime_s = ([0-9.eE+-]+)")
CXL_LAT_RE = re.compile(r"stat\.node\.(\d+)\.llc\.avg_cxl_lat = ([0-9.eE+-]+)")
CXL_MISS_RE = re.compile(r"stat\.node\.(\d+)\.llc\.cxl_miss = ([0-9.eE+-]+)")
SW_REPL_RE = re.compile(r"stat\.switch\.replicated_messages = ([0-9.eE+-]+)")
SW_POOL_UTIL_RE = re.compile(r"stat\.switch\.util\.pool_ingress_avg = ([0-9.eE+-]+)")
POOL_REQ_UTIL_RE = re.compile(r"stat\.pool\.(\d+)\.util\.req_link_avg = ([0-9.eE+-]+)")


def parse_run_metrics(path: Path) -> Dict[str, float]:
    txt = path.read_text(errors="ignore")

    walls = [float(m.group(2)) for m in WALL_RE.finditer(txt)]
    cxl_lat = {int(m.group(1)): float(m.group(2)) for m in CXL_LAT_RE.finditer(txt)}
    cxl_miss = {int(m.group(1)): float(m.group(2)) for m in CXL_MISS_RE.finditer(txt)}
    sw_repl = [float(m.group(1)) for m in SW_REPL_RE.finditer(txt)]
    sw_pool_util = [float(m.group(1)) for m in SW_POOL_UTIL_RE.finditer(txt)]
    pool_req_utils = [float(m.group(2)) for m in POOL_REQ_UTIL_RE.finditer(txt)]

    weighted_cxl_lat = 0.0
    total_cxl_miss = 0.0
    for node, miss in cxl_miss.items():
        lat = cxl_lat.get(node, 0.0)
        weighted_cxl_lat += miss * lat
        total_cxl_miss += miss
    avg_cxl_lat_weighted = (weighted_cxl_lat / total_cxl_miss) if total_cxl_miss > 0 else 0.0

    return {
        "max_node_walltime_s": max(walls) if walls else 0.0,
        "weighted_avg_cxl_lat_cycles": avg_cxl_lat_weighted,
        "total_cxl_miss": total_cxl_miss,
        "switch_replicated_messages": sw_repl[-1] if sw_repl else 0.0,
        "switch_pool_ingress_avg": sw_pool_util[-1] if sw_pool_util else 0.0,
        "avg_pool_req_link_util": (sum(pool_req_utils) / len(pool_req_utils)) if pool_req_utils else 0.0,
    }


def write_summary(rows: List[Dict[str, float]]) -> None:
    out_csv = OUTPUT_ROOT / "replica_count_summary.csv"
    rows_sorted = sorted(rows, key=lambda r: (r["config"], r["replicas"]))
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows_sorted[0].keys()))
        writer.writeheader()
        writer.writerows(rows_sorted)
    print(f"[STATUS] Wrote summary: {out_csv}")


def main() -> int:
    print("[STATUS] Starting replica-count experiment")
    print(f"[STATUS] Fixed trace mix load_pct={LOAD_PCT}, mem_pct={MEM_PCT}, cxl_pct={CXL_PCT}")
    print(f"[STATUS] Replica sweep: {REPLICA_COUNTS}")
    build_generator()
    generate_trace()
    write_cxl_config(CONFIG_PATH)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)

    tasks = []
    for replicas in REPLICA_COUNTS:
        no_rep_out = OUTPUT_ROOT / f"run_replica{replicas:03d}_no_rep.out"
        no_rep_err = OUTPUT_ROOT / f"run_replica{replicas:03d}_no_rep.err"
        rep_out = OUTPUT_ROOT / f"run_replica{replicas:03d}_rep.out"
        rep_err = OUTPUT_ROOT / f"run_replica{replicas:03d}_rep.err"
        tasks.append(("no_rep", replicas, SIM_NO_REP, no_rep_out, no_rep_err))
        tasks.append(("rep", replicas, SIM_REP, rep_out, rep_err))

    print(f"[STATUS] Launching {len(tasks)} runs with up to {MAX_PARALLEL} in parallel")

    failures = 0
    results = []
    completed = 0

    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as executor:
        futures = {}
        for config, replicas, sim_script, out_path, err_path in tasks:
            fut = executor.submit(run_sst, sim_script, replicas, out_path, err_path)
            futures[fut] = (config, replicas, out_path)

        for fut in concurrent.futures.as_completed(futures):
            config, replicas, out_path = futures[fut]
            try:
                rc, _ = fut.result()
            except Exception as exc:
                print(f"[FAIL] replicas={replicas} {config}: {exc}")
                failures += 1
                continue

            if rc != 0:
                print(f"[FAIL] replicas={replicas} {config}: rc={rc}")
                failures += 1
            else:
                metrics = parse_run_metrics(out_path)
                row = {"config": config, "replicas": replicas}
                row.update(metrics)
                results.append(row)

            completed += 1
            print(f"[STATUS] Completed {completed}/{len(tasks)}")

    if results:
        write_summary(results)

    if failures:
        print(f"Replica-count experiment finished with {failures} failures.")
        return 1
    print("Replica-count experiment complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
