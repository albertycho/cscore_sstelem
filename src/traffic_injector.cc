#include "traffic_injector.h"

#include <algorithm>

namespace SST {
namespace csimCore {

namespace {
constexpr uint64_t kLineSize = 64;
}

void TrafficInjector::configure(double bytes_per_cycle,
                                uint64_t load_pct,
                                uint32_t node_id,
                                uint32_t dst_node,
                                uint64_t addr_base,
                                uint64_t addr_size)
{
    uint64_t inject_base = addr_base;
    uint64_t inject_size = addr_size;
    if (addr_size >= (2 * kLineSize)) {
        const uint64_t half_size = (addr_size / 2 / kLineSize) * kLineSize;
        if (half_size > 0) {
            inject_base = addr_base + half_size;
            inject_size = half_size;
        }
    }

    enabled_ = (bytes_per_cycle > 0.0) && (inject_size > 0);
    bytes_per_cycle_ = bytes_per_cycle;
    byte_budget_ = 0.0;
    load_pct_ = std::min<uint64_t>(load_pct, 100);
    mix_phase_ = 0;
    next_trace_tag_ = 1;
    node_id_ = node_id;
    dst_node_ = dst_node;
    addr_base_ = inject_base;
    addr_size_ = inject_size;
    next_addr_ = inject_base;
}

void TrafficInjector::tick(const std::function<bool(const sst_request&)>& send_request)
{
    if (!enabled_) {
        return;
    }

    byte_budget_ += bytes_per_cycle_;
    while (true) {
        const uint64_t next_mix_phase = mix_phase_ + load_pct_;
        const bool is_load = next_mix_phase >= 100;
        const uint16_t req_bytes = is_load ? 8 : 64;
        if (byte_budget_ + 1e-9 < static_cast<double>(req_bytes)) {
            break;
        }

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

        byte_budget_ -= static_cast<double>(req_bytes);
        mix_phase_ = is_load ? (next_mix_phase - 100) : next_mix_phase;
        next_trace_tag_++;
        next_addr_ += 64;
        if (next_addr_ >= addr_base_ + addr_size_) {
            next_addr_ = addr_base_;
        }
    }
}

} // namespace csimCore
} // namespace SST
