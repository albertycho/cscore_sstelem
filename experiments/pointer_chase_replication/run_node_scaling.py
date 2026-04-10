#!/usr/bin/env python3
import concurrent.futures
import csv
import os
import re
import subprocess
from pathlib import Path

# use: python3 experiments/pointer_chase_replication/run_node_scaling.py

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path(os.environ.get("TRACE_ROOT", "/shared/kshan/CXL_sst_traces_pointer_chase"))
OUTPUT_ROOT = SCRIPT_DIR / "logs_node_scaling"
SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen_pointer_chase"
GEN_SRC = REPO_ROOT / "scripts" / "generate_pointer_chase_trace.cpp"
SIM_SCRIPT = SCRIPT_DIR / "pool_sweep.py"

MAX_CORE_BUDGET = 160
NODE_COUNTS = [int(value) for value in os.environ.get("NODE_COUNTS", "1,2,4,8,16").split(",") if value.strip()]
CONFIGS = [value.strip() for value in os.environ.get("CONFIGS", "no_rep,rep2").split(",") if value.strip()]
LOAD_PCT = int(os.environ.get("LOAD_PCT", "80"))
MANUAL_INJECT_BANDWIDTH_GBPS = os.environ.get("INJECT_BANDWIDTH_GBPS")
OPERATING_POINT_FRAC = float(os.environ.get("OPERATING_POINT_FRAC", "0.85"))
MAX_PARALLEL = int(os.environ.get("MAX_PARALLEL", str(min(8, len(NODE_COUNTS) * max(1, len(CONFIGS))))))

NUM_INSTRS = 10_000
SEED = 0x12345678
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20
POOL_NODE_ID_BASE = 100
FULL_CAPACITY_GBPS = float(os.environ.get("FULL_CAPACITY_GBPS", "49.152"))
MAX_GRAPH_BROADCAST_RETRIES = int(os.environ.get("MAX_GRAPH_BROADCAST_RETRIES", "2"))

LAT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.avg_load_issue_to_complete_lat\s*=\s*([0-9eE+.\-]+)")
LAT_COUNT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.load_issue_to_complete_count\s*=\s*([0-9eE+.\-]+)")
REQ_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.request_gbps\s*=\s*([0-9eE+.\-]+)")
RESP_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.response_gbps\s*=\s*([0-9eE+.\-]+)")
AGG_BW_RE = re.compile(r"stat\.node\.(\d+)\.injector\.aggregate_gbps\s*=\s*([0-9eE+.\-]+)")


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


def parse_config_name(config_name: str) -> tuple[int, int]:
    if config_name == "no_rep":
        return 0, 1
    if config_name.startswith("rep"):
        return 1, int(config_name[3:])
    raise ValueError(f"unsupported config name: {config_name}")


def request_mix_fractions(load_pct: int) -> tuple[float, float, float]:
    load_frac = load_pct / 100.0
    avg_request_bytes = 64.0 - (56.0 * load_frac)
    load_request_frac = 0.0 if avg_request_bytes <= 0.0 else (8.0 * load_frac) / avg_request_bytes
    store_request_frac = 1.0 - load_request_frac
    response_to_request_ratio = 0.0 if avg_request_bytes <= 0.0 else (64.0 * load_frac) / avg_request_bytes
    return load_request_frac, store_request_frac, response_to_request_ratio


def target_request_gbps(num_nodes: int, num_pools: int, replicate_writes: bool, load_pct: int) -> float:
    load_request_frac, store_request_frac, response_ratio = request_mix_fractions(load_pct)
    nodes_per_pool = num_nodes / num_pools

    node_forward_factor = 1.0
    node_reverse_factor = response_ratio
    if replicate_writes:
        pool_forward_factor = num_nodes * (store_request_frac + (load_request_frac / num_pools))
    else:
        pool_forward_factor = nodes_per_pool
    pool_reverse_factor = nodes_per_pool * response_ratio

    worst_factor = max(
        node_forward_factor,
        node_reverse_factor,
        pool_forward_factor,
        pool_reverse_factor,
    )
    return FULL_CAPACITY_GBPS / worst_factor


def inject_bandwidth_gbps(num_nodes: int, config_name: str) -> float:
    if MANUAL_INJECT_BANDWIDTH_GBPS is not None:
        return float(MANUAL_INJECT_BANDWIDTH_GBPS)
    replicate_writes, num_pools = parse_config_name(config_name)
    target_bw = target_request_gbps(num_nodes, num_pools, bool(replicate_writes), LOAD_PCT)
    return target_bw * OPERATING_POINT_FRAC


