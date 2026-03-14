#include "cxl_mempool.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
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

double safe_stddev(long double sq_sum, long double sum, uint64_t count) {
    if (count == 0) {
        return 0.0;
    }
    const long double mean = sum / static_cast<long double>(count);
    const long double variance = std::max<long double>(sq_sum / static_cast<long double>(count) - mean * mean, 0.0L);
    return std::sqrt(static_cast<double>(variance));
}

template <typename T>
void record_distinct_count(T& stats, uint64_t count) {
    if (count == 0) {
        return;
    }
    stats.nonempty_cycles++;
    stats.sum += count;
    stats.max = std::max(stats.max, count);
}

std::size_t diag_class_index(access_type type) {
    switch (type) {
    case access_type::LOAD:
        return static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Load);
    case access_type::RFO:
        return static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Rfo);
    case access_type::WRITE:
        return static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Write);
    default:
        return static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Other);
    }
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
    std::array<uint64_t, MAX_CXL_PORTS> pending_counts{};
    std::array<uint64_t, MAX_CXL_PORTS> ready_counts{};
    std::array<std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount>, MAX_CXL_PORTS> pending_counts_by_class{};
    std::array<std::array<uint64_t, MY_MEMORY_CONTROLLER::kDiagClassCount>, MAX_CXL_PORTS> ready_counts_by_class{};
    uint64_t pending_distinct_src = 0;
    uint64_t ready_distinct_src = 0;
    for (const auto& [tag, req] : pending_) {
        if (req.src_node >= MAX_CXL_PORTS) {
            continue;
        }
        const auto src = static_cast<std::size_t>(req.src_node);
        const auto cls = diag_class_index(req.type);
        if (pending_counts[src]++ == 0) {
            pending_distinct_src++;
        }
        if (cls < MY_MEMORY_CONTROLLER::kDiagClassCount) {
            pending_counts_by_class[src][cls]++;
        }
        auto& diag = source_diag_[src];
        const uint64_t pending_age = tick_count_ >= req.enqueue_cycle ? (tick_count_ - req.enqueue_cycle) : 0;
        diag.pending_age_sum_cycles += pending_age;
        diag.pending_age_samples++;
        diag.pending_age_max_cycles = std::max(diag.pending_age_max_cycles, pending_age);
        if (req.response_ready_cycle != std::numeric_limits<uint64_t>::max()) {
            if (ready_counts[src]++ == 0) {
                ready_distinct_src++;
            }
            if (cls < MY_MEMORY_CONTROLLER::kDiagClassCount) {
                ready_counts_by_class[src][cls]++;
            }
            const uint64_t ready_age = tick_count_ >= req.response_ready_cycle
                ? (tick_count_ - req.response_ready_cycle)
                : 0;
            diag.ready_age_sum_cycles += ready_age;
            diag.ready_age_samples++;
            diag.ready_age_max_cycles = std::max(diag.ready_age_max_cycles, ready_age);
        }
    }
    record_distinct_count(pending_distinct_src_stats_, pending_distinct_src);
    record_distinct_count(ready_distinct_src_stats_, ready_distinct_src);
    for (std::size_t src = 0; src < MAX_CXL_PORTS; ++src) {
        auto& diag = source_diag_[src];
        diag.pending_occ_sum += pending_counts[src];
        diag.pending_occ_max = std::max(diag.pending_occ_max, pending_counts[src]);
        diag.ready_occ_sum += ready_counts[src];
        diag.ready_occ_max = std::max(diag.ready_occ_max, ready_counts[src]);
        for (std::size_t cls = 0; cls < MY_MEMORY_CONTROLLER::kDiagClassCount; ++cls) {
            diag.pending_occ_sum_by_class[cls] += pending_counts_by_class[src][cls];
            diag.pending_occ_max_by_class[cls] =
                std::max(diag.pending_occ_max_by_class[cls], pending_counts_by_class[src][cls]);
            diag.ready_occ_sum_by_class[cls] += ready_counts_by_class[src][cls];
            diag.ready_occ_max_by_class[cls] =
                std::max(diag.ready_occ_max_by_class[cls], ready_counts_by_class[src][cls]);
        }
    }
    const uint64_t returned_before = static_cast<uint64_t>(mem_channel_.returned.size());
    const uint64_t pending_before_send = static_cast<uint64_t>(pending_.size());
    returned_occ_stats_.samples++;
    returned_occ_stats_.sum += returned_before;
    returned_occ_stats_.sq_sum += static_cast<long double>(returned_before) * static_cast<long double>(returned_before);
    returned_occ_stats_.max = std::max(returned_occ_stats_.max, returned_before);
    if (returned_before > 0) {
        returned_burst_stats_.nonempty_cycles++;
        returned_burst_stats_.sum_pkts += returned_before;
        returned_burst_stats_.sq_sum_pkts += static_cast<long double>(returned_before) * static_cast<long double>(returned_before);
        returned_burst_stats_.max_pkts = std::max(returned_burst_stats_.max_pkts, returned_before);
    }
    pending_occ_stats_.samples++;
    pending_occ_stats_.sum += pending_before_send;
    pending_occ_stats_.sq_sum += static_cast<long double>(pending_before_send) * static_cast<long double>(pending_before_send);
    pending_occ_stats_.max = std::max<uint64_t>(pending_occ_stats_.max, pending_before_send);
    uint64_t sent_this_cycle = 0;
    std::array<bool, MAX_CXL_PORTS> ready_src_seen{};
    std::array<bool, MAX_CXL_PORTS> sent_dst_seen{};
    uint64_t ready_distinct_now = 0;
    uint64_t sent_distinct_dst = 0;
    for (const auto& response : mem_channel_.returned) {
        if (response.instr_depend_on_me.empty()) {
            continue;
        }
        const auto it = pending_.find(response.instr_depend_on_me.front());
        if (it == pending_.end()) {
            continue;
        }
        const auto src = it->second.src_node;
        if (src < MAX_CXL_PORTS && !ready_src_seen[src]) {
            ready_src_seen[src] = true;
            ready_distinct_now++;
        }
    }
    while (!mem_channel_.returned.empty()) {
        const auto& response = mem_channel_.returned.front();
        uint32_t response_dst = std::numeric_limits<uint32_t>::max();
        if (!response.instr_depend_on_me.empty()) {
            const auto it = pending_.find(response.instr_depend_on_me.front());
            if (it != pending_.end()) {
                response_dst = it->second.src_node;
            }
        }
        if (!try_send_response(response)) {
            break;
        }
        mem_channel_.returned.pop_front();
        ++total_completed_;
        ++sent_this_cycle;
        if (response_dst < MAX_CXL_PORTS && !sent_dst_seen[response_dst]) {
            sent_dst_seen[response_dst] = true;
            sent_distinct_dst++;
        }
    }
    record_distinct_count(response_ready_distinct_src_stats_, ready_distinct_now);
    record_distinct_count(response_sent_distinct_dst_stats_, sent_distinct_dst);
    if (sent_this_cycle > 0) {
        sent_burst_stats_.nonempty_cycles++;
        sent_burst_stats_.sum_pkts += sent_this_cycle;
        sent_burst_stats_.sq_sum_pkts += static_cast<long double>(sent_this_cycle) * static_cast<long double>(sent_this_cycle);
        sent_burst_stats_.max_pkts = std::max(sent_burst_stats_.max_pkts, sent_this_cycle);
    }
    if (!mem_channel_.returned.empty()) {
        response_blocked_cycles_++;
        response_blocked_sum_pkts_ += static_cast<uint64_t>(mem_channel_.returned.size());
        response_blocked_max_pkts_ = std::max<uint64_t>(response_blocked_max_pkts_, mem_channel_.returned.size());
    }
    if (pending_before_send > 0) {
        if (!pending_episode_active_) {
            pending_episode_state_ = PendingEpisodeState{};
            pending_episode_active_ = true;
        }
        pending_episode_state_.cycles++;
        pending_episode_state_.peak_occ = std::max(pending_episode_state_.peak_occ, pending_before_send);
        pending_episode_state_.accepted += request_accepted_this_tick_;
        pending_episode_state_.sent += sent_this_cycle;
        pending_episode_state_.max_pending_distinct_src =
            std::max(pending_episode_state_.max_pending_distinct_src, pending_distinct_src);
        pending_episode_state_.max_ready_distinct_src =
            std::max(pending_episode_state_.max_ready_distinct_src, ready_distinct_src);
        pending_episode_state_.max_sent_distinct_dst =
            std::max(pending_episode_state_.max_sent_distinct_dst, sent_distinct_dst);
        if (pending_.empty()) {
            pending_episode_stats_.count++;
            pending_episode_stats_.sum_cycles += pending_episode_state_.cycles;
            pending_episode_stats_.max_cycles =
                std::max(pending_episode_stats_.max_cycles, pending_episode_state_.cycles);
            pending_episode_stats_.sum_peak_occ += pending_episode_state_.peak_occ;
            pending_episode_stats_.max_peak_occ =
                std::max(pending_episode_stats_.max_peak_occ, pending_episode_state_.peak_occ);
            pending_episode_stats_.sum_accepted += pending_episode_state_.accepted;
            pending_episode_stats_.max_accepted =
                std::max(pending_episode_stats_.max_accepted, pending_episode_state_.accepted);
            pending_episode_stats_.sum_sent += pending_episode_state_.sent;
            pending_episode_stats_.max_sent =
                std::max(pending_episode_stats_.max_sent, pending_episode_state_.sent);
            pending_episode_stats_.sum_pending_distinct_src += pending_episode_state_.max_pending_distinct_src;
            pending_episode_stats_.max_pending_distinct_src =
                std::max(pending_episode_stats_.max_pending_distinct_src,
                         pending_episode_state_.max_pending_distinct_src);
            pending_episode_stats_.sum_ready_distinct_src += pending_episode_state_.max_ready_distinct_src;
            pending_episode_stats_.max_ready_distinct_src =
                std::max(pending_episode_stats_.max_ready_distinct_src,
                         pending_episode_state_.max_ready_distinct_src);
            pending_episode_stats_.sum_sent_distinct_dst += pending_episode_state_.max_sent_distinct_dst;
            pending_episode_stats_.max_sent_distinct_dst =
                std::max(pending_episode_stats_.max_sent_distinct_dst,
                         pending_episode_state_.max_sent_distinct_dst);
            pending_episode_state_ = PendingEpisodeState{};
            pending_episode_active_ = false;
        }
    }
    if (returned_before > 0) {
        if (!returned_episode_active_) {
            returned_episode_state_ = ReturnedEpisodeState{};
            returned_episode_active_ = true;
        }
        returned_episode_state_.cycles++;
        returned_episode_state_.peak_occ = std::max(returned_episode_state_.peak_occ, returned_before);
        returned_episode_state_.sent += sent_this_cycle;
        returned_episode_state_.max_ready_distinct_src =
            std::max(returned_episode_state_.max_ready_distinct_src, ready_distinct_now);
        returned_episode_state_.max_sent_distinct_dst =
            std::max(returned_episode_state_.max_sent_distinct_dst, sent_distinct_dst);
        if (mem_channel_.returned.empty()) {
            returned_episode_stats_.count++;
            returned_episode_stats_.sum_cycles += returned_episode_state_.cycles;
            returned_episode_stats_.max_cycles =
                std::max(returned_episode_stats_.max_cycles, returned_episode_state_.cycles);
            returned_episode_stats_.sum_peak_occ += returned_episode_state_.peak_occ;
            returned_episode_stats_.max_peak_occ =
                std::max(returned_episode_stats_.max_peak_occ, returned_episode_state_.peak_occ);
            returned_episode_stats_.sum_sent += returned_episode_state_.sent;
            returned_episode_stats_.max_sent =
                std::max(returned_episode_stats_.max_sent, returned_episode_state_.sent);
            returned_episode_stats_.sum_ready_distinct_src += returned_episode_state_.max_ready_distinct_src;
            returned_episode_stats_.max_ready_distinct_src =
                std::max(returned_episode_stats_.max_ready_distinct_src,
                         returned_episode_state_.max_ready_distinct_src);
            returned_episode_stats_.sum_sent_distinct_dst += returned_episode_state_.max_sent_distinct_dst;
            returned_episode_stats_.max_sent_distinct_dst =
                std::max(returned_episode_stats_.max_sent_distinct_dst,
                         returned_episode_state_.max_sent_distinct_dst);
            returned_episode_state_ = ReturnedEpisodeState{};
            returned_episode_active_ = false;
        }
    }
    if (heartbeat_period_ > 0 && (tick_count_ % heartbeat_period_ == 0)) {
        const auto prefix = std::string("stat.pool.") + std::to_string(pool_node_id_) + ".heartbeat.";
        std::cout << prefix << "cycle = " << tick_count_ << '\n';
        std::cout << prefix << "total_enqueued = " << total_enqueued_ << '\n';
        std::cout << prefix << "total_completed = " << total_completed_ << '\n';
        std::cout << prefix << "pending_responses = " << pending_.size() << '\n';
        std::cout << prefix << "mem_queue_occ = " << mem_ctrl_.queue_occupancy(0) << '\n';
        std::cout << prefix << "mem_queue_util = " << mem_ctrl_.queue_utilization(0) << '\n';
        const auto stats = request_link_stats();
        if (stats.occ > 0) {
            std::cout << prefix << "req_link_occ = " << stats.occ << '\n';
            std::cout << prefix << "req_link_util = " << stats.util << '\n';
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
        pending_[tag] = OutstandingRequest{
            request.cpu,
            request.sst_cpu,
            request.src_node,
            request.dst_node,
            request.type,
            tick_count_,
            std::numeric_limits<uint64_t>::max()
        };
    }

    if (request.src_node < MAX_CXL_PORTS) {
        const auto cls = diag_class_index(request.type);
        auto& diag = source_diag_[static_cast<std::size_t>(request.src_node)];
        diag.request_enqueued_by_class[cls]++;
        if (diag.saw_request_enqueue_by_class[cls] && tick_count_ >= diag.last_request_enqueue_cycle_by_class[cls]) {
            const uint64_t gap = tick_count_ - diag.last_request_enqueue_cycle_by_class[cls];
            auto& gap_stats = diag.request_enqueue_gap_by_class[cls];
            gap_stats.samples++;
            gap_stats.sum_cycles += gap;
            gap_stats.max_cycles = std::max(gap_stats.max_cycles, gap);
        }
        diag.saw_request_enqueue_by_class[cls] = true;
        diag.last_request_enqueue_cycle_by_class[cls] = tick_count_;
    }

    auto& pool_queue = mem_channel_.PQ;
    pool_queue.push_back(std::move(channel_req));
    ++total_enqueued_;
}

