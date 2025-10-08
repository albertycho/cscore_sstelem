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
#include <vcpkg_installed/x64-linux/include/fmt/chrono.h>
#include <vcpkg_installed/x64-linux/include/fmt/core.h>
#include <vcpkg_installed/x64-linux/include/fmt/ranges.h>

#include <stats_printer.h>
#include <fstream>
#include <memory>

const auto start_time = std::chrono::steady_clock::now();

std::chrono::seconds elapsed_time() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time); }


namespace SST {
	namespace csimCore {
	
		csimCore::csimCore(ComponentId_t id, Params& params) : Component(id),
			//DRAM(champsim::chrono::picoseconds{500}, champsim::chrono::picoseconds{1000}, std::size_t{24}, std::size_t{24}, std::size_t{24}, std::size_t{52}, champsim::chrono::microseconds{32000}, {&channels.at(1)}, 64, 64, 1, champsim::data::bytes{8}, 65536, 1024, 1, 8, 4, 8192),
			MYDRAM(champsim::chrono::picoseconds{500}, {&channels.at(1)}),
			vmem(champsim::data::bytes{4096}, 5, champsim::chrono::picoseconds{500*200}, MYDRAM, 1)
		{
			/* This function sets up and builds core (and cache and bp and etc) */
			std::cout<<"Starting cscore constructor"<<std::endl;
			//std::cout<<"MYDRAM size:"<<MYDRAM.size()<<std::endl;

			// SST variable initialization

			clock_frequency_str = params.find<std::string>("clock", "2GHz");

			trace_name = params.find<std::string>("trace_name", "example_tracename.xz");
			send_trace_name = params.find<std::string>("send_trace_name", "/scratch/acho44/DIST_CXL/TRACES/SendRecv/250228/sendI_2891634_0_110.trace.xz");
			recv_trace_name = params.find<std::string>("recv_trace_name", "/scratch/acho44/DIST_CXL/TRACES/SendRecv/250228/recv_2891635_0_120.trace.xz");
			outfile_name = params.find<std::string>("output_file", "~/SST_TEST/sst-elements/src/sst/elements/cscore/tests/asdfasdf2.out");
			node_id = params.find<int64_t>("node_id", 0);
			cpuid = params.find<int64_t>("cpu_id", 0);
			uint64_t warmup_insts=params.find<int64_t>("warmup_insts", 5000);
			uint64_t sim_insts=params.find<int64_t>("sim_insts", 50000);

			// Older version registered this as primary component
			registerAsPrimaryComponent();
		    primaryComponentDoNotEndSim();
			

			tmp_instr.msgSize=0;

			std::cout<<"trace_name: "<<trace_name<<std::endl;
			std::vector<std::string> trace_names;
			trace_names.push_back(trace_name);
			// std::transform(
			// 	std::begin(trace_names), std::end(trace_names), std::back_inserter(traces),
			// 	[i = uint8_t(0)](auto name) mutable { return get_tracereader(name, i++, true, true); });
			traces.push_back(get_tracereader(trace_name, 0, false, true));

			
			std::cout<<"traces.size(): "<<traces.size()<<std::endl;
			// std::transform(
			// std::begin(trace_names), std::end(trace_names), std::back_inserter(traces),
			// [false, repeat = true, i = uint8_t(0)](auto name) mutable { return get_tracereader(name, i++, false, repeat); });


			/* Component initialization */ 

			/* channels instantiated in .h, as we need them for DRAM and vmem init from calling this constructor */

			// channels = {
			// 	champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1},
			// 	champsim::channel{std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(), champsim::data::bits{champsim::lg2(BLOCK_SIZE)}, 0},
			// 	champsim::channel{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0},
			// 	champsim::channel{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0},
			// 	champsim::channel{32, 16, 32, champsim::data::bits{champsim::lg2(64)}, 0},
			// 	champsim::channel{32, 16, 32, champsim::data::bits{champsim::lg2(64)}, 0},
			// 	champsim::channel{32, 32, 32, champsim::data::bits{champsim::lg2(64)}, 0},
			// 	champsim::channel{16, 0, 0, champsim::data::bits{champsim::lg2(PAGE_SIZE)}, 0},
			// 	champsim::channel{16, 0, 16, champsim::data::bits{champsim::lg2(4096)}, 1},
			// 	champsim::channel{16, 0, 16, champsim::data::bits{champsim::lg2(4096)}, 1},
			// 	champsim::channel{32, 0, 32, champsim::data::bits{champsim::lg2(4096)}, 0},
			// 	champsim::channel{64, 32, 64, champsim::data::bits{champsim::lg2(64)}, 1},
			// 	champsim::channel{64, 8, 64, champsim::data::bits{champsim::lg2(64)}, 1}
        	// };

			//DRAM(champsim::chrono::picoseconds{500}, champsim::chrono::picoseconds{1000}, std::size_t{24}, std::size_t{24}, std::size_t{24}, std::size_t{52}, champsim::chrono::microseconds{32000}, {&channels.at(1)}, 64, 64, 1, champsim::data::bytes{8}, 65536, 1024, 1, 8, 4, 8192);

			/* DRAM and vmem initialized from constructor call */

			/* Populating PTWS */

			auto ptw0builder = champsim::ptw_builder{ champsim::defaults::default_ptw }
			.name("cpu0_PTW")
			.upper_levels({&channels.at(7)})
			.virtual_memory(&vmem)
			.cpu(0)
			.lower_level(&channels.at(0))
			.mshr_size(5)
			.tag_bandwidth(champsim::bandwidth::maximum_type{2})
			.fill_bandwidth(champsim::bandwidth::maximum_type{2})
			.clock_period(champsim::chrono::picoseconds{500})
			.add_pscl(5, 1, 2)
			.add_pscl(4, 1, 4)
			.add_pscl(3, 2, 4)
			.add_pscl(2, 4, 8);
			ptws.push_back(PageTableWalker(ptw0builder));
			
			/* DONE Populating PTWS */


			/* Populating CACHES */
			
			auto llc_builder = champsim::cache_builder{ champsim::defaults::default_llc }
				.name("LLC")
				.upper_levels({&channels.at(6)})
				.sets(2048)
				.ways(16)
				.pq_size(32)
				.mshr_size(64)
				.latency(20)
				.tag_bandwidth(champsim::bandwidth::maximum_type{1})
				.fill_bandwidth(champsim::bandwidth::maximum_type{1})
				.offset_bits(champsim::data::bits{champsim::lg2(64)})
				.prefetch_activate(access_type::LOAD, access_type::PREFETCH)
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_level(&channels.at(1))
				.clock_period(champsim::chrono::picoseconds{500})
				.reset_prefetch_as_load()
				.reset_virtual_prefetch();
			caches.push_back(CACHE(llc_builder));

			auto dtlb_builder = champsim::cache_builder{ champsim::defaults::default_dtlb }
				.name("cpu0_DTLB")
				.upper_levels({&channels.at(8)})
				.sets(16)
				.ways(4)
				.pq_size(0)
				.mshr_size(8)
				.latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{2})
				.fill_bandwidth(champsim::bandwidth::maximum_type{2})
				.offset_bits(champsim::data::bits{champsim::lg2(4096)})
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_level(&channels.at(2))
				.clock_period(champsim::chrono::picoseconds{500})
				.reset_prefetch_as_load();
			caches.push_back(CACHE(dtlb_builder));

