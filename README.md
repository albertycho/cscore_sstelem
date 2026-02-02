# cscore_sstelem

SST element for a ChampSim-based node plus a simple CXL pool path (node -> fabric -> pool).

## What you need to run

1) A ChampSim trace (virtual addresses).
2) An AddressMap CSV (VA ranges -> local/pool).
3) An SST Python config wiring nodes, fabrics, and the pool.

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
- `cache_heartbeat_period` (0 disables)

`cscore.Fabric`:
- `link_bandwidth`
- `link_base_latency`

`cscore.CXLMemoryPool`:
- `pool_node_id` (must match AddressMap `target`)
- `device_bandwidth`, `memory_bandwidth`
- `latency_cycles`
- `heartbeat_period` (0 disables)

## Minimal Python example

```python
import os
import sst

NUM_NODES = 2
POOL_NODE_ID = 100

pool = sst.Component("cxl_pool", "cscore.CXLMemoryPool")
pool.addParams({"pool_node_id": POOL_NODE_ID, "heartbeat_period": 1000})

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": os.path.join(os.getcwd(), "champsim.trace"),
        "address_map_config": os.path.join(os.getcwd(), "address_map_example.csv"),
        "dram_size_bytes": 1073741824,
        "pool_pa_base": 1073741824,
        "cache_heartbeat_period": 10000,
    })

    fabric = sst.Component(f"fabric{i}", "cscore.Fabric")
    fabric.addParams({"link_bandwidth": 1, "link_base_latency": 1})

    l0 = sst.Link(f"s{i}_to_fabric{i}")
    l0.connect((sock, "port_handler_FABRIC", "1ns"),
               (fabric, "port_handler0", "1ns"))

    l1 = sst.Link(f"fabric{i}_to_pool")
    l1.connect((fabric, "port_handler1", "1ns"),
               (pool, f"port_handler_cores{i}", "1ns"))
```
