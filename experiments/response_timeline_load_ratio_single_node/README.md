This experiment isolates the load-ratio inversion at fixed aggregate bandwidth and emits per-response timeline CSVs.

It reuses the same single-node, single-pool topology and timeline stages as
`experiments/response_timeline_single_node`, but the driver sweeps `load_pct`
at one fixed `mem_pct` target.

Files:
- `pool_pair.py`: single-node, single-pool SST topology for the targeted runs
- `run_cases.py`: builds the synthetic trace generator, generates one trace per load ratio, runs SST, and writes timeline CSVs under `logs/`
- `analyze_timeline.py`: prints stage-to-stage average deltas and per-stage gap summaries from one or more timeline CSVs

Default cases:
- `mem_pct = 100`
- `load_pcts = [0, 12, 40, 100]`

Environment overrides:
- `MEM_PCT`: fixed aggregate-bandwidth target percent for all runs
- `LOAD_PCTS`: comma-separated load percentages, for example `0,12,25,40,60,100`
- `AGGREGATE_PEAK_GBPS`: aggregate host-link peak used by the generator
- `SST_BIN`: SST executable name/path

Typical use:

```bash
python3 experiments/response_timeline_load_ratio_single_node/run_cases.py
python3 experiments/response_timeline_load_ratio_single_node/analyze_timeline.py \
  experiments/response_timeline_load_ratio_single_node/logs/load000_mem100.timeline.csv \
  experiments/response_timeline_load_ratio_single_node/logs/load012_mem100.timeline.csv \
  experiments/response_timeline_load_ratio_single_node/logs/load040_mem100.timeline.csv \
  experiments/response_timeline_load_ratio_single_node/logs/load100_mem100.timeline.csv
```
