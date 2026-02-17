#ifndef CSCORE_CXL_REQUEST_BUFFER_H
#define CSCORE_CXL_REQUEST_BUFFER_H

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "SST_CS_packets.h"

namespace SST {
namespace csimCore {

struct CXLRange {
    int pid = -1;
    uint64_t start = 0;
    uint64_t size = 0;

    bool contains(uint64_t address) const { return address >= start && address < start + size; }
};

using CXLAddressMap = std::vector<CXLRange>;

class CXLRequestBuffer {
public:
    void set_range(const CXLRange& range) { range_ = range; has_range_ = true; }
    void set_limit(std::size_t limit) { limit_ = limit; }

  bool try_enqueue(const sst_request& req, uint64_t enqueue_cycle);
  bool is_cxl_address(uint64_t address) const { return in_range(address); }
  bool has_pending() const { return !queue_.empty(); }
  sst_request pop_front();

private:
    struct Entry {
        sst_request req;
        uint64_t enqueue_cycle = 0;
    };

    bool in_range(uint64_t address) const;

    CXLRange range_{};
    bool has_range_ = false;
    std::size_t limit_ = 0;
    std::deque<Entry> queue_;
};

} // namespace csimCore
} // namespace SST

#endif // CSCORE_CXL_REQUEST_BUFFER_H
