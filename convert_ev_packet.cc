#include <convert_ev_packet.h>

// Helper function to convert sst_request to csEvent
SST::csimCore::csEvent* convert_request_to_event(const sst_request &req) {
    auto nev = new SST::csimCore::csEvent();
    nev->payload.push_back(req.address);
    nev->payload.push_back(req.v_address);
    nev->payload.push_back(req.data);
    nev->payload.push_back(req.instr_id);
    nev->payload.push_back(req.ip);
    nev->payload.push_back(req.pf_metadata);
    nev->payload.push_back(req.cpu);
    nev->payload.push_back(req.sst_cpu);  // New field
    nev->payload.push_back(static_cast<uint64_t>(req.type));
    nev->payload.push_back(req.asid[0]);
    nev->payload.push_back(req.asid[1]);
    nev->payload.push_back(req.forward_checked ? 1 : 0);
    nev->payload.push_back(req.is_translated ? 1 : 0);
    nev->payload.push_back(req.response_requested ? 1 : 0);
    return nev;
}


// Helper function to convert csEvent to sst_request
sst_request convert_event_to_request(const SST::csimCore::csEvent &event) {
    sst_request req;
    req.address = event.payload[0];
    req.v_address = event.payload[1];
    req.data = event.payload[2];
    req.instr_id = event.payload[3];
    req.ip = event.payload[4];
    req.pf_metadata = event.payload[5];
    req.cpu = event.payload[6];
    req.sst_cpu = event.payload[7];  // New field
    req.type = static_cast<access_type>(event.payload[8]);
    req.asid[0] = static_cast<uint8_t>(event.payload[9]);
    req.asid[1] = static_cast<uint8_t>(event.payload[10]);
    req.forward_checked = event.payload[11];
    req.is_translated = event.payload[12];
    req.response_requested = event.payload[13];
    return req;
}

// Helper function to convert sst_response to csEvent
SST::csimCore::csEvent* convert_response_to_event(const sst_response &resp) {
    auto nev = new SST::csimCore::csEvent();
    nev->payload.push_back(resp.address);
    nev->payload.push_back(resp.v_address);
    nev->payload.push_back(resp.data);
    nev->payload.push_back(resp.pf_metadata);
    nev->payload.push_back(resp.cpu);
    nev->payload.push_back(resp.sst_cpu);  // New field
    return nev;
}


// Helper function to convert csEvent to sst_response
sst_response convert_event_to_response(const SST::csimCore::csEvent &event) {
    sst_response resp(event.payload[0], event.payload[1], event.payload[2], event.payload[3], event.payload[4], event.payload[5]);  // New field
    return resp;
}


