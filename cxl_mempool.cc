#include "cxl_mempool.h"

#include <algorithm>
#include <utility>
#include <cassert>

#include "convert_ev_packet.h"

namespace SST {
namespace csimCore {

CXLMemoryPool::CXLMemoryPool(SST::ComponentId_t id, SST::Params& params)
    : Component(id),
      device_bandwidth_(params.find<uint64_t>("device_bandwidth", 0)),
      memory_bandwidth_(params.find<uint64_t>("memory_bandwidth", 0)),
      latency_cycles_(static_cast<int64_t>(params.find<uint64_t>("latency_cycles", 50))),
      clock_frequency_(params.find<std::string>("clock", "1GHz")),
      requests_(static_cast<int64_t>(std::max<uint64_t>(device_bandwidth_ ? device_bandwidth_ : 1, 1)),
               [this](double) { return latency_cycles_; }) {

    stats_file_.open("/nethome/kshan9/sst-test/cxl_stats.txt", std::ios::out | std::ios::trunc);

    registerClock(clock_frequency_, new Clock::Handler<CXLMemoryPool>(this, &CXLMemoryPool::clock_tick));

    for (int i = 0; i < MAX_CXL_PORTS; ++i) {
        std::string port_name = "port_handler_cores" + std::to_string(i);
        core_links_[i] = configureLink(
            port_name,
            new Event::Handler<CXLMemoryPool>(this, &CXLMemoryPool::handle_request));
    }
}

bool CXLMemoryPool::clock_tick(SST::Cycle_t current) {
    ++tick_count_;
    auto completed = requests_.on_tick();
    total_completed_ += completed.size();
    for (auto& req : completed) {
        produce_placeholder_response(req);
    }
    if ((tick_count_ % 1000 == 0)) {
        stats_file_ << "CXL heartbeat cycle " << tick_count_ << '\n';
        stats_file_ << "  total_enqueued:     " << total_enqueued_ << '\n';
        stats_file_ << "  total_completed:    " << total_completed_ << std::endl;
    }
    return false;
}

void CXLMemoryPool::handle_request(SST::Event* ev) {
    auto* cevent = dynamic_cast<csEvent*>(ev);
    if (!cevent) {
        delete ev;
        return;
    }

    auto request = convert_event_to_request(*cevent);
    requests_.add_packet(std::move(request));
    ++total_enqueued_;

    delete cevent;
}

void CXLMemoryPool::produce_placeholder_response(const sst_request& request) {
    auto idx = static_cast<int>(request.sst_cpu);

    SST::Link* target_link = nullptr;
    if (idx >= 0 && idx < MAX_CXL_PORTS) {
        target_link = core_links_[idx];
    }

    assert(target_link != nullptr && "CXLMemoryPool: target_link is null for requested port");

    sst_response response(request);
    responses_.push_back(response);

    auto* event = convert_response_to_event(responses_.front());
    target_link->send(event);
    responses_.pop_front();
}

} // namespace csimCore
} // namespace SST
