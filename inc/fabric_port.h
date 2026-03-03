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
#include "control_event.h"
#include "csEvent.h"
#include "lat_bw_queue.h"

extern template class lat_bw_queue<SST::csimCore::csEvent*>;

namespace SST {
namespace csimCore {

// Construct a control event that resets utilization counters.
csEvent* make_reset_util_event(uint64_t src, uint64_t dst);

// FabricPort is a minimal API wrapper around a bidirectional SST link with:
//  - ingress latency/bandwidth modeling
//  - credit-based egress backpressure
//
// Usage (csEvent* only):
//   port.configure(link, self_id, bw_cycles, lat_cycles, queue_size_packets);
//   port.send(ev);                 // returns false if egress queue full
//   auto ev = port.receive(cyc);   // returns at most one message per cycle
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
    //   self_id            Node id for credit/control messages.
    //   bw_cycles          Cycles per 64B for ingress bandwidth modeling (0 disables).
    //   lat_cycles         Base latency in cycles for ingress modeling (0 disables).
    //   queue_size_packets Ingress queue capacity in packets (0 = unbounded).
    //                     Egress uses this as credit capacity (0 = unbounded).
    void configure(SST::Link* link,
                   uint64_t self_id,
                   int64_t bw_cycles,
                   int64_t lat_cycles,
                   int64_t queue_size_packets);

    [[nodiscard]] bool send(csEvent* item);

    // Returns at most one message per cycle for this port.
    std::optional<csEvent*> receive(uint64_t cycle);
    bool try_receive(uint64_t cycle,
                     const std::function<bool(csEvent*)>& handle);

    void handle_event(SST::Event* ev);

    void reset_ingress_utilization();
    bool has_ingress() const;
    bool has_link() const;
    bool can_send() const;
    double ingress_avg_utilization() const;
    double ingress_utilization() const;
    std::size_t ingress_occupancy() const;

private:
    bool can_receive(uint64_t cycle);
    void tick_ingress();
    void drain_egress();
    void send_credit(csEvent* const& item);

    SST::Link* link_ = nullptr;
    uint64_t self_id_ = 0;
    std::unique_ptr<::lat_bw_queue<csEvent*>> ingress_;
    int64_t egress_credits_ = 0;
    int64_t egress_credit_cap_ = 0;
    std::size_t egress_queue_max_ = 0;
    std::deque<csEvent*> egress_queue_;
    std::deque<csEvent*> ready_;
    uint64_t last_tick_cycle_ = std::numeric_limits<uint64_t>::max();
    uint64_t last_deliver_cycle_ = std::numeric_limits<uint64_t>::max();

    bool egress_queue_full() const;
};

} // namespace csimCore
} // namespace SST
