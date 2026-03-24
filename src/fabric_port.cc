#include "fabric_port.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "lat_bw_queue.h"

namespace SST {
namespace csimCore {
namespace {
constexpr uint64_t kDefaultMsgBytes = 64;
constexpr int64_t kInfiniteCredits = std::numeric_limits<int64_t>::max() / 4;
constexpr uint64_t kControlCredit = 2;

bool is_reset_control_event(const csEvent* ev) {
    return ev && ev->payload.size() >= 3 && ev->payload[2] == kControlResetUtil;
}

uint64_t msg_bytes(const csEvent& ev) {
    if (ev.payload.size() > 16) {
        return std::max<uint64_t>(ev.payload[16], 1);
    }
    if (ev.payload.size() > 8) {
        return std::max<uint64_t>(ev.payload[8], 1);
    }
    return kDefaultMsgBytes;
}

uint64_t event_bytes(const csEvent* ev) {
    return ev ? msg_bytes(*ev) : kDefaultMsgBytes;
}

uint64_t credit_bytes(const csEvent* ev) {
    if (ev && ::SST::csimCore::is_control_event(*ev)) {
        return 0;
    }
    return event_bytes(ev);
}

int64_t credit_capacity_bytes(int64_t queue_size_bytes) {
    if (queue_size_bytes > 0) {
        return queue_size_bytes;
    }
    return kInfiniteCredits;
}

void add_credit(int64_t& credits, int64_t capacity, uint64_t bytes) {
    if (credits == kInfiniteCredits || bytes == 0) {
        return;
    }
    credits = std::min<int64_t>(credits + static_cast<int64_t>(bytes), capacity);
}

bool try_consume_credit(int64_t& credits, uint64_t bytes) {
    if (bytes == 0) {
        return true;
    }
    if (credits == kInfiniteCredits) {
        return true;
    }
    if (credits < static_cast<int64_t>(bytes)) {
        return false;
    }
    credits -= static_cast<int64_t>(bytes);
    return true;
}

uint64_t event_credit_dst(const csEvent* ev) {
    if (ev && !ev->payload.empty()) {
        return ev->payload[0];
    }
    return kControlBroadcast;
}

csEvent* make_credit_event(uint64_t src, uint64_t dst, uint64_t bytes) {
    auto* ev = new csEvent();
    ev->payload.reserve(4);
    ev->payload.push_back(src);
    ev->payload.push_back(dst);
    ev->payload.push_back(kControlCredit);
    ev->payload.push_back(bytes);
    return ev;
}

double safe_stddev(long double sq_sum, long double sum, uint64_t count) {
    if (count == 0) {
        return 0.0;
    }
    const long double mean = sum / static_cast<long double>(count);
    const long double variance = std::max<long double>(sq_sum / static_cast<long double>(count) - mean * mean, 0.0L);
    return std::sqrt(static_cast<double>(variance));
}
} // namespace

csEvent* make_reset_util_event(uint64_t src, uint64_t dst) {
    return ::SST::csimCore::make_control_event(src, dst, kControlResetUtil);
}

FabricPort::FabricPort()
    : egress_credits_(kInfiniteCredits),
      egress_credit_cap_(kInfiniteCredits) {}
FabricPort::~FabricPort() = default;

std::size_t FabricPort::byte_bucket_index(uint64_t bytes) {
    if (bytes == 0) {
        return 0;
    }
    if (bytes <= 64) {
        return 1;
    }
    if (bytes <= 256) {
        return 2;
    }
    if (bytes <= 1024) {
        return 3;
    }
    if (bytes <= 4096) {
        return 4;
    }
    return 5;
}

std::size_t FabricPort::cycle_bucket_index(uint64_t cycles) {
    if (cycles == 0) {
        return 0;
    }
    if (cycles <= 63) {
        return 1;
    }
    if (cycles <= 255) {
        return 2;
    }
    if (cycles <= 1023) {
        return 3;
    }
    if (cycles <= 4095) {
        return 4;
    }
    return 5;
}

const char* FabricPort::byte_bucket_name(std::size_t idx) {
    static constexpr std::array<const char*, kDiagBucketCount> kNames{
        "0", "1_64", "65_256", "257_1024", "1025_4096", "4097_plus"
    };
    return idx < kNames.size() ? kNames[idx] : "unknown";
}

const char* FabricPort::cycle_bucket_name(std::size_t idx) {
    static constexpr std::array<const char*, kDiagBucketCount> kNames{
        "0", "1_63", "64_255", "256_1023", "1024_4095", "4096_plus"
    };
    return idx < kNames.size() ? kNames[idx] : "unknown";
}

void FabricPort::configure(SST::Link* link,
                           uint64_t self_id,
                           int64_t bw_cycles,
                           int64_t lat_cycles,
                           int64_t queue_size_bytes) {
    if (bw_cycles < 0 || lat_cycles < 0 || queue_size_bytes < 0) {
        throw std::runtime_error("FabricPort: negative configuration values are invalid.");
    }
    if (!link) {
        throw std::runtime_error("FabricPort: null link provided to configure.");
    }
    link_ = link;
    self_id_ = self_id;
    ingress_bw_cycles_ = bw_cycles;
    ingress_lat_cycles_ = lat_cycles;
    const bool bw_enabled = (bw_cycles > 0);
    const bool lat_enabled = (lat_cycles > 0);
    if (bw_enabled || lat_enabled) {
        const double peak_bw_per_cycle = bw_enabled
            ? (64.0 / static_cast<double>(bw_cycles))
            : std::numeric_limits<double>::infinity();
        auto latency_fn = [lat_cycles, lat_enabled](double) {
            return lat_enabled ? lat_cycles : int64_t{0};
        };
        auto bw_cost_fn = [](csEvent* const& item) {
            return static_cast<double>(event_bytes(item));
        };
        const int64_t ingress_max_pending = 0; // credits gate ingress; never drop on ingress full
        ingress_ = std::make_unique<::lat_bw_queue<csEvent*>>(peak_bw_per_cycle,
                                                           latency_fn,
                                                           bw_cost_fn,
                                                           ingress_max_pending);
    } else {
        // No ingress timing model when both are disabled.
        ingress_.reset();
    }
    egress_credit_cap_ = credit_capacity_bytes(queue_size_bytes);
    egress_credits_ = egress_credit_cap_;
    egress_queue_max_bytes_ = queue_size_bytes > 0 ? queue_size_bytes : 0;
    egress_queue_bytes_ = 0;
    ingress_enqueue_cycle_.clear();
    ingress_wait_sum_cycles_ = 0;
    ingress_wait_samples_ = 0;
    ingress_wait_max_cycles_ = 0;
    ingress_queue_wait_sum_cycles_ = 0;
    ingress_queue_wait_samples_ = 0;
    ingress_queue_wait_max_cycles_ = 0;
    egress_wait_sum_cycles_ = 0;
    egress_wait_samples_ = 0;
    egress_wait_max_cycles_ = 0;
    egress_occ_sum_bytes_ = 0;
    egress_occ_sq_sum_bytes_ = 0.0L;
    egress_occ_nonempty_cycles_ = 0;
    egress_occ_max_bytes_ = 0;
    egress_blocked_cycles_ = 0;
    egress_blocked_occ_sum_bytes_ = 0;
    egress_blocked_occ_max_bytes_ = 0;
    egress_send_burst_max_pkts_ = 0;
    egress_send_burst_max_bytes_ = 0;
    egress_send_nonempty_cycles_ = 0;
    egress_send_burst_sum_pkts_ = 0;
    egress_send_burst_sum_bytes_ = 0;
    egress_send_burst_sq_sum_pkts_ = 0.0L;
    egress_send_burst_sq_sum_bytes_ = 0.0L;
    ingress_arrival_burst_cycle_ = std::numeric_limits<uint64_t>::max();
    ingress_arrival_burst_pkts_cur_ = 0;
    ingress_arrival_burst_bytes_cur_ = 0;
    ingress_arrival_burst_max_pkts_ = 0;
    ingress_arrival_burst_max_bytes_ = 0;
    ingress_arrival_nonempty_cycles_ = 0;
    ingress_arrival_burst_sum_pkts_ = 0;
    ingress_arrival_burst_sum_bytes_ = 0;
    ingress_arrival_burst_sq_sum_pkts_ = 0.0L;
    ingress_arrival_burst_sq_sum_bytes_ = 0.0L;
    ingress_arrival_prev_cycle_ = std::numeric_limits<uint64_t>::max();
    ingress_arrival_run_cur_cycles_ = 0;
    ingress_arrival_run_max_cycles_ = 0;
    ingress_arrival_run_count_ = 0;
    ingress_arrival_run_sum_cycles_ = 0;
    ingress_arrival_run_sq_sum_cycles_ = 0.0L;
    ingress_arrival_gap_max_cycles_ = 0;
    ingress_arrival_gap_count_ = 0;
    ingress_arrival_gap_sum_cycles_ = 0;
    ingress_arrival_gap_sq_sum_cycles_ = 0.0L;
    ingress_release_burst_max_pkts_ = 0;
    ingress_release_burst_max_bytes_ = 0;
    ingress_release_nonempty_cycles_ = 0;
    ingress_release_burst_sum_pkts_ = 0;
    ingress_release_burst_sum_bytes_ = 0;
    ingress_release_burst_sq_sum_pkts_ = 0.0L;
    ingress_release_burst_sq_sum_bytes_ = 0.0L;
    ingress_release_seen_nonempty_ = false;
    ingress_release_run_cur_cycles_ = 0;
    ingress_release_run_max_cycles_ = 0;
    ingress_release_run_count_ = 0;
    ingress_release_run_sum_cycles_ = 0;
    ingress_release_run_sq_sum_cycles_ = 0.0L;
    ingress_release_gap_cur_cycles_ = 0;
    ingress_release_gap_max_cycles_ = 0;
    ingress_release_gap_count_ = 0;
    ingress_release_gap_sum_cycles_ = 0;
    ingress_release_gap_sq_sum_cycles_ = 0.0L;
    ingress_occ_sum_bytes_ = 0;
    ingress_occ_sq_sum_bytes_ = 0.0L;
    ingress_occ_samples_ = 0;
    ingress_occ_nonempty_cycles_ = 0;
    ingress_occ_max_bytes_ = 0;
    ingress_occ_seen_nonempty_ = false;
    ingress_occ_run_cur_cycles_ = 0;
    ingress_occ_run_max_cycles_ = 0;
    ingress_occ_run_count_ = 0;
    ingress_occ_run_sum_cycles_ = 0;
    ingress_occ_run_sq_sum_cycles_ = 0.0L;
    ingress_occ_gap_cur_cycles_ = 0;
    ingress_occ_gap_max_cycles_ = 0;
    ingress_occ_gap_count_ = 0;
    ingress_occ_gap_sum_cycles_ = 0;
    ingress_occ_gap_sq_sum_cycles_ = 0.0L;
    tick_samples_ = 0;
    tx_bytes_total_ = 0;
    rx_bytes_total_ = 0;
    tx_bytes_by_class_.fill(0);
    rx_bytes_by_class_.fill(0);
    tx_packets_by_class_.fill(0);
    rx_packets_by_class_.fill(0);
    ingress_arrival_empty_packets_by_class_.fill(0);
    ingress_arrival_nonempty_packets_by_class_.fill(0);
    ingress_arrival_empty_bytes_by_class_.fill(0);
    ingress_arrival_nonempty_bytes_by_class_.fill(0);
    ingress_nonempty_pre_occ_sum_bytes_by_class_.fill(0);
    ingress_nonempty_pre_occ_max_bytes_by_class_.fill(0);
    ingress_release_after_empty_packets_by_class_.fill(0);
    ingress_release_after_nonempty_packets_by_class_.fill(0);
    ingress_wait_after_empty_sum_cycles_by_class_.fill(0);
    ingress_wait_after_nonempty_sum_cycles_by_class_.fill(0);
    ingress_queue_wait_after_empty_sum_cycles_by_class_.fill(0);
    ingress_queue_wait_after_nonempty_sum_cycles_by_class_.fill(0);
    ingress_queue_wait_after_empty_max_cycles_by_class_.fill(0);
    ingress_queue_wait_after_nonempty_max_cycles_by_class_.fill(0);
    ingress_wait_sum_cycles_by_class_.fill(0);
    ingress_wait_samples_by_class_.fill(0);
    ingress_wait_max_cycles_by_class_.fill(0);
    ingress_queue_wait_sum_cycles_by_class_.fill(0);
    ingress_queue_wait_samples_by_class_.fill(0);
    ingress_queue_wait_max_cycles_by_class_.fill(0);
    egress_wait_sum_cycles_by_class_.fill(0);
    egress_wait_samples_by_class_.fill(0);
    egress_wait_max_cycles_by_class_.fill(0);
    egress_queue_bytes_by_class_.fill(0);
    egress_occ_sum_bytes_by_class_.fill(0);
    egress_occ_max_bytes_by_class_.fill(0);
    egress_blocked_cycles_by_class_.fill(0);
    ingress_episode_diag_ = EpisodeDiag{};
    ingress_episode_state_ = EpisodeState{};
    ingress_episode_active_ = false;
}

bool FabricPort::send(csEvent* item) {
    if (is_reset_control_event(item)) {
        // Reset control events must never be backpressured.
        link_->send(item);
        return true;
    }
    const uint64_t bytes = event_bytes(item);
    if (!can_send(bytes)) {
        return false;
    }
    const uint64_t enqueue_cycle =
        (last_tick_cycle_ == std::numeric_limits<uint64_t>::max()) ? 0 : last_tick_cycle_;
    egress_queue_.push_back(EgressEntry{item, enqueue_cycle});
    egress_queue_bytes_ += static_cast<int64_t>(bytes);
    egress_queue_bytes_by_class_[traffic_class_index(classify_event(item))] += bytes;
    return true;
}

void FabricPort::tick(uint64_t cycle) {
    if (last_tick_cycle_ != cycle) {
        last_tick_cycle_ = cycle;
        tick_ingress();
        drain_egress();
        const auto egress_occ = static_cast<uint64_t>(std::max<int64_t>(egress_queue_bytes_, 0));
        egress_occ_sum_bytes_ += egress_occ;
        egress_occ_sq_sum_bytes_ += static_cast<long double>(egress_occ) * static_cast<long double>(egress_occ);
        egress_occ_max_bytes_ = std::max(egress_occ_max_bytes_, egress_occ);
        if (egress_occ > 0) {
            egress_occ_nonempty_cycles_++;
        }
        ready_occ_bucket_counts_[byte_bucket_index(static_cast<uint64_t>(ready_.size()) * kDefaultMsgBytes)]++;
        for (std::size_t idx = 0; idx < static_cast<std::size_t>(TrafficClass::Count); ++idx) {
            egress_occ_sum_bytes_by_class_[idx] += egress_queue_bytes_by_class_[idx];
            egress_occ_max_bytes_by_class_[idx] =
                std::max(egress_occ_max_bytes_by_class_[idx], egress_queue_bytes_by_class_[idx]);
        }
        const auto occ = ingress_ ? static_cast<uint64_t>(ingress_->occupancy()) : 0;
        ingress_occ_sum_bytes_ += occ;
        ingress_occ_sq_sum_bytes_ += static_cast<long double>(occ) * static_cast<long double>(occ);
        ingress_occ_samples_++;
        ingress_occ_bucket_counts_[byte_bucket_index(occ)]++;
        ingress_occ_max_bytes_ = std::max(ingress_occ_max_bytes_, occ);
        if (occ > 0) {
            ingress_occ_nonempty_cycles_++;
            if (ingress_occ_gap_cur_cycles_ > 0) {
                ingress_occ_gap_count_++;
                ingress_occ_gap_sum_cycles_ += ingress_occ_gap_cur_cycles_;
                ingress_occ_gap_sq_sum_cycles_ += static_cast<long double>(ingress_occ_gap_cur_cycles_) *
                                                  static_cast<long double>(ingress_occ_gap_cur_cycles_);
                ingress_occ_gap_cur_cycles_ = 0;
            }
            ingress_occ_seen_nonempty_ = true;
            ingress_occ_run_cur_cycles_++;
            ingress_occ_run_max_cycles_ = std::max(ingress_occ_run_max_cycles_, ingress_occ_run_cur_cycles_);
        } else if (ingress_occ_seen_nonempty_) {
            if (ingress_occ_run_cur_cycles_ > 0) {
                ingress_occ_run_count_++;
                ingress_occ_run_sum_cycles_ += ingress_occ_run_cur_cycles_;
                ingress_occ_run_sq_sum_cycles_ += static_cast<long double>(ingress_occ_run_cur_cycles_) *
                                                  static_cast<long double>(ingress_occ_run_cur_cycles_);
                ingress_occ_run_cur_cycles_ = 0;
            }
            ingress_occ_gap_cur_cycles_++;
            ingress_occ_gap_max_cycles_ = std::max(ingress_occ_gap_max_cycles_, ingress_occ_gap_cur_cycles_);
        }
        if (ingress_episode_active_) {
            if (occ > 0) {
                ingress_episode_state_.cycles++;
                ingress_episode_state_.peak_occ_bytes =
                    std::max(ingress_episode_state_.peak_occ_bytes, occ);
            } else {
                ingress_episode_diag_.count++;
                ingress_episode_diag_.sum_cycles += ingress_episode_state_.cycles;
                ingress_episode_diag_.max_cycles =
                    std::max(ingress_episode_diag_.max_cycles, ingress_episode_state_.cycles);
                ingress_episode_diag_.sum_peak_occ_bytes += ingress_episode_state_.peak_occ_bytes;
                ingress_episode_diag_.max_peak_occ_bytes =
                    std::max(ingress_episode_diag_.max_peak_occ_bytes, ingress_episode_state_.peak_occ_bytes);
                ingress_episode_diag_.sum_arrival_pkts += ingress_episode_state_.arrival_pkts;
                ingress_episode_diag_.max_arrival_pkts =
                    std::max(ingress_episode_diag_.max_arrival_pkts, ingress_episode_state_.arrival_pkts);
                ingress_episode_diag_.sum_arrival_bytes += ingress_episode_state_.arrival_bytes;
                ingress_episode_diag_.max_arrival_bytes =
                    std::max(ingress_episode_diag_.max_arrival_bytes, ingress_episode_state_.arrival_bytes);
                ingress_episode_diag_.sum_release_pkts += ingress_episode_state_.release_pkts;
                ingress_episode_diag_.max_release_pkts =
                    std::max(ingress_episode_diag_.max_release_pkts, ingress_episode_state_.release_pkts);
                ingress_episode_diag_.sum_release_bytes += ingress_episode_state_.release_bytes;
                ingress_episode_diag_.max_release_bytes =
                    std::max(ingress_episode_diag_.max_release_bytes, ingress_episode_state_.release_bytes);
                ingress_episode_diag_.sum_wait_cycles += ingress_episode_state_.total_wait_cycles;
                ingress_episode_diag_.max_wait_cycles =
                    std::max(ingress_episode_diag_.max_wait_cycles, ingress_episode_state_.max_wait_cycles);
                ingress_episode_diag_.sum_queue_wait_cycles += ingress_episode_state_.total_queue_wait_cycles;
                ingress_episode_diag_.max_queue_wait_cycles =
                    std::max(ingress_episode_diag_.max_queue_wait_cycles, ingress_episode_state_.max_queue_wait_cycles);
                ingress_episode_diag_.sum_src_distinct += static_cast<uint64_t>(ingress_episode_state_.srcs.size());
                ingress_episode_diag_.max_src_distinct =
                    std::max<uint64_t>(ingress_episode_diag_.max_src_distinct, ingress_episode_state_.srcs.size());
                ingress_episode_diag_.sum_dst_distinct += static_cast<uint64_t>(ingress_episode_state_.dsts.size());
                ingress_episode_diag_.max_dst_distinct =
                    std::max<uint64_t>(ingress_episode_diag_.max_dst_distinct, ingress_episode_state_.dsts.size());
                for (std::size_t idx = 0; idx < static_cast<std::size_t>(TrafficClass::Count); ++idx) {
                    ingress_episode_diag_.sum_arrival_pkts_by_class[idx] += ingress_episode_state_.arrival_pkts_by_class[idx];
                    ingress_episode_diag_.max_arrival_pkts_by_class[idx] =
                        std::max(ingress_episode_diag_.max_arrival_pkts_by_class[idx],
                                 ingress_episode_state_.arrival_pkts_by_class[idx]);
                    ingress_episode_diag_.sum_release_pkts_by_class[idx] += ingress_episode_state_.release_pkts_by_class[idx];
                    ingress_episode_diag_.max_release_pkts_by_class[idx] =
                        std::max(ingress_episode_diag_.max_release_pkts_by_class[idx],
                                 ingress_episode_state_.release_pkts_by_class[idx]);
                }
                ingress_episode_state_ = EpisodeState{};
                ingress_episode_active_ = false;
            }
        }
        tick_samples_++;
        ready_occ_sum_ += static_cast<uint64_t>(ready_.size());
        ready_occ_samples_++;
        ready_occ_max_ = std::max(ready_occ_max_, ready_.size());
    }
}

std::optional<csEvent*> FabricPort::receive(uint64_t cycle) {
    if (!can_receive(cycle)) {
        return std::nullopt;
    }
    csEvent* item = std::move(ready_.front());
    const uint64_t credit_dst = event_credit_dst(item);
    const uint64_t credit_len = credit_bytes(item);
    ready_.pop_front();
    record_ready_pop(cycle, item);
    last_deliver_cycle_ = cycle;
    send_credit(credit_dst, credit_len);
    return item;
}

bool FabricPort::try_receive(uint64_t cycle,
                             const std::function<bool(csEvent*)>& handle) {
    if (!can_receive(cycle)) {
        return false;
    }
    csEvent* item = ready_.front();
    const uint64_t credit_dst = event_credit_dst(item);
    const uint64_t credit_len = credit_bytes(item);
    if (!handle(item)) {
        ready_retry_count_++;
        return false;
    }
    ready_.pop_front();
    record_ready_pop(cycle, item);
    last_deliver_cycle_ = cycle;
    send_credit(credit_dst, credit_len);
    return true;
}

void FabricPort::handle_event(SST::Event* ev) {
    auto* cevent = dynamic_cast<csEvent*>(ev);
    if (!cevent) {
        delete ev;
        return;
    }

    if (cevent->payload.size() == 3 || cevent->payload.size() == 4) {
        const uint64_t ctrl_code = cevent->payload[2];
        if (ctrl_code == kControlCredit) {
            const uint64_t ctrl_value = (cevent->payload.size() > 3) ? cevent->payload[3] : 0;
            add_credit(egress_credits_, egress_credit_cap_, ctrl_value);
            delete cevent;
            return;
        }
        if (ctrl_code == kControlResetUtil) {
            reset_stats((last_tick_cycle_ == std::numeric_limits<uint64_t>::max()) ? 0 : last_tick_cycle_);
            // Reset controls are out-of-band: do not let data backlog delay phase reset propagation.
            push_ready(cevent, true);
            return;
        }
    }

    record_rx(cevent);
    const auto cls = classify_event(cevent);
    const uint64_t bytes = event_bytes(cevent);
    const uint64_t src = !cevent->payload.empty() ? cevent->payload[0] : 0;
    const uint64_t dst = cevent->payload.size() > 1 ? cevent->payload[1] : 0;
    const uint64_t pre_enqueue_occ_bytes = ingress_ ? static_cast<uint64_t>(ingress_->occupancy()) : 0;
    record_ingress_arrival((last_tick_cycle_ == std::numeric_limits<uint64_t>::max()) ? 0 : last_tick_cycle_,
                           bytes);
    record_conditional_ingress_arrival(cls, bytes, pre_enqueue_occ_bytes, src, dst);
    if (!ingress_) {
        record_ingress_wait(cls, 0, 0);
        record_conditional_ingress_release(cls, src, dst, pre_enqueue_occ_bytes > 0, bytes, 0, 0);
        push_ready(cevent);
        return;
    }

    if (!ingress_->add_packet(cevent)) {
        throw std::runtime_error("FabricPort: ingress queue full; credit accounting mismatch.");
    }
    const uint64_t enqueue_cycle =
        (last_tick_cycle_ == std::numeric_limits<uint64_t>::max()) ? 0 : last_tick_cycle_;
    ingress_enqueue_cycle_[cevent] = IngressArrivalMeta{
        enqueue_cycle,
        pre_enqueue_occ_bytes,
        src,
        dst,
        cls,
        pre_enqueue_occ_bytes > 0
    };
}

void FabricPort::reset_stats(uint64_t cycle) {
    if (ingress_) {
        ingress_->reset_utilization();
    }

    for (auto& entry : egress_queue_) {
        entry.enqueue_cycle = cycle;
    }
    for (auto& [item, meta] : ingress_enqueue_cycle_) {
        (void)item;
        meta.enqueue_cycle = cycle;
        meta.pre_enqueue_occ_bytes = 0;
        meta.saw_nonempty_queue = false;
    }
    for (auto& [item, enqueue_cycle] : ready_enqueue_cycle_) {
        (void)item;
        enqueue_cycle = cycle;
    }

    ingress_wait_sum_cycles_ = 0;
    ingress_wait_samples_ = 0;
    ingress_wait_max_cycles_ = 0;
    ingress_queue_wait_sum_cycles_ = 0;
    ingress_queue_wait_samples_ = 0;
    ingress_queue_wait_max_cycles_ = 0;
    egress_wait_sum_cycles_ = 0;
    egress_wait_samples_ = 0;
    egress_wait_max_cycles_ = 0;
    egress_occ_sum_bytes_ = 0;
    egress_occ_sq_sum_bytes_ = 0.0L;
    egress_occ_nonempty_cycles_ = 0;
    egress_occ_max_bytes_ = 0;
    egress_blocked_cycles_ = 0;
    egress_blocked_occ_sum_bytes_ = 0;
    egress_blocked_occ_max_bytes_ = 0;
    egress_send_burst_max_pkts_ = 0;
    egress_send_burst_max_bytes_ = 0;
    egress_send_nonempty_cycles_ = 0;
    egress_send_burst_sum_pkts_ = 0;
    egress_send_burst_sum_bytes_ = 0;
    egress_send_burst_sq_sum_pkts_ = 0.0L;
    egress_send_burst_sq_sum_bytes_ = 0.0L;
    ingress_arrival_burst_cycle_ = std::numeric_limits<uint64_t>::max();
    ingress_arrival_burst_pkts_cur_ = 0;
    ingress_arrival_burst_bytes_cur_ = 0;
    ingress_arrival_burst_max_pkts_ = 0;
    ingress_arrival_burst_max_bytes_ = 0;
    ingress_arrival_nonempty_cycles_ = 0;
    ingress_arrival_burst_sum_pkts_ = 0;
    ingress_arrival_burst_sum_bytes_ = 0;
    ingress_arrival_burst_sq_sum_pkts_ = 0.0L;
    ingress_arrival_burst_sq_sum_bytes_ = 0.0L;
    ingress_arrival_prev_cycle_ = std::numeric_limits<uint64_t>::max();
    ingress_arrival_run_cur_cycles_ = 0;
    ingress_arrival_run_max_cycles_ = 0;
    ingress_arrival_run_count_ = 0;
    ingress_arrival_run_sum_cycles_ = 0;
    ingress_arrival_run_sq_sum_cycles_ = 0.0L;
    ingress_arrival_gap_max_cycles_ = 0;
    ingress_arrival_gap_count_ = 0;
    ingress_arrival_gap_sum_cycles_ = 0;
    ingress_arrival_gap_sq_sum_cycles_ = 0.0L;
    ingress_release_burst_max_pkts_ = 0;
    ingress_release_burst_max_bytes_ = 0;
    ingress_release_nonempty_cycles_ = 0;
    ingress_release_burst_sum_pkts_ = 0;
    ingress_release_burst_sum_bytes_ = 0;
    ingress_release_burst_sq_sum_pkts_ = 0.0L;
    ingress_release_burst_sq_sum_bytes_ = 0.0L;
    ingress_release_seen_nonempty_ = false;
    ingress_release_run_cur_cycles_ = 0;
    ingress_release_run_max_cycles_ = 0;
    ingress_release_run_count_ = 0;
    ingress_release_run_sum_cycles_ = 0;
    ingress_release_run_sq_sum_cycles_ = 0.0L;
    ingress_release_gap_cur_cycles_ = 0;
    ingress_release_gap_max_cycles_ = 0;
    ingress_release_gap_count_ = 0;
    ingress_release_gap_sum_cycles_ = 0;
    ingress_release_gap_sq_sum_cycles_ = 0.0L;
    ingress_occ_sum_bytes_ = 0;
    ingress_occ_sq_sum_bytes_ = 0.0L;
    ingress_occ_samples_ = 0;
    ingress_occ_nonempty_cycles_ = 0;
    ingress_occ_max_bytes_ = 0;
    ingress_occ_seen_nonempty_ = false;
    ingress_occ_run_cur_cycles_ = 0;
    ingress_occ_run_max_cycles_ = 0;
    ingress_occ_run_count_ = 0;
    ingress_occ_run_sum_cycles_ = 0;
    ingress_occ_run_sq_sum_cycles_ = 0.0L;
    ingress_occ_gap_cur_cycles_ = 0;
    ingress_occ_gap_max_cycles_ = 0;
    ingress_occ_gap_count_ = 0;
    ingress_occ_gap_sum_cycles_ = 0;
    ingress_occ_gap_sq_sum_cycles_ = 0.0L;
    ready_wait_sum_cycles_ = 0;
    ready_wait_samples_ = 0;
    ready_wait_max_cycles_ = 0;
    ready_retry_count_ = 0;
    ready_occ_sum_ = 0;
    ready_occ_samples_ = 0;
    ready_occ_max_ = 0;
    ready_occ_bucket_counts_.fill(0);
    ingress_occ_bucket_counts_.fill(0);
    tick_samples_ = 0;
    tx_bytes_total_ = 0;
    rx_bytes_total_ = 0;
    tx_bytes_by_class_.fill(0);
    rx_bytes_by_class_.fill(0);
    tx_packets_by_class_.fill(0);
    rx_packets_by_class_.fill(0);
    ingress_arrival_empty_packets_by_class_.fill(0);
    ingress_arrival_nonempty_packets_by_class_.fill(0);
    ingress_arrival_empty_bytes_by_class_.fill(0);
    ingress_arrival_nonempty_bytes_by_class_.fill(0);
    ingress_nonempty_pre_occ_sum_bytes_by_class_.fill(0);
    ingress_nonempty_pre_occ_max_bytes_by_class_.fill(0);
    ingress_release_after_empty_packets_by_class_.fill(0);
    ingress_release_after_nonempty_packets_by_class_.fill(0);
    ingress_wait_after_empty_sum_cycles_by_class_.fill(0);
    ingress_wait_after_nonempty_sum_cycles_by_class_.fill(0);
    ingress_queue_wait_after_empty_sum_cycles_by_class_.fill(0);
    ingress_queue_wait_after_nonempty_sum_cycles_by_class_.fill(0);
    ingress_queue_wait_after_empty_max_cycles_by_class_.fill(0);
    ingress_queue_wait_after_nonempty_max_cycles_by_class_.fill(0);
    ingress_wait_sum_cycles_by_class_.fill(0);
    ingress_wait_samples_by_class_.fill(0);
    ingress_wait_max_cycles_by_class_.fill(0);
    ingress_queue_wait_sum_cycles_by_class_.fill(0);
    ingress_queue_wait_samples_by_class_.fill(0);
    ingress_queue_wait_max_cycles_by_class_.fill(0);
    egress_wait_sum_cycles_by_class_.fill(0);
    egress_wait_samples_by_class_.fill(0);
    egress_wait_max_cycles_by_class_.fill(0);
    egress_occ_sum_bytes_by_class_.fill(0);
    egress_occ_max_bytes_by_class_.fill(0);
    egress_blocked_cycles_by_class_.fill(0);
    ingress_pre_occ_bucket_counts_by_class_ = {};
    ingress_queue_wait_bucket_counts_by_class_ = {};
    rx_by_src_peer_.clear();
    rx_by_dst_peer_.clear();
    tx_by_src_peer_.clear();
    tx_by_dst_peer_.clear();
    ingress_episode_diag_ = EpisodeDiag{};
    ingress_episode_state_ = EpisodeState{};
    ingress_episode_active_ = false;
}

bool FabricPort::can_send() const {
    return can_send(kDefaultMsgBytes);
}

bool FabricPort::can_send(uint64_t bytes) const {
    const uint64_t bounded_bytes = std::max<uint64_t>(bytes, 1);
    if (egress_queue_max_bytes_ <= 0) {
        return true;
    }
    return !egress_queue_full(bounded_bytes);
}

bool FabricPort::can_send(const csEvent* item) const {
    if (!item) {
        return can_send();
    }
    return can_send(event_bytes(item));
}

double FabricPort::ingress_avg_utilization() const {
    if (!ingress_) {
        return 0.0;
    }
    return ingress_->average_utilization();
}

double FabricPort::ingress_utilization() const {
    if (!ingress_) {
        return 0.0;
    }
    return ingress_->utilization();
}

std::size_t FabricPort::ingress_occupancy() const {
    if (!ingress_) {
        return 0;
    }
    return ingress_->occupancy();
}

std::size_t FabricPort::ready_occupancy() const {
    return ready_.size();
}

double FabricPort::ready_occupancy_avg() const {
    if (ready_occ_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ready_occ_sum_) / static_cast<double>(ready_occ_samples_);
}

std::size_t FabricPort::ready_occupancy_max() const {
    return ready_occ_max_;
}

double FabricPort::ingress_wait_avg_cycles() const {
    if (ingress_wait_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_wait_sum_cycles_) / static_cast<double>(ingress_wait_samples_);
}

uint64_t FabricPort::ingress_wait_max_cycles() const {
    return ingress_wait_max_cycles_;
}

double FabricPort::ingress_queue_wait_avg_cycles() const {
    if (ingress_queue_wait_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_queue_wait_sum_cycles_) / static_cast<double>(ingress_queue_wait_samples_);
}

uint64_t FabricPort::ingress_queue_wait_max_cycles() const {
    return ingress_queue_wait_max_cycles_;
}

double FabricPort::ready_wait_avg_cycles() const {
    if (ready_wait_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ready_wait_sum_cycles_) / static_cast<double>(ready_wait_samples_);
}

uint64_t FabricPort::ready_wait_max_cycles() const {
    return ready_wait_max_cycles_;
}

uint64_t FabricPort::ready_retry_count() const {
    return ready_retry_count_;
}

double FabricPort::ingress_wait_avg_cycles(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    if (ingress_wait_samples_by_class_[idx] == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_wait_sum_cycles_by_class_[idx]) /
           static_cast<double>(ingress_wait_samples_by_class_[idx]);
}

uint64_t FabricPort::ingress_wait_max_cycles(TrafficClass cls) const {
    return ingress_wait_max_cycles_by_class_[traffic_class_index(cls)];
}

double FabricPort::ingress_queue_wait_avg_cycles(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    if (ingress_queue_wait_samples_by_class_[idx] == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_queue_wait_sum_cycles_by_class_[idx]) /
           static_cast<double>(ingress_queue_wait_samples_by_class_[idx]);
}

uint64_t FabricPort::ingress_queue_wait_max_cycles(TrafficClass cls) const {
    return ingress_queue_wait_max_cycles_by_class_[traffic_class_index(cls)];
}

double FabricPort::egress_wait_avg_cycles() const {
    if (egress_wait_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_wait_sum_cycles_) / static_cast<double>(egress_wait_samples_);
}

uint64_t FabricPort::egress_wait_max_cycles() const {
    return egress_wait_max_cycles_;
}

double FabricPort::egress_occ_avg_bytes() const {
    if (tick_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_occ_sum_bytes_) / static_cast<double>(tick_samples_);
}

double FabricPort::egress_occ_stddev_bytes() const {
    return safe_stddev(egress_occ_sq_sum_bytes_,
                       static_cast<long double>(egress_occ_sum_bytes_),
                       tick_samples_);
}

uint64_t FabricPort::egress_occ_max_bytes() const {
    return egress_occ_max_bytes_;
}

double FabricPort::egress_occ_nonempty_frac() const {
    if (tick_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_occ_nonempty_cycles_) / static_cast<double>(tick_samples_);
}

uint64_t FabricPort::egress_blocked_cycles() const {
    return egress_blocked_cycles_;
}

double FabricPort::egress_blocked_nonempty_frac() const {
    if (tick_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_blocked_cycles_) / static_cast<double>(tick_samples_);
}

double FabricPort::egress_blocked_avg_occ_bytes() const {
    if (egress_blocked_cycles_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_blocked_occ_sum_bytes_) / static_cast<double>(egress_blocked_cycles_);
}

uint64_t FabricPort::egress_blocked_max_occ_bytes() const {
    return egress_blocked_occ_max_bytes_;
}

uint64_t FabricPort::egress_send_burst_max_pkts() const {
    return egress_send_burst_max_pkts_;
}

uint64_t FabricPort::egress_send_burst_max_bytes() const {
    return egress_send_burst_max_bytes_;
}

double FabricPort::egress_send_burst_avg_pkts() const {
    if (egress_send_nonempty_cycles_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_send_burst_sum_pkts_) / static_cast<double>(egress_send_nonempty_cycles_);
}

double FabricPort::egress_send_burst_avg_bytes() const {
    if (egress_send_nonempty_cycles_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_send_burst_sum_bytes_) / static_cast<double>(egress_send_nonempty_cycles_);
}

double FabricPort::egress_send_burst_stddev_pkts() const {
    return safe_stddev(egress_send_burst_sq_sum_pkts_,
                       static_cast<long double>(egress_send_burst_sum_pkts_),
                       egress_send_nonempty_cycles_);
}

double FabricPort::egress_send_burst_stddev_bytes() const {
    return safe_stddev(egress_send_burst_sq_sum_bytes_,
                       static_cast<long double>(egress_send_burst_sum_bytes_),
                       egress_send_nonempty_cycles_);
}

double FabricPort::egress_send_nonempty_frac() const {
    if (tick_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_send_nonempty_cycles_) / static_cast<double>(tick_samples_);
}

uint64_t FabricPort::ingress_arrival_burst_max_pkts() const {
    return ingress_arrival_burst_max_pkts_;
}

uint64_t FabricPort::ingress_arrival_burst_max_bytes() const {
    return ingress_arrival_burst_max_bytes_;
}

double FabricPort::ingress_arrival_burst_avg_pkts() const {
    const uint64_t count = ingress_arrival_nonempty_cycles_ + (ingress_arrival_burst_pkts_cur_ > 0 ? 1 : 0);
    if (count == 0) {
        return 0.0;
    }
    const uint64_t sum = ingress_arrival_burst_sum_pkts_ + ingress_arrival_burst_pkts_cur_;
    return static_cast<double>(sum) / static_cast<double>(count);
}

double FabricPort::ingress_arrival_burst_avg_bytes() const {
    const uint64_t count = ingress_arrival_nonempty_cycles_ + (ingress_arrival_burst_pkts_cur_ > 0 ? 1 : 0);
    if (count == 0) {
        return 0.0;
    }
    const uint64_t sum = ingress_arrival_burst_sum_bytes_ + ingress_arrival_burst_bytes_cur_;
    return static_cast<double>(sum) / static_cast<double>(count);
}

double FabricPort::ingress_arrival_burst_stddev_pkts() const {
    const uint64_t count = ingress_arrival_nonempty_cycles_ + (ingress_arrival_burst_pkts_cur_ > 0 ? 1 : 0);
    const long double sum = static_cast<long double>(ingress_arrival_burst_sum_pkts_ + ingress_arrival_burst_pkts_cur_);
    const long double sq_sum =
        ingress_arrival_burst_sq_sum_pkts_ +
        static_cast<long double>(ingress_arrival_burst_pkts_cur_) * static_cast<long double>(ingress_arrival_burst_pkts_cur_);
    return safe_stddev(sq_sum, sum, count);
}

double FabricPort::ingress_arrival_burst_stddev_bytes() const {
    const uint64_t count = ingress_arrival_nonempty_cycles_ + (ingress_arrival_burst_pkts_cur_ > 0 ? 1 : 0);
    const long double sum = static_cast<long double>(ingress_arrival_burst_sum_bytes_ + ingress_arrival_burst_bytes_cur_);
    const long double sq_sum =
        ingress_arrival_burst_sq_sum_bytes_ +
        static_cast<long double>(ingress_arrival_burst_bytes_cur_) * static_cast<long double>(ingress_arrival_burst_bytes_cur_);
    return safe_stddev(sq_sum, sum, count);
}

double FabricPort::ingress_arrival_nonempty_frac() const {
    if (tick_samples_ == 0) {
        return 0.0;
    }
    const uint64_t count = ingress_arrival_nonempty_cycles_ + (ingress_arrival_burst_pkts_cur_ > 0 ? 1 : 0);
    return static_cast<double>(count) / static_cast<double>(tick_samples_);
}

uint64_t FabricPort::ingress_arrival_run_max_cycles() const {
    return std::max(ingress_arrival_run_max_cycles_, ingress_arrival_run_cur_cycles_);
}

double FabricPort::ingress_arrival_run_avg_cycles() const {
    const uint64_t count = ingress_arrival_run_count_ + (ingress_arrival_run_cur_cycles_ > 0 ? 1 : 0);
    if (count == 0) {
        return 0.0;
    }
    const uint64_t sum = ingress_arrival_run_sum_cycles_ + ingress_arrival_run_cur_cycles_;
    return static_cast<double>(sum) / static_cast<double>(count);
}

double FabricPort::ingress_arrival_run_stddev_cycles() const {
    const uint64_t count = ingress_arrival_run_count_ + (ingress_arrival_run_cur_cycles_ > 0 ? 1 : 0);
    const long double sum = static_cast<long double>(ingress_arrival_run_sum_cycles_ + ingress_arrival_run_cur_cycles_);
    const long double sq_sum =
        ingress_arrival_run_sq_sum_cycles_ +
        static_cast<long double>(ingress_arrival_run_cur_cycles_) * static_cast<long double>(ingress_arrival_run_cur_cycles_);
    return safe_stddev(sq_sum, sum, count);
}

uint64_t FabricPort::ingress_arrival_gap_max_cycles() const {
    return ingress_arrival_gap_max_cycles_;
}

double FabricPort::ingress_arrival_gap_avg_cycles() const {
    if (ingress_arrival_gap_count_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_arrival_gap_sum_cycles_) / static_cast<double>(ingress_arrival_gap_count_);
}

double FabricPort::ingress_arrival_gap_stddev_cycles() const {
    return safe_stddev(ingress_arrival_gap_sq_sum_cycles_,
                       static_cast<long double>(ingress_arrival_gap_sum_cycles_),
                       ingress_arrival_gap_count_);
}

uint64_t FabricPort::ingress_release_burst_max_pkts() const {
    return ingress_release_burst_max_pkts_;
}

uint64_t FabricPort::ingress_release_burst_max_bytes() const {
    return ingress_release_burst_max_bytes_;
}

double FabricPort::ingress_release_burst_avg_pkts() const {
    if (ingress_release_nonempty_cycles_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_release_burst_sum_pkts_) / static_cast<double>(ingress_release_nonempty_cycles_);
}

double FabricPort::ingress_release_burst_avg_bytes() const {
    if (ingress_release_nonempty_cycles_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_release_burst_sum_bytes_) / static_cast<double>(ingress_release_nonempty_cycles_);
}

double FabricPort::ingress_release_burst_stddev_pkts() const {
    return safe_stddev(ingress_release_burst_sq_sum_pkts_,
                       static_cast<long double>(ingress_release_burst_sum_pkts_),
                       ingress_release_nonempty_cycles_);
}

double FabricPort::ingress_release_burst_stddev_bytes() const {
    return safe_stddev(ingress_release_burst_sq_sum_bytes_,
                       static_cast<long double>(ingress_release_burst_sum_bytes_),
                       ingress_release_nonempty_cycles_);
}

double FabricPort::ingress_release_nonempty_frac() const {
    if (tick_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_release_nonempty_cycles_) / static_cast<double>(tick_samples_);
}

uint64_t FabricPort::ingress_release_run_max_cycles() const {
    return std::max(ingress_release_run_max_cycles_, ingress_release_run_cur_cycles_);
}

double FabricPort::ingress_release_run_avg_cycles() const {
    const uint64_t count = ingress_release_run_count_ + (ingress_release_run_cur_cycles_ > 0 ? 1 : 0);
    if (count == 0) {
        return 0.0;
    }
    const uint64_t sum = ingress_release_run_sum_cycles_ + ingress_release_run_cur_cycles_;
    return static_cast<double>(sum) / static_cast<double>(count);
}

double FabricPort::ingress_release_run_stddev_cycles() const {
    const uint64_t count = ingress_release_run_count_ + (ingress_release_run_cur_cycles_ > 0 ? 1 : 0);
    const long double sum = static_cast<long double>(ingress_release_run_sum_cycles_ + ingress_release_run_cur_cycles_);
    const long double sq_sum =
        ingress_release_run_sq_sum_cycles_ +
        static_cast<long double>(ingress_release_run_cur_cycles_) * static_cast<long double>(ingress_release_run_cur_cycles_);
    return safe_stddev(sq_sum, sum, count);
}

uint64_t FabricPort::ingress_release_gap_max_cycles() const {
    return std::max(ingress_release_gap_max_cycles_, ingress_release_gap_cur_cycles_);
}

double FabricPort::ingress_release_gap_avg_cycles() const {
    const uint64_t count = ingress_release_gap_count_ + (ingress_release_gap_cur_cycles_ > 0 ? 1 : 0);
    if (count == 0) {
        return 0.0;
    }
    const uint64_t sum = ingress_release_gap_sum_cycles_ + ingress_release_gap_cur_cycles_;
    return static_cast<double>(sum) / static_cast<double>(count);
}

double FabricPort::ingress_release_gap_stddev_cycles() const {
    const uint64_t count = ingress_release_gap_count_ + (ingress_release_gap_cur_cycles_ > 0 ? 1 : 0);
    const long double sum = static_cast<long double>(ingress_release_gap_sum_cycles_ + ingress_release_gap_cur_cycles_);
    const long double sq_sum =
        ingress_release_gap_sq_sum_cycles_ +
        static_cast<long double>(ingress_release_gap_cur_cycles_) * static_cast<long double>(ingress_release_gap_cur_cycles_);
    return safe_stddev(sq_sum, sum, count);
}

double FabricPort::ingress_occ_avg_bytes() const {
    if (ingress_occ_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_occ_sum_bytes_) / static_cast<double>(ingress_occ_samples_);
}

double FabricPort::ingress_occ_stddev_bytes() const {
    return safe_stddev(ingress_occ_sq_sum_bytes_,
                       static_cast<long double>(ingress_occ_sum_bytes_),
                       ingress_occ_samples_);
}

uint64_t FabricPort::ingress_occ_max_bytes() const {
    return ingress_occ_max_bytes_;
}

double FabricPort::ingress_occ_nonempty_frac() const {
    if (ingress_occ_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_occ_nonempty_cycles_) / static_cast<double>(ingress_occ_samples_);
}

uint64_t FabricPort::ingress_occ_run_max_cycles() const {
    return std::max(ingress_occ_run_max_cycles_, ingress_occ_run_cur_cycles_);
}

double FabricPort::ingress_occ_run_avg_cycles() const {
    const uint64_t count = ingress_occ_run_count_ + (ingress_occ_run_cur_cycles_ > 0 ? 1 : 0);
    if (count == 0) {
        return 0.0;
    }
    const uint64_t sum = ingress_occ_run_sum_cycles_ + ingress_occ_run_cur_cycles_;
    return static_cast<double>(sum) / static_cast<double>(count);
}

double FabricPort::ingress_occ_run_stddev_cycles() const {
    const uint64_t count = ingress_occ_run_count_ + (ingress_occ_run_cur_cycles_ > 0 ? 1 : 0);
    const long double sum = static_cast<long double>(ingress_occ_run_sum_cycles_ + ingress_occ_run_cur_cycles_);
    const long double sq_sum =
        ingress_occ_run_sq_sum_cycles_ +
        static_cast<long double>(ingress_occ_run_cur_cycles_) * static_cast<long double>(ingress_occ_run_cur_cycles_);
    return safe_stddev(sq_sum, sum, count);
}

uint64_t FabricPort::ingress_occ_gap_max_cycles() const {
    return std::max(ingress_occ_gap_max_cycles_, ingress_occ_gap_cur_cycles_);
}

double FabricPort::ingress_occ_gap_avg_cycles() const {
    const uint64_t count = ingress_occ_gap_count_ + (ingress_occ_gap_cur_cycles_ > 0 ? 1 : 0);
    if (count == 0) {
        return 0.0;
    }
    const uint64_t sum = ingress_occ_gap_sum_cycles_ + ingress_occ_gap_cur_cycles_;
    return static_cast<double>(sum) / static_cast<double>(count);
}

double FabricPort::ingress_occ_gap_stddev_cycles() const {
    const uint64_t count = ingress_occ_gap_count_ + (ingress_occ_gap_cur_cycles_ > 0 ? 1 : 0);
    const long double sum = static_cast<long double>(ingress_occ_gap_sum_cycles_ + ingress_occ_gap_cur_cycles_);
    const long double sq_sum =
        ingress_occ_gap_sq_sum_cycles_ +
        static_cast<long double>(ingress_occ_gap_cur_cycles_) * static_cast<long double>(ingress_occ_gap_cur_cycles_);
    return safe_stddev(sq_sum, sum, count);
}

uint64_t FabricPort::tx_bytes_total() const {
    return tx_bytes_total_;
}

uint64_t FabricPort::rx_bytes_total() const {
    return rx_bytes_total_;
}

uint64_t FabricPort::tx_bytes(TrafficClass cls) const {
    return tx_bytes_by_class_[static_cast<std::size_t>(cls)];
}

uint64_t FabricPort::rx_bytes(TrafficClass cls) const {
    return rx_bytes_by_class_[static_cast<std::size_t>(cls)];
}

uint64_t FabricPort::tx_packets(TrafficClass cls) const {
    return tx_packets_by_class_[static_cast<std::size_t>(cls)];
}

uint64_t FabricPort::rx_packets(TrafficClass cls) const {
    return rx_packets_by_class_[static_cast<std::size_t>(cls)];
}

double FabricPort::egress_wait_avg_cycles(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    if (egress_wait_samples_by_class_[idx] == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_wait_sum_cycles_by_class_[idx]) /
           static_cast<double>(egress_wait_samples_by_class_[idx]);
}

uint64_t FabricPort::egress_wait_max_cycles(TrafficClass cls) const {
    return egress_wait_max_cycles_by_class_[traffic_class_index(cls)];
}

double FabricPort::egress_occ_avg_bytes(TrafficClass cls) const {
    if (tick_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_occ_sum_bytes_by_class_[traffic_class_index(cls)]) /
           static_cast<double>(tick_samples_);
}

uint64_t FabricPort::egress_occ_max_bytes(TrafficClass cls) const {
    return egress_occ_max_bytes_by_class_[traffic_class_index(cls)];
}

uint64_t FabricPort::egress_blocked_cycles(TrafficClass cls) const {
    return egress_blocked_cycles_by_class_[traffic_class_index(cls)];
}

uint64_t FabricPort::ingress_arrival_empty_packets(TrafficClass cls) const {
    return ingress_arrival_empty_packets_by_class_[traffic_class_index(cls)];
}

uint64_t FabricPort::ingress_arrival_nonempty_packets(TrafficClass cls) const {
    return ingress_arrival_nonempty_packets_by_class_[traffic_class_index(cls)];
}

double FabricPort::ingress_arrival_nonempty_packet_frac(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    const uint64_t total =
        ingress_arrival_empty_packets_by_class_[idx] + ingress_arrival_nonempty_packets_by_class_[idx];
    if (total == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_arrival_nonempty_packets_by_class_[idx]) /
           static_cast<double>(total);
}

double FabricPort::ingress_nonempty_arrival_pre_occ_avg_bytes(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    if (ingress_arrival_nonempty_packets_by_class_[idx] == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_nonempty_pre_occ_sum_bytes_by_class_[idx]) /
           static_cast<double>(ingress_arrival_nonempty_packets_by_class_[idx]);
}

uint64_t FabricPort::ingress_nonempty_arrival_pre_occ_max_bytes(TrafficClass cls) const {
    return ingress_nonempty_pre_occ_max_bytes_by_class_[traffic_class_index(cls)];
}

uint64_t FabricPort::ingress_release_after_empty_arrival_packets(TrafficClass cls) const {
    return ingress_release_after_empty_packets_by_class_[traffic_class_index(cls)];
}

uint64_t FabricPort::ingress_release_after_nonempty_arrival_packets(TrafficClass cls) const {
    return ingress_release_after_nonempty_packets_by_class_[traffic_class_index(cls)];
}

double FabricPort::ingress_wait_after_empty_arrival_avg_cycles(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    if (ingress_release_after_empty_packets_by_class_[idx] == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_wait_after_empty_sum_cycles_by_class_[idx]) /
           static_cast<double>(ingress_release_after_empty_packets_by_class_[idx]);
}

double FabricPort::ingress_wait_after_nonempty_arrival_avg_cycles(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    if (ingress_release_after_nonempty_packets_by_class_[idx] == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_wait_after_nonempty_sum_cycles_by_class_[idx]) /
           static_cast<double>(ingress_release_after_nonempty_packets_by_class_[idx]);
}

double FabricPort::ingress_queue_wait_after_empty_arrival_avg_cycles(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    if (ingress_release_after_empty_packets_by_class_[idx] == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_queue_wait_after_empty_sum_cycles_by_class_[idx]) /
           static_cast<double>(ingress_release_after_empty_packets_by_class_[idx]);
}

double FabricPort::ingress_queue_wait_after_nonempty_arrival_avg_cycles(TrafficClass cls) const {
    const auto idx = traffic_class_index(cls);
    if (ingress_release_after_nonempty_packets_by_class_[idx] == 0) {
        return 0.0;
    }
    return static_cast<double>(ingress_queue_wait_after_nonempty_sum_cycles_by_class_[idx]) /
           static_cast<double>(ingress_release_after_nonempty_packets_by_class_[idx]);
}

uint64_t FabricPort::ingress_queue_wait_after_empty_arrival_max_cycles(TrafficClass cls) const {
    return ingress_queue_wait_after_empty_max_cycles_by_class_[traffic_class_index(cls)];
}

uint64_t FabricPort::ingress_queue_wait_after_nonempty_arrival_max_cycles(TrafficClass cls) const {
    return ingress_queue_wait_after_nonempty_max_cycles_by_class_[traffic_class_index(cls)];
}

void FabricPort::emit_deep_diagnostics(std::ostream& os, const std::string& prefix) const {
    for (std::size_t i = 0; i < kDiagBucketCount; ++i) {
        os << prefix << "ingress_occ_bucket_bytes." << byte_bucket_name(i) << " = "
           << ingress_occ_bucket_counts_[i] << '\n';
        os << prefix << "ready_occ_bucket_bytes." << byte_bucket_name(i) << " = "
           << ready_occ_bucket_counts_[i] << '\n';
    }

    os << prefix << "ingress_episode.count = " << ingress_episode_diag_.count << '\n';
    os << prefix << "ingress_episode.avg_cycles = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_cycles) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_cycles = " << ingress_episode_diag_.max_cycles << '\n';
    os << prefix << "ingress_episode.avg_peak_occ_bytes = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_peak_occ_bytes) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_peak_occ_bytes = " << ingress_episode_diag_.max_peak_occ_bytes << '\n';
    os << prefix << "ingress_episode.avg_arrival_pkts = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_arrival_pkts) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_arrival_pkts = " << ingress_episode_diag_.max_arrival_pkts << '\n';
    os << prefix << "ingress_episode.avg_arrival_bytes = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_arrival_bytes) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_arrival_bytes = " << ingress_episode_diag_.max_arrival_bytes << '\n';
    os << prefix << "ingress_episode.avg_release_pkts = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_release_pkts) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_release_pkts = " << ingress_episode_diag_.max_release_pkts << '\n';
    os << prefix << "ingress_episode.avg_release_bytes = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_release_bytes) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_release_bytes = " << ingress_episode_diag_.max_release_bytes << '\n';
    os << prefix << "ingress_episode.avg_wait_cycles = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_wait_cycles) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_wait_cycles = " << ingress_episode_diag_.max_wait_cycles << '\n';
    os << prefix << "ingress_episode.avg_queue_wait_cycles = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_queue_wait_cycles) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_queue_wait_cycles = " << ingress_episode_diag_.max_queue_wait_cycles << '\n';
    os << prefix << "ingress_episode.avg_src_distinct = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_src_distinct) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_src_distinct = " << ingress_episode_diag_.max_src_distinct << '\n';
    os << prefix << "ingress_episode.avg_dst_distinct = "
       << (ingress_episode_diag_.count > 0
               ? static_cast<double>(ingress_episode_diag_.sum_dst_distinct) /
                     static_cast<double>(ingress_episode_diag_.count)
               : 0.0)
       << '\n';
    os << prefix << "ingress_episode.max_dst_distinct = " << ingress_episode_diag_.max_dst_distinct << '\n';

    static constexpr std::array<std::pair<TrafficClass, const char*>, 4> kClasses{{
        {TrafficClass::DemandReq, "demand_req"},
        {TrafficClass::WriteReq, "write_req"},
        {TrafficClass::Response, "response"},
        {TrafficClass::OtherReq, "other_req"},
    }};

    for (const auto& [cls, cls_name] : kClasses) {
        const auto idx = traffic_class_index(cls);
        os << prefix << "ingress_episode.avg_arrival_pkts." << cls_name << " = "
           << (ingress_episode_diag_.count > 0
                   ? static_cast<double>(ingress_episode_diag_.sum_arrival_pkts_by_class[idx]) /
                         static_cast<double>(ingress_episode_diag_.count)
                   : 0.0)
           << '\n';
        os << prefix << "ingress_episode.max_arrival_pkts." << cls_name << " = "
           << ingress_episode_diag_.max_arrival_pkts_by_class[idx] << '\n';
        os << prefix << "ingress_episode.avg_release_pkts." << cls_name << " = "
           << (ingress_episode_diag_.count > 0
                   ? static_cast<double>(ingress_episode_diag_.sum_release_pkts_by_class[idx]) /
                         static_cast<double>(ingress_episode_diag_.count)
                   : 0.0)
           << '\n';
        os << prefix << "ingress_episode.max_release_pkts." << cls_name << " = "
           << ingress_episode_diag_.max_release_pkts_by_class[idx] << '\n';
        for (std::size_t b = 0; b < kDiagBucketCount; ++b) {
            os << prefix << "ingress_pre_occ_bucket." << cls_name << '.'
               << byte_bucket_name(b) << " = "
               << ingress_pre_occ_bucket_counts_by_class_[idx][b] << '\n';
            os << prefix << "ingress_queue_wait_bucket." << cls_name << '.'
               << cycle_bucket_name(b) << " = "
               << ingress_queue_wait_bucket_counts_by_class_[idx][b] << '\n';
        }
    }

    auto emit_peer_map = [&](const std::unordered_map<uint64_t, PeerDiag>& peers,
                             const std::string& role) {
        std::vector<uint64_t> ids;
        ids.reserve(peers.size());
        for (const auto& entry : peers) {
            ids.push_back(entry.first);
        }
        std::sort(ids.begin(), ids.end());
        for (uint64_t peer : ids) {
            const auto it = peers.find(peer);
            if (it == peers.end()) {
                continue;
            }
            for (const auto& [cls, cls_name] : kClasses) {
                const auto idx = traffic_class_index(cls);
                const auto& diag = it->second.classes[idx];
                if (diag.rx_packets == 0 && diag.tx_packets == 0 && diag.ingress_arrivals == 0 &&
                    diag.ingress_wait_samples == 0) {
                    continue;
                }
                os << prefix << role << '.' << peer << ".rx_bytes." << cls_name << " = " << diag.rx_bytes << '\n';
                os << prefix << role << '.' << peer << ".rx_pkts." << cls_name << " = " << diag.rx_packets << '\n';
                os << prefix << role << '.' << peer << ".tx_bytes." << cls_name << " = " << diag.tx_bytes << '\n';
                os << prefix << role << '.' << peer << ".tx_pkts." << cls_name << " = " << diag.tx_packets << '\n';
                os << prefix << role << '.' << peer << ".ingress_arrivals." << cls_name << " = " << diag.ingress_arrivals << '\n';
                os << prefix << role << '.' << peer << ".ingress_arrival_nonempty_packet_frac." << cls_name << " = "
                   << (diag.ingress_arrivals > 0
                           ? static_cast<double>(diag.ingress_nonempty_arrivals) / static_cast<double>(diag.ingress_arrivals)
                           : 0.0)
                   << '\n';
                os << prefix << role << '.' << peer << ".ingress_nonempty_arrival_pre_occ_avg_bytes." << cls_name << " = "
                   << (diag.ingress_nonempty_arrivals > 0
                           ? static_cast<double>(diag.ingress_nonempty_pre_occ_sum_bytes) /
                                 static_cast<double>(diag.ingress_nonempty_arrivals)
                           : 0.0)
                   << '\n';
                os << prefix << role << '.' << peer << ".ingress_nonempty_arrival_pre_occ_max_bytes." << cls_name << " = "
                   << diag.ingress_nonempty_pre_occ_max_bytes << '\n';
                os << prefix << role << '.' << peer << ".ingress_wait_avg_cycles." << cls_name << " = "
                   << (diag.ingress_wait_samples > 0
                           ? static_cast<double>(diag.ingress_wait_sum_cycles) / static_cast<double>(diag.ingress_wait_samples)
                           : 0.0)
                   << '\n';
                os << prefix << role << '.' << peer << ".ingress_queue_wait_avg_cycles." << cls_name << " = "
                   << (diag.ingress_queue_wait_samples > 0
                           ? static_cast<double>(diag.ingress_queue_wait_sum_cycles) /
                                 static_cast<double>(diag.ingress_queue_wait_samples)
                           : 0.0)
                   << '\n';
                os << prefix << role << '.' << peer << ".ingress_queue_wait_after_nonempty_arrival_avg_cycles." << cls_name << " = "
                   << (diag.ingress_queue_wait_after_nonempty_samples > 0
                           ? static_cast<double>(diag.ingress_queue_wait_after_nonempty_sum_cycles) /
                                 static_cast<double>(diag.ingress_queue_wait_after_nonempty_samples)
                           : 0.0)
                   << '\n';
                for (std::size_t b = 0; b < kDiagBucketCount; ++b) {
                    os << prefix << role << '.' << peer << ".ingress_pre_occ_bucket." << cls_name << '.'
                       << byte_bucket_name(b) << " = " << diag.pre_occ_bucket_counts[b] << '\n';
                    os << prefix << role << '.' << peer << ".ingress_queue_wait_bucket." << cls_name << '.'
                       << cycle_bucket_name(b) << " = " << diag.queue_wait_bucket_counts[b] << '\n';
                }
            }
        }
    };

    emit_peer_map(rx_by_src_peer_, "peer_src");
    emit_peer_map(rx_by_dst_peer_, "peer_dst");
    emit_peer_map(tx_by_src_peer_, "tx_src");
    emit_peer_map(tx_by_dst_peer_, "tx_dst");
}

bool FabricPort::can_receive(uint64_t cycle) {
    tick(cycle);
    if (last_deliver_cycle_ == cycle) {
        return false;
    }
    return !ready_.empty();
}

void FabricPort::tick_ingress() {
    if (!ingress_) {
        return;
    }
    auto ready = ingress_->on_tick();
    record_ingress_release(ready);
    for (auto& item : ready) {
        uint64_t wait_cycles = 0;
        auto it = ingress_enqueue_cycle_.find(item);
        TrafficClass cls = classify_event(item);
        bool saw_nonempty_queue = false;
        uint64_t src = 0;
        uint64_t dst = 0;
        if (it != ingress_enqueue_cycle_.end()) {
            const auto enqueue = it->second;
            const uint64_t enqueue_cycle = enqueue.enqueue_cycle;
            cls = enqueue.cls;
            src = enqueue.src;
            dst = enqueue.dst;
            saw_nonempty_queue = enqueue.saw_nonempty_queue;
            if (last_tick_cycle_ != std::numeric_limits<uint64_t>::max() && last_tick_cycle_ >= enqueue_cycle) {
                wait_cycles = last_tick_cycle_ - enqueue_cycle;
            }
            ingress_enqueue_cycle_.erase(it);
        }
        const uint64_t service_floor = ingress_service_floor_cycles(item);
        const uint64_t queue_wait = wait_cycles > service_floor ? (wait_cycles - service_floor) : 0;
        record_ingress_wait(cls, wait_cycles, queue_wait);
        record_conditional_ingress_release(cls, src, dst, saw_nonempty_queue, event_bytes(item), wait_cycles, queue_wait);
        push_ready(item);
    }
}

void FabricPort::drain_egress() {
    uint64_t sent_pkts = 0;
    uint64_t sent_bytes = 0;
    while (!egress_queue_.empty()) {
        EgressEntry entry = egress_queue_.front();
        csEvent* ev = entry.ev;
        if (!try_consume_credit(egress_credits_, credit_bytes(ev))) {
            egress_blocked_cycles_++;
            const auto occ_bytes = static_cast<uint64_t>(std::max<int64_t>(egress_queue_bytes_, 0));
            egress_blocked_occ_sum_bytes_ += occ_bytes;
            egress_blocked_occ_max_bytes_ = std::max(egress_blocked_occ_max_bytes_, occ_bytes);
            egress_blocked_cycles_by_class_[traffic_class_index(classify_event(ev))]++;
            break;
        }
        uint64_t wait_cycles = 0;
        if (last_tick_cycle_ != std::numeric_limits<uint64_t>::max() && last_tick_cycle_ >= entry.enqueue_cycle) {
            wait_cycles = last_tick_cycle_ - entry.enqueue_cycle;
        }
        egress_wait_sum_cycles_ += wait_cycles;
        egress_wait_samples_++;
        egress_wait_max_cycles_ = std::max(egress_wait_max_cycles_, wait_cycles);
        const auto cls = classify_event(ev);
        const auto idx = traffic_class_index(cls);
        egress_wait_sum_cycles_by_class_[idx] += wait_cycles;
        egress_wait_samples_by_class_[idx]++;
        egress_wait_max_cycles_by_class_[idx] = std::max(egress_wait_max_cycles_by_class_[idx], wait_cycles);
        link_->send(ev);
        record_tx(ev);
        sent_pkts++;
        sent_bytes += event_bytes(ev);
        egress_queue_.pop_front();
        egress_queue_bytes_ = std::max<int64_t>(egress_queue_bytes_ - static_cast<int64_t>(event_bytes(ev)), 0);
        egress_queue_bytes_by_class_[idx] =
            egress_queue_bytes_by_class_[idx] >= event_bytes(ev)
                ? egress_queue_bytes_by_class_[idx] - event_bytes(ev)
                : 0;
    }
    if (sent_pkts > 0) {
        egress_send_nonempty_cycles_++;
        egress_send_burst_sum_pkts_ += sent_pkts;
        egress_send_burst_sum_bytes_ += sent_bytes;
        egress_send_burst_sq_sum_pkts_ += static_cast<long double>(sent_pkts) * static_cast<long double>(sent_pkts);
        egress_send_burst_sq_sum_bytes_ += static_cast<long double>(sent_bytes) * static_cast<long double>(sent_bytes);
        egress_send_burst_max_pkts_ = std::max(egress_send_burst_max_pkts_, sent_pkts);
        egress_send_burst_max_bytes_ = std::max(egress_send_burst_max_bytes_, sent_bytes);
    }
}

void FabricPort::send_credit(uint64_t dst, uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    auto* credit = make_credit_event(self_id_, dst, bytes);
    link_->send(credit);
}

bool FabricPort::egress_queue_full(uint64_t bytes) const {
    return egress_queue_max_bytes_ > 0 &&
           (egress_queue_bytes_ + static_cast<int64_t>(bytes)) > egress_queue_max_bytes_;
}

uint64_t FabricPort::ingress_service_floor_cycles(const csEvent* item) const {
    uint64_t floor = 0;
    if (ingress_lat_cycles_ > 0) {
        floor += static_cast<uint64_t>(ingress_lat_cycles_);
    }
    if (ingress_bw_cycles_ > 0) {
        const uint64_t bytes = event_bytes(item);
        floor += (bytes * static_cast<uint64_t>(ingress_bw_cycles_) + 63ull) / 64ull;
    }
    return floor;
}

void FabricPort::push_ready(csEvent* item, bool front) {
    const uint64_t enqueue_cycle =
        (last_tick_cycle_ == std::numeric_limits<uint64_t>::max()) ? 0 : last_tick_cycle_;
    ready_enqueue_cycle_[item] = enqueue_cycle;
    if (front) {
        ready_.push_front(item);
    } else {
        ready_.push_back(item);
    }
    ready_occ_max_ = std::max(ready_occ_max_, ready_.size());
}

void FabricPort::record_ready_pop(uint64_t cycle, csEvent* item) {
    uint64_t wait_cycles = 0;
    auto it = ready_enqueue_cycle_.find(item);
    if (it != ready_enqueue_cycle_.end()) {
        const uint64_t enqueue_cycle = it->second;
        if (cycle >= enqueue_cycle) {
            wait_cycles = cycle - enqueue_cycle;
        }
        ready_enqueue_cycle_.erase(it);
    }
    ready_wait_sum_cycles_ += wait_cycles;
    ready_wait_samples_++;
    ready_wait_max_cycles_ = std::max(ready_wait_max_cycles_, wait_cycles);
}

FabricPort::TrafficClass FabricPort::classify_event(const csEvent* item) {
    if (!item) {
        return TrafficClass::OtherReq;
    }
    if (is_control_event(*item)) {
        return TrafficClass::OtherReq;
    }
    if (item->payload.size() > 10) {
        const auto type = static_cast<access_type>(item->payload[10]);
        if (type == access_type::LOAD || type == access_type::RFO) {
            return TrafficClass::DemandReq;
        }
        if (type == access_type::WRITE) {
            return TrafficClass::WriteReq;
        }
        return TrafficClass::OtherReq;
    }
    if (item->payload.size() > 8) {
        return TrafficClass::Response;
    }
    return TrafficClass::OtherReq;
}

void FabricPort::record_rx(const csEvent* item) {
    const auto bytes = event_bytes(item);
    rx_bytes_total_ += bytes;
    const auto cls = classify_event(item);
    const auto idx = static_cast<std::size_t>(cls);
    rx_bytes_by_class_[idx] += bytes;
    rx_packets_by_class_[idx]++;
    const uint64_t src = (item && !item->payload.empty()) ? item->payload[0] : 0;
    const uint64_t dst = (item && item->payload.size() > 1) ? item->payload[1] : 0;
    rx_by_src_peer_[src].classes[idx].rx_bytes += bytes;
    rx_by_src_peer_[src].classes[idx].rx_packets++;
    rx_by_dst_peer_[dst].classes[idx].rx_bytes += bytes;
    rx_by_dst_peer_[dst].classes[idx].rx_packets++;
}

void FabricPort::record_tx(const csEvent* item) {
    const auto bytes = event_bytes(item);
    tx_bytes_total_ += bytes;
    const auto cls = classify_event(item);
    const auto idx = static_cast<std::size_t>(cls);
    tx_bytes_by_class_[idx] += bytes;
    tx_packets_by_class_[idx]++;
    const uint64_t src = (item && !item->payload.empty()) ? item->payload[0] : self_id_;
    const uint64_t dst = (item && item->payload.size() > 1) ? item->payload[1] : 0;
    tx_by_src_peer_[src].classes[idx].tx_bytes += bytes;
    tx_by_src_peer_[src].classes[idx].tx_packets++;
    tx_by_dst_peer_[dst].classes[idx].tx_bytes += bytes;
    tx_by_dst_peer_[dst].classes[idx].tx_packets++;
}

void FabricPort::record_ingress_arrival(uint64_t cycle, uint64_t bytes) {
    if (ingress_arrival_burst_cycle_ != cycle) {
        if (ingress_arrival_burst_pkts_cur_ > 0) {
            ingress_arrival_nonempty_cycles_++;
            ingress_arrival_burst_sum_pkts_ += ingress_arrival_burst_pkts_cur_;
            ingress_arrival_burst_sum_bytes_ += ingress_arrival_burst_bytes_cur_;
            ingress_arrival_burst_sq_sum_pkts_ += static_cast<long double>(ingress_arrival_burst_pkts_cur_) *
                                                  static_cast<long double>(ingress_arrival_burst_pkts_cur_);
            ingress_arrival_burst_sq_sum_bytes_ += static_cast<long double>(ingress_arrival_burst_bytes_cur_) *
                                                   static_cast<long double>(ingress_arrival_burst_bytes_cur_);
        }
        if (ingress_arrival_prev_cycle_ == std::numeric_limits<uint64_t>::max()) {
            ingress_arrival_run_cur_cycles_ = 1;
        } else if (cycle == ingress_arrival_prev_cycle_ + 1) {
            ingress_arrival_run_cur_cycles_++;
        } else {
            if (ingress_arrival_run_cur_cycles_ > 0) {
                ingress_arrival_run_count_++;
                ingress_arrival_run_sum_cycles_ += ingress_arrival_run_cur_cycles_;
                ingress_arrival_run_sq_sum_cycles_ += static_cast<long double>(ingress_arrival_run_cur_cycles_) *
                                                      static_cast<long double>(ingress_arrival_run_cur_cycles_);
            }
            const uint64_t gap_cycles = cycle > ingress_arrival_prev_cycle_ ? (cycle - ingress_arrival_prev_cycle_ - 1) : 0;
            if (gap_cycles > 0) {
                ingress_arrival_gap_count_++;
                ingress_arrival_gap_sum_cycles_ += gap_cycles;
                ingress_arrival_gap_sq_sum_cycles_ += static_cast<long double>(gap_cycles) *
                                                      static_cast<long double>(gap_cycles);
                ingress_arrival_gap_max_cycles_ = std::max(ingress_arrival_gap_max_cycles_, gap_cycles);
            }
            ingress_arrival_run_cur_cycles_ = 1;
        }
        ingress_arrival_run_max_cycles_ = std::max(ingress_arrival_run_max_cycles_, ingress_arrival_run_cur_cycles_);
        ingress_arrival_prev_cycle_ = cycle;
        ingress_arrival_burst_cycle_ = cycle;
        ingress_arrival_burst_pkts_cur_ = 0;
        ingress_arrival_burst_bytes_cur_ = 0;
    }
    ingress_arrival_burst_pkts_cur_++;
    ingress_arrival_burst_bytes_cur_ += bytes;
    ingress_arrival_burst_max_pkts_ = std::max(ingress_arrival_burst_max_pkts_, ingress_arrival_burst_pkts_cur_);
    ingress_arrival_burst_max_bytes_ = std::max(ingress_arrival_burst_max_bytes_, ingress_arrival_burst_bytes_cur_);
}

void FabricPort::record_ingress_wait(TrafficClass cls, uint64_t wait_cycles, uint64_t queue_wait_cycles) {
    ingress_wait_sum_cycles_ += wait_cycles;
    ingress_wait_samples_++;
    ingress_wait_max_cycles_ = std::max(ingress_wait_max_cycles_, wait_cycles);
    ingress_queue_wait_sum_cycles_ += queue_wait_cycles;
    ingress_queue_wait_samples_++;
    ingress_queue_wait_max_cycles_ = std::max(ingress_queue_wait_max_cycles_, queue_wait_cycles);
    const auto idx = traffic_class_index(cls);
    ingress_wait_sum_cycles_by_class_[idx] += wait_cycles;
    ingress_wait_samples_by_class_[idx]++;
    ingress_wait_max_cycles_by_class_[idx] = std::max(ingress_wait_max_cycles_by_class_[idx], wait_cycles);
    ingress_queue_wait_sum_cycles_by_class_[idx] += queue_wait_cycles;
    ingress_queue_wait_samples_by_class_[idx]++;
    ingress_queue_wait_max_cycles_by_class_[idx] =
        std::max(ingress_queue_wait_max_cycles_by_class_[idx], queue_wait_cycles);
}

void FabricPort::record_conditional_ingress_arrival(TrafficClass cls,
                                                    uint64_t bytes,
                                                    uint64_t pre_enqueue_occ_bytes,
                                                    uint64_t src,
                                                    uint64_t dst) {
    if (!ingress_episode_active_) {
        ingress_episode_state_ = EpisodeState{};
        ingress_episode_active_ = true;
    }
    ingress_episode_state_.arrival_pkts++;
    ingress_episode_state_.arrival_bytes += bytes;
    ingress_episode_state_.arrival_pkts_by_class[traffic_class_index(cls)]++;
    ingress_episode_state_.srcs.insert(src);
    ingress_episode_state_.dsts.insert(dst);
    ingress_episode_state_.peak_occ_bytes =
        std::max(ingress_episode_state_.peak_occ_bytes, pre_enqueue_occ_bytes + bytes);
    const auto idx = traffic_class_index(cls);
    ingress_pre_occ_bucket_counts_by_class_[idx][byte_bucket_index(pre_enqueue_occ_bytes)]++;
    auto& src_diag = rx_by_src_peer_[src].classes[idx];
    auto& dst_diag = rx_by_dst_peer_[dst].classes[idx];
    src_diag.ingress_arrivals++;
    dst_diag.ingress_arrivals++;
    if (pre_enqueue_occ_bytes > 0) {
        ingress_arrival_nonempty_packets_by_class_[idx]++;
        ingress_arrival_nonempty_bytes_by_class_[idx] += bytes;
        ingress_nonempty_pre_occ_sum_bytes_by_class_[idx] += pre_enqueue_occ_bytes;
        ingress_nonempty_pre_occ_max_bytes_by_class_[idx] =
            std::max(ingress_nonempty_pre_occ_max_bytes_by_class_[idx], pre_enqueue_occ_bytes);
        src_diag.ingress_nonempty_arrivals++;
        src_diag.ingress_nonempty_pre_occ_sum_bytes += pre_enqueue_occ_bytes;
        src_diag.ingress_nonempty_pre_occ_max_bytes =
            std::max(src_diag.ingress_nonempty_pre_occ_max_bytes, pre_enqueue_occ_bytes);
        src_diag.pre_occ_bucket_counts[byte_bucket_index(pre_enqueue_occ_bytes)]++;
        dst_diag.ingress_nonempty_arrivals++;
        dst_diag.ingress_nonempty_pre_occ_sum_bytes += pre_enqueue_occ_bytes;
        dst_diag.ingress_nonempty_pre_occ_max_bytes =
            std::max(dst_diag.ingress_nonempty_pre_occ_max_bytes, pre_enqueue_occ_bytes);
        dst_diag.pre_occ_bucket_counts[byte_bucket_index(pre_enqueue_occ_bytes)]++;
    } else {
        ingress_arrival_empty_packets_by_class_[idx]++;
        ingress_arrival_empty_bytes_by_class_[idx] += bytes;
        src_diag.pre_occ_bucket_counts[0]++;
        dst_diag.pre_occ_bucket_counts[0]++;
    }
}

void FabricPort::record_conditional_ingress_release(TrafficClass cls,
                                                    uint64_t src,
                                                    uint64_t dst,
                                                    bool saw_nonempty_queue,
                                                    uint64_t bytes,
                                                    uint64_t wait_cycles,
                                                    uint64_t queue_wait_cycles) {
    if (!ingress_episode_active_) {
        ingress_episode_state_ = EpisodeState{};
        ingress_episode_active_ = true;
    }
    ingress_episode_state_.release_pkts++;
    ingress_episode_state_.release_bytes += bytes;
    ingress_episode_state_.release_pkts_by_class[traffic_class_index(cls)]++;
    ingress_episode_state_.srcs.insert(src);
    ingress_episode_state_.dsts.insert(dst);
    ingress_episode_state_.total_wait_cycles += wait_cycles;
    ingress_episode_state_.max_wait_cycles =
        std::max(ingress_episode_state_.max_wait_cycles, wait_cycles);
    ingress_episode_state_.total_queue_wait_cycles += queue_wait_cycles;
    ingress_episode_state_.max_queue_wait_cycles =
        std::max(ingress_episode_state_.max_queue_wait_cycles, queue_wait_cycles);
    const auto idx = traffic_class_index(cls);
    ingress_queue_wait_bucket_counts_by_class_[idx][cycle_bucket_index(queue_wait_cycles)]++;
    auto update_peer = [&](PeerClassDiag& diag) {
        diag.ingress_wait_sum_cycles += wait_cycles;
        diag.ingress_wait_samples++;
        diag.ingress_wait_max_cycles = std::max(diag.ingress_wait_max_cycles, wait_cycles);
        diag.ingress_queue_wait_sum_cycles += queue_wait_cycles;
        diag.ingress_queue_wait_samples++;
        diag.ingress_queue_wait_max_cycles = std::max(diag.ingress_queue_wait_max_cycles, queue_wait_cycles);
        diag.queue_wait_bucket_counts[cycle_bucket_index(queue_wait_cycles)]++;
        if (saw_nonempty_queue) {
            diag.ingress_wait_after_nonempty_sum_cycles += wait_cycles;
            diag.ingress_wait_after_nonempty_samples++;
            diag.ingress_queue_wait_after_nonempty_sum_cycles += queue_wait_cycles;
            diag.ingress_queue_wait_after_nonempty_samples++;
        }
    };
    update_peer(rx_by_src_peer_[src].classes[idx]);
    update_peer(rx_by_dst_peer_[dst].classes[idx]);
    if (saw_nonempty_queue) {
        ingress_release_after_nonempty_packets_by_class_[idx]++;
        ingress_wait_after_nonempty_sum_cycles_by_class_[idx] += wait_cycles;
        ingress_queue_wait_after_nonempty_sum_cycles_by_class_[idx] += queue_wait_cycles;
        ingress_queue_wait_after_nonempty_max_cycles_by_class_[idx] =
            std::max(ingress_queue_wait_after_nonempty_max_cycles_by_class_[idx], queue_wait_cycles);
    } else {
        ingress_release_after_empty_packets_by_class_[idx]++;
        ingress_wait_after_empty_sum_cycles_by_class_[idx] += wait_cycles;
        ingress_queue_wait_after_empty_sum_cycles_by_class_[idx] += queue_wait_cycles;
        ingress_queue_wait_after_empty_max_cycles_by_class_[idx] =
            std::max(ingress_queue_wait_after_empty_max_cycles_by_class_[idx], queue_wait_cycles);
    }
}

void FabricPort::record_ingress_release(const std::vector<csEvent*>& ready) {
    uint64_t burst_pkts = 0;
    uint64_t burst_bytes = 0;
    for (const auto* item : ready) {
        burst_pkts++;
        burst_bytes += event_bytes(item);
    }
    if (burst_pkts > 0) {
        ingress_release_nonempty_cycles_++;
        ingress_release_burst_sum_pkts_ += burst_pkts;
        ingress_release_burst_sum_bytes_ += burst_bytes;
        ingress_release_burst_sq_sum_pkts_ += static_cast<long double>(burst_pkts) * static_cast<long double>(burst_pkts);
        ingress_release_burst_sq_sum_bytes_ += static_cast<long double>(burst_bytes) * static_cast<long double>(burst_bytes);
        if (ingress_release_gap_cur_cycles_ > 0) {
            ingress_release_gap_count_++;
            ingress_release_gap_sum_cycles_ += ingress_release_gap_cur_cycles_;
            ingress_release_gap_sq_sum_cycles_ += static_cast<long double>(ingress_release_gap_cur_cycles_) *
                                                  static_cast<long double>(ingress_release_gap_cur_cycles_);
            ingress_release_gap_cur_cycles_ = 0;
        }
        ingress_release_seen_nonempty_ = true;
        ingress_release_run_cur_cycles_++;
        ingress_release_run_max_cycles_ = std::max(ingress_release_run_max_cycles_, ingress_release_run_cur_cycles_);
    } else if (ingress_release_seen_nonempty_) {
        if (ingress_release_run_cur_cycles_ > 0) {
            ingress_release_run_count_++;
            ingress_release_run_sum_cycles_ += ingress_release_run_cur_cycles_;
            ingress_release_run_sq_sum_cycles_ += static_cast<long double>(ingress_release_run_cur_cycles_) *
                                                  static_cast<long double>(ingress_release_run_cur_cycles_);
            ingress_release_run_cur_cycles_ = 0;
        }
        ingress_release_gap_cur_cycles_++;
        ingress_release_gap_max_cycles_ = std::max(ingress_release_gap_max_cycles_, ingress_release_gap_cur_cycles_);
    }
    ingress_release_burst_max_pkts_ = std::max(ingress_release_burst_max_pkts_, burst_pkts);
    ingress_release_burst_max_bytes_ = std::max(ingress_release_burst_max_bytes_, burst_bytes);
}

} // namespace csimCore
} // namespace SST
