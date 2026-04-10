#!/usr/bin/env python3
import csv
import os
import re
import subprocess
from pathlib import Path

# use: python3 experiments/pointer_chase_replication/run_cdf_points.py

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path(os.environ.get("TRACE_ROOT", "/shared/kshan/CXL_sst_traces_pointer_chase"))
OUTPUT_ROOT = SCRIPT_DIR / "logs_cdf"
SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen_pointer_chase"
GEN_SRC = REPO_ROOT / "scripts" / "generate_pointer_chase_trace.cpp"
SIM_SCRIPT = SCRIPT_DIR / "pool_sweep.py"

NUM_NODES = int(os.environ.get("NUM_NODES", "8"))
MPI_RANKS = int(os.environ.get("MPI_RANKS", str(NUM_NODES)))
POOL_NODE_ID_BASE = 100

NUM_INSTRS = 10_000
SEED = 0x12345678
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20
MAX_GRAPH_BROADCAST_RETRIES = int(os.environ.get("MAX_GRAPH_BROADCAST_RETRIES", "2"))

DEFAULT_CASES = "no_rep,80,1.90;rep2,80,4.15"
CASE_SPEC = os.environ.get("CDF_CASES", DEFAULT_CASES)

LAT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.avg_load_issue_to_complete_lat\s*=\s*([0-9eE+.\-]+)")
LAT_COUNT_RE = re.compile(r"stat\.node\.(\d+)\.cpu\.0\.load_issue_to_complete_count\s*=\s*([0-9eE+.\-]+)")
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
    inject_bw: float,
    inject_load_pct: int,
    config_name: str,
    replicate_writes: int,
    num_pools: int,
    out_path: Path,
    err_path: Path,
) -> int:
    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(cxl_config_path)
    env["INJECT_BANDWIDTH_GBPS"] = str(inject_bw)
    env["INJECT_LOAD_PCT"] = str(inject_load_pct)
    env["REPLICATE_WRITES"] = str(replicate_writes)
    env["NUM_POOLS"] = str(num_pools)
    env["NUM_NODES"] = str(NUM_NODES)
    env["MPI_RANKS"] = str(MPI_RANKS)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)
    print(
        f"[STATUS] Launching {SIM_SCRIPT.name} -> {out_path.name} "
        f"(config={config_name}, inject_bw={inject_bw}, load_pct={inject_load_pct}, num_nodes={NUM_NODES})"
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


def parse_metrics(path: Path) -> tuple[float | None, float | None]:
    text = path.read_text(errors="ignore")
    lat_by_node = parse_scalars(LAT_RE, text)
    lat_count_by_node = parse_scalars(LAT_COUNT_RE, text)
    agg_bw_by_node = parse_scalars(AGG_BW_RE, text)
    if len(lat_by_node) != NUM_NODES or len(lat_count_by_node) != NUM_NODES or len(agg_bw_by_node) != NUM_NODES:
        return None, None

    weighted_lat_sum = 0.0
    weighted_lat_count = 0.0
    for node, lat in lat_by_node.items():
        count = lat_count_by_node.get(node, 0.0)
        weighted_lat_sum += lat * count
        weighted_lat_count += count
    if weighted_lat_count <= 0.0:
        return None, None

    return weighted_lat_sum / weighted_lat_count, sum(agg_bw_by_node.values())


def parse_cases(spec: str) -> list[tuple[str, int, float]]:
    cases: list[tuple[str, int, float]] = []
    for raw_case in spec.split(";"):
        raw_case = raw_case.strip()
        if not raw_case:
            continue
        parts = [part.strip() for part in raw_case.split(",")]
        if len(parts) != 3:
            raise ValueError(
                f"invalid CDF case '{raw_case}'; expected config,load_pct,inject_bw"
            )
        config_name = parts[0]
        load_pct = int(parts[1])
        inject_bw = float(parts[2])
        cases.append((config_name, load_pct, inject_bw))
    return cases


def write_summary(rows: list[dict[str, object]], out_csv: Path) -> None:
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "config",
                "num_nodes",
                "load_pct",
                "requested_request_gbps_per_node",
                "aggregate_bw_gbps",
                "latency_cycles",
                "log_path",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)
    print(f"[STATUS] Wrote summary: {out_csv}")


def main() -> int:
    print("[STATUS] Starting pointer-chase CDF point experiment")
    print(f"[STATUS] CDF cases: {CASE_SPEC}")
    build_generator()
    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    trace_path = generate_trace(TRACE_ROOT)
    config_path = OUTPUT_ROOT / f"cxl_config_nodes{NUM_NODES:03d}.csv"
    write_cxl_config(config_path, NUM_NODES)

    failures = 0
    rows: list[dict[str, object]] = []
    for config_name, load_pct, inject_bw in parse_cases(CASE_SPEC):
        replicate_writes, num_pools = parse_config_name(config_name)
        bw_token = format_bw_token(inject_bw)
        out_path = OUTPUT_ROOT / f"run_cdf_{config_name}_load{load_pct:03d}_bw{bw_token}.out"
        err_path = OUTPUT_ROOT / f"run_cdf_{config_name}_load{load_pct:03d}_bw{bw_token}.err"
        rc = run_sst(
            trace_path,
            config_path,
            inject_bw,
            load_pct,
            config_name,
            replicate_writes,
            num_pools,
            out_path,
            err_path,
        )
        if rc != 0:
            print(f"[FAIL] {out_path.name} (rc={rc})")
            failures += 1
            continue

        avg_latency, aggregate_bw = parse_metrics(out_path)
        if avg_latency is None or aggregate_bw is None:
            print(f"[FAIL] missing complete stats in {out_path}")
            failures += 1
            continue

        rows.append({
            "config": config_name,
            "num_nodes": NUM_NODES,
            "load_pct": load_pct,
            "requested_request_gbps_per_node": inject_bw,
            "aggregate_bw_gbps": aggregate_bw,
            "latency_cycles": avg_latency,
            "log_path": str(out_path),
        })
        print(
            f"[STATUS] config={config_name} load_pct={load_pct} req_bw_per_node={inject_bw:.3f} "
            f"total_agg_bw={aggregate_bw:.3f} avg_load_issue_to_complete_lat={avg_latency:.3f}"
        )

    if rows:
        write_summary(rows, OUTPUT_ROOT / "cdf_points_summary.csv")

    if failures:
        print(f"CDF point experiment finished with {failures} failures.")
        return 1
    print("CDF point experiment complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
