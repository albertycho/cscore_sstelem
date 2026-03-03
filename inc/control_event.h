#pragma once

#include <cstdint>
#include <limits>

#include "csEvent.h"

namespace SST {
namespace csimCore {

constexpr uint64_t kControlBroadcast = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kControlResetUtil = 1;

inline csEvent* make_control_event(uint64_t src, uint64_t dst, uint64_t code) {
    auto* ev = new csEvent();
    ev->payload.reserve(3);
    ev->payload.push_back(src);
    ev->payload.push_back(dst);
    ev->payload.push_back(code);
    return ev;
}

inline bool is_control_event(const csEvent& ev, uint64_t* code_out = nullptr) {
    if (!(ev.payload.size() == 3 || ev.payload.size() == 4)) {
        return false;
    }
    if (code_out) {
        *code_out = ev.payload[2];
    }
    return true;
}

} // namespace csimCore
} // namespace SST
