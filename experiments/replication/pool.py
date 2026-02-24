import os
import sst

# Replication topology (with switch):
#   nodes -> fabric -> switch -> fabric -> pools
#
# For testing parity with experiments/simple/pool.py, keep NUM_NODES and NUM_POOLS
# identical across both scripts.

NUM_NODES = 4
NUM_POOLS = 1
POOL_NODE_ID_BASE = 100

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


def switch_node_port(node_idx: int) -> str:
    return f"port_handler_nodes{node_idx}"


def switch_pool_port(pool_idx: int) -> str:
    return f"port_handler_pools{pool_idx}"


switch = sst.Component("switch0", "cscore.Switch")
switch.addParams({
    "num_nodes": NUM_NODES,
    "num_pools": NUM_POOLS,
    "pool_node_id_base": POOL_NODE_ID_BASE,
})

pools = []
for j in range(NUM_POOLS):
    pool = sst.Component(f"cxl_pool{j}", "cscore.CXLMemoryPool")
    pool.addParams({
        "pool_node_id": POOL_NODE_ID_BASE + j,
        "clock": "2.4GHz",
        "pool_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
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
        "warmup_insts": 1000000,
        "sim_insts": 5000000,
    })

    node_fabric = sst.Component(f"fabric_node{i}", "cscore.Fabric")
    node_fabric.addParams({
        "link_bandwidth": BW_CXL_CYCLES,
        "link_base_latency": T_CXL,
        "clock": "2.4GHz",
    })

    link_node_to_fabric = sst.Link(f"link_node{i}_to_fabric")
    link_node_to_fabric.connect((sock, "port_handler_FABRIC", "1ns"),
                                (node_fabric, "port_handler0", "1ns"))

    link_fabric_to_switch = sst.Link(f"link_fabric_node{i}_to_switch")
    link_fabric_to_switch.connect((node_fabric, "port_handler1", "1ns"),
                                  (switch, switch_node_port(i), "1ns"))

for j, pool in enumerate(pools):
    pool_fabric = sst.Component(f"fabric_pool{j}", "cscore.Fabric")
    pool_fabric.addParams({
        "link_bandwidth": BW_CXL_CYCLES,
        "link_base_latency": T_CXL,
        "clock": "2.4GHz",
    })

    link_switch_to_fabric = sst.Link(f"link_switch_to_fabric_pool{j}")
    link_switch_to_fabric.connect((switch, switch_pool_port(j), "1ns"),
                                  (pool_fabric, "port_handler0", "1ns"))

    link_fabric_to_pool = sst.Link(f"link_fabric_pool{j}_to_pool")
    link_fabric_to_pool.connect((pool_fabric, "port_handler1", "1ns"),
                                (pool, "port_handler_pool", "1ns"))
