#include "switch.h"

#include <algorithm>
#include <limits>
#include <string>

#include "access_type.h"

namespace SST {
namespace csimCore {

Switch::Switch(SST::ComponentId_t id, SST::Params& params)
    : Component(id)
{
    num_nodes_ = params.find<int>("num_nodes", 0);
    num_pools_ = params.find<int>("num_pools", 0);
    pool_node_id_base_ = params.find<uint64_t>("pool_node_id_base", 100);
    replicate_writes_ = params.find<int>("replicate_writes", 0) != 0;
    clock_frequency_ = params.find<std::string>("clock", "2.4GHz");
    link_bw_cycles_ = params.find<int64_t>("link_bw_cycles", 0);
    link_latency_cycles_ = params.find<int64_t>("link_latency_cycles", 0);
    link_queue_size_ = params.find<int64_t>("link_queue_size", 0);
    use_link_queues_ = (link_bw_cycles_ > 0 || link_latency_cycles_ > 0 || link_queue_size_ > 0);

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

    if (use_link_queues_) {
        const int64_t bw_cycles = std::max<int64_t>(link_bw_cycles_, 1);
        const int64_t lat_cycles = std::max<int64_t>(link_latency_cycles_, 1);
        auto latency_fn = [lat_cycles](double) { return lat_cycles; };
        auto bw_cost_fn = [bw_cycles](const csEvent* ev) {
            uint64_t bytes = 64;
            if (ev && ev->payload.size() > 16) {
                bytes = ev->payload[16];
            } else if (ev && ev->payload.size() > 8) {
                bytes = ev->payload[8];
            }
            const uint64_t cost = (bw_cycles * bytes) / 64;
            return static_cast<int64_t>(std::max<uint64_t>(cost, 1));
        };
        node_queues_.resize(std::max(num_nodes_, 0));
        for (int i = 0; i < num_nodes_; ++i) {
            node_queues_[i] = std::make_unique<lat_bw_queue<csEvent*>>(
                bw_cycles, latency_fn, bw_cost_fn, link_queue_size_);
        }
        pool_queues_.resize(std::max(num_pools_, 0));
        for (int p = 0; p < num_pools_; ++p) {
            pool_queues_[p] = std::make_unique<lat_bw_queue<csEvent*>>(
                bw_cycles, latency_fn, bw_cost_fn, link_queue_size_);
        }

        registerClock(clock_frequency_, new Clock::Handler<Switch>(this, &Switch::clock_tick));
    }
}

namespace {
bool is_write_request(const csEvent& ev) {
    if (ev.payload.size() <= 10) {
        return false;
    }
    const auto type = static_cast<access_type>(ev.payload[10]);
    return type == access_type::WRITE || type == access_type::RFO;
}

csEvent* clone_event_with_dst(const csEvent& ev, uint64_t dst) {
    auto* out = new csEvent();
    out->payload = ev.payload;
    out->last = ev.last;
    if (out->payload.size() >= 2) {
        out->payload[1] = dst;
    }
    return out;
}
} // namespace

bool Switch::clock_tick(SST::Cycle_t /*cycle*/)
{
    if (!use_link_queues_) {
        return false;
    }

    for (size_t i = 0; i < node_queues_.size(); ++i) {
        auto& q = node_queues_[i];
        if (!q) {
            continue;
        }
        auto ready = q->on_tick();
        for (auto* ev : ready) {
            if (i < node_links_.size() && node_links_[i]) {
                node_links_[i]->send(ev);
            } else {
                delete ev;
            }
        }
    }

    for (size_t p = 0; p < pool_queues_.size(); ++p) {
        auto& q = pool_queues_[p];
        if (!q) {
            continue;
        }
        auto ready = q->on_tick();
        for (auto* ev : ready) {
            if (p < pool_links_.size() && pool_links_[p]) {
                pool_links_[p]->send(ev);
            } else {
                delete ev;
            }
        }
    }

    return false;
}

void Switch::handle_event(SST::Event* ev)
{
    auto* cevent = dynamic_cast<csEvent*>(ev);
    if (!cevent || cevent->payload.size() < 2) {
        delete ev;
        return;
    }

    const uint64_t dst = cevent->payload[1];

    if (dst < static_cast<uint64_t>(num_nodes_)) {
        const auto node_idx = static_cast<size_t>(dst);
        if (node_idx < node_links_.size() && node_links_[node_idx]) {
            if (use_link_queues_ && node_idx < node_queues_.size() && node_queues_[node_idx]) {
                if (!node_queues_[node_idx]->add_packet(std::move(cevent))) {
                    delete cevent;
                }
                return;
            }
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
        if (replicate_writes_ && is_write_request(*cevent)) {
            for (int p = 0; p < num_pools_; ++p) {
                if (p >= static_cast<int>(pool_links_.size()) || pool_links_[p] == nullptr) {
                    continue;
                }
                const uint64_t pool_dst = pool_node_id_base_ + static_cast<uint64_t>(p);
                auto* clone = clone_event_with_dst(*cevent, pool_dst);
                if (use_link_queues_ && p < static_cast<int>(pool_queues_.size()) && pool_queues_[p]) {
                    if (!pool_queues_[p]->add_packet(std::move(clone))) {
                        delete clone;
                    }
                    continue;
                }
                pool_links_[p]->send(clone);
            }
            delete cevent;
            return;
        }
        const auto pool_idx = static_cast<size_t>(0);
        if (pool_idx < pool_links_.size() && pool_links_[pool_idx]) {
            cevent->payload[1] = pool_node_id_base_ + pool_idx;
            if (use_link_queues_ && pool_idx < pool_queues_.size() && pool_queues_[pool_idx]) {
                if (!pool_queues_[pool_idx]->add_packet(std::move(cevent))) {
                    delete cevent;
                }
                return;
            }
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
