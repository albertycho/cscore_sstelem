
# Automatically generated SST Python input
import sst
import csv
import os

# Define SST core options
sst.setProgramOption("timebase", "250 ps")
sst.setProgramOption("stop-at", "100000s")
#sst.setProgramOption("sync", "optimistic")
#sst.setProgramOption("numRanks", "17")

sst.setStatisticLoadLevel(4)
sst.enableAllStatisticsForAllComponents({
    "type": "sst.AccumulatorStatistic",
    "rate": "1ns"   # <--- This only works with perf tracking enabled
})

#######################################
#### Define Config Params and Vars ####
#######################################
NUM_NODES = 2
CORES_PER_NODE=1

TRACE_PATH = "/scratch/acho44/DIST_CXL/TRACES/SPMV/BASELINE/250411_cage/"
OUTPUT_PATH = os.getcwd()
TRACE_SUFFIX = ".trace.xz"
OUTPUT_SUFFIX = "_2us.out"

warmupinsts = 0

billion_ins = 1000000000
siminsts = 4*billion_ins


# Define the simulation components

##########################
#### Processing Nodes ####
##########################
cscores = []
for i in range(NUM_NODES):
    cscores.append(sst.Component(f"csn{i}", "cscore.csimCore"))

# Add parameters to each node
for i in range(NUM_NODES):
    print(f"{TRACE_PATH}/node_{i}{TRACE_SUFFIX}")
    cscores[i].addParams(
        {
            #"trace_name": f"{TRACE_PATH}/node{ii}{TRACE_SUFFIX}",
            "trace_name": f"{TRACE_PATH}/node_{i}{TRACE_SUFFIX}",
            #"trace_name": f"{TRACE_PATH}/node_5{TRACE_SUFFIX}",
            "output_file": f"{OUTPUT_PATH}/node{i}{OUTPUT_SUFFIX}",
            "node_id": i,
            "warmup_insts": warmupinsts,
            "sim_insts": siminsts
        }
    )
