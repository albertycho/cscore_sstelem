#!/usr/bin/env python3
import concurrent.futures
import csv
import itertools
import os
import re
import subprocess
from pathlib import Path

# use: python3 experiments/load_store_util_sweep/run_sweep.py


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path("/shared/kshan/CXL_sst_traces")
OUTPUT_ROOT = SCRIPT_DIR / "logs"
CONFIG_PATH = SCRIPT_DIR / "cxl_config.csv"
SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen"
GEN_SRC = REPO_ROOT / "scripts" / "generate_synth_trace.cpp"

MPI_RANKS = 8
MAX_CORE_BUDGET = 160
MAX_PARALLEL = max(1, MAX_CORE_BUDGET // MPI_RANKS)

# Sweep space (10x10)
LOAD_PCTS = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
MEM_PCTS = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

# Trace generation parameters
NUM_INSTRS = 500_000
WARMUP_MEM_INSTRS = 200_000
SEED = 0x12345678
CXL_PCT = 100

# Fixed address/WS parameters (must match generator defaults)
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20

# SST topology parameters
NUM_NODES = MPI_RANKS
NUM_POOLS = 2
POOL_NODE_ID_BASE = 100

# Output switches
LIGHTWEIGHT_OUTPUT = "1"
PRINT_LAT_HIST = "1"

SIM_NO_REP = SCRIPT_DIR / "pool_sweep.py"
SIM_REP = SCRIPT_DIR / "pool_sweep_replication.py"


def build_generator() -> None:
    if GEN_BIN.exists():
        return
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
    with path.open("w") as f:
        f.write("# node_id,start,size,type,target\n")
        for node in range(NUM_NODES):
            f.write(f"{node},0x{CXL_BASE:x},0x{CXL_WS_BYTES:x},pool,{POOL_NODE_ID_BASE}\n")


def generate_trace(out_dir: Path, trace_name: str, load_pct: int, mem_pct: int) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    trace_path = out_dir / trace_name
    print(f"[STATUS] Generating trace {trace_name} (load={load_pct} mem={mem_pct})")

    cmd = [
        str(GEN_BIN),
        "--out-dir", str(out_dir),
        "--out-name", trace_name,
        "--num-instrs", str(NUM_INSTRS),
        "--warmup-mem-instrs", str(WARMUP_MEM_INSTRS),
        "--mem-pct", str(mem_pct),
        "--load-pct", str(load_pct),
        "--cxl-pct", str(CXL_PCT),
        "--seed", hex(SEED),
    ]
    result = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True)
    return trace_path, result.stdout


BW_LINE_RE = re.compile(
    r"load_local:\\s*([0-9.eE+-]+)\\s*"
    r"load_cxl:\\s*([0-9.eE+-]+)\\s*"
    r"store_local:\\s*([0-9.eE+-]+)\\s*"
    r"store_cxl:\\s*([0-9.eE+-]+)\\s*"
    r"total:\\s*([0-9.eE+-]+)"
)


def parse_bw(output: str) -> dict:
    for line in output.splitlines():
        match = BW_LINE_RE.search(line)
        if match:
            return {
                "load_local_gbps": float(match.group(1)),
                "load_cxl_gbps": float(match.group(2)),
                "store_local_gbps": float(match.group(3)),
                "store_cxl_gbps": float(match.group(4)),
                "total_gbps": float(match.group(5)),
            }
    return {}


def run_sst(sim_script: Path, trace_path: Path, cxl_config: Path, out_path: Path, err_path: Path) -> int:

    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(cxl_config)
    env["LIGHTWEIGHT_OUTPUT"] = LIGHTWEIGHT_OUTPUT
    env["PRINT_LAT_HIST"] = PRINT_LAT_HIST

    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Create files immediately so they show up even before MPI starts writing.
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)
    print(f"[STATUS] Launching {sim_script.name} -> {out_path.name}")
    with out_path.open("w") as out_f, err_path.open("w") as err_f:
        proc = subprocess.run(
            ["mpirun", "-n", str(MPI_RANKS), SST_BIN, str(sim_script)],
            cwd=str(SCRIPT_DIR),
            env=env,
            stdout=out_f,
            stderr=err_f,
        )
    return proc.returncode


def main() -> int:
    print("[STATUS] Starting load/store/util sweep")
    build_generator()

    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    write_cxl_config(CONFIG_PATH)

    cases = list(itertools.product(LOAD_PCTS, MEM_PCTS))
    tasks = []
    bw_rows = []

    for load_pct, mem_pct in cases:
        trace_name = f"synth_load{load_pct:03d}_mem{mem_pct:03d}.champsim.trace"
        trace_path, gen_out = generate_trace(TRACE_ROOT, trace_name, load_pct, mem_pct)

        bw = parse_bw(gen_out)
        if bw:
            bw_row = {
                "load_pct": load_pct,
                "mem_pct": mem_pct,
                "cxl_pct": CXL_PCT,
                "num_instrs": NUM_INSTRS,
                "warmup_mem_instrs": WARMUP_MEM_INSTRS,
            }
            bw_row.update(bw)
            bw_rows.append(bw_row)

        no_rep_out = OUTPUT_ROOT / f"run_load{load_pct:03d}_mem{mem_pct:03d}_no_rep.out"
        no_rep_err = OUTPUT_ROOT / f"run_load{load_pct:03d}_mem{mem_pct:03d}_no_rep.err"
        rep_out = OUTPUT_ROOT / f"run_load{load_pct:03d}_mem{mem_pct:03d}_rep.out"
        rep_err = OUTPUT_ROOT / f"run_load{load_pct:03d}_mem{mem_pct:03d}_rep.err"

        tasks.append((SIM_NO_REP, trace_path, CONFIG_PATH, no_rep_out, no_rep_err))
        tasks.append((SIM_REP, trace_path, CONFIG_PATH, rep_out, rep_err))

    if bw_rows:
        bw_rows.sort(key=lambda r: (r["load_pct"], r["mem_pct"]))
        out_csv = OUTPUT_ROOT / "expected_bandwidths.csv"
        with out_csv.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(bw_rows[0].keys()))
            writer.writeheader()
            writer.writerows(bw_rows)

    total_tasks = len(tasks)
    print(f"[STATUS] Launching {total_tasks} runs with up to {MAX_PARALLEL} in parallel")
    failures = 0
    completed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as executor:
        future_to_task = {executor.submit(run_sst, *task): task for task in tasks}
        for future in concurrent.futures.as_completed(future_to_task):
            sim_script, _, _, out_path, _ = future_to_task[future]
            try:
                rc = future.result()
            except Exception as exc:
                print(f"[FAIL] {sim_script.name} -> {out_path}: {exc}")
                failures += 1
                continue
            if rc != 0:
                print(f"[FAIL] {sim_script.name} -> {out_path} (rc={rc})")
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
