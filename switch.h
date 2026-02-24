#pragma once

#include <cstdint>
#include <vector>

#include <sst/core/component.h>
#include <sst/core/link.h>

#include "csEvent.h"

namespace SST {
namespace csimCore {

/**
 * Switch is a multi-port pass-through router.
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
        { "pool_node_id_base", "Base node_id for pools (contiguous)", "100" }
    )

    SST_ELI_DOCUMENT_PORTS(
        { "port_handler_nodes0", "Node-facing port 0", { "cscore.Switch", "" } },
        { "port_handler_pools0", "Pool-facing port 0", { "cscore.Switch", "" } }
    )

private:
    Switch();
    Switch(const Switch&) = delete;
    void operator=(const Switch&) = delete;

    void handle_event(SST::Event* ev);

    int num_nodes_ = 0;
    int num_pools_ = 0;
    uint64_t pool_node_id_base_ = 100;
    std::vector<SST::Link*> node_links_;
    std::vector<SST::Link*> pool_links_;
};

} // namespace csimCore
} // namespace SST
