#include "switch.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>

#include "access_type.h"
#include "control_event.h"

namespace SST {
namespace csimCore {

namespace {
bool is_write_request(const csEvent& ev) {
    if (ev.payload.size() <= 10) {
        return false;
    }
    const auto type = static_cast<access_type>(ev.payload[10]);
    return type == access_type::WRITE;
}

bool is_reset_event(const csEvent& ev) {
    return (ev.payload.size() == 3 || ev.payload.size() == 4) && ev.payload[2] == kControlResetUtil;
}

csEvent* clone_event_with_dst(const csEvent& ev, uint64_t dst) {
    auto* out = new csEvent();
    out->payload = ev.payload;
    out->last = ev.last;
    if (out->payload.size() >= 2) {
        out->payload[1] = dst;
    }
    return out;
}

FabricPort::TrafficClass classify_fabric_event(const csEvent& ev) {
    if (is_control_event(ev)) {
        return FabricPort::TrafficClass::OtherReq;
    }
    if (ev.payload.size() > 10) {
        const auto type = static_cast<access_type>(ev.payload[10]);
        if (type == access_type::LOAD || type == access_type::RFO) {
            return FabricPort::TrafficClass::DemandReq;
        }
        if (type == access_type::WRITE) {
            return FabricPort::TrafficClass::WriteReq;
        }
        return FabricPort::TrafficClass::OtherReq;
    }
    if (ev.payload.size() > 8) {
        return FabricPort::TrafficClass::Response;
    }
    return FabricPort::TrafficClass::OtherReq;
}

double parse_clock_ghz(std::string clock_str) {
    clock_str.erase(
        std::remove_if(clock_str.begin(), clock_str.end(), [](unsigned char c) { return std::isspace(c); }),
        clock_str.end());
    if (clock_str.empty()) {
        return 0.0;
    }
    std::size_t idx = 0;
    double value = 0.0;
    try {
        value = std::stod(clock_str, &idx);
    } catch (...) {
        return 0.0;
    }
    if (idx >= clock_str.size()) {
        return 0.0;
    }
    std::string unit = clock_str.substr(idx);
    for (auto& ch : unit) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (unit == "ghz") return value;
    if (unit == "mhz") return value / 1'000.0;
    if (unit == "khz") return value / 1'000'000.0;
    if (unit == "hz") return value / 1'000'000'000.0;
    if (unit == "thz") return value * 1'000.0;
    return 0.0;
}

} // namespace

Switch::Switch(SST::ComponentId_t id, SST::Params& params)
    : Component(id)
{
    num_nodes_ = params.find<int>("num_nodes", 0);
    num_pools_ = params.find<int>("num_pools", 0);
    pool_node_id_base_ = params.find<uint64_t>("pool_node_id_base", 100);
    replicate_writes_ = params.find<int>("replicate_writes", 0) != 0;
    {
        auto policy = params.find<std::string>("pool_select_policy", "round_robin");
        for (auto& ch : policy) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (policy == "fixed0" || policy == "fixed") {
            pool_select_policy_ = PoolSelectPolicy::Fixed0;
        } else {
            pool_select_policy_ = PoolSelectPolicy::RoundRobin;
        }
    }
    clock_frequency_ = params.find<std::string>("clock", "2.4GHz");
    link_bw_cycles_ = params.find<int64_t>("link_bw_cycles", 0);
    link_latency_cycles_ = params.find<int64_t>("link_latency_cycles", 0);
    link_queue_size_ = params.find<int64_t>("link_queue_size", 0);
    lightweight_output_ = params.find<int>("lightweight_output", 0) != 0;

    if (num_nodes_ <= 0) {
        throw std::runtime_error("Switch: num_nodes must be > 0.");
    }
    if (num_pools_ <= 0) {
        throw std::runtime_error("Switch: num_pools must be > 0.");
    }
    if (pool_node_id_base_ < static_cast<uint64_t>(num_nodes_)) {
        throw std::runtime_error("Switch: pool_node_id_base overlaps node id range. "
                                 "pool_node_id_base must be >= num_nodes.");
    }

    node_ports_.resize(std::max(num_nodes_, 0));
    for (int i = 0; i < num_nodes_; ++i) {
        std::string port_name = "port_handler_nodes" + std::to_string(i);
        auto* link = configureLink(port_name, new Event::Handler<Switch>(this, &Switch::handle_event));
        if (!link) {
            throw std::runtime_error("Switch: missing link for " + port_name +
                                     ". Ensure the topology connects all configured node ports.");
        }
        node_ports_[i].port.configure(link,
                                      static_cast<uint64_t>(i),
                                      link_bw_cycles_,
                                      link_latency_cycles_,
                                      link_queue_size_);
    }

    pool_ports_.resize(std::max(num_pools_, 0));
    for (int p = 0; p < num_pools_; ++p) {
        std::string port_name = "port_handler_pools" + std::to_string(p);
        auto* link = configureLink(port_name, new Event::Handler<Switch>(this, &Switch::handle_event));
        if (!link) {
            throw std::runtime_error("Switch: missing link for " + port_name +
                                     ". Ensure the topology connects all configured pool ports.");
        }
        const uint64_t port_id = pool_node_id_base_ + static_cast<uint64_t>(p);
        pool_ports_[p].port.configure(link,
                                      port_id,
                                      link_bw_cycles_,
                                      link_latency_cycles_,
                                      link_queue_size_);
    }

    registerClock(clock_frequency_, new Clock::Handler<Switch>(this, &Switch::clock_tick));
}

void Switch::setup() {
    wall_start_ = std::chrono::steady_clock::now();
    active_time_ = std::chrono::steady_clock::duration{};
    active_calls_ = 0;
    tick_count_ = 0;
    route_blocked_to_node_ = 0;
    route_blocked_to_pool_ = 0;
    route_blocked_replicated_write_ = 0;
    route_send_fail_to_node_ = 0;
    route_send_fail_to_pool_ = 0;
    route_attempt_to_node_by_class_.fill(0);
    route_attempt_to_pool_by_class_.fill(0);
    route_blocked_to_node_by_class_.fill(0);
    route_blocked_to_pool_by_class_.fill(0);
    route_send_fail_to_node_by_class_.fill(0);
    route_send_fail_to_pool_by_class_.fill(0);
    route_replicated_clones_by_class_.fill(0);
    stats_start_tick_ = 0;
    last_cycle_ = 0;
}

bool Switch::clock_tick(SST::Cycle_t cycle)
{
    ScopedTimer timer(active_time_, active_calls_);
    ++tick_count_;
    const auto cycle_u = static_cast<uint64_t>(cycle);
    last_cycle_ = cycle_u;
    for_each_port([&](PortState& port) {
        port.port.tick(cycle_u);
        try_receive_and_route(port, cycle_u);
    });
    return false;
}

void Switch::handle_event(SST::Event* ev)
{
    auto* cevent = dynamic_cast<csEvent*>(ev);
    if (!cevent) {
        throw std::runtime_error("Switch: received non-csEvent on a switch port.");
    }
    if (cevent->payload.size() < 2) {
        throw std::runtime_error("Switch: received malformed csEvent (payload size < 2).");
    }
    PortState* port = nullptr;
    const uint64_t src = cevent->payload[0];
    if (src < static_cast<uint64_t>(num_nodes_)) {
        const auto idx = static_cast<size_t>(src);
        if (idx >= node_ports_.size()) {
            throw std::runtime_error("Switch: src node id out of range for configured ports.");
        }
        port = &node_ports_[idx];
    } else if (src >= pool_node_id_base_) {
        const auto idx = static_cast<size_t>(src - pool_node_id_base_);
        if (idx >= pool_ports_.size()) {
            throw std::runtime_error("Switch: src pool id out of range for configured ports.");
        }
        port = &pool_ports_[idx];
    }

    if (!port) {
        throw std::runtime_error("Switch: unable to map src id to a port.");
    }

    port->port.handle_event(cevent);

}

bool Switch::try_route_event(csEvent* ev)
{
    if (!ev) {
        throw std::runtime_error("Switch: null event in try_route_event.");
    }
    if (ev->payload.size() < 2) {
        throw std::runtime_error("Switch: received malformed csEvent (payload size < 2).");
    }
    if (is_reset_event(*ev)) {
        reset_stats_and_broadcast();
        delete ev;
        return true;
    }
    const uint64_t dst = ev->payload[1];
    const auto cls = classify_fabric_event(*ev);
    const auto cls_idx = static_cast<std::size_t>(cls);
    if (dst < static_cast<uint64_t>(num_nodes_)) {
        const auto idx = static_cast<size_t>(dst);
        if (idx >= node_ports_.size()) {
            throw std::runtime_error("Switch: dst node id out of range for configured ports.");
        }
        route_attempt_to_node_by_class_[cls_idx]++;
        if (!node_ports_[idx].port.can_send(ev)) {
            route_blocked_to_node_++;
            route_blocked_to_node_by_class_[cls_idx]++;
            return false;
        }
        if (!node_ports_[idx].port.send(ev)) {
            route_send_fail_to_node_++;
            route_send_fail_to_node_by_class_[cls_idx]++;
            delete ev;
        }
        return true;
    }

    if (dst >= pool_node_id_base_) {
        const auto idx = static_cast<size_t>(dst - pool_node_id_base_);
        if (idx >= pool_ports_.size()) {
            throw std::runtime_error("Switch: dst pool id out of range for configured ports.");
        }
        if (replicate_writes_ && is_write_request(*ev)) {
            for (const auto& pool : pool_ports_) {
                if (!pool.port.can_send(ev)) {
                    route_blocked_replicated_write_++;
                    route_blocked_to_pool_by_class_[cls_idx]++;
                    return false;
                }
            }
            for (int p = 0; p < num_pools_; ++p) {
                const uint64_t pool_dst = pool_node_id_base_ + static_cast<uint64_t>(p);
                auto* clone = clone_event_with_dst(*ev, pool_dst);
                replicated_count_++;
                route_attempt_to_pool_by_class_[cls_idx]++;
                route_replicated_clones_by_class_[cls_idx]++;
                if (!pool_ports_[static_cast<size_t>(p)].port.send(clone)) {
                    route_send_fail_to_pool_++;
                    route_send_fail_to_pool_by_class_[cls_idx]++;
                    delete clone;
                }
            }
            delete ev;
            return true;
        }
        std::size_t pick = pick_pool_index(true, ev);
        if (pick >= pool_ports_.size()) {
            route_blocked_to_pool_++;
            route_blocked_to_pool_by_class_[cls_idx]++;
            return false;
        }
        route_attempt_to_pool_by_class_[cls_idx]++;
        if (!pool_ports_[pick].port.can_send(ev)) {
            route_blocked_to_pool_++;
            route_blocked_to_pool_by_class_[cls_idx]++;
            return false;
        }
        ev->payload[1] = pool_node_id_base_ + pick;
        if (!pool_ports_[pick].port.send(ev)) {
            route_send_fail_to_pool_++;
            route_send_fail_to_pool_by_class_[cls_idx]++;
            delete ev;
        }
        return true;
    }

    const std::string msg =
        "Switch: dst id " + std::to_string(dst) +
        " is not in node range [0," + std::to_string(num_nodes_ - 1) +
        "] or pool range [" + std::to_string(pool_node_id_base_) + "," +
        std::to_string(pool_node_id_base_ + static_cast<uint64_t>(num_pools_ - 1)) + "].";
    delete ev;
    throw std::runtime_error(msg);
}

void Switch::try_receive_and_route(PortState& port, uint64_t cycle)
{
    port.port.try_receive(cycle, [this](csEvent* ev) {
        return try_route_event(ev);
    });
}

void Switch::for_each_port(const std::function<void(PortState&)>& fn)
{
    for (auto& port : node_ports_) {
        fn(port);
    }
    for (auto& port : pool_ports_) {
        fn(port);
    }
}

void Switch::for_each_port(const std::function<void(const PortState&)>& fn) const
{
    for (const auto& port : node_ports_) {
        fn(port);
    }
    for (const auto& port : pool_ports_) {
        fn(port);
    }
}

std::size_t Switch::pick_pool_index(bool advance, const csEvent* probe)
{
    if (pool_ports_.empty()) {
        return pool_ports_.size();
    }
    if (pool_select_policy_ == PoolSelectPolicy::Fixed0) {
        return 0;
    }
    const std::size_t total = pool_ports_.size();
    for (std::size_t offset = 0; offset < total; ++offset) {
        const std::size_t idx = (rr_pool_idx_ + offset) % total;
        const auto& port = pool_ports_[idx].port;
        if (!port.can_send(probe)) {
            continue;
        }
        if (advance) {
            rr_pool_idx_ = (idx + 1) % total;
        }
        return idx;
    }
    return total;
}

void Switch::reset_stats_and_broadcast()
{
    for_each_port([this](PortState& port) { port.port.reset_stats(last_cycle_); });
    route_blocked_to_node_ = 0;
    route_blocked_to_pool_ = 0;
    route_blocked_replicated_write_ = 0;
    route_send_fail_to_node_ = 0;
    route_send_fail_to_pool_ = 0;
    route_attempt_to_node_by_class_.fill(0);
    route_attempt_to_pool_by_class_.fill(0);
    route_blocked_to_node_by_class_.fill(0);
    route_blocked_to_pool_by_class_.fill(0);
    route_send_fail_to_node_by_class_.fill(0);
    route_send_fail_to_pool_by_class_.fill(0);
    route_replicated_clones_by_class_.fill(0);
    stats_start_tick_ = tick_count_;
    for (int p = 0; p < num_pools_; ++p) {
        const uint64_t pool_dst = pool_node_id_base_ + static_cast<uint64_t>(p);
        auto* clone = make_reset_util_event(kControlBroadcast, pool_dst);
        if (!pool_ports_[static_cast<std::size_t>(p)].port.send(clone)) {
            delete clone;
        }
    }
}

void Switch::finish()
{
    auto avg_util = [](const std::vector<PortState>& ports) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& port : ports) {
            sum += port.port.ingress_avg_utilization();
            count++;
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };
    auto avg_ingress_wait = [](const std::vector<PortState>& ports) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& port : ports) {
            sum += port.port.ingress_wait_avg_cycles();
            count++;
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };
    auto avg_egress_wait = [](const std::vector<PortState>& ports) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& port : ports) {
            sum += port.port.egress_wait_avg_cycles();
            count++;
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };
    auto avg_ingress_queue_wait = [](const std::vector<PortState>& ports) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& port : ports) {
            sum += port.port.ingress_queue_wait_avg_cycles();
            count++;
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };
    auto avg_ready_wait = [](const std::vector<PortState>& ports) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& port : ports) {
            sum += port.port.ready_wait_avg_cycles();
            count++;
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };
    auto avg_ready_occ = [](const std::vector<PortState>& ports) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& port : ports) {
            sum += port.port.ready_occupancy_avg();
            count++;
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };
    auto max_ingress_wait = [](const std::vector<PortState>& ports) {
        uint64_t max_wait = 0;
        for (const auto& port : ports) {
            max_wait = std::max(max_wait, port.port.ingress_wait_max_cycles());
        }
        return max_wait;
    };
    auto max_egress_wait = [](const std::vector<PortState>& ports) {
        uint64_t max_wait = 0;
        for (const auto& port : ports) {
            max_wait = std::max(max_wait, port.port.egress_wait_max_cycles());
        }
        return max_wait;
    };
    auto max_ingress_queue_wait = [](const std::vector<PortState>& ports) {
        uint64_t max_wait = 0;
        for (const auto& port : ports) {
            max_wait = std::max(max_wait, port.port.ingress_queue_wait_max_cycles());
        }
        return max_wait;
    };
    auto max_ready_wait = [](const std::vector<PortState>& ports) {
        uint64_t max_wait = 0;
        for (const auto& port : ports) {
            max_wait = std::max(max_wait, port.port.ready_wait_max_cycles());
        }
        return max_wait;
    };
    auto sum_ingress_occ = [](const std::vector<PortState>& ports) {
        std::size_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.ingress_occupancy();
        }
        return sum;
    };
    auto sum_ready_occ = [](const std::vector<PortState>& ports) {
        std::size_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.ready_occupancy();
        }
        return sum;
    };
    auto max_ready_occ = [](const std::vector<PortState>& ports) {
        std::size_t max_occ = 0;
        for (const auto& port : ports) {
            max_occ = std::max(max_occ, port.port.ready_occupancy_max());
        }
        return max_occ;
    };
    auto sum_ready_retries = [](const std::vector<PortState>& ports) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.ready_retry_count();
        }
        return sum;
    };
    auto max_ingress_arrival_burst_pkts = [](const std::vector<PortState>& ports) {
        uint64_t max_burst = 0;
        for (const auto& port : ports) {
            max_burst = std::max(max_burst, port.port.ingress_arrival_burst_max_pkts());
        }
        return max_burst;
    };
    auto max_ingress_arrival_burst_bytes = [](const std::vector<PortState>& ports) {
        uint64_t max_burst = 0;
        for (const auto& port : ports) {
            max_burst = std::max(max_burst, port.port.ingress_arrival_burst_max_bytes());
        }
        return max_burst;
    };
    auto max_ingress_release_burst_pkts = [](const std::vector<PortState>& ports) {
        uint64_t max_burst = 0;
        for (const auto& port : ports) {
            max_burst = std::max(max_burst, port.port.ingress_release_burst_max_pkts());
        }
        return max_burst;
    };
    auto max_ingress_release_burst_bytes = [](const std::vector<PortState>& ports) {
        uint64_t max_burst = 0;
        for (const auto& port : ports) {
            max_burst = std::max(max_burst, port.port.ingress_release_burst_max_bytes());
        }
        return max_burst;
    };
    auto avg_ingress_arrival_burst_pkts = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_burst_avg_pkts();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_arrival_burst_bytes = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_burst_avg_bytes();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_arrival_burst_stddev_pkts = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_burst_stddev_pkts();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_arrival_burst_stddev_bytes = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_burst_stddev_bytes();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_arrival_nonempty_frac = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_nonempty_frac();
        return sum / static_cast<double>(ports.size());
    };
    auto max_ingress_arrival_run_cycles = [](const std::vector<PortState>& ports) {
        uint64_t max_run = 0;
        for (const auto& port : ports) max_run = std::max(max_run, port.port.ingress_arrival_run_max_cycles());
        return max_run;
    };
    auto avg_ingress_arrival_run_cycles = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_run_avg_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_arrival_run_stddev = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_run_stddev_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto max_ingress_arrival_gap_cycles = [](const std::vector<PortState>& ports) {
        uint64_t max_gap = 0;
        for (const auto& port : ports) max_gap = std::max(max_gap, port.port.ingress_arrival_gap_max_cycles());
        return max_gap;
    };
    auto avg_ingress_arrival_gap_cycles = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_gap_avg_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_arrival_gap_stddev = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_arrival_gap_stddev_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_release_burst_pkts = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_burst_avg_pkts();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_release_burst_bytes = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_burst_avg_bytes();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_release_burst_stddev_pkts = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_burst_stddev_pkts();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_release_burst_stddev_bytes = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_burst_stddev_bytes();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_release_nonempty_frac = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_nonempty_frac();
        return sum / static_cast<double>(ports.size());
    };
    auto max_ingress_release_run_cycles = [](const std::vector<PortState>& ports) {
        uint64_t max_run = 0;
        for (const auto& port : ports) max_run = std::max(max_run, port.port.ingress_release_run_max_cycles());
        return max_run;
    };
    auto avg_ingress_release_run_cycles = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_run_avg_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_release_run_stddev = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_run_stddev_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto max_ingress_release_gap_cycles = [](const std::vector<PortState>& ports) {
        uint64_t max_gap = 0;
        for (const auto& port : ports) max_gap = std::max(max_gap, port.port.ingress_release_gap_max_cycles());
        return max_gap;
    };
    auto avg_ingress_release_gap_cycles = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_gap_avg_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_release_gap_stddev = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_release_gap_stddev_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_occ = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_occ_avg_bytes();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_occ_stddev = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_occ_stddev_bytes();
        return sum / static_cast<double>(ports.size());
    };
    auto max_ingress_occ_bytes = [](const std::vector<PortState>& ports) {
        uint64_t max_occ = 0;
        for (const auto& port : ports) max_occ = std::max(max_occ, port.port.ingress_occ_max_bytes());
        return max_occ;
    };
    auto avg_ingress_occ_nonempty_frac = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_occ_nonempty_frac();
        return sum / static_cast<double>(ports.size());
    };
    auto max_ingress_occ_run_cycles = [](const std::vector<PortState>& ports) {
        uint64_t max_run = 0;
        for (const auto& port : ports) max_run = std::max(max_run, port.port.ingress_occ_run_max_cycles());
        return max_run;
    };
    auto avg_ingress_occ_run_cycles = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_occ_run_avg_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_occ_run_stddev = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_occ_run_stddev_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto max_ingress_occ_gap_cycles = [](const std::vector<PortState>& ports) {
        uint64_t max_gap = 0;
        for (const auto& port : ports) max_gap = std::max(max_gap, port.port.ingress_occ_gap_max_cycles());
        return max_gap;
    };
    auto avg_ingress_occ_gap_cycles = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_occ_gap_avg_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto avg_ingress_occ_gap_stddev = [](const std::vector<PortState>& ports) {
        if (ports.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& port : ports) sum += port.port.ingress_occ_gap_stddev_cycles();
        return sum / static_cast<double>(ports.size());
    };
    auto sum_rx_bytes_class = [](const std::vector<PortState>& ports, FabricPort::TrafficClass cls) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.rx_bytes(cls);
        }
        return sum;
    };
    auto sum_tx_bytes_class = [](const std::vector<PortState>& ports, FabricPort::TrafficClass cls) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.tx_bytes(cls);
        }
        return sum;
    };
    auto sum_rx_pkts_class = [](const std::vector<PortState>& ports, FabricPort::TrafficClass cls) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.rx_packets(cls);
        }
        return sum;
    };
    auto sum_tx_pkts_class = [](const std::vector<PortState>& ports, FabricPort::TrafficClass cls) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.tx_packets(cls);
        }
        return sum;
    };
    auto sum_tx_bytes = [](const std::vector<PortState>& ports) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.tx_bytes_total();
        }
        return sum;
    };
    auto sum_rx_bytes = [](const std::vector<PortState>& ports) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.rx_bytes_total();
        }
        return sum;
    };
    const auto now = std::chrono::steady_clock::now();
    const auto sec = std::chrono::duration<double>(now - wall_start_).count();
    const auto stats_ticks_u = std::max<uint64_t>(tick_count_ - stats_start_tick_, 1);
    const double ticks = static_cast<double>(stats_ticks_u);
    const uint64_t host_to_switch_bytes = sum_rx_bytes(node_ports_);
    const uint64_t switch_to_host_bytes = sum_tx_bytes(node_ports_);
    const uint64_t host_link_total_bytes = host_to_switch_bytes + switch_to_host_bytes;
    const double host_to_switch_bpc = static_cast<double>(host_to_switch_bytes) / ticks;
    const double switch_to_host_bpc = static_cast<double>(switch_to_host_bytes) / ticks;
    const double host_link_total_bpc = static_cast<double>(host_link_total_bytes) / ticks;
    double clock_ghz = parse_clock_ghz(clock_frequency_);
    const double host_to_switch_gbps = host_to_switch_bpc * clock_ghz;
    const double switch_to_host_gbps = switch_to_host_bpc * clock_ghz;
    const double host_link_total_gbps = host_link_total_bpc * clock_ghz;
    if (lightweight_output_) {
        static constexpr std::array<std::pair<FabricPort::TrafficClass, const char*>, 4> kTrafficClasses{{
            {FabricPort::TrafficClass::DemandReq, "demand_req"},
            {FabricPort::TrafficClass::WriteReq, "write_req"},
            {FabricPort::TrafficClass::Response, "response"},
            {FabricPort::TrafficClass::OtherReq, "other_req"},
        }};
        std::cout << "stat.switch.replicated_messages = " << replicated_count_ << '\n';
        std::cout << "stat.switch.util.node_ingress_avg = " << avg_util(node_ports_) << '\n';
        std::cout << "stat.switch.util.pool_ingress_avg = " << avg_util(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_wait_avg_cycles = " << avg_ingress_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_egress_wait_avg_cycles = " << avg_egress_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_wait_avg_cycles = " << avg_ingress_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_egress_wait_avg_cycles = " << avg_egress_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_queue_wait_avg_cycles = " << avg_ingress_queue_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_queue_wait_avg_cycles = " << avg_ingress_queue_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ready_wait_avg_cycles = " << avg_ready_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ready_wait_avg_cycles = " << avg_ready_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_wait_max_cycles = " << max_ingress_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_egress_wait_max_cycles = " << max_egress_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_wait_max_cycles = " << max_ingress_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_egress_wait_max_cycles = " << max_egress_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_queue_wait_max_cycles = " << max_ingress_queue_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_queue_wait_max_cycles = " << max_ingress_queue_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ready_wait_max_cycles = " << max_ready_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ready_wait_max_cycles = " << max_ready_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_bytes = " << sum_ingress_occ(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_bytes = " << sum_ingress_occ(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ready_occ_avg_pkts = " << avg_ready_occ(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ready_occ_avg_pkts = " << avg_ready_occ(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ready_occ_pkts = " << sum_ready_occ(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ready_occ_pkts = " << sum_ready_occ(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ready_occ_max_pkts = " << max_ready_occ(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ready_occ_max_pkts = " << max_ready_occ(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ready_retry_count = " << sum_ready_retries(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ready_retry_count = " << sum_ready_retries(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_burst_max_pkts = " << max_ingress_arrival_burst_pkts(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_burst_max_bytes = " << max_ingress_arrival_burst_bytes(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_burst_avg_pkts = " << avg_ingress_arrival_burst_pkts(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_burst_avg_bytes = " << avg_ingress_arrival_burst_bytes(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_burst_stddev_pkts = " << avg_ingress_arrival_burst_stddev_pkts(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_burst_stddev_bytes = " << avg_ingress_arrival_burst_stddev_bytes(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_nonempty_frac = " << avg_ingress_arrival_nonempty_frac(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_run_max_cycles = " << max_ingress_arrival_run_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_run_avg_cycles = " << avg_ingress_arrival_run_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_run_stddev_cycles = " << avg_ingress_arrival_run_stddev(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_gap_max_cycles = " << max_ingress_arrival_gap_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_gap_avg_cycles = " << avg_ingress_arrival_gap_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_arrival_gap_stddev_cycles = " << avg_ingress_arrival_gap_stddev(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_burst_max_pkts = " << max_ingress_release_burst_pkts(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_burst_max_bytes = " << max_ingress_release_burst_bytes(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_burst_avg_pkts = " << avg_ingress_release_burst_pkts(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_burst_avg_bytes = " << avg_ingress_release_burst_bytes(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_burst_stddev_pkts = " << avg_ingress_release_burst_stddev_pkts(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_burst_stddev_bytes = " << avg_ingress_release_burst_stddev_bytes(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_nonempty_frac = " << avg_ingress_release_nonempty_frac(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_run_max_cycles = " << max_ingress_release_run_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_run_avg_cycles = " << avg_ingress_release_run_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_run_stddev_cycles = " << avg_ingress_release_run_stddev(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_gap_max_cycles = " << max_ingress_release_gap_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_gap_avg_cycles = " << avg_ingress_release_gap_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_release_gap_stddev_cycles = " << avg_ingress_release_gap_stddev(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_burst_max_pkts = " << max_ingress_arrival_burst_pkts(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_burst_max_bytes = " << max_ingress_arrival_burst_bytes(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_burst_avg_pkts = " << avg_ingress_arrival_burst_pkts(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_burst_avg_bytes = " << avg_ingress_arrival_burst_bytes(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_burst_stddev_pkts = " << avg_ingress_arrival_burst_stddev_pkts(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_burst_stddev_bytes = " << avg_ingress_arrival_burst_stddev_bytes(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_nonempty_frac = " << avg_ingress_arrival_nonempty_frac(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_run_max_cycles = " << max_ingress_arrival_run_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_run_avg_cycles = " << avg_ingress_arrival_run_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_run_stddev_cycles = " << avg_ingress_arrival_run_stddev(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_gap_max_cycles = " << max_ingress_arrival_gap_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_gap_avg_cycles = " << avg_ingress_arrival_gap_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_arrival_gap_stddev_cycles = " << avg_ingress_arrival_gap_stddev(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_burst_max_pkts = " << max_ingress_release_burst_pkts(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_burst_max_bytes = " << max_ingress_release_burst_bytes(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_burst_avg_pkts = " << avg_ingress_release_burst_pkts(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_burst_avg_bytes = " << avg_ingress_release_burst_bytes(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_burst_stddev_pkts = " << avg_ingress_release_burst_stddev_pkts(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_burst_stddev_bytes = " << avg_ingress_release_burst_stddev_bytes(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_nonempty_frac = " << avg_ingress_release_nonempty_frac(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_run_max_cycles = " << max_ingress_release_run_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_run_avg_cycles = " << avg_ingress_release_run_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_run_stddev_cycles = " << avg_ingress_release_run_stddev(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_gap_max_cycles = " << max_ingress_release_gap_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_gap_avg_cycles = " << avg_ingress_release_gap_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_release_gap_stddev_cycles = " << avg_ingress_release_gap_stddev(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_avg_bytes = " << avg_ingress_occ(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_stddev_bytes = " << avg_ingress_occ_stddev(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_max_bytes = " << max_ingress_occ_bytes(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_nonempty_frac = " << avg_ingress_occ_nonempty_frac(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_run_max_cycles = " << max_ingress_occ_run_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_run_avg_cycles = " << avg_ingress_occ_run_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_run_stddev_cycles = " << avg_ingress_occ_run_stddev(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_gap_max_cycles = " << max_ingress_occ_gap_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_gap_avg_cycles = " << avg_ingress_occ_gap_cycles(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_gap_stddev_cycles = " << avg_ingress_occ_gap_stddev(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_avg_bytes = " << avg_ingress_occ(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_stddev_bytes = " << avg_ingress_occ_stddev(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_max_bytes = " << max_ingress_occ_bytes(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_nonempty_frac = " << avg_ingress_occ_nonempty_frac(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_run_max_cycles = " << max_ingress_occ_run_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_run_avg_cycles = " << avg_ingress_occ_run_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_run_stddev_cycles = " << avg_ingress_occ_run_stddev(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_gap_max_cycles = " << max_ingress_occ_gap_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_gap_avg_cycles = " << avg_ingress_occ_gap_cycles(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_gap_stddev_cycles = " << avg_ingress_occ_gap_stddev(pool_ports_) << '\n';
        std::cout << "stat.switch.route.blocked_to_node = " << route_blocked_to_node_ << '\n';
        std::cout << "stat.switch.route.blocked_to_pool = " << route_blocked_to_pool_ << '\n';
        std::cout << "stat.switch.route.blocked_replicated_write = " << route_blocked_replicated_write_ << '\n';
        std::cout << "stat.switch.route.send_fail_to_node = " << route_send_fail_to_node_ << '\n';
        std::cout << "stat.switch.route.send_fail_to_pool = " << route_send_fail_to_pool_ << '\n';
        for (const auto& [cls, cls_name] : kTrafficClasses) {
            const auto idx = static_cast<std::size_t>(cls);
            std::cout << "stat.switch.route.attempt_to_node." << cls_name << " = " << route_attempt_to_node_by_class_[idx] << '\n';
            std::cout << "stat.switch.route.attempt_to_pool." << cls_name << " = " << route_attempt_to_pool_by_class_[idx] << '\n';
            std::cout << "stat.switch.route.blocked_to_node." << cls_name << " = " << route_blocked_to_node_by_class_[idx] << '\n';
            std::cout << "stat.switch.route.blocked_to_pool." << cls_name << " = " << route_blocked_to_pool_by_class_[idx] << '\n';
            std::cout << "stat.switch.route.send_fail_to_node." << cls_name << " = " << route_send_fail_to_node_by_class_[idx] << '\n';
            std::cout << "stat.switch.route.send_fail_to_pool." << cls_name << " = " << route_send_fail_to_pool_by_class_[idx] << '\n';
            std::cout << "stat.switch.route.replicated_clones." << cls_name << " = " << route_replicated_clones_by_class_[idx] << '\n';
            std::cout << "stat.switch.fabric.node_rx_bytes." << cls_name << " = " << sum_rx_bytes_class(node_ports_, cls) << '\n';
            std::cout << "stat.switch.fabric.node_tx_bytes." << cls_name << " = " << sum_tx_bytes_class(node_ports_, cls) << '\n';
            std::cout << "stat.switch.fabric.pool_rx_bytes." << cls_name << " = " << sum_rx_bytes_class(pool_ports_, cls) << '\n';
            std::cout << "stat.switch.fabric.pool_tx_bytes." << cls_name << " = " << sum_tx_bytes_class(pool_ports_, cls) << '\n';
            std::cout << "stat.switch.fabric.node_rx_pkts." << cls_name << " = " << sum_rx_pkts_class(node_ports_, cls) << '\n';
            std::cout << "stat.switch.fabric.node_tx_pkts." << cls_name << " = " << sum_tx_pkts_class(node_ports_, cls) << '\n';
            std::cout << "stat.switch.fabric.pool_rx_pkts." << cls_name << " = " << sum_rx_pkts_class(pool_ports_, cls) << '\n';
            std::cout << "stat.switch.fabric.pool_tx_pkts." << cls_name << " = " << sum_tx_pkts_class(pool_ports_, cls) << '\n';
        }
        std::cout << "stat.switch.bw.host_to_switch_gbps = " << host_to_switch_gbps << '\n';
        std::cout << "stat.switch.bw.switch_to_host_gbps = " << switch_to_host_gbps << '\n';
        std::cout << "stat.switch.bw.host_link_total_gbps = " << host_link_total_gbps << '\n';
        auto print_port_stats = [](const std::string& prefix, const FabricPort& port) {
            static constexpr std::array<std::pair<FabricPort::TrafficClass, const char*>, 4> kPortClasses{{
                {FabricPort::TrafficClass::DemandReq, "demand_req"},
                {FabricPort::TrafficClass::WriteReq, "write_req"},
                {FabricPort::TrafficClass::Response, "response"},
                {FabricPort::TrafficClass::OtherReq, "other_req"},
            }};
            std::cout << prefix << "ingress_wait_avg_cycles = " << port.ingress_wait_avg_cycles() << '\n';
            std::cout << prefix << "ingress_wait_max_cycles = " << port.ingress_wait_max_cycles() << '\n';
            std::cout << prefix << "egress_wait_avg_cycles = " << port.egress_wait_avg_cycles() << '\n';
            std::cout << prefix << "egress_wait_max_cycles = " << port.egress_wait_max_cycles() << '\n';
            std::cout << prefix << "ingress_queue_wait_avg_cycles = " << port.ingress_queue_wait_avg_cycles() << '\n';
            std::cout << prefix << "ingress_queue_wait_max_cycles = " << port.ingress_queue_wait_max_cycles() << '\n';
            std::cout << prefix << "egress_occ_avg_bytes = " << port.egress_occ_avg_bytes() << '\n';
            std::cout << prefix << "egress_occ_stddev_bytes = " << port.egress_occ_stddev_bytes() << '\n';
            std::cout << prefix << "egress_occ_max_bytes = " << port.egress_occ_max_bytes() << '\n';
            std::cout << prefix << "egress_occ_nonempty_frac = " << port.egress_occ_nonempty_frac() << '\n';
            std::cout << prefix << "egress_blocked_cycles = " << port.egress_blocked_cycles() << '\n';
            std::cout << prefix << "egress_blocked_nonempty_frac = " << port.egress_blocked_nonempty_frac() << '\n';
            std::cout << prefix << "egress_blocked_avg_occ_bytes = " << port.egress_blocked_avg_occ_bytes() << '\n';
            std::cout << prefix << "egress_blocked_max_occ_bytes = " << port.egress_blocked_max_occ_bytes() << '\n';
            std::cout << prefix << "egress_send_burst_max_pkts = " << port.egress_send_burst_max_pkts() << '\n';
            std::cout << prefix << "egress_send_burst_max_bytes = " << port.egress_send_burst_max_bytes() << '\n';
            std::cout << prefix << "egress_send_burst_avg_pkts = " << port.egress_send_burst_avg_pkts() << '\n';
            std::cout << prefix << "egress_send_burst_avg_bytes = " << port.egress_send_burst_avg_bytes() << '\n';
            std::cout << prefix << "egress_send_burst_stddev_pkts = " << port.egress_send_burst_stddev_pkts() << '\n';
            std::cout << prefix << "egress_send_burst_stddev_bytes = " << port.egress_send_burst_stddev_bytes() << '\n';
            std::cout << prefix << "egress_send_nonempty_frac = " << port.egress_send_nonempty_frac() << '\n';
            std::cout << prefix << "ingress_arrival_burst_avg_bytes = " << port.ingress_arrival_burst_avg_bytes() << '\n';
            std::cout << prefix << "ingress_arrival_nonempty_frac = " << port.ingress_arrival_nonempty_frac() << '\n';
            std::cout << prefix << "ingress_arrival_run_max_cycles = " << port.ingress_arrival_run_max_cycles() << '\n';
            std::cout << prefix << "ingress_arrival_run_avg_cycles = " << port.ingress_arrival_run_avg_cycles() << '\n';
            std::cout << prefix << "ingress_arrival_gap_max_cycles = " << port.ingress_arrival_gap_max_cycles() << '\n';
            std::cout << prefix << "ingress_arrival_gap_avg_cycles = " << port.ingress_arrival_gap_avg_cycles() << '\n';
            std::cout << prefix << "ingress_release_burst_avg_bytes = " << port.ingress_release_burst_avg_bytes() << '\n';
            std::cout << prefix << "ingress_release_nonempty_frac = " << port.ingress_release_nonempty_frac() << '\n';
            std::cout << prefix << "ingress_release_run_max_cycles = " << port.ingress_release_run_max_cycles() << '\n';
            std::cout << prefix << "ingress_release_run_avg_cycles = " << port.ingress_release_run_avg_cycles() << '\n';
            std::cout << prefix << "ingress_release_gap_max_cycles = " << port.ingress_release_gap_max_cycles() << '\n';
            std::cout << prefix << "ingress_release_gap_avg_cycles = " << port.ingress_release_gap_avg_cycles() << '\n';
            std::cout << prefix << "ingress_occ_avg_bytes = " << port.ingress_occ_avg_bytes() << '\n';
            std::cout << prefix << "ingress_occ_stddev_bytes = " << port.ingress_occ_stddev_bytes() << '\n';
            std::cout << prefix << "ingress_occ_max_bytes = " << port.ingress_occ_max_bytes() << '\n';
            std::cout << prefix << "ingress_occ_nonempty_frac = " << port.ingress_occ_nonempty_frac() << '\n';
            std::cout << prefix << "ingress_occ_run_max_cycles = " << port.ingress_occ_run_max_cycles() << '\n';
            std::cout << prefix << "ingress_occ_run_avg_cycles = " << port.ingress_occ_run_avg_cycles() << '\n';
            std::cout << prefix << "ingress_occ_gap_max_cycles = " << port.ingress_occ_gap_max_cycles() << '\n';
            std::cout << prefix << "ingress_occ_gap_avg_cycles = " << port.ingress_occ_gap_avg_cycles() << '\n';
            for (const auto& [cls, cls_name] : kPortClasses) {
                std::cout << prefix << "ingress_wait_avg_cycles." << cls_name << " = " << port.ingress_wait_avg_cycles(cls) << '\n';
                std::cout << prefix << "ingress_wait_max_cycles." << cls_name << " = " << port.ingress_wait_max_cycles(cls) << '\n';
                std::cout << prefix << "ingress_queue_wait_avg_cycles." << cls_name << " = " << port.ingress_queue_wait_avg_cycles(cls) << '\n';
                std::cout << prefix << "ingress_queue_wait_max_cycles." << cls_name << " = " << port.ingress_queue_wait_max_cycles(cls) << '\n';
                std::cout << prefix << "egress_wait_avg_cycles." << cls_name << " = " << port.egress_wait_avg_cycles(cls) << '\n';
                std::cout << prefix << "egress_wait_max_cycles." << cls_name << " = " << port.egress_wait_max_cycles(cls) << '\n';
                std::cout << prefix << "egress_occ_avg_bytes." << cls_name << " = " << port.egress_occ_avg_bytes(cls) << '\n';
                std::cout << prefix << "egress_occ_max_bytes." << cls_name << " = " << port.egress_occ_max_bytes(cls) << '\n';
                std::cout << prefix << "egress_blocked_cycles." << cls_name << " = " << port.egress_blocked_cycles(cls) << '\n';
                std::cout << prefix << "ingress_arrival_empty_packets." << cls_name << " = " << port.ingress_arrival_empty_packets(cls) << '\n';
                std::cout << prefix << "ingress_arrival_nonempty_packets." << cls_name << " = " << port.ingress_arrival_nonempty_packets(cls) << '\n';
                std::cout << prefix << "ingress_arrival_nonempty_packet_frac." << cls_name << " = " << port.ingress_arrival_nonempty_packet_frac(cls) << '\n';
                std::cout << prefix << "ingress_nonempty_arrival_pre_occ_avg_bytes." << cls_name << " = " << port.ingress_nonempty_arrival_pre_occ_avg_bytes(cls) << '\n';
                std::cout << prefix << "ingress_nonempty_arrival_pre_occ_max_bytes." << cls_name << " = " << port.ingress_nonempty_arrival_pre_occ_max_bytes(cls) << '\n';
                std::cout << prefix << "ingress_release_after_empty_arrival_packets." << cls_name << " = " << port.ingress_release_after_empty_arrival_packets(cls) << '\n';
                std::cout << prefix << "ingress_release_after_nonempty_arrival_packets." << cls_name << " = " << port.ingress_release_after_nonempty_arrival_packets(cls) << '\n';
                std::cout << prefix << "ingress_wait_after_empty_arrival_avg_cycles." << cls_name << " = " << port.ingress_wait_after_empty_arrival_avg_cycles(cls) << '\n';
                std::cout << prefix << "ingress_wait_after_nonempty_arrival_avg_cycles." << cls_name << " = " << port.ingress_wait_after_nonempty_arrival_avg_cycles(cls) << '\n';
                std::cout << prefix << "ingress_queue_wait_after_empty_arrival_avg_cycles." << cls_name << " = " << port.ingress_queue_wait_after_empty_arrival_avg_cycles(cls) << '\n';
                std::cout << prefix << "ingress_queue_wait_after_nonempty_arrival_avg_cycles." << cls_name << " = " << port.ingress_queue_wait_after_nonempty_arrival_avg_cycles(cls) << '\n';
                std::cout << prefix << "ingress_queue_wait_after_empty_arrival_max_cycles." << cls_name << " = " << port.ingress_queue_wait_after_empty_arrival_max_cycles(cls) << '\n';
                std::cout << prefix << "ingress_queue_wait_after_nonempty_arrival_max_cycles." << cls_name << " = " << port.ingress_queue_wait_after_nonempty_arrival_max_cycles(cls) << '\n';
                std::cout << prefix << "rx_bytes." << cls_name << " = " << port.rx_bytes(cls) << '\n';
                std::cout << prefix << "tx_bytes." << cls_name << " = " << port.tx_bytes(cls) << '\n';
                std::cout << prefix << "rx_pkts." << cls_name << " = " << port.rx_packets(cls) << '\n';
                std::cout << prefix << "tx_pkts." << cls_name << " = " << port.tx_packets(cls) << '\n';
            }
            port.emit_deep_diagnostics(std::cout, prefix);
        };
        for (std::size_t i = 0; i < node_ports_.size(); ++i) {
            print_port_stats("stat.switch.port.node." + std::to_string(i) + ".", node_ports_[i].port);
        }
        for (std::size_t i = 0; i < pool_ports_.size(); ++i) {
            print_port_stats("stat.switch.port.pool." + std::to_string(i) + ".", pool_ports_[i].port);
        }
        std::cout << "stat.switch.walltime_s = " << sec << '\n';
        if (active_calls_ > 0) {
            const auto active_sec = std::chrono::duration<double>(active_time_).count();
            std::cout << "stat.switch.active_time_s = " << active_sec << '\n';
        }
    } else {
        std::cout << "Switch replicated messages: " << replicated_count_ << std::endl;
        std::cout << "Switch avg util node ingress: " << avg_util(node_ports_) << std::endl;
        std::cout << "Switch avg util pool ingress: " << avg_util(pool_ports_) << std::endl;
        std::cout << "Switch host->switch BW (GB/s): " << host_to_switch_gbps << std::endl;
        std::cout << "Switch switch->host BW (GB/s): " << switch_to_host_gbps << std::endl;
        std::cout << "Switch host-link total BW (GB/s): " << host_link_total_gbps << std::endl;
        std::cout << "Switch wall time (s): " << sec << std::endl;
        if (active_calls_ > 0) {
            const auto active_sec = std::chrono::duration<double>(active_time_).count();
            std::cout << "Component Time Summary\n";
            std::cout << "  Switch active time (s): " << active_sec << std::endl;
        }
    }
}

} // namespace csimCore
} // namespace SST
