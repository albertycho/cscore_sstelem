#!/usr/bin/env python3
import concurrent.futures
import csv
import os
import re
import subprocess
from pathlib import Path

# use: python3 experiments/load_store_util_sweep/run_iterative_bw_sweep.py


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = Path("/shared/kshan/CXL_sst_traces_single_node")
OUTPUT_ROOT = SCRIPT_DIR / "logs_iterative"
CONFIG_PATH = SCRIPT_DIR / "cxl_config.csv"
SST_BIN = "sst"
GEN_BIN = REPO_ROOT / "scripts" / "gen"
GEN_SRC = REPO_ROOT / "scripts" / "generate_synth_trace.cpp"

SIM_NO_REP = SCRIPT_DIR / "pool_sweep.py"
SIM_REP = SCRIPT_DIR / "pool_sweep_replication.py"

MPI_RANKS = 1
MAX_CORE_BUDGET = 160
MAX_PARALLEL_CASES = max(1, MAX_CORE_BUDGET // MPI_RANKS)

# Per-case sweep space: one curve per load_pct and mode
LOAD_PCTS = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]

# Iterative target stepping (GB/s)
TARGET_START_GBPS = 1.0
TARGET_STEP_GBPS = 0.5
TARGET_MAX_GBPS = 20.0

# Stop criteria
STOP_OBS_SWITCH_BW_GBPS = 12.5
STOP_AVG_CXL_LAT_CYCLES = 3000.0
PLATEAU_DELTA_GBPS = 0.10
PLATEAU_STEPS = 3

# Trace generation parameters
NUM_INSTRS = 4_000_000
WARM_CACHE_INSTS = 200_000
SEED = 0x12345678
CXL_PCT = 100

# Must match pool_sweep*.py
WARMUP_MAIN_INSTS = 100_000
SST_WARMUP_INSTS = WARM_CACHE_INSTS + WARMUP_MAIN_INSTS

# Fixed address/WS parameters (must match generator defaults)
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20

# SST topology parameters
NUM_NODES = MPI_RANKS
POOL_NODE_ID_BASE = 100

# Output switches
LIGHTWEIGHT_OUTPUT = "1"
PRINT_LAT_HIST = "1"

BW_GBPS_LINE_RE = re.compile(
    r"PROJECTED_HOST_LINK_BW_GBPS\s+"
    r"host_to_switch=([0-9.eE+-]+)\s+"
    r"switch_to_host=([0-9.eE+-]+)\s+"
    r"total=([0-9.eE+-]+)"
)
MAIN_LOOP_COUNTS_RE = re.compile(r"main_loop_loads=(\d+)\s+main_loop_stores=(\d+)")

STAT_MISS_RE = re.compile(r"^stat\.node\.(\d+)\.llc\.cxl_miss\s*=\s*([0-9]+)\s*$")
STAT_LAT_RE = re.compile(r"^stat\.node\.(\d+)\.llc\.avg_cxl_lat\s*=\s*([0-9.eE+-]+)\s*$")
STAT_POOL_LINK_UTIL_RE = re.compile(r"^stat\.pool\.(\d+)\.util\.req_link_avg\s*=\s*([0-9.eE+-]+)\s*$")
STAT_SWITCH_HOST_TO_RE = re.compile(r"^stat\.switch\.bw\.host_to_switch_gbps\s*=\s*([0-9.eE+-]+)\s*$")
STAT_SWITCH_TO_HOST_RE = re.compile(r"^stat\.switch\.bw\.switch_to_host_gbps\s*=\s*([0-9.eE+-]+)\s*$")
STAT_SWITCH_TOTAL_RE = re.compile(r"^stat\.switch\.bw\.host_link_total_gbps\s*=\s*([0-9.eE+-]+)\s*$")
SIM_TIME_RE = re.compile(r"^Simulation is complete, simulated time:\s*([0-9.eE+-]+)\s*([a-zA-Z]+)\s*$")


def to_seconds(value: float, unit: str) -> float:
    unit = unit.strip().lower()
    if unit in ("s", "sec", "secs", "second", "seconds"):
        return value
    if unit in ("ms",):
        return value * 1e-3
    if unit in ("us",):
        return value * 1e-6
    if unit in ("ns",):
        return value * 1e-9
    if unit in ("ps",):
        return value * 1e-12
    return 0.0


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
    with path.open("w") as f:
        f.write("# node_id,start,size,type,target\n")
        for node in range(NUM_NODES):
            f.write(f"{node},0x{CXL_BASE:x},0x{CXL_WS_BYTES:x},pool,{POOL_NODE_ID_BASE}\n")


def parse_projected(output: str) -> dict:
    parsed = {}
    for line in output.splitlines():
        match = BW_GBPS_LINE_RE.search(line)
        if match:
            parsed = {
                "projected_host_to_switch_gbps": float(match.group(1)),
                "projected_switch_to_host_gbps": float(match.group(2)),
                "projected_host_link_total_gbps": float(match.group(3)),
            }
            break
    for line in output.splitlines():
        match = MAIN_LOOP_COUNTS_RE.search(line)
        if match:
            parsed["main_loop_loads"] = int(match.group(1))
            parsed["main_loop_stores"] = int(match.group(2))
            break
    return parsed


