#include <cstdio>
#include <cstdlib>
#include <cstdint>

int main() {
    constexpr size_t N = 4096;
    uint64_t* cxl_buf;
    posix_memalign((void**)&cxl_buf, 64, N * sizeof(uint64_t));
    uint64_t* norm_buf = (uint64_t*)malloc(N * sizeof(uint64_t));
    printf("CXL buffer base: %p\n", (void*)cxl_buf);

    volatile uint64_t val;

    for (int i = 0; i < 1000; i++) {
        if (i % 4 == 0)
            val = cxl_buf[i % N];    // CXL pool access
        else
            val = norm_buf[i % N];   // normal memory
    }
    return 0;
}