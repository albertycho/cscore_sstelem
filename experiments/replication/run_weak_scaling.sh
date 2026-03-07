#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Weak-scaling sweep: keep per-node work fixed, scale nodes/ranks together.
RANKS_LIST=(1 2 4 8 16)

# Defaults (override via env before running):
#   NUM_POOLS=2 ./run_weak_scaling.sh
: "${NUM_POOLS:=2}"
: "${POOL_NODE_ID_BASE:=100}"

LOG_DIR="logs_weak_scaling"
mkdir -p "$LOG_DIR"

for r in "${RANKS_LIST[@]}"; do
  echo "=== Weak scaling: ranks=${r} nodes=${r} pools=${NUM_POOLS} ==="

  MPI_RANKS="$r" \
  NUM_NODES="$r" \
  NUM_POOLS="$NUM_POOLS" \
  POOL_NODE_ID_BASE="$POOL_NODE_ID_BASE" \
  mpirun -n "$r" --output-filename "${LOG_DIR}/run_${r}.out" sst pool_with_replication_mpi.py

done
