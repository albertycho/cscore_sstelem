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
    ingress_release_burst_max_pkts_ = 0;
    ingress_release_burst_max_bytes_ = 0;
    ingress_release_nonempty_cycles_ = 0;
    ingress_release_burst_sum_pkts_ = 0;
    ingress_release_burst_sum_bytes_ = 0;
    ingress_release_burst_sq_sum_pkts_ = 0.0L;
    ingress_release_burst_sq_sum_bytes_ = 0.0L;
    ingress_occ_sum_bytes_ = 0;
    ingress_occ_sq_sum_bytes_ = 0.0L;
    ingress_occ_samples_ = 0;
    ingress_occ_nonempty_cycles_ = 0;
    ingress_occ_max_bytes_ = 0;
    tick_samples_ = 0;
    tx_bytes_total_ = 0;
    rx_bytes_total_ = 0;
    tx_bytes_by_class_.fill(0);
    rx_bytes_by_class_.fill(0);
    tx_packets_by_class_.fill(0);
    rx_packets_by_class_.fill(0);
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
    return true;
}

void FabricPort::tick(uint64_t cycle) {
    if (last_tick_cycle_ != cycle) {
        last_tick_cycle_ = cycle;
        tick_ingress();
        drain_egress();
        const auto occ = ingress_ ? static_cast<uint64_t>(ingress_->occupancy()) : 0;
        ingress_occ_sum_bytes_ += occ;
        ingress_occ_sq_sum_bytes_ += static_cast<long double>(occ) * static_cast<long double>(occ);
        ingress_occ_samples_++;
        ingress_occ_max_bytes_ = std::max(ingress_occ_max_bytes_, occ);
        if (occ > 0) {
            ingress_occ_nonempty_cycles_++;
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
            reset_ingress_utilization();
            // Reset controls are out-of-band: do not let data backlog delay phase reset propagation.
            push_ready(cevent, true);
            return;
        }
    }

    record_rx(cevent);
    record_ingress_arrival((last_tick_cycle_ == std::numeric_limits<uint64_t>::max()) ? 0 : last_tick_cycle_,
                           event_bytes(cevent));
    if (!ingress_) {
        ingress_wait_samples_++;
        push_ready(cevent);
        return;
    }

    if (!ingress_->add_packet(cevent)) {
        throw std::runtime_error("FabricPort: ingress queue full; credit accounting mismatch.");
    }
    const uint64_t enqueue_cycle =
        (last_tick_cycle_ == std::numeric_limits<uint64_t>::max()) ? 0 : last_tick_cycle_;
    ingress_enqueue_cycle_[cevent] = enqueue_cycle;
}

void FabricPort::reset_ingress_utilization() {
    if (ingress_) {
        ingress_->reset_utilization();
    }
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

double FabricPort::egress_wait_avg_cycles() const {
    if (egress_wait_samples_ == 0) {
        return 0.0;
    }
    return static_cast<double>(egress_wait_sum_cycles_) / static_cast<double>(egress_wait_samples_);
}

uint64_t FabricPort::egress_wait_max_cycles() const {
    return egress_wait_max_cycles_;
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
        if (it != ingress_enqueue_cycle_.end()) {
            const uint64_t enqueue_cycle = it->second;
            if (last_tick_cycle_ != std::numeric_limits<uint64_t>::max() && last_tick_cycle_ >= enqueue_cycle) {
                wait_cycles = last_tick_cycle_ - enqueue_cycle;
            }
            ingress_enqueue_cycle_.erase(it);
        }
        ingress_wait_sum_cycles_ += wait_cycles;
        ingress_wait_samples_++;
        ingress_wait_max_cycles_ = std::max(ingress_wait_max_cycles_, wait_cycles);
        const uint64_t service_floor = ingress_service_floor_cycles(item);
        const uint64_t queue_wait = wait_cycles > service_floor ? (wait_cycles - service_floor) : 0;
        ingress_queue_wait_sum_cycles_ += queue_wait;
        ingress_queue_wait_samples_++;
        ingress_queue_wait_max_cycles_ = std::max(ingress_queue_wait_max_cycles_, queue_wait);
        push_ready(item);
    }
}

void FabricPort::drain_egress() {
    while (!egress_queue_.empty()) {
        EgressEntry entry = egress_queue_.front();
        csEvent* ev = entry.ev;
        if (!try_consume_credit(egress_credits_, credit_bytes(ev))) {
            break;
        }
        uint64_t wait_cycles = 0;
        if (last_tick_cycle_ != std::numeric_limits<uint64_t>::max() && last_tick_cycle_ >= entry.enqueue_cycle) {
            wait_cycles = last_tick_cycle_ - entry.enqueue_cycle;
        }
        egress_wait_sum_cycles_ += wait_cycles;
        egress_wait_samples_++;
        egress_wait_max_cycles_ = std::max(egress_wait_max_cycles_, wait_cycles);
        link_->send(ev);
        record_tx(ev);
        egress_queue_.pop_front();
        egress_queue_bytes_ = std::max<int64_t>(egress_queue_bytes_ - static_cast<int64_t>(event_bytes(ev)), 0);
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
    rx_bytes_by_class_[static_cast<std::size_t>(cls)] += bytes;
    rx_packets_by_class_[static_cast<std::size_t>(cls)]++;
}

void FabricPort::record_tx(const csEvent* item) {
    const auto bytes = event_bytes(item);
    tx_bytes_total_ += bytes;
    const auto cls = classify_event(item);
    tx_bytes_by_class_[static_cast<std::size_t>(cls)] += bytes;
    tx_packets_by_class_[static_cast<std::size_t>(cls)]++;
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
        ingress_arrival_burst_cycle_ = cycle;
        ingress_arrival_burst_pkts_cur_ = 0;
        ingress_arrival_burst_bytes_cur_ = 0;
    }
    ingress_arrival_burst_pkts_cur_++;
    ingress_arrival_burst_bytes_cur_ += bytes;
    ingress_arrival_burst_max_pkts_ = std::max(ingress_arrival_burst_max_pkts_, ingress_arrival_burst_pkts_cur_);
    ingress_arrival_burst_max_bytes_ = std::max(ingress_arrival_burst_max_bytes_, ingress_arrival_burst_bytes_cur_);
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
    }
    ingress_release_burst_max_pkts_ = std::max(ingress_release_burst_max_pkts_, burst_pkts);
    ingress_release_burst_max_bytes_ = std::max(ingress_release_burst_max_bytes_, burst_bytes);
}

} // namespace csimCore
} // namespace SST
