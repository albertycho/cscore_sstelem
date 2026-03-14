#include "my_memory_controller.h"

#include <algorithm>

MY_MEMORY_CONTROLLER::MY_MEMORY_CONTROLLER() {}

MY_MEMORY_CONTROLLER::MY_MEMORY_CONTROLLER(champsim::chrono::picoseconds mc_period,
                                           std::vector<channel_type*>&& ul, int64_t bw_cycles_per_req,
                                           latency_function_type&& latency_function,
                                           champsim::data::bytes size)
    : operable(mc_period)
    , queues(std::move(ul))
    , lat_bw_queues(
        queues.size(), 
        lat_bw_queue_type(
            /*peak_bw_per_cycle=*/(bw_cycles_per_req > 0
                ? (64.0 / static_cast<double>(bw_cycles_per_req))
                : 0.0),
            /*latency_function=*/std::forward<latency_function_type>(latency_function),
            /*bw_cost_fn=*/[](const channel_type::request_type&) { return 64.0; },
            /*max_pending_bytes=*/0,
            /*class_count=*/kDiagClassCount,
            /*classify_fn=*/[](const channel_type::request_type& req) {
                return static_cast<std::size_t>(classify_request(req));
            },
            /*enqueue_observer=*/[this](channel_type::request_type& req,
                                        const lat_bw_queue_type::queue_snapshot& snapshot,
                                        int64_t cycle) {
                observe_enqueue(req, snapshot, cycle);
            },
            /*service_start_observer=*/[this](channel_type::request_type& req, int64_t cycle) {
                observe_service_start(req, cycle);
            },
            /*completion_observer=*/[this](channel_type::request_type& req, int64_t cycle) {
                observe_completion(req, cycle);
            }))
    , size_(size)
{
}

void MY_MEMORY_CONTROLLER::initialize()
{
    // minimal initialization
}

