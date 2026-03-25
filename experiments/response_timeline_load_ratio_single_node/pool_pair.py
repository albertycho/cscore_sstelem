import os
import sst

NUM_NODES = 1
NUM_POOLS = 1
POOL_NODE_ID_BASE = 100

T_CXL = 120
BW_CXL_CYCLES = 25
REMOTE_LINK_QUEUE_SIZE = 8192

DRAM_SIZE_BYTES = 68719476736
POOL_PA_BASE = 68719476736
DRAM_BW_CYCLES_PER_REQ = 4

TRACE_PATH = os.environ["TRACE_PATH"]
CXL_CONFIG_PATH = os.environ["CXL_CONFIG_PATH"]

LIGHTWEIGHT_OUTPUT = int(os.environ.get("LIGHTWEIGHT_OUTPUT", "1"))
PRINT_LAT_HIST = int(os.environ.get("PRINT_LAT_HIST", "0"))
WARM_CACHE_INSTS = 200_000
WARMUP_MAIN_INSTS = 100_000
WARMUP_INSTS = WARM_CACHE_INSTS + WARMUP_MAIN_INSTS
SIM_INSTS = 300_000

switch = sst.Component("switch0", "cscore.Switch")
switch.addParams({
    "num_nodes": NUM_NODES,
    "num_pools": NUM_POOLS,
    "pool_node_id_base": POOL_NODE_ID_BASE,
    "replicate_writes": 0,
    "pool_select_policy": "round_robin",
    "link_bw_cycles": BW_CXL_CYCLES,
    "link_latency_cycles": T_CXL,
    "link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    "lightweight_output": LIGHTWEIGHT_OUTPUT,
})

pool = sst.Component("cxl_pool0", "cscore.CXLMemoryPool")
pool.addParams({
    "pool_node_id": POOL_NODE_ID_BASE,
    "clock": "2.4GHz",
    "pool_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
    "pool_latency_model": "utilization-based",
    "link_bw_cycles": BW_CXL_CYCLES,
    "link_latency_cycles": T_CXL,
    "link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    "heartbeat_period": 0,
    "lightweight_output": LIGHTWEIGHT_OUTPUT,
})

sock = sst.Component("s0", "cscore.csimCore")
sock.addParams({
    "node_id": 0,
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

l0 = sst.Link("s0_to_switch")
l0.connect((sock, "port_handler_cxl", "1ns"),
           (switch, "port_handler_nodes0", "1ns"))

l1 = sst.Link("switch_to_pool0")
l1.connect((switch, "port_handler_pools0", "1ns"),
           (pool, "port_handler_switch", "1ns"))
