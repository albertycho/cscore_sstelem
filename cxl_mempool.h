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
#include "eli_port_lists.h"
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
        { "link_bw_cycles", "CXL ingress bandwidth in cycles per 64B request (0 disables ingress bandwidth shaping)", "0" },
        { "link_latency_cycles", "CXL ingress base latency in cycles (0 disables ingress latency shaping; when both bw+lat are 0, ingress queue is bypassed)", "0" },
        { "link_queue_size", "CXL ingress queue capacity in bytes (0 = unbounded)", "0" },
        { "pool_node_id", "Logical node id used in fabric headers", "100" },
        { "heartbeat_period", "Cycles between CXL heartbeat dumps", "1000" },
        { "lightweight_output", "If set, emit stat.* pool summaries", "0" }
    )

#define CS_CXL_NODE_PORT_ENTRY(N) { "port_handler_nodes" #N, "Bidirectional CXL traffic", { "cscore.CXLMemoryPool", "" } },
    SST_ELI_DOCUMENT_PORTS(
        { "port_handler_switch", "Bidirectional CXL traffic (switch uplink)", { "cscore.CXLMemoryPool", "" } },
        FOR_EACH_INDEX_64(CS_CXL_NODE_PORT_ENTRY)
    )
#undef CS_CXL_NODE_PORT_ENTRY

