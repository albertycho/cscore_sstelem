#include "switch.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "access_type.h"

namespace SST {
namespace csimCore {

namespace {
constexpr uint64_t kControlCredit = 2;

bool is_write_request(const csEvent& ev) {
    if (ev.payload.size() > 11) {
        const auto type = static_cast<access_type>(ev.payload[11]);
        return type == access_type::WRITE;
    }
    if (ev.payload.size() <= 10) {
        return false;
    }
    const auto type = static_cast<access_type>(ev.payload[10]);
    return type == access_type::WRITE;
}

bool is_credit_event(const csEvent& ev) {
    return ev.payload.size() == 4 && ev.payload[2] == kControlCredit;
}

csEvent* clone_event_with_dst(const csEvent& ev, uint64_t dst) {
    auto* out = new csEvent();
    out->payload = ev.payload;
    out->remote_timing = ev.remote_timing;
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
    const int64_t legacy_link_queue_size = params.find<int64_t>("link_queue_size", 0);
    link_egress_buffer_size_ = params.find<int64_t>("link_egress_buffer_size", legacy_link_queue_size);
    link_credit_window_size_ = params.find<int64_t>("link_credit_window_size", legacy_link_queue_size);
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
        node_ports_[i].configure(link,
                                 static_cast<uint64_t>(i),
                                 link_bw_cycles_,
                                 link_latency_cycles_,
                                 link_egress_buffer_size_,
                                 link_credit_window_size_);
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
        pool_ports_[p].configure(link,
                                 port_id,
                                 link_bw_cycles_,
                                 link_latency_cycles_,
                                 link_egress_buffer_size_,
                                 link_credit_window_size_);
    }