			auto itlb_builder = champsim::cache_builder{ champsim::defaults::default_itlb }
				.name("cpu0_ITLB")
				.upper_levels({&channels.at(9)})
				.sets(16)
				.ways(4)
				.pq_size(0)
				.mshr_size(8)
				.latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{2})
				.fill_bandwidth(champsim::bandwidth::maximum_type{2})
				.offset_bits(champsim::data::bits{champsim::lg2(4096)})
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_level(&channels.at(3))
				.clock_period(champsim::chrono::picoseconds{500})
				.reset_prefetch_as_load();
			caches.push_back(CACHE(itlb_builder));

			auto l1d_builder = champsim::cache_builder{ champsim::defaults::default_l1d }
				.name("cpu0_L1D")
				.upper_levels({{&channels.at(0), &channels.at(12)}})
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
				.lower_translate(&channels.at(8))
				.lower_level(&channels.at(4))
				.clock_period(champsim::chrono::picoseconds{500})
				.reset_prefetch_as_load()
				.reset_virtual_prefetch();
			caches.push_back(CACHE(l1d_builder));

			auto l1i_builder = champsim::cache_builder{ champsim::defaults::default_l1i }
				.name("cpu0_L1I")
				.upper_levels({&channels.at(11)})
				.sets(64)
				.ways(8)
				.pq_size(32)
				.mshr_size(8)
				.latency(4)
				.tag_bandwidth(champsim::bandwidth::maximum_type{2})
				.fill_bandwidth(champsim::bandwidth::maximum_type{2})
				.offset_bits(champsim::data::bits{champsim::lg2(64)})
				.prefetch_activate(access_type::LOAD, access_type::PREFETCH)
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_translate(&channels.at(9))
				.lower_level(&channels.at(5))
				.clock_period(champsim::chrono::picoseconds{500})
				.reset_prefetch_as_load()
				.set_virtual_prefetch();
			caches.push_back(CACHE(l1i_builder));

