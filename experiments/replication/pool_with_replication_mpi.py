import os
import sst

# Replication topology (MPI-friendly):
#   nodes -> switch -> pools
#
# Rank strategy:
#   - Cores are the heaviest components.
#   - Place each core on its own rank when possible.
#   - Pack switch + all pools on the last rank to minimize interference.

NUM_NODES = 16
NUM_POOLS = 2
POOL_NODE_ID_BASE = 100

# MPI configuration (must match mpirun -n). Override via env:
#   MPI_RANKS=<n> mpirun -n <n> sst pool_with_replication_mpi.py
MPI_RANKS = int(os.environ.get("MPI_RANKS", "4"))
MPI_THREAD = 0

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
TRACE_DIR = "/nethome/kshan9/scratch/src/sst-elements/src/sst/elements/cscore_sstelem/experiments/replication"
TRACE_NAME = "champsim.trace"
CXL_CONFIG = os.path.join(TRACE_DIR, "cxl_config.csv")

# Switch policy
REPLICATE_WRITES = 1


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
    "lightweight_output": 1,
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
        "lightweight_output": 1,
    })
    pool.setRank(max(MPI_RANKS - 1, 0), MPI_THREAD)
    pools.append(pool)

for i in range(NUM_NODES):
    sock = sst.Component(f"s{i}", "cscore.csimCore")
    sock.addParams({
        "node_id": i,
        "trace_name": os.path.join(TRACE_DIR, TRACE_NAME),
        "address_map_config": CXL_CONFIG,
        "dram_size_bytes": DRAM_SIZE_BYTES,
        "dram_bw_cycles_per_req": DRAM_BW_CYCLES_PER_REQ,
        "dram_latency_model": "utilization-based",
        "pool_pa_base": POOL_PA_BASE,
        "cache_heartbeat_period": 0,
        "clock": "2.4GHz",
        "warmup_insts": 2000000,
        "sim_insts": 5000000,
        "cxl_link_bw_cycles": BW_CXL_CYCLES,
        "cxl_link_latency_cycles": T_CXL,
        "cxl_link_queue_size": REMOTE_LINK_QUEUE_SIZE,
        "lightweight_output": 1,
    })
    sock.setRank(i % max(MPI_RANKS, 1), MPI_THREAD)

    l0 = sst.Link(f"s{i}_to_switch")
    l0.connect((sock, "port_handler_cxl", "1ns"),
               (switch, f"port_handler_nodes{i}", "1ns"))

for j, pool in enumerate(pools):
    l1 = sst.Link(f"switch_to_pool{j}")
    l1.connect((switch, f"port_handler_pools{j}", "1ns"),
               (pool, "port_handler_switch", "1ns"))
