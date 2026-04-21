#include "lat_bw_queue.h"

#include <cmath>

#include "channel.h"
#include "csEvent.h"
#include "SST_CS_packets.h"

namespace {
template <typename T>
void annotate_enqueue_cycle(T&, int64_t) {}

template <typename T>
void annotate_service_start_cycle(T&, int64_t) {}

template <typename T>
void annotate_completion_cycle(T&, int64_t) {}

void annotate_enqueue_cycle(csEvent*& ev, int64_t cycle) {
    if (ev == nullptr) {
        return;
    }
    ev->timing_mark_cycle = static_cast<uint64_t>(std::max<int64_t>(cycle + 1, 0));
}

void annotate_service_start_cycle(csEvent*& ev, int64_t cycle) {
    if (ev == nullptr) {
        return;
    }
    const auto start_cycle = static_cast<uint64_t>(std::max<int64_t>(cycle, 0));
    if (start_cycle >= ev->timing_mark_cycle) {
        ev->remote_timing.queue_cycles += (start_cycle - ev->timing_mark_cycle);
    }
    ev->timing_mark_cycle = start_cycle;
}

void annotate_completion_cycle(csEvent*&, int64_t) {}

// For pool memory requests, carry queueing and service time on the request
// itself so the completed LLC miss can consume one coherent timing record.
void annotate_enqueue_cycle(champsim::channel::request_type& req, int64_t cycle) {
    req.timing_mark_cycle = static_cast<uint64_t>(std::max<int64_t>(cycle + 1, 0));
}

void annotate_service_start_cycle(champsim::channel::request_type& req, int64_t cycle) {
    const auto start_cycle = static_cast<uint64_t>(std::max<int64_t>(cycle, 0));
    if (start_cycle >= req.timing_mark_cycle) {
        req.remote_timing.queue_cycles += (start_cycle - req.timing_mark_cycle);
    }
    req.timing_mark_cycle = start_cycle;
}

void annotate_completion_cycle(champsim::channel::request_type& req, int64_t cycle) {
    const auto completion_cycle = static_cast<uint64_t>(std::max<int64_t>(cycle, 0));
    if (completion_cycle >= req.timing_mark_cycle) {
        req.remote_timing.access_service_cycles += (completion_cycle - req.timing_mark_cycle);
    }
    req.timing_mark_cycle = completion_cycle;
}
} // namespace

template<typename T>
lat_bw_queue<T>::lat_bw_queue(double peak_bw_per_cycle,
                              latency_function_type&& latency_function,
                              bandwidth_function_type&& bw_cost_fn,
                              int64_t max_pending_bytes)
    : peak_bw_per_cycle{peak_bw_per_cycle}
    , internal_clock{0}
    , latency_function{std::forward<latency_function_type>(latency_function)}
    , bw_cost_fn{std::forward<bandwidth_function_type>(bw_cost_fn)}
    , max_pending_bytes{max_pending_bytes}
    , bw_hist{} {}

template<typename T>
std::vector<T> lat_bw_queue<T>::on_tick() {
    tick();
    return drain_ready();
}

template<typename T>
void lat_bw_queue<T>::tick() {
    internal_clock++;
    move_completed_to_ready();

    // Transmit bytes for this cycle
    service_bandwidth();

    // Move packets that become ready in the same tick (needed for zero-latency mode).
    move_completed_to_ready();

    // Update bandwidth history for utilization
    bw_sum -= bw_hist[bw_idx];
    bw_hist[bw_idx] = bw_used_this_cycle;
    bw_sum += bw_hist[bw_idx];
    bw_idx = (bw_idx + 1) % kUtilWindow;

    const auto util = get_utilization();
    util_sum += util;
    util_samples++;
}

template<typename T>
bool lat_bw_queue<T>::add_packet(T packet) {
    annotate_enqueue_cycle(packet, internal_clock);
    auto bytes = bw_cost_fn ? bw_cost_fn(packet) : 64.0;
    bytes = std::max<double>(bytes, 1.0);
    const auto packet_bytes = static_cast<int64_t>(std::ceil(bytes));
    if (max_pending_bytes > 0 && (occupancy_bytes + packet_bytes) > max_pending_bytes) {
        return false;
    }
    blocked_queue.emplace(pending_entry{std::move(packet), bytes, packet_bytes});
    occupancy_bytes += packet_bytes;
    return true;
}

