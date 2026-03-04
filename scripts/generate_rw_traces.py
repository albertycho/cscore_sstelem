#!/usr/bin/env python3
import itertools
import os
import subprocess
import sys

# -----------------------------
# User configuration
# -----------------------------
PIN_BIN = None  # e.g., "/nethome/kshan9/pin_dir/pin-3.30/pin"
TRACER_SO = None  # e.g., "/nethome/kshan9/pin_dir/ChampSim/tracer/pin/obj-intel64/champsim_tracer.so"
RW_EXE = "experiments/replication/rw"
OUT_DIR = "/shared/kshan/CXL_sst_traces"

SKIP_INSTS = 130_000_000
TRACE_INSTS = 20_000_000

ITERS = [10_000_000]
LOAD_PCT = [50]
CXL_PCT = [100]
MEM_OPS = [1]
COMPUTE_OPS = [0]

PREFIX = "rw_"
SUFFIX = ""

DRY_RUN = False
OVERWRITE = False


def resolve_pin(pin_arg):
    if pin_arg:
        return pin_arg
    pin_root = os.getenv("PIN_ROOT")
    if not pin_root:
        return None
    return os.path.join(pin_root, "pin")


def resolve_tracer(tracer_arg):
    if tracer_arg:
        return tracer_arg
    return os.getenv("CHAMPSIM_TRACER_SO")


def build_out_name(params):
    iters, load_pct, cxl_pct, mem_ops, compute_ops = params
    name = (
        f"{PREFIX}"
        f"it{iters}_ld{load_pct}_cxl{cxl_pct}_mem{mem_ops}_comp{compute_ops}"
        f"_skip{SKIP_INSTS}_trace{TRACE_INSTS}"
    )
    if SUFFIX:
        name += f"_{SUFFIX}"
    return f"{name}.champsim.trace"


def main():
    pin = resolve_pin(PIN_BIN)
    tracer = resolve_tracer(TRACER_SO)

    if not pin or not os.path.exists(pin):
        print("error: pin binary not found. Set PIN_BIN or PIN_ROOT.", file=sys.stderr)
        return 2
    if not tracer or not os.path.exists(tracer):
        print("error: tracer .so not found. Set TRACER_SO or CHAMPSIM_TRACER_SO.", file=sys.stderr)
        return 2
    if not os.path.exists(RW_EXE):
        print(f"error: rw executable not found: {RW_EXE}", file=sys.stderr)
        return 2

    os.makedirs(OUT_DIR, exist_ok=True)

    combos = itertools.product(ITERS, LOAD_PCT, CXL_PCT, MEM_OPS, COMPUTE_OPS)

    for iters, load_pct, cxl_pct, mem_ops, compute_ops in combos:
        out_name = build_out_name((iters, load_pct, cxl_pct, mem_ops, compute_ops))
        out_path = os.path.join(OUT_DIR, out_name)
        if os.path.exists(out_path) and not OVERWRITE:
            print(f"skip existing: {out_path}")
            continue

        cmd = [
            pin,
            "-t",
            tracer,
            "-s",
            str(SKIP_INSTS),
            "-t",
            str(TRACE_INSTS),
            "-o",
            out_path,
            "--",
            os.path.abspath(RW_EXE),
            "--iters",
            str(iters),
            "--load-pct",
            str(load_pct),
            "--cxl-pct",
            str(cxl_pct),
            "--mem-ops",
            str(mem_ops),
            "--compute-ops",
            str(compute_ops),
        ]

        print(" ".join(cmd))
        if not DRY_RUN:
            subprocess.run(cmd, check=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
