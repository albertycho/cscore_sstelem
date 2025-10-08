
#ifndef _NWSIM_H
#define _NWSIM_H

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <iostream>
#include <fstream>
#include "csEvent.h"

#include "cs_sst_constants.h"



namespace SST {
namespace csimCore {
class NWsim : public SST::Component
{
public:
    NWsim(SST::ComponentId_t id, SST::Params& params);		// Constructor

    SST_ELI_REGISTER_COMPONENT(
        NWsim,
        "cscore",
        "NWsim",
        SST_ELI_ELEMENT_VERSION(1,0,0),
        "Network Model Componnent",
	COMPONENT_CATEGORY_UNCATEGORIZED
    )

    SST_ELI_DOCUMENT_PARAMS(
	    //{ "clock", "Clock frequency", "1GHz" },
        { "outfile_name", "Path to output file", ""}
        
    )
    SST_ELI_DOCUMENT_PORTS(
        {"port_handler0",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler1",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler2",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler3",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler4",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler5",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler6",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler7",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler8",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler9",  "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler10", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler11", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler12", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler13", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler14", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler15", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler16", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler17", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler18", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler19", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler20", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler21", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler22", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler23", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler24", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler25", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler26", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler27", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler28", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler29", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler30", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler31", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler32", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler33", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler34", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler35", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler36", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler37", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler38", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler39", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler40", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler41", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler42", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler43", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler44", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler45", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler46", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler47", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler48", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler49", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler50", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler51", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler52", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler53", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler54", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler55", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler56", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler57", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler58", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler59", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler60", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler61", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler62", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} },
        {"port_handler63", "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.NWsim", ""} }
    )
    

private:
    std::vector<csEvent*> delayed_msgs;
    uint64_t allowed_msgs;
	uint64_t NWBW_in_GB;

private:
    NWsim();  // for serialization only
    NWsim(const NWsim&); // do not implement
    void operator=(const NWsim&); // do not implement

    std::ofstream outf;
    SST::Link* linkHandler[NUM_NODES];
    int cycle_count;

    void handleNWEvent(SST::Event *ev);
    bool NW_tick(Cycle_t);

    //for stat tracking
    uint64_t msg_count[NUM_NODES][NUM_NODES]={0};

};
}
}

#endif /* _NWS_H */
