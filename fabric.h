#pragma once

#include <cstdint>
#include <memory>

#include <sst/core/component.h>
#include <sst/core/link.h>

#include "csEvent.h"
#include "lat_bw_queue.h"

namespace SST {
namespace csimCore {

/**
 * Fabric is a simple 2-port link (port 0 <-> port 1).
 * It forwards events from one side to the other and models latency/bandwidth.
 */
class Fabric : public SST::Component {
public:
    Fabric(SST::ComponentId_t id, SST::Params& params);

    SST_ELI_REGISTER_COMPONENT(
        Fabric,
        "cscore",
        "Fabric",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Fabric router for message passing",
        COMPONENT_CATEGORY_UNCATEGORIZED
    )

    SST_ELI_DOCUMENT_PARAMS(
        { "clock", "Clock frequency", "2.4GHz" },
        { "link_bandwidth", "Bandwidth tokens per port (see lat_bw_queue bandwidth units)", "1" },
        { "link_base_latency", "Base latency (cycles) per port; latency function TODO", "1" }
    )

    SST_ELI_DOCUMENT_PORTS(
        { "port_handler0", "Bidirectional port", { "cscore.Fabric", "" } },
        { "port_handler1", "Bidirectional port", { "cscore.Fabric", "" } }
    )

private:
    Fabric();                 // for serialization
    Fabric(const Fabric&) = delete;
    void operator=(const Fabric&) = delete;

    bool tick(SST::Cycle_t);

    SST::Link* port0_ = nullptr;
    SST::Link* port1_ = nullptr;
    std::unique_ptr<lat_bw_queue<csEvent*>> queue_0_to_1_;
    std::unique_ptr<lat_bw_queue<csEvent*>> queue_1_to_0_;
    int64_t link_bandwidth_ = 1;
    int64_t link_base_latency_ = 1;

    void handle_event_port0(SST::Event* ev);
    void handle_event_port1(SST::Event* ev);
};

} // namespace csimCore
} // namespace SST
