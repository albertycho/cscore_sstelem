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
SIM_SCRIPT = SCRIPT_DIR / "pool_sweep.py"

NUM_NODES = int(os.environ.get("NUM_NODES", "8"))
MPI_RANKS = int(os.environ.get("MPI_RANKS", str(NUM_NODES)))
MAX_CORE_BUDGET = 160
DEFAULT_MAX_PARALLEL = min(20, max(1, MAX_CORE_BUDGET // max(MPI_RANKS, 1)))
MAX_PARALLEL = int(os.environ.get("MAX_PARALLEL", str(DEFAULT_MAX_PARALLEL)))

LOAD_PCTS = [int(value) for value in os.environ.get("LOAD_PCTS", "0,10,20,30,40,50,60,70,80,90,100").split(",") if value.strip()]
REPLICA_COUNTS = [int(value) for value in os.environ.get("REPLICA_COUNTS", "1,2,3,4,5,6,7,8").split(",") if value.strip()]

NUM_INSTRS = 10_000
SEED = 0x12345678
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20
CLOCK_GHZ = 2.4
LINK_BW_CYCLES = 25

LATENCY_THRESHOLD = float(os.environ.get("LATENCY_THRESHOLD", "1000.0"))
SEARCH_LEFT_FRAC = float(os.environ.get("SEARCH_LEFT_FRAC", "0.50"))
SEARCH_RIGHT_FRAC = float(os.environ.get("SEARCH_RIGHT_FRAC", "1.15"))
SEARCH_SHRINK_FRAC = float(os.environ.get("SEARCH_SHRINK_FRAC", "0.25"))
MAX_SEARCH_ITERS = int(os.environ.get("MAX_SEARCH_ITERS", "15"))
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


def write_cxl_config(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        f.write("# node_id,start,size,type,target\n")
        for node in range(NUM_NODES):
            f.write(f"{node},0x{CXL_BASE:x},0x{CXL_WS_BYTES:x},pool,100\n")


def link_peak_gbps() -> float:
    bits_per_cycle = 64.0 * 8.0 / LINK_BW_CYCLES
    return bits_per_cycle * CLOCK_GHZ


def request_mix_fractions(load_pct: int) -> tuple[float, float, float]:
    load_frac = load_pct / 100.0
    avg_request_bytes = 64.0 - (56.0 * load_frac)
    load_request_frac = 0.0 if avg_request_bytes <= 0.0 else (8.0 * load_frac) / avg_request_bytes
    store_request_frac = 1.0 - load_request_frac
    response_to_request_ratio = 0.0 if avg_request_bytes <= 0.0 else (64.0 * load_frac) / avg_request_bytes
    return load_request_frac, store_request_frac, response_to_request_ratio


def ratio_target_request_gbps(load_pct: int, replica_count: int) -> float:
    load_request_frac, store_request_frac, response_ratio = request_mix_fractions(load_pct)
    nodes_per_pool = NUM_NODES / replica_count

    node_forward_factor = 1.0
    node_reverse_factor = response_ratio
    pool_forward_factor = NUM_NODES * (store_request_frac + (load_request_frac / replica_count))
    pool_reverse_factor = nodes_per_pool * response_ratio

    worst_factor = max(
        node_forward_factor,
        node_reverse_factor,
        pool_forward_factor,
        pool_reverse_factor,
    )
    return FULL_CAPACITY_GBPS / worst_factor


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
    inject_bw: float,
    inject_load_pct: int,
    replica_count: int,
    out_path: Path,
    err_path: Path,
) -> int:
    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(CONFIG_PATH)
    env["NUM_NODES"] = str(NUM_NODES)
    env["MPI_RANKS"] = str(MPI_RANKS)
    env["INJECT_BANDWIDTH_GBPS"] = str(inject_bw)
    env["INJECT_LOAD_PCT"] = str(inject_load_pct)
    env["REPLICATE_WRITES"] = "1"
    env["NUM_POOLS"] = str(replica_count)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)
    print(
        f"[STATUS] Launching {SIM_SCRIPT.name} -> {out_path.name} "
        f"(replicas={replica_count}, inject_bw={inject_bw}, load_pct={inject_load_pct})"
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
                ["mpirun", "-n", str(MPI_RANKS), SST_BIN, str(SIM_SCRIPT)],
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


def parse_metrics(path: Path) -> tuple[float | None, float | None, float | None, float | None]:
    text = path.read_text(errors="ignore")
    lat_by_node = parse_scalars(LAT_RE, text)
    lat_count_by_node = parse_scalars(LAT_COUNT_RE, text)
    req_bw_by_node = parse_scalars(REQ_BW_RE, text)
    resp_bw_by_node = parse_scalars(RESP_BW_RE, text)
    agg_bw_by_node = parse_scalars(AGG_BW_RE, text)
    if (
        len(lat_by_node) != NUM_NODES
        or len(lat_count_by_node) != NUM_NODES
        or len(req_bw_by_node) != NUM_NODES
        or len(resp_bw_by_node) != NUM_NODES
        or len(agg_bw_by_node) != NUM_NODES
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


def clear_previous_outputs() -> None:
    removed = 0
    for pattern in ("run_replica*_load*_bw*.out", "run_replica*_load*_bw*.err"):
        for path in OUTPUT_ROOT.glob(pattern):
            path.unlink()
            removed += 1
    for path in (
        OUTPUT_ROOT / "cxl_config.csv",
        OUTPUT_ROOT / "replica_sweep_bw_latency.csv",
        OUTPUT_ROOT / "replica_sweep_bw_latency.png",
    ):
        if path.exists():
            path.unlink()
            removed += 1
    if removed:
        print(f"[STATUS] Cleared {removed} prior generated files from {OUTPUT_ROOT}")


def run_replica_load_sweep(trace_path: Path, replica_count: int, load_pct: int) -> int:
    target_bw = ratio_target_request_gbps(load_pct, replica_count)
    left = target_bw * SEARCH_LEFT_FRAC
    right = target_bw * SEARCH_RIGHT_FRAC
    sampled_bws: set[float] = set()
    run_count = 0

    print(
        f"[STATUS] replicas={replica_count} load_pct={load_pct}: "
        f"target per-node request bw={target_bw:.3f} Gbps, "
        f"search window=[{left:.3f}, {right:.3f}]"
    )

    def run_sample(label: str, inject_bw: float) -> tuple[int, float]:
        nonlocal run_count
        inject_bw = round(inject_bw, 6)
        sampled_bws.add(inject_bw)
        bw_token = format_bw_token(inject_bw)
        out_path = OUTPUT_ROOT / f"run_replica{replica_count:03d}_load{load_pct:03d}_{label}_bw{bw_token}.out"
        err_path = OUTPUT_ROOT / f"run_replica{replica_count:03d}_load{load_pct:03d}_{label}_bw{bw_token}.err"
        rc = run_sst(trace_path, inject_bw, load_pct, replica_count, out_path, err_path)
        run_count += 1
        if rc != 0:
            print(f"[FAIL] {SIM_SCRIPT.name} -> {out_path} (rc={rc})")
            return rc, 0.0

        avg_latency, total_request_gbps, total_response_gbps, total_aggregate_gbps = parse_metrics(out_path)
        if (
            avg_latency is None
            or total_request_gbps is None
            or total_response_gbps is None
            or total_aggregate_gbps is None
        ):
            print(f"[FAIL] missing injector/latency stats in {out_path}")
            return 1, 0.0

        print(
            f"[STATUS] replicas={replica_count} load_pct={load_pct} label={label} "
            f"req_bw_per_node={inject_bw:.3f} total_req_bw={total_request_gbps:.3f} "
            f"total_resp_bw={total_response_gbps:.3f} total_agg_bw={total_aggregate_gbps:.3f} "
            f"avg_load_issue_to_complete_lat={avg_latency:.3f}"
        )
        return 0, avg_latency

    for label, inject_bw in (("base", 0.3), ("left", left), ("right", right)):
        inject_bw = round(inject_bw, 6)
        if inject_bw in sampled_bws:
            continue
        rc, _ = run_sample(label, inject_bw)
        if rc != 0:
            return 1

    for iter_idx in range(MAX_SEARCH_ITERS):
        window = right - left
        inject_bw = round((left + right) / 2.0, 6)
        if inject_bw in sampled_bws:
            print(
                f"[STATUS] replicas={replica_count} load_pct={load_pct}: "
                f"stopping because midpoint {inject_bw:.6f} repeated"
            )
            break

        rc, latency = run_sample(f"iter{iter_idx:02d}", inject_bw)
        if rc != 0:
            return 1

        shift = window * SEARCH_SHRINK_FRAC
        if latency > LATENCY_THRESHOLD:
            right -= shift
        else:
            left += shift

    print(f"[STATUS] replicas={replica_count} load_pct={load_pct}: completed {run_count} runs")
    return 0


def collect_rows() -> list[dict[str, object]]:
    out_rows: list[dict[str, object]] = []
    out_re = re.compile(
        r"run_replica(?P<replicas>\d{3})_load(?P<load_pct>\d{3})(?:_(?P<label>[^_]+))?_bw(?P<req_bw>[0-9]+p[0-9]+)\.out$"
    )
    for out_path in sorted(OUTPUT_ROOT.glob("run_replica*_load*_bw*.out")):
        match = out_re.match(out_path.name)
        if match is None:
            continue
        avg_latency, total_request_gbps, total_response_gbps, total_aggregate_gbps = parse_metrics(out_path)
        if (
            avg_latency is None
            or total_request_gbps is None
            or total_response_gbps is None
            or total_aggregate_gbps is None
        ):
            continue
        out_rows.append({
            "replicas": int(match.group("replicas")),
            "load_pct": int(match.group("load_pct")),
            "requested_request_gbps_per_node": float(match.group("req_bw").replace("p", ".")),
            "request_gbps": total_request_gbps,
            "response_gbps": total_response_gbps,
            "aggregate_bw_gbps": total_aggregate_gbps,
            "latency_cycles": avg_latency,
            "label": match.group("label") or "sample",
            "log_path": str(out_path),
        })
    return out_rows


def write_summary(rows: list[dict[str, object]]) -> None:
    out_csv = OUTPUT_ROOT / "replica_sweep_bw_latency.csv"
    rows_sorted = sorted(rows, key=lambda row: (int(row["replicas"]), int(row["load_pct"]), float(row["aggregate_bw_gbps"])))
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "replicas",
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


def main() -> int:
    print("[STATUS] Starting 8-node pointer-chase replica sweep")
    print(f"[STATUS] replica_counts={REPLICA_COUNTS} load_pcts={LOAD_PCTS}")
    print(f"[STATUS] Pointer trace instructions={NUM_INSTRS}")
    print("[STATUS] SST warmup=1000 main=4000")
    print(f"[STATUS] Link peak per direction={link_peak_gbps():.3f} Gbps")
    print(f"[STATUS] Full-capacity reference={FULL_CAPACITY_GBPS:.3f} Gbps")
    print(f"[STATUS] Latency threshold={LATENCY_THRESHOLD} cycles")

    build_generator()
    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    clear_previous_outputs()
    write_cxl_config(CONFIG_PATH)
    trace_path = generate_trace(TRACE_ROOT)

    lanes = [(replica_count, load_pct) for replica_count in REPLICA_COUNTS for load_pct in LOAD_PCTS]
    parallel_lanes = min(MAX_PARALLEL, len(lanes))
    print(f"[STATUS] Launching {len(lanes)} replica/load_pct lanes with up to {parallel_lanes} in parallel")

    failures = 0
    completed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=parallel_lanes) as executor:
        future_to_lane = {
            executor.submit(run_replica_load_sweep, trace_path, replica_count, load_pct): (replica_count, load_pct)
            for replica_count, load_pct in lanes
        }
        for future in concurrent.futures.as_completed(future_to_lane):
            replica_count, load_pct = future_to_lane[future]
            try:
                rc = future.result()
            except Exception as exc:
                print(f"[FAIL] replicas={replica_count} load_pct={load_pct}: {exc}")
                failures += 1
                completed += 1
                print(f"[STATUS] Completed {completed}/{len(lanes)} lanes")
                continue

            if rc != 0:
                print(f"[FAIL] replicas={replica_count} load_pct={load_pct} lane failed")
                failures += 1
            completed += 1
            print(f"[STATUS] Completed {completed}/{len(lanes)} lanes")

    rows = collect_rows()
    if rows:
        write_summary(rows)

    if failures:
        print(f"Replica sweep finished with {failures} failures.")
        return 1
    print("Replica sweep complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
