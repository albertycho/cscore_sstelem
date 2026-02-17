#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>

#include <sst/core/component.h>
#include <sst/core/link.h>

#include "channel.h"
#include "my_memory_controller.h"
#include "SST_CS_packets.h"


namespace SST {
namespace csimCore {

class CXLMemoryPool : public SST::Component {
public:
    CXLMemoryPool(SST::ComponentId_t id, SST::Params& params);

    SST_ELI_REGISTER_COMPONENT(
        CXLMemoryPool,
        "cscore",
        "CXLMemoryPool",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "CXL Memory Pool Model",
        COMPONENT_CATEGORY_UNCATEGORIZED
    )

    SST_ELI_DOCUMENT_PARAMS(
        { "clock", "Clock frequency for the memory pool", "2.4GHz" },
        { "pool_bw_cycles_per_req", "Pool DRAM bandwidth in cycles per request (overrides memory/device bandwidth if nonzero)", "0" },
        { "device_bandwidth", "Device side bandwidth in bytes per cycle (converted to cycles/request using BLOCK_SIZE)", "0" },
        { "memory_bandwidth", "Memory side bandwidth in bytes per cycle (converted to cycles/request using BLOCK_SIZE)", "0" },
        { "latency_cycles", "Fixed latency (in cycles) for the CXL path", "0" },
        { "pool_node_id", "Logical node id used in fabric headers", "100" },
        { "heartbeat_period", "Cycles between CXL heartbeat dumps", "1000" }
    )

    SST_ELI_DOCUMENT_PORTS(
        { "port_handler_cores0", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores1", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores2", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores3", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores4", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores5", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores6", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores7", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores8", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores9", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores10", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores11", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores12", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores13", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores14", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores15", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores16", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores17", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores18", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores19", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores20", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores21", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores22", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores23", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores24", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores25", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores26", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores27", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores28", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores29", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores30", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores31", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores32", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores33", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores34", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores35", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores36", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores37", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores38", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores39", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores40", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores41", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores42", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores43", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores44", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores45", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores46", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores47", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores48", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores49", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores50", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores51", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores52", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores53", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores54", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores55", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores56", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores57", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores58", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores59", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores60", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores61", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores62", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_cores63", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } }
    )

private:
    struct OutstandingRequest {
        uint32_t cpu = 0;
        uint32_t sst_cpu = 0;
        uint32_t src_node = std::numeric_limits<uint32_t>::max();
        uint32_t dst_node = std::numeric_limits<uint32_t>::max();
    };

    bool clock_tick(SST::Cycle_t current);
    void handle_request(SST::Event* ev);
    void produce_placeholder_response(const champsim::channel::response_type& response);

    uint64_t device_bandwidth_;
    uint64_t memory_bandwidth_;
    uint64_t pool_bw_cycles_per_req_ = 0;
    int64_t latency_cycles_;
    uint32_t pool_node_id_ = 100;

    std::string clock_frequency_;
    static constexpr int MAX_CXL_PORTS = 64;
    SST::Link* core_links_[MAX_CXL_PORTS] = {};

    champsim::channel mem_channel_{};
    MY_MEMORY_CONTROLLER mem_ctrl_;
    uint64_t next_tag_ = 1;
    std::unordered_map<uint64_t, OutstandingRequest> pending_;
    uint64_t tick_count_ = 0;
    uint64_t total_enqueued_ = 0;
    uint64_t total_completed_ = 0;
    uint64_t heartbeat_period_ = 1000;
};

} // namespace csimCore
} // namespace SST
