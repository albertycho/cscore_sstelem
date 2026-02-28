#pragma once

#include <cstdint>
#include <limits>

#include "csEvent.h"

namespace SST {
namespace csimCore {

constexpr uint64_t kControlResetUtil = 1;
constexpr uint64_t kControlBroadcast = std::numeric_limits<uint64_t>::max();

inline bool is_control_event(const csEvent& ev, uint64_t* code = nullptr) {
    if (ev.payload.size() != 3) {
        return false;
    }
    if (code) {
        *code = ev.payload[2];
    }
    return true;
}

inline csEvent* make_control_event(uint64_t src, uint64_t dst, uint64_t code) {
    auto* ev = new csEvent();
    ev->payload.reserve(3);
    ev->payload.push_back(src);
    ev->payload.push_back(dst);
    ev->payload.push_back(code);
    return ev;
}

} // namespace csimCore
} // namespace SST

