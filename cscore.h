// Copyright 2009-2024 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2024, NTESS
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef _CSCORE_H
#define _CSCORE_H

#include <sst/core/component.h>
#include <sst/core/link.h>
#include <iostream>
#include <fstream>
#include <forward_list>

#include <trace_instruction.h>
#include <channel.h>
#include <cache.h>

namespace SST {
    namespace csimCore {
    
    class csimCore : public SST::Component
    {
    public:
        csimCore(SST::ComponentId_t id, SST::Params& params);		// Constructor
    
        //void setup()  { }
        //void finish() { }
    
        SST_ELI_REGISTER_COMPONENT(
            csimCore,
            "cscore",
            "csimCore",
            SST_ELI_ELEMENT_VERSION(1,0,0),
            "Champsim Core Componnent",
        COMPONENT_CATEGORY_UNCATEGORIZED
        )
    
        SST_ELI_DOCUMENT_PARAMS(
            { "clock", "Clock frequency", "1GHz" },
            { "clockcount", "Number of clock ticks to execute", "100000" },
            { "trace_name", "Path to input trace file", ""},
            { "send_trace_name", "Path to send trace file", ""},
            { "recv_trace_name", "Path to recv trace file", ""},
            { "output_file", "Path to output file", ""}
            
        )
        SST_ELI_DOCUMENT_PORTS(
            {"port_handler_NW",    "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.csimCore", ""} },
            //{"port_handler_CXL",    "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.csimCore", ""} },
            {"port_handler_LLC",    "Link to another component. This port uses an event handler to capture incoming events.", { "cscore.csimCore", ""} }
        )
        

        private:
        csimCore();  // for serialization only
        csimCore(const csimCore&); // do not implement
        void operator=(const csimCore&); // do not implement
    
        virtual bool champsim_tick(SST::Cycle_t);
    
        // virtual void Oneshot1Callback(uint32_t);
        // virtual void Oneshot2Callback();
    
        // Carwash member variables and functions    
    
        TimeConverter*      tc;
        //Clock::HandlerBase* Clock3Handler;
    
        // Variables to store OneShot Callback Handlers
        // OneShot::HandlerBase* callback1Handler;
        // OneShot::HandlerBase* callback2Handler;
    
        std::string clock_frequency_str;
        int clock_count;
        std::string trace_name;
        std::string send_trace_name;
        std::string recv_trace_name;
        std::string outfile_name;
        std::ofstream outf;
        std::streambuf* coutBuf;
        int node_id;
        int cpuid;
        SST::Link* linkHandler_NW;
        // SST::Link* linkHandler_CXL;
        SST::Link* linkHandler_LLC;
        int cycle_count;
        int NW_BW_counter_in;
        int NW_BW_counter_out;
        //std::deque<NW_packet_t> NW_ingress_buffer;
        //champsim::csim_sst *csst;
        //champsim::csim_sst csst;

        input_instr tmp_instr;
        //champsim::channel tmp_chan;
        std::vector<champsim::channel> channels={
        			champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1},
					champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1},
					champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 0},
					champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 0}
                };
        //CACHE* tmp_cache;
        std::vector<CACHE> caches;
        //std::forward_list<CACHE> caches;
    
        bool send_NW_packet();
        bool send_CXL_req();
        bool send_LLC_req();
    
        void dummyhandleEvent_NW(SST::Event *ev);
        void dummyhandleEvent_CXL(SST::Event *ev);
        void HandleEvent_LLC(SST::Event *ev);
    };
    
    } // namespace cscore
    } // namespace SST
    
    #endif /* _CSCORE_H */
