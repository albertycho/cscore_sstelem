#!/usr/bin/env python3
import concurrent.futures
import csv
import os
import re
import subprocess
from pathlib import Path

# use: python3 experiments/replica_count/run_replica_count.py

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path(os.environ.get("TRACE_ROOT", "/shared/kshan/CXL_sst_traces_pointer_chase"))
OUTPUT_ROOT = SCRIPT_DIR / "logs"
CONFIG_PATH = OUTPUT_ROOT / "cxl_config.csv"
SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen_pointer_chase"
GEN_SRC = REPO_ROOT / "scripts" / "generate_pointer_chase_trace.cpp"
SIM_SCRIPT = REPO_ROOT / "experiments" / "pointer_chase_replication" / "pool_sweep.py"

POOL_NODE_ID_BASE = 100
NODE_COUNTS = [int(value) for value in os.environ.get("NODE_COUNTS", "1,2,4,8,16").split(",") if value.strip()]
REPLICA_COUNTS = [int(value) for value in os.environ.get("REPLICA_COUNTS", "2,4,8,16").split(",") if value.strip()]

MAX_CORE_BUDGET = 160
MAX_RANKS_PER_RUN = max(NODE_COUNTS) if NODE_COUNTS else 1
DEFAULT_MAX_PARALLEL = min(20, max(1, MAX_CORE_BUDGET // max(MAX_RANKS_PER_RUN, 1)))
MAX_PARALLEL = int(os.environ.get("MAX_PARALLEL", str(DEFAULT_MAX_PARALLEL)))

LOAD_PCT = int(os.environ.get("LOAD_PCT", "80"))
INJECT_BANDWIDTH_GBPS = float(os.environ.get("INJECT_BANDWIDTH_GBPS", "1.50"))
NUM_INSTRS = 10_000
SEED = 0x12345678
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20
MAX_GRAPH_BROADCAST_RETRIES = int(os.environ.get("MAX_GRAPH_BROADCAST_RETRIES", "2"))

LAT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.avg_load_issue_to_complete_lat\s*=\s*([0-9eE+.\-]+)")
LAT_COUNT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.load_issue_to_complete_count\s*=\s*([0-9eE+.\-]+)")
REQ_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.request_gbps\s*=\s*([0-9eE+.\-]+)")
RESP_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.response_gbps\s*=\s*([0-9eE+.\-]+)")
AGG_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.aggregate_gbps\s*=\s*([0-9eE+.\-]+)")
SW_REPL_RE = re.compile(r"stat\.switch\.replicated_messages = ([0-9.eE+-]+)")


def build_generator() -> None:
    print("[STATUS] Building pointer-chase generator...")
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


def generate_trace(out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    trace_path = out_dir / "pointer_chase.champsim.trace"
    print(f"[STATUS] Generating pointer-chase trace -> {trace_path.name}")
    cmd = [
        str(GEN_BIN),
        "--out-dir", str(out_dir),
        "--out-name", trace_path.name,
        "--num-instrs", str(NUM_INSTRS),
        "--base-addr", hex(CXL_BASE),
        "--region-size", hex(CXL_WS_BYTES),
        "--working-set-bytes", hex(CXL_WS_BYTES),
        "--seed", hex(SEED),
    ]
    subprocess.check_call(cmd)
    return trace_path


def write_cxl_config(path: Path, num_nodes: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        f.write("# node_id,start,size,type,target\n")
        for node in range(num_nodes):
            f.write(f"{node},0x{CXL_BASE:x},0x{CXL_WS_BYTES:x},pool,{POOL_NODE_ID_BASE}\n")


def is_retryable_startup_failure(out_path: Path, err_path: Path) -> bool:
    if not out_path.exists() or not err_path.exists():
        return False
    if out_path.stat().st_size != 0:
        return False
    err_text = err_path.read_text(errors="ignore")
    return "Error encountered during graph broadcast" in err_text


def run_sst(
    trace_path: Path,
    cxl_config_path: Path,
    num_nodes: int,
    replicate_writes: int,
    num_pools: int,
    out_path: Path,
    err_path: Path,
) -> int:
    mpi_ranks = int(os.environ.get("MPI_RANKS", str(num_nodes)))

    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(cxl_config_path)
    env["NUM_NODES"] = str(num_nodes)
    env["MPI_RANKS"] = str(mpi_ranks)
    env["REPLICATE_WRITES"] = str(replicate_writes)
    env["NUM_POOLS"] = str(num_pools)
    env["INJECT_BANDWIDTH_GBPS"] = str(INJECT_BANDWIDTH_GBPS)
    env["INJECT_LOAD_PCT"] = str(LOAD_PCT)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)

    print(
        f"[STATUS] Launching {out_path.name} "
        f"(num_nodes={num_nodes}, replicate_writes={replicate_writes}, num_pools={num_pools}, "
        f"load_pct={LOAD_PCT}, inject_bw={INJECT_BANDWIDTH_GBPS})"
    )

    max_attempts = 1 + max(0, MAX_GRAPH_BROADCAST_RETRIES)
    for attempt_idx in range(max_attempts):
        if attempt_idx > 0:
            print(
                f"[STATUS] Retrying {out_path.name} after graph-broadcast startup failure "
                f"(attempt {attempt_idx + 1}/{max_attempts})"
            )
        with out_path.open("w") as out_f, err_path.open("w") as err_f:
            proc = subprocess.run(
                ["mpirun", "-n", str(mpi_ranks), SST_BIN, str(SIM_SCRIPT)],
                cwd=str(SCRIPT_DIR),
                env=env,
                stdout=out_f,
                stderr=err_f,
            )
        if proc.returncode == 0:
            return 0
        if not is_retryable_startup_failure(out_path, err_path):
            return proc.returncode
    return proc.returncode


def parse_scalars(pattern: re.Pattern[str], text: str) -> dict[int, float]:
    return {int(m.group(1)): float(m.group(2)) for m in pattern.finditer(text)}


def parse_run_metrics(path: Path, expected_nodes: int) -> dict[str, float] | None:
    text = path.read_text(errors="ignore")
    lat_by_node = parse_scalars(LAT_RE, text)
    lat_count_by_node = parse_scalars(LAT_COUNT_RE, text)
    req_bw_by_node = parse_scalars(REQ_BW_RE, text)
    resp_bw_by_node = parse_scalars(RESP_BW_RE, text)
    agg_bw_by_node = parse_scalars(AGG_BW_RE, text)
    sw_repl = [float(m.group(1)) for m in SW_REPL_RE.finditer(text)]

    if (
        len(lat_by_node) != expected_nodes
        or len(lat_count_by_node) != expected_nodes
        or len(req_bw_by_node) != expected_nodes
        or len(resp_bw_by_node) != expected_nodes
        or len(agg_bw_by_node) != expected_nodes
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
        "weighted_avg_load_lat_cycles": weighted_lat_sum / weighted_lat_count,
        "request_gbps": sum(req_bw_by_node.values()),
        "response_gbps": sum(resp_bw_by_node.values()),
        "aggregate_bw_gbps": sum(agg_bw_by_node.values()),
        "switch_replicated_messages": sw_repl[-1] if sw_repl else 0.0,
    }


def write_summary(rows: list[dict[str, object]]) -> None:
    out_csv = OUTPUT_ROOT / "replica_count_summary.csv"
    rows_sorted = sorted(rows, key=lambda row: (int(row["num_nodes"]), str(row["config"]), int(row["replicas"])))
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "config",
                "replicas",
                "num_nodes",
                "load_pct",
                "requested_request_gbps_per_node",
                "weighted_avg_load_lat_cycles",
                "request_gbps",
                "response_gbps",
                "aggregate_bw_gbps",
                "switch_replicated_messages",
                "log_path",
            ],
        )
        writer.writeheader()
        writer.writerows(rows_sorted)
    print(f"[STATUS] Wrote summary: {out_csv}")


def main() -> int:
    print("[STATUS] Starting pointer-chase replica-count experiment")
    print(
        f"[STATUS] node_counts={NODE_COUNTS} load_pct={LOAD_PCT} "
        f"inject_bw={INJECT_BANDWIDTH_GBPS} replica_counts={REPLICA_COUNTS}"
    )
    build_generator()
    trace_path = generate_trace(TRACE_ROOT)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    config_paths: dict[int, Path] = {}
    for num_nodes in NODE_COUNTS:
        config_path = OUTPUT_ROOT / f"cxl_config_nodes{num_nodes:03d}.csv"
        write_cxl_config(config_path, num_nodes)
        config_paths[num_nodes] = config_path

    tasks = []
    for num_nodes in NODE_COUNTS:
        tasks.append((
            "no_rep",
            num_nodes,
            1,
            0,
            1,
            config_paths[num_nodes],
            OUTPUT_ROOT / f"run_nodes{num_nodes:03d}_replica001_no_rep.out",
            OUTPUT_ROOT / f"run_nodes{num_nodes:03d}_replica001_no_rep.err",
        ))
        for replicas in REPLICA_COUNTS:
            tasks.append((
                "rep",
                num_nodes,
                replicas,
                1,
                replicas,
                config_paths[num_nodes],
                OUTPUT_ROOT / f"run_nodes{num_nodes:03d}_replica{replicas:03d}_rep.out",
                OUTPUT_ROOT / f"run_nodes{num_nodes:03d}_replica{replicas:03d}_rep.err",
            ))

    failures = 0
    results: list[dict[str, object]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(MAX_PARALLEL, len(tasks))) as executor:
        future_to_task = {}
        for config, num_nodes, replicas, replicate_writes, num_pools, config_path, out_path, err_path in tasks:
            future = executor.submit(
                run_sst,
                trace_path,
                config_path,
                num_nodes,
                replicate_writes,
                num_pools,
                out_path,
                err_path,
            )
            future_to_task[future] = (config, num_nodes, replicas, out_path)

        completed = 0
        for future in concurrent.futures.as_completed(future_to_task):
            config, num_nodes, replicas, out_path = future_to_task[future]
            try:
                rc = future.result()
            except Exception as exc:
                print(f"[FAIL] num_nodes={num_nodes} replicas={replicas} {config}: {exc}")
                failures += 1
                completed += 1
                continue

            if rc != 0:
                print(f"[FAIL] num_nodes={num_nodes} replicas={replicas} {config}: rc={rc}")
                failures += 1
            else:
                metrics = parse_run_metrics(out_path, num_nodes)
                if metrics is None:
                    print(f"[FAIL] num_nodes={num_nodes} replicas={replicas} {config}: missing complete stats")
                    failures += 1
                else:
                    row = {
                        "config": config,
                        "replicas": replicas,
                        "num_nodes": num_nodes,
                        "load_pct": LOAD_PCT,
                        "requested_request_gbps_per_node": INJECT_BANDWIDTH_GBPS,
                        "log_path": str(out_path),
                    }
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
