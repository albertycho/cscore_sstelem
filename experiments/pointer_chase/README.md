# Pointer Chase Sweep

This experiment runs a single-node direct `node -> pool` topology with:
- a fixed pointer-chase trace
- direct injected traffic on the same node CXL link
- one sequential bandwidth sweep per injected load ratio (`load_pct`)

The pointer-chase trace is generated once per run:
- `10,000` total instructions
- all dependent loads
- CXL-address range only

The SST run uses:
- `1,000` warmup instructions
- `5,000` ROI instructions

Files:
- `pool_sweep.py`: SST config
- `run_sweep.py`: builds the pointer-chase generator, generates one trace, then runs one bandwidth lane per `load_pct`
- `cxl_config.csv`: direct pool mapping

Run:

```bash
python3 experiments/pointer_chase/run_sweep.py
```

Environment overrides:
- `TRACE_ROOT`: optional directory for the generated pointer-chase trace
- `MAX_PARALLEL`: max concurrent SST runs
- `BW_STEP_GBPS`: request-bandwidth increment per step, default `1.0`
- `LATENCY_JUMP_THRESHOLD`: stop a lane once `avg_load_issue_to_complete_lat` jumps by at least this many cycles, default `100.0`

The injector bandwidth knob is request-direction bandwidth in Gbps. For each fixed `load_pct`, the runner:

1. computes the theoretical request-bandwidth ceiling for that load/store mix from the per-direction CXL link limit
2. runs `inject_bandwidth_gbps = 1, 2, 3, ...` up to that ceiling
3. stops early once `stat.node.0.cpu.0.avg_load_issue_to_complete_lat` jumps by at least the configured threshold

All `load_pct` lanes run in parallel, but bandwidth points within a lane run sequentially.
