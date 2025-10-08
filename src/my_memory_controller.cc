#include "my_memory_controller.h"

#include <iostream>
#include <unistd.h>
#include <random>

MY_MEMORY_CONTROLLER::MY_MEMORY_CONTROLLER()
{
    std::cout<<" hello"<<std::endl;
}
MY_MEMORY_CONTROLLER::MY_MEMORY_CONTROLLER(champsim::chrono::picoseconds mc_period,
                                           std::vector<channel_type*>&& ul)
    : operable(mc_period), queues(std::move(ul))
{
    auto sleepcnt = rand() % 10;
    sleep(sleepcnt);
    std::cout << "MY_MEMORY_CONTROLLER constructed" << std::endl;
}

void MY_MEMORY_CONTROLLER::initialize()
{
    // minimal initialization
}

long MY_MEMORY_CONTROLLER::operate()
{
    long progress = 0;

    for (auto* ul : queues) {
        if (ul == nullptr) continue;

        // Process RQ: reads
        while (!ul->RQ.empty()) {
            auto req = std::move(ul->RQ.front());
            ul->RQ.pop_front();
            // create a response and push to returned if requested
            if (req.response_requested) {
                ul->returned.emplace_back(req);
            }
            ++progress;
        }

        // Process PQ: prefetch queue
        while (!ul->PQ.empty()) {
            auto req = std::move(ul->PQ.front());
            ul->PQ.pop_front();
            if (req.response_requested) {
                ul->returned.emplace_back(req);
            }
            ++progress;
        }

        // Process WQ: writes
        while (!ul->WQ.empty()) {
            auto req = std::move(ul->WQ.front());
            ul->WQ.pop_front();
            if (req.response_requested) {
                ul->returned.emplace_back(req);
            }
            ++progress;
        }

        // Warn if more than one response was queued for this channel
        if (ul->returned.size() > 1) {
            std::cerr << "Warning: channel returned queue has " << ul->returned.size() << " responses (expected <=1) at "
                      << static_cast<const void*>(ul) << std::endl;
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
    std::cerr << "MY_MEMORY_CONTROLLER deadlock check" << std::endl;
}
 // namespace champsim
