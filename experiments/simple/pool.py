import os
import sst

# Simple wiring: node -> fabric -> pool (one fabric per node).
# The pool port index must match the node_id (sst_cpu) used in responses.

NUM_NODES = 4
POOL_NODE_ID = 100

# Latency/bandwidth (cycles per packet) for the CXL links.
T_CXL = 120
BW_CXL_CYCLES = 25

# Memory sizing
DRAM_SIZE_BYTES = 68719476736  # 64 GiB
POOL_PA_BASE = 68719476736     # pool PA starts at 64 GiB

# Local DRAM bandwidth as cycles per 64B request
DRAM_BW_CYCLES_PER_REQ = 4

# Inputs
TRACE_DIR = os.getcwd()
TRACE_NAME = "champsim.trace"
CXL_CONFIG = os.path.join(os.getcwd(), "cxl_config.csv")

pool = sst.Component("cxl_pool", "cscore.CXLMemoryPool")
pool.addParams({
    "pool_node_id": POOL_NODE_ID,
    "clock": "2.4GHz",
    "pool_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
    "heartbeat_period": 0,
})

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": os.path.join(TRACE_DIR, TRACE_NAME),
        "address_map_config": CXL_CONFIG,
        "dram_size_bytes": DRAM_SIZE_BYTES,
        "dram_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
        "pool_pa_base": POOL_PA_BASE,
        "cache_heartbeat_period": 0,
        "clock": "2.4GHz",
        "warmup_insts": 10000,
        "sim_insts": 50000,
    })

    fabric = sst.Component(f"fabric{i}", "cscore.Fabric")
    fabric.addParams({
        "link_bandwidth": BW_CXL_CYCLES,
        "link_base_latency": T_CXL,
        "clock": "2.4GHz",
    })

    l0 = sst.Link(f"s{i}_to_fabric{i}")
    l0.connect((sock, "port_handler_FABRIC", "1ns"),
               (fabric, "port_handler0", "1ns"))

    l1 = sst.Link(f"fabric{i}_to_pool")
    l1.connect((fabric, "port_handler1", "1ns"),
               (pool, f"port_handler_cores{i}", "1ns"))