def format_bw_token(inject_bw: float) -> str:
    return f"{inject_bw:05.2f}".replace(".", "p")


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
    config_name: str,
    inject_bw: float,
    out_path: Path,
    err_path: Path,
) -> int:
    replicate_writes, num_pools = parse_config_name(config_name)
    mpi_ranks = int(os.environ.get("MPI_RANKS", str(num_nodes)))

    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(cxl_config_path)
    env["INJECT_BANDWIDTH_GBPS"] = str(inject_bw)
    env["INJECT_LOAD_PCT"] = str(LOAD_PCT)
    env["REPLICATE_WRITES"] = str(replicate_writes)
    env["NUM_POOLS"] = str(num_pools)
    env["NUM_NODES"] = str(num_nodes)
    env["MPI_RANKS"] = str(mpi_ranks)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)
    print(
        f"[STATUS] Launching {SIM_SCRIPT.name} -> {out_path.name} "
        f"(config={config_name}, num_nodes={num_nodes}, inject_bw={inject_bw}, load_pct={LOAD_PCT})"
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


def parse_metrics(path: Path, expected_nodes: int) -> tuple[float | None, float | None, float | None, float | None]:
    text = path.read_text(errors="ignore")
    lat_by_node = parse_scalars(LAT_RE, text)
    lat_count_by_node = parse_scalars(LAT_COUNT_RE, text)
    req_bw_by_node = parse_scalars(REQ_BW_RE, text)
    resp_bw_by_node = parse_scalars(RESP_BW_RE, text)
    agg_bw_by_node = parse_scalars(AGG_BW_RE, text)
    if (
        len(lat_by_node) != expected_nodes
        or len(lat_count_by_node) != expected_nodes
        or len(req_bw_by_node) != expected_nodes
        or len(resp_bw_by_node) != expected_nodes
        or len(agg_bw_by_node) != expected_nodes
    ):
        return None, None, None, None

    weighted_lat_sum = 0.0
    weighted_lat_count = 0.0
    for node, lat in lat_by_node.items():
        count = lat_count_by_node.get(node, 0.0)
        weighted_lat_sum += lat * count
        weighted_lat_count += count
    if weighted_lat_count <= 0.0:
        return None, None, None, None

    return (
        weighted_lat_sum / weighted_lat_count,
        sum(req_bw_by_node.values()),
        sum(resp_bw_by_node.values()),
        sum(agg_bw_by_node.values()),
    )


def write_summary(rows: list[dict[str, object]], out_csv: Path) -> None:
    rows_sorted = sorted(rows, key=lambda row: (str(row["config"]), int(row["num_nodes"])))
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "config",
                "num_nodes",
                "load_pct",
                "requested_request_gbps_per_node",
                "request_gbps",
                "response_gbps",
                "aggregate_bw_gbps",
                "latency_cycles",
                "log_path",
            ],
        )
        writer.writeheader()
        writer.writerows(rows_sorted)
    print(f"[STATUS] Wrote summary: {out_csv}")


def main() -> int:
    print("[STATUS] Starting pointer-chase node-count scaling experiment")
    if MANUAL_INJECT_BANDWIDTH_GBPS is None:
        print(
            f"[STATUS] configs={CONFIGS} node_counts={NODE_COUNTS} load_pct={LOAD_PCT} "
            f"operating_point_frac={OPERATING_POINT_FRAC}"
        )
    else:
        print(
            f"[STATUS] configs={CONFIGS} node_counts={NODE_COUNTS} load_pct={LOAD_PCT} "
            f"inject_bw={float(MANUAL_INJECT_BANDWIDTH_GBPS)}"
        )
    build_generator()
    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    trace_path = generate_trace(TRACE_ROOT)
    for num_nodes in NODE_COUNTS:
        write_cxl_config(OUTPUT_ROOT / f"cxl_config_nodes{num_nodes:03d}.csv", num_nodes)

    tasks = [(config_name, num_nodes) for config_name in CONFIGS for num_nodes in NODE_COUNTS]
    failures = 0
    rows: list[dict[str, object]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(MAX_PARALLEL, len(tasks))) as executor:
        future_to_task = {}
        for config_name, num_nodes in tasks:
            inject_bw = round(inject_bandwidth_gbps(num_nodes, config_name), 6)
            bw_token = format_bw_token(inject_bw)
            out_path = OUTPUT_ROOT / (
                f"run_nodes{num_nodes:03d}_{config_name}_load{LOAD_PCT:03d}_bw{bw_token}.out"
            )
            err_path = OUTPUT_ROOT / (
                f"run_nodes{num_nodes:03d}_{config_name}_load{LOAD_PCT:03d}_bw{bw_token}.err"
            )
            config_path = OUTPUT_ROOT / f"cxl_config_nodes{num_nodes:03d}.csv"
            future = executor.submit(
                run_sst,
                trace_path,
                config_path,
                num_nodes,
                config_name,
                inject_bw,
                out_path,
                err_path,
            )
            future_to_task[future] = (config_name, num_nodes, inject_bw, out_path)

        completed = 0
        for future in concurrent.futures.as_completed(future_to_task):
            config_name, num_nodes, inject_bw, out_path = future_to_task[future]
            try:
                rc = future.result()
            except Exception as exc:
                print(f"[FAIL] config={config_name} num_nodes={num_nodes}: {exc}")
                failures += 1
                completed += 1
                continue

            if rc != 0:
                print(f"[FAIL] config={config_name} num_nodes={num_nodes}: rc={rc}")
                failures += 1
            else:
                avg_latency, req_bw, resp_bw, agg_bw = parse_metrics(out_path, num_nodes)
                if avg_latency is None or req_bw is None or resp_bw is None or agg_bw is None:
                    print(f"[FAIL] missing complete stats in {out_path}")
                    failures += 1
                else:
                    rows.append({
                        "config": config_name,
                        "num_nodes": num_nodes,
                        "load_pct": LOAD_PCT,
                        "requested_request_gbps_per_node": inject_bw,
                        "request_gbps": req_bw,
                        "response_gbps": resp_bw,
                        "aggregate_bw_gbps": agg_bw,
                        "latency_cycles": avg_latency,
                        "log_path": str(out_path),
                    })
                    print(
                        f"[STATUS] config={config_name} num_nodes={num_nodes} inject_bw={inject_bw:.3f} "
                        f"total_agg_bw={agg_bw:.3f} "
                        f"avg_load_issue_to_complete_lat={avg_latency:.3f}"
                    )
            completed += 1
            print(f"[STATUS] Completed {completed}/{len(tasks)}")

    if rows:
        write_summary(rows, OUTPUT_ROOT / "node_scaling_summary.csv")

    if failures:
        print(f"Node-count scaling experiment finished with {failures} failures.")
        return 1
    print("Node-count scaling experiment complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
