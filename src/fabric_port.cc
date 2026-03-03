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

uint64_t msg_bytes(const csEvent& ev) {
    if (ev.payload.size() > 16) {
        return std::max<uint64_t>(ev.payload[16], 1);
    }
    if (ev.payload.size() > 8) {
        return std::max<uint64_t>(ev.payload[8], 1);
    }
    return kDefaultMsgBytes;
}

uint64_t event_bytes(csEvent* const& ev) {
    return ev ? msg_bytes(*ev) : kDefaultMsgBytes;
}

int64_t credit_capacity_bytes(int64_t queue_size_packets) {
    if (queue_size_packets > 0) {
        return queue_size_packets * static_cast<int64_t>(kDefaultMsgBytes);
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

uint64_t event_credit_dst(csEvent* const& ev) {
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
                           int64_t queue_size_packets) {
    if (bw_cycles < 0 || lat_cycles < 0 || queue_size_packets < 0) {
        throw std::runtime_error("FabricPort: negative configuration values are invalid.");
    }
    link_ = link;
    self_id_ = self_id;

    if (bw_cycles > 0 || lat_cycles > 0 || queue_size_packets > 0) {
        const int64_t bw = std::max<int64_t>(bw_cycles, 1);
        const int64_t lat = std::max<int64_t>(lat_cycles, 1);
        const double peak_bw_per_cycle = 64.0 / static_cast<double>(bw);
        auto latency_fn = [lat](double) { return lat; };
        auto bw_cost_fn = [](csEvent* const& item) {
            return static_cast<double>(event_bytes(item));
        };
        const int64_t ingress_max_pending = queue_size_packets > 0 ? queue_size_packets : 0;
        ingress_ = std::make_unique<::lat_bw_queue<csEvent*>>(peak_bw_per_cycle,
                                                           latency_fn,
                                                           bw_cost_fn,
                                                           ingress_max_pending);
    }
    egress_credit_cap_ = credit_capacity_bytes(queue_size_packets);
    egress_credits_ = egress_credit_cap_;
    egress_queue_max_ = queue_size_packets > 0
        ? static_cast<std::size_t>(queue_size_packets)
        : std::size_t{0};
}

bool FabricPort::send(csEvent* item) {
    if (!can_send()) {
        return false;
    }
    egress_queue_.push_back(item);
    return true;
}

std::optional<csEvent*> FabricPort::receive(uint64_t cycle) {
    if (!can_receive(cycle)) {
        return std::nullopt;
    }
    csEvent* item = std::move(ready_.front());
    ready_.pop_front();
    last_deliver_cycle_ = cycle;
    return item;
}

bool FabricPort::try_receive(uint64_t cycle,
                             const std::function<bool(csEvent*)>& handle) {
    if (!can_receive(cycle)) {
        return false;
    }
    csEvent* item = ready_.front();
    if (!handle(item)) {
        return false;
    }
    ready_.pop_front();
    last_deliver_cycle_ = cycle;
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
        }
    }

    if (ingress_) {
        ingress_->add_packet(cevent);
    } else {
        ready_.push_back(cevent);
        send_credit(cevent);
    }
}

void FabricPort::reset_ingress_utilization() {
    if (ingress_) {
        ingress_->reset_utilization();
    }
}

bool FabricPort::has_ingress() const {
    return static_cast<bool>(ingress_);
}

bool FabricPort::has_link() const {
    return link_ != nullptr;
}

bool FabricPort::can_send() const {
    return link_ != nullptr && !egress_queue_full();
}

double FabricPort::ingress_avg_utilization() const {
    return ingress_ ? ingress_->average_utilization() : 0.0;
}

double FabricPort::ingress_utilization() const {
    return ingress_ ? ingress_->utilization() : 0.0;
}

std::size_t FabricPort::ingress_occupancy() const {
    return ingress_ ? ingress_->occupancy() : 0;
}

bool FabricPort::can_receive(uint64_t cycle) {
    if (last_tick_cycle_ != cycle) {
        last_tick_cycle_ = cycle;
        tick_ingress();
        drain_egress();
    }
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
        send_credit(item);
    }
}

void FabricPort::drain_egress() {
    if (!link_) {
        return;
    }
    while (!egress_queue_.empty()) {
        csEvent* ev = egress_queue_.front();
        if (!try_consume_credit(egress_credits_, event_bytes(ev))) {
            break;
        }
        link_->send(ev);
        egress_queue_.pop_front();
    }
}

void FabricPort::send_credit(csEvent* const& item) {
    if (!link_) {
        return;
    }
    auto* credit = make_credit_event(self_id_, event_credit_dst(item), event_bytes(item));
    link_->send(credit);
}

bool FabricPort::egress_queue_full() const {
    return egress_queue_max_ > 0 && egress_queue_.size() >= egress_queue_max_;
}

} // namespace csimCore
} // namespace SST
