#include "cxl_mempool.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <limits>
#include <utility>
#include <vector>
#include <iostream>

#include "convert_ev_packet.h"
#include "chrono.h"
#include "champsim.h"
#include "control_event.h"

namespace SST {
namespace csimCore {

namespace {
constexpr uint64_t kClockPeriodPs = 417; // ~2.4 GHz
int64_t resolve_mem_bw(uint64_t mem_bw, uint64_t dev_bw, uint64_t bw_cycles) {
    if (bw_cycles != 0) {
        return static_cast<int64_t>(std::max<uint64_t>(bw_cycles, 1));
    }
    auto chosen = mem_bw != 0 ? mem_bw : dev_bw;
    if (chosen == 0) {
        return static_cast<int64_t>(DEFAULT_BW);
    }
    auto cycles = (BLOCK_SIZE + chosen - 1) / chosen;
    return static_cast<int64_t>(std::max<uint64_t>(cycles, 1));
}

MY_MEMORY_CONTROLLER::latency_function_type select_pool_latency_fn(SST::Params& params, int64_t fixed_cycles) {
    auto model = params.find<std::string>("pool_latency_model", "fixed");
    for (auto& ch : model) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (model == "percentile" || model == "util" || model == "utilization") {
        return estimate_latency_percentile;
    }
    return [fixed_cycles](double) { return fixed_cycles; };
}
} // namespace

CXLMemoryPool::CXLMemoryPool(SST::ComponentId_t id, SST::Params& params)
    : Component(id),
      pool_bw_cycles_per_req_(params.find<uint64_t>("pool_bw_cycles_per_req", 0)),
      device_bandwidth_(params.find<uint64_t>("device_bandwidth", 0)),
      memory_bandwidth_(params.find<uint64_t>("memory_bandwidth", 0)),
      latency_cycles_(static_cast<int64_t>(params.find<uint64_t>("latency_cycles", DEFAULT_FIXED_LATENCY_CYCLES))),
      link_bw_cycles_(params.find<int64_t>("link_bw_cycles", 0)),
      link_latency_cycles_(params.find<int64_t>("link_latency_cycles", 0)),
      link_queue_size_(params.find<int64_t>("link_queue_size", 0)),
      pool_node_id_(static_cast<uint32_t>(params.find<uint64_t>("pool_node_id", 100))),
      clock_frequency_(params.find<std::string>("clock", "2.4GHz")),
      mem_channel_{},
      mem_ctrl_(champsim::chrono::picoseconds{kClockPeriodPs},
                std::vector<champsim::channel*>{&mem_channel_},
                resolve_mem_bw(memory_bandwidth_, device_bandwidth_, pool_bw_cycles_per_req_),
                select_pool_latency_fn(params, latency_cycles_)),
      heartbeat_period_(params.find<uint64_t>("heartbeat_period", 1000)) {

    registerClock(clock_frequency_, new Clock::Handler<CXLMemoryPool>(this, &CXLMemoryPool::clock_tick));

    pool_link_ = configureLink(
        "port_handler_pool",
        new Event::Handler<CXLMemoryPool>(this, &CXLMemoryPool::handle_request));

    for (int i = 0; i < MAX_CXL_PORTS; ++i) {
        std::string port_name = "port_handler_cores" + std::to_string(i);
        core_links_[i] = configureLink(
            port_name,
            new Event::Handler<CXLMemoryPool>(this, &CXLMemoryPool::handle_request));
    }

    if (link_bw_cycles_ > 0 || link_latency_cycles_ > 0 || link_queue_size_ > 0) {
        const int64_t bw_cycles = std::max<int64_t>(link_bw_cycles_, 1);
        const int64_t lat_cycles = std::max<int64_t>(link_latency_cycles_, 1);
        auto latency_fn = [lat_cycles](double) { return lat_cycles; };
        const double peak_bw_per_cycle = 64.0 / static_cast<double>(bw_cycles);
        auto bw_cost_fn = [](const sst_response& resp) {
            const double bytes = (resp.msg_bytes == 0) ? 64.0 : static_cast<double>(resp.msg_bytes);
            return std::max<double>(bytes, 1.0);
        };
        resp_link_queue_ = std::make_unique<lat_bw_queue<sst_response>>(peak_bw_per_cycle, std::move(latency_fn), bw_cost_fn, link_queue_size_);
    }
}

bool CXLMemoryPool::clock_tick(SST::Cycle_t /*current*/) {
    ++tick_count_;
    mem_ctrl_.operate();
    if (resp_link_queue_) {
        auto ready = resp_link_queue_->on_tick();
        for (auto& resp : ready) {
            send_response_event(resp);
        }
    }
    while (!mem_channel_.returned.empty()) {
        if (resp_link_queue_ && resp_link_queue_->is_full()) {
            break;
        }
        const auto& response = mem_channel_.returned.front();
        if (!produce_placeholder_response(response)) {
            break;
        }
        mem_channel_.returned.pop_front();
        ++total_completed_;
    }
    if (heartbeat_period_ > 0 && (tick_count_ % heartbeat_period_ == 0)) {
        std::cout << "CXL heartbeat cycle " << tick_count_ << '\n';
        std::cout << "  total_enqueued:     " << total_enqueued_ << '\n';
        std::cout << "  total_completed:    " << total_completed_ << '\n';
        std::cout << "  pending_responses:  " << pending_.size() << '\n';
        std::cout << "  mem queue occ:      " << mem_ctrl_.queue_occupancy(0)
                  << " util: " << mem_ctrl_.queue_utilization(0) << '\n';
        if (resp_link_queue_) {
            std::cout << "  resp link occ:      " << resp_link_queue_->occupancy()
                      << " util: " << resp_link_queue_->utilization() << '\n';
        }
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

    uint64_t ctrl_code = 0;
    if (is_control_event(*cevent, &ctrl_code)) {
        if (ctrl_code == kControlResetUtil) {
            mem_ctrl_.reset_utilization();
            if (resp_link_queue_) {
                resp_link_queue_->reset_utilization();
            }
        }
        delete cevent;
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

bool CXLMemoryPool::produce_placeholder_response(const champsim::channel::response_type& response) {
    if (response.instr_depend_on_me.empty()) {
        return true;
    }

    auto tag = response.instr_depend_on_me.front();
    auto pending_it = pending_.find(tag);
    if (pending_it == pending_.end()) {
        return true;
    }
    OutstandingRequest route = pending_it->second;
    pending_.erase(pending_it);

    auto idx = static_cast<int>(route.sst_cpu);

    SST::Link* target_link = nullptr;
    if (pool_link_ != nullptr) {
        target_link = pool_link_;
    } else if (idx >= 0 && idx < MAX_CXL_PORTS) {
        target_link = core_links_[idx];
    }

    assert(target_link != nullptr && "CXLMemoryPool: target_link is null for requested port");

    sst_response out(response.address.to<uint64_t>(),
                     response.v_address.to<uint64_t>(),
                     response.data.to<uint64_t>(),
                     response.pf_metadata,
                     route.cpu,
                     route.sst_cpu);
    out.msg_bytes = 64;
    out.src_node = pool_node_id_;
    out.dst_node = route.src_node == std::numeric_limits<uint32_t>::max()
                       ? route.sst_cpu
                       : route.src_node;

    if (resp_link_queue_) {
        return resp_link_queue_->add_packet(std::move(out));
    }
    send_response_event(out);
    return true;
}

void CXLMemoryPool::send_response_event(const sst_response& resp) {
    auto idx = static_cast<int>(resp.sst_cpu);
    SST::Link* target_link = nullptr;
    if (pool_link_ != nullptr) {
        target_link = pool_link_;
    } else if (idx >= 0 && idx < MAX_CXL_PORTS) {
        target_link = core_links_[idx];
    }
    assert(target_link != nullptr && "CXLMemoryPool: target_link is null for requested port");
    auto* event = convert_response_to_event(resp);
    target_link->send(event);
}

void CXLMemoryPool::finish() {
    std::cout << "CXL pool " << pool_node_id_ << " utilization summary\n";
    std::cout << "  mem avg util: " << mem_ctrl_.queue_average_utilization(0) << '\n';
    if (resp_link_queue_) {
        std::cout << "  resp link avg util: " << resp_link_queue_->average_utilization() << '\n';
    }
    std::cout << std::flush;
}

} // namespace csimCore
} // namespace SST
