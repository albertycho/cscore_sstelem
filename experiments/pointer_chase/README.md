# Pointer Chase Sweep

This experiment runs a single-node direct `node -> pool` topology with:
- a fixed pointer-chase trace
- direct injected traffic on the same node CXL link
- sweeps over injected utilization (`mem_pct`) and injected load ratio (`load_pct`)

The pointer-chase trace is generated once per run:
- `10,000` total instructions
- all dependent loads
- CXL-address range only

The SST run uses:
- `1,000` warmup instructions
- `5,000` ROI instructions

Files:
- `pool_sweep.py`: SST config
- `run_sweep.py`: builds the pointer-chase generator, generates one trace, and runs the sweep
- `cxl_config.csv`: direct pool mapping

Run:

```bash
python3 experiments/pointer_chase/run_sweep.py
```

Environment overrides:
- `TRACE_ROOT`: optional directory for the generated pointer-chase trace
- `MAX_PARALLEL`: max concurrent SST runs

The injector bandwidth knob is request-direction bandwidth in Gbps and is set as:

```text
inject_bandwidth_gbps = (mem_pct / 100) * 12.0
```
