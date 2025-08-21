#include "SST_CS_packets.h"
#include <iostream>
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
    std::cout<<"print_sstreq"<<std::endl;
    std::cout<<"    addr: "<<sreq.address<<std::endl;
    std::cout<<"  v_addr: "<<sreq.v_address<<std::endl;
    std::cout<<"    instr_id: "<<sreq.instr_id<<std::endl;
    std::cout<<"    type: "<<type_to_string(sreq.type)<<std::endl;
}


void print_sstresp(sst_response sresp){
    std::cout<<"print_sstresp"<<std::endl;
    std::cout<<"    addr: "<<sresp.address<<std::endl;
    std::cout<<"  v_addr: "<<sresp.v_address<<std::endl;
    //std::cout<<"    addr: "<<sresp.<<std::endl;

}

//dbug function
bool is_debug_inst(sst_request sreq){
    uint64_t in_instr_id = sreq.instr_id;

    std::vector dbg_instr_ids = {809219,1229619, 1263796,  1362009};
    if(std::find(dbg_instr_ids.begin(), dbg_instr_ids.end(), in_instr_id) != dbg_instr_ids.end()) {
        return true;
    }
    return false;

}
