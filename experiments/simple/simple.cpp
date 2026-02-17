#include <cstdio>
#include <cstdlib>
#include <cstdint>

constexpr size_t N = 1 << 20; // 1M elements (~8 MiB)
constexpr uint64_t MASK = N - 1;
constexpr size_t ALIGN = 4096;
constexpr size_t EXTRA = ALIGN / sizeof(uint64_t);
uint64_t cxl_buf_raw[N + EXTRA];
uint64_t norm_buf_raw[N + EXTRA];
int main() {
    // Initialize buffers so loads are not optimized away.
    auto align_ptr = [](uint64_t* p) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        addr = (addr + (ALIGN - 1)) & ~(ALIGN - 1);
        return reinterpret_cast<uint64_t*>(addr);
    };

    uint64_t* cxl_buf = align_ptr(cxl_buf_raw);
    uint64_t* norm_buf = align_ptr(norm_buf_raw);

    for (size_t i = 0; i < N; i += 64) {
        cxl_buf[i] = i;
        norm_buf[i] = i + 1;
    }

    volatile uint64_t* cxl = cxl_buf;
    volatile uint64_t* norm = norm_buf;
    uint64_t state = 0x123456789abcdef0ULL;
    volatile uint64_t sink = 0;
    for (size_t i = 0; i < 10'000'000; i++) {
        state = state * 6364136223846793005ULL + 1; // LCG
        uint64_t idx = state & MASK;
        if ((i & 3) == 0)
            sink ^= cxl[idx];    // CXL pool access
        else
            sink ^= norm[idx];   // normal memory
    }
    printf("sink=%llu\n", (unsigned long long)sink);
    printf("Normal buffer base: %p\n", (void*)norm_buf);
    printf("CXL buffer base: %p\n", (void*)cxl_buf);
    return 0;
}