long MY_MEMORY_CONTROLLER::operate()
{
    long progress = 0;
    std::array<uint64_t, kDiagClassCount> enqueued_by_class{};
    std::array<uint64_t, kDiagClassCount> completed_by_class{};
    uint64_t enqueued_total = 0;
    uint64_t completed_total = 0;
    uint64_t occ_total_bytes = 0;
    std::array<uint64_t, kDiagClassCount> occ_bytes_by_class{};
    std::array<uint64_t, kDiagClassCount> occ_pkts_by_class{};

    for(size_t i = 0; i < queues.size(); ++i) {
        if(queues[i] == nullptr) continue;
        auto& champsim_channel = *queues[i];
        auto& lat_bw_queue = lat_bw_queues[i];

        // Get fulfilled requests
        auto completed_requests = lat_bw_queue.on_tick();
        while(!completed_requests.empty()) {
            auto& req = completed_requests.back();
            const auto cls_idx = static_cast<std::size_t>(classify_request(req));
            if (cls_idx < kDiagClassCount) {
                completed_by_class[cls_idx]++;
            }
            completed_total++;
            if(req.response_requested) {
                champsim_channel.returned.emplace_back(std::move(req));
            }
            completed_requests.pop_back();
            progress++;
        }

        // Drain requests from the champsim channel into the latency/bandwidth queue
        auto drain_into_queue = [&](auto& q) {
            while(!q.empty()) {
                const auto cls_idx = static_cast<std::size_t>(classify_request(q.front()));
                if (cls_idx < kDiagClassCount) {
                    enqueued_by_class[cls_idx]++;
                }
                enqueued_total++;
                lat_bw_queue.add_packet(std::move(q.front()));
                q.pop_front();
                progress++;
            }
        };
        drain_into_queue(champsim_channel.RQ);
        drain_into_queue(champsim_channel.PQ);
        drain_into_queue(champsim_channel.WQ);

        occ_total_bytes += static_cast<uint64_t>(lat_bw_queue.occupancy());
        for (std::size_t cls = 0; cls < kDiagClassCount; ++cls) {
            occ_bytes_by_class[cls] += lat_bw_queue.occupancy_bytes_by_class(cls);
            occ_pkts_by_class[cls] += lat_bw_queue.occupancy_packets_by_class(cls);
        }

        // // Warn if more than one response was queued for this channel
        // if (lat_bw_queue.returned.size() > 1) {
        //     std::cerr << "Warning: channel returned queue has " << lat_bw_queue.returned.size() << " responses (expected <=1) at "
        //               << static_cast<const void*>(ul) << std::endl;
        // }
    }

    queue_diag_stats_.occ_samples++;
    queue_diag_stats_.occ_sum_bytes += occ_total_bytes;
    queue_diag_stats_.occ_sq_sum_bytes += static_cast<long double>(occ_total_bytes) * static_cast<long double>(occ_total_bytes);
    queue_diag_stats_.occ_max_bytes = std::max(queue_diag_stats_.occ_max_bytes, occ_total_bytes);
    if (occ_total_bytes > 0) {
        queue_diag_stats_.occ_nonempty_cycles++;
    }
    for (std::size_t cls = 0; cls < kDiagClassCount; ++cls) {
        queue_diag_stats_.occ_sum_bytes_by_class[cls] += occ_bytes_by_class[cls];
        queue_diag_stats_.occ_max_bytes_by_class[cls] =
            std::max(queue_diag_stats_.occ_max_bytes_by_class[cls], occ_bytes_by_class[cls]);
        queue_diag_stats_.occ_sum_pkts_by_class[cls] += occ_pkts_by_class[cls];
        queue_diag_stats_.occ_max_pkts_by_class[cls] =
            std::max(queue_diag_stats_.occ_max_pkts_by_class[cls], occ_pkts_by_class[cls]);
    }
    if (enqueued_total > 0) {
        queue_diag_stats_.enqueue_burst_nonempty_cycles++;
        queue_diag_stats_.enqueue_burst_sum_pkts += enqueued_total;
        queue_diag_stats_.enqueue_burst_sq_sum_pkts +=
            static_cast<long double>(enqueued_total) * static_cast<long double>(enqueued_total);
        queue_diag_stats_.enqueue_burst_max_pkts =
            std::max(queue_diag_stats_.enqueue_burst_max_pkts, enqueued_total);
        for (std::size_t cls = 0; cls < kDiagClassCount; ++cls) {
            queue_diag_stats_.enqueue_burst_sum_pkts_by_class[cls] += enqueued_by_class[cls];
            queue_diag_stats_.enqueue_burst_max_pkts_by_class[cls] =
                std::max(queue_diag_stats_.enqueue_burst_max_pkts_by_class[cls], enqueued_by_class[cls]);
        }
    }
    if (completed_total > 0) {
        queue_diag_stats_.complete_burst_nonempty_cycles++;
        queue_diag_stats_.complete_burst_sum_pkts += completed_total;
        queue_diag_stats_.complete_burst_sq_sum_pkts +=
            static_cast<long double>(completed_total) * static_cast<long double>(completed_total);
        queue_diag_stats_.complete_burst_max_pkts =
            std::max(queue_diag_stats_.complete_burst_max_pkts, completed_total);
        for (std::size_t cls = 0; cls < kDiagClassCount; ++cls) {
            queue_diag_stats_.complete_burst_sum_pkts_by_class[cls] += completed_by_class[cls];
            queue_diag_stats_.complete_burst_max_pkts_by_class[cls] =
                std::max(queue_diag_stats_.complete_burst_max_pkts_by_class[cls], completed_by_class[cls]);
        }
    }

    return progress;
}

void MY_MEMORY_CONTROLLER::begin_phase()
{
}

void MY_MEMORY_CONTROLLER::end_phase(unsigned cpu)
{
}

void MY_MEMORY_CONTROLLER::print_deadlock()
{
}

MY_MEMORY_CONTROLLER::RequestDiagStats MY_MEMORY_CONTROLLER::demand_diag_stats() const
{
    RequestDiagStats out{};
    auto merge = [&out](const RequestDiagStats& in) {
        out.completed += in.completed;
        out.response_completed += in.response_completed;
        out.queue_wait_sum_cycles += in.queue_wait_sum_cycles;
        out.queue_wait_max_cycles = std::max(out.queue_wait_max_cycles, in.queue_wait_max_cycles);
        out.service_sum_cycles += in.service_sum_cycles;
        out.service_max_cycles = std::max(out.service_max_cycles, in.service_max_cycles);
        out.total_sum_cycles += in.total_sum_cycles;
        out.total_max_cycles = std::max(out.total_max_cycles, in.total_max_cycles);
        for (std::size_t i = 0; i < kDiagClassCount; ++i) {
            out.ahead_pkts_sum[i] += in.ahead_pkts_sum[i];
            out.ahead_pkts_max[i] = std::max(out.ahead_pkts_max[i], in.ahead_pkts_max[i]);
            out.ahead_bytes_sum[i] += in.ahead_bytes_sum[i];
            out.ahead_bytes_max[i] = std::max(out.ahead_bytes_max[i], in.ahead_bytes_max[i]);
        }
    };
    merge(request_diag_stats_[static_cast<std::size_t>(RequestDiagClass::Load)]);
    merge(request_diag_stats_[static_cast<std::size_t>(RequestDiagClass::Rfo)]);
    return out;
}

