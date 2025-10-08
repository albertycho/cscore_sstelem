
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
#nw_lat = "500ns"
nw_lat = "1us"
#nw_lat = "50ns"
cxl_lat_overhead = "50ns"

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

#########################
#### NW and CXL Pool ####
#########################
nwsim = sst.Component("nwsim", "cscore.NWsim")
nwsim.addParams(
    {"output_file": OUTPUT_PATH + "/NW.out"}
)


# Initialize nwls list and connect the links
nwls = []
for i in range(NUM_NODES):
    for j in range(CORES_PER_NODE):
        k = (i*CORES_PER_NODE)+j
        print(f"nwl for node {k}")
        nwls.append(sst.Link(f"node{k}_nw_link"))
        nwls[k].connect(
            (cscores[k], "port_handler_NW", nw_lat),
            (nwsim, f"port_handler{k}", nw_lat)
        )


