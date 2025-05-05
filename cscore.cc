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

//#include <assert.h>

#include "sst_config.h"
#include "cscore.h"
#include <vcpkg/installed/x64-linux/include/fmt/chrono.h>
#include <vcpkg/installed/x64-linux/include/fmt/core.h>
#include <vcpkg/installed/x64-linux/include/fmt/ranges.h>




namespace SST {
	namespace csimCore {
	
		csimCore::csimCore(ComponentId_t id, Params& params) : Component(id)
		{
			clock_frequency_str = params.find<std::string>("clock", "2GHz");

			trace_name = params.find<std::string>("trace_name", "example_tracename.xz");
			send_trace_name = params.find<std::string>("send_trace_name", "/scratch/acho44/DIST_CXL/TRACES/SendRecv/250228/sendI_2891634_0_110.trace.xz");
			recv_trace_name = params.find<std::string>("recv_trace_name", "/scratch/acho44/DIST_CXL/TRACES/SendRecv/250228/recv_2891635_0_120.trace.xz");
			outfile_name = params.find<std::string>("output_file", "~/SST_TEST/sst-elements/src/sst/elements/cscore/tests/asdfasdf2.out");
			node_id = params.find<int64_t>("node_id", 0);
			cpuid = params.find<int64_t>("cpu_id", 0);
			uint64_t warmup_insts=params.find<int64_t>("warmup_insts", 5000);
			uint64_t sim_insts=params.find<int64_t>("sim_insts", 50000);

			tmp_instr.msgSize=0;
			
			registerClock(clock_frequency_str, new Clock::Handler<csimCore>(this,
				&csimCore::champsim_tick));	
		}
		
		bool csimCore::champsim_tick(Cycle_t){
			tmp_instr.msgSize++;
			std::cout<<"oh hello @ champsim_tick, msgSize="<<tmp_instr.msgSize<<std::endl;
			return false;
		}
			

	} // namespace csimCore
} // namespace SST
