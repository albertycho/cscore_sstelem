/*
 * Minimal MY_MEMORY_CONTROLLER header
 * Mirrors the basic API of MEMORY_CONTROLLER from dram_controller.h
 */
#ifndef MY_MEMORY_CONTROLLER_H
#define MY_MEMORY_CONTROLLER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "chrono.h"
#include "operable.h"
#include "channel.h"


class MY_MEMORY_CONTROLLER : public champsim::operable {
public:
    using channel_type = champsim::channel;

    MY_MEMORY_CONTROLLER();
    MY_MEMORY_CONTROLLER(champsim::chrono::picoseconds mc_period,
                         std::vector<channel_type*>&& queues);

    void initialize() final;
    long operate() final;
    void begin_phase() final;
    void end_phase(unsigned cpu) final;
    void print_deadlock() final;

private:
    std::vector<channel_type*> queues;
    //champsim::data::bytes channel_width;
    champsim::data::bytes size_ = champsim::data::bytes{1}; //orig 128?

public:
    champsim::data::bytes size() const { return size_; }
    //champsim::data::bytes size() const { return champsim::data::bytes{size_}; }

};
// namespace champsim

#endif // MY_MEMORY_CONTROLLER_H
