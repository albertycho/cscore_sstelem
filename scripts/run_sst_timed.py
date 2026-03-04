#!/usr/bin/env python3
import os
import subprocess
import time

# -----------------------------
# User configuration
# -----------------------------
# Example:
# CMD = ["mpirun", "-n", "4", "sst", "experiments/replication/pool.py"]
CMD = ["sst", "experiments/replication/pool.py"]
WORKDIR = None  # e.g., "/nethome/kshan9/scratch/src/sst-elements/src/sst/elements/cscore_sstelem"


def main():
    if WORKDIR:
        os.chdir(WORKDIR)
    start = time.monotonic()
    subprocess.run(CMD, check=True)
    end = time.monotonic()
    print(f"TOTAL SIM WALLTIME (s): {end - start:.6f}")


if __name__ == "__main__":
    main()
