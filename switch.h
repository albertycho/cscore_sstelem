#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <chrono>

#include <sst/core/component.h>
#include <sst/core/link.h>

#include "csEvent.h"
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
        { "link_bw_cycles", "Link bandwidth in cycles per 64B (enables ingress queues if nonzero)", "0" },
        { "link_latency_cycles", "Link base latency in cycles (ingress queues)", "0" },
        { "link_queue_size", "Link ingress queue capacity in packets (used as byte credits; 0 = unbounded)", "0" }
    )

    SST_ELI_DOCUMENT_PORTS(
        { "port_handler_nodes0", "Node-facing port 0", { "cscore.Switch", "" } },
        { "port_handler_nodes1", "Node-facing port 1", { "cscore.Switch", "" } },
        { "port_handler_nodes2", "Node-facing port 2", { "cscore.Switch", "" } },
        { "port_handler_nodes3", "Node-facing port 3", { "cscore.Switch", "" } },
        { "port_handler_nodes4", "Node-facing port 4", { "cscore.Switch", "" } },
        { "port_handler_nodes5", "Node-facing port 5", { "cscore.Switch", "" } },
        { "port_handler_nodes6", "Node-facing port 6", { "cscore.Switch", "" } },
        { "port_handler_nodes7", "Node-facing port 7", { "cscore.Switch", "" } },
        { "port_handler_nodes8", "Node-facing port 8", { "cscore.Switch", "" } },
        { "port_handler_nodes9", "Node-facing port 9", { "cscore.Switch", "" } },
        { "port_handler_nodes10", "Node-facing port 10", { "cscore.Switch", "" } },
        { "port_handler_nodes11", "Node-facing port 11", { "cscore.Switch", "" } },
        { "port_handler_nodes12", "Node-facing port 12", { "cscore.Switch", "" } },
        { "port_handler_nodes13", "Node-facing port 13", { "cscore.Switch", "" } },
        { "port_handler_nodes14", "Node-facing port 14", { "cscore.Switch", "" } },
        { "port_handler_nodes15", "Node-facing port 15", { "cscore.Switch", "" } },
        { "port_handler_nodes16", "Node-facing port 16", { "cscore.Switch", "" } },
        { "port_handler_nodes17", "Node-facing port 17", { "cscore.Switch", "" } },
        { "port_handler_nodes18", "Node-facing port 18", { "cscore.Switch", "" } },
        { "port_handler_nodes19", "Node-facing port 19", { "cscore.Switch", "" } },
        { "port_handler_nodes20", "Node-facing port 20", { "cscore.Switch", "" } },
        { "port_handler_nodes21", "Node-facing port 21", { "cscore.Switch", "" } },
        { "port_handler_nodes22", "Node-facing port 22", { "cscore.Switch", "" } },
        { "port_handler_nodes23", "Node-facing port 23", { "cscore.Switch", "" } },
        { "port_handler_nodes24", "Node-facing port 24", { "cscore.Switch", "" } },
        { "port_handler_nodes25", "Node-facing port 25", { "cscore.Switch", "" } },
        { "port_handler_nodes26", "Node-facing port 26", { "cscore.Switch", "" } },
        { "port_handler_nodes27", "Node-facing port 27", { "cscore.Switch", "" } },
        { "port_handler_nodes28", "Node-facing port 28", { "cscore.Switch", "" } },
        { "port_handler_nodes29", "Node-facing port 29", { "cscore.Switch", "" } },
        { "port_handler_nodes30", "Node-facing port 30", { "cscore.Switch", "" } },
        { "port_handler_nodes31", "Node-facing port 31", { "cscore.Switch", "" } },
        { "port_handler_nodes32", "Node-facing port 32", { "cscore.Switch", "" } },
        { "port_handler_nodes33", "Node-facing port 33", { "cscore.Switch", "" } },
        { "port_handler_nodes34", "Node-facing port 34", { "cscore.Switch", "" } },
        { "port_handler_nodes35", "Node-facing port 35", { "cscore.Switch", "" } },
        { "port_handler_nodes36", "Node-facing port 36", { "cscore.Switch", "" } },
        { "port_handler_nodes37", "Node-facing port 37", { "cscore.Switch", "" } },
        { "port_handler_nodes38", "Node-facing port 38", { "cscore.Switch", "" } },
        { "port_handler_nodes39", "Node-facing port 39", { "cscore.Switch", "" } },
        { "port_handler_nodes40", "Node-facing port 40", { "cscore.Switch", "" } },
        { "port_handler_nodes41", "Node-facing port 41", { "cscore.Switch", "" } },
        { "port_handler_nodes42", "Node-facing port 42", { "cscore.Switch", "" } },
        { "port_handler_nodes43", "Node-facing port 43", { "cscore.Switch", "" } },
        { "port_handler_nodes44", "Node-facing port 44", { "cscore.Switch", "" } },
        { "port_handler_nodes45", "Node-facing port 45", { "cscore.Switch", "" } },
        { "port_handler_nodes46", "Node-facing port 46", { "cscore.Switch", "" } },
        { "port_handler_nodes47", "Node-facing port 47", { "cscore.Switch", "" } },
        { "port_handler_nodes48", "Node-facing port 48", { "cscore.Switch", "" } },
        { "port_handler_nodes49", "Node-facing port 49", { "cscore.Switch", "" } },
        { "port_handler_nodes50", "Node-facing port 50", { "cscore.Switch", "" } },
        { "port_handler_nodes51", "Node-facing port 51", { "cscore.Switch", "" } },
        { "port_handler_nodes52", "Node-facing port 52", { "cscore.Switch", "" } },
        { "port_handler_nodes53", "Node-facing port 53", { "cscore.Switch", "" } },
        { "port_handler_nodes54", "Node-facing port 54", { "cscore.Switch", "" } },
        { "port_handler_nodes55", "Node-facing port 55", { "cscore.Switch", "" } },
        { "port_handler_nodes56", "Node-facing port 56", { "cscore.Switch", "" } },
        { "port_handler_nodes57", "Node-facing port 57", { "cscore.Switch", "" } },
        { "port_handler_nodes58", "Node-facing port 58", { "cscore.Switch", "" } },
        { "port_handler_nodes59", "Node-facing port 59", { "cscore.Switch", "" } },
        { "port_handler_nodes60", "Node-facing port 60", { "cscore.Switch", "" } },
        { "port_handler_nodes61", "Node-facing port 61", { "cscore.Switch", "" } },
        { "port_handler_nodes62", "Node-facing port 62", { "cscore.Switch", "" } },
        { "port_handler_nodes63", "Node-facing port 63", { "cscore.Switch", "" } },
        { "port_handler_pools0", "Pool-facing port 0", { "cscore.Switch", "" } },
        { "port_handler_pools1", "Pool-facing port 1", { "cscore.Switch", "" } },
        { "port_handler_pools2", "Pool-facing port 2", { "cscore.Switch", "" } },
        { "port_handler_pools3", "Pool-facing port 3", { "cscore.Switch", "" } },
        { "port_handler_pools4", "Pool-facing port 4", { "cscore.Switch", "" } },
        { "port_handler_pools5", "Pool-facing port 5", { "cscore.Switch", "" } },
        { "port_handler_pools6", "Pool-facing port 6", { "cscore.Switch", "" } },
        { "port_handler_pools7", "Pool-facing port 7", { "cscore.Switch", "" } },
        { "port_handler_pools8", "Pool-facing port 8", { "cscore.Switch", "" } },
        { "port_handler_pools9", "Pool-facing port 9", { "cscore.Switch", "" } },
        { "port_handler_pools10", "Pool-facing port 10", { "cscore.Switch", "" } },
        { "port_handler_pools11", "Pool-facing port 11", { "cscore.Switch", "" } },
        { "port_handler_pools12", "Pool-facing port 12", { "cscore.Switch", "" } },
        { "port_handler_pools13", "Pool-facing port 13", { "cscore.Switch", "" } },
        { "port_handler_pools14", "Pool-facing port 14", { "cscore.Switch", "" } },
        { "port_handler_pools15", "Pool-facing port 15", { "cscore.Switch", "" } },
        { "port_handler_pools16", "Pool-facing port 16", { "cscore.Switch", "" } },
        { "port_handler_pools17", "Pool-facing port 17", { "cscore.Switch", "" } },
        { "port_handler_pools18", "Pool-facing port 18", { "cscore.Switch", "" } },
        { "port_handler_pools19", "Pool-facing port 19", { "cscore.Switch", "" } },
        { "port_handler_pools20", "Pool-facing port 20", { "cscore.Switch", "" } },
        { "port_handler_pools21", "Pool-facing port 21", { "cscore.Switch", "" } },
        { "port_handler_pools22", "Pool-facing port 22", { "cscore.Switch", "" } },
        { "port_handler_pools23", "Pool-facing port 23", { "cscore.Switch", "" } },
        { "port_handler_pools24", "Pool-facing port 24", { "cscore.Switch", "" } },
        { "port_handler_pools25", "Pool-facing port 25", { "cscore.Switch", "" } },
        { "port_handler_pools26", "Pool-facing port 26", { "cscore.Switch", "" } },
        { "port_handler_pools27", "Pool-facing port 27", { "cscore.Switch", "" } },
        { "port_handler_pools28", "Pool-facing port 28", { "cscore.Switch", "" } },
        { "port_handler_pools29", "Pool-facing port 29", { "cscore.Switch", "" } },
        { "port_handler_pools30", "Pool-facing port 30", { "cscore.Switch", "" } },
        { "port_handler_pools31", "Pool-facing port 31", { "cscore.Switch", "" } },
        { "port_handler_pools32", "Pool-facing port 32", { "cscore.Switch", "" } },
        { "port_handler_pools33", "Pool-facing port 33", { "cscore.Switch", "" } },
        { "port_handler_pools34", "Pool-facing port 34", { "cscore.Switch", "" } },
        { "port_handler_pools35", "Pool-facing port 35", { "cscore.Switch", "" } },
        { "port_handler_pools36", "Pool-facing port 36", { "cscore.Switch", "" } },
        { "port_handler_pools37", "Pool-facing port 37", { "cscore.Switch", "" } },
        { "port_handler_pools38", "Pool-facing port 38", { "cscore.Switch", "" } },
        { "port_handler_pools39", "Pool-facing port 39", { "cscore.Switch", "" } },
        { "port_handler_pools40", "Pool-facing port 40", { "cscore.Switch", "" } },
        { "port_handler_pools41", "Pool-facing port 41", { "cscore.Switch", "" } },
        { "port_handler_pools42", "Pool-facing port 42", { "cscore.Switch", "" } },
        { "port_handler_pools43", "Pool-facing port 43", { "cscore.Switch", "" } },
        { "port_handler_pools44", "Pool-facing port 44", { "cscore.Switch", "" } },
        { "port_handler_pools45", "Pool-facing port 45", { "cscore.Switch", "" } },
        { "port_handler_pools46", "Pool-facing port 46", { "cscore.Switch", "" } },
        { "port_handler_pools47", "Pool-facing port 47", { "cscore.Switch", "" } },
        { "port_handler_pools48", "Pool-facing port 48", { "cscore.Switch", "" } },
        { "port_handler_pools49", "Pool-facing port 49", { "cscore.Switch", "" } },
        { "port_handler_pools50", "Pool-facing port 50", { "cscore.Switch", "" } },
        { "port_handler_pools51", "Pool-facing port 51", { "cscore.Switch", "" } },
        { "port_handler_pools52", "Pool-facing port 52", { "cscore.Switch", "" } },
        { "port_handler_pools53", "Pool-facing port 53", { "cscore.Switch", "" } },
        { "port_handler_pools54", "Pool-facing port 54", { "cscore.Switch", "" } },
        { "port_handler_pools55", "Pool-facing port 55", { "cscore.Switch", "" } },
        { "port_handler_pools56", "Pool-facing port 56", { "cscore.Switch", "" } },
        { "port_handler_pools57", "Pool-facing port 57", { "cscore.Switch", "" } },
        { "port_handler_pools58", "Pool-facing port 58", { "cscore.Switch", "" } },
        { "port_handler_pools59", "Pool-facing port 59", { "cscore.Switch", "" } },
        { "port_handler_pools60", "Pool-facing port 60", { "cscore.Switch", "" } },
        { "port_handler_pools61", "Pool-facing port 61", { "cscore.Switch", "" } },
        { "port_handler_pools62", "Pool-facing port 62", { "cscore.Switch", "" } },
        { "port_handler_pools63", "Pool-facing port 63", { "cscore.Switch", "" } },
    )

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
    std::size_t pick_pool_index(bool advance);

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
    struct PortState {
        FabricPort port;
    };
    std::vector<PortState> node_ports_;
    std::vector<PortState> pool_ports_;
    uint64_t replicated_count_ = 0;
    uint64_t current_cycle_ = 0;
    std::chrono::steady_clock::time_point wall_start_{};
    bool wall_start_set_ = false;
    std::chrono::steady_clock::duration active_time_{};
    uint64_t active_calls_ = 0;
};

} // namespace csimCore
} // namespace SST
