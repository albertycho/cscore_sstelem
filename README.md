# cscore_sstelem

SST element for a ChampSim-based node plus a CXL pool path (node -> switch -> pool, or direct node -> pool).

## What you need to run

1) A ChampSim trace (virtual addresses).
2) An AddressMap CSV (VA ranges -> local/pool).
3) An SST Python config wiring nodes, switches (optional), and the pool.

## VA/PA + CXL addressing

- **CXL routing is VA-based.** The AddressMap CSV segments VA space and marks which VA ranges go to the pool.
- **Translation produces the final PA.** For pool ranges, the PTW/VMEM maps VA directly into the pool PA window, so caches/MSHRs see a consistent PA from the start.
- **Pooling is explicit.** You decide which VA ranges map into the pool; they can be disjoint to model flexible placement.
- **Pool PA window is separate.** `pool_pa_base` defines where VMEM maps pool pages in PA space (default is `dram_size_bytes` to avoid overlap).

Mapping rule for pool ranges:
```
PA = pool_pa_base + pool_offset + (VA - range_start)
```
`pool_offset` is computed by sorting pool ranges and assigning a running sum, so disjoint VA ranges map to disjoint pool PA regions.

### AddressMap CSV

Format:
```csv
# node_id,start,size,type,target
```
Ranges must be **page‑granular**: `start` and `size` must be page‑aligned (the loader will error if not).

Example (map one 4 KiB VA page to pool 100):
```csv
0,0x5611a2255000,0x1000,pool,100
```

## Python config params

`cscore.csimCore`:
- `trace_name` (required)
- `node_id` (required)
- `address_map_config` (required for CXL routing)
- `dram_size_bytes` (default 1 GiB; must be > 1 MiB)
- `pool_pa_base` (default 0 -> set to `dram_size_bytes`)
- `dram_bw_cycles_per_req` (cycles per 64B request; optional)
- `dram_latency_model` (`fixed` or `utilization-based`)
- `cache_heartbeat_period` (0 disables)
- `remote_link_bw_cycles`, `remote_link_latency_cycles`, `remote_link_queue_size` (CXL link model)

`cscore.Switch`:
- `num_nodes` (required; must match number of node ports you connect)
- `num_pools` (required; must match number of pool ports you connect)
- `pool_node_id_base` (must be >= `num_nodes`)
- `replicate_writes` (0/1)
- `pool_select_policy` (`fixed0` or `round_robin`)
- `link_bw_cycles`, `link_latency_cycles`, `link_queue_size` (switch link model)

`cscore.CXLMemoryPool`:
- `pool_node_id` (must match AddressMap `target`)
- `pool_bw_cycles_per_req` (cycles per 64B request; optional)
- `pool_latency_model` (`fixed` or `utilization-based`)
- `latency_cycles` (fixed model)
- `link_bw_cycles`, `link_latency_cycles`, `link_queue_size` (pool ingress link model)
- `heartbeat_period` (0 disables)

## Minimal Python example (direct node -> pool)

```python
import os
import sst

NUM_NODES = 2
POOL_NODE_ID = 100

pool = sst.Component("cxl_pool", "cscore.CXLMemoryPool")
pool.addParams({
    "pool_node_id": POOL_NODE_ID,
    "pool_bw_cycles_per_req": 4,
    "pool_latency_model": "utilization-based",
    "link_bw_cycles": 25,
    "link_latency_cycles": 120,
    "link_queue_size": 8192,
    "heartbeat_period": 1000,
})

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": os.path.join(os.getcwd(), "champsim.trace"),
        "address_map_config": os.path.join(os.getcwd(), "address_map_example.csv"),
        "dram_size_bytes": 1073741824,
        "pool_pa_base": 1073741824,
        "dram_bw_cycles_per_req": 4,
        "dram_latency_model": "utilization-based",
        "cache_heartbeat_period": 10000,
        "remote_link_bw_cycles": 25,
        "remote_link_latency_cycles": 120,
        "remote_link_queue_size": 8192,
    })

    link_node_to_pool = sst.Link(f"s{i}_to_pool")
    link_node_to_pool.connect((sock, "port_handler_cxl", "1ns"),
                              (pool, f"port_handler_nodes{i}", "1ns"))
```

## Notes on topology validation
- `CXLMemoryPool` must be connected **either** via `port_handler_switch` **or** via one or more `port_handler_nodesX` ports (not both).
- `Switch` requires all configured node/pool ports to be connected.
Misconfigurations throw with errors at construction time.


## Running instructions
SST:
Build with 
```sh
make install
```
To run:
```sh
sst pool.py 1>run.out 2>run.err
```

StarNUMA:
Build with
```sh
python3 config.py CONFIGS/1C_4SOCKET.json
make -j
```

To run
```sh
./bin/1C_4S_3tb --warmup_instructions 5000 --simulation_instructions 1000 --tb_cpu_trace_path /shared/kshan/StarNUMA_BFS_traces --tb_cpu_forward 0 /shared/kshan/StarNUMA_BFS_traces/champsim_0_0.trace.xz > run.out
```
