#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>
#include <string>
#include <array>
#include <unordered_map>
#include <chrono>

#include <sst/core/component.h>
#include <sst/core/link.h>

#include "channel.h"
#include "fabric_port.h"
#include "my_memory_controller.h"
#include "SST_CS_packets.h"
#include "timing_utility.h"


namespace SST {
namespace csimCore {

class CXLMemoryPool : public SST::Component {
public:
    CXLMemoryPool(SST::ComponentId_t id, SST::Params& params);
    void setup() override;
    void finish() override;

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
        { "pool_latency_model", "Pool memory latency model: fixed or utilization-based", "fixed" },
        { "latency_cycles", "Fixed pool memory latency in cycles (used when pool_latency_model=fixed)", "300" },
        { "link_bw_cycles", "CXL ingress bandwidth in cycles per 64B request", "0" },
        { "link_latency_cycles", "CXL ingress base latency in cycles", "0" },
        { "link_queue_size", "CXL ingress queue capacity in packets (used as byte credits; 0 = unbounded)", "0" },
        { "pool_node_id", "Logical node id used in fabric headers", "100" },
        { "heartbeat_period", "Cycles between CXL heartbeat dumps", "1000" },
        { "lightweight_output", "If set, suppress pool summary output", "0" }
    )

    SST_ELI_DOCUMENT_PORTS(
        { "port_handler_switch", "Bidirectional CXL traffic (switch uplink)", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes0", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes1", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes2", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes3", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes4", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes5", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes6", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes7", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes8", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes9", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes10", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes11", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes12", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes13", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes14", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes15", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes16", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes17", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes18", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes19", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes20", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes21", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes22", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes23", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes24", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes25", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes26", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes27", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes28", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes29", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes30", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes31", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes32", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes33", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes34", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes35", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes36", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes37", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes38", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes39", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes40", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes41", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes42", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes43", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes44", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes45", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes46", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes47", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes48", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes49", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes50", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes51", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes52", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes53", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes54", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes55", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes56", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes57", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes58", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes59", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes60", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes61", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes62", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
        { "port_handler_nodes63", "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } }
    )

private:
    struct OutstandingRequest {
        uint32_t cpu = 0;
        uint32_t sst_cpu = 0;
        uint32_t src_node = std::numeric_limits<uint32_t>::max();
        uint32_t dst_node = std::numeric_limits<uint32_t>::max();
    };

    bool clock_tick(SST::Cycle_t current);
    void enqueue_mem_request(const sst_request& request);
    // Aggregated ingress stats across active CXL ports.
    struct LinkStats {
        double util = 0.0;
        double avg_util = 0.0;
        std::size_t occ = 0;
    };
    void poll_ports(uint64_t cycle);
    // Average utilization plus total occupancy for request ingress links.
    LinkStats request_link_stats() const;
    void reset_stats();
    void for_each_port(const std::function<void(FabricPort&)>& fn);
    void for_each_port(const std::function<void(const FabricPort&)>& fn) const;
    FabricPort* select_egress_port(uint32_t sst_cpu);
    bool try_send_response(const champsim::channel::response_type& response);

    uint64_t device_bandwidth_;
    uint64_t memory_bandwidth_;
    uint64_t pool_bw_cycles_per_req_ = 0;
    int64_t latency_cycles_;
    int64_t link_bw_cycles_ = 0;
    int64_t link_latency_cycles_ = 0;
    int64_t link_queue_size_ = 0;
    uint32_t pool_node_id_ = 100;

    std::string clock_frequency_;
    static constexpr int MAX_CXL_PORTS = 64;
    FabricPort switch_port_;
    std::array<FabricPort, MAX_CXL_PORTS> core_ports_{};
    std::array<bool, MAX_CXL_PORTS> core_port_connected_{};
    std::vector<FabricPort*> active_ports_;
    bool use_switch_port_ = false;
    champsim::channel mem_channel_{};
    MY_MEMORY_CONTROLLER mem_ctrl_;
    uint64_t next_tag_ = 1;
    std::unordered_map<uint64_t, OutstandingRequest> pending_;
    uint64_t tick_count_ = 0;
    uint64_t total_enqueued_ = 0;
    uint64_t total_completed_ = 0;
    uint64_t heartbeat_period_ = 1000;
    bool lightweight_output_ = false;
    std::chrono::steady_clock::time_point wall_start_{};
    std::chrono::steady_clock::duration active_time_{};
    uint64_t active_calls_ = 0;
};

} // namespace csimCore
} // namespace SST
