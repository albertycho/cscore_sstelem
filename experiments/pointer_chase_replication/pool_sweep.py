import os
import sst

# Topology: 8 nodes -> switch -> {1,2} pools
NUM_NODES = 8
POOL_NODE_ID_BASE = 100
MPI_RANKS = int(os.environ.get("MPI_RANKS", "8"))
MPI_THREAD = 0

# Latency/bandwidth (cycles per 64B) for the CXL links.
T_CXL = 120
BW_CXL_CYCLES = 25
REMOTE_LINK_QUEUE_SIZE = 512

# Memory sizing
DRAM_SIZE_BYTES = 68719476736  # 64 GiB
POOL_PA_BASE = 68719476736     # pool PA starts at 64 GiB

# Local DRAM bandwidth as cycles per 64B request
DRAM_BW_CYCLES_PER_REQ = 4

# Inputs
TRACE_PATH = os.environ["TRACE_PATH"]
CXL_CONFIG_PATH = os.environ["CXL_CONFIG_PATH"]
INJECT_BANDWIDTH_GBPS = os.environ["INJECT_BANDWIDTH_GBPS"]
INJECT_LOAD_PCT = os.environ["INJECT_LOAD_PCT"]
REPLICATE_WRITES = int(os.environ["REPLICATE_WRITES"])
NUM_POOLS = 2 if REPLICATE_WRITES else 1
MAX_AVG_LOAD_ISSUE_TO_COMPLETE_LAT = int(os.environ.get("MAX_AVG_LOAD_ISSUE_TO_COMPLETE_LAT", "0"))
MIN_RETIRED_BEFORE_LATENCY_CUTOFF = int(os.environ.get("MIN_RETIRED_BEFORE_LATENCY_CUTOFF", "100"))

# Output and run length
LIGHTWEIGHT_OUTPUT = 1
PRINT_LAT_HIST = 1
WARMUP_INSTS = 500
SIM_INSTS = 1500

sst.setProgramOption("partitioner", "sst.self")

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
switch.setRank(max(MPI_RANKS - 1, 0), MPI_THREAD)

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
    pool.setRank(max(MPI_RANKS - 1, 0), MPI_THREAD)
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
        "cpu_heartbeat_period": 1_000_000,
        "clock": "2.4GHz",
        "warmup_insts": WARMUP_INSTS,
        "warm_cache_insts": 0,
        "sim_insts": SIM_INSTS,
        "cxl_link_bw_cycles": BW_CXL_CYCLES,
        "cxl_link_latency_cycles": T_CXL,
        "cxl_link_queue_size": REMOTE_LINK_QUEUE_SIZE,
        "lightweight_output": LIGHTWEIGHT_OUTPUT,
        "print_latency_hist": PRINT_LAT_HIST,
        "inject_enable": 1,
        "inject_bandwidth_gbps": INJECT_BANDWIDTH_GBPS,
        "inject_load_pct": INJECT_LOAD_PCT,
        "max_avg_load_issue_to_complete_lat": MAX_AVG_LOAD_ISSUE_TO_COMPLETE_LAT,
        "min_retired_before_latency_cutoff": MIN_RETIRED_BEFORE_LATENCY_CUTOFF,
    })
    sock.setRank(i % max(MPI_RANKS, 1), MPI_THREAD)

    node_link = sst.Link(f"s{i}_to_switch")
    node_link.connect(
        (sock, "port_handler_cxl", "1ns"),
        (switch, f"port_handler_nodes{i}", "1ns"),
    )

for j, pool in enumerate(pools):
    pool_link = sst.Link(f"switch_to_pool{j}")
    pool_link.connect(
        (switch, f"port_handler_pools{j}", "1ns"),
        (pool, "port_handler_switch", "1ns"),
    )
