#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>

#include <sst/core/link.h>

#include "SST_CS_packets.h"
#include "csEvent.h"
#include "lat_bw_queue.h"

namespace SST {
namespace csimCore {

class Switch;

// FabricPort is a minimal API wrapper around a bidirectional SST link with:
//  - ingress latency/bandwidth modeling
//  - credit-based egress backpressure
//
// Usage (csEvent* only):
//   port.configure(link, self_id, bw_cycles, lat_cycles,
//                  egress_buffer_bytes, credit_window_bytes);
//   port.advance(cyc);
//   port.try_receive_ready(cyc, handle); // consume at most one already-ready head
//   port.send(ev);                        // returns false if egress queue full
class FabricPort {
private:
public:
    FabricPort();
    ~FabricPort();
    FabricPort(const FabricPort&) = delete;
    FabricPort& operator=(const FabricPort&) = delete;
    FabricPort(FabricPort&&) = default;
    FabricPort& operator=(FabricPort&&) = default;

    // Configure the port using default type traits for conversion/bytes.
    // Parameters:
    //   link               SST link pointer (may be null; send() will fail).
    //   self_id            Node id for returned-credit messages.
    //   bw_cycles          Cycles per 64B for ingress bandwidth modeling (0 disables bandwidth shaping).
    //   lat_cycles         Base latency in cycles for ingress modeling (0 disables latency shaping).
    //                     If both bw_cycles and lat_cycles are 0, ingress queue/timing is bypassed.
    //   egress_buffer_bytes Egress queue capacity in bytes (0 = unbounded).
    //   credit_window_bytes Returned-credit window in bytes (0 = unbounded).
    //                     Ingress is unbounded to avoid drops; credits gate senders.
    void configure(SST::Link* link,
                   uint64_t self_id,
                   int64_t bw_cycles,
                   int64_t lat_cycles,
                   int64_t egress_buffer_bytes,
                   int64_t credit_window_bytes);

    [[nodiscard]] bool send(csEvent* item);

    void advance(uint64_t cycle);
    // Present the current ready head to the callback. A true return consumes
    // the message and returns credits upstream; a false return leaves the head
    // in place so the caller can retry later.
    bool try_receive_ready(uint64_t cycle,
                           const std::function<bool(csEvent*)>& handle);

    void handle_event(SST::Event* ev);

    double ingress_avg_utilization() const;
    double ingress_utilization() const;
    std::size_t ingress_occupancy() const;
    uint64_t tx_bytes_total() const;
    uint64_t rx_bytes_total() const;
    void reset_counters();

private:
    friend class Switch;

    bool has_ready_to_receive(uint64_t cycle) const;
    void tick_ingress();
    void drain_egress();
    void send_credit(uint64_t dst, uint64_t bytes);
    bool can_enqueue(uint64_t bytes) const;
    bool can_enqueue(const csEvent* item) const;

    SST::Link* link_ = nullptr;
    uint64_t self_id_ = 0;
    std::unique_ptr<::lat_bw_queue<csEvent*>> ingress_;
    int64_t egress_credits_ = 0;
    int64_t egress_credit_cap_ = 0;
    int64_t egress_queue_max_bytes_ = 0;
    int64_t egress_queue_bytes_ = 0;
    std::deque<csEvent*> egress_queue_;
    std::deque<csEvent*> ready_;
    uint64_t last_tick_cycle_ = std::numeric_limits<uint64_t>::max();
    uint64_t last_deliver_cycle_ = std::numeric_limits<uint64_t>::max();
    uint64_t tx_bytes_total_ = 0;
    uint64_t rx_bytes_total_ = 0;

    bool egress_queue_full(uint64_t bytes) const;
};

} // namespace csimCore
} // namespace SST
