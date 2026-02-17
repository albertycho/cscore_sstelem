#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <iostream>

#include "chrono.h"
#include "operable.h"
#include "channel.h"
#include "lat_bw_queue.h"

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

inline int64_t estimate_latency_percentile(double util) {
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

const size_t DEFAULT_BW = 96; /* cycles per request @ 2.4 GHz (scaled from 2.0 GHz) */
constexpr uint64_t DEFAULT_DRAM_SIZE_BYTES = 1ULL << 30;

class MY_MEMORY_CONTROLLER : public champsim::operable {
public:
    using channel_type = champsim::channel;
    using lat_bw_queue_type = lat_bw_queue<channel_type::request_type>;
    using latency_function_type = lat_bw_queue_type::latency_function_type;

    MY_MEMORY_CONTROLLER();
    MY_MEMORY_CONTROLLER(champsim::chrono::picoseconds mc_period,
                         std::vector<channel_type*>&& queues, 
                         int64_t bandwidth = DEFAULT_BW,
                         latency_function_type&& latency_function = estimate_latency_percentile,
                         champsim::data::bytes size = champsim::data::bytes{DEFAULT_DRAM_SIZE_BYTES});

    void initialize() final;
    long operate() final;
    void begin_phase() final;
    void end_phase(unsigned cpu) final;
    void print_deadlock() final;

private:
    std::vector<channel_type*> queues;
    std::vector<lat_bw_queue_type> lat_bw_queues;
    //champsim::data::bytes channel_width;
    champsim::data::bytes size_ = champsim::data::bytes{DEFAULT_DRAM_SIZE_BYTES};

public:
    champsim::data::bytes size() const { return size_; }
    //champsim::data::bytes size() const { return champsim::data::bytes{size_}; }

};
// namespace champsim
