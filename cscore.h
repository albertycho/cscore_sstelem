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
#include <deque>
#include <forward_list>
#include <memory>

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
#include "address_map.h"

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
            { "cxl_config", "Deprecated (unused): pool routing is controlled by address_map_config", "" },
            { "cxl_outstanding_limit", "Deprecated (unused): pool backpressure is modeled by link queues", "32" },
            { "address_map_config", "Path to CSV mapping address ranges to socket/pool (node_id,start,size,type,target; node_id matches component node_id)", "" },
            { "dram_size_bytes", "Physical DRAM size for VA->PA mapping (must be > 1 MiB)", "1073741824" },
            { "pool_pa_base", "Base physical address for pool mapping (0 means use dram_size_bytes)", "0" },
            { "cache_heartbeat_period", "Cycles between cache stats prints (0 disables)", "1000" }
            
        )
        SST_ELI_DOCUMENT_PORTS(
            {"port_handler_FABRIC", "Fabric link for off-socket traffic.", { "cscore.csimCore", ""} }
        )
        

        private:
        csimCore();  // for serialization only
        csimCore(const csimCore&); // do not implement
        void operator=(const csimCore&); // do not implement
    
        virtual bool champsim_tick(SST::Cycle_t);
    
        // virtual void Oneshot1Callback(uint32_t);
        // virtual void Oneshot2Callback();
    
        // Carwash member variables and functions    
    
        std::string clock_frequency_str;
        std::string trace_name;
        int node_id;
        SST::Link* linkHandler_FABRIC = nullptr;
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
        //MEMORY_CONTROLLER DRAM;
        MY_MEMORY_CONTROLLER MYDRAM;
        VirtualMemory vmem;
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

        using picoseconds = std::chrono::duration<std::intmax_t, std::pico>;
        using duration=picoseconds;
        champsim::chrono::clock global_clock;
        duration time_quantum;

        //DBG
        uint64_t fetched_insts=0;
        uint64_t heartbeat_count=0;
        uint64_t cache_heartbeat_period=1000;
        uint64_t pool_pa_base=0;
        AddressMap address_map;
        std::string address_map_path;
        std::deque<sst_request> remote_outbox;

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
        bool enqueue_remote_request(const sst_request& req);
        void advance_remote_requests();
    
        void handleEvent_FABRIC(SST::Event *ev);
    };
    
    } // namespace cscore
    } // namespace SST
    
    #endif /* _CSCORE_H */
