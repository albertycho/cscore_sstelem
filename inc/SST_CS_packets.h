#ifndef CSIM_SST_PACKET_H
#define CSIM_SST_PACKET_H

#include <string>
#include <limits>
#include <iostream>
#include "remote_access_timing.h"
#include "access_type.h"
// enum class access_type : unsigned {
//   LOAD = 0,
//   RFO,
//   PREFETCH,
//   WRITE,
//   TRANSLATION,
//   NUM_TYPES,
// };

/*
    sst_request / response are same as request and response, removed intr_depend_on_me
 */
struct sst_request {
    uint32_t src_node = std::numeric_limits<uint32_t>::max();
    uint32_t dst_node = std::numeric_limits<uint32_t>::max();

    bool forward_checked = false;
    bool is_translated = true;
    bool response_requested = true;

    uint8_t asid[2] = {std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()};
    access_type type{access_type::LOAD};

    uint32_t pf_metadata = 0;
    uint32_t cpu = std::numeric_limits<uint32_t>::max();
    uint32_t sst_cpu = 0;

    uint64_t address = 0;
    uint64_t v_address = 0;
    uint64_t data = 0;
    uint64_t instr_id = 0;
    uint64_t trace_tag = 0;
    uint64_t ip = 0;
    uint16_t msg_bytes = 64;
    remote_access_timing remote_timing{};

};

struct sst_response {
    uint32_t src_node = std::numeric_limits<uint32_t>::max();
    uint32_t dst_node = std::numeric_limits<uint32_t>::max();

    uint64_t address;
    uint64_t v_address;
    uint64_t data;
    uint32_t pf_metadata = 0;
    uint32_t cpu=0;
    uint32_t sst_cpu = 0;
    uint64_t instr_id = 0;
    uint64_t trace_tag = 0;
    uint16_t msg_bytes = 64;
    remote_access_timing remote_timing{};

    sst_response(uint64_t addr, uint64_t v_addr, uint64_t data_, uint32_t pf_meta, uint32_t cpu_n, uint32_t sst_cpu_n,
                 uint64_t instr_id_n = 0, uint64_t trace_tag_n = 0)
        : address(addr), v_address(v_addr), data(data_), pf_metadata(pf_meta), cpu(cpu_n), sst_cpu(sst_cpu_n),
          instr_id(instr_id_n), trace_tag(trace_tag_n), msg_bytes(64)
    {
    }
    explicit sst_response(sst_request req)
        : sst_response(req.address, req.v_address, req.data, req.pf_metadata, req.cpu, req.sst_cpu, req.instr_id, req.trace_tag) {}
};

std::string type_to_string(access_type type);
void print_sstreq(sst_request sreq);
void print_sstresp(sst_response sresp);
bool is_debug_inst(sst_request sreq);


#endif
