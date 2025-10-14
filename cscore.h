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
// #include <cache.h>
// #include <ooo_cpu.h>

#include <functional>
#include <vector>

#include "cache.h"
#include "dram_controller.h"
#include "my_memory_controller.h"
#include "ooo_cpu.h"
#include "operable.h"
#include "ptw.h"
#include "bimodal/bimodal.h"

#include "tracereader.h"
//#include <environment.h>
#include "defaults.hpp"
#include "vmem.h"

#include "convert_ev_packet.h"

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

        // this was there for early debug. probably should remove
        input_instr tmp_instr;


        private:

       std::vector<champsim::channel> channels {
				champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1}, // 0
				champsim::channel{std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(), champsim::data::bits{champsim::lg2(BLOCK_SIZE)}, 0},
				champsim::channel{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0},
				champsim::channel{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0},
				champsim::channel{32, 16, 32, champsim::data::bits{champsim::lg2(64)}, 0}, // 4
				champsim::channel{32, 16, 32, champsim::data::bits{champsim::lg2(64)}, 0},
				champsim::channel{32, 32, 32, champsim::data::bits{champsim::lg2(64)}, 0},
				champsim::channel{16, 0, 0, champsim::data::bits{champsim::lg2(PAGE_SIZE)}, 0},
				champsim::channel{16, 0, 16, champsim::data::bits{champsim::lg2(4096)}, 1}, // 8
				champsim::channel{16, 0, 16, champsim::data::bits{champsim::lg2(4096)}, 1},
				champsim::channel{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0},
				champsim::channel{64, 32, 64, champsim::data::bits{champsim::lg2(64)}, 1},
				champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1} // 12
        	};
        VirtualMemory vmem;
        //MEMORY_CONTROLLER DRAM;
        MY_MEMORY_CONTROLLER MYDRAM;
        //MEMORY_CONTROLLER DRAM = MEMORY_CONTROLLER(champsim::chrono::picoseconds{500}, champsim::chrono::picoseconds{1000}, std::size_t{24}, std::size_t{24}, std::size_t{24}, std::size_t{52}, champsim::chrono::microseconds{32000}, {&channels.at(1)}, 64, 64, 1, champsim::data::bytes{8}, 65536, 1024, 1, 8, 4, 8192);
        //MEMORY_CONTROLLER DRAM(champsim::chrono::picoseconds{500}, champsim::chrono::picoseconds{1000}, std::size_t{24}, std::size_t{24}, std::size_t{24}, std::size_t{52}, champsim::chrono::microseconds{32000}, {&channels.at(1)}, 64, 64, 1, champsim::data::bytes{8}, 65536, 1024, 1, 8, 4, 8192);
        
        //std::forward_list<PageTableWalker> ptws;
        std::vector<PageTableWalker> ptws;
        //std::forward_list<CACHE> caches;
        std::vector<CACHE> caches;
        //std::forward_list<O3_CPU> cores;
        std::vector<O3_CPU> cores;

        std::vector<champsim::tracereader> traces;

        std::shared_ptr<std::ofstream> heartbeat_file;

        constexpr static std::size_t num_cpus = 1;
        constexpr static std::size_t block_size = 64;
        constexpr static std::size_t page_size = 4096;

        using picoseconds = std::chrono::duration<std::intmax_t, std::pico>;
        using duration=picoseconds;
        champsim::chrono::clock global_clock;
        duration time_quantum;

        //DBG
        uint64_t fetched_insts=0;
        uint64_t heartbeat_count=0;
        std::vector<NW_packet_t> egress_buffer;
        std::vector<NW_packet_t> ingress_buffer;

        // champsim::channel cpu0_STLB_to_cpu0_PTW_queues{16, 0, 0, champsim::data::bits{champsim::lg2(PAGE_SIZE)}, 0};
        // champsim::channel cpu0_DTLB_to_cpu0_STLB_queues{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0};
        // champsim::channel cpu0_ITLB_to_cpu0_STLB_queues{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0};
        // champsim::channel cpu0_L2C_to_cpu0_STLB_queues{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0};
        // champsim::channel cpu0_L1D_to_cpu0_L2C_queues{32, 16, 32, champsim::data::bits{champsim::lg2(64)}, 0};
        // champsim::channel cpu0_L1I_to_cpu0_L2C_queues{32, 16, 32, champsim::data::bits{champsim::lg2(64)}, 0};
        // champsim::channel cpu0_to_cpu0_L1I_queues{64, 32, 64, champsim::data::bits{champsim::lg2(64)}, 1};
        // champsim::channel cpu0_PTW_to_cpu0_L1D_queues{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1};
        // champsim::channel cpu0_to_cpu0_L1D_queues{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1};
        // champsim::channel cpu0_L1I_to_cpu0_ITLB_queues{16, 0, 16, champsim::data::bits{champsim::lg2(4096)}, 1};
        // champsim::channel cpu0_L1D_to_cpu0_DTLB_queues{16, 0, 16, champsim::data::bits{champsim::lg2(4096)}, 1};
        //CACHE* tmp_cache;
        //std::vector<CACHE> caches;
        //std::forward_list<CACHE> caches;
        //champsim::configured::generated_environment<0x05899e0703813be6> gen_environment{};
    
        public:
        bool send_NW_packet(NW_packet_t outpacket);
        bool send_CXL_req();
        bool send_LLC_req();

        NW_packet_t get_egress_packet(uint32_t core_id);
    
        void handleEvent_NW(SST::Event *ev);
        void dummyhandleEvent_CXL(SST::Event *ev);
        void HandleEvent_LLC(SST::Event *ev);
    };
    
    } // namespace cscore
    } // namespace SST
    
    #endif /* _CSCORE_H */
