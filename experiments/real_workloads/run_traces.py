#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path


THIS_DIR = Path(__file__).resolve().parent
DEFAULT_TRACE_LIST = THIS_DIR / "high_mpki_traces.txt"
DEFAULT_LOG_DIR = THIS_DIR / "logs"


def parse_args():
    parser = argparse.ArgumentParser(description="Run the 1-node pooled-memory real-workload experiment across a trace list.")
    parser.add_argument(
        "--trace-root",
        default=os.environ.get("TRACE_ROOT", ""),
        help="Root directory for traces. Relative trace names are resolved under this directory.",
    )
    parser.add_argument(
        "--trace-list",
        default=str(DEFAULT_TRACE_LIST),
        help="Text file containing one trace path or basename per line.",
    )
    parser.add_argument(
        "--trace",
        action="append",
        default=[],
        help="Explicit trace path or basename. May be passed multiple times. Overrides --trace-list when present.",
    )
    parser.add_argument(
        "--sst",
        default=os.environ.get("SST_BIN", "sst"),
        help="SST executable to invoke.",
    )
    parser.add_argument(
        "--topology",
        default=str(THIS_DIR / "pool.py"),
        help="Topology Python file passed to SST.",
    )
    parser.add_argument(
        "--log-dir",
        default=str(DEFAULT_LOG_DIR),
        help="Directory for stdout/stderr logs.",
    )
    parser.add_argument(
        "--warmup-insts",
        type=int,
        default=int(os.environ.get("WARMUP_INSTS", "5000000")),
        help="Warmup instructions per run.",
    )
    parser.add_argument(
        "--sim-insts",
        type=int,
        default=int(os.environ.get("SIM_INSTS", "20000000")),
        help="ROI instructions per run.",
    )
    parser.add_argument(
        "--lightweight-output",
        type=int,
        default=int(os.environ.get("LIGHTWEIGHT_OUTPUT", "1")),
        help="Pass LIGHTWEIGHT_OUTPUT to the topology.",
    )
    parser.add_argument(
        "--print-lat-hist",
        type=int,
        default=int(os.environ.get("PRINT_LAT_HIST", "0")),
        help="Pass PRINT_LAT_HIST to the topology.",
    )
    parser.add_argument(
        "--skip-missing",
        action="store_true",
        help="Skip traces that do not exist instead of failing immediately.",
    )
    return parser.parse_args()


def load_trace_entries(args):
    if args.trace:
        return args.trace

    trace_list_path = Path(args.trace_list)
    if not trace_list_path.exists():
        raise FileNotFoundError(f"trace list not found: {trace_list_path}")

    entries = []
    for line in trace_list_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        entries.append(line)
    return entries


def resolve_trace_path(entry: str, trace_root: Path | None) -> Path:
    raw = Path(entry)
    if raw.is_absolute():
        return raw
    if trace_root is not None:
        return trace_root / raw
    return raw


def sanitize_trace_name(path: Path) -> str:
    name = path.name
    for suffix in (".champsimtrace.gz", ".champsimtrace.xz", ".champsimtrace", ".gz", ".xz"):
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return path.stem


def run_one_trace(args, topology: Path, trace_path: Path, log_dir: Path):
    stem = sanitize_trace_name(trace_path)
    stdout_path = log_dir / f"{stem}.out"
    stderr_path = log_dir / f"{stem}.err"

    env = os.environ.copy()
    env["TRACE_PATH"] = str(trace_path)
    env["WARMUP_INSTS"] = str(args.warmup_insts)
    env["SIM_INSTS"] = str(args.sim_insts)
    env["LIGHTWEIGHT_OUTPUT"] = str(args.lightweight_output)
    env["PRINT_LAT_HIST"] = str(args.print_lat_hist)

    cmd = [args.sst, str(topology)]
    start = time.monotonic()
    with stdout_path.open("w") as stdout_file, stderr_path.open("w") as stderr_file:
        stdout_file.write(f"# CMD: {' '.join(cmd)}\n")
        stdout_file.write(f"# TRACE_PATH: {trace_path}\n")
        stdout_file.write(f"# WARMUP_INSTS: {args.warmup_insts}\n")
        stdout_file.write(f"# SIM_INSTS: {args.sim_insts}\n")
        stdout_file.flush()
        proc = subprocess.run(cmd, env=env, stdout=stdout_file, stderr=stderr_file)
    elapsed = time.monotonic() - start
    print(f"{trace_path} -> rc={proc.returncode} walltime_s={elapsed:.2f}")
    return proc.returncode


def main():
    args = parse_args()
    topology = Path(args.topology).resolve()
    trace_root = Path(args.trace_root).resolve() if args.trace_root else None
    log_dir = Path(args.log_dir).resolve()
    log_dir.mkdir(parents=True, exist_ok=True)

    entries = load_trace_entries(args)
    if not entries:
        raise RuntimeError("no traces specified")

    failures = 0
    for entry in entries:
        trace_path = resolve_trace_path(entry, trace_root)
        if not trace_path.exists():
            msg = f"missing trace: {trace_path}"
            if args.skip_missing:
                print(f"skip: {msg}")
                continue
            raise FileNotFoundError(msg)
        rc = run_one_trace(args, topology, trace_path, log_dir)
        if rc != 0:
            failures += 1

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