const char* MY_MEMORY_CONTROLLER::request_diag_class_name(RequestDiagClass cls)
{
    switch (cls) {
    case RequestDiagClass::Load:
        return "load";
    case RequestDiagClass::Rfo:
        return "rfo";
    case RequestDiagClass::Write:
        return "write";
    case RequestDiagClass::Other:
    default:
        return "other";
    }
}

MY_MEMORY_CONTROLLER::RequestDiagClass
MY_MEMORY_CONTROLLER::classify_request(const channel_type::request_type& req)
{
    switch (req.type) {
    case access_type::LOAD:
        return RequestDiagClass::Load;
    case access_type::RFO:
        return RequestDiagClass::Rfo;
    case access_type::WRITE:
        return RequestDiagClass::Write;
    default:
        return RequestDiagClass::Other;
    }
}

uint64_t MY_MEMORY_CONTROLLER::request_tag(const channel_type::request_type& req)
{
    if (req.instr_depend_on_me.empty()) {
        return 0;
    }
    return req.instr_depend_on_me.front();
}

void MY_MEMORY_CONTROLLER::observe_enqueue(channel_type::request_type& req,
                                           const lat_bw_queue_type::queue_snapshot& snapshot,
                                           int64_t cycle)
{
    const auto tag = request_tag(req);
    if (tag == 0) {
        return;
    }
    RequestDiagState state{};
    state.cls = classify_request(req);
    state.response_requested = req.response_requested;
    state.enqueue_cycle = cycle;
    for (std::size_t i = 0; i < std::min<std::size_t>(snapshot.packets_by_class.size(), kDiagClassCount); ++i) {
        state.ahead_pkts[i] = snapshot.packets_by_class[i];
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(snapshot.bytes_by_class.size(), kDiagClassCount); ++i) {
        state.ahead_bytes[i] = snapshot.bytes_by_class[i];
    }
    request_diag_state_[tag] = state;
}

void MY_MEMORY_CONTROLLER::observe_service_start(channel_type::request_type& req, int64_t cycle)
{
    const auto tag = request_tag(req);
    auto it = request_diag_state_.find(tag);
    if (it == request_diag_state_.end()) {
        return;
    }
    if (it->second.service_start_cycle < 0) {
        it->second.service_start_cycle = cycle;
    }
}

void MY_MEMORY_CONTROLLER::observe_completion(channel_type::request_type& req, int64_t cycle)
{
    const auto tag = request_tag(req);
    auto it = request_diag_state_.find(tag);
    if (it == request_diag_state_.end()) {
        return;
    }
    auto& state = it->second;
    auto& stats = request_diag_stats_[static_cast<std::size_t>(state.cls)];
    const uint64_t queue_wait = (state.enqueue_cycle >= 0 && state.service_start_cycle >= state.enqueue_cycle)
        ? static_cast<uint64_t>(state.service_start_cycle - state.enqueue_cycle)
        : 0;
    const uint64_t service_wait = (state.service_start_cycle >= 0 && cycle >= state.service_start_cycle)
        ? static_cast<uint64_t>(cycle - state.service_start_cycle)
        : 0;
    const uint64_t total_wait = (state.enqueue_cycle >= 0 && cycle >= state.enqueue_cycle)
        ? static_cast<uint64_t>(cycle - state.enqueue_cycle)
        : 0;

    stats.completed++;
    if (state.response_requested) {
        stats.response_completed++;
    }
    stats.queue_wait_sum_cycles += queue_wait;
    stats.queue_wait_max_cycles = std::max(stats.queue_wait_max_cycles, queue_wait);
    stats.service_sum_cycles += service_wait;
    stats.service_max_cycles = std::max(stats.service_max_cycles, service_wait);
    stats.total_sum_cycles += total_wait;
    stats.total_max_cycles = std::max(stats.total_max_cycles, total_wait);
    for (std::size_t i = 0; i < kDiagClassCount; ++i) {
        stats.ahead_pkts_sum[i] += state.ahead_pkts[i];
        stats.ahead_pkts_max[i] = std::max(stats.ahead_pkts_max[i], state.ahead_pkts[i]);
        stats.ahead_bytes_sum[i] += state.ahead_bytes[i];
        stats.ahead_bytes_max[i] = std::max(stats.ahead_bytes_max[i], state.ahead_bytes[i]);
    }
    request_diag_state_.erase(it);
}
// namespace champsim
