#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

constexpr size_t N = 1 << 20; // 1M elements (~8 MiB)
constexpr uint64_t MASK = N - 1;
constexpr size_t ALIGN = 4096;
constexpr size_t EXTRA = ALIGN / sizeof(uint64_t);

uint64_t cxl_buf_raw[N + EXTRA];
uint64_t norm_buf_raw[N + EXTRA];

struct Args {
    uint64_t iters = 10'000'000;
    uint32_t load_pct = 75;
    uint32_t cxl_pct = 25;
    uint32_t mem_ops = 1;
    uint32_t compute_ops = 0;
};

static Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--iters") && i + 1 < argc) {
            args.iters = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--load-pct") && i + 1 < argc) {
            args.load_pct = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--cxl-pct") && i + 1 < argc) {
            args.cxl_pct = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--mem-ops") && i + 1 < argc) {
            args.mem_ops = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--compute-ops") && i + 1 < argc) {
            args.compute_ops = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }
    if (args.load_pct > 100) args.load_pct = 100;
    if (args.cxl_pct > 100) args.cxl_pct = 100;
    if (args.mem_ops == 0) args.mem_ops = 1;
    return args;
}

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);

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
    volatile uint64_t compute_sink = 0;

    for (uint64_t i = 0; i < args.iters; i++) {
        for (uint32_t op = 0; op < args.mem_ops; ++op) {
            state = state * 6364136223846793005ULL + 1; // LCG
            uint64_t idx = state & MASK;
            uint32_t r = static_cast<uint32_t>(state >> 32);
            const bool to_cxl = (r % 100) < args.cxl_pct;
            const bool do_load = ((r / 101) % 100) < args.load_pct;

            if (do_load) {
                sink ^= to_cxl ? cxl[idx] : norm[idx];
            } else {
                if (to_cxl) {
                    cxl[idx] = i + op;
                } else {
                    norm[idx] = i + op;
                }
            }
        }

        for (uint32_t j = 0; j < args.compute_ops; ++j) {
            state = state * 2862933555777941757ULL + 3037000493ULL;
            compute_sink ^= state;
        }
    }

    printf("sink=%llu\n", (unsigned long long)sink);
    printf("Normal buffer base: %p\n", (void*)norm_buf);
    printf("CXL buffer base: %p\n", (void*)cxl_buf);
    printf("iters=%llu load_pct=%u cxl_pct=%u mem_ops=%u compute_ops=%u\n",
           (unsigned long long)args.iters, args.load_pct, args.cxl_pct,
           args.mem_ops, args.compute_ops);
    return 0;
}