void CXLMemoryPool::poll_ports(uint64_t cycle) {
    std::array<bool, MAX_CXL_PORTS> accepted_src_seen{};
    std::array<bool, MAX_CXL_PORTS> blocked_src_seen{};
    uint64_t accepted_distinct_src = 0;
    uint64_t blocked_distinct_src = 0;
    uint64_t accepted_total = 0;
    uint64_t blocked_total = 0;
    auto classify_req_diag_from_event = [](const csEvent* ev) {
        if (!ev || ev->payload.size() <= 10) {
            return static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Other);
        }
        return diag_class_index(static_cast<access_type>(ev->payload[10]));
    };
    auto handle_event = [this, &accepted_src_seen, &blocked_src_seen, &accepted_distinct_src, classify_req_diag_from_event](csEvent* ev) {
        if (is_reset_event(*ev)) {
            reset_stats();
            delete ev;
            return true;
        }
        const auto cls_idx = classify_req_diag_from_event(ev);
        const uint32_t src = (!ev->payload.empty()) ? static_cast<uint32_t>(ev->payload[0]) : std::numeric_limits<uint32_t>::max();
        if (mem_channel_.pq_occupancy() >= mem_channel_.pq_size()) {
            if (src < MAX_CXL_PORTS) {
                blocked_src_seen[src] = true;
                source_diag_[static_cast<std::size_t>(src)].request_blocked_by_class[cls_idx]++;
            }
            blocked_total++;
            return false;
        }
        sst_request req = convert_event_to_request(*ev);
        if (req.src_node < MAX_CXL_PORTS) {
            if (!accepted_src_seen[req.src_node]) {
                accepted_src_seen[req.src_node] = true;
                accepted_distinct_src++;
            }
        }
        delete ev;
        enqueue_mem_request(req);
        accepted_total++;
        return true;
    };

    for_each_port([&](FabricPort& port) {
        port.tick(cycle);
        port.try_receive(cycle, handle_event);
    });
    for (std::size_t src = 0; src < MAX_CXL_PORTS; ++src) {
        if (blocked_src_seen[src]) {
            blocked_distinct_src++;
        }
    }
    record_distinct_count(request_accept_distinct_src_stats_, accepted_distinct_src);
    record_distinct_count(request_blocked_distinct_src_stats_, blocked_distinct_src);
    request_accepted_this_tick_ = accepted_total;
    request_blocked_this_tick_ = blocked_total;
}

