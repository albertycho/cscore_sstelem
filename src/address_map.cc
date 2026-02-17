#include "address_map.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "champsim.h"
namespace SST {
namespace csimCore {

namespace {
AddressType parse_type(const std::string& s) {
    if (s == "local" || s == "LOCAL") return AddressType::Local;
    if (s == "pool" || s == "POOL") return AddressType::Pool;
    return AddressType::Remote;
}
}

bool AddressMap::load(const std::string& path)
{
    entries_by_node_.clear();
    std::ifstream f(path);
    if (!f.is_open()) {
        return false;
    }

    std::string line;
    std::size_t line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') continue;
        std::stringstream ss(line);
        std::string node_s, start_s, size_s, type_s, target_s;
        if (!std::getline(ss, node_s, ',')) continue;
        if (!std::getline(ss, start_s, ',')) continue;
        if (!std::getline(ss, size_s, ',')) continue;
        if (!std::getline(ss, type_s, ',')) continue;
        if (!std::getline(ss, target_s, ',')) target_s = "0";

        AddressEntry e;
        e.node_id = static_cast<uint32_t>(std::stoul(node_s, nullptr, 0));
        const uint64_t raw_start = std::stoull(start_s, nullptr, 0);
        const uint64_t raw_size = std::stoull(size_s, nullptr, 0);
        if ((raw_start % PAGE_SIZE) != 0 || (raw_size % PAGE_SIZE) != 0) {
            std::ostringstream msg;
            msg << "address_map: start/size must be page-aligned at line " << line_no
                << " (start=" << start_s << ", size=" << size_s << ") : " << line;
            throw std::runtime_error(msg.str());
        }
        e.start = raw_start;
        e.size = raw_size;
        e.type = parse_type(type_s);
        e.target = static_cast<uint32_t>(std::stoul(target_s, nullptr, 0));
        if (e.node_id >= entries_by_node_.size()) {
            entries_by_node_.resize(e.node_id + 1);
        }
        entries_by_node_[e.node_id].push_back(e);
    }

    for (auto& vec : entries_by_node_) {
        std::sort(vec.begin(), vec.end(), [](const AddressEntry& a, const AddressEntry& b) {
            return a.start < b.start;
        });
        uint64_t pool_running = 0;
        for (auto& entry : vec) {
            if (entry.type == AddressType::Pool) {
                entry.pool_offset = pool_running;
                pool_running += entry.size;
            }
        }
    }
    return true;
}

std::optional<AddressEntry> AddressMap::lookup(uint32_t node_id, uint64_t addr) const
{
    if (node_id >= entries_by_node_.size() || entries_by_node_[node_id].empty()) {
        return std::nullopt;
    }

    const auto& vec = entries_by_node_[node_id];
    auto ub = std::upper_bound(vec.begin(), vec.end(), addr,
                               [](uint64_t value, const AddressEntry& e) { return value < e.start; });
    if (ub == vec.begin()) {
        return std::nullopt;
    }
    const auto& candidate = *std::prev(ub);
    if (candidate.contains(addr)) {
        return candidate;
    }
    return std::nullopt;
}

} // namespace csimCore
} // namespace SST
