import os
import sst

# Replication topology:
#   nodes -> switch -> pools

NUM_NODES = 4
NUM_POOLS = 1
POOL_NODE_ID_BASE = 100

# Latency/bandwidth (cycles per packet) for the CXL links.
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

# Switch policy
REPLICATE_WRITES = 1


def switch_node_port(node_idx: int) -> str:
    return f"port_handler_nodes{node_idx}"


def switch_pool_port(pool_idx: int) -> str:
    return f"port_handler_pools{pool_idx}"


switch = sst.Component("switch0", "cscore.Switch")
switch.addParams({
    "num_nodes": NUM_NODES,
    "num_pools": NUM_POOLS,
    "pool_node_id_base": POOL_NODE_ID_BASE,
    "replicate_writes": REPLICATE_WRITES,
    "link_bw_cycles": BW_CXL_CYCLES,
    "link_latency_cycles": T_CXL,
    "link_queue_size": REMOTE_LINK_QUEUE_SIZE,
})

pools = []
for j in range(NUM_POOLS):
    pool = sst.Component(f"cxl_pool{j}", "cscore.CXLMemoryPool")
    pool.addParams({
        "pool_node_id": POOL_NODE_ID_BASE + j,
        "clock": "2.4GHz",
        "pool_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
        "link_bw_cycles": BW_CXL_CYCLES,
        "link_latency_cycles": T_CXL,
        "link_queue_size": REMOTE_LINK_QUEUE_SIZE,
        "heartbeat_period": 0,
    })
    pools.append(pool)

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
        "remote_link_bw_cycles": BW_CXL_CYCLES,
        "remote_link_latency_cycles": T_CXL,
        "remote_link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    })

    l0 = sst.Link(f"s{i}_to_switch")
    l0.connect((sock, "port_handler_cxl", "0ns"),
               (switch, switch_node_port(i), "0ns"))

for j, pool in enumerate(pools):
    l1 = sst.Link(f"switch_to_pool{j}")
    l1.connect((switch, switch_pool_port(j), "0ns"),
               (pool, "port_handler_pool", "0ns"))
