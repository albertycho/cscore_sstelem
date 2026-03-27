#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "trace_instruction.h"

struct Config {
    std::string out_dir = ".";
    std::string out_name = "pointer_chase.champsim.trace";
    uint64_t num_instrs = 20'000'000;
    uint64_t seed = 0x12345678ull;
    uint64_t base_addr = 64ull * 1024 * 1024 * 1024;
    uint64_t region_size = 64ull * 1024 * 1024 * 1024;
    uint64_t working_set_bytes = 8ull * 1024 * 1024;
};

constexpr uint64_t kLineSize = 64;
constexpr uint64_t kBaseIp = 0x1000ull;
constexpr unsigned char kPtrReg = 1;

static_assert(std::is_trivial<input_instr>::value, "input_instr must be trivial");
static_assert(std::is_standard_layout<input_instr>::value, "input_instr must be standard layout");
static_assert(sizeof(input_instr) == 64, "input_instr layout changed; update generator");

static bool parse_u64(const std::string& s, uint64_t* out)
{
    try {
        std::size_t idx = 0;
        const auto val = std::stoull(s, &idx, 0);
        if (idx != s.size()) {
            return false;
        }
        *out = val;
        return true;
    } catch (...) {
        return false;
    }
}

static void print_usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0 << " [options]\n"
        << "Options:\n"
        << "  --out-dir <path>\n"
        << "  --out-name <file>\n"
        << "  --num-instrs <N>\n"
        << "  --seed <u64>\n"
        << "  --base-addr <addr>\n"
        << "  --region-size <bytes>\n"
        << "  --working-set-bytes <bytes>\n";
}

static bool parse_args(int argc, char** argv, Config& cfg)
{
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
        const auto eq = arg.find('=');
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
        } else if (key == "seed") {
            if (!parse_u64(value, &cfg.seed)) return false;
        } else if (key == "base-addr") {
            if (!parse_u64(value, &cfg.base_addr)) return false;
        } else if (key == "region-size") {
            if (!parse_u64(value, &cfg.region_size)) return false;
        } else if (key == "working-set-bytes") {
            if (!parse_u64(value, &cfg.working_set_bytes)) return false;
        } else {
            std::cerr << "error: unknown option --" << key << "\n";
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

int main(int argc, char** argv)
{
    Config cfg{};
    if (!parse_args(argc, argv, cfg)) {
        return 2;
    }

    if (cfg.region_size < kLineSize) {
        std::cerr << "error: region-size must be at least " << kLineSize << " bytes\n";
        return 2;
    }

    uint64_t working_set = cfg.working_set_bytes;
    if (working_set == 0 || working_set > cfg.region_size) {
        working_set = cfg.region_size;
    }
    working_set = std::max<uint64_t>(working_set, kLineSize);
    working_set = (working_set / kLineSize) * kLineSize;
    if (working_set == 0) {
        working_set = kLineSize;
    }

    const uint64_t line_count = working_set / kLineSize;
    std::vector<uint64_t> line_offsets(static_cast<std::size_t>(line_count));
    std::iota(line_offsets.begin(), line_offsets.end(), uint64_t{0});

    std::mt19937_64 rng(cfg.seed);
    std::shuffle(line_offsets.begin(), line_offsets.end(), rng);

    const std::string out_path = cfg.out_dir + "/" + cfg.out_name;
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "error: failed to open output: " << out_path << "\n";
        return 2;
    }

    for (uint64_t i = 0; i < cfg.num_instrs; ++i) {
        input_instr instr{};
        instr.ip = kBaseIp;
        instr.is_branch = 0;
        instr.branch_taken = 0;
        instr.source_registers[0] = kPtrReg;
        instr.destination_registers[0] = kPtrReg;
        instr.source_memory[0] = cfg.base_addr + (line_offsets[static_cast<std::size_t>(i % line_count)] * kLineSize);

        out.write(reinterpret_cast<const char*>(&instr), sizeof(instr));
        if (!out) {
            std::cerr << "error: write failed at instruction " << i << "\n";
            return 2;
        }
    }

    std::cout << "Wrote " << cfg.num_instrs << " instructions to " << out_path << "\n";
    std::cout << "mode=pointer_chase\n";
    std::cout << "base_addr=0x" << std::hex << cfg.base_addr << std::dec << "\n";
    std::cout << "region_size=" << cfg.region_size << "\n";
    std::cout << "working_set_bytes=" << working_set << "\n";
    std::cout << "line_size=" << kLineSize << "\n";
    std::cout << "line_count=" << line_count << "\n";
    std::cout << "dependency_reg=" << static_cast<uint32_t>(kPtrReg) << "\n";
    std::cout << "addr_order=random_permutation_repeated\n";
    return 0;
}
