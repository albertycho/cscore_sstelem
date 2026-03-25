#!/usr/bin/env python3
import concurrent.futures
import csv
import itertools
import os
import subprocess
from pathlib import Path

# use: python3 experiments/load_store_util_sweep_direct_single_node/run_sweep.py


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path(os.environ.get("TRACE_ROOT", "/shared/kshan/CXL_sst_traces_direct_single_node"))
OUTPUT_ROOT = SCRIPT_DIR / "logs"
CONFIG_PATH = SCRIPT_DIR / "cxl_config.csv"
SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen"
GEN_SRC = REPO_ROOT / "scripts" / "generate_synth_trace.cpp"

MPI_RANKS = 1
MAX_CORE_BUDGET = 160
DEFAULT_MAX_PARALLEL = min(20, max(1, MAX_CORE_BUDGET // max(MPI_RANKS, 1)))
MAX_PARALLEL = int(os.environ.get("MAX_PARALLEL", DEFAULT_MAX_PARALLEL))

# Sweep space (11x10)
LOAD_PCTS = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
MEM_PCTS = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

# Trace generation parameters
NUM_INSTRS = 4_000_000
WARM_CACHE_INSTS = 200_000
SEED = 0x12345678
CXL_PCT = 100
AGGREGATE_PEAK_GBPS = 12.0

# Must match pool_sweep.py
WARMUP_MAIN_INSTS = 100_000
SST_WARMUP_INSTS = WARM_CACHE_INSTS + WARMUP_MAIN_INSTS

# Fixed address/WS parameters (must match generator defaults)
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20

# SST topology parameters
NUM_NODES = MPI_RANKS
POOL_NODE_ID_BASE = 100

SIM_SCRIPT = SCRIPT_DIR / "pool_sweep.py"


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


def write_cxl_config(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"[STATUS] Writing CXL config: {path}")
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["# node_id", "start", "size", "type", "target"])
        for node in range(NUM_NODES):
            writer.writerow([node, f"0x{CXL_BASE:x}", f"0x{CXL_WS_BYTES:x}", "pool", POOL_NODE_ID_BASE])


def generate_trace(out_dir: Path, trace_name: str, load_pct: int, mem_pct: int) -> tuple[Path, str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    trace_path = out_dir / trace_name
    print(f"[STATUS] Generating trace {trace_name} (load={load_pct} mem={mem_pct})")

    cmd = [
        str(GEN_BIN),
        "--out-dir", str(out_dir),
        "--out-name", trace_name,
        "--num-instrs", str(NUM_INSTRS),
        "--warm-cache-instrs", str(WARM_CACHE_INSTS),
        "--mem-pct", str(mem_pct),
        "--load-pct", str(load_pct),
        "--cxl-pct", str(CXL_PCT),
        "--aggregate-peak-gbps", str(AGGREGATE_PEAK_GBPS),
        "--num-nodes", str(NUM_NODES),
        "--seed", hex(SEED),
    ]
    result = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True)
    return trace_path, result.stdout


def run_sst(trace_path: Path, cxl_config: Path, out_path: Path, err_path: Path) -> int:
    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(cxl_config)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)
    print(f"[STATUS] Launching {SIM_SCRIPT.name} -> {out_path.name}")
    with out_path.open("w") as out_f, err_path.open("w") as err_f:
        proc = subprocess.run(
            ["mpirun", "-n", str(MPI_RANKS), SST_BIN, str(SIM_SCRIPT)],
            cwd=str(SCRIPT_DIR),
            env=env,
            stdout=out_f,
            stderr=err_f,
        )
    return proc.returncode


def main() -> int:
    print("[STATUS] Starting direct single-node load/store/util sweep")
    print(f"[STATUS] Using fixed aggregate synth target peak: {AGGREGATE_PEAK_GBPS} GB/s")
    print(
        "[STATUS] Warmup split: "
        f"warm_cache={WARM_CACHE_INSTS}, "
        f"main_loop_warmup={WARMUP_MAIN_INSTS}, "
        f"total_warmup={SST_WARMUP_INSTS}"
    )
    build_generator()

    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    write_cxl_config(CONFIG_PATH)

    cases = list(itertools.product(LOAD_PCTS, MEM_PCTS))
    tasks = []

    for load_pct, mem_pct in cases:
        trace_name = f"synth_load{load_pct:03d}_mem{mem_pct:03d}.champsim.trace"
        trace_path, gen_out = generate_trace(TRACE_ROOT, trace_name, load_pct, mem_pct)
        print(gen_out, end="" if gen_out.endswith("\n") else "\n")

        out_path = OUTPUT_ROOT / f"run_load{load_pct:03d}_mem{mem_pct:03d}.out"
        err_path = OUTPUT_ROOT / f"run_load{load_pct:03d}_mem{mem_pct:03d}.err"
        tasks.append((trace_path, CONFIG_PATH, out_path, err_path))

    total_tasks = len(tasks)
    print(f"[STATUS] Launching {total_tasks} runs with up to {MAX_PARALLEL} in parallel")
    failures = 0
    completed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as executor:
        future_to_task = {executor.submit(run_sst, *task): task for task in tasks}
        for future in concurrent.futures.as_completed(future_to_task):
            _, _, out_path, _ = future_to_task[future]
            try:
                rc = future.result()
            except Exception as exc:
                print(f"[FAIL] {SIM_SCRIPT.name} -> {out_path}: {exc}")
                failures += 1
                continue
            if rc != 0:
                print(f"[FAIL] {SIM_SCRIPT.name} -> {out_path} (rc={rc})")
                failures += 1
            completed += 1
            if completed % 10 == 0 or completed == total_tasks:
                print(f"[STATUS] Completed {completed}/{total_tasks}")

    if failures:
        print(f"Completed with {failures} failures.")
        return 1
    print("Sweep complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