def generate_trace(out_dir: Path, trace_name: str, load_pct: int, target_gbps: float) -> tuple[Path, dict]:
    out_dir.mkdir(parents=True, exist_ok=True)
    trace_path = out_dir / trace_name
    cmd = [
        str(GEN_BIN),
        "--out-dir", str(out_dir),
        "--out-name", trace_name,
        "--num-instrs", str(NUM_INSTRS),
        "--warm-cache-instrs", str(WARM_CACHE_INSTS),
        "--mem-pct", "100",
        "--load-pct", str(load_pct),
        "--cxl-pct", str(CXL_PCT),
        "--aggregate-peak-gbps", f"{target_gbps:.6f}",
        "--seed", hex(SEED),
    ]
    result = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True)
    return trace_path, parse_projected(result.stdout)


def weighted_avg_latency(node_miss, node_lat):
    numer = 0.0
    denom = 0
    for node, miss in node_miss.items():
        lat = node_lat.get(node)
        if lat is None:
            continue
        numer += lat * miss
        denom += miss
    if denom == 0:
        return 0.0, 0
    return numer / float(denom), denom


def parse_run_output(out_path: Path) -> dict:
    node_miss = {}
    node_lat = {}
    pool_req_link_util = []
    sim_time_s = 0.0
    host_to_switch = 0.0
    switch_to_host = 0.0
    host_link_total = 0.0

    with out_path.open("r", errors="ignore") as f:
        for raw in f:
            line = raw.strip()
            mm = STAT_MISS_RE.match(line)
            if mm:
                node_miss[int(mm.group(1))] = int(mm.group(2))
                continue
            ml = STAT_LAT_RE.match(line)
            if ml:
                node_lat[int(ml.group(1))] = float(ml.group(2))
                continue
            mu = STAT_POOL_LINK_UTIL_RE.match(line)
            if mu:
                pool_req_link_util.append(float(mu.group(2)))
                continue
            mh2s = STAT_SWITCH_HOST_TO_RE.match(line)
            if mh2s:
                host_to_switch = float(mh2s.group(1))
                continue
            ms2h = STAT_SWITCH_TO_HOST_RE.match(line)
            if ms2h:
                switch_to_host = float(ms2h.group(1))
                continue
            mtot = STAT_SWITCH_TOTAL_RE.match(line)
            if mtot:
                host_link_total = float(mtot.group(1))
                continue
            mst = SIM_TIME_RE.match(line)
            if mst:
                sim_time_s = to_seconds(float(mst.group(1)), mst.group(2))

    avg_lat, miss_sum = weighted_avg_latency(node_miss, node_lat)
    avg_pool_util = (
        sum(pool_req_link_util) / float(len(pool_req_link_util))
        if pool_req_link_util
        else 0.0
    )
    return {
        "observed_switch_host_to_switch_gbps": host_to_switch,
        "observed_switch_to_host_gbps": switch_to_host,
        "observed_switch_host_link_total_gbps": host_link_total,
        "weighted_avg_cxl_lat_cycles": avg_lat,
        "cxl_miss_sum": miss_sum,
        "avg_pool_req_link_util": avg_pool_util,
        "simulated_time_s": sim_time_s,
    }


def run_sst(sim_script: Path, trace_path: Path, out_path: Path, err_path: Path) -> int:
    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(CONFIG_PATH)
    env["LIGHTWEIGHT_OUTPUT"] = LIGHTWEIGHT_OUTPUT
    env["PRINT_LAT_HIST"] = PRINT_LAT_HIST

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.touch(exist_ok=True)
    err_path.touch(exist_ok=True)
    with out_path.open("w") as out_f, err_path.open("w") as err_f:
        proc = subprocess.run(
            ["mpirun", "-n", str(MPI_RANKS), SST_BIN, str(sim_script)],
            cwd=str(SCRIPT_DIR),
            env=env,
            stdout=out_f,
            stderr=err_f,
        )
    return proc.returncode


def mode_script(mode: str) -> Path:
    return SIM_REP if mode == "rep" else SIM_NO_REP


def target_tag(target_gbps: float) -> str:
    return f"{int(round(target_gbps * 100)):04d}"


