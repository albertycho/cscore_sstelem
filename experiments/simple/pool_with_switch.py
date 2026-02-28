import os
import sst

# Simple wiring with a pass-through switch (no Fabric):
#   nodes -> switch -> pool

NUM_NODES = 4
NUM_POOLS = 1
POOL_NODE_ID_BASE = 100

# Latency/bandwidth (cycles per 64B) for the CXL links.
T_CXL = 120
BW_CXL_CYCLES = 25
REMOTE_LINK_QUEUE_SIZE = 8192

# Memory sizing
DRAM_SIZE_BYTES = 68719476736  # 64 GiB
POOL_PA_BASE = 68719476736     # pool PA starts at 64 GiB

# Local DRAM bandwidth as cycles per 64B request
DRAM_BW_CYCLES_PER_REQ = 4

# Inputs
TRACE_DIR = "/nethome/kshan9/scratch/src/sst-elements/src/sst/elements/cscore_sstelem/experiments/simple"
TRACE_NAME = "champsim.trace"
CXL_CONFIG = os.path.join(TRACE_DIR, "cxl_config.csv")

pool = sst.Component("cxl_pool", "cscore.CXLMemoryPool")
pool.addParams({
    "pool_node_id": POOL_NODE_ID_BASE,
    "clock": "2.4GHz",
    "pool_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
    "pool_latency_model": "utilization",
    "link_bw_cycles": BW_CXL_CYCLES,
    "link_latency_cycles": T_CXL,
    "link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    "heartbeat_period": 0,
})

switch = sst.Component("switch0", "cscore.Switch")
switch.addParams({
    "num_nodes": NUM_NODES,
    "num_pools": NUM_POOLS,
    "pool_node_id_base": POOL_NODE_ID_BASE,
    "replicate_writes": 0,
    # Bypass switch cost for parity with direct wiring.
    "link_bw_cycles": 0,
    "link_latency_cycles": 0,
    "link_queue_size": 0,
})

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": os.path.join(TRACE_DIR, TRACE_NAME),
        "address_map_config": CXL_CONFIG,
        "dram_size_bytes": DRAM_SIZE_BYTES,
        "dram_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
        "dram_latency_model": "percentile",
        "pool_pa_base": POOL_PA_BASE,
        "cache_heartbeat_period": 0,
        "clock": "2.4GHz",
        "warmup_insts": 10000,
        "sim_insts": 50000,
        "remote_link_bw_cycles": BW_CXL_CYCLES,
        "remote_link_latency_cycles": T_CXL,
        "remote_link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    })

    link_node_to_switch = sst.Link(f"s{i}_to_switch")
    link_node_to_switch.connect((sock, "port_handler_cxl", "0ns"),
                                (switch, f"port_handler_nodes{i}", "0ns"))

link_switch_to_pool = sst.Link("switch_to_pool")
link_switch_to_pool.connect((switch, "port_handler_pools0", "0ns"),
                            (pool, "port_handler_pool", "0ns"))