CXLMemoryPool::LinkStats CXLMemoryPool::request_link_stats() const {
    LinkStats stats{};
    double util_sum = 0.0;
    double avg_sum = 0.0;
    double ingress_wait_avg_sum = 0.0;
    double egress_wait_avg_sum = 0.0;
    double ingress_queue_wait_avg_sum = 0.0;
    double ready_wait_avg_sum = 0.0;
    double ready_occ_avg_sum = 0.0;
    double ingress_arrival_burst_avg_pkts_sum = 0.0;
    double ingress_arrival_burst_avg_bytes_sum = 0.0;
    double ingress_arrival_burst_stddev_pkts_sum = 0.0;
    double ingress_arrival_burst_stddev_bytes_sum = 0.0;
    double ingress_arrival_nonempty_frac_sum = 0.0;
    double ingress_arrival_run_avg_cycles_sum = 0.0;
    double ingress_arrival_run_stddev_cycles_sum = 0.0;
    double ingress_arrival_gap_avg_cycles_sum = 0.0;
    double ingress_arrival_gap_stddev_cycles_sum = 0.0;
    double ingress_release_burst_avg_pkts_sum = 0.0;
    double ingress_release_burst_avg_bytes_sum = 0.0;
    double ingress_release_burst_stddev_pkts_sum = 0.0;
    double ingress_release_burst_stddev_bytes_sum = 0.0;
    double ingress_release_nonempty_frac_sum = 0.0;
    double ingress_release_run_avg_cycles_sum = 0.0;
    double ingress_release_run_stddev_cycles_sum = 0.0;
    double ingress_release_gap_avg_cycles_sum = 0.0;
    double ingress_release_gap_stddev_cycles_sum = 0.0;
    double ingress_occ_avg_bytes_sum = 0.0;
    double ingress_occ_stddev_bytes_sum = 0.0;
    double ingress_occ_nonempty_frac_sum = 0.0;
    double ingress_occ_run_avg_cycles_sum = 0.0;
    double ingress_occ_run_stddev_cycles_sum = 0.0;
    double ingress_occ_gap_avg_cycles_sum = 0.0;
    double ingress_occ_gap_stddev_cycles_sum = 0.0;
    std::size_t count = 0;
    std::size_t occ_total = 0;
    std::size_t ready_occ_total = 0;
    std::size_t ready_occ_max = 0;
    uint64_t ingress_wait_max = 0;
    uint64_t egress_wait_max = 0;
    uint64_t ingress_queue_wait_max = 0;
    uint64_t ready_wait_max = 0;
    uint64_t ready_retry_count = 0;
    uint64_t ingress_arrival_burst_max_pkts = 0;
    uint64_t ingress_arrival_burst_max_bytes = 0;
    uint64_t ingress_arrival_run_max_cycles = 0;
    uint64_t ingress_arrival_gap_max_cycles = 0;
    uint64_t ingress_release_burst_max_pkts = 0;
    uint64_t ingress_release_burst_max_bytes = 0;
    uint64_t ingress_release_run_max_cycles = 0;
    uint64_t ingress_release_gap_max_cycles = 0;
    uint64_t ingress_occ_max_bytes = 0;
    uint64_t ingress_occ_run_max_cycles = 0;
    uint64_t ingress_occ_gap_max_cycles = 0;
    std::array<uint64_t, static_cast<std::size_t>(FabricPort::TrafficClass::Count)> rx_bytes_by_class{};
    std::array<uint64_t, static_cast<std::size_t>(FabricPort::TrafficClass::Count)> tx_bytes_by_class{};
    std::array<uint64_t, static_cast<std::size_t>(FabricPort::TrafficClass::Count)> rx_packets_by_class{};
    std::array<uint64_t, static_cast<std::size_t>(FabricPort::TrafficClass::Count)> tx_packets_by_class{};

    auto accumulate = [&](const FabricPort& port) {
        util_sum += port.ingress_utilization();
        avg_sum += port.ingress_avg_utilization();
        ingress_wait_avg_sum += port.ingress_wait_avg_cycles();
        egress_wait_avg_sum += port.egress_wait_avg_cycles();
        ingress_wait_max = std::max(ingress_wait_max, port.ingress_wait_max_cycles());
        egress_wait_max = std::max(egress_wait_max, port.egress_wait_max_cycles());
        ingress_queue_wait_avg_sum += port.ingress_queue_wait_avg_cycles();
        ingress_queue_wait_max = std::max(ingress_queue_wait_max, port.ingress_queue_wait_max_cycles());
        occ_total += port.ingress_occupancy();
        ingress_arrival_burst_max_pkts = std::max(ingress_arrival_burst_max_pkts, port.ingress_arrival_burst_max_pkts());
        ingress_arrival_burst_max_bytes = std::max(ingress_arrival_burst_max_bytes, port.ingress_arrival_burst_max_bytes());
        ingress_arrival_burst_avg_pkts_sum += port.ingress_arrival_burst_avg_pkts();
        ingress_arrival_burst_avg_bytes_sum += port.ingress_arrival_burst_avg_bytes();
        ingress_arrival_burst_stddev_pkts_sum += port.ingress_arrival_burst_stddev_pkts();
        ingress_arrival_burst_stddev_bytes_sum += port.ingress_arrival_burst_stddev_bytes();
        ingress_arrival_nonempty_frac_sum += port.ingress_arrival_nonempty_frac();
        ingress_arrival_run_max_cycles = std::max(ingress_arrival_run_max_cycles, port.ingress_arrival_run_max_cycles());
        ingress_arrival_run_avg_cycles_sum += port.ingress_arrival_run_avg_cycles();
        ingress_arrival_run_stddev_cycles_sum += port.ingress_arrival_run_stddev_cycles();
        ingress_arrival_gap_max_cycles = std::max(ingress_arrival_gap_max_cycles, port.ingress_arrival_gap_max_cycles());
        ingress_arrival_gap_avg_cycles_sum += port.ingress_arrival_gap_avg_cycles();
        ingress_arrival_gap_stddev_cycles_sum += port.ingress_arrival_gap_stddev_cycles();
        ingress_release_burst_max_pkts = std::max(ingress_release_burst_max_pkts, port.ingress_release_burst_max_pkts());
        ingress_release_burst_max_bytes = std::max(ingress_release_burst_max_bytes, port.ingress_release_burst_max_bytes());
        ingress_release_burst_avg_pkts_sum += port.ingress_release_burst_avg_pkts();
        ingress_release_burst_avg_bytes_sum += port.ingress_release_burst_avg_bytes();
        ingress_release_burst_stddev_pkts_sum += port.ingress_release_burst_stddev_pkts();
        ingress_release_burst_stddev_bytes_sum += port.ingress_release_burst_stddev_bytes();
        ingress_release_nonempty_frac_sum += port.ingress_release_nonempty_frac();
        ingress_release_run_max_cycles = std::max(ingress_release_run_max_cycles, port.ingress_release_run_max_cycles());
        ingress_release_run_avg_cycles_sum += port.ingress_release_run_avg_cycles();
        ingress_release_run_stddev_cycles_sum += port.ingress_release_run_stddev_cycles();
        ingress_release_gap_max_cycles = std::max(ingress_release_gap_max_cycles, port.ingress_release_gap_max_cycles());
        ingress_release_gap_avg_cycles_sum += port.ingress_release_gap_avg_cycles();
        ingress_release_gap_stddev_cycles_sum += port.ingress_release_gap_stddev_cycles();
        ingress_occ_avg_bytes_sum += port.ingress_occ_avg_bytes();
        ingress_occ_stddev_bytes_sum += port.ingress_occ_stddev_bytes();
        ingress_occ_max_bytes = std::max(ingress_occ_max_bytes, port.ingress_occ_max_bytes());
        ingress_occ_nonempty_frac_sum += port.ingress_occ_nonempty_frac();
        ingress_occ_run_max_cycles = std::max(ingress_occ_run_max_cycles, port.ingress_occ_run_max_cycles());
        ingress_occ_run_avg_cycles_sum += port.ingress_occ_run_avg_cycles();
        ingress_occ_run_stddev_cycles_sum += port.ingress_occ_run_stddev_cycles();
        ingress_occ_gap_max_cycles = std::max(ingress_occ_gap_max_cycles, port.ingress_occ_gap_max_cycles());
        ingress_occ_gap_avg_cycles_sum += port.ingress_occ_gap_avg_cycles();
        ingress_occ_gap_stddev_cycles_sum += port.ingress_occ_gap_stddev_cycles();
        ready_wait_avg_sum += port.ready_wait_avg_cycles();
        ready_wait_max = std::max(ready_wait_max, port.ready_wait_max_cycles());
        ready_occ_avg_sum += port.ready_occupancy_avg();
        ready_occ_total += port.ready_occupancy();
        ready_occ_max = std::max(ready_occ_max, port.ready_occupancy_max());
        ready_retry_count += port.ready_retry_count();
        for (std::size_t i = 0; i < static_cast<std::size_t>(FabricPort::TrafficClass::Count); ++i) {
            const auto cls = static_cast<FabricPort::TrafficClass>(i);
            rx_bytes_by_class[i] += port.rx_bytes(cls);
            tx_bytes_by_class[i] += port.tx_bytes(cls);
            rx_packets_by_class[i] += port.rx_packets(cls);
            tx_packets_by_class[i] += port.tx_packets(cls);
        }
        ++count;
    };

    for_each_port([&](const FabricPort& port) { accumulate(port); });

    stats.occ = occ_total;
    if (count > 0) {
        stats.util = util_sum / static_cast<double>(count);
        stats.avg_util = avg_sum / static_cast<double>(count);
        stats.ingress_wait_avg_cycles = ingress_wait_avg_sum / static_cast<double>(count);
        stats.egress_wait_avg_cycles = egress_wait_avg_sum / static_cast<double>(count);
        stats.ingress_queue_wait_avg_cycles = ingress_queue_wait_avg_sum / static_cast<double>(count);
        stats.ingress_arrival_burst_avg_pkts = ingress_arrival_burst_avg_pkts_sum / static_cast<double>(count);
        stats.ingress_arrival_burst_avg_bytes = ingress_arrival_burst_avg_bytes_sum / static_cast<double>(count);
        stats.ingress_arrival_burst_stddev_pkts = ingress_arrival_burst_stddev_pkts_sum / static_cast<double>(count);
        stats.ingress_arrival_burst_stddev_bytes = ingress_arrival_burst_stddev_bytes_sum / static_cast<double>(count);
        stats.ingress_arrival_nonempty_frac = ingress_arrival_nonempty_frac_sum / static_cast<double>(count);
        stats.ingress_arrival_run_avg_cycles = ingress_arrival_run_avg_cycles_sum / static_cast<double>(count);
        stats.ingress_arrival_run_stddev_cycles = ingress_arrival_run_stddev_cycles_sum / static_cast<double>(count);
        stats.ingress_arrival_gap_avg_cycles = ingress_arrival_gap_avg_cycles_sum / static_cast<double>(count);
        stats.ingress_arrival_gap_stddev_cycles = ingress_arrival_gap_stddev_cycles_sum / static_cast<double>(count);
        stats.ingress_release_burst_avg_pkts = ingress_release_burst_avg_pkts_sum / static_cast<double>(count);
        stats.ingress_release_burst_avg_bytes = ingress_release_burst_avg_bytes_sum / static_cast<double>(count);
        stats.ingress_release_burst_stddev_pkts = ingress_release_burst_stddev_pkts_sum / static_cast<double>(count);
        stats.ingress_release_burst_stddev_bytes = ingress_release_burst_stddev_bytes_sum / static_cast<double>(count);
        stats.ingress_release_nonempty_frac = ingress_release_nonempty_frac_sum / static_cast<double>(count);
        stats.ingress_release_run_avg_cycles = ingress_release_run_avg_cycles_sum / static_cast<double>(count);
        stats.ingress_release_run_stddev_cycles = ingress_release_run_stddev_cycles_sum / static_cast<double>(count);
        stats.ingress_release_gap_avg_cycles = ingress_release_gap_avg_cycles_sum / static_cast<double>(count);
        stats.ingress_release_gap_stddev_cycles = ingress_release_gap_stddev_cycles_sum / static_cast<double>(count);
        stats.ingress_occ_avg_bytes = ingress_occ_avg_bytes_sum / static_cast<double>(count);
        stats.ingress_occ_stddev_bytes = ingress_occ_stddev_bytes_sum / static_cast<double>(count);
        stats.ingress_occ_nonempty_frac = ingress_occ_nonempty_frac_sum / static_cast<double>(count);
        stats.ingress_occ_run_avg_cycles = ingress_occ_run_avg_cycles_sum / static_cast<double>(count);
        stats.ingress_occ_run_stddev_cycles = ingress_occ_run_stddev_cycles_sum / static_cast<double>(count);
        stats.ingress_occ_gap_avg_cycles = ingress_occ_gap_avg_cycles_sum / static_cast<double>(count);
        stats.ingress_occ_gap_stddev_cycles = ingress_occ_gap_stddev_cycles_sum / static_cast<double>(count);
        stats.ready_wait_avg_cycles = ready_wait_avg_sum / static_cast<double>(count);
        stats.ready_occ_avg = ready_occ_avg_sum / static_cast<double>(count);
    }
    stats.ingress_wait_max_cycles = ingress_wait_max;
    stats.egress_wait_max_cycles = egress_wait_max;
    stats.ingress_queue_wait_max_cycles = ingress_queue_wait_max;
    stats.ingress_arrival_burst_max_pkts = ingress_arrival_burst_max_pkts;
    stats.ingress_arrival_burst_max_bytes = ingress_arrival_burst_max_bytes;
    stats.ingress_arrival_run_max_cycles = ingress_arrival_run_max_cycles;
    stats.ingress_arrival_gap_max_cycles = ingress_arrival_gap_max_cycles;
    stats.ingress_release_burst_max_pkts = ingress_release_burst_max_pkts;
    stats.ingress_release_burst_max_bytes = ingress_release_burst_max_bytes;
    stats.ingress_release_run_max_cycles = ingress_release_run_max_cycles;
    stats.ingress_release_gap_max_cycles = ingress_release_gap_max_cycles;
    stats.ingress_occ_max_bytes = ingress_occ_max_bytes;
    stats.ingress_occ_run_max_cycles = ingress_occ_run_max_cycles;
    stats.ingress_occ_gap_max_cycles = ingress_occ_gap_max_cycles;
    stats.ready_wait_max_cycles = ready_wait_max;
    stats.ready_occ = ready_occ_total;
    stats.ready_occ_max = ready_occ_max;
    stats.ready_retry_count = ready_retry_count;
    stats.rx_bytes_by_class = rx_bytes_by_class;
    stats.tx_bytes_by_class = tx_bytes_by_class;
    stats.rx_packets_by_class = rx_packets_by_class;
    stats.tx_packets_by_class = tx_packets_by_class;
    return stats;
}

