#ifndef _CONVERT_EV_PACKET_H
#define _CONVERT_EV_PACKET_H

#include "csEvent.h"
#include "SST_CS_packets.h"
#include <vector>

// Helper function to convert sst_request to csEvent
SST::csimCore::csEvent* convert_request_to_event(const sst_request &req);

// Helper function to convert csEvent to sst_request
sst_request convert_event_to_request(const SST::csimCore::csEvent &event);

// Helper function to convert sst_response to csEvent
SST::csimCore::csEvent* convert_response_to_event(const sst_response &resp);

// Helper function to convert csEvent to sst_response
sst_response convert_event_to_response(const SST::csimCore::csEvent &event);

#endif // _CONVERT_EV_PACKET_H
