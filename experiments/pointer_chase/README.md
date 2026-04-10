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
- `4,000` ROI instructions

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
- `LATENCY_THRESHOLD`: latency target for the quarter-window search, default `1000.0`
- `SEARCH_LEFT_FRAC`: initial left bound as a fraction of the target bandwidth, default `0.50`
- `SEARCH_RIGHT_FRAC`: initial right bound as a fraction of the target bandwidth, default `2.00`
- `SEARCH_SHRINK_FRAC`: fractional window shift after each sample, default `0.25`
- `MAX_SEARCH_ITERS`: maximum binary-search refinement steps, default `15`
- `MAX_GRAPH_BROADCAST_RETRIES`: retry count for transient SST graph-broadcast startup failures, default `2`

The injector bandwidth knob is request-direction bandwidth in Gbps. For each fixed `load_pct`, the runner:

1. computes the theoretical request-bandwidth ceiling for that load/store mix from the per-direction CXL link limit
2. samples `0.3 Gbps`, the initial left bound, and the initial right bound
3. runs the quarter-window search around the latency threshold

All `load_pct` lanes run in parallel, but bandwidth points within a lane run sequentially.

Before launching a new sweep, the runner clears prior generated logs and plot artifacts in `experiments/pointer_chase/logs` so the next plot reflects only the current run.