			auto l2c_builder = champsim::cache_builder{ champsim::defaults::default_l2c }
				.name("cpu0_L2C")
				.upper_levels({{&channels.at(4), &channels.at(5)}})
				.sets(1024)
				.ways(8)
				.pq_size(16)
				.mshr_size(32)
				.latency(10)
				.tag_bandwidth(champsim::bandwidth::maximum_type{1})
				.fill_bandwidth(champsim::bandwidth::maximum_type{1})
				.offset_bits(champsim::data::bits{champsim::lg2(64)})
				.prefetch_activate(access_type::LOAD, access_type::PREFETCH)
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_translate(&channels.at(10))
				.lower_level(&channels.at(6))
				.clock_period(champsim::chrono::picoseconds{500})
				.reset_prefetch_as_load()
				.reset_virtual_prefetch();
			caches.push_back(CACHE(l2c_builder));

			auto stlb_builder = champsim::cache_builder{ champsim::defaults::default_stlb }
				.name("cpu0_STLB")
				.upper_levels({{&channels.at(2), &channels.at(3), &channels.at(10)}})
				.sets(128)
				.ways(12)
				.pq_size(0)
				.mshr_size(16)
				.latency(8)
				.tag_bandwidth(champsim::bandwidth::maximum_type{1})
				.fill_bandwidth(champsim::bandwidth::maximum_type{1})
				.offset_bits(champsim::data::bits{champsim::lg2(4096)})
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_level(&channels.at(7))
				.clock_period(champsim::chrono::picoseconds{500})
				.reset_prefetch_as_load();
			caches.push_back(CACHE(stlb_builder));
			
			std::cout<<"Done instantiating caches"<<std::endl;

			/* DONE Populating CACHES */

			/* Populating CORES */

