#!/usr/bin/env python3
import os
import subprocess
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent

TRACE_ROOT = SCRIPT_DIR / "traces"
OUTPUT_ROOT = SCRIPT_DIR / "logs"
CONFIG_PATH = SCRIPT_DIR / "cxl_config.csv"
SST_BIN = os.environ.get("SST_BIN", "sst")
GEN_BIN = REPO_ROOT / "scripts" / "gen"
GEN_SRC = REPO_ROOT / "scripts" / "generate_synth_trace.cpp"
SIM_SCRIPT = SCRIPT_DIR / "pool_pair.py"

NUM_INSTRS = 4_000_000
WARM_CACHE_INSTS = 200_000
SEED = 0x12345678
CXL_PCT = 100
DEFAULT_MEM_PCT = 100
DEFAULT_LOAD_PCTS = [0, 12, 40, 100]
DEFAULT_AGGREGATE_PEAK_GBPS = 12.0
NUM_NODES = 1
CXL_BASE = 64 << 30
CXL_WS_BYTES = 8 << 20


def parse_load_pcts() -> list[int]:
    raw = os.environ.get("LOAD_PCTS")
    if not raw:
        return DEFAULT_LOAD_PCTS[:]
    vals = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        vals.append(int(part))
    if not vals:
        raise ValueError("LOAD_PCTS produced no values")
    return vals


def build_generator() -> None:
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


def write_cxl_config() -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with CONFIG_PATH.open("w") as f:
        f.write("# node_id,start,size,type,target\n")
        f.write(f"0,0x{CXL_BASE:x},0x{CXL_WS_BYTES:x},pool,100\n")


def generate_trace(load_pct: int, mem_pct: int, aggregate_peak_gbps: float) -> Path:
    TRACE_ROOT.mkdir(parents=True, exist_ok=True)
    trace_name = f"synth_load{load_pct:03d}_mem{mem_pct:03d}.champsim.trace"
    trace_path = TRACE_ROOT / trace_name
    cmd = [
        str(GEN_BIN),
        "--out-dir", str(TRACE_ROOT),
        "--out-name", trace_name,
        "--num-instrs", str(NUM_INSTRS),
        "--warm-cache-instrs", str(WARM_CACHE_INSTS),
        "--mem-pct", str(mem_pct),
        "--load-pct", str(load_pct),
        "--cxl-pct", str(CXL_PCT),
        "--aggregate-peak-gbps", str(aggregate_peak_gbps),
        "--num-nodes", str(NUM_NODES),
        "--seed", hex(SEED),
    ]
    subprocess.check_call(cmd)
    return trace_path


def run_case(load_pct: int, mem_pct: int, trace_path: Path) -> None:
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    case_name = f"load{load_pct:03d}_mem{mem_pct:03d}"
    out_path = OUTPUT_ROOT / f"{case_name}.out"
    err_path = OUTPUT_ROOT / f"{case_name}.err"
    timeline_path = OUTPUT_ROOT / f"{case_name}.timeline.csv"

    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["CXL_CONFIG_PATH"] = str(CONFIG_PATH)
    env["LIGHTWEIGHT_OUTPUT"] = "1"
    env["PRINT_LAT_HIST"] = "0"
    env["CSCORE_RESPONSE_TIMELINE_PATH"] = str(timeline_path)

    with out_path.open("w") as out_f, err_path.open("w") as err_f:
        subprocess.check_call(
            [SST_BIN, str(SIM_SCRIPT)],
            cwd=str(SCRIPT_DIR),
            env=env,
            stdout=out_f,
            stderr=err_f,
        )


def main() -> int:
    mem_pct = int(os.environ.get("MEM_PCT", str(DEFAULT_MEM_PCT)))
    aggregate_peak_gbps = float(os.environ.get("AGGREGATE_PEAK_GBPS", str(DEFAULT_AGGREGATE_PEAK_GBPS)))
    load_pcts = parse_load_pcts()

    build_generator()
    write_cxl_config()
    for load_pct in load_pcts:
        trace_path = generate_trace(load_pct, mem_pct, aggregate_peak_gbps)
        run_case(load_pct, mem_pct, trace_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
