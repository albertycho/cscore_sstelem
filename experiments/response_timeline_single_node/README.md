This experiment isolates `load40 mem60` and `load40 mem100` and emits a per-response timeline CSV.

Files:
- `pool_pair.py`: single-node, single-pool SST topology for the targeted pair run
- `run_pair.py`: builds the synthetic trace generator, generates the two traces, runs SST twice, and writes timeline CSVs under `logs/`
- `analyze_timeline.py`: prints stage-to-stage average deltas from one or more timeline CSVs

The timeline is gated by the `CSCORE_RESPONSE_TIMELINE_PATH` env var and logs:
- `node.request_issue`
- `pool.request_accept`
- `pool.response_ready`
- `pool.response_send`
- `switch.port.pool.0:fabric.rx_enqueue`
- `switch.port.pool.0:fabric.ingress_release`
- `switch.port.node.0:fabric.tx_send`
- `node.0.remote_port:fabric.rx_enqueue`
- `node.0.remote_port:fabric.ingress_release`
- `node.response_receive`

Typical use:

```bash
python3 experiments/response_timeline_single_node/run_pair.py
python3 experiments/response_timeline_single_node/analyze_timeline.py \
  experiments/response_timeline_single_node/logs/load040_mem060.timeline.csv \
  experiments/response_timeline_single_node/logs/load040_mem100.timeline.csv
```
