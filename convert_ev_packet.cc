#include <convert_ev_packet.h>
#include <limits>

// Helper function to convert sst_request to csEvent
SST::csimCore::csEvent* convert_request_to_event(const sst_request &req) {
    auto src = (req.src_node == std::numeric_limits<uint32_t>::max()) ? req.cpu : req.src_node;
    auto dst = (req.dst_node == std::numeric_limits<uint32_t>::max()) ? req.sst_cpu : req.dst_node;
    auto nev = new SST::csimCore::csEvent();
    nev->payload.reserve(17);
    nev->payload.push_back(src);
    nev->payload.push_back(dst);
    nev->payload.push_back(req.address);
    nev->payload.push_back(req.v_address);
    nev->payload.push_back(req.data);
    nev->payload.push_back(req.instr_id);
    nev->payload.push_back(req.ip);
    nev->payload.push_back(req.pf_metadata);
    nev->payload.push_back(req.cpu);
    nev->payload.push_back(req.sst_cpu);
    nev->payload.push_back(static_cast<uint64_t>(req.type));
    nev->payload.push_back(req.asid[0]);
    nev->payload.push_back(req.asid[1]);
    nev->payload.push_back(req.forward_checked ? 1 : 0);
    nev->payload.push_back(req.is_translated ? 1 : 0);
    nev->payload.push_back(req.response_requested ? 1 : 0);
    nev->payload.push_back(req.msg_bytes);
    return nev;
}

// Helper function to convert csEvent to sst_request
sst_request convert_event_to_request(const SST::csimCore::csEvent &event) {
    sst_request req;
    if (event.payload.size() < 2 + 14) {
        return req;
    }
    req.src_node = static_cast<uint32_t>(event.payload[0]);
    req.dst_node = static_cast<uint32_t>(event.payload[1]);
    req.address = event.payload[2];
    req.v_address = event.payload[3];
    req.data = event.payload[4];
    req.instr_id = event.payload[5];
    req.ip = event.payload[6];
    req.pf_metadata = static_cast<uint32_t>(event.payload[7]);
    req.cpu = static_cast<uint32_t>(event.payload[8]);
    req.sst_cpu = static_cast<uint32_t>(event.payload[9]);
    req.type = static_cast<access_type>(event.payload[10]);
    req.asid[0] = static_cast<uint8_t>(event.payload[11]);
    req.asid[1] = static_cast<uint8_t>(event.payload[12]);
    req.forward_checked = (event.payload[13] != 0);
    req.is_translated = (event.payload[14] != 0);
    req.response_requested = (event.payload[15] != 0);
    if (event.payload.size() > 16) {
        req.msg_bytes = static_cast<uint16_t>(event.payload[16]);
    }
    return req;
}

// Helper function to convert sst_response to csEvent
SST::csimCore::csEvent* convert_response_to_event(const sst_response &resp) {
    auto src = (resp.src_node == std::numeric_limits<uint32_t>::max()) ? resp.sst_cpu : resp.src_node;
    auto dst = (resp.dst_node == std::numeric_limits<uint32_t>::max()) ? resp.cpu : resp.dst_node;
    auto nev = new SST::csimCore::csEvent();
    nev->payload.reserve(9);
    nev->payload.push_back(src);
    nev->payload.push_back(dst);
    nev->payload.push_back(resp.address);
    nev->payload.push_back(resp.v_address);
    nev->payload.push_back(resp.data);
    nev->payload.push_back(resp.pf_metadata);
    nev->payload.push_back(resp.cpu);
    nev->payload.push_back(resp.sst_cpu);
    nev->payload.push_back(resp.msg_bytes);
    return nev;
}

// Helper function to convert csEvent to sst_response
sst_response convert_event_to_response(const SST::csimCore::csEvent &event) {
    if (event.payload.size() < 2 + 6) {
        return sst_response{0,0,0,0,0,0};
    }
    sst_response resp(event.payload[2], event.payload[3], event.payload[4],
                      event.payload[5], event.payload[6], event.payload[7]);
    resp.src_node = static_cast<uint32_t>(event.payload[0]);
    resp.dst_node = static_cast<uint32_t>(event.payload[1]);
    if (event.payload.size() > 8) {
        resp.msg_bytes = static_cast<uint16_t>(event.payload[8]);
    }
    return resp;
}