    registerClock(clock_frequency_, new Clock::Handler<Switch>(this, &Switch::clock_tick));
}

void Switch::setup() {
    wall_start_ = std::chrono::steady_clock::now();
    active_time_ = std::chrono::steady_clock::duration{};
    active_calls_ = 0;
    tick_count_ = 0;
    last_cycle_ = 0;
    rr_pool_idx_ = 0;
    rr_node_input_idx_ = 0;
    rr_pool_input_idx_ = 0;
}

bool Switch::clock_tick(SST::Cycle_t cycle)
{
    ScopedTimer timer(active_time_, active_calls_);
    ++tick_count_;
    const auto cycle_u = static_cast<uint64_t>(cycle);
    last_cycle_ = cycle_u;
    for (auto& port : node_ports_) {
        port.advance(cycle_u);
    }
    for (auto& port : pool_ports_) {
        port.advance(cycle_u);
    }
    service_next_input(node_ports_, rr_node_input_idx_, cycle_u);
    service_next_input(pool_ports_, rr_pool_input_idx_, cycle_u);
    return false;
}

bool Switch::service_next_input(std::vector<FabricPort>& ingress_ports,
                                std::size_t& rr_ingress_idx,
                                uint64_t cycle)
{
    if (ingress_ports.empty()) {
        return false;
    }

    const std::size_t total = ingress_ports.size();
    for (std::size_t offset = 0; offset < total; ++offset) {
        const std::size_t idx = (rr_ingress_idx + offset) % total;
        bool saw_ready = false;
        bool granted = false;
        ingress_ports[idx].try_receive_ready(cycle, [this, idx, total, &rr_ingress_idx, &saw_ready, &granted](csEvent* ev) {
            saw_ready = true;
            // Arbitration is on the first ready head, not the first admissible
            // head. If that ingress is blocked downstream, hold the turn and
            // retry from the same ingress next cycle.
            if (!try_route_event(ev)) {
                return false;
            }
            rr_ingress_idx = (idx + 1) % total;
            granted = true;
            return true;
        });
        if (saw_ready) {
            return granted;
        }
    }

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
    FabricPort* port = nullptr;
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

    port->handle_event(cevent);

}

bool Switch::try_route_event(csEvent* ev)
{
    if (!ev) {
        throw std::runtime_error("Switch: null event in try_route_event.");
    }
    if (ev->payload.size() < 2) {
        throw std::runtime_error("Switch: received malformed csEvent (payload size < 2).");
    }
    const uint64_t dst = ev->payload[1];
    if (dst < static_cast<uint64_t>(num_nodes_)) {
        const auto idx = static_cast<size_t>(dst);
        if (idx >= node_ports_.size()) {
            throw std::runtime_error("Switch: dst node id out of range for configured ports.");
        }
        return node_ports_[idx].send(ev);
    }

    if (dst >= pool_node_id_base_) {
        if (is_credit_event(*ev)) {
            const auto idx = static_cast<size_t>(dst - pool_node_id_base_);
            if (idx >= pool_ports_.size()) {
                throw std::runtime_error("Switch: credit dst pool id out of range for configured ports.");
            }
            return pool_ports_[idx].send(ev);
        }
        if (replicate_writes_ && is_write_request(*ev)) {
            if (!can_replicate_to_all_pools(ev)) {
                return false;
            }
            for (int p = 0; p < num_pools_; ++p) {
                const uint64_t pool_dst = pool_node_id_base_ + static_cast<uint64_t>(p);
                auto* clone = clone_event_with_dst(*ev, pool_dst);
                replicated_count_++;
                if (!pool_ports_[static_cast<size_t>(p)].send(clone)) {
                    delete clone;
                    throw std::runtime_error("Switch: replicated write preflight passed but send failed.");
                }
            }
            delete ev;
            return true;
        }
        return try_send_to_any_pool(ev);
    }

    const std::string msg =
        "Switch: dst id " + std::to_string(dst) +
        " is not in node range [0," + std::to_string(num_nodes_ - 1) +
        "] or pool range [" + std::to_string(pool_node_id_base_) + "," +
        std::to_string(pool_node_id_base_ + static_cast<uint64_t>(num_pools_ - 1)) + "].";
    delete ev;
    throw std::runtime_error(msg);
}

bool Switch::can_replicate_to_all_pools(const csEvent* ev) const
{
    for (const auto& pool : pool_ports_) {
        if (!pool.can_enqueue(ev)) {
            return false;
        }
    }
    return true;
}

bool Switch::try_send_to_any_pool(csEvent* ev)
{
    if (pool_ports_.empty()) {
        return false;
    }

    const uint64_t original_dst = ev->payload[1];
    if (pool_select_policy_ == PoolSelectPolicy::Fixed0) {
        ev->payload[1] = pool_node_id_base_;
        if (!pool_ports_[0].send(ev)) {
            ev->payload[1] = original_dst;
            return false;
        }
        return true;
    }

    const std::size_t total = pool_ports_.size();
    for (std::size_t offset = 0; offset < total; ++offset) {
        const std::size_t idx = (rr_pool_idx_ + offset) % total;
        ev->payload[1] = pool_node_id_base_ + idx;
        if (!pool_ports_[idx].send(ev)) {
            continue;
        }
        rr_pool_idx_ = (idx + 1) % total;
        return true;
    }

    ev->payload[1] = original_dst;
    return false;
}

void Switch::finish()
{
    auto avg_util = [](const std::vector<FabricPort>& ports) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto& port : ports) {
            sum += port.ingress_avg_utilization();
            count++;
        }
        return count > 0 ? sum / static_cast<double>(count) : 0.0;
    };
    auto sum_tx_bytes = [](const std::vector<FabricPort>& ports) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.tx_bytes_total();
        }
        return sum;
    };
    auto sum_rx_bytes = [](const std::vector<FabricPort>& ports) {
        uint64_t sum = 0;
        for (const auto& port : ports) {
            sum += port.rx_bytes_total();
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
    const double host_to_switch_gbps = host_to_switch_bpc * clock_ghz * 8.0;
    const double switch_to_host_gbps = switch_to_host_bpc * clock_ghz * 8.0;
    const double host_link_total_gbps = host_link_total_bpc * clock_ghz * 8.0;
    if (lightweight_output_) {
        std::cout << "stat.switch.replicated_messages = " << replicated_count_ << '\n';
        std::cout << "stat.switch.util.node_ingress_avg = " << avg_util(node_ports_) << '\n';
        std::cout << "stat.switch.util.pool_ingress_avg = " << avg_util(pool_ports_) << '\n';
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
        std::cout << "Switch host->switch BW (Gbps): " << host_to_switch_gbps << std::endl;
        std::cout << "Switch switch->host BW (Gbps): " << switch_to_host_gbps << std::endl;
        std::cout << "Switch host-link total BW (Gbps): " << host_link_total_gbps << std::endl;
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
