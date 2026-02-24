#include "switch.h"

#include <algorithm>
#include <limits>
#include <string>

namespace SST {
namespace csimCore {

Switch::Switch(SST::ComponentId_t id, SST::Params& params)
    : Component(id)
{
    num_nodes_ = params.find<int>("num_nodes", 0);
    num_pools_ = params.find<int>("num_pools", 0);
    pool_node_id_base_ = params.find<uint64_t>("pool_node_id_base", 100);

    node_links_.resize(std::max(num_nodes_, 0));
    for (int i = 0; i < num_nodes_; ++i) {
        std::string port_name = "port_handler_nodes" + std::to_string(i);
        node_links_[i] = configureLink(port_name, new Event::Handler<Switch>(this, &Switch::handle_event));
    }

    pool_links_.resize(std::max(num_pools_, 0));
    for (int p = 0; p < num_pools_; ++p) {
        std::string port_name = "port_handler_pools" + std::to_string(p);
        pool_links_[p] = configureLink(port_name, new Event::Handler<Switch>(this, &Switch::handle_event));
    }
}

void Switch::handle_event(SST::Event* ev)
{
    auto* cevent = dynamic_cast<csEvent*>(ev);
    if (!cevent || cevent->payload.size() < 2) {
        delete ev;
        return;
    }

    const uint64_t src = cevent->payload[0];
    const uint64_t dst = cevent->payload[1];

    if (dst < static_cast<uint64_t>(num_nodes_)) {
        const auto node_idx = static_cast<size_t>(dst);
        if (node_idx < node_links_.size() && node_links_[node_idx]) {
            node_links_[node_idx]->send(cevent);
            return;
        }
        delete cevent;
        return;
    }

    if (dst >= pool_node_id_base_) {
        if (num_pools_ <= 0) {
            delete cevent;
            return;
        }
        const auto pool_idx = static_cast<size_t>(0);
        if (pool_idx < pool_links_.size() && pool_links_[pool_idx]) {
            pool_links_[pool_idx]->send(cevent);
            return;
        }
        delete cevent;
        return;
    }

    delete cevent;
}

} // namespace csimCore
} // namespace SST
