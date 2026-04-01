#include "traffic_injector.h"

#include <algorithm>

namespace SST {
namespace csimCore {

namespace {
constexpr uint64_t kStartDelayStepCycles = 3;
constexpr uint64_t kStartDelaySlots[] = {3, 0, 6, 1, 7, 4, 2, 5};

double request_rate_per_cycle(double bytes_per_cycle, uint64_t load_pct)
{
    const double load_frac = static_cast<double>(std::min<uint64_t>(load_pct, 100)) / 100.0;
    const double avg_request_bytes = (8.0 * load_frac) + (64.0 * (1.0 - load_frac));
    if (avg_request_bytes <= 0.0) {
        return 0.0;
    }
    return bytes_per_cycle / avg_request_bytes;
}
}

void TrafficInjector::configure(double bytes_per_cycle,
                                uint64_t load_pct,
                                uint32_t node_id,
                                uint32_t dst_node,
                                uint64_t addr_base,
                                uint64_t addr_size)
{
    enabled_ = (bytes_per_cycle > 0.0) && (addr_size > 0);
    const uint64_t clamped_load_pct = std::min<uint64_t>(load_pct, 100);
    const double reqs_per_cycle = request_rate_per_cycle(bytes_per_cycle, clamped_load_pct);
    load_requests_per_cycle_ = reqs_per_cycle * (static_cast<double>(clamped_load_pct) / 100.0);
    store_requests_per_cycle_ = reqs_per_cycle - load_requests_per_cycle_;
    load_request_budget_ = 0.0;
    store_request_budget_ = 0.0;
    next_trace_tag_ = 1;
    node_id_ = node_id;
    dst_node_ = dst_node;
    addr_base_ = addr_base;
    addr_size_ = addr_size;
    start_delay_cycles_ =
        kStartDelaySlots[static_cast<std::size_t>(node_id_) % std::size(kStartDelaySlots)] *
        kStartDelayStepCycles;
    next_addr_ = addr_base_;
    reset_stats();
}

void TrafficInjector::tick(const std::function<bool(const sst_request&)>& send_request)
{
    if (!enabled_) {
        return;
    }

    if (start_delay_cycles_ > 0) {
        --start_delay_cycles_;
        return;
    }

    load_request_budget_ += load_requests_per_cycle_;
    store_request_budget_ += store_requests_per_cycle_;
    while (true) {
        const bool load_ready = load_request_budget_ + 1e-9 >= 1.0;
        const bool store_ready = store_request_budget_ + 1e-9 >= 1.0;
        if (!load_ready && !store_ready) {
            break;
        }

        bool is_load = false;
        if (load_ready && !store_ready) {
            is_load = true;
        } else if (!load_ready && store_ready) {
            is_load = false;
        } else {
            const double load_lag =
                (load_requests_per_cycle_ > 0.0) ? (load_request_budget_ / load_requests_per_cycle_) :
                                                   0.0;
            const double store_lag =
                (store_requests_per_cycle_ > 0.0) ? (store_request_budget_ / store_requests_per_cycle_) :
                                                    0.0;
            is_load = (load_lag >= store_lag);
        }

        const uint16_t req_bytes = is_load ? 8 : 64;

        sst_request req;
        req.src_node = node_id_;
        req.dst_node = dst_node_;
        req.type = is_load ? access_type::LOAD : access_type::WRITE;
        req.response_requested = is_load;
        req.cpu = 0;
        req.sst_cpu = node_id_;
        req.address = next_addr_;
        req.v_address = next_addr_;
        req.msg_bytes = req_bytes;
        req.trace_tag = kTraceTagBit | next_trace_tag_;

        if (!send_request(req)) {
            break;
        }

        request_bytes_sent_ += req.msg_bytes;
        if (is_load) {
            load_request_budget_ = std::max(0.0, load_request_budget_ - 1.0);
        } else {
            store_request_budget_ = std::max(0.0, store_request_budget_ - 1.0);
        }
        next_trace_tag_++;
        next_addr_ += 64;
        if (next_addr_ >= addr_base_ + addr_size_) {
            next_addr_ = addr_base_;
        }
    }
}

void TrafficInjector::note_response(const sst_response& resp)
{
    if (!owns_response(resp)) {
        return;
    }
    response_bytes_received_ += resp.msg_bytes;
}

void TrafficInjector::reset_stats()
{
    request_bytes_sent_ = 0;
    response_bytes_received_ = 0;
}

} // namespace csimCore
} // namespace SST
