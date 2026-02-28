import os
import sst

# StarNUMA-like parameters (4 sockets, 1 core per socket).
NUM_NODES = 4
POOL_NODE_ID = 100

# Latency/bandwidth targets from StarNUMA icn_sim (cycles per 64B)
T_CXL = 120          # cycles (matches icn_sim.t_cxl in StarNUMA config)
# Bandwidth here is cycles-per-packet (not MB/s). For 64B packets @ 2.4GHz:
# cycles = ceil(64 / (MBps * 1e6) * 2.4e9). 6000 MB/s -> 25 cycles.
BW_CXL_CYCLES = 25
REMOTE_LINK_QUEUE_SIZE = 8192  # matches StarNUMA DELAY_Q_SIZE

# Local DRAM bandwidth as cycles per 64B request.
# StarNUMA physical_memory: channels=2, channel_width=8 bytes => 16 B/cycle.
# cycles_per_req = ceil(64 / 16) = 4
DRAM_BW_CYCLES_PER_REQ = 4

# Memory sizing from virtual_memory/physical_memory
DRAM_SIZE_BYTES = 68719476736  # 64 GiB
POOL_PA_BASE = 68719476736     # pool PA starts at 64 GiB

# Absolute paths (avoid copying files)
TRACE_DIR = "/shared/kshan/StarNUMA_BFS_traces"
CXL_CONFIG = "/nethome/kshan9/scratch/src/sst-elements/src/sst/elements/cscore_sstelem/scripts/config.csv"

# Optional: stop after a fixed number of cycles (SST wall-clock cap).
# sst.setProgramOption("stopAtCycle", "1000000000")

pool = sst.Component("cxl_pool", "cscore.CXLMemoryPool")
pool.addParams({
    "pool_node_id": POOL_NODE_ID,
    "clock": "2.4GHz",
    "pool_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
    "pool_latency_model": "utilization",
    "link_bw_cycles": BW_CXL_CYCLES,
    "link_latency_cycles": T_CXL,
    "link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    # CXL pool heartbeat prints:
    "heartbeat_period": 100000,
})

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": os.path.join(TRACE_DIR, f"champsim_{i}_0.trace.xz"),
        "address_map_config": CXL_CONFIG,
        "dram_size_bytes": DRAM_SIZE_BYTES,
        "dram_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
        "dram_latency_model": "percentile",
        "pool_pa_base": POOL_PA_BASE,
        "cache_heartbeat_period": 100000,
        "clock": "2.4GHz",
        "warmup_insts": 5000,
        "sim_insts": 1000,
        "remote_link_bw_cycles": BW_CXL_CYCLES,
        "remote_link_latency_cycles": T_CXL,
        "remote_link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    })

    link_node_to_pool = sst.Link(f"s{i}_to_pool")
    link_node_to_pool.connect((sock, "port_handler_cxl", "0ns"),
                              (pool, f"port_handler_cores{i}", "0ns"))
