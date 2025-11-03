/*
 * Minimal MY_MEMORY_CONTROLLER header
 * Mirrors the basic API of MEMORY_CONTROLLER from dram_controller.h
 */
#ifndef MY_MEMORY_CONTROLLER_H
#define MY_MEMORY_CONTROLLER_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <iostream>

#include "chrono.h"
#include "operable.h"
#include "channel.h"
#include "physical_channel.h"

// 64B request
// 2 GHz: cycle = 0.5 ns
// bw: 40 GB/s --> 20 bytes per cycle
// 3 cycles per request

// lat:
// - 0%:    30ns    --> 60 cycles
// - 20%:   30ns
// - 30%:   50ns
// - 50%:   100ns
// - 60%:   150ns
// - 70%+:  200ns

// --> can define a lower BW to see if latency spikes

inline int64_t estimate_latency_percentile(double util) {
    std::cout << "utilization: " << util << std::endl;
    if (util >= 0.70) return 400;  // 200 ns @ 2 GHz
    if (util >= 0.60) return 300;  // 150 ns
    if (util >= 0.50) return 200;  // 100 ns
    if (util >= 0.30) return 100;  // 50 ns
    return 60;                     // 30 ns
}

const size_t DEFAULT_BW = 80; /* cycles per request */

class MY_MEMORY_CONTROLLER : public champsim::operable {
public:
    using channel_type = champsim::channel;
    using phys_channel_type = physical_channel<channel_type::request_type>;
    using latency_function_type = phys_channel_type::latency_function_type;

    MY_MEMORY_CONTROLLER();
    MY_MEMORY_CONTROLLER(champsim::chrono::picoseconds mc_period,
                         std::vector<channel_type*>&& queues, 
                         int64_t bandwidth = DEFAULT_BW,
                         latency_function_type&& latency_function = estimate_latency_percentile);

    void initialize() final;
    long operate() final;
    void begin_phase() final;
    void end_phase(unsigned cpu) final;
    void print_deadlock() final;

private:
    std::vector<channel_type*> queues;
    std::vector<phys_channel_type> phys_queues;
    //champsim::data::bytes channel_width;
    champsim::data::bytes size_ = champsim::data::bytes{1}; //orig 128?

public:
    champsim::data::bytes size() const { return size_; }
    //champsim::data::bytes size() const { return champsim::data::bytes{size_}; }

};
// namespace champsim

#endif // MY_MEMORY_CONTROLLER_H