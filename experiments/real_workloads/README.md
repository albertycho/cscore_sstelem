This directory runs minimal CoaXiaL-style experiments in the existing simulator: by default, eight ChampSim-trace-driven nodes run the same trace concurrently against pooled memory, with the full traced address space mapped to the pool.

Available topologies:

`pool.py` uses a direct `node -> pool` link.

`switch_pool.py` uses `node -> switch -> pool`.

`switch_replication.py` uses `node -> switch -> 2 pools` with replicated writes and round-robin load placement at the switch.

All three topologies default to `NUM_NODES=8`, `WARMUP_INSTS=5000000`, `SIM_INSTS=20000000`, and a CPU heartbeat every `1000000` cycles. `all_to_pool_node0.csv` maps the lower canonical user virtual-address range to pool node `100` for nodes `0-15`, so all traced pages are treated as remote pooled memory. The switch-based topologies reuse that same mapping; the switch selects among backend pools.

`run_traces.py` runs a batch of traces through this topology. By default it reads basenames from `high_mpki_traces.txt`, resolves them under `TRACE_ROOT`, and writes one `.out` and `.err` file per trace under `experiments/real_workloads/logs`.

Example:

```bash
export TRACE_ROOT=/scratch/kshan/coaxial_high_mpki/gz
python3 experiments/real_workloads/run_traces.py --skip-missing
```

To use one of the switch-based topologies:

```bash
python3 experiments/real_workloads/run_traces.py \
  --trace-root /scratch/kshan/coaxial_high_mpki/gz \
  --topology experiments/real_workloads/switch_pool.py \
  --skip-missing
```

```bash
python3 experiments/real_workloads/run_traces.py \
  --trace-root /scratch/kshan/coaxial_high_mpki/gz \
  --topology experiments/real_workloads/switch_replication.py \
  --skip-missing
```

You can also pass traces explicitly:

```bash
python3 experiments/real_workloads/run_traces.py \
  --trace-root /scratch/kshan/coaxial_high_mpki/gz \
  --trace 470.lbm-1274B.champsimtrace.gz \
  --trace ligra_BC.com-lj.ungraph.gcc_6.3.0_O3.drop_5000M.length_250M.champsimtrace.gz
```

To generate a CoaXiaL-style stacked latency-breakdown bar chart from the switch-based logs:

```bash
FIXED_ONCHIP_CYCLES=25 python3 experiments/real_workloads/plot_latency_breakdown.py
```

This writes `real_workload_latency_breakdown.{csv,png,pdf}` in this directory.
