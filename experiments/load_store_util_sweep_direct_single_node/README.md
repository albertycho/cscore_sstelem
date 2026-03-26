# Direct Single-Node Load/Store Sweep

This experiment is the direct-link version of `experiments/load_store_util_sweep_single_node`.

Topology:
- one `csimCore` node
- directly connected to one `CXLMemoryPool`
- no switch component

Files:
- `pool_sweep.py`: SST config for the direct node-to-pool topology
- `run_sweep.py`: generates traces and runs the full load/mem sweep
- `cxl_config.csv`: address-map config written/overwritten by the runner

Run:

```bash
python3 experiments/load_store_util_sweep_direct_single_node/run_sweep.py
```

Plot the bandwidth/latency curves from completed logs:

```bash
python3 experiments/load_store_util_sweep_direct_single_node/plot_bw_latency_curves.py
```

Outputs:
- `logs/bw_latency_summary.csv`
- `logs/bw_latency_curves.png`
- `logs/bw_latency_curves_bottleneck_bw.png`
- `logs/bw_latency_vs_loadpct.png`
- `logs/bw_latency_vs_mempct.png`

Environment overrides:
- `MAX_PARALLEL`: max concurrent SST runs
- `TRACE_ROOT`: optional directory for generated traces
- `COMPLETE_STORES_AFTER_ISSUE`: `0` keeps the original core behavior; `1` delays store instruction completion until `issue_write()` succeeds
- `L1D_MSHR_SIZE_OVERRIDE`: override for `cpu0_L1D.mshr_size` in the direct experiment

Example A/B runs:

```bash
python3 experiments/load_store_util_sweep_direct_single_node/run_sweep.py
COMPLETE_STORES_AFTER_ISSUE=1 python3 experiments/load_store_util_sweep_direct_single_node/run_sweep.py
L1D_MSHR_SIZE_OVERRIDE=64 python3 experiments/load_store_util_sweep_direct_single_node/run_sweep.py
```
