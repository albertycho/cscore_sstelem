#include "address_map.h"

#include <fstream>
#include <sstream>

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
    entries_.clear();
    std::ifstream f(path);
    if (!f.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
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
        e.start = std::stoull(start_s, nullptr, 0);
        e.size = std::stoull(size_s, nullptr, 0);
        e.type = parse_type(type_s);
        e.target = static_cast<uint32_t>(std::stoul(target_s, nullptr, 0));
        entries_.push_back(e);
    }
    return true;
}

std::optional<AddressEntry> AddressMap::lookup(uint32_t node_id, uint64_t addr) const
{
    for (const auto& e : entries_) {
        if (e.matches(node_id, addr)) {
            return e;
        }
    }
    return std::nullopt;
}

} // namespace csimCore
} // namespace SST