private:
    struct OutstandingRequest {
        uint32_t cpu = 0;
        uint32_t sst_cpu = 0;
        uint32_t src_node = std::numeric_limits<uint32_t>::max();
        uint32_t dst_node = std::numeric_limits<uint32_t>::max();
        access_type type = access_type::LOAD;
        uint64_t enqueue_cycle = 0;
        uint64_t response_ready_cycle = std::numeric_limits<uint64_t>::max();
    };

    struct ResponseWaitStats {
        uint64_t attempted = 0;
        uint64_t completed = 0;
        uint64_t can_send_blocked = 0;
        uint64_t send_failed = 0;
        uint64_t wait_sum_cycles = 0;
        uint64_t wait_max_cycles = 0;
    };

    struct CycleBurstStats {
        uint64_t max_pkts = 0;
        uint64_t nonempty_cycles = 0;
        uint64_t sum_pkts = 0;
        long double sq_sum_pkts = 0.0L;
    };

    struct QueueOccStats {
        uint64_t samples = 0;
        uint64_t sum = 0;
        long double sq_sum = 0.0L;
        uint64_t max = 0;
    };

    bool clock_tick(SST::Cycle_t current);
    void enqueue_mem_request(const sst_request& request);
    // Aggregated ingress stats across active CXL ports.
    struct LinkStats {
        double util = 0.0;
        double avg_util = 0.0;
        double ingress_wait_avg_cycles = 0.0;
        double egress_wait_avg_cycles = 0.0;
        uint64_t ingress_wait_max_cycles = 0;
        uint64_t egress_wait_max_cycles = 0;
        double ingress_queue_wait_avg_cycles = 0.0;
        uint64_t ingress_queue_wait_max_cycles = 0;
        std::size_t occ = 0;
        double ready_wait_avg_cycles = 0.0;
        uint64_t ready_wait_max_cycles = 0;
        double ready_occ_avg = 0.0;
        std::size_t ready_occ = 0;
        std::size_t ready_occ_max = 0;
        uint64_t ready_retry_count = 0;
        uint64_t ingress_arrival_burst_max_pkts = 0;
        uint64_t ingress_arrival_burst_max_bytes = 0;
        double ingress_arrival_burst_avg_pkts = 0.0;
        double ingress_arrival_burst_avg_bytes = 0.0;
        double ingress_arrival_burst_stddev_pkts = 0.0;
        double ingress_arrival_burst_stddev_bytes = 0.0;
        double ingress_arrival_nonempty_frac = 0.0;
        uint64_t ingress_arrival_run_max_cycles = 0;
        double ingress_arrival_run_avg_cycles = 0.0;
        double ingress_arrival_run_stddev_cycles = 0.0;
        uint64_t ingress_arrival_gap_max_cycles = 0;
        double ingress_arrival_gap_avg_cycles = 0.0;
        double ingress_arrival_gap_stddev_cycles = 0.0;
        uint64_t ingress_release_burst_max_pkts = 0;
        uint64_t ingress_release_burst_max_bytes = 0;
        double ingress_release_burst_avg_pkts = 0.0;
        double ingress_release_burst_avg_bytes = 0.0;
        double ingress_release_burst_stddev_pkts = 0.0;
        double ingress_release_burst_stddev_bytes = 0.0;
        double ingress_release_nonempty_frac = 0.0;
        uint64_t ingress_release_run_max_cycles = 0;
        double ingress_release_run_avg_cycles = 0.0;
        double ingress_release_run_stddev_cycles = 0.0;
        uint64_t ingress_release_gap_max_cycles = 0;
        double ingress_release_gap_avg_cycles = 0.0;
        double ingress_release_gap_stddev_cycles = 0.0;
        double ingress_occ_avg_bytes = 0.0;
        double ingress_occ_stddev_bytes = 0.0;
        uint64_t ingress_occ_max_bytes = 0;
        double ingress_occ_nonempty_frac = 0.0;
        uint64_t ingress_occ_run_max_cycles = 0;
        double ingress_occ_run_avg_cycles = 0.0;
        double ingress_occ_run_stddev_cycles = 0.0;
        uint64_t ingress_occ_gap_max_cycles = 0;
        double ingress_occ_gap_avg_cycles = 0.0;
        double ingress_occ_gap_stddev_cycles = 0.0;
        std::array<uint64_t, static_cast<std::size_t>(FabricPort::TrafficClass::Count)> rx_bytes_by_class{};
        std::array<uint64_t, static_cast<std::size_t>(FabricPort::TrafficClass::Count)> tx_bytes_by_class{};
        std::array<uint64_t, static_cast<std::size_t>(FabricPort::TrafficClass::Count)> rx_packets_by_class{};
        std::array<uint64_t, static_cast<std::size_t>(FabricPort::TrafficClass::Count)> tx_packets_by_class{};
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
    struct DistinctCountStats {
        uint64_t nonempty_cycles = 0;
        uint64_t sum = 0;
        uint64_t max = 0;
    };
    struct SourceDiag {
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> request_enqueued_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> request_blocked_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> response_ready_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> response_sent_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> response_blocked_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> response_wait_sum_cycles_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> response_wait_samples_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> response_wait_max_cycles_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> total_turnaround_sum_cycles_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> total_turnaround_samples_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> total_turnaround_max_cycles_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> mem_ready_sum_cycles_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> mem_ready_samples_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> mem_ready_max_cycles_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> pending_occ_sum_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> pending_occ_max_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> ready_occ_sum_by_class{};
        std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount> ready_occ_max_by_class{};
        uint64_t pending_occ_sum = 0;
        uint64_t pending_occ_max = 0;
        uint64_t ready_occ_sum = 0;
        uint64_t ready_occ_max = 0;
        uint64_t pending_age_sum_cycles = 0;
        uint64_t pending_age_samples = 0;
        uint64_t pending_age_max_cycles = 0;
        uint64_t ready_age_sum_cycles = 0;
        uint64_t ready_age_samples = 0;
        uint64_t ready_age_max_cycles = 0;
    };
    FabricPort switch_port_;
    std::array<FabricPort, MAX_CXL_PORTS> core_ports_{};
    std::array<bool, MAX_CXL_PORTS> core_port_connected_{};
    std::vector<FabricPort*> active_ports_;
    bool use_switch_port_ = false;
    champsim::channel mem_channel_{};
    MY_MEMORY_CONTROLLER mem_ctrl_;
    uint64_t next_tag_ = 1;
    std::unordered_map<uint64_t, OutstandingRequest> pending_;
    std::array<ResponseWaitStats, MY_MEMORY_CONTROLLER::kDiagClassCount> response_wait_stats_{};
    CycleBurstStats returned_burst_stats_{};
    CycleBurstStats sent_burst_stats_{};
    uint64_t response_blocked_cycles_ = 0;
    uint64_t response_blocked_sum_pkts_ = 0;
    uint64_t response_blocked_max_pkts_ = 0;
    QueueOccStats returned_occ_stats_{};
    QueueOccStats pending_occ_stats_{};
    std::array<SourceDiag, MAX_CXL_PORTS> source_diag_{};
    DistinctCountStats request_accept_distinct_src_stats_{};
    DistinctCountStats request_blocked_distinct_src_stats_{};
    DistinctCountStats pending_distinct_src_stats_{};
    DistinctCountStats ready_distinct_src_stats_{};
    DistinctCountStats response_ready_distinct_src_stats_{};
    DistinctCountStats response_sent_distinct_dst_stats_{};
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
