#include "fabric.h"

#include <string>

namespace SST {
namespace csimCore {

Fabric::Fabric(SST::ComponentId_t id, SST::Params& params)
    : Component(id)
{
    auto clock_frequency_str = params.find<std::string>("clock", "1GHz");
    link_bandwidth_ = static_cast<int64_t>(params.find<int>("link_bandwidth", 1));
    link_base_latency_ = static_cast<int64_t>(params.find<int>("link_base_latency", 1));

    port0_ = configureLink("port_handler0", new Event::Handler<Fabric>(this, &Fabric::handle_event_port0));
    port1_ = configureLink("port_handler1", new Event::Handler<Fabric>(this, &Fabric::handle_event_port1));

    registerClock(clock_frequency_str, new Clock::Handler<Fabric>(this, &Fabric::tick));

    queue_0_to_1_ = std::make_unique<lat_bw_queue<csEvent*>>(
        link_bandwidth_, [this](double) { return link_base_latency_; });
    queue_1_to_0_ = std::make_unique<lat_bw_queue<csEvent*>>(
        link_bandwidth_, [this](double) { return link_base_latency_; });
}

bool Fabric::tick(SST::Cycle_t /*current*/)
{
    auto completed_to_1 = queue_0_to_1_->on_tick();
    for (auto* ev : completed_to_1) {
        if (port1_) {
            port1_->send(ev);
        } else {
            delete ev;
        }
    }

    auto completed_to_0 = queue_1_to_0_->on_tick();
    for (auto* ev : completed_to_0) {
        if (port0_) {
            port0_->send(ev);
        } else {
            delete ev;
        }
    }

    return false;
}

void Fabric::handle_event_port0(SST::Event* ev)
{
    queue_0_to_1_->add_packet(static_cast<csEvent*>(ev));
}

void Fabric::handle_event_port1(SST::Event* ev)
{
    queue_1_to_0_->add_packet(static_cast<csEvent*>(ev));
}

} // namespace csimCore
} // namespace SST
