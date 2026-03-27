#pragma once

#include <cstdint>
#include <functional>
#include <limits>

#include "SST_CS_packets.h"

namespace SST {
namespace csimCore {

class TrafficInjector {
public:
    static constexpr uint64_t kTraceTagBit = 1ULL << 63;

    void configure(double bytes_per_cycle,
                   uint64_t load_pct,
                   uint32_t node_id,
                   uint32_t dst_node,
                   uint64_t addr_base,
                   uint64_t addr_size);

    bool enabled() const { return enabled_; }
    bool owns_response(const sst_response& resp) const {
        return enabled_ && ((resp.trace_tag & kTraceTagBit) != 0);
    }

    void tick(const std::function<bool(const sst_request&)>& send_request);
    void note_response(const sst_response& resp);
    void reset_stats();
    uint64_t request_bytes_sent() const { return request_bytes_sent_; }
    uint64_t response_bytes_received() const { return response_bytes_received_; }

private:
    bool enabled_ = false;
    double bytes_per_cycle_ = 0.0;
    double byte_budget_ = 0.0;
    uint64_t load_pct_ = 100;
    uint64_t mix_phase_ = 0;
    uint64_t next_trace_tag_ = 1;
    uint32_t node_id_ = 0;
    uint32_t dst_node_ = std::numeric_limits<uint32_t>::max();
    uint64_t addr_base_ = 0;
    uint64_t addr_size_ = 0;
    uint64_t next_addr_ = 0;
    uint64_t request_bytes_sent_ = 0;
    uint64_t response_bytes_received_ = 0;
};

} // namespace csimCore
} // namespace SST
