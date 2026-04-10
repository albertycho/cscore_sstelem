#!/usr/bin/env python3
import concurrent.futures
import os
import re
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

NUM_INSTRS = 10_000
SEED = 0x12345678
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20
CLOCK_GHZ = 2.4
LINK_BW_CYCLES = 25
LATENCY_THRESHOLD = float(os.environ.get("LATENCY_THRESHOLD", "1000.0"))
SEARCH_LEFT_FRAC = float(os.environ.get("SEARCH_LEFT_FRAC", "0.50"))
SEARCH_RIGHT_FRAC = float(os.environ.get("SEARCH_RIGHT_FRAC", "2.00"))
SEARCH_SHRINK_FRAC = float(os.environ.get("SEARCH_SHRINK_FRAC", "0.25"))
MAX_SEARCH_ITERS = int(os.environ.get("MAX_SEARCH_ITERS", "15"))
# MIN_WINDOW_GBPS = float(os.environ.get("MIN_WINDOW_GBPS", "0.05"))
FULL_CAPACITY_GBPS = float(os.environ.get("FULL_CAPACITY_GBPS", "49.152"))
MAX_GRAPH_BROADCAST_RETRIES = int(os.environ.get("MAX_GRAPH_BROADCAST_RETRIES", "2"))

SIM_SCRIPT = SCRIPT_DIR / "pool_sweep.py"
LOAD_LAT_RE = re.compile(r"stat\.node\.0\.cpu\.0\.avg_load_issue_to_complete_lat\s*=\s*([0-9eE+.\-]+)")
AGG_BW_RE = re.compile(r"stat\.node\.0\.injector\.aggregate_gbps\s*=\s*([0-9eE+.\-]+)")


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


def is_retryable_startup_failure(out_path: Path, err_path: Path) -> bool:
    if not out_path.exists() or not err_path.exists():
        return False
    if out_path.stat().st_size != 0:
        return False
    err_text = err_path.read_text(errors="ignore")
    return "Error encountered during graph broadcast" in err_text


def link_peak_gbps() -> float:
    bits_per_cycle = 64.0 * 8.0 / LINK_BW_CYCLES
    return bits_per_cycle * (CLOCK_GHZ * 1e9) / 1e9


def ratio_target_request_gbps(load_pct: int) -> float:
    load_frac = load_pct / 100.0
    fwd_bytes = (8.0 * load_frac) + (64.0 * (1.0 - load_frac))
    rev_bytes = 64.0 * load_frac
    peak = FULL_CAPACITY_GBPS
    if rev_bytes <= 0.0:
        return peak
    return min(peak, peak * (fwd_bytes / rev_bytes))


def format_bw_token(inject_bw: float) -> str:
    return f"{inject_bw:05.2f}".replace(".", "p")


def parse_load_latency(out_path: Path) -> float | None:
    text = out_path.read_text()
    match = LOAD_LAT_RE.search(text)
    if match is None:
        return None
    return float(match.group(1))


def parse_aggregate_gbps(out_path: Path) -> float | None:
    text = out_path.read_text()
    match = AGG_BW_RE.search(text)
    if match is None:
        return None
    return float(match.group(1))


def clear_previous_outputs() -> None:
    removed = 0
    for pattern in ("run_load*.out", "run_load*.err"):
        for path in OUTPUT_ROOT.glob(pattern):
            path.unlink()
            removed += 1
    for path in (
        OUTPUT_ROOT / "pointer_chase_bw_latency.csv",
        OUTPUT_ROOT / "pointer_chase_bw_latency.png",
    ):
        if path.exists():
            path.unlink()
            removed += 1
    if removed:
        print(f"[STATUS] Cleared {removed} prior generated files from {OUTPUT_ROOT}")


