#!/usr/bin/env python3
import concurrent.futures
import itertools
import os
import subprocess
from pathlib import Path

# use: python3 experiments/pointer_chase/run_sweep.py

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path(os.environ.get("TRACE_ROOT", "/shared/kshan/CXL_sst_traces_pointer_chase"))
OUTPUT_ROOT = SCRIPT_DIR / "logs"
CONFIG_PATH = SCRIPT_DIR / "cxl_config.csv"
SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen_pointer_chase"
GEN_SRC = REPO_ROOT / "scripts" / "generate_pointer_chase_trace.cpp"

MPI_RANKS = 1
MAX_CORE_BUDGET = 160
DEFAULT_MAX_PARALLEL = min(20, max(1, MAX_CORE_BUDGET // max(MPI_RANKS, 1)))
MAX_PARALLEL = int(os.environ.get("MAX_PARALLEL", DEFAULT_MAX_PARALLEL))

LOAD_PCTS = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
MEM_PCTS = [10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

NUM_INSTRS = 10_000
SEED = 0x12345678
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20
INJECT_PEAK_GBPS = 12.0

SIM_SCRIPT = SCRIPT_DIR / "pool_sweep.py"


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


def run_sst(trace_path: Path, cxl_config: Path, out_path: Path, err_path: Path, inject_bw: float, inject_load_pct: int) -> int:
    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(cxl_config)
    env["INJECT_BANDWIDTH_GBPS"] = str(inject_bw)
    env["INJECT_LOAD_PCT"] = str(inject_load_pct)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)
    print(
        f"[STATUS] Launching {SIM_SCRIPT.name} -> {out_path.name} "
        f"(inject_bw={inject_bw}, load_pct={inject_load_pct})"
    )
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
    print("[STATUS] Starting pointer-chase injector sweep")
    print(f"[STATUS] Pointer trace instructions={NUM_INSTRS}")
    print("[STATUS] SST warmup=1000 main=5000")
    print(f"[STATUS] Injector request-bandwidth peak target={INJECT_PEAK_GBPS} Gbps")

    build_generator()
    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    trace_path = generate_trace(TRACE_ROOT)

    tasks = []
    for load_pct, mem_pct in itertools.product(LOAD_PCTS, MEM_PCTS):
        inject_bw = (mem_pct / 100.0) * INJECT_PEAK_GBPS
        out_path = OUTPUT_ROOT / f"run_load{load_pct:03d}_mem{mem_pct:03d}.out"
        err_path = OUTPUT_ROOT / f"run_load{load_pct:03d}_mem{mem_pct:03d}.err"
        tasks.append((trace_path, CONFIG_PATH, out_path, err_path, inject_bw, load_pct))

    total_tasks = len(tasks)
    print(f"[STATUS] Launching {total_tasks} runs with up to {MAX_PARALLEL} in parallel")
    failures = 0
    completed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL) as executor:
        future_to_task = {executor.submit(run_sst, *task): task for task in tasks}
        for future in concurrent.futures.as_completed(future_to_task):
            _, _, out_path, _, _, _ = future_to_task[future]
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
