#include "cxl_request_buffer.h"
#include <iostream>
#include <cassert>

namespace SST {
namespace csimCore {

bool CXLRequestBuffer::try_enqueue(const sst_request& req, uint64_t enqueue_cycle)
{
    assert(in_range(req.address) && "Enqueing address not in CXL range into CXL request buffer");

    if (limit_ != 0 && queue_.size() >= limit_) {
        return false;
    }

    queue_.push_back(Entry{req, enqueue_cycle});
    return true;
}

sst_request CXLRequestBuffer::pop_front()
{
    auto entry = queue_.front();
    queue_.pop_front();
    return entry.req;
}

bool CXLRequestBuffer::in_range(uint64_t address) const
{
    if (!has_range_) {
        return false;
    }

    return range_.contains(address);
}

} // namespace csimCore
} // namespace SST