def run_case(mode: str, load_pct: int) -> list[dict]:
    rows = []
    script = mode_script(mode)
    recent_obs = []
    step_idx = 0
    target = TARGET_START_GBPS

    print(f"[STATUS] CASE start mode={mode} load={load_pct}")

    while target <= TARGET_MAX_GBPS + 1e-9:
        tg = target_tag(target)
        trace_name = f"synth_iter_{mode}_load{load_pct:03d}_tg{tg}.champsim.trace"
        trace_path, projected = generate_trace(TRACE_ROOT, trace_name, load_pct, target)

        out_name = f"run_iter_{mode}_load{load_pct:03d}_tg{tg}.out"
        err_name = f"run_iter_{mode}_load{load_pct:03d}_tg{tg}.err"
        out_path = OUTPUT_ROOT / out_name
        err_path = OUTPUT_ROOT / err_name

        rc = run_sst(script, trace_path, out_path, err_path)
        if rc != 0:
            rows.append(
                {
                    "mode": mode,
                    "load_pct": load_pct,
                    "target_gbps": target,
                    "step_idx": step_idx,
                    "trace_name": trace_name,
                    "out_file": out_name,
                    "err_file": err_name,
                    "stop_reason": f"run_failed_rc_{rc}",
                }
            )
            print(f"[FAIL] mode={mode} load={load_pct} target={target:.2f} rc={rc}")
            break

        stats = parse_run_output(out_path)
        row = {
            "mode": mode,
            "load_pct": load_pct,
            "target_gbps": target,
            "step_idx": step_idx,
            "trace_name": trace_name,
            "out_file": out_name,
            "err_file": err_name,
            "stop_reason": "",
        }
        row.update(projected)
        row.update(stats)
        rows.append(row)

        obs_bw = stats["observed_switch_host_link_total_gbps"]
        obs_lat = stats["weighted_avg_cxl_lat_cycles"]
        recent_obs.append(obs_bw)
        if len(recent_obs) > (PLATEAU_STEPS + 1):
            recent_obs.pop(0)

        print(
            "[STATUS] CASE step "
            f"mode={mode} load={load_pct} target={target:.2f} "
            f"obs_bw={obs_bw:.3f} lat={obs_lat:.1f}"
        )

        stop_reason = ""
        if obs_bw >= STOP_OBS_SWITCH_BW_GBPS:
            stop_reason = f"stop_bw_ge_{STOP_OBS_SWITCH_BW_GBPS:.2f}"
        elif obs_lat >= STOP_AVG_CXL_LAT_CYCLES:
            stop_reason = f"stop_lat_ge_{STOP_AVG_CXL_LAT_CYCLES:.1f}"
        elif len(recent_obs) == (PLATEAU_STEPS + 1):
            deltas = [recent_obs[i + 1] - recent_obs[i] for i in range(len(recent_obs) - 1)]
            if all(d <= PLATEAU_DELTA_GBPS for d in deltas):
                stop_reason = f"stop_plateau_le_{PLATEAU_DELTA_GBPS:.2f}_for_{PLATEAU_STEPS}_steps"

        if stop_reason:
            rows[-1]["stop_reason"] = stop_reason
            print(f"[STATUS] CASE stop mode={mode} load={load_pct}: {stop_reason}")
            break

        target += TARGET_STEP_GBPS
        step_idx += 1

    if rows and rows[-1].get("stop_reason", "") == "":
        rows[-1]["stop_reason"] = "stop_reached_target_max"
        print(f"[STATUS] CASE stop mode={mode} load={load_pct}: stop_reached_target_max")

    return rows


def write_summary(rows: list[dict], out_csv: Path) -> None:
    if not rows:
        return
    rows.sort(key=lambda r: (r.get("mode", ""), int(r.get("load_pct", 0)), float(r.get("target_gbps", 0.0))))

    keys = [
        "mode",
        "load_pct",
        "target_gbps",
        "step_idx",
        "projected_host_to_switch_gbps",
        "projected_switch_to_host_gbps",
        "projected_host_link_total_gbps",
        "observed_switch_host_to_switch_gbps",
        "observed_switch_to_host_gbps",
        "observed_switch_host_link_total_gbps",
        "weighted_avg_cxl_lat_cycles",
        "cxl_miss_sum",
        "avg_pool_req_link_util",
        "simulated_time_s",
        "trace_name",
        "out_file",
        "err_file",
        "stop_reason",
    ]
    with out_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)
    print(f"[STATUS] Wrote summary: {out_csv}")


def main() -> int:
    print("[STATUS] Starting iterative target-BW sweep")
    print(
        "[STATUS] Iterative settings: "
        f"target_start={TARGET_START_GBPS}, target_step={TARGET_STEP_GBPS}, target_max={TARGET_MAX_GBPS}, "
        f"stop_bw>={STOP_OBS_SWITCH_BW_GBPS}, stop_lat>={STOP_AVG_CXL_LAT_CYCLES}, "
        f"plateau<={PLATEAU_DELTA_GBPS} for {PLATEAU_STEPS} steps"
    )
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

    cases = []
    for mode in ("no_rep", "rep"):
        for load_pct in LOAD_PCTS:
            cases.append((mode, load_pct))

    all_rows = []
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PARALLEL_CASES) as executor:
        fut_to_case = {executor.submit(run_case, mode, load): (mode, load) for mode, load in cases}
        for fut in concurrent.futures.as_completed(fut_to_case):
            mode, load = fut_to_case[fut]
            try:
                rows = fut.result()
                all_rows.extend(rows)
            except Exception as exc:
                failures += 1
                print(f"[FAIL] CASE mode={mode} load={load}: {exc}")

    write_summary(all_rows, OUTPUT_ROOT / "iterative_bw_summary.csv")
    if failures:
        print(f"[FAIL] Completed with {failures} failed case(s)")
        return 1
    print("[STATUS] Iterative sweep complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
