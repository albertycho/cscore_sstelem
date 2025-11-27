#pragma once

#include <cstdint>
#include <cstdint>
#include <deque>
#include <fstream>
#include <string>

#include <sst/core/component.h>
#include <sst/core/link.h>

#include "SST_CS_packets.h"
#include "physical_channel.h"


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
        { "clock", "Clock frequency for the memory pool", "1GHz" },
        { "device_bandwidth", "Device side bandwidth in bytes per cycle", "0" },
        { "memory_bandwidth", "Memory side bandwidth in bytes per cycle", "0" },
        { "latency_cycles", "Fixed latency (in cycles) for the CXL path", "0" },
        { "heartbeat_file", "Optional path to append CXL stats", "" }
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
    using ResponseQueue = std::deque<sst_response>;

    bool clock_tick(SST::Cycle_t current);
    void handle_request(SST::Event* ev);
    void produce_placeholder_response(const sst_request& request);

    uint64_t device_bandwidth_;
    uint64_t memory_bandwidth_;
    int64_t latency_cycles_;

    std::string clock_frequency_;
    static constexpr int MAX_CXL_PORTS = 64;
    SST::Link* core_links_[MAX_CXL_PORTS] = {};

    ResponseQueue responses_;
    physical_channel<sst_request> requests_;

    uint64_t tick_count_ = 0;
    uint64_t total_enqueued_ = 0;
    uint64_t total_completed_ = 0;
    std::ofstream stats_file_;
};

} // namespace csimCore
} // namespace SST
