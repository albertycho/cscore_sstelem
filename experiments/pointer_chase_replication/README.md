# Pointer-Chase Replication Sweep

This experiment runs the pointer-chase probe on `8` nodes behind one switch and compares:

- `no_rep`: `2` pools, `replicate_writes=0`
- `rep2`: `2` pools, `replicate_writes=1`

Each node runs the same dependent-load pointer-chase trace and injects additional background traffic through its shared CXL link.

## Run

```bash
python3 experiments/pointer_chase_replication/run_sweep.py
```

Useful overrides:

```bash
MPI_RANKS=8 MAX_PARALLEL=4 LATENCY_THRESHOLD=750 python3 experiments/pointer_chase_replication/run_sweep.py
```

## Method

For each `(config, load_pct)` lane, the runner:

1. computes the per-node request bandwidth that would drive the worst link direction to the full-capacity reference
2. samples `1.0 Gbps`, the initial left bound, and the initial right bound
3. runs the quarter-window search around the latency threshold

The runner uses:

- `load_pct` as a request-count mix
- `49.152 Gbps` as the per-direction link-capacity reference
- the sum of all node injector bandwidth stats as the observed aggregate x-axis
- the weighted average `avg_load_issue_to_complete_lat` across all `8` nodes as the y-axis

## Plot

```bash
python3 experiments/pointer_chase_replication/plot_bw_latency.py
```

Outputs:

- `experiments/pointer_chase_replication/logs/pointer_chase_replication_bw_latency.csv`
- `experiments/pointer_chase_replication/logs/pointer_chase_replication_bw_latency.png`
