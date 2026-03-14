#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    double ingress_wait_avg_cycles(TrafficClass cls) const;
    uint64_t ingress_wait_max_cycles(TrafficClass cls) const;
    double ingress_queue_wait_avg_cycles(TrafficClass cls) const;
    uint64_t ingress_queue_wait_max_cycles(TrafficClass cls) const;
    double egress_wait_avg_cycles() const;
    uint64_t egress_wait_max_cycles() const;
    double egress_occ_avg_bytes() const;
    double egress_occ_stddev_bytes() const;
    uint64_t egress_occ_max_bytes() const;
    double egress_occ_nonempty_frac() const;
    uint64_t egress_blocked_cycles() const;
    double egress_blocked_nonempty_frac() const;
    double egress_blocked_avg_occ_bytes() const;
    uint64_t egress_blocked_max_occ_bytes() const;
    uint64_t egress_send_burst_max_pkts() const;
    uint64_t egress_send_burst_max_bytes() const;
    double egress_send_burst_avg_pkts() const;
    double egress_send_burst_avg_bytes() const;
    double egress_send_burst_stddev_pkts() const;
    double egress_send_burst_stddev_bytes() const;
    double egress_send_nonempty_frac() const;
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
    double egress_wait_avg_cycles(TrafficClass cls) const;
    uint64_t egress_wait_max_cycles(TrafficClass cls) const;
    double egress_occ_avg_bytes(TrafficClass cls) const;
    uint64_t egress_occ_max_bytes(TrafficClass cls) const;
    uint64_t egress_blocked_cycles(TrafficClass cls) const;
    uint64_t ingress_arrival_empty_packets(TrafficClass cls) const;
    uint64_t ingress_arrival_nonempty_packets(TrafficClass cls) const;
    double ingress_arrival_nonempty_packet_frac(TrafficClass cls) const;
    double ingress_nonempty_arrival_pre_occ_avg_bytes(TrafficClass cls) const;
    uint64_t ingress_nonempty_arrival_pre_occ_max_bytes(TrafficClass cls) const;
    uint64_t ingress_release_after_empty_arrival_packets(TrafficClass cls) const;
    uint64_t ingress_release_after_nonempty_arrival_packets(TrafficClass cls) const;
    double ingress_wait_after_empty_arrival_avg_cycles(TrafficClass cls) const;
    double ingress_wait_after_nonempty_arrival_avg_cycles(TrafficClass cls) const;
    double ingress_queue_wait_after_empty_arrival_avg_cycles(TrafficClass cls) const;
    double ingress_queue_wait_after_nonempty_arrival_avg_cycles(TrafficClass cls) const;
    uint64_t ingress_queue_wait_after_empty_arrival_max_cycles(TrafficClass cls) const;
    uint64_t ingress_queue_wait_after_nonempty_arrival_max_cycles(TrafficClass cls) const;
    void emit_deep_diagnostics(std::ostream& os, const std::string& prefix) const;

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
    void record_ingress_wait(TrafficClass cls, uint64_t wait_cycles, uint64_t queue_wait_cycles);
    void record_conditional_ingress_arrival(TrafficClass cls,
                                            uint64_t bytes,
                                            uint64_t pre_enqueue_occ_bytes,
                                            uint64_t src,
                                            uint64_t dst);
    void record_conditional_ingress_release(TrafficClass cls,
                                            uint64_t src,
                                            uint64_t dst,
                                            bool saw_nonempty_queue,
                                            uint64_t bytes,
                                            uint64_t wait_cycles,
                                            uint64_t queue_wait_cycles);
    static std::size_t byte_bucket_index(uint64_t bytes);
    static std::size_t cycle_bucket_index(uint64_t cycles);
    static const char* byte_bucket_name(std::size_t idx);
    static const char* cycle_bucket_name(std::size_t idx);
    static constexpr std::size_t traffic_class_index(TrafficClass cls) {
        return static_cast<std::size_t>(cls);
    }
    static constexpr std::size_t kDiagBucketCount = 6;
    struct PeerClassDiag {
        uint64_t rx_bytes = 0;
        uint64_t rx_packets = 0;
        uint64_t tx_bytes = 0;
        uint64_t tx_packets = 0;
        uint64_t ingress_arrivals = 0;
        uint64_t ingress_nonempty_arrivals = 0;
        uint64_t ingress_nonempty_pre_occ_sum_bytes = 0;
        uint64_t ingress_nonempty_pre_occ_max_bytes = 0;
        uint64_t ingress_wait_sum_cycles = 0;
        uint64_t ingress_wait_samples = 0;
        uint64_t ingress_wait_max_cycles = 0;
        uint64_t ingress_queue_wait_sum_cycles = 0;
        uint64_t ingress_queue_wait_samples = 0;
        uint64_t ingress_queue_wait_max_cycles = 0;
        uint64_t ingress_wait_after_nonempty_sum_cycles = 0;
        uint64_t ingress_wait_after_nonempty_samples = 0;
        uint64_t ingress_queue_wait_after_nonempty_sum_cycles = 0;
        uint64_t ingress_queue_wait_after_nonempty_samples = 0;
        std::array<uint64_t, kDiagBucketCount> pre_occ_bucket_counts{};
        std::array<uint64_t, kDiagBucketCount> queue_wait_bucket_counts{};
    };
    struct PeerDiag {
        std::array<PeerClassDiag, static_cast<std::size_t>(TrafficClass::Count)> classes{};
    };
    struct EpisodeDiag {
        uint64_t count = 0;
        uint64_t sum_cycles = 0;
        uint64_t max_cycles = 0;
        uint64_t sum_peak_occ_bytes = 0;
        uint64_t max_peak_occ_bytes = 0;
        uint64_t sum_arrival_pkts = 0;
        uint64_t max_arrival_pkts = 0;
        uint64_t sum_arrival_bytes = 0;
        uint64_t max_arrival_bytes = 0;
        uint64_t sum_release_pkts = 0;
        uint64_t max_release_pkts = 0;
        uint64_t sum_release_bytes = 0;
        uint64_t max_release_bytes = 0;
        uint64_t sum_wait_cycles = 0;
        uint64_t max_wait_cycles = 0;
        uint64_t sum_queue_wait_cycles = 0;
        uint64_t max_queue_wait_cycles = 0;
        uint64_t sum_src_distinct = 0;
        uint64_t max_src_distinct = 0;
        uint64_t sum_dst_distinct = 0;
        uint64_t max_dst_distinct = 0;
        std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> sum_arrival_pkts_by_class{};
        std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> max_arrival_pkts_by_class{};
        std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> sum_release_pkts_by_class{};
        std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> max_release_pkts_by_class{};
    };
    struct EpisodeState {
        uint64_t cycles = 0;
        uint64_t peak_occ_bytes = 0;
        uint64_t arrival_pkts = 0;
        uint64_t arrival_bytes = 0;
        uint64_t release_pkts = 0;
        uint64_t release_bytes = 0;
        uint64_t total_wait_cycles = 0;
        uint64_t max_wait_cycles = 0;
        uint64_t total_queue_wait_cycles = 0;
        uint64_t max_queue_wait_cycles = 0;
        std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> arrival_pkts_by_class{};
        std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> release_pkts_by_class{};
        std::unordered_set<uint64_t> srcs{};
        std::unordered_set<uint64_t> dsts{};
    };

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
    struct IngressArrivalMeta {
        uint64_t enqueue_cycle = 0;
        uint64_t pre_enqueue_occ_bytes = 0;
        uint64_t src = 0;
        uint64_t dst = 0;
        TrafficClass cls = TrafficClass::OtherReq;
        bool saw_nonempty_queue = false;
    };
    std::deque<EgressEntry> egress_queue_;
    std::deque<csEvent*> ready_;
    std::unordered_map<csEvent*, IngressArrivalMeta> ingress_enqueue_cycle_;
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
    std::array<uint64_t, kDiagBucketCount> ready_occ_bucket_counts_{};
    uint64_t ingress_wait_sum_cycles_ = 0;
    uint64_t ingress_wait_samples_ = 0;
    uint64_t ingress_wait_max_cycles_ = 0;
    uint64_t ingress_queue_wait_sum_cycles_ = 0;
    uint64_t ingress_queue_wait_samples_ = 0;
    uint64_t ingress_queue_wait_max_cycles_ = 0;
    uint64_t egress_wait_sum_cycles_ = 0;
    uint64_t egress_wait_samples_ = 0;
    uint64_t egress_wait_max_cycles_ = 0;
    uint64_t egress_occ_sum_bytes_ = 0;
    long double egress_occ_sq_sum_bytes_ = 0.0L;
    uint64_t egress_occ_nonempty_cycles_ = 0;
    uint64_t egress_occ_max_bytes_ = 0;
    uint64_t egress_blocked_cycles_ = 0;
    uint64_t egress_blocked_occ_sum_bytes_ = 0;
    uint64_t egress_blocked_occ_max_bytes_ = 0;
    uint64_t egress_send_burst_max_pkts_ = 0;
    uint64_t egress_send_burst_max_bytes_ = 0;
    uint64_t egress_send_nonempty_cycles_ = 0;
    uint64_t egress_send_burst_sum_pkts_ = 0;
    uint64_t egress_send_burst_sum_bytes_ = 0;
    long double egress_send_burst_sq_sum_pkts_ = 0.0L;
    long double egress_send_burst_sq_sum_bytes_ = 0.0L;
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
    std::array<uint64_t, kDiagBucketCount> ingress_occ_bucket_counts_{};
    uint64_t tick_samples_ = 0;
    uint64_t tx_bytes_total_ = 0;
    uint64_t rx_bytes_total_ = 0;
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> tx_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> rx_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> tx_packets_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> rx_packets_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_arrival_empty_packets_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_arrival_nonempty_packets_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_arrival_empty_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_arrival_nonempty_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_nonempty_pre_occ_sum_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_nonempty_pre_occ_max_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_release_after_empty_packets_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_release_after_nonempty_packets_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_wait_after_empty_sum_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_wait_after_nonempty_sum_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_queue_wait_after_empty_sum_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_queue_wait_after_nonempty_sum_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_queue_wait_after_empty_max_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_queue_wait_after_nonempty_max_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_wait_sum_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_wait_samples_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_wait_max_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_queue_wait_sum_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_queue_wait_samples_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> ingress_queue_wait_max_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> egress_wait_sum_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> egress_wait_samples_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> egress_wait_max_cycles_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> egress_queue_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> egress_occ_sum_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> egress_occ_max_bytes_by_class_{};
    std::array<uint64_t, static_cast<std::size_t>(TrafficClass::Count)> egress_blocked_cycles_by_class_{};
    std::array<std::array<uint64_t, kDiagBucketCount>, static_cast<std::size_t>(TrafficClass::Count)> ingress_pre_occ_bucket_counts_by_class_{};
    std::array<std::array<uint64_t, kDiagBucketCount>, static_cast<std::size_t>(TrafficClass::Count)> ingress_queue_wait_bucket_counts_by_class_{};
    std::unordered_map<uint64_t, PeerDiag> rx_by_src_peer_{};
    std::unordered_map<uint64_t, PeerDiag> rx_by_dst_peer_{};
    std::unordered_map<uint64_t, PeerDiag> tx_by_src_peer_{};
    std::unordered_map<uint64_t, PeerDiag> tx_by_dst_peer_{};
    EpisodeDiag ingress_episode_diag_{};
    EpisodeState ingress_episode_state_{};
    bool ingress_episode_active_ = false;

    bool egress_queue_full(uint64_t bytes) const;
};

} // namespace csimCore
} // namespace SST
