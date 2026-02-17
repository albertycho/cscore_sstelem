import os
import sst

# Simple wiring: node -> fabric -> pool (one fabric per node).
# The pool port index must match the node_id (sst_cpu) used in responses.

NUM_NODES = 4
POOL_NODE_ID = 100

pool = sst.Component("cxl_pool", "cscore.CXLMemoryPool")
pool.addParams({
    "pool_node_id": POOL_NODE_ID,
    "heartbeat_period": 0,
})

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": os.path.join(os.getcwd(), "champsim.trace"),
        "address_map_config": os.path.join(os.getcwd(), "cxl_config.csv"),
        "dram_size_bytes": 1073741824,  # 1 GiB
        "pool_pa_base": 1073741824,     # pool PA starts at 1 GiB
        "cache_heartbeat_period":1000,
    })

    fabric = sst.Component(f"fabric{i}", "cscore.Fabric")
    fabric.addParams({
        "link_bandwidth": 1,
        "link_base_latency": 1,
    })

    l0 = sst.Link(f"s{i}_to_fabric{i}")
    l0.connect((sock, "port_handler_FABRIC", "1ns"),
               (fabric, "port_handler0", "1ns"))

    l1 = sst.Link(f"fabric{i}_to_pool")
    l1.connect((fabric, "port_handler1", "1ns"),
               (pool, f"port_handler_cores{i}", "1ns"))
