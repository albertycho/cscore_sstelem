#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <cerrno>
#include <cstring>
#include <type_traits>

#include "trace_instruction.h"

// Config
constexpr const char* out_dir = "/nethome/kshan9/scratch/src/sst-elements/src/sst/elements/cscore_sstelem/experiments/replication";
constexpr const char* out_name = "synth_rw.champsim.trace";

constexpr uint64_t num_instrs = 20'000'000;
constexpr int mem_pct = 100;     // percent of instructions that are memory ops
constexpr int load_pct = 50;     // percent of mem ops that are loads
constexpr int cxl_pct = 100;     // percent of mem ops that target CXL

constexpr uint64_t line_size = 64;
constexpr uint64_t seed = 0x12345678ull;

// Projection assumptions (for bandwidth printout)
constexpr double clock_ghz = 2.4;
constexpr double ipc_assumed = 1.0;

// Address regions (bytes)
constexpr uint64_t local_base = 0;
constexpr uint64_t local_size = 64ull * 1024 * 1024 * 1024;
constexpr uint64_t cxl_base = 64ull * 1024 * 1024 * 1024;
constexpr uint64_t cxl_size = 64ull * 1024 * 1024 * 1024;

// Working set sizes (bytes). If 0, use full region size.
// Target ~8 MiB total footprint to mirror rw.cpp behavior.
constexpr uint64_t local_ws_bytes = 8ull * 1024 * 1024;
constexpr uint64_t cxl_ws_bytes = 8ull * 1024 * 1024;

static_assert(std::is_trivial<input_instr>::value, "input_instr must be trivial");
static_assert(std::is_standard_layout<input_instr>::value, "input_instr must be standard layout");
static_assert(sizeof(input_instr) == 64, "input_instr layout is not 64 bytes; update generator");

static int clamp_pct(int x) {
    if (x < 0) return 0;
    if (x > 100) return 100;
    return x;
}

static uint64_t pick_addr(bool is_cxl, std::mt19937_64& rng) {
    uint64_t base = is_cxl ? cxl_base : local_base;
    uint64_t size = is_cxl ? cxl_size : local_size;
    uint64_t ws = is_cxl ? cxl_ws_bytes : local_ws_bytes;
    if (ws == 0 || ws > size) ws = size;
    if (ws < line_size) ws = line_size;

    uint64_t elems = ws / line_size;
    if (elems == 0) elems = 1;

    uint64_t idx = static_cast<uint64_t>(rng() % elems);

    uint64_t addr = base + idx * line_size;
    if (addr == 0) addr = line_size;
    return addr;
}

int main() {
    const int mem_pct_clamped = clamp_pct(mem_pct);
    const int load_pct_clamped = clamp_pct(load_pct);
    const int cxl_pct_clamped = clamp_pct(cxl_pct);

    const std::string out_path = std::string(out_dir) + "/" + out_name;

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "error: failed to open output: " << out_path << "\n";
        if (errno == ENOENT) {
            std::cerr << "note: output directory does not exist: " << out_dir << "\n";
        } else if (errno != 0) {
            std::cerr << "note: errno=" << errno << " (" << std::strerror(errno) << ")\n";
        }
        return 2;
    }

    std::mt19937_64 rng(seed);

    uint64_t load_count = 0;
    uint64_t store_count = 0;
    uint64_t load_cxl_count = 0;
    uint64_t load_local_count = 0;
    uint64_t store_cxl_count = 0;
    uint64_t store_local_count = 0;

    for (uint64_t i = 0; i < num_instrs; ++i) {
        input_instr instr{};
        instr.ip = 0x1000ull + (i * 4ull);
        instr.is_branch = 0;
        instr.branch_taken = 0;

        const bool do_mem = (static_cast<int>(rng() % 100) < mem_pct_clamped);
        if (do_mem) {
            const bool do_load = (static_cast<int>(rng() % 100) < load_pct_clamped);
            const bool is_cxl = (static_cast<int>(rng() % 100) < cxl_pct_clamped);
            uint64_t addr = pick_addr(is_cxl, rng);
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
        }

        out.write(reinterpret_cast<const char*>(&instr), sizeof(instr));
        if (!out) {
            std::cerr << "error: write failed at instruction " << i << "\n";
            return 2;
        }
    }

    std::cout << "Wrote " << num_instrs << " instructions to " << out_path << "\n";
    std::cout << "mem_pct=" << mem_pct_clamped
              << " load_pct=" << load_pct_clamped
              << " cxl_pct=" << cxl_pct_clamped
              << " ops_per_instr=1 (single load or store)\n";
    std::cout << "local_base=0x" << std::hex << local_base
              << " cxl_base=0x" << cxl_base << std::dec << "\n";
    std::cout << "use_permutation=false line_size=" << line_size << "\n";

    const double instrs = static_cast<double>(num_instrs);
    const double bytes_per_op = static_cast<double>(line_size);
    const double load_local_bpi = (instrs > 0) ? (static_cast<double>(load_local_count) / instrs) * bytes_per_op : 0.0;
    const double load_cxl_bpi = (instrs > 0) ? (static_cast<double>(load_cxl_count) / instrs) * bytes_per_op : 0.0;
    const double store_local_bpi = (instrs > 0) ? (static_cast<double>(store_local_count) / instrs) * bytes_per_op : 0.0;
    const double store_cxl_bpi = (instrs > 0) ? (static_cast<double>(store_cxl_count) / instrs) * bytes_per_op : 0.0;

    const double load_local_gbps = load_local_bpi * ipc_assumed * clock_ghz;
    const double load_cxl_gbps = load_cxl_bpi * ipc_assumed * clock_ghz;
    const double store_local_gbps = store_local_bpi * ipc_assumed * clock_ghz;
    const double store_cxl_gbps = store_cxl_bpi * ipc_assumed * clock_ghz;
    const double total_gbps = load_local_gbps + load_cxl_gbps + store_local_gbps + store_cxl_gbps;

    std::cout << "Projected BW (GB/s) assuming IPC=" << ipc_assumed
              << " @ " << clock_ghz << "GHz\n";
    std::cout << "  load_local: " << load_local_gbps
              << " load_cxl: " << load_cxl_gbps
              << " store_local: " << store_local_gbps
              << " store_cxl: " << store_cxl_gbps
              << " total: " << total_gbps << "\n";
    return 0;
}