void CXLMemoryPool::reset_stats() {
    mem_ctrl_.reset_utilization();
    mem_ctrl_.reset_diagnostics();
    response_wait_stats_.fill(ResponseWaitStats{});
    returned_burst_stats_ = CycleBurstStats{};
    sent_burst_stats_ = CycleBurstStats{};
    response_blocked_cycles_ = 0;
    response_blocked_sum_pkts_ = 0;
    response_blocked_max_pkts_ = 0;
    returned_occ_stats_ = QueueOccStats{};
    pending_occ_stats_ = QueueOccStats{};
    source_diag_.fill(SourceDiag{});
    request_accept_distinct_src_stats_ = DistinctCountStats{};
    request_blocked_distinct_src_stats_ = DistinctCountStats{};
    pending_distinct_src_stats_ = DistinctCountStats{};
    ready_distinct_src_stats_ = DistinctCountStats{};
    response_ready_distinct_src_stats_ = DistinctCountStats{};
    response_sent_distinct_dst_stats_ = DistinctCountStats{};
    pending_episode_stats_ = PendingEpisodeStats{};
    pending_episode_state_ = PendingEpisodeState{};
    returned_episode_stats_ = ReturnedEpisodeStats{};
    returned_episode_state_ = ReturnedEpisodeState{};
    pending_episode_active_ = false;
    returned_episode_active_ = false;
    request_accepted_this_tick_ = 0;
    request_blocked_this_tick_ = 0;
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
    auto& pending = pending_it->second;
    if (pending.response_ready_cycle == std::numeric_limits<uint64_t>::max()) {
        pending.response_ready_cycle = tick_count_;
        if (pending.src_node < MAX_CXL_PORTS) {
            auto& diag = source_diag_[static_cast<std::size_t>(pending.src_node)];
            diag.response_ready_by_class[diag_class_index(pending.type)]++;
            const uint64_t mem_ready_cycles = tick_count_ >= pending.enqueue_cycle
                ? (tick_count_ - pending.enqueue_cycle)
                : 0;
            const auto cls = diag_class_index(pending.type);
            if (diag.saw_response_ready_by_class[cls] &&
                tick_count_ >= diag.last_response_ready_cycle_by_class[cls]) {
                const uint64_t gap = tick_count_ - diag.last_response_ready_cycle_by_class[cls];
                auto& gap_stats = diag.response_ready_gap_by_class[cls];
                gap_stats.samples++;
                gap_stats.sum_cycles += gap;
                gap_stats.max_cycles = std::max(gap_stats.max_cycles, gap);
            }
            diag.saw_response_ready_by_class[cls] = true;
            diag.last_response_ready_cycle_by_class[cls] = tick_count_;
            diag.mem_ready_sum_cycles_by_class[cls] += mem_ready_cycles;
            diag.mem_ready_samples_by_class[cls]++;
            diag.mem_ready_max_cycles_by_class[cls] =
                std::max(diag.mem_ready_max_cycles_by_class[cls], mem_ready_cycles);
        }
    }
    const OutstandingRequest route = pending;
    std::size_t cls_idx = static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Other);
    switch (route.type) {
    case access_type::LOAD:
        cls_idx = static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Load);
        break;
    case access_type::RFO:
        cls_idx = static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Rfo);
        break;
    case access_type::WRITE:
        cls_idx = static_cast<std::size_t>(MY_MEMORY_CONTROLLER::RequestDiagClass::Write);
        break;
    default:
        break;
    }
    auto& resp_stats = response_wait_stats_[cls_idx];
    resp_stats.attempted++;

    auto* target_port = select_egress_port(route.sst_cpu);
    if (!target_port) {
        pending_.erase(pending_it);
        return true;
    }
    if (!target_port->can_send(64)) {
        resp_stats.can_send_blocked++;
        if (route.src_node < MAX_CXL_PORTS) {
            source_diag_[static_cast<std::size_t>(route.src_node)].response_blocked_by_class[cls_idx]++;
        }
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
        resp_stats.send_failed++;
        if (route.src_node < MAX_CXL_PORTS) {
            source_diag_[static_cast<std::size_t>(route.src_node)].response_blocked_by_class[cls_idx]++;
        }
        delete ev;
        return false;
    }
    const uint64_t response_wait =
        (route.response_ready_cycle != std::numeric_limits<uint64_t>::max() && tick_count_ >= route.response_ready_cycle)
            ? (tick_count_ - route.response_ready_cycle)
            : 0;
    const uint64_t total_turnaround =
        tick_count_ >= route.enqueue_cycle ? (tick_count_ - route.enqueue_cycle) : 0;
    resp_stats.completed++;
    resp_stats.wait_sum_cycles += response_wait;
    resp_stats.wait_max_cycles = std::max(resp_stats.wait_max_cycles, response_wait);
    if (route.src_node < MAX_CXL_PORTS) {
        auto& diag = source_diag_[static_cast<std::size_t>(route.src_node)];
        diag.response_sent_by_class[cls_idx]++;
        if (diag.saw_response_sent_by_class[cls_idx] &&
            tick_count_ >= diag.last_response_sent_cycle_by_class[cls_idx]) {
            const uint64_t gap = tick_count_ - diag.last_response_sent_cycle_by_class[cls_idx];
            auto& gap_stats = diag.response_sent_gap_by_class[cls_idx];
            gap_stats.samples++;
            gap_stats.sum_cycles += gap;
            gap_stats.max_cycles = std::max(gap_stats.max_cycles, gap);
        }
        diag.saw_response_sent_by_class[cls_idx] = true;
        diag.last_response_sent_cycle_by_class[cls_idx] = tick_count_;
        diag.response_wait_sum_cycles_by_class[cls_idx] += response_wait;
        diag.response_wait_samples_by_class[cls_idx]++;
        diag.response_wait_max_cycles_by_class[cls_idx] =
            std::max(diag.response_wait_max_cycles_by_class[cls_idx], response_wait);
        diag.total_turnaround_sum_cycles_by_class[cls_idx] += total_turnaround;
        diag.total_turnaround_samples_by_class[cls_idx]++;
        diag.total_turnaround_max_cycles_by_class[cls_idx] =
            std::max(diag.total_turnaround_max_cycles_by_class[cls_idx], total_turnaround);
    }
    pending_.erase(pending_it);
    return true;
}

