import os
import sst

# Topology: nodes -> switch -> pools
NUM_NODES = 8
NUM_POOLS = 2
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

# Inputs (must be set by the sweep runner)
TRACE_PATH = os.environ["TRACE_PATH"]
CXL_CONFIG_PATH = os.environ["CXL_CONFIG_PATH"]

# Output
LIGHTWEIGHT_OUTPUT = 1
PRINT_LAT_HIST = 1
WARM_CACHE_INSTS = 200_000
WARMUP_MAIN_INSTS = 100_000
WARMUP_INSTS = WARM_CACHE_INSTS + WARMUP_MAIN_INSTS
SIM_INSTS = 2_000_000

# Switch policy (replication enabled)
REPLICATE_WRITES = 1


switch = sst.Component("switch0", "cscore.Switch")
switch.addParams({
    "num_nodes": NUM_NODES,
    "num_pools": NUM_POOLS,
    "pool_node_id_base": POOL_NODE_ID_BASE,
    "replicate_writes": REPLICATE_WRITES,
    "pool_select_policy": "round_robin",
    "link_bw_cycles": BW_CXL_CYCLES,
    "link_latency_cycles": T_CXL,
    "link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    "lightweight_output": LIGHTWEIGHT_OUTPUT,
})

pools = []
for j in range(NUM_POOLS):
    pool = sst.Component(f"cxl_pool{j}", "cscore.CXLMemoryPool")
    pool.addParams({
        "pool_node_id": POOL_NODE_ID_BASE + j,
        "clock": "2.4GHz",
        "pool_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
        "pool_latency_model": "utilization-based",
        "link_bw_cycles": BW_CXL_CYCLES,
        "link_latency_cycles": T_CXL,
        "link_queue_size": REMOTE_LINK_QUEUE_SIZE,
        "heartbeat_period": 0,
        "lightweight_output": LIGHTWEIGHT_OUTPUT,
    })
    pools.append(pool)

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": TRACE_PATH,
        "address_map_config": CXL_CONFIG_PATH,
        "dram_size_bytes": DRAM_SIZE_BYTES,
        "dram_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
        "dram_latency_model": "utilization-based",
        "pool_pa_base": POOL_PA_BASE,
        "cache_heartbeat_period": 0,
        "cpu_heartbeat_period": 0,
        "clock": "2.4GHz",
        "warmup_insts": WARMUP_INSTS,
        "warm_cache_insts": WARM_CACHE_INSTS,
        "sim_insts": SIM_INSTS,
        "cxl_link_bw_cycles": BW_CXL_CYCLES,
        "cxl_link_latency_cycles": T_CXL,
        "cxl_link_queue_size": REMOTE_LINK_QUEUE_SIZE,
        "lightweight_output": LIGHTWEIGHT_OUTPUT,
        "print_latency_hist": PRINT_LAT_HIST,
    })

    l0 = sst.Link(f"s{i}_to_switch")
    l0.connect((sock, "port_handler_cxl", "1ns"),
               (switch, f"port_handler_nodes{i}", "1ns"))

for j, pool in enumerate(pools):
    l1 = sst.Link(f"switch_to_pool{j}")
    l1.connect((switch, f"port_handler_pools{j}", "1ns"),
               (pool, "port_handler_switch", "1ns"))
