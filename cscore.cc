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

#include <stats_printer.h>
#include <stdexcept>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>

#include "trace_instruction.h"
#include "bimodal/bimodal.h"
#include "prefetcher/no/no.h"

const auto start_time = std::chrono::steady_clock::now();

std::chrono::seconds elapsed_time() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time); }

namespace {
constexpr uint64_t kClockPeriodPs = 417; // ~2.4 GHz
int64_t cycles_per_request_from_bw_bytes(uint64_t bytes_per_cycle) {
    if (bytes_per_cycle == 0) {
        return static_cast<int64_t>(DEFAULT_BW);
    }
    auto cycles = (BLOCK_SIZE + bytes_per_cycle - 1) / bytes_per_cycle;
    return static_cast<int64_t>(std::max<uint64_t>(cycles, 1));
}
int64_t resolve_dram_bw_cycles(uint64_t cycles_per_req, uint64_t bytes_per_cycle) {
    if (cycles_per_req != 0) {
        return static_cast<int64_t>(std::max<uint64_t>(cycles_per_req, 1));
    }
    return cycles_per_request_from_bw_bytes(bytes_per_cycle);
}
} // namespace


namespace SST {
	namespace csimCore {

		csimCore::csimCore(ComponentId_t id, Params& params) : Component(id),
		//DRAM(champsim::chrono::picoseconds{500}, champsim::chrono::picoseconds{1000}, std::size_t{24}, std::size_t{24}, std::size_t{24}, std::size_t{52}, champsim::chrono::microseconds{32000}, {&channels.at(1)}, 64, 64, 1, champsim::data::bytes{8}, 65536, 1024, 1, 8, 4, 8192),
		MYDRAM(champsim::chrono::picoseconds{kClockPeriodPs},
               {&channels.at(1)},
               resolve_dram_bw_cycles(
                   params.find<uint64_t>("dram_bw_cycles_per_req", 0),
                   params.find<uint64_t>("dram_bandwidth_bytes_per_cycle", 0)),
               estimate_latency_fixed,
               champsim::data::bytes{params.find<uint64_t>("dram_size_bytes", DEFAULT_DRAM_SIZE_BYTES)}),
			vmem(champsim::data::bytes{4096}, 5, champsim::chrono::picoseconds{kClockPeriodPs * 200}, MYDRAM, 1)
		{
			/* This function sets up and builds core (and cache and bp and etc) */
			std::cout<<"Starting cscore constructor"<<std::endl;
			//std::cout<<"MYDRAM size:"<<MYDRAM.size()<<std::endl;

			// SST variable initialization

			clock_frequency_str = params.find<std::string>("clock", "2.4GHz");

            trace_name = params.find<std::string>("trace_name", "example_tracename.xz");
			address_map_path = params.find<std::string>("address_map_config", "");
            node_id = params.find<int64_t>("node_id", 0);
            warmup_insts = params.find<int64_t>("warmup_insts", 0);
            sim_insts = params.find<int64_t>("sim_insts", 0);
            auto dram_size_bytes = params.find<uint64_t>("dram_size_bytes", DEFAULT_DRAM_SIZE_BYTES);
            pool_pa_base = params.find<uint64_t>("pool_pa_base", 0);
            if (pool_pa_base == 0) {
                pool_pa_base = dram_size_bytes;
            }
            if (pool_pa_base < dram_size_bytes) {
                std::cerr << "WARNING: pool_pa_base overlaps DRAM range. Forcing pool_pa_base = dram_size_bytes." << std::endl;
                pool_pa_base = dram_size_bytes;
            }
            cache_heartbeat_period = params.find<uint64_t>("cache_heartbeat_period", 1000);

			// Older version registered this as primary component
			registerAsPrimaryComponent();
		    primaryComponentDoNotEndSim();
			

			// tmp_instr.msgSize=0;

			std::cout<<"trace_name: "<<trace_name<<std::endl;
			std::vector<std::string> trace_names;
			trace_names.push_back(trace_name);
			traces.push_back(get_tracereader(trace_name, 0, false, false));

			
			std::cout<<"traces.size(): "<<traces.size()<<std::endl;
			
			/* Component initialization */ 

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
			.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
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
				.fill_latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{1})
				.fill_bandwidth(champsim::bandwidth::maximum_type{1})
				.offset_bits(champsim::data::bits{champsim::lg2(64)})
				.prefetch_activate(access_type::LOAD, access_type::PREFETCH)
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_level(&channels.at(1))
				.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
				.reset_prefetch_as_load()
				.reset_wq_checks_full_addr()
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
				.hit_latency(0)
				.fill_latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{2})
				.fill_bandwidth(champsim::bandwidth::maximum_type{2})
				.offset_bits(champsim::data::bits{champsim::lg2(4096)})
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_level(&channels.at(2))
				.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
				.reset_prefetch_as_load()
				.set_wq_checks_full_addr()
				.reset_virtual_prefetch();
			caches.push_back(CACHE(dtlb_builder));

