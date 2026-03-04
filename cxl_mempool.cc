#include "cxl_mempool.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#include <iostream>

#include "chrono.h"
#include "champsim.h"
#include "control_event.h"
#include "convert_ev_packet.h"

namespace SST {
namespace csimCore {

namespace {
constexpr uint64_t kClockPeriodPs = 417; // ~2.4 GHz
bool is_reset_event(const csEvent& ev) {
    return (ev.payload.size() == 3 || ev.payload.size() == 4) && ev.payload[2] == kControlResetUtil;
}
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
    if (model == "utilization-based") {
        return estimate_latency_utilization_based;
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
      heartbeat_period_(params.find<uint64_t>("heartbeat_period", 1000)),
      lightweight_output_(params.find<int>("lightweight_output", 0) != 0) {

    registerClock(clock_frequency_, new Clock::Handler<CXLMemoryPool>(this, &CXLMemoryPool::clock_tick));

    auto* pool_link = configureLink(
        "port_handler_switch",
        new Event::Handler<FabricPort>(
            &switch_port_,
            &FabricPort::handle_event));
    use_switch_port_ = (pool_link != nullptr);
    if (use_switch_port_) {
        switch_port_.configure(pool_link,
                               static_cast<uint64_t>(pool_node_id_),
                               link_bw_cycles_,
                               link_latency_cycles_,
                               link_queue_size_);
    }

    for (int i = 0; i < MAX_CXL_PORTS; ++i) {
        std::string port_name = "port_handler_nodes" + std::to_string(i);
        auto* core_link = configureLink(
            port_name,
            new Event::Handler<FabricPort>(
                &core_ports_[i],
                &FabricPort::handle_event));
        if (core_link != nullptr) {
            core_ports_[i].configure(core_link,
                                     static_cast<uint64_t>(pool_node_id_),
                                     link_bw_cycles_,
                                     link_latency_cycles_,
                                     link_queue_size_);
            core_port_connected_[static_cast<size_t>(i)] = true;
        }
    }

    std::vector<int> connected_cores;
    connected_cores.reserve(MAX_CXL_PORTS);
    for (int i = 0; i < MAX_CXL_PORTS; ++i) {
        if (core_port_connected_[static_cast<size_t>(i)]) {
            connected_cores.push_back(i);
        }
    }

    if (use_switch_port_ && !connected_cores.empty()) {
        throw std::runtime_error("CXLMemoryPool: both switch and direct core links are connected. "
                                 "Choose one topology (port_handler_switch OR port_handler_nodesX).");
    }
    if (!use_switch_port_ && connected_cores.empty()) {
        throw std::runtime_error("CXLMemoryPool: no links connected. "
                                 "Connect port_handler_switch or at least one port_handler_nodesX.");
    }
    if (use_switch_port_) {
        active_ports_.push_back(&switch_port_);
    } else {
        for (int idx : connected_cores) {
            active_ports_.push_back(&core_ports_[static_cast<size_t>(idx)]);
        }
    }
}

void CXLMemoryPool::setup() {
    wall_start_ = std::chrono::steady_clock::now();
    active_time_ = std::chrono::steady_clock::duration{};
    active_calls_ = 0;
}

bool CXLMemoryPool::clock_tick(SST::Cycle_t /*current*/) {
    ScopedTimer timer(active_time_, active_calls_);
    ++tick_count_;
    poll_ports(tick_count_);
    mem_ctrl_.operate();
    while (!mem_channel_.returned.empty()) {
        const auto& response = mem_channel_.returned.front();
        if (!try_send_response(response)) {
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
        const auto stats = request_link_stats();
        if (stats.occ > 0) {
            std::cout << "  req link occ:       " << stats.occ
                      << " util: " << stats.util << '\n';
        }
        std::cout << std::flush;
    }
    return false;
}

void CXLMemoryPool::enqueue_mem_request(const sst_request& request) {
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
}

void CXLMemoryPool::poll_ports(uint64_t cycle) {
    auto handle_event = [this](csEvent* ev) {
        if (is_reset_event(*ev)) {
            reset_stats();
            delete ev;
            return true;
        }
        if (mem_channel_.pq_occupancy() >= mem_channel_.pq_size()) {
            return false;
        }
        sst_request req = convert_event_to_request(*ev);
        delete ev;
        enqueue_mem_request(req);
        return true;
    };

    for_each_port([&](FabricPort& port) {
        port.tick(cycle);
        port.try_receive(cycle, handle_event);
    });
}

CXLMemoryPool::LinkStats CXLMemoryPool::request_link_stats() const {
    LinkStats stats{};
    double util_sum = 0.0;
    double avg_sum = 0.0;
    std::size_t count = 0;
    std::size_t occ_total = 0;

    auto accumulate = [&](const FabricPort& port) {
        util_sum += port.ingress_utilization();
        avg_sum += port.ingress_avg_utilization();
        occ_total += port.ingress_occupancy();
        ++count;
    };

    for_each_port([&](const FabricPort& port) { accumulate(port); });

    stats.occ = occ_total;
    if (count > 0) {
        stats.util = util_sum / static_cast<double>(count);
        stats.avg_util = avg_sum / static_cast<double>(count);
    }
    return stats;
}

void CXLMemoryPool::reset_stats() {
    mem_ctrl_.reset_utilization();
    for_each_port([](FabricPort& port) { port.reset_ingress_utilization(); });
}

void CXLMemoryPool::for_each_port(const std::function<void(FabricPort&)>& fn) {
    for (auto* port : active_ports_) {
        fn(*port);
    }
}

void CXLMemoryPool::for_each_port(const std::function<void(const FabricPort&)>& fn) const {
    for (const auto* port : active_ports_) {
        fn(*port);
    }
}

FabricPort* CXLMemoryPool::select_egress_port(uint32_t sst_cpu) {
    if (use_switch_port_) {
        return &switch_port_;
    }
    const auto idx = static_cast<int>(sst_cpu);
    if (idx >= 0 && idx < MAX_CXL_PORTS) {
        if (!core_port_connected_[static_cast<size_t>(idx)]) {
            throw std::runtime_error("CXLMemoryPool: response targets unconnected core port " +
                                     std::to_string(idx) + ". Check topology configuration.");
        }
        return &core_ports_[idx];
    }
    throw std::runtime_error("CXLMemoryPool: response targets out-of-range core port " +
                             std::to_string(idx) + ".");
}

bool CXLMemoryPool::try_send_response(const champsim::channel::response_type& response) {
    if (response.instr_depend_on_me.empty()) {
        return true;
    }

    auto tag = response.instr_depend_on_me.front();
    auto pending_it = pending_.find(tag);
    if (pending_it == pending_.end()) {
        return true;
    }
    const OutstandingRequest route = pending_it->second;

    auto* target_port = select_egress_port(route.sst_cpu);
    if (!target_port) {
        pending_.erase(pending_it);
        return true;
    }
    if (!target_port->can_send()) {
        return false;
    }

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

    auto* ev = convert_response_to_event(out);
    if (!target_port->send(ev)) {
        delete ev;
        return false;
    }
    pending_.erase(pending_it);
    return true;
}

void CXLMemoryPool::finish() {
    std::cout << "CXL pool " << pool_node_id_ << " utilization summary\n";
    std::cout << "  mem avg util: " << mem_ctrl_.queue_average_utilization(0) << '\n';
    const auto stats = request_link_stats();
    if (stats.avg_util > 0.0) {
        std::cout << "  req link avg util: " << stats.avg_util << '\n';
    }
    const auto now = std::chrono::steady_clock::now();
    const auto sec = std::chrono::duration<double>(now - wall_start_).count();
    std::cout << "  wall time (s): " << sec << '\n';
    if (active_calls_ > 0) {
        const auto active_sec = std::chrono::duration<double>(active_time_).count();
        std::cout << "Component Time Summary\n";
        std::cout << "  CXL pool active time (s): " << active_sec << '\n';
    }
    std::cout << std::flush;
}

} // namespace csimCore
} // namespace SST