void CXLMemoryPool::finish() {
    const auto now = std::chrono::steady_clock::now();
    const auto sec = std::chrono::duration<double>(now - wall_start_).count();
    const auto stats = request_link_stats();

    if (lightweight_output_) {
        const auto prefix = std::string("stat.pool.") + std::to_string(pool_node_id_) + ".";
        static constexpr std::array<std::pair<FabricPort::TrafficClass, const char*>, 4> kTrafficClasses{{
            {FabricPort::TrafficClass::DemandReq, "demand_req"},
            {FabricPort::TrafficClass::WriteReq, "write_req"},
            {FabricPort::TrafficClass::Response, "response"},
            {FabricPort::TrafficClass::OtherReq, "other_req"},
        }};
        const auto demand_diag = mem_ctrl_.demand_diag_stats();
        const auto& queue_diag = mem_ctrl_.queue_diag_stats();
        std::cout << prefix << "util.mem_avg = " << mem_ctrl_.queue_average_utilization(0) << '\n';
        if (stats.avg_util > 0.0) {
            std::cout << prefix << "util.req_link_avg = " << stats.avg_util << '\n';
        }
        auto print_diag = [&](const std::string& name, const MY_MEMORY_CONTROLLER::RequestDiagStats& diag) {
            const double denom = diag.completed > 0 ? static_cast<double>(diag.completed) : 1.0;
            std::cout << prefix << "memq." << name << ".completed = " << diag.completed << '\n';
            std::cout << prefix << "memq." << name << ".response_completed = " << diag.response_completed << '\n';
            std::cout << prefix << "memq." << name << ".queue_wait_avg_cycles = "
                      << (diag.completed > 0 ? static_cast<double>(diag.queue_wait_sum_cycles) / denom : 0.0) << '\n';
            std::cout << prefix << "memq." << name << ".queue_wait_max_cycles = " << diag.queue_wait_max_cycles << '\n';
            std::cout << prefix << "memq." << name << ".service_avg_cycles = "
                      << (diag.completed > 0 ? static_cast<double>(diag.service_sum_cycles) / denom : 0.0) << '\n';
            std::cout << prefix << "memq." << name << ".service_max_cycles = " << diag.service_max_cycles << '\n';
            std::cout << prefix << "memq." << name << ".total_avg_cycles = "
                      << (diag.completed > 0 ? static_cast<double>(diag.total_sum_cycles) / denom : 0.0) << '\n';
            std::cout << prefix << "memq." << name << ".total_max_cycles = " << diag.total_max_cycles << '\n';
            static constexpr std::array<const char*, MY_MEMORY_CONTROLLER::kDiagClassCount> kAheadNames = {"load", "rfo", "write", "other"};
            for (std::size_t i = 0; i < kAheadNames.size(); ++i) {
                std::cout << prefix << "memq." << name << ".ahead_pkts_avg." << kAheadNames[i] << " = "
                          << (diag.completed > 0 ? static_cast<double>(diag.ahead_pkts_sum[i]) / denom : 0.0) << '\n';
                std::cout << prefix << "memq." << name << ".ahead_pkts_max." << kAheadNames[i] << " = "
                          << diag.ahead_pkts_max[i] << '\n';
                std::cout << prefix << "memq." << name << ".ahead_bytes_avg." << kAheadNames[i] << " = "
                          << (diag.completed > 0 ? static_cast<double>(diag.ahead_bytes_sum[i]) / denom : 0.0) << '\n';
                std::cout << prefix << "memq." << name << ".ahead_bytes_max." << kAheadNames[i] << " = "
                          << diag.ahead_bytes_max[i] << '\n';
            }
            for (std::size_t i = 0; i < MY_MEMORY_CONTROLLER::kDiagBucketCount; ++i) {
                std::cout << prefix << "memq." << name << ".queue_wait_bucket."
                          << MY_MEMORY_CONTROLLER::cycle_bucket_name(i) << " = "
                          << diag.queue_wait_bucket_counts[i] << '\n';
                std::cout << prefix << "memq." << name << ".service_bucket."
                          << MY_MEMORY_CONTROLLER::cycle_bucket_name(i) << " = "
                          << diag.service_bucket_counts[i] << '\n';
                std::cout << prefix << "memq." << name << ".total_bucket."
                          << MY_MEMORY_CONTROLLER::cycle_bucket_name(i) << " = "
                          << diag.total_bucket_counts[i] << '\n';
                std::cout << prefix << "memq." << name << ".ahead_total_pkts_bucket."
                          << MY_MEMORY_CONTROLLER::count_bucket_name(i) << " = "
                          << diag.ahead_total_pkts_bucket_counts[i] << '\n';
                std::cout << prefix << "memq." << name << ".ahead_total_bytes_bucket."
                          << MY_MEMORY_CONTROLLER::byte_bucket_name(i) << " = "
                          << diag.ahead_total_bytes_bucket_counts[i] << '\n';
            }
        };
        print_diag("demand", demand_diag);
        static constexpr std::array<const char*, MY_MEMORY_CONTROLLER::kDiagClassCount> kDiagNames = {"load", "rfo", "write", "other"};
        std::cout << prefix << "memq.queue_occ_avg_bytes = "
                  << (queue_diag.occ_samples > 0 ? static_cast<double>(queue_diag.occ_sum_bytes) / static_cast<double>(queue_diag.occ_samples) : 0.0)
                  << '\n';
        std::cout << prefix << "memq.queue_occ_stddev_bytes = "
                  << safe_stddev(queue_diag.occ_sq_sum_bytes,
                                 static_cast<long double>(queue_diag.occ_sum_bytes),
                                 queue_diag.occ_samples)
                  << '\n';
        std::cout << prefix << "memq.queue_occ_max_bytes = " << queue_diag.occ_max_bytes << '\n';
        std::cout << prefix << "memq.queue_occ_nonempty_frac = "
                  << (queue_diag.occ_samples > 0 ? static_cast<double>(queue_diag.occ_nonempty_cycles) / static_cast<double>(queue_diag.occ_samples) : 0.0)
                  << '\n';
        std::cout << prefix << "memq.enqueue_burst_avg_pkts = "
                  << (queue_diag.enqueue_burst_nonempty_cycles > 0
                          ? static_cast<double>(queue_diag.enqueue_burst_sum_pkts) /
                                static_cast<double>(queue_diag.enqueue_burst_nonempty_cycles)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "memq.enqueue_burst_stddev_pkts = "
                  << safe_stddev(queue_diag.enqueue_burst_sq_sum_pkts,
                                 static_cast<long double>(queue_diag.enqueue_burst_sum_pkts),
                                 queue_diag.enqueue_burst_nonempty_cycles)
                  << '\n';
        std::cout << prefix << "memq.enqueue_burst_max_pkts = " << queue_diag.enqueue_burst_max_pkts << '\n';
        std::cout << prefix << "memq.complete_burst_avg_pkts = "
                  << (queue_diag.complete_burst_nonempty_cycles > 0
                          ? static_cast<double>(queue_diag.complete_burst_sum_pkts) /
                                static_cast<double>(queue_diag.complete_burst_nonempty_cycles)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "memq.complete_burst_stddev_pkts = "
                  << safe_stddev(queue_diag.complete_burst_sq_sum_pkts,
                                 static_cast<long double>(queue_diag.complete_burst_sum_pkts),
                                 queue_diag.complete_burst_nonempty_cycles)
                  << '\n';
        std::cout << prefix << "memq.complete_burst_max_pkts = " << queue_diag.complete_burst_max_pkts << '\n';
        std::cout << prefix << "memq.queue_episode.count = " << queue_diag.episode_count << '\n';
        std::cout << prefix << "memq.queue_episode.avg_cycles = "
                  << (queue_diag.episode_count > 0
                          ? static_cast<double>(queue_diag.episode_sum_cycles) /
                                static_cast<double>(queue_diag.episode_count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "memq.queue_episode.max_cycles = " << queue_diag.episode_max_cycles << '\n';
        std::cout << prefix << "memq.queue_episode.avg_peak_occ_bytes = "
                  << (queue_diag.episode_count > 0
                          ? static_cast<double>(queue_diag.episode_sum_peak_occ_bytes) /
                                static_cast<double>(queue_diag.episode_count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "memq.queue_episode.max_peak_occ_bytes = "
                  << queue_diag.episode_max_peak_occ_bytes << '\n';
        std::cout << prefix << "memq.queue_episode.avg_enqueue_pkts = "
                  << (queue_diag.episode_count > 0
                          ? static_cast<double>(queue_diag.episode_sum_enqueue_pkts) /
                                static_cast<double>(queue_diag.episode_count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "memq.queue_episode.max_enqueue_pkts = "
                  << queue_diag.episode_max_enqueue_pkts << '\n';
        std::cout << prefix << "memq.queue_episode.avg_complete_pkts = "
                  << (queue_diag.episode_count > 0
                          ? static_cast<double>(queue_diag.episode_sum_complete_pkts) /
                                static_cast<double>(queue_diag.episode_count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "memq.queue_episode.max_complete_pkts = "
                  << queue_diag.episode_max_complete_pkts << '\n';
        for (std::size_t i = 0; i < MY_MEMORY_CONTROLLER::kDiagBucketCount; ++i) {
            std::cout << prefix << "memq.queue_occ_bucket."
                      << MY_MEMORY_CONTROLLER::byte_bucket_name(i) << " = "
                      << queue_diag.occ_bucket_counts[i] << '\n';
            std::cout << prefix << "memq.enqueue_burst_bucket."
                      << MY_MEMORY_CONTROLLER::count_bucket_name(i) << " = "
                      << queue_diag.enqueue_burst_bucket_counts[i] << '\n';
            std::cout << prefix << "memq.complete_burst_bucket."
                      << MY_MEMORY_CONTROLLER::count_bucket_name(i) << " = "
                      << queue_diag.complete_burst_bucket_counts[i] << '\n';
        }
        for (std::size_t i = 0; i < kDiagNames.size(); ++i) {
            std::cout << prefix << "memq.queue_occ_avg_bytes." << kDiagNames[i] << " = "
                      << (queue_diag.occ_samples > 0
                              ? static_cast<double>(queue_diag.occ_sum_bytes_by_class[i]) / static_cast<double>(queue_diag.occ_samples)
                              : 0.0)
                      << '\n';
            std::cout << prefix << "memq.queue_occ_max_bytes." << kDiagNames[i] << " = "
                      << queue_diag.occ_max_bytes_by_class[i] << '\n';
            std::cout << prefix << "memq.queue_occ_avg_pkts." << kDiagNames[i] << " = "
                      << (queue_diag.occ_samples > 0
                              ? static_cast<double>(queue_diag.occ_sum_pkts_by_class[i]) / static_cast<double>(queue_diag.occ_samples)
                              : 0.0)
                      << '\n';
            std::cout << prefix << "memq.queue_occ_max_pkts." << kDiagNames[i] << " = "
                      << queue_diag.occ_max_pkts_by_class[i] << '\n';
            std::cout << prefix << "memq.enqueue_burst_avg_pkts." << kDiagNames[i] << " = "
                      << (queue_diag.enqueue_burst_nonempty_cycles > 0
                              ? static_cast<double>(queue_diag.enqueue_burst_sum_pkts_by_class[i]) /
                                    static_cast<double>(queue_diag.enqueue_burst_nonempty_cycles)
                              : 0.0)
                      << '\n';
            std::cout << prefix << "memq.enqueue_burst_max_pkts." << kDiagNames[i] << " = "
                      << queue_diag.enqueue_burst_max_pkts_by_class[i] << '\n';
            std::cout << prefix << "memq.complete_burst_avg_pkts." << kDiagNames[i] << " = "
                      << (queue_diag.complete_burst_nonempty_cycles > 0
                              ? static_cast<double>(queue_diag.complete_burst_sum_pkts_by_class[i]) /
                                    static_cast<double>(queue_diag.complete_burst_nonempty_cycles)
                              : 0.0)
                      << '\n';
            std::cout << prefix << "memq.complete_burst_max_pkts." << kDiagNames[i] << " = "
                      << queue_diag.complete_burst_max_pkts_by_class[i] << '\n';
        }
        for (std::size_t i = 0; i < MY_MEMORY_CONTROLLER::kDiagClassCount; ++i) {
            const auto cls = static_cast<MY_MEMORY_CONTROLLER::RequestDiagClass>(i);
            print_diag(MY_MEMORY_CONTROLLER::request_diag_class_name(cls), mem_ctrl_.request_diag_stats(cls));
            const auto& resp_stats = response_wait_stats_[i];
            const double resp_denom = resp_stats.completed > 0 ? static_cast<double>(resp_stats.completed) : 1.0;
            const auto resp_name = MY_MEMORY_CONTROLLER::request_diag_class_name(cls);
            std::cout << prefix << "respq." << resp_name << ".attempted = " << resp_stats.attempted << '\n';
            std::cout << prefix << "respq." << MY_MEMORY_CONTROLLER::request_diag_class_name(cls) << ".completed = "
                      << resp_stats.completed << '\n';
            std::cout << prefix << "respq." << resp_name << ".can_send_blocked = " << resp_stats.can_send_blocked << '\n';
            std::cout << prefix << "respq." << resp_name << ".send_failed = " << resp_stats.send_failed << '\n';
            std::cout << prefix << "respq." << MY_MEMORY_CONTROLLER::request_diag_class_name(cls) << ".wait_avg_cycles = "
                      << (resp_stats.completed > 0 ? static_cast<double>(resp_stats.wait_sum_cycles) / resp_denom : 0.0) << '\n';
            std::cout << prefix << "respq." << MY_MEMORY_CONTROLLER::request_diag_class_name(cls) << ".wait_max_cycles = "
                      << resp_stats.wait_max_cycles << '\n';
        }
        const double returned_burst_avg =
            returned_burst_stats_.nonempty_cycles > 0
                ? static_cast<double>(returned_burst_stats_.sum_pkts) / static_cast<double>(returned_burst_stats_.nonempty_cycles)
                : 0.0;
        const double returned_burst_stddev =
            safe_stddev(returned_burst_stats_.sq_sum_pkts,
                        static_cast<long double>(returned_burst_stats_.sum_pkts),
                        returned_burst_stats_.nonempty_cycles);
        const double sent_burst_avg =
            sent_burst_stats_.nonempty_cycles > 0
                ? static_cast<double>(sent_burst_stats_.sum_pkts) / static_cast<double>(sent_burst_stats_.nonempty_cycles)
                : 0.0;
        const double sent_burst_stddev =
            safe_stddev(sent_burst_stats_.sq_sum_pkts,
                        static_cast<long double>(sent_burst_stats_.sum_pkts),
                        sent_burst_stats_.nonempty_cycles);
        const double returned_occ_avg =
            returned_occ_stats_.samples > 0
                ? static_cast<double>(returned_occ_stats_.sum) / static_cast<double>(returned_occ_stats_.samples)
                : 0.0;
        const double returned_occ_stddev =
            safe_stddev(returned_occ_stats_.sq_sum,
                        static_cast<long double>(returned_occ_stats_.sum),
                        returned_occ_stats_.samples);
        const double pending_occ_avg =
            pending_occ_stats_.samples > 0
                ? static_cast<double>(pending_occ_stats_.sum) / static_cast<double>(pending_occ_stats_.samples)
                : 0.0;
        const double pending_occ_stddev =
            safe_stddev(pending_occ_stats_.sq_sum,
                        static_cast<long double>(pending_occ_stats_.sum),
                        pending_occ_stats_.samples);
        std::cout << prefix << "response.returned_burst_max_pkts = " << returned_burst_stats_.max_pkts << '\n';
        std::cout << prefix << "response.returned_burst_avg_pkts = " << returned_burst_avg << '\n';
        std::cout << prefix << "response.returned_burst_stddev_pkts = " << returned_burst_stddev << '\n';
        std::cout << prefix << "response.returned_nonempty_frac = "
                  << (tick_count_ > 0 ? static_cast<double>(returned_burst_stats_.nonempty_cycles) / static_cast<double>(tick_count_) : 0.0)
                  << '\n';
        std::cout << prefix << "response.sent_burst_max_pkts = " << sent_burst_stats_.max_pkts << '\n';
        std::cout << prefix << "response.sent_burst_avg_pkts = " << sent_burst_avg << '\n';
        std::cout << prefix << "response.sent_burst_stddev_pkts = " << sent_burst_stddev << '\n';
        std::cout << prefix << "response.sent_nonempty_frac = "
                  << (tick_count_ > 0 ? static_cast<double>(sent_burst_stats_.nonempty_cycles) / static_cast<double>(tick_count_) : 0.0)
                  << '\n';
        std::cout << prefix << "response.blocked_cycles = " << response_blocked_cycles_ << '\n';
        std::cout << prefix << "response.blocked_avg_pending_pkts = "
                  << (response_blocked_cycles_ > 0 ? static_cast<double>(response_blocked_sum_pkts_) / static_cast<double>(response_blocked_cycles_) : 0.0)
                  << '\n';
        std::cout << prefix << "response.blocked_max_pending_pkts = " << response_blocked_max_pkts_ << '\n';
        std::cout << prefix << "response.returned_occ_avg_pkts = " << returned_occ_avg << '\n';
        std::cout << prefix << "response.returned_occ_stddev_pkts = " << returned_occ_stddev << '\n';
        std::cout << prefix << "response.returned_occ_max_pkts = " << returned_occ_stats_.max << '\n';
        std::cout << prefix << "response.pending_occ_avg = " << pending_occ_avg << '\n';
        std::cout << prefix << "response.pending_occ_stddev = " << pending_occ_stddev << '\n';
        std::cout << prefix << "response.pending_occ_max = " << pending_occ_stats_.max << '\n';
        auto print_distinct = [&](const std::string& name, const DistinctCountStats& stats) {
            std::cout << prefix << name << ".distinct_avg = "
                      << (stats.nonempty_cycles > 0
                              ? static_cast<double>(stats.sum) / static_cast<double>(stats.nonempty_cycles)
                              : 0.0)
                      << '\n';
            std::cout << prefix << name << ".distinct_max = " << stats.max << '\n';
            std::cout << prefix << name << ".nonempty_frac = "
                      << (tick_count_ > 0
                              ? static_cast<double>(stats.nonempty_cycles) / static_cast<double>(tick_count_)
                              : 0.0)
                      << '\n';
        };
        print_distinct("request.accept_src", request_accept_distinct_src_stats_);
        print_distinct("request.blocked_src", request_blocked_distinct_src_stats_);
        print_distinct("pending.src", pending_distinct_src_stats_);
        print_distinct("pending.ready_src", ready_distinct_src_stats_);
        print_distinct("response.ready_src", response_ready_distinct_src_stats_);
        print_distinct("response.sent_dst", response_sent_distinct_dst_stats_);
        std::cout << prefix << "pending_episode.count = " << pending_episode_stats_.count << '\n';
        std::cout << prefix << "pending_episode.avg_cycles = "
                  << (pending_episode_stats_.count > 0
                          ? static_cast<double>(pending_episode_stats_.sum_cycles) /
                                static_cast<double>(pending_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "pending_episode.max_cycles = " << pending_episode_stats_.max_cycles << '\n';
        std::cout << prefix << "pending_episode.avg_peak_occ = "
                  << (pending_episode_stats_.count > 0
                          ? static_cast<double>(pending_episode_stats_.sum_peak_occ) /
                                static_cast<double>(pending_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "pending_episode.max_peak_occ = " << pending_episode_stats_.max_peak_occ << '\n';
        std::cout << prefix << "pending_episode.avg_accepted = "
                  << (pending_episode_stats_.count > 0
                          ? static_cast<double>(pending_episode_stats_.sum_accepted) /
                                static_cast<double>(pending_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "pending_episode.max_accepted = " << pending_episode_stats_.max_accepted << '\n';
        std::cout << prefix << "pending_episode.avg_sent = "
                  << (pending_episode_stats_.count > 0
                          ? static_cast<double>(pending_episode_stats_.sum_sent) /
                                static_cast<double>(pending_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "pending_episode.max_sent = " << pending_episode_stats_.max_sent << '\n';
        std::cout << prefix << "pending_episode.avg_pending_distinct_src = "
                  << (pending_episode_stats_.count > 0
                          ? static_cast<double>(pending_episode_stats_.sum_pending_distinct_src) /
                                static_cast<double>(pending_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "pending_episode.max_pending_distinct_src = "
                  << pending_episode_stats_.max_pending_distinct_src << '\n';
        std::cout << prefix << "pending_episode.avg_ready_distinct_src = "
                  << (pending_episode_stats_.count > 0
                          ? static_cast<double>(pending_episode_stats_.sum_ready_distinct_src) /
                                static_cast<double>(pending_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "pending_episode.max_ready_distinct_src = "
                  << pending_episode_stats_.max_ready_distinct_src << '\n';
        std::cout << prefix << "pending_episode.avg_sent_distinct_dst = "
                  << (pending_episode_stats_.count > 0
                          ? static_cast<double>(pending_episode_stats_.sum_sent_distinct_dst) /
                                static_cast<double>(pending_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "pending_episode.max_sent_distinct_dst = "
                  << pending_episode_stats_.max_sent_distinct_dst << '\n';
        std::cout << prefix << "response.returned_episode.count = " << returned_episode_stats_.count << '\n';
        std::cout << prefix << "response.returned_episode.avg_cycles = "
                  << (returned_episode_stats_.count > 0
                          ? static_cast<double>(returned_episode_stats_.sum_cycles) /
                                static_cast<double>(returned_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "response.returned_episode.max_cycles = " << returned_episode_stats_.max_cycles << '\n';
        std::cout << prefix << "response.returned_episode.avg_peak_occ = "
                  << (returned_episode_stats_.count > 0
                          ? static_cast<double>(returned_episode_stats_.sum_peak_occ) /
                                static_cast<double>(returned_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "response.returned_episode.max_peak_occ = " << returned_episode_stats_.max_peak_occ << '\n';
        std::cout << prefix << "response.returned_episode.avg_sent = "
                  << (returned_episode_stats_.count > 0
                          ? static_cast<double>(returned_episode_stats_.sum_sent) /
                                static_cast<double>(returned_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "response.returned_episode.max_sent = " << returned_episode_stats_.max_sent << '\n';
        std::cout << prefix << "response.returned_episode.avg_ready_distinct_src = "
                  << (returned_episode_stats_.count > 0
                          ? static_cast<double>(returned_episode_stats_.sum_ready_distinct_src) /
                                static_cast<double>(returned_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "response.returned_episode.max_ready_distinct_src = "
                  << returned_episode_stats_.max_ready_distinct_src << '\n';
        std::cout << prefix << "response.returned_episode.avg_sent_distinct_dst = "
                  << (returned_episode_stats_.count > 0
                          ? static_cast<double>(returned_episode_stats_.sum_sent_distinct_dst) /
                                static_cast<double>(returned_episode_stats_.count)
                          : 0.0)
                  << '\n';
        std::cout << prefix << "response.returned_episode.max_sent_distinct_dst = "
                  << returned_episode_stats_.max_sent_distinct_dst << '\n';
        static constexpr std::array<const char*, MY_MEMORY_CONTROLLER::kDiagClassCount> kNodeDiagNames = {"load", "rfo", "write", "other"};
        for (std::size_t src = 0; src < MAX_CXL_PORTS; ++src) {
            const auto& diag = source_diag_[src];
            bool any = diag.pending_occ_max > 0 || diag.ready_occ_max > 0;
            for (std::size_t cls = 0; cls < MY_MEMORY_CONTROLLER::kDiagClassCount; ++cls) {
                any = any || diag.request_enqueued_by_class[cls] > 0 || diag.request_blocked_by_class[cls] > 0 ||
                    diag.response_ready_by_class[cls] > 0 || diag.response_sent_by_class[cls] > 0 ||
                    diag.response_blocked_by_class[cls] > 0;
            }
            if (!any) {
                continue;
            }
            const auto node_prefix = prefix + "node." + std::to_string(src) + ".";
            std::cout << node_prefix << "pending_occ_avg = "
                      << (tick_count_ > 0 ? static_cast<double>(diag.pending_occ_sum) / static_cast<double>(tick_count_) : 0.0)
                      << '\n';
            std::cout << node_prefix << "pending_occ_max = " << diag.pending_occ_max << '\n';
            std::cout << node_prefix << "ready_occ_avg = "
                      << (tick_count_ > 0 ? static_cast<double>(diag.ready_occ_sum) / static_cast<double>(tick_count_) : 0.0)
                      << '\n';
            std::cout << node_prefix << "ready_occ_max = " << diag.ready_occ_max << '\n';
            std::cout << node_prefix << "pending_age_avg_cycles = "
                      << (diag.pending_age_samples > 0
                              ? static_cast<double>(diag.pending_age_sum_cycles) / static_cast<double>(diag.pending_age_samples)
                              : 0.0)
                      << '\n';
            std::cout << node_prefix << "pending_age_max_cycles = " << diag.pending_age_max_cycles << '\n';
            std::cout << node_prefix << "ready_age_avg_cycles = "
                      << (diag.ready_age_samples > 0
                              ? static_cast<double>(diag.ready_age_sum_cycles) / static_cast<double>(diag.ready_age_samples)
                              : 0.0)
                      << '\n';
            std::cout << node_prefix << "ready_age_max_cycles = " << diag.ready_age_max_cycles << '\n';
            for (std::size_t cls = 0; cls < MY_MEMORY_CONTROLLER::kDiagClassCount; ++cls) {
                const auto* cls_name = kNodeDiagNames[cls];
                std::cout << node_prefix << "request_enqueued." << cls_name << " = " << diag.request_enqueued_by_class[cls] << '\n';
                std::cout << node_prefix << "request_blocked." << cls_name << " = " << diag.request_blocked_by_class[cls] << '\n';
                std::cout << node_prefix << "response_ready." << cls_name << " = " << diag.response_ready_by_class[cls] << '\n';
                std::cout << node_prefix << "response_sent." << cls_name << " = " << diag.response_sent_by_class[cls] << '\n';
                std::cout << node_prefix << "response_blocked." << cls_name << " = " << diag.response_blocked_by_class[cls] << '\n';
                std::cout << node_prefix << "pending_occ_avg." << cls_name << " = "
                          << (tick_count_ > 0
                                  ? static_cast<double>(diag.pending_occ_sum_by_class[cls]) / static_cast<double>(tick_count_)
                                  : 0.0)
                          << '\n';
                std::cout << node_prefix << "pending_occ_max." << cls_name << " = " << diag.pending_occ_max_by_class[cls] << '\n';
                std::cout << node_prefix << "ready_occ_avg." << cls_name << " = "
                          << (tick_count_ > 0
                                  ? static_cast<double>(diag.ready_occ_sum_by_class[cls]) / static_cast<double>(tick_count_)
                                  : 0.0)
                          << '\n';
                std::cout << node_prefix << "ready_occ_max." << cls_name << " = " << diag.ready_occ_max_by_class[cls] << '\n';
                std::cout << node_prefix << "mem_ready_avg_cycles." << cls_name << " = "
                          << (diag.mem_ready_samples_by_class[cls] > 0
                                  ? static_cast<double>(diag.mem_ready_sum_cycles_by_class[cls]) /
                                        static_cast<double>(diag.mem_ready_samples_by_class[cls])
                                  : 0.0)
                          << '\n';
                std::cout << node_prefix << "mem_ready_max_cycles." << cls_name << " = " << diag.mem_ready_max_cycles_by_class[cls] << '\n';
                std::cout << node_prefix << "response_wait_avg_cycles." << cls_name << " = "
                          << (diag.response_wait_samples_by_class[cls] > 0
                                  ? static_cast<double>(diag.response_wait_sum_cycles_by_class[cls]) /
                                        static_cast<double>(diag.response_wait_samples_by_class[cls])
                                  : 0.0)
                          << '\n';
                std::cout << node_prefix << "response_wait_max_cycles." << cls_name << " = " << diag.response_wait_max_cycles_by_class[cls] << '\n';
                std::cout << node_prefix << "request_enqueue_gap_avg_cycles." << cls_name << " = "
                          << (diag.request_enqueue_gap_by_class[cls].samples > 0
                                  ? static_cast<double>(diag.request_enqueue_gap_by_class[cls].sum_cycles) /
                                        static_cast<double>(diag.request_enqueue_gap_by_class[cls].samples)
                                  : 0.0)
                          << '\n';
                std::cout << node_prefix << "request_enqueue_gap_max_cycles." << cls_name << " = "
                          << diag.request_enqueue_gap_by_class[cls].max_cycles << '\n';
                std::cout << node_prefix << "response_ready_gap_avg_cycles." << cls_name << " = "
                          << (diag.response_ready_gap_by_class[cls].samples > 0
                                  ? static_cast<double>(diag.response_ready_gap_by_class[cls].sum_cycles) /
                                        static_cast<double>(diag.response_ready_gap_by_class[cls].samples)
                                  : 0.0)
                          << '\n';
                std::cout << node_prefix << "response_ready_gap_max_cycles." << cls_name << " = "
                          << diag.response_ready_gap_by_class[cls].max_cycles << '\n';
                std::cout << node_prefix << "response_sent_gap_avg_cycles." << cls_name << " = "
                          << (diag.response_sent_gap_by_class[cls].samples > 0
                                  ? static_cast<double>(diag.response_sent_gap_by_class[cls].sum_cycles) /
                                        static_cast<double>(diag.response_sent_gap_by_class[cls].samples)
                                  : 0.0)
                          << '\n';
                std::cout << node_prefix << "response_sent_gap_max_cycles." << cls_name << " = "
                          << diag.response_sent_gap_by_class[cls].max_cycles << '\n';
                std::cout << node_prefix << "total_turnaround_avg_cycles." << cls_name << " = "
                          << (diag.total_turnaround_samples_by_class[cls] > 0
                                  ? static_cast<double>(diag.total_turnaround_sum_cycles_by_class[cls]) /
                                        static_cast<double>(diag.total_turnaround_samples_by_class[cls])
                                  : 0.0)
                          << '\n';
                std::cout << node_prefix << "total_turnaround_max_cycles." << cls_name << " = " << diag.total_turnaround_max_cycles_by_class[cls] << '\n';
            }
        }
        std::cout << prefix << "fabric.ingress_wait_avg_cycles = " << stats.ingress_wait_avg_cycles << '\n';
        std::cout << prefix << "fabric.egress_wait_avg_cycles = " << stats.egress_wait_avg_cycles << '\n';
        std::cout << prefix << "fabric.ingress_wait_max_cycles = " << stats.ingress_wait_max_cycles << '\n';
        std::cout << prefix << "fabric.egress_wait_max_cycles = " << stats.egress_wait_max_cycles << '\n';
        std::cout << prefix << "fabric.ingress_queue_wait_avg_cycles = " << stats.ingress_queue_wait_avg_cycles << '\n';
        std::cout << prefix << "fabric.ingress_queue_wait_max_cycles = " << stats.ingress_queue_wait_max_cycles << '\n';
        std::cout << prefix << "fabric.ingress_occ_bytes = " << stats.occ << '\n';
        std::cout << prefix << "fabric.ready_wait_avg_cycles = " << stats.ready_wait_avg_cycles << '\n';
        std::cout << prefix << "fabric.ready_wait_max_cycles = " << stats.ready_wait_max_cycles << '\n';
        std::cout << prefix << "fabric.ready_occ_avg_pkts = " << stats.ready_occ_avg << '\n';
        std::cout << prefix << "fabric.ready_occ_pkts = " << stats.ready_occ << '\n';
        std::cout << prefix << "fabric.ready_occ_max_pkts = " << stats.ready_occ_max << '\n';
        std::cout << prefix << "fabric.ready_retry_count = " << stats.ready_retry_count << '\n';
        std::cout << prefix << "fabric.ingress_arrival_burst_max_pkts = " << stats.ingress_arrival_burst_max_pkts << '\n';
        std::cout << prefix << "fabric.ingress_arrival_burst_max_bytes = " << stats.ingress_arrival_burst_max_bytes << '\n';
        std::cout << prefix << "fabric.ingress_arrival_burst_avg_pkts = " << stats.ingress_arrival_burst_avg_pkts << '\n';
        std::cout << prefix << "fabric.ingress_arrival_burst_avg_bytes = " << stats.ingress_arrival_burst_avg_bytes << '\n';
        std::cout << prefix << "fabric.ingress_arrival_burst_stddev_pkts = " << stats.ingress_arrival_burst_stddev_pkts << '\n';
        std::cout << prefix << "fabric.ingress_arrival_burst_stddev_bytes = " << stats.ingress_arrival_burst_stddev_bytes << '\n';
        std::cout << prefix << "fabric.ingress_arrival_nonempty_frac = " << stats.ingress_arrival_nonempty_frac << '\n';
        std::cout << prefix << "fabric.ingress_arrival_run_max_cycles = " << stats.ingress_arrival_run_max_cycles << '\n';
        std::cout << prefix << "fabric.ingress_arrival_run_avg_cycles = " << stats.ingress_arrival_run_avg_cycles << '\n';
        std::cout << prefix << "fabric.ingress_arrival_run_stddev_cycles = " << stats.ingress_arrival_run_stddev_cycles << '\n';
        std::cout << prefix << "fabric.ingress_arrival_gap_max_cycles = " << stats.ingress_arrival_gap_max_cycles << '\n';
        std::cout << prefix << "fabric.ingress_arrival_gap_avg_cycles = " << stats.ingress_arrival_gap_avg_cycles << '\n';
        std::cout << prefix << "fabric.ingress_arrival_gap_stddev_cycles = " << stats.ingress_arrival_gap_stddev_cycles << '\n';
        std::cout << prefix << "fabric.ingress_release_burst_max_pkts = " << stats.ingress_release_burst_max_pkts << '\n';
        std::cout << prefix << "fabric.ingress_release_burst_max_bytes = " << stats.ingress_release_burst_max_bytes << '\n';
        std::cout << prefix << "fabric.ingress_release_burst_avg_pkts = " << stats.ingress_release_burst_avg_pkts << '\n';
        std::cout << prefix << "fabric.ingress_release_burst_avg_bytes = " << stats.ingress_release_burst_avg_bytes << '\n';
        std::cout << prefix << "fabric.ingress_release_burst_stddev_pkts = " << stats.ingress_release_burst_stddev_pkts << '\n';
        std::cout << prefix << "fabric.ingress_release_burst_stddev_bytes = " << stats.ingress_release_burst_stddev_bytes << '\n';
        std::cout << prefix << "fabric.ingress_release_nonempty_frac = " << stats.ingress_release_nonempty_frac << '\n';
        std::cout << prefix << "fabric.ingress_release_run_max_cycles = " << stats.ingress_release_run_max_cycles << '\n';
        std::cout << prefix << "fabric.ingress_release_run_avg_cycles = " << stats.ingress_release_run_avg_cycles << '\n';
        std::cout << prefix << "fabric.ingress_release_run_stddev_cycles = " << stats.ingress_release_run_stddev_cycles << '\n';
        std::cout << prefix << "fabric.ingress_release_gap_max_cycles = " << stats.ingress_release_gap_max_cycles << '\n';
        std::cout << prefix << "fabric.ingress_release_gap_avg_cycles = " << stats.ingress_release_gap_avg_cycles << '\n';
        std::cout << prefix << "fabric.ingress_release_gap_stddev_cycles = " << stats.ingress_release_gap_stddev_cycles << '\n';
        std::cout << prefix << "fabric.ingress_occ_avg_bytes = " << stats.ingress_occ_avg_bytes << '\n';
        std::cout << prefix << "fabric.ingress_occ_stddev_bytes = " << stats.ingress_occ_stddev_bytes << '\n';
        std::cout << prefix << "fabric.ingress_occ_max_bytes = " << stats.ingress_occ_max_bytes << '\n';
        std::cout << prefix << "fabric.ingress_occ_nonempty_frac = " << stats.ingress_occ_nonempty_frac << '\n';
        std::cout << prefix << "fabric.ingress_occ_run_max_cycles = " << stats.ingress_occ_run_max_cycles << '\n';
        std::cout << prefix << "fabric.ingress_occ_run_avg_cycles = " << stats.ingress_occ_run_avg_cycles << '\n';
        std::cout << prefix << "fabric.ingress_occ_run_stddev_cycles = " << stats.ingress_occ_run_stddev_cycles << '\n';
        std::cout << prefix << "fabric.ingress_occ_gap_max_cycles = " << stats.ingress_occ_gap_max_cycles << '\n';
        std::cout << prefix << "fabric.ingress_occ_gap_avg_cycles = " << stats.ingress_occ_gap_avg_cycles << '\n';
        std::cout << prefix << "fabric.ingress_occ_gap_stddev_cycles = " << stats.ingress_occ_gap_stddev_cycles << '\n';
        for (const auto& [cls, cls_name] : kTrafficClasses) {
            const auto idx = static_cast<std::size_t>(cls);
            std::cout << prefix << "fabric.rx_bytes." << cls_name << " = " << stats.rx_bytes_by_class[idx] << '\n';
            std::cout << prefix << "fabric.tx_bytes." << cls_name << " = " << stats.tx_bytes_by_class[idx] << '\n';
            std::cout << prefix << "fabric.rx_pkts." << cls_name << " = " << stats.rx_packets_by_class[idx] << '\n';
            std::cout << prefix << "fabric.tx_pkts." << cls_name << " = " << stats.tx_packets_by_class[idx] << '\n';
        }
        auto print_port_stats = [&](const std::string& port_prefix, const FabricPort& port) {
            static constexpr std::array<std::pair<FabricPort::TrafficClass, const char*>, 4> kPortClasses{{
                {FabricPort::TrafficClass::DemandReq, "demand_req"},
                {FabricPort::TrafficClass::WriteReq, "write_req"},
                {FabricPort::TrafficClass::Response, "response"},
                {FabricPort::TrafficClass::OtherReq, "other_req"},
            }};
            std::cout << port_prefix << "ingress_wait_avg_cycles = " << port.ingress_wait_avg_cycles() << '\n';
            std::cout << port_prefix << "ingress_wait_max_cycles = " << port.ingress_wait_max_cycles() << '\n';
            std::cout << port_prefix << "egress_wait_avg_cycles = " << port.egress_wait_avg_cycles() << '\n';
            std::cout << port_prefix << "egress_wait_max_cycles = " << port.egress_wait_max_cycles() << '\n';
            std::cout << port_prefix << "ingress_queue_wait_avg_cycles = " << port.ingress_queue_wait_avg_cycles() << '\n';
            std::cout << port_prefix << "ingress_queue_wait_max_cycles = " << port.ingress_queue_wait_max_cycles() << '\n';
            std::cout << port_prefix << "egress_occ_avg_bytes = " << port.egress_occ_avg_bytes() << '\n';
            std::cout << port_prefix << "egress_occ_stddev_bytes = " << port.egress_occ_stddev_bytes() << '\n';
            std::cout << port_prefix << "egress_occ_max_bytes = " << port.egress_occ_max_bytes() << '\n';
            std::cout << port_prefix << "egress_occ_nonempty_frac = " << port.egress_occ_nonempty_frac() << '\n';
            std::cout << port_prefix << "egress_blocked_cycles = " << port.egress_blocked_cycles() << '\n';
            std::cout << port_prefix << "egress_blocked_nonempty_frac = " << port.egress_blocked_nonempty_frac() << '\n';
            std::cout << port_prefix << "egress_blocked_avg_occ_bytes = " << port.egress_blocked_avg_occ_bytes() << '\n';
            std::cout << port_prefix << "egress_blocked_max_occ_bytes = " << port.egress_blocked_max_occ_bytes() << '\n';
            std::cout << port_prefix << "egress_send_burst_max_pkts = " << port.egress_send_burst_max_pkts() << '\n';
            std::cout << port_prefix << "egress_send_burst_max_bytes = " << port.egress_send_burst_max_bytes() << '\n';
            std::cout << port_prefix << "egress_send_burst_avg_pkts = " << port.egress_send_burst_avg_pkts() << '\n';
            std::cout << port_prefix << "egress_send_burst_avg_bytes = " << port.egress_send_burst_avg_bytes() << '\n';
            std::cout << port_prefix << "egress_send_burst_stddev_pkts = " << port.egress_send_burst_stddev_pkts() << '\n';
            std::cout << port_prefix << "egress_send_burst_stddev_bytes = " << port.egress_send_burst_stddev_bytes() << '\n';
            std::cout << port_prefix << "egress_send_nonempty_frac = " << port.egress_send_nonempty_frac() << '\n';
            std::cout << port_prefix << "ingress_arrival_burst_avg_bytes = " << port.ingress_arrival_burst_avg_bytes() << '\n';
            std::cout << port_prefix << "ingress_arrival_nonempty_frac = " << port.ingress_arrival_nonempty_frac() << '\n';
            std::cout << port_prefix << "ingress_arrival_run_max_cycles = " << port.ingress_arrival_run_max_cycles() << '\n';
            std::cout << port_prefix << "ingress_arrival_run_avg_cycles = " << port.ingress_arrival_run_avg_cycles() << '\n';
            std::cout << port_prefix << "ingress_arrival_gap_max_cycles = " << port.ingress_arrival_gap_max_cycles() << '\n';
            std::cout << port_prefix << "ingress_arrival_gap_avg_cycles = " << port.ingress_arrival_gap_avg_cycles() << '\n';
            std::cout << port_prefix << "ingress_release_burst_avg_bytes = " << port.ingress_release_burst_avg_bytes() << '\n';
            std::cout << port_prefix << "ingress_release_nonempty_frac = " << port.ingress_release_nonempty_frac() << '\n';
            std::cout << port_prefix << "ingress_release_run_max_cycles = " << port.ingress_release_run_max_cycles() << '\n';
            std::cout << port_prefix << "ingress_release_run_avg_cycles = " << port.ingress_release_run_avg_cycles() << '\n';
            std::cout << port_prefix << "ingress_release_gap_max_cycles = " << port.ingress_release_gap_max_cycles() << '\n';
            std::cout << port_prefix << "ingress_release_gap_avg_cycles = " << port.ingress_release_gap_avg_cycles() << '\n';
            std::cout << port_prefix << "ingress_occ_avg_bytes = " << port.ingress_occ_avg_bytes() << '\n';
            std::cout << port_prefix << "ingress_occ_stddev_bytes = " << port.ingress_occ_stddev_bytes() << '\n';
            std::cout << port_prefix << "ingress_occ_max_bytes = " << port.ingress_occ_max_bytes() << '\n';
            std::cout << port_prefix << "ingress_occ_nonempty_frac = " << port.ingress_occ_nonempty_frac() << '\n';
            std::cout << port_prefix << "ingress_occ_run_max_cycles = " << port.ingress_occ_run_max_cycles() << '\n';
            std::cout << port_prefix << "ingress_occ_run_avg_cycles = " << port.ingress_occ_run_avg_cycles() << '\n';
            std::cout << port_prefix << "ingress_occ_gap_max_cycles = " << port.ingress_occ_gap_max_cycles() << '\n';
            std::cout << port_prefix << "ingress_occ_gap_avg_cycles = " << port.ingress_occ_gap_avg_cycles() << '\n';
            for (const auto& [cls, cls_name] : kPortClasses) {
                std::cout << port_prefix << "ingress_wait_avg_cycles." << cls_name << " = " << port.ingress_wait_avg_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_wait_max_cycles." << cls_name << " = " << port.ingress_wait_max_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_queue_wait_avg_cycles." << cls_name << " = " << port.ingress_queue_wait_avg_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_queue_wait_max_cycles." << cls_name << " = " << port.ingress_queue_wait_max_cycles(cls) << '\n';
                std::cout << port_prefix << "egress_wait_avg_cycles." << cls_name << " = " << port.egress_wait_avg_cycles(cls) << '\n';
                std::cout << port_prefix << "egress_wait_max_cycles." << cls_name << " = " << port.egress_wait_max_cycles(cls) << '\n';
                std::cout << port_prefix << "egress_occ_avg_bytes." << cls_name << " = " << port.egress_occ_avg_bytes(cls) << '\n';
                std::cout << port_prefix << "egress_occ_max_bytes." << cls_name << " = " << port.egress_occ_max_bytes(cls) << '\n';
                std::cout << port_prefix << "egress_blocked_cycles." << cls_name << " = " << port.egress_blocked_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_arrival_empty_packets." << cls_name << " = " << port.ingress_arrival_empty_packets(cls) << '\n';
                std::cout << port_prefix << "ingress_arrival_nonempty_packets." << cls_name << " = " << port.ingress_arrival_nonempty_packets(cls) << '\n';
                std::cout << port_prefix << "ingress_arrival_nonempty_packet_frac." << cls_name << " = " << port.ingress_arrival_nonempty_packet_frac(cls) << '\n';
                std::cout << port_prefix << "ingress_nonempty_arrival_pre_occ_avg_bytes." << cls_name << " = " << port.ingress_nonempty_arrival_pre_occ_avg_bytes(cls) << '\n';
                std::cout << port_prefix << "ingress_nonempty_arrival_pre_occ_max_bytes." << cls_name << " = " << port.ingress_nonempty_arrival_pre_occ_max_bytes(cls) << '\n';
                std::cout << port_prefix << "ingress_release_after_empty_arrival_packets." << cls_name << " = " << port.ingress_release_after_empty_arrival_packets(cls) << '\n';
                std::cout << port_prefix << "ingress_release_after_nonempty_arrival_packets." << cls_name << " = " << port.ingress_release_after_nonempty_arrival_packets(cls) << '\n';
                std::cout << port_prefix << "ingress_wait_after_empty_arrival_avg_cycles." << cls_name << " = " << port.ingress_wait_after_empty_arrival_avg_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_wait_after_nonempty_arrival_avg_cycles." << cls_name << " = " << port.ingress_wait_after_nonempty_arrival_avg_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_queue_wait_after_empty_arrival_avg_cycles." << cls_name << " = " << port.ingress_queue_wait_after_empty_arrival_avg_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_queue_wait_after_nonempty_arrival_avg_cycles." << cls_name << " = " << port.ingress_queue_wait_after_nonempty_arrival_avg_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_queue_wait_after_empty_arrival_max_cycles." << cls_name << " = " << port.ingress_queue_wait_after_empty_arrival_max_cycles(cls) << '\n';
                std::cout << port_prefix << "ingress_queue_wait_after_nonempty_arrival_max_cycles." << cls_name << " = " << port.ingress_queue_wait_after_nonempty_arrival_max_cycles(cls) << '\n';
                std::cout << port_prefix << "rx_bytes." << cls_name << " = " << port.rx_bytes(cls) << '\n';
                std::cout << port_prefix << "tx_bytes." << cls_name << " = " << port.tx_bytes(cls) << '\n';
                std::cout << port_prefix << "rx_pkts." << cls_name << " = " << port.rx_packets(cls) << '\n';
                std::cout << port_prefix << "tx_pkts." << cls_name << " = " << port.tx_packets(cls) << '\n';
            }
            port.emit_deep_diagnostics(std::cout, port_prefix);
        };
        if (use_switch_port_) {
            print_port_stats(prefix + "port.switch.", switch_port_);
        } else {
            for (int idx = 0; idx < MAX_CXL_PORTS; ++idx) {
                if (core_port_connected_[static_cast<std::size_t>(idx)]) {
                    print_port_stats(prefix + "port.core." + std::to_string(idx) + ".", core_ports_[idx]);
                }
            }
        }
        std::cout << prefix << "walltime_s = " << sec << '\n';
        if (active_calls_ > 0) {
            const auto active_sec = std::chrono::duration<double>(active_time_).count();
            std::cout << prefix << "active_time_s = " << active_sec << '\n';
        }
    } else {
        std::cout << "CXL pool " << pool_node_id_ << " utilization summary\n";
        std::cout << "  mem avg util: " << mem_ctrl_.queue_average_utilization(0) << '\n';
        if (stats.avg_util > 0.0) {
            std::cout << "  req link avg util: " << stats.avg_util << '\n';
        }
        std::cout << "  wall time (s): " << sec << '\n';
        if (active_calls_ > 0) {
            const auto active_sec = std::chrono::duration<double>(active_time_).count();
            std::cout << "Component Time Summary\n";
            std::cout << "  CXL pool active time (s): " << active_sec << '\n';
        }
    }
    std::cout << std::flush;
}

} // namespace csimCore
} // namespace SST
