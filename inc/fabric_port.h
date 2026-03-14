#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <sst/core/link.h>

#include "SST_CS_packets.h"
#include "control_event.h"
#include "csEvent.h"
#include "lat_bw_queue.h"

namespace SST {
namespace csimCore {

// Construct a control event that resets utilization counters.
csEvent* make_reset_util_event(uint64_t src, uint64_t dst);

// FabricPort is a minimal API wrapper around a bidirectional SST link with:
//  - ingress latency/bandwidth modeling
//  - credit-based egress backpressure
//
// Usage (csEvent* only):
//   port.configure(link, self_id, bw_cycles, lat_cycles, queue_size_bytes);
//   port.send(ev);                 // returns false if egress queue full
//   auto ev = port.receive(cyc);   // returns at most one message per cycle
class FabricPort {
private:
public:
    enum class TrafficClass : std::size_t {
        DemandReq = 0,
        WriteReq = 1,
        Response = 2,
        OtherReq = 3,
        Count = 4,
    };

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
    //   bw_cycles          Cycles per 64B for ingress bandwidth modeling (0 disables bandwidth shaping).
    //   lat_cycles         Base latency in cycles for ingress modeling (0 disables latency shaping).
    //                     If both bw_cycles and lat_cycles are 0, ingress queue/timing is bypassed.
    //   queue_size_bytes   Egress credit capacity in bytes (0 = unbounded).
    //                     Ingress is unbounded to avoid drops; credits gate senders.
    void configure(SST::Link* link,
                   uint64_t self_id,
                   int64_t bw_cycles,
                   int64_t lat_cycles,
                   int64_t queue_size_bytes);

    [[nodiscard]] bool send(csEvent* item);

    // Returns at most one message per cycle for this port.
    void tick(uint64_t cycle);
    std::optional<csEvent*> receive(uint64_t cycle);
    bool try_receive(uint64_t cycle,
                     const std::function<bool(csEvent*)>& handle);

    void handle_event(SST::Event* ev);

    void reset_ingress_utilization();
    bool can_send() const;
    bool can_send(uint64_t bytes) const;
    bool can_send(const csEvent* item) const;
    double ingress_avg_utilization() const;
    double ingress_utilization() const;
    std::size_t ingress_occupancy() const;
    std::size_t ready_occupancy() const;
    double ready_occupancy_avg() const;
    std::size_t ready_occupancy_max() const;
    double ingress_wait_avg_cycles() const;
    uint64_t ingress_wait_max_cycles() const;
    double ingress_queue_wait_avg_cycles() const;
    uint64_t ingress_queue_wait_max_cycles() const;
    double ready_wait_avg_cycles() const;
    uint64_t ready_wait_max_cycles() const;
    uint64_t ready_retry_count() const;
    double egress_wait_avg_cycles() const;
    uint64_t egress_wait_max_cycles() const;
    uint64_t ingress_arrival_burst_max_pkts() const;
    uint64_t ingress_arrival_burst_max_bytes() const;
    double ingress_arrival_burst_avg_pkts() const;
    double ingress_arrival_burst_avg_bytes() const;
    double ingress_arrival_burst_stddev_pkts() const;
    double ingress_arrival_burst_stddev_bytes() const;
    double ingress_arrival_nonempty_frac() const;
    uint64_t ingress_arrival_run_max_cycles() const;
    double ingress_arrival_run_avg_cycles() const;
    double ingress_arrival_run_stddev_cycles() const;
    uint64_t ingress_arrival_gap_max_cycles() const;
    double ingress_arrival_gap_avg_cycles() const;
    double ingress_arrival_gap_stddev_cycles() const;
    uint64_t ingress_release_burst_max_pkts() const;
    uint64_t ingress_release_burst_max_bytes() const;
    double ingress_release_burst_avg_pkts() const;
    double ingress_release_burst_avg_bytes() const;
    double ingress_release_burst_stddev_pkts() const;
    double ingress_release_burst_stddev_bytes() const;
    double ingress_release_nonempty_frac() const;
    uint64_t ingress_release_run_max_cycles() const;
    double ingress_release_run_avg_cycles() const;
    double ingress_release_run_stddev_cycles() const;
    uint64_t ingress_release_gap_max_cycles() const;
    double ingress_release_gap_avg_cycles() const;
    double ingress_release_gap_stddev_cycles() const;
    double ingress_occ_avg_bytes() const;
    double ingress_occ_stddev_bytes() const;
    uint64_t ingress_occ_max_bytes() const;
    double ingress_occ_nonempty_frac() const;
    uint64_t ingress_occ_run_max_cycles() const;
    double ingress_occ_run_avg_cycles() const;
    double ingress_occ_run_stddev_cycles() const;
    uint64_t ingress_occ_gap_max_cycles() const;
    double ingress_occ_gap_avg_cycles() const;
    double ingress_occ_gap_stddev_cycles() const;
    uint64_t tx_bytes_total() const;
    uint64_t rx_bytes_total() const;
    uint64_t tx_bytes(TrafficClass cls) const;
    uint64_t rx_bytes(TrafficClass cls) const;
    uint64_t tx_packets(TrafficClass cls) const;
    uint64_t rx_packets(TrafficClass cls) const;

private:
    bool can_receive(uint64_t cycle);
    void tick_ingress();
    void drain_egress();
    void send_credit(uint64_t dst, uint64_t bytes);
    uint64_t ingress_service_floor_cycles(const csEvent* item) const;
    void push_ready(csEvent* item, bool front = false);
    void record_ready_pop(uint64_t cycle, csEvent* item);
    static TrafficClass classify_event(const csEvent* item);
    void record_rx(const csEvent* item);
    void record_tx(const csEvent* item);
    void record_ingress_arrival(uint64_t cycle, uint64_t bytes);
    void record_ingress_release(const std::vector<csEvent*>& ready);

