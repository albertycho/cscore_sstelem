#pragma once

#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include "SST_CS_packets.h"
#include "control_event.h"
#include "csEvent.h"

namespace SST {
namespace csimCore {
namespace response_timeline {

inline constexpr std::size_t kResponseInstrIdPayloadIndex = 9;
inline constexpr std::size_t kResponseTraceTagPayloadIndex = 10;
inline constexpr std::size_t kResponseTypePayloadIndex = 11;
inline constexpr std::size_t kRequestPayloadMinSize = 18;

inline std::mutex g_mutex;
inline std::unique_ptr<std::ofstream> g_stream;
inline bool g_initialized = false;
inline bool g_enabled = false;
inline bool g_header_written = false;
inline std::string g_path;

inline void initialize() {
    if (g_initialized) {
        return;
    }
    g_initialized = true;
    if (const char* env = std::getenv("CSCORE_RESPONSE_TIMELINE_PATH"); env != nullptr && *env != '\0') {
        g_enabled = true;
        g_path = env;
    }
}

inline bool enabled() {
    initialize();
    return g_enabled;
}

inline std::ofstream* stream() {
    initialize();
    if (!g_enabled) {
        return nullptr;
    }
    if (!g_stream) {
        g_stream = std::make_unique<std::ofstream>(g_path, std::ios::out | std::ios::trunc);
    }
    return g_stream.get();
}

inline uint64_t normalize_access_type(access_type type) {
    return static_cast<uint64_t>(type);
}

inline bool is_response_event(const csEvent* ev) {
    return ev != nullptr &&
           !::SST::csimCore::is_control_event(*ev) &&
           ev->payload.size() > 8 &&
           ev->payload.size() < kRequestPayloadMinSize;
}

inline uint64_t response_instr_id(const csEvent* ev) {
    if (!is_response_event(ev) || ev->payload.size() <= kResponseInstrIdPayloadIndex) {
        return 0;
    }
    return ev->payload[kResponseInstrIdPayloadIndex];
}

inline uint64_t response_trace_tag(const csEvent* ev) {
    if (!is_response_event(ev) || ev->payload.size() <= kResponseTraceTagPayloadIndex) {
        return 0;
    }
    return ev->payload[kResponseTraceTagPayloadIndex];
}

inline uint64_t response_access_type(const csEvent* ev) {
    if (!is_response_event(ev) || ev->payload.size() <= kResponseTypePayloadIndex) {
        return normalize_access_type(access_type::LOAD);
    }
    return ev->payload[kResponseTypePayloadIndex];
}

inline uint64_t response_address(const csEvent* ev) {
    return (ev != nullptr && ev->payload.size() > 2) ? ev->payload[2] : 0;
}

inline uint64_t response_msg_bytes(const csEvent* ev) {
    return (ev != nullptr && ev->payload.size() > 8) ? ev->payload[8] : 0;
}

inline uint64_t response_src(const csEvent* ev) {
    return (ev != nullptr && !ev->payload.empty()) ? ev->payload[0] : 0;
}

inline uint64_t response_dst(const csEvent* ev) {
    return (ev != nullptr && ev->payload.size() > 1) ? ev->payload[1] : 0;
}

inline void write_record(const char* kind,
                         const std::string& location,
                         const std::string& stage,
                         uint64_t cycle,
                         uint64_t instr_id,
                         uint64_t trace_tag,
                         uint64_t type,
                         uint64_t src,
                         uint64_t dst,
                         uint64_t address,
                         uint64_t msg_bytes,
                         uint64_t wait_cycles,
                         uint64_t queue_wait_cycles,
                         uint64_t pre_queue_occ_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto* out = stream();
    if (out == nullptr) {
        return;
    }
    if (!g_header_written) {
        *out << "kind,location,stage,cycle,instr_id,trace_tag,type,src,dst,address,msg_bytes,wait_cycles,queue_wait_cycles,pre_queue_occ_bytes\n";
        g_header_written = true;
    }
    *out << kind << ','
         << location << ','
         << stage << ','
         << cycle << ','
         << instr_id << ','
         << trace_tag << ','
         << type << ','
         << src << ','
         << dst << ','
         << address << ','
         << msg_bytes << ','
         << wait_cycles << ','
         << queue_wait_cycles << ','
         << pre_queue_occ_bytes << '\n';
}

inline void log_request(const std::string& location,
                        const std::string& stage,
                        uint64_t cycle,
                        const sst_request& req,
                        uint64_t trace_tag = 0,
                        uint64_t wait_cycles = 0) {
    if (!enabled()) {
        return;
    }
    write_record("request",
                 location,
                 stage,
                 cycle,
                 req.instr_id,
                 trace_tag != 0 ? trace_tag : req.trace_tag,
                 normalize_access_type(req.type),
                 req.src_node == std::numeric_limits<uint32_t>::max() ? req.sst_cpu : req.src_node,
                 req.dst_node == std::numeric_limits<uint32_t>::max() ? req.cpu : req.dst_node,
                 req.address,
                 req.msg_bytes,
                 wait_cycles,
                 0,
                 0);
}

inline void log_response(const std::string& location,
                         const std::string& stage,
                         uint64_t cycle,
                         const sst_response& resp,
                         uint64_t wait_cycles = 0,
                         uint64_t queue_wait_cycles = 0,
                         uint64_t pre_queue_occ_bytes = 0) {
    if (!enabled()) {
        return;
    }
    write_record("response",
                 location,
                 stage,
                 cycle,
                 resp.instr_id,
                 resp.trace_tag,
                 normalize_access_type(resp.type),
                 resp.src_node == std::numeric_limits<uint32_t>::max() ? resp.sst_cpu : resp.src_node,
                 resp.dst_node == std::numeric_limits<uint32_t>::max() ? resp.cpu : resp.dst_node,
                 resp.address,
                 resp.msg_bytes,
                 wait_cycles,
                 queue_wait_cycles,
                 pre_queue_occ_bytes);
}

inline void log_response_event(const std::string& location,
                               const std::string& stage,
                               uint64_t cycle,
                               const csEvent* ev,
                               uint64_t wait_cycles = 0,
                               uint64_t queue_wait_cycles = 0,
                               uint64_t pre_queue_occ_bytes = 0) {
    if (!enabled() || !is_response_event(ev)) {
        return;
    }
    write_record("response",
                 location,
                 stage,
                 cycle,
                 response_instr_id(ev),
                 response_trace_tag(ev),
                 response_access_type(ev),
                 response_src(ev),
                 response_dst(ev),
                 response_address(ev),
                 response_msg_bytes(ev),
                 wait_cycles,
                 queue_wait_cycles,
                 pre_queue_occ_bytes);
}

} // namespace response_timeline
} // namespace csimCore
} // namespace SST