template<typename T>
std::size_t lat_bw_queue<T>::occupancy() const {
    return static_cast<std::size_t>(std::max<int64_t>(occupancy_bytes, 0));
}

template<typename T>
bool lat_bw_queue<T>::has_ready() const {
    return !ready_queue.empty();
}

template<typename T>
const T& lat_bw_queue<T>::front_ready() const {
    return ready_queue.front().payload;
}

template<typename T>
T lat_bw_queue<T>::pop_ready() {
    auto ready = std::move(ready_queue.front());
    ready_queue.pop_front();
    return consume_ready_entry(std::move(ready));
}

template<typename T>
std::vector<T> lat_bw_queue<T>::drain_ready() {
    return drain_ready_if([](const T&) { return true; });
}

template<typename T>
std::vector<T> lat_bw_queue<T>::drain_ready_if(const std::function<bool(const T&)>& pred) {
    std::vector<T> drained;
    std::deque<entry> remaining;
    while (!ready_queue.empty()) {
        auto ready = std::move(ready_queue.front());
        ready_queue.pop_front();
        if (!pred || pred(ready.payload)) {
            drained.emplace_back(consume_ready_entry(std::move(ready)));
        } else {
            remaining.push_back(std::move(ready));
        }
    }
    ready_queue.swap(remaining);
    return drained;
}

template<typename T>
double lat_bw_queue<T>::utilization() const {
    return get_utilization();
}

template<typename T>
double lat_bw_queue<T>::average_utilization() const {
    if (util_samples == 0) {
        return 0.0;
    }
    return util_sum / static_cast<double>(util_samples);
}

template<typename T>
void lat_bw_queue<T>::reset_utilization() {
    util_sum = 0.0;
    util_samples = 0;
    bw_hist.fill(0.0);
    bw_sum = 0.0;
    bw_used_this_cycle = 0.0;
}

template<typename T>
T lat_bw_queue<T>::consume_ready_entry(entry ready) {
    occupancy_bytes = std::max<int64_t>(occupancy_bytes - ready.total_bytes, 0);
    return std::move(ready.payload);
}

template<typename T>
void lat_bw_queue<T>::move_completed_to_ready() {
    while (!active_queue.empty() && active_queue.top().completion_time <= internal_clock) {
        auto ready = std::move(const_cast<entry&>(active_queue.top()));
        active_queue.pop();
        annotate_completion_cycle(ready.payload, ready.completion_time);
        ready_queue.push_back(std::move(ready));
    }
}

template<typename T>
void lat_bw_queue<T>::service_bandwidth() {
    bw_used_this_cycle = 0.0;
    auto avail = peak_bw_per_cycle;
    if (avail <= 0.0) {
        return;
    }

    while (avail > 0.0 && !blocked_queue.empty()) {
        auto& front = blocked_queue.front();
        const double send = std::min(avail, front.remaining_bytes);
        front.remaining_bytes -= send;
        avail -= send;
        bw_used_this_cycle += send;

        if (front.remaining_bytes <= 0.0) {
            const int64_t completion_latency = std::max<int64_t>(latency_function(get_utilization()), 0);
            annotate_service_start_cycle(front.payload, internal_clock);
            active_queue.push(entry{
                std::move(front.payload),
                internal_clock,
                internal_clock + completion_latency,
                front.total_bytes
            });
            blocked_queue.pop();
        } else {
            break;
        }
    }
}

template<typename T>
double lat_bw_queue<T>::get_utilization() const {
    if (peak_bw_per_cycle <= 0.0) {
        return 0.0;
    }
    const double denom = static_cast<double>(kUtilWindow) * peak_bw_per_cycle;
    if (denom <= 0.0) {
        return 0.0;
    }
    return std::clamp(bw_sum / denom, 0.0, 1.0);
}

template class lat_bw_queue<SST::csimCore::csEvent*>;
template class lat_bw_queue<champsim::channel::request_type>;
template class lat_bw_queue<sst_request>;
