#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <chrono>

#include <sst/core/component.h>
#include <sst/core/link.h>

#include "csEvent.h"
#include "eli_port_lists.h"
#include "fabric_port.h"
#include "timing_utility.h"

namespace SST {
namespace csimCore {

/**
 * Switch is a multi-port router.
 * For now, all pool-bound traffic is forwarded to pool 0.
 */
class Switch : public SST::Component {
public:
    Switch(SST::ComponentId_t id, SST::Params& params);
    void setup() override;

    SST_ELI_REGISTER_COMPONENT(
        Switch,
        "cscore",
        "Switch",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Pass-through switch for message forwarding",
        COMPONENT_CATEGORY_UNCATEGORIZED
    )

    SST_ELI_DOCUMENT_PARAMS(
        { "num_nodes", "Number of node-facing ports", "0" },
        { "num_pools", "Number of pool-facing ports", "0" },
        { "pool_node_id_base", "Base node_id for pools (contiguous)", "100" },
        { "replicate_writes", "If set, send WRITE requests to all pools", "0" },
        { "pool_select_policy", "Pool selection policy for reads/non-replicated requests (round_robin|fixed0)", "round_robin" },
        { "clock", "Clock frequency for queue timing", "2.4GHz" },
        { "link_bw_cycles", "Link ingress bandwidth in cycles per 64B (0 disables ingress bandwidth shaping)", "0" },
        { "link_latency_cycles", "Link ingress base latency in cycles (0 disables ingress latency shaping; when both bw+lat are 0, ingress queue is bypassed)", "0" },
        { "link_queue_size", "Link ingress queue capacity in bytes (0 = unbounded)", "0" },
        { "lightweight_output", "If set, emit stat.* switch summaries", "0" }
    )

#define CS_SWITCH_NODE_PORT_ENTRY(N) { "port_handler_nodes" #N, "Node-facing port " #N, { "cscore.Switch", "" } },
#define CS_SWITCH_POOL_PORT_ENTRY(N) { "port_handler_pools" #N, "Pool-facing port " #N, { "cscore.Switch", "" } },
    SST_ELI_DOCUMENT_PORTS(
        FOR_EACH_INDEX_64(CS_SWITCH_NODE_PORT_ENTRY)
        FOR_EACH_INDEX_64(CS_SWITCH_POOL_PORT_ENTRY)
    )
#undef CS_SWITCH_NODE_PORT_ENTRY
#undef CS_SWITCH_POOL_PORT_ENTRY

private:
    Switch();
    Switch(const Switch&) = delete;
    void operator=(const Switch&) = delete;

    struct PortState;

    void handle_event(SST::Event* ev);
    void finish() override;
    bool clock_tick(SST::Cycle_t cycle);
    void reset_stats_and_broadcast();
    bool try_route_event(csEvent* ev);
    void try_receive_and_route(PortState& port, uint64_t cycle);
    void for_each_port(const std::function<void(PortState&)>& fn);
    void for_each_port(const std::function<void(const PortState&)>& fn) const;
    std::size_t pick_pool_index(bool advance, const csEvent* probe);

    int num_nodes_ = 0;
    int num_pools_ = 0;
    uint64_t pool_node_id_base_ = 100;
    bool replicate_writes_ = false;
    enum class PoolSelectPolicy { Fixed0, RoundRobin };
    PoolSelectPolicy pool_select_policy_ = PoolSelectPolicy::RoundRobin;
    std::size_t rr_pool_idx_ = 0;
    std::string clock_frequency_{"2.4GHz"};
    int64_t link_bw_cycles_ = 0;
    int64_t link_latency_cycles_ = 0;
    int64_t link_queue_size_ = 0;
    bool lightweight_output_ = false;
    struct PortState {
        FabricPort port;
    };
    std::vector<PortState> node_ports_;
    std::vector<PortState> pool_ports_;
    uint64_t replicated_count_ = 0;
    uint64_t route_blocked_to_node_ = 0;
    uint64_t route_blocked_to_pool_ = 0;
    uint64_t route_blocked_replicated_write_ = 0;
    uint64_t route_send_fail_to_node_ = 0;
    uint64_t route_send_fail_to_pool_ = 0;
    uint64_t tick_count_ = 0;
    std::chrono::steady_clock::time_point wall_start_{};
    std::chrono::steady_clock::duration active_time_{};
    uint64_t active_calls_ = 0;
};

} // namespace csimCore
} // namespace SST
