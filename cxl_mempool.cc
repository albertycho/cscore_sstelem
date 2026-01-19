#include "cxl_mempool.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>
#include <vector>
#include <iostream>

#include "convert_ev_packet.h"
#include "chrono.h"
#include "champsim.h"

namespace SST {
namespace csimCore {

namespace {
int64_t resolve_mem_bw(uint64_t mem_bw, uint64_t dev_bw) {
    auto chosen = mem_bw != 0 ? mem_bw : dev_bw;
    if (chosen == 0) {
        return static_cast<int64_t>(DEFAULT_BW);
    }
    auto cycles = (BLOCK_SIZE + chosen - 1) / chosen;
    return static_cast<int64_t>(std::max<uint64_t>(cycles, 1));
}
} // namespace

CXLMemoryPool::CXLMemoryPool(SST::ComponentId_t id, SST::Params& params)
    : Component(id),
      device_bandwidth_(params.find<uint64_t>("device_bandwidth", 0)),
      memory_bandwidth_(params.find<uint64_t>("memory_bandwidth", 0)),
      latency_cycles_(static_cast<int64_t>(params.find<uint64_t>("latency_cycles", 50))),
      pool_node_id_(static_cast<uint32_t>(params.find<uint64_t>("pool_node_id", 100))),
      clock_frequency_(params.find<std::string>("clock", "1GHz")),
      mem_channel_{},
      mem_ctrl_(champsim::chrono::picoseconds{500},
                std::vector<champsim::channel*>{&mem_channel_},
                resolve_mem_bw(memory_bandwidth_, device_bandwidth_),
                estimate_latency_percentile),
      heartbeat_period_(params.find<uint64_t>("heartbeat_period", 1000)) {

    registerClock(clock_frequency_, new Clock::Handler<CXLMemoryPool>(this, &CXLMemoryPool::clock_tick));

    for (int i = 0; i < MAX_CXL_PORTS; ++i) {
        std::string port_name = "port_handler_cores" + std::to_string(i);
        core_links_[i] = configureLink(
            port_name,
            new Event::Handler<CXLMemoryPool>(this, &CXLMemoryPool::handle_request));
    }
}

bool CXLMemoryPool::clock_tick(SST::Cycle_t /*current*/) {
    ++tick_count_;
    mem_ctrl_.operate();
    while (!mem_channel_.returned.empty()) {
        auto response = std::move(mem_channel_.returned.front());
        mem_channel_.returned.pop_front();
        produce_placeholder_response(response);
        ++total_completed_;
    }
    if (heartbeat_period_ > 0 && (tick_count_ % heartbeat_period_ == 0)) {
        std::cout << "CXL heartbeat cycle " << tick_count_ << '\n';
        std::cout << "  total_enqueued:     " << total_enqueued_ << '\n';
        std::cout << "  total_completed:    " << total_completed_ << '\n';
        std::cout << "  pending_responses:  " << pending_.size() << '\n';
        std::cout << std::flush;
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
    champsim::channel::request_type channel_req{};
    channel_req.forward_checked = request.forward_checked;
    channel_req.is_translated = request.is_translated;
    channel_req.response_requested = request.response_requested;
    channel_req.asid[0] = request.asid[0];
    channel_req.asid[1] = request.asid[1];
    channel_req.type = request.type;
    channel_req.pf_metadata = request.pf_metadata;
    channel_req.cpu = request.cpu;
    channel_req.address = champsim::address{request.address};
    channel_req.v_address = champsim::address{request.v_address};
    channel_req.data = champsim::address{request.data};
    channel_req.instr_id = request.instr_id;
    channel_req.ip = champsim::address{request.ip};

    const uint64_t tag = next_tag_++;
    channel_req.instr_depend_on_me.push_back(tag);
    if (request.response_requested) {
        pending_[tag] = OutstandingRequest{request.cpu, request.sst_cpu, request.src_node, request.dst_node};
    }

    auto& pool_queue = mem_channel_.PQ;
    pool_queue.push_back(std::move(channel_req));
    ++total_enqueued_;

    delete cevent;
}

void CXLMemoryPool::produce_placeholder_response(const champsim::channel::response_type& response) {
    if (response.instr_depend_on_me.empty()) {
        return;
    }

    auto tag = response.instr_depend_on_me.front();
    auto pending_it = pending_.find(tag);
    if (pending_it == pending_.end()) {
        return;
    }
    OutstandingRequest route = pending_it->second;
    pending_.erase(pending_it);

    auto idx = static_cast<int>(route.sst_cpu);

    SST::Link* target_link = nullptr;
    if (idx >= 0 && idx < MAX_CXL_PORTS) {
        target_link = core_links_[idx];
    }

    assert(target_link != nullptr && "CXLMemoryPool: target_link is null for requested port");

    sst_response out(response.address.to<uint64_t>(),
                     response.v_address.to<uint64_t>(),
                     response.data.to<uint64_t>(),
                     response.pf_metadata,
                     route.cpu,
                     route.sst_cpu);
    out.src_node = pool_node_id_;
    out.dst_node = route.src_node == std::numeric_limits<uint32_t>::max()
                       ? route.sst_cpu
                       : route.src_node;

    auto* event = convert_response_to_event(out);
    target_link->send(event);
}

} // namespace csimCore
} // namespace SST
