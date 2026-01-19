#pragma once

#include <queue>
#include <utility>
#include <functional>
#include <bitset>
#include <algorithm>
#include <vector>

template<typename T>
class lat_bw_queue {
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
    lat_bw_queue(int64_t bandwidth, latency_function_type&& latency_function)
    : bandwidth{bandwidth} 
    , internal_clock{0}
    , last_insert_time{-bandwidth}
    , latency_function{std::forward<latency_function_type>(latency_function)}
    , buffer{} {}

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

        // Shift buffer
        buffer <<= 1;

        // Refill from blocked queue up to available bandwidth
        try_fire_request();

        return completed_on_tick;
    }

    /// Adds a single packet to the channel. Packet will be queued if there is not sufficient bandwidth.
    void add_packet(T&& packet) {
        blocked_queue.emplace(std::forward<T>(packet));
        try_fire_request();
    }

private:

    /// Attempts to fire a new request, from the blocked queue
    void try_fire_request() {
        if(!blocked_queue.empty() && (last_insert_time + bandwidth <= internal_clock)) {
            buffer |= 1;

            active_queue.push(entry{
                std::move(blocked_queue.front()), 
                internal_clock, 
                internal_clock + std::max<int64_t>(latency_function(get_utilization()), 1)
            });
            last_insert_time = internal_clock;

            blocked_queue.pop();
        }
    }

    // computes the current utilization
    double get_utilization() const {
        return std::clamp(buffer.count() / 256.0 * bandwidth, 0.0, 1.0);
    }

    int64_t bandwidth;
    int64_t internal_clock;
    int64_t last_insert_time;
    std::function<int64_t(double)> latency_function;

    std::priority_queue<entry> active_queue;    // (packet, injection_time)
    std::queue<T> blocked_queue;                // waiting to enter
    std::bitset<256> buffer;                    // history buffer
};
