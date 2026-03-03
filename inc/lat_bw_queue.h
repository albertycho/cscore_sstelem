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
                 int64_t max_pending = 0);

    /// Called once per tick (start of each cycle)
    /// Returns packets completed on this tick
    std::vector<T> on_tick();

    /// Adds a single packet to the channel. Packet will be queued if there is not sufficient bandwidth.
    bool add_packet(T&& packet);

    std::size_t occupancy() const;

    double utilization() const;

    double average_utilization() const;

    void reset_utilization();

    bool is_full() const;

private:
    void service_bandwidth();
    double get_utilization() const;

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