			auto o3corebuilder = champsim::core_builder{ champsim::defaults::default_core }
			.ifetch_buffer_size(64)
			.decode_buffer_size(32)
			.dispatch_buffer_size(32)
			.register_file_size(128)
			.rob_size(352)
			.lq_size(128)
			.sq_size(72)
			.fetch_width(champsim::bandwidth::maximum_type{6})
			.decode_width(champsim::bandwidth::maximum_type{6})
			.dispatch_width(champsim::bandwidth::maximum_type{6})
			.schedule_width(champsim::bandwidth::maximum_type{128})
			.execute_width(champsim::bandwidth::maximum_type{4})
			.lq_width(champsim::bandwidth::maximum_type{2})
			.sq_width(champsim::bandwidth::maximum_type{2})
			.retire_width(champsim::bandwidth::maximum_type{5})
			.mispredict_penalty(1)
			.decode_latency(1)
			.dispatch_latency(1)
			.schedule_latency(0)
			.execute_latency(0)
			.l1i(&(*std::next(std::begin(caches), 4)))
			.l1i_bandwidth((*std::next(std::begin(caches), 4)).MAX_TAG)
			.fetch_queues(&channels.at(11))
			.l1d_bandwidth((*std::next(std::begin(caches), 3)).MAX_TAG)
			.data_queues(&channels.at(12))
			.branch_predictor<class bimodal>()
			.btb<class basic_btb>()
			.index(0)
			.clock_period(champsim::chrono::picoseconds{500})
			.dib_set(32)
			.dib_way(8)
			.dib_window(16);

			cores.push_back(O3_CPU(o3corebuilder));
			
			/* DONE Populating CORES */

			//MYDRAM init?
			//DRAM.initialize();
			//DRAM.warmup=false;

			// for (auto ptw : ptws){
			// 	ptw.initialize();
			// }
			ptws.front().initialize();
			ptws.front().warmup=false;
			for (CACHE& cache_c : caches){
				cache_c.initialize();
				cache_c.begin_phase();
				cache_c.warmup=false;
			}

			//std::shared_ptr<std::ofstream> 
			heartbeat_file = std::make_shared<std::ofstream>("heartbeat_Node" + std::to_string(node_id) + ".log");
			for (O3_CPU& core : cores){
				core.initialize();
				core.warmup=false;
				std::cout<<"core heartbeat true? "<<core.show_heartbeat<<std::endl;
				// what if multiple cores? still want them to write to the same file.
				//core.heartbeat_file.open("heartbeat_cpu" + std::to_string(node_id) + ".log");
				core.heartbeat_file=heartbeat_file;
			}

			time_quantum = std::accumulate(std::cbegin(cores), std::cend(cores), champsim::chrono::clock::duration::max(),
                                            [](const auto acc, const O3_CPU& y) { return std::min(acc, y.clock_period); });
				//std::cout << "time quantum: " << fmt::format("{}", time_quantum) << std::endl;
				//std::cout << "time quantum: " << time_quantum.count() <<" picoseconds"<< std::endl;

			
			linkHandler_NW = configureLink("port_handler_NW", new Event::Handler<csimCore>(this, &csimCore::handleEvent_NW));
			if (!linkHandler_NW) {
				std::cerr << "ERROR: linkHandler_NW is NULL for node " << node_id << std::endl;
			}
			registerClock(clock_frequency_str, new Clock::Handler<csimCore>(this,
				&csimCore::champsim_tick));	

