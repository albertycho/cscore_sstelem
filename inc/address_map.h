#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SST {
namespace csimCore {

enum class AddressType { Local, Remote, Pool };

struct AddressEntry {
    uint32_t node_id = 0;
    uint64_t start = 0;
    uint64_t size = 0;
    AddressType type = AddressType::Local;
    uint32_t target = 0; // socket id for Remote, ignored for Local, pool id for Pool if needed
    uint64_t pool_offset = 0; // offset within the pool PA space for this range

    bool contains(uint64_t addr) const { return addr >= start && addr < start + size; }
    bool matches(uint32_t node_value, uint64_t addr) const { return node_value == node_id && contains(addr); }
};

class AddressMap {
public:
    // Ranges are interpreted in the same address space used for routing (currently VA for pool/remote).
    // The node_id field is matched against the per-node id used in the simulation.
    bool load(const std::string& path);

    std::optional<AddressEntry> lookup(uint32_t node_id, uint64_t addr) const;

private:
    std::vector<std::vector<AddressEntry>> entries_by_node_;
};

} // namespace csimCore
} // namespace SST
