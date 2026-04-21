#ifndef REMOTE_ACCESS_TIMING_H
#define REMOTE_ACCESS_TIMING_H

#include <cstdint>

struct remote_access_timing {
    uint64_t roundtrip_cycles = 0;
    uint64_t interface_cycles = 0;
    uint64_t queue_cycles = 0;
    uint64_t access_service_cycles = 0;
};

#endif