			auto itlb_builder = champsim::cache_builder{ champsim::defaults::default_itlb }
				.name("cpu0_ITLB")
				.upper_levels({&channels.at(9)})
				.sets(16)
				.ways(4)
				.pq_size(0)
				.mshr_size(8)
				.latency(1)
				.hit_latency(0)
				.fill_latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{2})
				.fill_bandwidth(champsim::bandwidth::maximum_type{2})
				.offset_bits(champsim::data::bits{champsim::lg2(4096)})
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_level(&channels.at(3))
				.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
				.reset_prefetch_as_load()
				.set_wq_checks_full_addr()
				.set_virtual_prefetch();
			caches.push_back(CACHE(itlb_builder));

			auto l1d_builder = champsim::cache_builder{ champsim::defaults::default_l1d }
				.name("cpu0_L1D")
				.upper_levels({{&channels.at(0), &channels.at(12)}})
				.sets(64)
				.ways(12)
				.pq_size(8)
				.mshr_size(16)
				.latency(5)
				.fill_latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{2})
				.fill_bandwidth(champsim::bandwidth::maximum_type{2})
				.offset_bits(champsim::data::bits{champsim::lg2(64)})
				.prefetch_activate(access_type::LOAD, access_type::PREFETCH)
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_translate(&channels.at(8))
				.lower_level(&channels.at(4))
				.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
				.reset_prefetch_as_load()
				.set_wq_checks_full_addr()
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
				.fill_latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{2})
				.fill_bandwidth(champsim::bandwidth::maximum_type{2})
				.offset_bits(champsim::data::bits{champsim::lg2(64)})
				.prefetch_activate(access_type::LOAD, access_type::PREFETCH)
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_translate(&channels.at(9))
				.lower_level(&channels.at(5))
				.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
				.reset_prefetch_as_load()
				.set_wq_checks_full_addr()
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
				.fill_latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{1})
				.fill_bandwidth(champsim::bandwidth::maximum_type{1})
				.offset_bits(champsim::data::bits{champsim::lg2(64)})
				.prefetch_activate(access_type::LOAD, access_type::PREFETCH)
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_translate(&channels.at(10))
				.lower_level(&channels.at(6))
				.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
				.reset_prefetch_as_load()
				.reset_wq_checks_full_addr()
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
				.fill_latency(1)
				.tag_bandwidth(champsim::bandwidth::maximum_type{1})
				.fill_bandwidth(champsim::bandwidth::maximum_type{1})
				.offset_bits(champsim::data::bits{champsim::lg2(4096)})
				.replacement<class lru>()
				.prefetcher<class no>()
				.lower_level(&channels.at(7))
				.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
				.reset_prefetch_as_load()
				.reset_wq_checks_full_addr()
				.reset_virtual_prefetch();
			caches.push_back(CACHE(stlb_builder));
			
			std::cout<<"Done instantiating caches"<<std::endl;

			/* DONE Populating CACHES */

			/* Populating CORES */

			auto o3corebuilder = champsim::core_builder{ champsim::defaults::default_core }
			.ifetch_buffer_size(64)
			.decode_buffer_size(24)
			.dispatch_buffer_size(32)
			.register_file_size(4096)
			.rob_size(256)
			.lq_size(116)
			.sq_size(64)
			.fetch_width(champsim::bandwidth::maximum_type{4})
			.decode_width(champsim::bandwidth::maximum_type{4})
			.dispatch_width(champsim::bandwidth::maximum_type{6})
			.schedule_width(champsim::bandwidth::maximum_type{160})
			.execute_width(champsim::bandwidth::maximum_type{6})
			.lq_width(champsim::bandwidth::maximum_type{3})
			.sq_width(champsim::bandwidth::maximum_type{2})
			.retire_width(champsim::bandwidth::maximum_type{8})
			.dib_hit_buffer_size(0)
			.dib_inorder_width(champsim::bandwidth::maximum_type{4})
			.dib_hit_latency(0)
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
			.clock_period(champsim::chrono::picoseconds{kClockPeriodPs})
			.dib_set(32)
			.dib_way(8)
			.dib_window(16);

			cores.push_back(O3_CPU(o3corebuilder));
			
			/* DONE Populating CORES */

			//MYDRAM init?
			//DRAM.initialize();
			//DRAM.warmup=false;

			// operating on each element of ptws in a for loop breaks for reasons I coudln't figure out
			// we only have 1 ptw for now so just do front()
			ptws.front().initialize();
			ptws.front().warmup=false;
			for (CACHE& cache_c : caches){
				cache_c.initialize();
				cache_c.begin_phase();
				cache_c.warmup=false;
				cache_c.node_id = static_cast<uint32_t>(node_id);
				if (!address_map_path.empty()) {
					cache_c.address_map = &address_map;
				} else {
					cache_c.address_map = nullptr;
				}
				if (cache_c.NAME == "LLC" && cache_c.address_map != nullptr) {
					cache_c.send_remote = [this](const sst_request& req) {
						return enqueue_remote_request(req);
					};
				} else {
					cache_c.send_remote = {};
				}
			}

			// Dump stat periodically
			heartbeat_file = std::make_shared<std::ofstream>();
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

			if (!address_map_path.empty()) {
				if (!address_map.load(address_map_path)) {
					std::cerr << "WARNING: failed to load address_map_config from " << address_map_path << std::endl;
					vmem.set_address_map(nullptr, static_cast<uint32_t>(node_id), pool_pa_base);
				} else {
					vmem.set_address_map(&address_map, static_cast<uint32_t>(node_id), pool_pa_base);
				}
			} else {
				vmem.set_address_map(nullptr, static_cast<uint32_t>(node_id), pool_pa_base);
			}

			
			linkHandler_FABRIC = configureLink("port_handler_FABRIC", new Event::Handler<csimCore>(this, &csimCore::handleEvent_FABRIC));
			if (!linkHandler_FABRIC) {
				std::cerr << "ERROR: linkHandler_FABRIC is NULL for node " << node_id << std::endl;
			}
			registerClock(clock_frequency_str, new Clock::Handler<csimCore>(this,
				&csimCore::champsim_tick));	

			std::cout<<"Node "<<node_id<<": Done with cscore constructor"<<std::endl;
			
		}
		
		bool csimCore::champsim_tick(Cycle_t){

			global_clock.tick(time_quantum);
			heartbeat_count++;
			

			/* OPERABLES:  DRAM, ptws, caches, cores*/

			MYDRAM.operate_on(global_clock);

			// operating on ptws in a for loop breaks for reasons I coudln't figure out
			// (unrootcausible issue where ptw's clock gets reset every loop)
			ptws.front().operate_on(global_clock);
			
			for (CACHE& cache_c : caches){
				//cache_c.operate();
				cache_c.operate_on(global_clock);
			
				
				if(cache_heartbeat_period > 0 && (heartbeat_count % cache_heartbeat_period == 0)){
					auto cache_formats = champsim::plain_printer::format(cache_c.sim_stats);
					for (const auto& line : cache_formats) {
						std::cout << line << '\n';
					}
				}
					
			}

			advance_remote_requests();

			uint8_t curr_core_id=0;
			for (O3_CPU& cpu : cores){
				//cpu.operate();
				cpu.operate_on(global_clock);
				//std::cout<<"core retired_insts: "<<cpu.num_retired<<std::endl;

				auto& trace = traces.at(0); // TODO change if multiple cores
				for (auto pkt_count = cpu.IN_QUEUE_SIZE - static_cast<long>(std::size(cpu.input_queue)); !trace.eof() && pkt_count > 0; --pkt_count) {
					cpu.input_queue.push_back(trace());
					fetched_insts++;
					//std::cout<<"input queue size: "<<cpu.input_queue.size()<<std::endl;
				}
				if(trace.eof()){
					std::cout<<"CSCORE_tick: trace.eof() returned true! Reached end of trace in core tick function: "<< trace_name <<std::endl;
				}

				curr_core_id++;
			}

            if (!cores.empty() && sim_insts > 0) {
                auto retired = static_cast<uint64_t>(cores.front().num_retired);
                if (!warmup_done && warmup_insts > 0 && retired >= warmup_insts) {
                    for (auto& cache : caches) {
                        cache.begin_phase();
                    }
                    for (auto& cpu : cores) {
                        cpu.begin_phase();
                    }
                    warmup_done = true;
                }

                if (retired >= (warmup_insts + sim_insts)) {
                    for (auto& cache : caches) {
                        cache.end_phase(0);
                    }
                    for (auto& cpu : cores) {
                        cpu.end_phase(0);
                    }
                    print_final_stats();
                    primaryComponentOKToEndSim();
                    return true;
                }
            }

            if (!cores.empty() && sim_insts == 0) {
                auto& trace = traces.at(0);
                bool drained = true;
                for (auto& cpu : cores) {
                    auto lq_empty = std::all_of(cpu.LQ.begin(), cpu.LQ.end(), [](const auto& e) { return !e.has_value(); });
                    if (!cpu.input_queue.empty() || !cpu.ROB.empty() || !cpu.IFETCH_BUFFER.empty() || !cpu.DECODE_BUFFER.empty() ||
                        !cpu.DISPATCH_BUFFER.empty() || !cpu.DIB_HIT_BUFFER.empty() || !lq_empty || !cpu.SQ.empty()) {
                        drained = false;
                        break;
                    }
                }
                if (trace.eof() && drained) {
                    for (auto& cache : caches) {
                        cache.end_phase(0);
                    }
                    for (auto& cpu : cores) {
                        cpu.end_phase(0);
                    }
                    print_final_stats();
                    primaryComponentOKToEndSim();
                    return true;
                }
            }

			//std::cout<<"ptw current_cycle after champsim_tick completed: "<<ptws.front().current_cycle()<<std::endl;

			return false;
        }

        void csimCore::print_final_stats()
        {
            if (final_stats_printed) {
                return;
            }
            final_stats_printed = true;

            champsim::phase_stats stats;
            stats.name = "Node " + std::to_string(node_id);
            stats.trace_names.push_back(trace_name);

            stats.sim_cpu_stats.reserve(cores.size());
            stats.roi_cpu_stats.reserve(cores.size());
            for (auto& cpu : cores) {
                stats.sim_cpu_stats.push_back(cpu.sim_stats);
                stats.roi_cpu_stats.push_back(cpu.roi_stats);
            }

            stats.sim_cache_stats.reserve(caches.size());
            stats.roi_cache_stats.reserve(caches.size());
            for (auto& cache : caches) {
                stats.sim_cache_stats.push_back(cache.sim_stats);
                stats.roi_cache_stats.push_back(cache.roi_stats);
            }

            // MY_MEMORY_CONTROLLER does not expose DRAM_CHANNEL stats; leave DRAM stats empty.

            champsim::plain_printer printer{std::cout};
            printer.print(stats);

            // StarNUMA-style LLC demand-miss summary (LOAD+RFO only), post-merge (MSHR return).
            const auto demand_return_count = [](const CACHE::stats_type& st) {
                uint64_t total = 0;
                for (const auto& key : st.mshr_return.get_keys()) {
                    if (key.first == access_type::LOAD || key.first == access_type::RFO) {
                        total += static_cast<uint64_t>(st.mshr_return.value_or(key, 0));
                    }
                }
                return total;
            };

            for (const auto& cache : caches) {
                if (cache.NAME != "LLC") {
                    continue;
                }
                const auto& st = warmup_done ? cache.roi_stats : cache.sim_stats;
                const uint64_t total_demand_miss = demand_return_count(st);
                const uint64_t cxl_demand_miss = st.pool_demand_miss_count;
                const double avg_miss_lat = (total_demand_miss > 0)
                    ? static_cast<double>(st.total_miss_latency_cycles) / static_cast<double>(total_demand_miss)
                    : 0.0;
                const double avg_cxl_lat = (cxl_demand_miss > 0)
                    ? static_cast<double>(st.pool_demand_miss_latency_sum) / static_cast<double>(cxl_demand_miss)
                    : 0.0;
                std::cout << cxl_demand_miss << " / " << total_demand_miss << " LLC misses are CXL" << std::endl;
                std::cout << "LLC miss lat: " << avg_miss_lat << ", cxl lat: " << avg_cxl_lat << std::endl;
                std::cout << "LLC_MISS_LAT_HIST (in ns):" << std::endl;
                for (std::size_t i = 0; i < st.miss_latency_hist.size(); ++i) {
                    std::cout << (i * 10) << " : " << st.miss_latency_hist[i] << std::endl;
                }
                break;
            }
        }

		void csimCore::handleEvent_FABRIC(SST::Event *ev)
		{
			auto* cevent = static_cast<csEvent*>(ev);
			auto resp = convert_event_to_response(*cevent);
			for (auto& cache : caches) {
				if (cache.NAME == "LLC" && cache.handle_remote_response(resp)) {
					delete cevent;
					return;
				}
			}
			delete cevent;
		}

		bool csimCore::enqueue_remote_request(const sst_request& req)
		{
			remote_outbox.emplace_back(req);
			return true;
		}

		void csimCore::advance_remote_requests()
		{
			if (!linkHandler_FABRIC) {
				return;
			}
			while (!remote_outbox.empty()) {
				auto req = remote_outbox.front();
				auto* event = convert_request_to_event(req);
				// TODO: path selection/routing metadata if needed
				linkHandler_FABRIC->send(event);
				remote_outbox.pop_front();
			}
		}

	} // namespace csimCore
} // namespace SST
