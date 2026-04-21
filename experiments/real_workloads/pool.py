import os
from pathlib import Path
import sst

# Topology: node -> pool (direct)
POOL_NODE_ID_BASE = 100

# Latency/bandwidth (cycles per 64B) for the CXL links.
T_CXL = 120
BW_CXL_CYCLES = 25
REMOTE_LINK_QUEUE_SIZE = 512

# Memory sizing
DRAM_SIZE_BYTES = 68719476736  # 64 GiB
POOL_PA_BASE = 68719476736     # pool PA starts at 64 GiB

# Local DRAM bandwidth as cycles per 64B request
DRAM_BW_CYCLES_PER_REQ = 4

TRACE_PATH = os.environ["TRACE_PATH"]
THIS_DIR = Path(__file__).resolve().parent
CXL_CONFIG_PATH = os.environ.get("CXL_CONFIG_PATH", str(THIS_DIR / "all_to_pool_node0.csv"))

LIGHTWEIGHT_OUTPUT = int(os.environ.get("LIGHTWEIGHT_OUTPUT", "1"))
PRINT_LAT_HIST = int(os.environ.get("PRINT_LAT_HIST", "1"))
WARMUP_INSTS = int(os.environ.get("WARMUP_INSTS", "50000000"))
SIM_INSTS = int(os.environ.get("SIM_INSTS", "0"))

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
    "num_nodes": 1,
    "trace_name": TRACE_PATH,
    "address_map_config": CXL_CONFIG_PATH,
    "dram_size_bytes": DRAM_SIZE_BYTES,
    "dram_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
    "dram_latency_model": "utilization-based",
    "pool_pa_base": POOL_PA_BASE,
    "cache_heartbeat_period": 0,
    "cpu_heartbeat_period": 100_000,
    "clock": "2.4GHz",
    "warmup_insts": WARMUP_INSTS,
    "warm_cache_insts": 0,
    "sim_insts": SIM_INSTS,
    "cxl_link_bw_cycles": BW_CXL_CYCLES,
    "cxl_link_latency_cycles": T_CXL,
    "cxl_link_queue_size": REMOTE_LINK_QUEUE_SIZE,
    "lightweight_output": LIGHTWEIGHT_OUTPUT,
    "print_latency_hist": PRINT_LAT_HIST,
    "inject_enable": 0,
})

link_node_to_pool = sst.Link("s0_to_pool0")
link_node_to_pool.connect(
    (sock, "port_handler_cxl", "1ns"),
    (pool, "port_handler_nodes0", "1ns"),
)
