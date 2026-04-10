# Pointer-Chase Replica Count

This experiment reruns the replica-count study using the current pointer-chase
methodology:

- pointer-chase probe trace
- background injector traffic
- `8` nodes behind one switch
- one `no_rep` baseline with `1` pool
- replicated runs with `repN` using `N` pools

Run:

```bash
python3 experiments/replica_count/run_replica_count.py
```

Useful overrides:

```bash
REPLICA_COUNTS=2,4,8 LOAD_PCT=80 INJECT_BANDWIDTH_GBPS=1.50 python3 experiments/replica_count/run_replica_count.py
```

Plot:

```bash
python3 experiments/replica_count/plot_replica_count.py
```

Outputs:

- `experiments/replica_count/logs/replica_count_summary.csv`
- `experiments/replica_count/logs/replica_count_latency_vs_count.png`
- `experiments/replica_count/logs/replica_count_latency_cdf_overlay.png`
