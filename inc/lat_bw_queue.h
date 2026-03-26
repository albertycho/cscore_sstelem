#pragma once

#include <queue>
#include <deque>
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
        int64_t total_bytes;
    };
    struct entry {
        T payload;
        int64_t injection_time;
        int64_t completion_time;
        int64_t total_bytes;

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
                 int64_t max_pending_bytes = 0);

    /// Called once per tick (start of each cycle)
    /// Returns packets completed on this tick
    std::vector<T> on_tick();

    /// Advances the queue by one cycle without draining completed packets.
    void tick();

    /// Adds a single packet to the channel. Packet will be queued if there is not sufficient bandwidth.
    bool add_packet(T packet);

    std::size_t occupancy() const;
    bool has_ready() const;
    const T& front_ready() const;
    T pop_ready();
    std::vector<T> drain_ready();
    std::vector<T> drain_ready_if(const std::function<bool(const T&)>& pred);

    double utilization() const;

    double average_utilization() const;

    void reset_utilization();

private:
    T consume_ready_entry(entry ready);
    void move_completed_to_ready();
    void service_bandwidth();
    double get_utilization() const;

    double peak_bw_per_cycle;
    int64_t internal_clock;
    std::function<int64_t(double)> latency_function;
    bandwidth_function_type bw_cost_fn;

    std::priority_queue<entry> active_queue;    // (packet, injection_time)
    std::queue<pending_entry> blocked_queue;    // waiting to transmit
    std::deque<entry> ready_queue;              // completed, awaiting consumer
    int64_t max_pending_bytes;
    int64_t occupancy_bytes = 0;
    double util_sum = 0.0;
    uint64_t util_samples = 0;
    std::array<double, kUtilWindow> bw_hist{};
    std::size_t bw_idx = 0;
    double bw_sum = 0.0;
    double bw_used_this_cycle = 0.0;
};

// Explicit instantiations live in src/lat_bw_queue.cc.
