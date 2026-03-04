#!/usr/bin/env bash
set -euo pipefail

for i in 1 2 4 8 16; do
  MPI_RANKS="$i" mpirun -n "$i" --output-filename "logs/run_${i}.out" sst pool_with_replication_mpi.py
done
