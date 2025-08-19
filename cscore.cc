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


			//std::vector<champsim::channel> channels;
			// channels={champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1},
			// 		champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1},
			// 		champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1},
			// 		champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1}};


			champsim::cache_builder builder;
			builder.name("L1D")
			.upper_levels({{&channels.at(2), &channels.at(3)}}) //TODO: match to correct channel
			.lower_level(&channels.at(0)) //TODO: match to correct channel
			.sets(64)
			.ways(12)
			.pq_size(8)
			.mshr_size(16)
			.latency(5)
			.tag_bandwidth(champsim::bandwidth::maximum_type{2})
			.fill_bandwidth(champsim::bandwidth::maximum_type{2})
			.offset_bits(champsim::data::bits{champsim::lg2(64)})
			.prefetch_activate(access_type::LOAD, access_type::PREFETCH)
			.replacement<class lru>()
			.prefetcher<class no>()
			.lower_translate(&channels.at(1)) //TODO: match to correct channel

			.clock_period(champsim::chrono::picoseconds{500})
			.reset_prefetch_as_load()
			.reset_virtual_prefetch();			

			// // alone
			// CACHE mycache(builder);
			// size_t my_mshr_size=mycache.get_mshr_size();
			// std::cout<<"mshr_size: "<<my_mshr_size<<std::endl;

			std::cout<<"builder.m_ll: "<<builder.m_ll<<std::endl;
			std::cout<<"builder.m_uls[0]: "<<builder.m_uls[0]<<std::endl;
			std::cout<<"builder.m_latency: "<<*(builder.m_latency)<<std::endl;
			std::cout<<"builder.clock_period: "<<*(builder.m_clock_period.count())<<std::endl;
			

			std::cout<<"hello before cache init"<<std::endl;
			// for vector:
			caches.push_back(CACHE(builder));
			auto my_mshr_size = caches[0].get_mshr_size();
			std::cout<<"mshr_size="<<my_mshr_size<<std::endl;

			uint64_t lower_level_ptr_addr = (uint64_t) (caches[0].lower_level);
			uint64_t channel_0_ptr_addr = (uint64_t) (&(channels[0])); 
			std::cout<<"in init: lower_level_addr: "<<lower_level_ptr_addr<<", channel[0]_addr: "<<channel_0_ptr_addr<<std::endl;

			// // for forwardlist
			// auto it = caches.before_begin();
			// for (auto next = caches.begin(); next != caches.end(); ++it, ++next);
			// caches.insert_after(it, CACHE(builder)); // safe insertion at the end

			// auto my_mshr_size = caches.front().get_mshr_size();
			// std::cout<<"mshr_size="<<my_mshr_size<<std::endl;

			
			registerClock(clock_frequency_str, new Clock::Handler<csimCore>(this,
				&csimCore::champsim_tick));	
		}
		
		bool csimCore::champsim_tick(Cycle_t){
			tmp_instr.msgSize++;
			std::cout<<"channels_size: "<<channels.size()<<", rq_size: "<<channels[0].rq_size()<<std::endl;
			int upper_level_rq_size = caches[0].upper_levels[0]->rq_size();
			std::cout<<"upper_level_rq_size: "<<upper_level_rq_size<<std::endl;
			uint64_t lower_level_ptr_addr = (uint64_t) (caches[0].lower_level); 
			uint64_t lower_translate_ptr_addr = (uint64_t) (caches[0].lower_translate); 
			uint64_t upper_level_ptr_addr = (uint64_t) (caches[0].upper_levels[0]); 
			uint64_t channel_0_ptr_addr = (uint64_t) (&(channels[0])); 
			uint64_t channel_2_ptr_addr = (uint64_t) (&(channels[2])); 
			uint64_t channel_1_ptr_addr = (uint64_t) (&(channels[1])); 
			std::cout<<"lower_level_addr: "<<lower_level_ptr_addr<<", channel[0]_addr: "<<channel_0_ptr_addr<<std::endl;
			std::cout<<"upper_level_addr: "<<upper_level_ptr_addr<<", channel[2]_addr: "<<channel_2_ptr_addr<<std::endl;
			std::cout<<"lower_translate_addr: "<<lower_translate_ptr_addr<<", channel[1]_addr: "<<channel_1_ptr_addr<<std::endl;
			int lower_level_rq_size = caches[0].lower_level->rq_size();
			std::cout<<"lower_level_rq_size: "<<lower_level_rq_size<<std::endl;
			//int chan_size=100;
			for (CACHE& cache_c : caches){
				std::cout<<"before call cache_c.operate()"<<std::endl;
				cache_c.operate();
				std::cout<<"after call cache_c.operate()"<<std::endl;
			}
			if(tmp_instr.msgSize%1000000==0){
				auto my_mshr_size = caches[0].get_mshr_size();
				auto my_mshr_occ = caches[0].get_mshr_occupancy();
				//std::cout<<"mshr_size="<<my_mshr_size<<std::endl;
				std::cout<<"champsim_tick, msgSize: "<<tmp_instr.msgSize<<", mshr: "<<my_mshr_occ<<"/"<<my_mshr_size <<", channel_size: "<<channels.size()<<std::endl;
			}
			return false;
		}
			

	} // namespace csimCore
} // namespace SST
