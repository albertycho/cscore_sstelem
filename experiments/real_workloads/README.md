This directory runs a minimal CoaXiaL-style experiment in the existing simulator: one ChampSim-trace-driven node connected directly to one pooled-memory device, with the full traced address space mapped to the pool.

`pool.py` is the SST topology. It defaults to `WARMUP_INSTS=5000000`, `SIM_INSTS=20000000`, and a CPU heartbeat every `100000` cycles. `all_to_pool_node0.csv` maps the lower canonical user virtual-address range to pool node `100`, so all traced pages are treated as remote pooled memory.

`run_traces.py` runs a batch of traces through this topology. By default it reads basenames from `high_mpki_traces.txt`, resolves them under `TRACE_ROOT`, and writes one `.out` and `.err` file per trace under `experiments/real_workloads/logs`.

Example:

```bash
export TRACE_ROOT=/scratch/kshan/coaxial_high_mpki/gz
python3 experiments/real_workloads/run_traces.py --skip-missing
```

You can also pass traces explicitly:

```bash
python3 experiments/real_workloads/run_traces.py \
  --trace-root /scratch/kshan/coaxial_high_mpki/gz \
  --trace 470.lbm-1274B.champsimtrace.gz \
  --trace ligra_BC.com-lj.ungraph.gcc_6.3.0_O3.drop_5000M.length_250M.champsimtrace.gz
```
