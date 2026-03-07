#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <cerrno>
#include <cstring>
#include <type_traits>

#include "trace_instruction.h"

struct Config {
    std::string out_dir = "/nethome/kshan9/scratch/src/sst-elements/src/sst/elements/cscore_sstelem/experiments/replication";
    std::string out_name = "synth_rw.champsim.trace";

    uint64_t num_instrs = 20'000'000;
    uint64_t warmup_mem_instrs = 200'000;
    int mem_pct = 100;   // target aggregate CXL request-link utilization percent (0-100)
    int load_pct = 50;  // percent of mem ops that are loads
    int cxl_pct = 100;  // percent of mem ops that target CXL
    uint64_t seed = 0x12345678ull;
};

// Fixed parameters (not configurable via CLI)
constexpr uint64_t kLineSize = 64;
constexpr double kClockGhz = 2.4;
// Utilization->probability mapping depends on this assumption.
constexpr double kIpcAssumed = 0.5;
constexpr uint64_t kCooldownInstrs = 500'000;
constexpr int kNumNodes = 8;
constexpr int kNumPools = 1;
constexpr bool kReplicateWrites = false;
constexpr int kBwCxlCycles = 25;
constexpr uint64_t kProbScale = 1'000'000;

// Address regions (bytes)
constexpr uint64_t kLocalBase = 0;
constexpr uint64_t kLocalSize = 64ull * 1024 * 1024 * 1024;
constexpr uint64_t kCxlBase = 64ull * 1024 * 1024 * 1024;
constexpr uint64_t kCxlSize = 64ull * 1024 * 1024 * 1024;

// Working set sizes (bytes). If 0, use full region size.
constexpr uint64_t kLocalWsBytes = 8ull * 1024 * 1024;
constexpr uint64_t kCxlWsBytes = 8ull * 1024 * 1024;

static_assert(std::is_trivial<input_instr>::value, "input_instr must be trivial");
static_assert(std::is_standard_layout<input_instr>::value, "input_instr must be standard layout");
static_assert(sizeof(input_instr) == 64, "input_instr layout is not 64 bytes; update generator");

static int clamp_pct(int x) {
    if (x < 0) return 0;
    if (x > 100) return 100;
    return x;
}

