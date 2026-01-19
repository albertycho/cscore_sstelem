#include "my_memory_controller.h"

#include <iostream>
#include <unistd.h>
#include <random>

MY_MEMORY_CONTROLLER::MY_MEMORY_CONTROLLER() {}

MY_MEMORY_CONTROLLER::MY_MEMORY_CONTROLLER(champsim::chrono::picoseconds mc_period,
                                           std::vector<channel_type*>&& ul, int64_t bandwidth,
                                           latency_function_type&& latency_function)
    : operable(mc_period)
    , queues(std::move(ul))
    , lat_bw_queues(
        queues.size(), 
        lat_bw_queue_type(
            /*badwidth=*/bandwidth, 
            /*latency_function=*/std::forward<latency_function_type>(latency_function)))
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

    for(size_t i = 0; i < queues.size(); ++i) {
        if(queues[i] == nullptr) continue;
        auto& champsim_channel = *queues[i];
        auto& lat_bw_queue = lat_bw_queues[i];

        // Get fulfilled requests
        auto completed_requests = lat_bw_queue.on_tick();
        while(!completed_requests.empty()) {
            auto& req = completed_requests.back();
            if(req.response_requested) {
                champsim_channel.returned.emplace_back(std::move(req));
            }
            completed_requests.pop_back();
            progress++;
        }

        // Drain requests from the champsim channel into the latency/bandwidth queue
        auto drain_into_queue = [&](auto& q) {
            while(!q.empty()) {
                lat_bw_queue.add_packet(std::move(q.front()));
                q.pop_front();
                progress++;
            }
        };
        drain_into_queue(champsim_channel.RQ);
        drain_into_queue(champsim_channel.PQ);
        drain_into_queue(champsim_channel.WQ);

        // // Warn if more than one response was queued for this channel
        // if (lat_bw_queue.returned.size() > 1) {
        //     std::cerr << "Warning: channel returned queue has " << lat_bw_queue.returned.size() << " responses (expected <=1) at "
        //               << static_cast<const void*>(ul) << std::endl;
        // }
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