			std::cout<<"Node "<<node_id<<": Done with cscore constructor"<<std::endl;
			
		}
		
		bool csimCore::champsim_tick(Cycle_t){

			//std::cout<<"CSCORE_tick @ "<<node_id<<": Entered champsim_tick function"<<std::endl;
			//*heartbeat_file<<"CSCORE_tick @ "<<node_id<<": Entered champsim_tick function"<<std::endl;
			
			//std::cout<<"ptw current_cycle at the beginning of  champsim_tick: "<<ptws.front().current_cycle()<<std::endl;
			//std::cout<<"O3Core current_cycle at the beginning of  champsim_tick: "<<cores.front().current_cycle()<<std::endl;

			//ptws.front().set_current_clock(global_clock);

			global_clock.tick(time_quantum);
			heartbeat_count++;
			

			/* OPERABLES:  DRAM, ptws, caches, cores*/

			// for (champsim::channel& chan : channels){
			// }

			//DRAM.operate();
			//DRAM.operate_on(global_clock);
			MYDRAM.operate_on(global_clock);

			// for (auto ptw : ptws){
			// 	//ptw.operate();
			// 	std::cout<<"ptw current_cycle before operate_on: "<<ptw.current_cycle()<<std::endl;
			// 	//ptw.set_current_clock(global_clock);
			// 	//std::cout<<"ptw current_cycle after set_current_clock operate_on: "<<ptw.current_cycle()<<std::endl;
			// 	ptw.operate_on(global_clock);
			// 	std::cout<<"ptw current_cycle after operate_on: "<<ptws.front().current_cycle()<<std::endl;
			// 	std::cout<<"ptw mshr size: "<<ptw.MSHR_SIZE<<std::endl;
			// }
			
			//std::cout<<"ptw current_cycle before operate_on: "<<ptws.front().current_cycle()<<std::endl;
			//ptw.set_current_clock(global_clock);
			//std::cout<<"ptw current_cycle after set_current_clock operate_on: "<<ptw.current_cycle()<<std::endl;
			ptws.front().operate_on(global_clock);
			//std::cout<<"ptw current_cycle after operate_on: "<<ptws.front().current_cycle()<<std::endl;
			//std::cout<<"ptw mshr size: "<<ptws.front().MSHR_SIZE<<std::endl;
			//std::cout<<"ptw current_cycle after ptw operated: "<<ptws.front().current_cycle()<<std::endl;

			for (CACHE& cache_c : caches){
				//cache_c.operate();
				cache_c.operate_on(global_clock);
				//std::cout<<"cache stat name: "<<cache_c.sim_stats.name <<", cache total miss latency: "<<cache_c.sim_stats.total_miss_latency_cycles <<std::endl;
				//<< "cache_total misses:"<< cache_c.sim_stats.misses <<std::endl;
				//std::cout<<"cache_total misses:"<< cache_c.sim_stats.misses<<std::endl;
				
				if(heartbeat_count % 1000 ==0){
					// auto cache_formats = champsim::plain_printer{std::cout}.format(cache_c.sim_stats);
					// for (const auto& line : cache_formats) {
					// 	std::cout << line << '\n';
					// }
					// auto cache_formats = champsim::plain_printer{std::cout}.format(cache_c.sim_stats);
					// for (const auto& line : cache_formats) {
					// 	std::cout << line << '\n';
					// }
					auto cache_formats = champsim::plain_printer{*heartbeat_file}.format(cache_c.sim_stats);
					for (const auto& line : cache_formats) {
						*heartbeat_file << line << '\n';
					}
					heartbeat_file->flush(); // Optional: ensure output is written immediately
				}
				
			}

			uint8_t curr_core_id=0;
			for (O3_CPU& cpu : cores){
				//cpu.operate();
				cpu.operate_on(global_clock);
				//std::cout<<"core retired_insts: "<<cpu.num_retired<<std::endl;

				if(!cpu.egress_buffer.empty()){
					// get the egress packet and send it
					NW_packet_t outpacket = get_egress_packet(curr_core_id);
					send_NW_packet(outpacket);
				}

				auto& trace = traces.at(0); // TODO change if multiple cores
				for (auto pkt_count = cpu.IN_QUEUE_SIZE - static_cast<long>(std::size(cpu.input_queue)); !trace.eof() && pkt_count > 0; --pkt_count) {
					cpu.input_queue.push_back(trace());
					fetched_insts++;
					//std::cout<<"input queue size: "<<cpu.input_queue.size()<<std::endl;
				}
				if(trace.eof()){
					std::cout<<"CSCORE_tick: trace.eof() returned true! Reached end of trace in core tick function: "<< trace_name <<std::endl;
					//trace = get_tracereader(trace_name, 0, false, true);
				}
				//std::cout<<"input queue size this cycle: "<<cpu.input_queue.size()<<std::endl;
				//std::cout<<"fetched_insts: "<<fetched_insts<<std::endl;

				curr_core_id++;
			}

			//std::cout<<"ptw current_cycle after champsim_tick completed: "<<ptws.front().current_cycle()<<std::endl;
			// if(heartbeat_count % 100000 == (node_id+1*100)){
			// 	send_NW_packet();
			// }

			return false;
		}

		bool csimCore::send_NW_packet(NW_packet_t outpacket){
			//NW_packet_t outpacket = csst.get_egress_packet();
			//NW_packet_t outpacket;
			// int dest_id=0;
			// if(node_id==0){
			// 	dest_id=1;
			// }
			// else{
			// 	dest_id=0;
			// }
			int dest_id= outpacket.receiver_core_id;
			// TODO for dummy testing, setting dest_id to 0 for all packets for now
			//if(dest_id>3)dest_id=0; // TODO remove this line later
			cycle_count++;
			std::cout<<"Node "<<node_id<<" has outgoing packet to "<< dest_id<<std::endl;
			*heartbeat_file<<"Node "<<node_id<<" has outgoing packet to "<< dest_id<<std::endl;
			csEvent * nev = new csEvent();
			nev->payload.push_back(node_id);
			nev->payload.push_back(dest_id);
			nev->payload.push_back(cycle_count);
			// std::cout<<"Node"<<node_id <<" at send_NW_packet: packet's event_cycle: "<<cycle_count<<", SST's curcycle: "<<cycle_count<<std::endl;
			// linkHandler_NW->send(nev);
			// std::cout<<"Node"<<node_id<<" linkhandler_nw->send() succeeded"<<std::endl;

			std::cout << "Node" << node_id << " sending event at " << cycle_count << std::endl;
			// << nev << " with payload size " << nev->payload.size() << std::endl;
			linkHandler_NW->send(nev);
			std::cout << "Node" << node_id << " linkHandler_NW->send() returned" << std::endl;
			*heartbeat_file << "Node" << node_id << " linkHandler_NW->send() returned" << std::endl;
			return true;
		}

		void csimCore::handleEvent_NW(SST::Event *ev)
		{
			csEvent *cevent = dynamic_cast<csEvent*>(ev);
			if(cevent){
				//std::cout<<"node "<<node_id<<" recieved csEvent @ cycle "<<cycle_count<<"from node"<<cevent->payload[0] <<std::endl;
				NW_packet_t inpacket;
				inpacket.sender_core_id=cevent->payload[0];
				inpacket.receiver_core_id = cevent->payload[1];
				inpacket.instr_id=0;
				inpacket.payload=0;
				inpacket.received_cycle= cycle_count;
				inpacket.event_cycle = cevent->payload[2];
				
				// TODO implement ingress buffer injection to core
				//NW_ingress_buffer.push_back(inpacket);

				std::cout<<"Node"<< node_id<<" handleEvent_NW recieved packet from NW, from Node "<<inpacket.sender_core_id<<", took " <<(cycle_count - inpacket.event_cycle) <<"cycles (sent at cycle "<<inpacket.event_cycle<<")"<<std::endl;
				*heartbeat_file<<"Node"<< node_id<<" handleEvent_NW recieved packet from NW, from Node "<<inpacket.sender_core_id<<", took " <<(cycle_count - inpacket.event_cycle) <<"cycles (sent at cycle "<<inpacket.event_cycle<<")"<<std::endl;
				
				delete(cevent);
			}
			else{
				std::cout<<"cevent NULL"<<std::endl;
			}
			return;
		}

		NW_packet_t csimCore::get_egress_packet(uint32_t core_id){
			if(cores[core_id].egress_buffer.empty()){
				*(heartbeat_file) << "Expected egress packet but egress buffer is empty!"<<std::endl;
				NW_packet_t dummypacket;
				return dummypacket;
			}
			NW_packet_t outpacket = *(cores[core_id].egress_buffer.begin());
			cores[core_id].egress_buffer.erase(cores[core_id].egress_buffer.begin());
			if(cores[core_id].egress_buffer.empty()){ // unset egress check flag if no packets left
				cores[core_id].egress_check=false;
			}
			return outpacket;
		}
			

	} // namespace csimCore
} // namespace SST