static bool parse_u64(const std::string& s, uint64_t* out) {
    try {
        std::size_t idx = 0;
        const auto val = std::stoull(s, &idx, 0);
        if (idx != s.size()) return false;
        *out = val;
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_i32(const std::string& s, int* out) {
    try {
        std::size_t idx = 0;
        const auto val = std::stol(s, &idx, 0);
        if (idx != s.size()) return false;
        *out = static_cast<int>(val);
        return true;
    } catch (...) {
        return false;
    }
}

static void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  --out-dir <path>\n"
        << "  --out-name <file>\n"
        << "  --num-instrs <N>\n"
        << "  --warmup-mem-instrs <N>\n"
        << "  --mem-pct <0-100>\n"
        << "  --load-pct <0-100>\n"
        << "  --cxl-pct <0-100>\n"
        << "  --seed <u64>\n";
}

static bool parse_args(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        }
        if (arg.rfind("--", 0) != 0) {
            std::cerr << "error: unexpected arg: " << arg << "\n";
            print_usage(argv[0]);
            return false;
        }

        std::string key;
        std::string value;
        auto eq = arg.find('=');
        if (eq != std::string::npos) {
            key = arg.substr(2, eq - 2);
            value = arg.substr(eq + 1);
        } else {
            key = arg.substr(2);
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for --" << key << "\n";
                print_usage(argv[0]);
                return false;
            }
            value = argv[++i];
        }

        if (key == "out-dir") {
            cfg.out_dir = value;
        } else if (key == "out-name") {
            cfg.out_name = value;
        } else if (key == "num-instrs") {
            if (!parse_u64(value, &cfg.num_instrs)) return false;
        } else if (key == "warmup-mem-instrs") {
            if (!parse_u64(value, &cfg.warmup_mem_instrs)) return false;
        } else if (key == "mem-pct") {
            if (!parse_i32(value, &cfg.mem_pct)) return false;
        } else if (key == "load-pct") {
            if (!parse_i32(value, &cfg.load_pct)) return false;
        } else if (key == "cxl-pct") {
            if (!parse_i32(value, &cfg.cxl_pct)) return false;
        } else if (key == "seed") {
            if (!parse_u64(value, &cfg.seed)) return false;
        } else {
            std::cerr << "error: unknown option --" << key << "\n";
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

static uint64_t pick_addr(bool is_cxl, std::mt19937_64& rng, const Config& cfg) {
    uint64_t base = is_cxl ? kCxlBase : kLocalBase;
    uint64_t size = is_cxl ? kCxlSize : kLocalSize;
    uint64_t ws = is_cxl ? kCxlWsBytes : kLocalWsBytes;
    if (ws == 0 || ws > size) ws = size;
    if (ws < kLineSize) ws = kLineSize;

    uint64_t elems = ws / kLineSize;
    if (elems == 0) elems = 1;

    uint64_t idx = static_cast<uint64_t>(rng() % elems);

    uint64_t addr = base + idx * kLineSize;
    if (addr == 0) addr = kLineSize;
    return addr;
}

int main(int argc, char** argv) {
    Config cfg{};
    if (!parse_args(argc, argv, cfg)) {
        return 2;
    }

    const int mem_pct_clamped = clamp_pct(cfg.mem_pct);
    const int load_pct_clamped = clamp_pct(cfg.load_pct);
    const int cxl_pct_clamped = clamp_pct(cfg.cxl_pct);

    const uint64_t fill_mem_instrs = std::min(cfg.warmup_mem_instrs, cfg.num_instrs);
    const uint64_t cooldown_instrs = std::min(kCooldownInstrs, cfg.num_instrs);
    const std::string out_path = cfg.out_dir + "/" + cfg.out_name;

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "error: failed to open output: " << out_path << "\n";
        if (errno == ENOENT) {
            std::cerr << "note: output directory does not exist: " << cfg.out_dir << "\n";
        } else if (errno != 0) {
            std::cerr << "note: errno=" << errno << " (" << std::strerror(errno) << ")\n";
        }
        return 2;
    }

    std::mt19937_64 rng(cfg.seed);

    uint64_t load_count = 0;
    uint64_t store_count = 0;
    uint64_t load_cxl_count = 0;
    uint64_t load_local_count = 0;
    uint64_t store_cxl_count = 0;
    uint64_t store_local_count = 0;
    uint64_t main_loop_load_count = 0;
    uint64_t main_loop_store_count = 0;

    const double util_target = static_cast<double>(mem_pct_clamped) / 100.0;
    const double load_frac = static_cast<double>(load_pct_clamped) / 100.0;
    const double store_frac = 1.0 - load_frac;
    const double cxl_frac = static_cast<double>(cxl_pct_clamped) / 100.0;
    const double pool_factor = kReplicateWrites
        ? (load_frac / static_cast<double>(kNumPools)) + store_frac
        : 1.0 / static_cast<double>(kNumPools);
    const double denom = kIpcAssumed
        * static_cast<double>(kNumNodes)
        * static_cast<double>(kBwCxlCycles)
        * cxl_frac
        * pool_factor;
    double mem_prob = (denom > 0.0) ? (util_target / denom) : 0.0;
    if (mem_prob < 0.0) mem_prob = 0.0;
    if (mem_prob > 1.0) mem_prob = 1.0;
    const uint64_t mem_threshold = static_cast<uint64_t>(mem_prob * static_cast<double>(kProbScale) + 0.5);
    const double modeled_util =
        mem_prob * kIpcAssumed * static_cast<double>(kNumNodes) * cxl_frac * pool_factor * static_cast<double>(kBwCxlCycles);

    uint64_t instr_idx = 0;

    // Loop 1: fill cache (all mem ops)
    for (uint64_t i = 0; i < fill_mem_instrs && instr_idx < cfg.num_instrs; ++i, ++instr_idx) {
        input_instr instr{};
        instr.ip = 0x1000ull + (instr_idx * 4ull);
        instr.is_branch = 0;
        instr.branch_taken = 0;

        const bool do_load = (static_cast<int>(rng() % 100) < load_pct_clamped);
        const bool is_cxl = (static_cast<int>(rng() % 100) < cxl_pct_clamped);
        uint64_t addr = pick_addr(is_cxl, rng, cfg);
        if (do_load) {
            instr.source_memory[0] = addr;
            load_count++;
            if (is_cxl) {
                load_cxl_count++;
            } else {
                load_local_count++;
            }
        } else {
            instr.destination_memory[0] = addr;
            store_count++;
            if (is_cxl) {
                store_cxl_count++;
            } else {
                store_local_count++;
            }
        }

        out.write(reinterpret_cast<const char*>(&instr), sizeof(instr));
        if (!out) {
            std::cerr << "error: write failed at instruction " << instr_idx << "\n";
            return 2;
        }
    }

    // Loop 2: cooldown (non-mem ops only)
    for (uint64_t i = 0; i < cooldown_instrs && instr_idx < cfg.num_instrs; ++i, ++instr_idx) {
        input_instr instr{};
        instr.ip = 0x1000ull + (instr_idx * 4ull);
        instr.is_branch = 0;
        instr.branch_taken = 0;

        out.write(reinterpret_cast<const char*>(&instr), sizeof(instr));
        if (!out) {
            std::cerr << "error: write failed at instruction " << instr_idx << "\n";
            return 2;
        }
    }

    // Loop 3: main traffic (mem_pct after cooldown)
    for (; instr_idx < cfg.num_instrs; ++instr_idx) {
        input_instr instr{};
        instr.ip = 0x1000ull + (instr_idx * 4ull);
        instr.is_branch = 0;
        instr.branch_taken = 0;

        const bool do_mem = ((rng() % kProbScale) < mem_threshold);
        if (do_mem) {
            const bool do_load = (static_cast<int>(rng() % 100) < load_pct_clamped);
            const bool is_cxl = (static_cast<int>(rng() % 100) < cxl_pct_clamped);
            uint64_t addr = pick_addr(is_cxl, rng, cfg);
            if (do_load) {
                instr.source_memory[0] = addr;
                load_count++;
                main_loop_load_count++;
                if (is_cxl) {
                    load_cxl_count++;
                } else {
                    load_local_count++;
                }
            } else {
                instr.destination_memory[0] = addr;
                store_count++;
                main_loop_store_count++;
                if (is_cxl) {
                    store_cxl_count++;
                } else {
                    store_local_count++;
                }
            }
        }

        out.write(reinterpret_cast<const char*>(&instr), sizeof(instr));
        if (!out) {
            std::cerr << "error: write failed at instruction " << instr_idx << "\n";
            return 2;
        }
    }

    std::cout << "Wrote " << cfg.num_instrs << " instructions to " << out_path << "\n";
    std::cout << "fill_mem_instrs=" << fill_mem_instrs
              << " cooldown_instrs=" << cooldown_instrs
              << " util_target_pct=" << mem_pct_clamped
              << " effective_mem_prob=" << mem_prob
              << " load_pct=" << load_pct_clamped
              << " cxl_pct=" << cxl_pct_clamped
              << " ops_per_instr=1 (single load or store)\n";
    std::cout << "main_loop_loads=" << main_loop_load_count
              << " main_loop_stores=" << main_loop_store_count << "\n";
    std::cout << "util_formula ipc=" << kIpcAssumed
              << " nodes=" << kNumNodes
              << " pools=" << kNumPools
              << " replicate_writes=" << (kReplicateWrites ? 1 : 0)
              << " cxl_frac=" << cxl_frac
              << " pool_factor=" << pool_factor
              << " modeled_util=" << modeled_util
              << "\n";
    std::cout << "local_base=0x" << std::hex << kLocalBase
              << " cxl_base=0x" << kCxlBase << std::dec << "\n";
    std::cout << "line_size=" << kLineSize << "\n";

    const double instrs = static_cast<double>(cfg.num_instrs);
    const double bytes_per_op = static_cast<double>(kLineSize);
    const double load_local_bpi = (instrs > 0) ? (static_cast<double>(load_local_count) / instrs) * bytes_per_op : 0.0;
    const double load_cxl_bpi = (instrs > 0) ? (static_cast<double>(load_cxl_count) / instrs) * bytes_per_op : 0.0;
    const double store_local_bpi = (instrs > 0) ? (static_cast<double>(store_local_count) / instrs) * bytes_per_op : 0.0;
    const double store_cxl_bpi = (instrs > 0) ? (static_cast<double>(store_cxl_count) / instrs) * bytes_per_op : 0.0;

    const double load_local_gbps = load_local_bpi * kIpcAssumed * kClockGhz;
    const double load_cxl_gbps = load_cxl_bpi * kIpcAssumed * kClockGhz;
    const double store_local_gbps = store_local_bpi * kIpcAssumed * kClockGhz;
    const double store_cxl_gbps = store_cxl_bpi * kIpcAssumed * kClockGhz;
    const double total_gbps = load_local_gbps + load_cxl_gbps + store_local_gbps + store_cxl_gbps;

    std::cout << "Projected BW (GB/s) assuming IPC=" << kIpcAssumed
              << " @ " << kClockGhz << "GHz\n";
    std::cout << "  load_local: " << load_local_gbps
              << " load_cxl: " << load_cxl_gbps
              << " store_local: " << store_local_gbps
              << " store_cxl: " << store_cxl_gbps
              << " total: " << total_gbps << "\n";
    return 0;
}
