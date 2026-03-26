#include "fabric_port.h"

#include <algorithm>
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
                           int64_t egress_buffer_bytes,
                           int64_t credit_window_bytes) {
    if (bw_cycles < 0 || lat_cycles < 0 || egress_buffer_bytes < 0 || credit_window_bytes < 0) {
        throw std::runtime_error("FabricPort: negative configuration values are invalid.");
    }
    if (!link) {
        throw std::runtime_error("FabricPort: null link provided to configure.");
    }
    link_ = link;
    self_id_ = self_id;
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
    egress_credit_cap_ = credit_capacity_bytes(credit_window_bytes);
    egress_credits_ = egress_credit_cap_;
    egress_queue_max_bytes_ = egress_buffer_bytes > 0 ? egress_buffer_bytes : 0;
    egress_queue_bytes_ = 0;
    tx_bytes_total_ = 0;
    rx_bytes_total_ = 0;
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
    egress_queue_.push_back(item);
    egress_queue_bytes_ += static_cast<int64_t>(bytes);
    return true;
}

void FabricPort::tick(uint64_t cycle) {
    advance(cycle);
}

void FabricPort::advance(uint64_t cycle) {
    if (last_tick_cycle_ != cycle) {
        last_tick_cycle_ = cycle;
        tick_ingress();
        drain_egress();
    }
}

bool FabricPort::try_receive(uint64_t cycle,
                             const std::function<bool(csEvent*)>& handle) {
    advance(cycle);
    return try_receive_ready(cycle, handle);
}

bool FabricPort::try_receive_ready(uint64_t cycle,
                                   const std::function<bool(csEvent*)>& handle) {
    if (!has_ready_to_receive(cycle)) {
        return false;
    }
    csEvent* item = ready_.front();
    const uint64_t credit_dst = event_credit_dst(item);
    const uint64_t credit_len = credit_bytes(item);
    if (!handle(item)) {
        return false;
    }
    ready_.pop_front();
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
            ready_.push_front(cevent);
            return;
        }
    }

    if (!ingress_) {
        rx_bytes_total_ += event_bytes(cevent);
        ready_.push_back(cevent);
        return;
    }

    rx_bytes_total_ += event_bytes(cevent);
    if (!ingress_->add_packet(cevent)) {
        throw std::runtime_error("FabricPort: ingress queue full; credit accounting mismatch.");
    }
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

uint64_t FabricPort::tx_bytes_total() const {
    return tx_bytes_total_;
}

uint64_t FabricPort::rx_bytes_total() const {
    return rx_bytes_total_;
}

bool FabricPort::has_ready_to_receive(uint64_t cycle) const {
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
    for (auto& item : ready) {
        ready_.push_back(item);
    }
}

void FabricPort::drain_egress() {
    while (!egress_queue_.empty()) {
        csEvent* ev = egress_queue_.front();
        if (!try_consume_credit(egress_credits_, credit_bytes(ev))) {
            break;
        }
        const uint64_t send_bytes = event_bytes(ev);
        link_->send(ev);
        egress_queue_.pop_front();
        tx_bytes_total_ += send_bytes;
        egress_queue_bytes_ = std::max<int64_t>(egress_queue_bytes_ - static_cast<int64_t>(send_bytes), 0);
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

} // namespace csimCore
} // namespace SST
