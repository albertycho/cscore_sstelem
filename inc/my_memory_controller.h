#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <iostream>
#include <array>
#include <unordered_map>

#include "chrono.h"
#include "operable.h"
#include "channel.h"
#include "lat_bw_queue.h"

extern template class lat_bw_queue<champsim::channel::request_type>;

// 64B request
// 2.4 GHz: cycle = ~0.4167 ns
// bw: 40 GB/s --> ~16.7 bytes per cycle
// ~4 cycles per request (64B)

// lat:
// - 0%:    30ns    --> 60 cycles
// - 20%:   30ns
// - 30%:   50ns
// - 50%:   100ns
// - 60%:   150ns
// - 70%+:  200ns

// --> can define a lower BW to see if latency spikes

inline int64_t estimate_latency_utilization_based(double util) {
    if (util >= 0.70) return 480;  // 200 ns @ 2.4 GHz
    if (util >= 0.60) return 360;  // 150 ns
    if (util >= 0.50) return 240;  // 100 ns
    if (util >= 0.30) return 120;  // 50 ns
    return 72;                     // 30 ns
}

constexpr int64_t DEFAULT_FIXED_LATENCY_CYCLES = 300;

inline int64_t estimate_latency_fixed(double) {
    return DEFAULT_FIXED_LATENCY_CYCLES;
}

const size_t DEFAULT_BW = 96; /* cycles per request @ 2.4 GHz */
constexpr uint64_t DEFAULT_DRAM_SIZE_BYTES = 1ULL << 30;

class MY_MEMORY_CONTROLLER : public champsim::operable {
public:
    using channel_type = champsim::channel;
    using lat_bw_queue_type = lat_bw_queue<channel_type::request_type>;
    using latency_function_type = lat_bw_queue_type::latency_function_type;
    static constexpr std::size_t kDiagClassCount = 4;

    enum class RequestDiagClass : std::size_t {
        Load = 0,
        Rfo = 1,
        Write = 2,
        Other = 3,
    };

    struct RequestDiagStats {
        uint64_t completed = 0;
        uint64_t response_completed = 0;
        uint64_t queue_wait_sum_cycles = 0;
        uint64_t queue_wait_max_cycles = 0;
        uint64_t service_sum_cycles = 0;
        uint64_t service_max_cycles = 0;
        uint64_t total_sum_cycles = 0;
        uint64_t total_max_cycles = 0;
        std::array<uint64_t, kDiagClassCount> ahead_pkts_sum{};
        std::array<uint64_t, kDiagClassCount> ahead_pkts_max{};
        std::array<uint64_t, kDiagClassCount> ahead_bytes_sum{};
        std::array<uint64_t, kDiagClassCount> ahead_bytes_max{};
    };

    MY_MEMORY_CONTROLLER();
    MY_MEMORY_CONTROLLER(champsim::chrono::picoseconds mc_period,
                         std::vector<channel_type*>&& queues, 
                         int64_t bw_cycles_per_req = DEFAULT_BW,
                         latency_function_type&& latency_function = estimate_latency_utilization_based,
                         champsim::data::bytes size = champsim::data::bytes{DEFAULT_DRAM_SIZE_BYTES});

    void initialize() final;
    long operate() final;
    void begin_phase() final;
    void end_phase(unsigned cpu) final;
    void print_deadlock() final;
    RequestDiagStats demand_diag_stats() const;
    const RequestDiagStats& request_diag_stats(RequestDiagClass cls) const {
        return request_diag_stats_[static_cast<std::size_t>(cls)];
    }
    static const char* request_diag_class_name(RequestDiagClass cls);

private:
    struct RequestDiagState {
        RequestDiagClass cls = RequestDiagClass::Other;
        bool response_requested = false;
        int64_t enqueue_cycle = -1;
        int64_t service_start_cycle = -1;
        std::array<uint64_t, kDiagClassCount> ahead_pkts{};
        std::array<uint64_t, kDiagClassCount> ahead_bytes{};
    };

    static RequestDiagClass classify_request(const channel_type::request_type& req);
    static uint64_t request_tag(const channel_type::request_type& req);
    void observe_enqueue(channel_type::request_type& req,
                         const lat_bw_queue_type::queue_snapshot& snapshot,
                         int64_t cycle);
    void observe_service_start(channel_type::request_type& req, int64_t cycle);
    void observe_completion(channel_type::request_type& req, int64_t cycle);

    std::vector<channel_type*> queues;
    std::vector<lat_bw_queue_type> lat_bw_queues;
    std::unordered_map<uint64_t, RequestDiagState> request_diag_state_;
    std::array<RequestDiagStats, kDiagClassCount> request_diag_stats_{};
    //champsim::data::bytes channel_width;
    champsim::data::bytes size_ = champsim::data::bytes{DEFAULT_DRAM_SIZE_BYTES};

public:
    champsim::data::bytes size() const { return size_; }
    //champsim::data::bytes size() const { return champsim::data::bytes{size_}; }

    std::size_t queue_count() const { return lat_bw_queues.size(); }
    std::size_t queue_occupancy(std::size_t idx) const {
        return idx < lat_bw_queues.size() ? lat_bw_queues[idx].occupancy() : 0;
    }
    double queue_utilization(std::size_t idx) const {
        return idx < lat_bw_queues.size() ? lat_bw_queues[idx].utilization() : 0.0;
    }
    double queue_average_utilization(std::size_t idx) const {
        return idx < lat_bw_queues.size() ? lat_bw_queues[idx].average_utilization() : 0.0;
    }
    void reset_utilization() {
        for (auto& q : lat_bw_queues) {
            q.reset_utilization();
        }
    }
    void reset_diagnostics() {
        request_diag_state_.clear();
        request_diag_stats_.fill(RequestDiagStats{});
    }

};
// namespace champsim
