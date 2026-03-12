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
}

bool Switch::clock_tick(SST::Cycle_t cycle)
{
    ScopedTimer timer(active_time_, active_calls_);
    ++tick_count_;
    const auto cycle_u = static_cast<uint64_t>(cycle);
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
    if (dst < static_cast<uint64_t>(num_nodes_)) {
        const auto idx = static_cast<size_t>(dst);
        if (idx >= node_ports_.size()) {
            throw std::runtime_error("Switch: dst node id out of range for configured ports.");
        }
        if (!node_ports_[idx].port.can_send(ev)) {
            return false;
        }
        if (!node_ports_[idx].port.send(ev)) {
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
                    return false;
                }
            }
            for (int p = 0; p < num_pools_; ++p) {
                const uint64_t pool_dst = pool_node_id_base_ + static_cast<uint64_t>(p);
                auto* clone = clone_event_with_dst(*ev, pool_dst);
                replicated_count_++;
                if (!pool_ports_[static_cast<size_t>(p)].port.send(clone)) {
                    delete clone;
                }
            }
            delete ev;
            return true;
        }
        std::size_t pick = pick_pool_index(true, ev);
        if (pick >= pool_ports_.size()) {
            return false;
        }
        if (!pool_ports_[pick].port.can_send(ev)) {
            return false;
        }
        ev->payload[1] = pool_node_id_base_ + pick;
        if (!pool_ports_[pick].port.send(ev)) {
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
    for_each_port([](PortState& port) { port.port.reset_ingress_utilization(); });
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
    auto sum_ingress_occ = [](const std::vector<PortState>& ports) {
        std::size_t sum = 0;
        for (const auto& port : ports) {
            sum += port.port.ingress_occupancy();
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
    const double ticks = tick_count_ > 0 ? static_cast<double>(tick_count_) : 1.0;
    const uint64_t host_to_switch_bytes = sum_rx_bytes(node_ports_);
    const uint64_t switch_to_host_bytes = sum_tx_bytes(node_ports_);
    const uint64_t host_link_total_bytes = host_to_switch_bytes + switch_to_host_bytes;
    const double host_to_switch_bpc = static_cast<double>(host_to_switch_bytes) / ticks;
    const double switch_to_host_bpc = static_cast<double>(switch_to_host_bytes) / ticks;
    const double host_link_total_bpc = static_cast<double>(host_link_total_bytes) / ticks;
    double clock_ghz = parse_clock_ghz(clock_frequency_);
    if (clock_ghz <= 0.0) {
        clock_ghz = 2.4; // fallback to default switch clock
    }
    const double host_to_switch_gbps = host_to_switch_bpc * clock_ghz;
    const double switch_to_host_gbps = switch_to_host_bpc * clock_ghz;
    const double host_link_total_gbps = host_link_total_bpc * clock_ghz;
    if (lightweight_output_) {
        std::cout << "stat.switch.replicated_messages = " << replicated_count_ << '\n';
        std::cout << "stat.switch.util.node_ingress_avg = " << avg_util(node_ports_) << '\n';
        std::cout << "stat.switch.util.pool_ingress_avg = " << avg_util(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_wait_avg_cycles = " << avg_ingress_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_egress_wait_avg_cycles = " << avg_egress_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_wait_avg_cycles = " << avg_ingress_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_egress_wait_avg_cycles = " << avg_egress_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_queue_wait_avg_cycles = " << avg_ingress_queue_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_queue_wait_avg_cycles = " << avg_ingress_queue_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_wait_max_cycles = " << max_ingress_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_egress_wait_max_cycles = " << max_egress_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_wait_max_cycles = " << max_ingress_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_egress_wait_max_cycles = " << max_egress_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_queue_wait_max_cycles = " << max_ingress_queue_wait(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_queue_wait_max_cycles = " << max_ingress_queue_wait(pool_ports_) << '\n';
        std::cout << "stat.switch.fabric.node_ingress_occ_bytes = " << sum_ingress_occ(node_ports_) << '\n';
        std::cout << "stat.switch.fabric.pool_ingress_occ_bytes = " << sum_ingress_occ(pool_ports_) << '\n';
        std::cout << "stat.switch.bw.host_to_switch_gbps = " << host_to_switch_gbps << '\n';
        std::cout << "stat.switch.bw.switch_to_host_gbps = " << switch_to_host_gbps << '\n';
        std::cout << "stat.switch.bw.host_link_total_gbps = " << host_link_total_gbps << '\n';
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
