#pragma once

#include <chrono>
#include <cstdint>

namespace SST {
namespace csimCore {

class ScopedTimer {
public:
    ScopedTimer(std::chrono::steady_clock::duration& accum, uint64_t& calls)
        : accum_(&accum), calls_(&calls), start_(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() {
        *accum_ += std::chrono::steady_clock::now() - start_;
        ++(*calls_);
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::chrono::steady_clock::duration* accum_;
    uint64_t* calls_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace csimCore
} // namespace SST
