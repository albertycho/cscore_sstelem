#pragma once

#include <queue>
#include <utility>
#include <functional>
#include <algorithm>
#include <vector>
#include <array>

template<typename T>
class lat_bw_queue {
    struct pending_entry {
        T payload;
        double remaining_bytes;
    };
    struct entry {
        T payload;
        int64_t injection_time;
        int64_t completion_time;

        // reversed comparator for priority queue
        friend bool operator<(const entry& a, const entry& b) {
            if(a.completion_time == b.completion_time) {
                return a.injection_time > b.injection_time;
            }
            return a.completion_time > b.completion_time;
        }
    };
public:
    using latency_function_type = std::function<int64_t(double)>;
    using bandwidth_function_type = std::function<double(const T&)>;
    static constexpr std::size_t kUtilWindow = 256;
    lat_bw_queue(double peak_bw_per_cycle,
                 latency_function_type&& latency_function,
                 bandwidth_function_type&& bw_cost_fn = {},
                 int64_t max_pending = 0)
    : peak_bw_per_cycle{peak_bw_per_cycle} 
    , internal_clock{0}
    , latency_function{std::forward<latency_function_type>(latency_function)}
    , bw_cost_fn{std::forward<bandwidth_function_type>(bw_cost_fn)}
    , max_pending{max_pending}
    , bw_hist{} {}

    /// Called once per tick (start of each cycle)
    /// Returns packets completed on this tick
    std::vector<T> on_tick() {
        internal_clock++;
        std::vector<T> completed_on_tick;

        // Pop completed packets
        while(!active_queue.empty() && active_queue.top().completion_time <= internal_clock) {
            completed_on_tick.emplace_back(std::move(active_queue.top().payload));
            active_queue.pop();
        }

        // Transmit bytes for this cycle
        service_bandwidth();

        // Update bandwidth history for utilization
        bw_sum -= bw_hist[bw_idx];
        bw_hist[bw_idx] = bw_used_this_cycle;
        bw_sum += bw_hist[bw_idx];
        bw_idx = (bw_idx + 1) % kUtilWindow;

        const auto util = get_utilization();
        util_sum += util;
        util_samples++;

        return completed_on_tick;
    }

    /// Adds a single packet to the channel. Packet will be queued if there is not sufficient bandwidth.
    bool add_packet(T&& packet) {
        if (is_full()) {
            return false;
        }
        auto bytes = bw_cost_fn ? bw_cost_fn(packet) : 64.0;
        bytes = std::max<double>(bytes, 1.0);
        blocked_queue.emplace(pending_entry{std::forward<T>(packet), bytes});
        return true;
    }

    std::size_t occupancy() const {
        return blocked_queue.size() + active_queue.size();
    }

    double utilization() const {
        return get_utilization();
    }

    double average_utilization() const {
        if (util_samples == 0) {
            return 0.0;
        }
        return util_sum / static_cast<double>(util_samples);
    }

    void reset_utilization() {
        util_sum = 0.0;
        util_samples = 0;
        bw_hist.fill(0.0);
        bw_sum = 0.0;
        bw_used_this_cycle = 0.0;
    }

    bool is_full() const {
        return max_pending > 0 && static_cast<int64_t>(occupancy()) >= max_pending;
    }

private:
    void service_bandwidth() {
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
                active_queue.push(entry{
                    std::move(front.payload),
                    internal_clock,
                    internal_clock + std::max<int64_t>(latency_function(get_utilization()), 1)
                });
                blocked_queue.pop();
            } else {
                break;
            }
        }
    }

    double get_utilization() const {
        if (peak_bw_per_cycle <= 0.0) {
            return 0.0;
        }
        const double denom = static_cast<double>(kUtilWindow) * peak_bw_per_cycle;
        if (denom <= 0.0) {
            return 0.0;
        }
        return std::clamp(bw_sum / denom, 0.0, 1.0);
    }

    double peak_bw_per_cycle;
    int64_t internal_clock;
    std::function<int64_t(double)> latency_function;
    bandwidth_function_type bw_cost_fn;

    std::priority_queue<entry> active_queue;    // (packet, injection_time)
    std::queue<pending_entry> blocked_queue;    // waiting to transmit
    int64_t max_pending;
    double util_sum = 0.0;
    uint64_t util_samples = 0;
    std::array<double, kUtilWindow> bw_hist{};
    std::size_t bw_idx = 0;
    double bw_sum = 0.0;
    double bw_used_this_cycle = 0.0;
};
