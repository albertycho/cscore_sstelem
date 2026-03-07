#include "SST_CS_packets.h"
#include <vector>
#include <algorithm>

std::string type_to_string(access_type type) {
    switch(type) {
        case access_type::LOAD: return "LOAD";
        case access_type::RFO: return "RFO";
        case access_type::PREFETCH: return "PREFETCH";
        case access_type::WRITE: return "WRITE";
        case access_type::TRANSLATION: return "TRANSLATION";
        case access_type::NUM_TYPES: return "NUM_TYPES";
        default: return "UNKNOWN";
    }
}

void print_sstreq(sst_request sreq){
    (void)sreq;
}


void print_sstresp(sst_response sresp){
    (void)sresp;
}

//dbug function
bool is_debug_inst(sst_request sreq){
    (void)sreq;
    return false;
}