def run_load_sweep(trace_path: Path, cxl_config: Path, load_pct: int) -> int:
    left = ratio_target_request_gbps(load_pct) * SEARCH_LEFT_FRAC
    right = ratio_target_request_gbps(load_pct) * SEARCH_RIGHT_FRAC
    sampled_bws: set[float] = set()
    run_count = 0
    print(
        f"[STATUS] load_pct={load_pct}: target request bw={ratio_target_request_gbps(load_pct):.3f} Gbps, "
        f"search window=[{left:.3f}, {right:.3f}]"
    )

    def run_sample(label: str, inject_bw: float) -> tuple[int, float]:
        nonlocal run_count
        inject_bw = round(inject_bw, 6)
        sampled_bws.add(inject_bw)
        bw_token = format_bw_token(inject_bw)
        out_path = OUTPUT_ROOT / f"run_load{load_pct:03d}_{label}_bw{bw_token}.out"
        err_path = OUTPUT_ROOT / f"run_load{load_pct:03d}_{label}_bw{bw_token}.err"
        rc = run_sst(trace_path, cxl_config, out_path, err_path, inject_bw, load_pct)
        run_count += 1
        if rc != 0:
            print(f"[FAIL] {SIM_SCRIPT.name} -> {out_path} (rc={rc})")
            return rc, 0.0

        latency = parse_load_latency(out_path)
        if latency is None:
            print(f"[FAIL] missing avg_load_issue_to_complete_lat in {out_path}")
            return 1, 0.0
        agg_gbps = parse_aggregate_gbps(out_path)
        if agg_gbps is None:
            print(f"[FAIL] missing injector aggregate_gbps in {out_path}")
            return 1, 0.0

        print(
            f"[STATUS] load_pct={load_pct} label={label} req_bw={inject_bw:.3f} "
            f"agg_bw={agg_gbps:.3f} avg_load_issue_to_complete_lat={latency:.3f}"
        )
        return 0, latency

    for label, inject_bw in (("base", 0.3), ("left", left), ("right", right)):
        inject_bw = round(inject_bw, 6)
        if inject_bw in sampled_bws:
            continue
        rc, _ = run_sample(label, inject_bw)
        if rc != 0:
            return 1

    for iter_idx in range(MAX_SEARCH_ITERS):
        window = right - left
        # if window <= MIN_WINDOW_GBPS:
        #     print(
        #         f"[STATUS] load_pct={load_pct}: stopping because window narrowed to "
        #         f"{window:.3f} Gbps"
        #     )
        #     break

        inject_bw = round((left + right) / 2.0, 6)
        if inject_bw in sampled_bws:
            print(
                f"[STATUS] load_pct={load_pct}: stopping because midpoint {inject_bw:.6f} repeated"
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

    print(f"[STATUS] load_pct={load_pct}: completed {run_count} runs")
    return 0


def main() -> int:
    print("[STATUS] Starting pointer-chase injector sweep")
    print(f"[STATUS] Pointer trace instructions={NUM_INSTRS}")
    print("[STATUS] SST warmup=1000 main=4000")
    print(f"[STATUS] Link peak per direction={link_peak_gbps():.3f} Gbps")
    print(f"[STATUS] Full-capacity reference={FULL_CAPACITY_GBPS:.3f} Gbps")
    print(f"[STATUS] Latency threshold={LATENCY_THRESHOLD} cycles")
    # print(
    #     f"[STATUS] Search window fractions=[{SEARCH_LEFT_FRAC:.2f}, {SEARCH_RIGHT_FRAC:.2f}], "
    #     f"shrink={SEARCH_SHRINK_FRAC:.2f}, max_iters={MAX_SEARCH_ITERS}, min_window={MIN_WINDOW_GBPS:.3f} Gbps"
    # )

    build_generator()
    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    clear_previous_outputs()
    trace_path = generate_trace(TRACE_ROOT)

    total_lanes = len(LOAD_PCTS)
    parallel_lanes = min(MAX_PARALLEL, total_lanes)
    print(f"[STATUS] Launching {total_lanes} load_pct lanes with up to {parallel_lanes} in parallel")
    failures = 0
    completed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=parallel_lanes) as executor:
        future_to_load = {executor.submit(run_load_sweep, trace_path, CONFIG_PATH, load_pct): load_pct for load_pct in LOAD_PCTS}
        for future in concurrent.futures.as_completed(future_to_load):
            load_pct = future_to_load[future]
            try:
                rc = future.result()
            except Exception as exc:
                print(f"[FAIL] load_pct={load_pct}: {exc}")
                failures += 1
                continue
            if rc != 0:
                print(f"[FAIL] load_pct={load_pct} lane failed")
                failures += 1
            completed += 1
            print(f"[STATUS] Completed {completed}/{total_lanes} load_pct lanes")

    if failures:
        print(f"Completed with {failures} failures.")
        return 1
    print("Sweep complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
