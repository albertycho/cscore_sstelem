# Pointer-Chase Replica Sweep

This experiment fixes the system at `8` nodes and runs the full pointer-chase
bandwidth-latency sweep across replica counts `1..8`. For each replica count,
the runner sweeps all configured load ratios and uses the same quarter-window
search used by the main replication experiment to concentrate samples around
the latency elbow.

Run:

```bash
python3 experiments/replica_count/run_replica_count.py
```

Useful overrides:

```bash
REPLICA_COUNTS=1,2,3,4,5,6,7,8 LOAD_PCTS=0,10,20,30,40,50,60,70,80,90,100 python3 experiments/replica_count/run_replica_count.py
```

Environment overrides:

- `NUM_NODES`: total nodes, default `8`
- `MPI_RANKS`: MPI ranks, default `NUM_NODES`
- `MAX_PARALLEL`: max concurrent `(replicas, load_pct)` lanes
- `LATENCY_THRESHOLD`: latency target for the quarter-window search, default `1000.0`
- `SEARCH_LEFT_FRAC`: initial left bound as a fraction of the theoretical target, default `0.50`
- `SEARCH_RIGHT_FRAC`: initial right bound as a fraction of the theoretical target, default `1.15`
- `SEARCH_SHRINK_FRAC`: fractional window shift after each sample, default `0.25`
- `MAX_SEARCH_ITERS`: maximum refinement steps, default `15`
- `MAX_GRAPH_BROADCAST_RETRIES`: retry count for transient SST graph-broadcast startup failures, default `2`

Plot:

```bash
python3 experiments/replica_count/plot_replica_count.py
```

Outputs:

- `experiments/replica_count/logs/replica_sweep_bw_latency.csv`
- `experiments/replica_count/logs/replica_sweep_bw_latency.png`
