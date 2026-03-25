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

Environment overrides:
- `MAX_PARALLEL`: max concurrent SST runs
- `TRACE_ROOT`: optional directory for generated traces