    SST::Link* link_ = nullptr;
    uint64_t self_id_ = 0;
    std::unique_ptr<::lat_bw_queue<csEvent*>> ingress_;
    int64_t ingress_bw_cycles_ = 0;
    int64_t ingress_lat_cycles_ = 0;
    int64_t egress_credits_ = 0;
    int64_t egress_credit_cap_ = 0;
    int64_t egress_queue_max_bytes_ = 0;
    int64_t egress_queue_bytes_ = 0;
    struct EgressEntry {
        csEvent* ev = nullptr;
        uint64_t enqueue_cycle = 0;
    };
    std::deque<EgressEntry> egress_queue_;
    std::deque<csEvent*> ready_;
    std::unordered_map<csEvent*, uint64_t> ingress_enqueue_cycle_;
    std::unordered_map<csEvent*, uint64_t> ready_enqueue_cycle_;
    uint64_t last_tick_cycle_ = std::numeric_limits<uint64_t>::max();
    uint64_t last_deliver_cycle_ = std::numeric_limits<uint64_t>::max();
    uint64_t ready_wait_sum_cycles_ = 0;
    uint64_t ready_wait_samples_ = 0;
    uint64_t ready_wait_max_cycles_ = 0;
    uint64_t ready_retry_count_ = 0;
    uint64_t ready_occ_sum_ = 0;
    uint64_t ready_occ_samples_ = 0;
    std::size_t ready_occ_max_ = 0;
    uint64_t ingress_wait_sum_cycles_ = 0;
    uint64_t ingress_wait_samples_ = 0;
    uint64_t ingress_wait_max_cycles_ = 0;
    uint64_t ingress_queue_wait_sum_cycles_ = 0;
    uint64_t ingress_queue_wait_samples_ = 0;
    uint64_t ingress_queue_wait_max_cycles_ = 0;
    uint64_t egress_wait_sum_cycles_ = 0;
    uint64_t egress_wait_samples_ = 0;
    uint64_t egress_wait_max_cycles_ = 0;
    uint64_t ingress_arrival_burst_cycle_ = std::numeric_limits<uint64_t>::max();
    uint64_t ingress_arrival_burst_pkts_cur_ = 0;
    uint64_t ingress_arrival_burst_bytes_cur_ = 0;
    uint64_t ingress_arrival_burst_max_pkts_ = 0;
    uint64_t ingress_arrival_burst_max_bytes_ = 0;
    uint64_t ingress_arrival_nonempty_cycles_ = 0;
    uint64_t ingress_arrival_burst_sum_pkts_ = 0;
    uint64_t ingress_arrival_burst_sum_bytes_ = 0;
    long double ingress_arrival_burst_sq_sum_pkts_ = 0.0L;
    long double ingress_arrival_burst_sq_sum_bytes_ = 0.0L;
    uint64_t ingress_arrival_prev_cycle_ = std::numeric_limits<uint64_t>::max();
    uint64_t ingress_arrival_run_cur_cycles_ = 0;
    uint64_t ingress_arrival_run_max_cycles_ = 0;
    uint64_t ingress_arrival_run_count_ = 0;
    uint64_t ingress_arrival_run_sum_cycles_ = 0;
    long double ingress_arrival_run_sq_sum_cycles_ = 0.0L;
    uint64_t ingress_arrival_gap_max_cycles_ = 0;
    uint64_t ingress_arrival_gap_count_ = 0;
    uint64_t ingress_arrival_gap_sum_cycles_ = 0;
    long double ingress_arrival_gap_sq_sum_cycles_ = 0.0L;
    uint64_t ingress_release_burst_max_pkts_ = 0;
    uint64_t ingress_release_burst_max_bytes_ = 0;
    uint64_t ingress_release_nonempty_cycles_ = 0;
    uint64_t ingress_release_burst_sum_pkts_ = 0;
    uint64_t ingress_release_burst_sum_bytes_ = 0;
    long double ingress_release_burst_sq_sum_pkts_ = 0.0L;
    long double ingress_release_burst_sq_sum_bytes_ = 0.0L;
    bool ingress_release_seen_nonempty_ = false;
    uint64_t ingress_release_run_cur_cycles_ = 0;
    uint64_t ingress_release_run_max_cycles_ = 0;
    uint64_t ingress_release_run_count_ = 0;
    uint64_t ingress_release_run_sum_cycles_ = 0;
    long double ingress_release_run_sq_sum_cycles_ = 0.0L;
    uint64_t ingress_release_gap_cur_cycles_ = 0;
    uint64_t ingress_release_gap_max_cycles_ = 0;
    uint64_t ingress_release_gap_count_ = 0;
    uint64_t ingress_release_gap_sum_cycles_ = 0;
    long double ingress_release_gap_sq_sum_cycles_ = 0.0L;
    uint64_t ingress_occ_sum_bytes_ = 0;
    long double ingress_occ_sq_sum_bytes_ = 0.0L;
    uint64_t ingress_occ_samples_ = 0;
    uint64_t ingress_occ_nonempty_cycles_ = 0;
    uint64_t ingress_occ_max_bytes_ = 0;
    bool ingress_occ_seen_nonempty_ = false;
    uint64_t ingress_occ_run_cur_cycles_ = 0;
    uint64_t ingress_occ_run_max_cycles_ = 0;
    uint64_t ingress_occ_run_count_ = 0;
    uint64_t ingress_occ_run_sum_cycles_ = 0;
    long double ingress_occ_run_sq_sum_cycles_ = 0.0L;
    uint64_t ingress_occ_gap_cur_cycles_ = 0;
    uint64_t ingress_occ_gap_max_cycles_ = 0;
    uint64_t ingress_occ_gap_count_ = 0;
    uint64_t ingress_occ_gap_sum_cycles_ = 0;
    long double ingress_occ_gap_sq_sum_cycles_ = 0.0L;
    uint64_t tick_samples_ = 0;
    uint64_t tx_bytes_total_ = 0;
    uint64_t rx_bytes_total_ = 0;
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> tx_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> rx_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> tx_packets_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> rx_packets_by_class_{};

    bool egress_queue_full(uint64_t bytes) const;
};

} // namespace csimCore
} // namespace SST
