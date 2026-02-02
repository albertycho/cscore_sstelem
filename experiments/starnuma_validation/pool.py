import os
import sst

# StarNUMA-like parameters (from 4C_16SOCKET_config.json).
NUM_NODES = 16
POOL_NODE_ID = 100

# Latency/bandwidth targets from icn_sim
T_CXL = 100          # cycles
BW_CXL_MB = 6000     # MB/s

# Memory sizing from virtual_memory/physical_memory
DRAM_SIZE_BYTES = 68719476736  # 64 GiB
POOL_PA_BASE = 68719476736     # pool PA starts at 64 GiB

# Optional: stop after a fixed number of cycles (SST wall-clock cap).
# sst.setProgramOption("stopAtCycle", "1000000000")

pool = sst.Component("cxl_pool", "cscore.CXLMemoryPool")
pool.addParams({
    "pool_node_id": POOL_NODE_ID,
    "clock": "1GHz",
    # CXL pool heartbeat prints:
    "heartbeat_period": 100000,
})

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": os.path.join(os.getcwd(), f"champsim_{i}_0.trace.xz"),
        "address_map_config": os.path.join(os.getcwd(), "cxl_config.csv"),
        "dram_size_bytes": DRAM_SIZE_BYTES,
        "pool_pa_base": POOL_PA_BASE,
        # Cache heartbeat prints:
        "cache_heartbeat_period": 100000,
        "clock": "1GHz",
        "warmup_insts": 20000000,
        "sim_insts": 100000000,
    })

    fabric = sst.Component(f"fabric{i}", "cscore.Fabric")
    fabric.addParams({
        "link_bandwidth": BW_CXL_MB,
        "link_base_latency": T_CXL,
        "clock": "1GHz",
    })

    l0 = sst.Link(f"s{i}_to_fabric{i}")
    l0.connect((sock, "port_handler_FABRIC", "1ns"),
               (fabric, "port_handler0", "1ns"))

    l1 = sst.Link(f"fabric{i}_to_pool")
    l1.connect((fabric, "port_handler1", "1ns"),
               (pool, f"port_handler_cores{i}", "1ns"))
