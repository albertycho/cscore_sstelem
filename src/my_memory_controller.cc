#include "my_memory_controller.h"

#include <algorithm>

MY_MEMORY_CONTROLLER::MY_MEMORY_CONTROLLER() {}

MY_MEMORY_CONTROLLER::MY_MEMORY_CONTROLLER(champsim::chrono::picoseconds mc_period,
                                           std::vector<channel_type*>&& ul, int64_t bw_cycles_per_req,
                                           latency_function_type&& latency_function,
                                           champsim::data::bytes size,
                                           int64_t max_pending_requests)
    : operable(mc_period)
    , queues(std::move(ul))
    , lat_bw_queues(
        queues.size(), 
        lat_bw_queue_type(
            /*peak_bw_per_cycle=*/(bw_cycles_per_req > 0
                ? (1.0 / static_cast<double>(bw_cycles_per_req))
                : 0.0),
            /*latency_function=*/std::forward<latency_function_type>(latency_function),
            /*bw_cost_fn=*/[](const channel_type::request_type&) { return 1.0; },
            /*max_pending_bytes=*/max_pending_requests))
    , size_(size)
{
}

void MY_MEMORY_CONTROLLER::initialize()
{
    // minimal initialization
}

long MY_MEMORY_CONTROLLER::operate()
{
    long progress = 0;

    for(size_t i = 0; i < queues.size(); ++i) {
        auto& lat_bw_queue = lat_bw_queues[i];
        lat_bw_queue.tick();

        if(queues[i] == nullptr) {
            auto completed_requests = lat_bw_queue.drain_ready_if([](const channel_type::request_type& req) {
                return !req.response_requested;
            });
            progress += static_cast<long>(completed_requests.size());
            continue;
        }
        auto& champsim_channel = *queues[i];

        // Get fulfilled requests
        auto completed_requests = lat_bw_queue.drain_ready();
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
                if (!lat_bw_queue.add_packet(std::move(q.front()))) {
                    break;
                }
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

bool MY_MEMORY_CONTROLLER::enqueue_request(std::size_t idx, channel_type::request_type req)
{
    return idx < lat_bw_queues.size() && lat_bw_queues[idx].add_packet(std::move(req));
}

bool MY_MEMORY_CONTROLLER::has_ready_response(std::size_t idx) const
{
    return idx < lat_bw_queues.size() && lat_bw_queues[idx].has_ready();
}

const MY_MEMORY_CONTROLLER::channel_type::request_type&
MY_MEMORY_CONTROLLER::front_ready_response(std::size_t idx) const
{
    return lat_bw_queues[idx].front_ready();
}

MY_MEMORY_CONTROLLER::channel_type::request_type
MY_MEMORY_CONTROLLER::pop_ready_response(std::size_t idx)
{
    return lat_bw_queues[idx].pop_ready();
}

void MY_MEMORY_CONTROLLER::begin_phase()
{
}

void MY_MEMORY_CONTROLLER::end_phase(unsigned cpu)
{
}

void MY_MEMORY_CONTROLLER::print_deadlock()
{
}
// namespace champsim
