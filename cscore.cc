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
#include <cctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <limits>
#include <optional>
#include <sstream>

#include "trace_instruction.h"
#include "bimodal/bimodal.h"
#include "prefetcher/no/no.h"
#include "control_event.h"

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

MY_MEMORY_CONTROLLER::latency_function_type select_latency_fn(SST::Params& params, const char* model_key, const char* fixed_key,
                                                               int64_t default_fixed_cycles) {
    auto model = params.find<std::string>(model_key, "fixed");
    for (auto& ch : model) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (model == "utilization-based") {
        return estimate_latency_utilization_based;
    }
    const auto fixed_cycles = static_cast<int64_t>(params.find<uint64_t>(fixed_key, static_cast<uint64_t>(default_fixed_cycles)));
    return [fixed_cycles](double) { return fixed_cycles; };
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
               select_latency_fn(params, "dram_latency_model", "dram_fixed_latency_cycles", DEFAULT_FIXED_LATENCY_CYCLES),
               champsim::data::bytes{static_cast<long long>(params.find<uint64_t>("dram_size_bytes", DEFAULT_DRAM_SIZE_BYTES))}),
			vmem(champsim::data::bytes{4096}, 5, champsim::chrono::picoseconds{kClockPeriodPs * 200}, MYDRAM, 1)
		{
			/* This function sets up and builds core (and cache and bp and etc) */

			clock_frequency_str = params.find<std::string>("clock", "2.4GHz");

            trace_name = params.find<std::string>("trace_name", "example_tracename.xz");
			address_map_path = params.find<std::string>("address_map_config", "");
            node_id = params.find<int64_t>("node_id", 0);
            warmup_insts = params.find<int64_t>("warmup_insts", 0);
            sim_insts = params.find<int64_t>("sim_insts", 0);
            warm_cache_insts_ = params.find<uint64_t>("warm_cache_insts", warmup_insts);
            if (warm_cache_insts_ > warmup_insts) {
                warm_cache_insts_ = warmup_insts;
            }
            lightweight_output_ = params.find<int>("lightweight_output", 0) != 0;
            print_latency_hist_ = params.find<int>("print_latency_hist", 1) != 0;
            fabric_diag_output_ = params.find<int>("fabric_diag_output", 0) != 0;
            auto dram_size_bytes = params.find<uint64_t>("dram_size_bytes", DEFAULT_DRAM_SIZE_BYTES);
            pool_pa_base = params.find<uint64_t>("pool_pa_base", 0);
            if (pool_pa_base == 0) {
                pool_pa_base = dram_size_bytes;
            }
            if (pool_pa_base < dram_size_bytes) {
                if (!lightweight_output_) {
                    std::cerr << "WARNING: pool_pa_base overlaps DRAM range. Forcing pool_pa_base = dram_size_bytes." << std::endl;
                }
                pool_pa_base = dram_size_bytes;
            }
            cache_heartbeat_period = params.find<uint64_t>("cache_heartbeat_period", 1000);
            cpu_heartbeat_period = params.find<uint64_t>("cpu_heartbeat_period", 0);
            util_heartbeat_period = params.find<uint64_t>("util_heartbeat_period", 0);
            cxl_link_bw_cycles_ = params.find<int64_t>("cxl_link_bw_cycles", 0);
            cxl_link_latency_cycles_ = params.find<int64_t>("cxl_link_latency_cycles", 0);
            cxl_link_queue_size_ = params.find<int64_t>("cxl_link_queue_size", 0);

			// Older version registered this as primary component
			registerAsPrimaryComponent();
		    primaryComponentDoNotEndSim();
			

			// tmp_instr.msgSize=0;

			std::vector<std::string> trace_names;
			trace_names.push_back(trace_name);
			traces.push_back(get_tracereader(trace_name, 0, false, false));

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
					if (!lightweight_output_) {
						std::cerr << "WARNING: failed to load address_map_config from " << address_map_path << std::endl;
					}
					vmem.set_address_map(nullptr, static_cast<uint32_t>(node_id), pool_pa_base);
				} else {
					vmem.set_address_map(&address_map, static_cast<uint32_t>(node_id), pool_pa_base);
				}
			} else {
				vmem.set_address_map(nullptr, static_cast<uint32_t>(node_id), pool_pa_base);
			}


            auto* cxl_link = configureLink(
                "port_handler_cxl",
                new Event::Handler<FabricPort>(&remote_port_, &FabricPort::handle_event));
            if (!cxl_link) {
                throw std::runtime_error("csimCore: missing link for port_handler_cxl on node " + std::to_string(node_id) + ".");
            }
            remote_port_.configure(cxl_link,
                                   static_cast<uint64_t>(node_id),
                                   cxl_link_bw_cycles_,
                                   cxl_link_latency_cycles_,
                                   cxl_link_queue_size_);
            cxl_port_configured_ = true;

			registerClock(clock_frequency_str, new Clock::Handler<csimCore>(this,
				&csimCore::champsim_tick));	

		}

        void csimCore::setup() {
            wall_start_ = std::chrono::steady_clock::now();
            active_time_ = std::chrono::steady_clock::duration{};
            active_calls_ = 0;
        }
		
		bool csimCore::champsim_tick(Cycle_t cycle){
            ScopedTimer timer(active_time_, active_calls_);

			global_clock.tick(time_quantum);
			heartbeat_count++;

            const auto cycle_u = static_cast<uint64_t>(cycle);
            while (!warmup_bypass_responses_.empty()) {
                if (!deliver_remote_response(warmup_bypass_responses_.front())) {
                    break;
                }
                warmup_bypass_responses_.pop_front();
            }
            remote_port_.tick(cycle_u);
            remote_port_.try_receive(cycle_u, [this](csEvent* ev) {
                return handle_remote_event(ev);
            });
			

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
				if (cpu_heartbeat_period > 0 && (heartbeat_count % cpu_heartbeat_period == 0)) {
					const auto node_prefix = std::string("stat.node.") + std::to_string(node_id) + ".cpu.";
					const auto core_prefix = node_prefix + std::to_string(static_cast<unsigned>(curr_core_id)) + ".";
					std::cout << core_prefix << "retired = " << cpu.num_retired << '\n';
					std::cout << core_prefix << "cycles = " << heartbeat_count << '\n';
				}

				curr_core_id++;
			}

            if (!cores.empty() && sim_insts > 0) {
                auto retired = static_cast<uint64_t>(cores.front().num_retired);
                if (!warmup_done && warmup_insts > 0 && retired >= warmup_insts) {
                    // Start ROI stats at warmup boundary. This excludes warm-cache bypass traffic
                    // from reported LLC miss/cxl-lat metrics while preserving functional state.
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

            if (!lightweight_output_) {
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
            }

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
                const uint64_t pool_accesses = st.pool_accesses;
                const uint64_t pool_completed = st.pool_completed;
                const uint64_t pool_outstanding = (pool_accesses >= pool_completed) ? (pool_accesses - pool_completed) : 0;
                const double avg_miss_lat = (total_demand_miss > 0)
                    ? static_cast<double>(st.total_miss_latency_cycles) / static_cast<double>(total_demand_miss)
                    : 0.0;
                const double avg_cxl_lat = (cxl_demand_miss > 0)
                    ? static_cast<double>(st.pool_demand_miss_latency_sum) / static_cast<double>(cxl_demand_miss)
                    : 0.0;
                if (lightweight_output_) {
                    const auto prefix = std::string("stat.node.") + std::to_string(node_id) + ".llc.";
                    std::cout << prefix << "cxl_miss = " << cxl_demand_miss << '\n';
                    std::cout << prefix << "total_miss = " << total_demand_miss << '\n';
                    std::cout << prefix << "avg_miss_lat = " << avg_miss_lat << '\n';
                    std::cout << prefix << "avg_cxl_lat = " << avg_cxl_lat << '\n';
                    std::cout << prefix << "pool_accesses = " << pool_accesses << '\n';
                    std::cout << prefix << "pool_completed = " << pool_completed << '\n';
                    std::cout << prefix << "pool_outstanding = " << pool_outstanding << '\n';
                    if (print_latency_hist_) {
                        std::cout << prefix << "miss_lat_hist_bin_ns = 10\n";
                        std::cout << prefix << "miss_lat_hist = [";
                        for (std::size_t i = 0; i < st.miss_latency_hist.size(); ++i) {
                            if (i != 0) {
                                std::cout << ",";
                            }
                            std::cout << st.miss_latency_hist[i];
                        }
                        std::cout << "]\n";
                    }
                } else {
                    std::cout << cxl_demand_miss << " / " << total_demand_miss << " LLC misses are CXL" << std::endl;
                    std::cout << "LLC miss lat: " << avg_miss_lat << ", cxl lat: " << avg_cxl_lat << std::endl;
                    std::cout << "LLC pool accesses/completed/outstanding: "
                              << pool_accesses << " / " << pool_completed << " / " << pool_outstanding << std::endl;
                    if (print_latency_hist_) {
                        std::cout << "LLC_MISS_LAT_HIST (in ns):" << std::endl;
                        for (std::size_t i = 0; i < st.miss_latency_hist.size(); ++i) {
                            std::cout << (i * 10) << " : " << st.miss_latency_hist[i] << std::endl;
                        }
                    }
                }
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto total_sec = std::chrono::duration<double>(now - wall_start_).count();

            if (lightweight_output_) {
                const auto prefix = std::string("stat.node.") + std::to_string(node_id) + ".";
                std::cout << prefix << "util.dram_avg = " << MYDRAM.queue_average_utilization(0) << '\n';
                std::cout << prefix << "walltime_s = " << total_sec << '\n';
                if (fabric_diag_output_) {
                    const auto fabric_prefix = prefix + "fabric.";
                    std::cout << fabric_prefix << "ingress_wait_avg_cycles = " << remote_port_.ingress_avg_wait_cycles() << '\n';
                    std::cout << fabric_prefix << "ingress_wait_max_cycles = " << remote_port_.ingress_max_wait_cycles() << '\n';
                    std::cout << fabric_prefix << "ingress_samples = " << remote_port_.ingress_samples() << '\n';
                    std::cout << fabric_prefix << "egress_wait_avg_cycles = " << remote_port_.egress_avg_wait_cycles() << '\n';
                    std::cout << fabric_prefix << "egress_wait_max_cycles = " << remote_port_.egress_max_wait_cycles() << '\n';
                    std::cout << fabric_prefix << "egress_samples = " << remote_port_.egress_samples() << '\n';
                    std::cout << fabric_prefix << "ingress_occ = " << remote_port_.ingress_occupancy() << '\n';
                    std::cout << fabric_prefix << "ingress_avg_util = " << remote_port_.ingress_avg_utilization() << '\n';
                }
                if (active_calls_ > 0) {
                    const auto active_sec = std::chrono::duration<double>(active_time_).count();
                    std::cout << prefix << "active_time_s = " << active_sec << '\n';
                }
            } else {
                std::cout << "UTILIZATION SUMMARY" << '\n';
                std::cout << "  DRAM avg util: " << MYDRAM.queue_average_utilization(0) << '\n';
                std::cout << "WALLTIME SUMMARY" << '\n';
                std::cout << "  sim wall time (s): " << total_sec << '\n';
                if (active_calls_ > 0) {
                    const auto active_sec = std::chrono::duration<double>(active_time_).count();
                    std::cout << "Component Time Summary\n";
                    std::cout << "  csimCore active time (s): " << active_sec << '\n';
                }
            }
        }

        bool csimCore::handle_remote_event(csEvent* ev)
        {
            uint64_t ctrl_code = 0;
            if (is_control_event(*ev, &ctrl_code)) {
                delete ev;
                return true;
            }
            auto resp = convert_event_to_response(*ev);
            if (!deliver_remote_response(resp)) {
                return false;
            }
            delete ev;
            return true;
        }

        bool csimCore::deliver_remote_response(const sst_response& resp)
        {
            for (auto& cache : caches) {
                if (cache.NAME == "LLC" && cache.handle_remote_response(resp)) {
                    return true;
                }
            }
            return false;
        }

		bool csimCore::enqueue_remote_request(const sst_request& req)
		{
			const uint64_t retired = (!cores.empty()) ? static_cast<uint64_t>(cores.front().num_retired) : 0;
			const bool bypass_phase =
				(warmup_insts > 0) &&
				!warmup_done &&
				(retired < warm_cache_insts_);
			if (bypass_phase) {
				if (req.response_requested) {
					sst_response resp(req);
					resp.src_node = (req.dst_node == std::numeric_limits<uint32_t>::max()) ? req.sst_cpu : req.dst_node;
					resp.dst_node = (req.src_node == std::numeric_limits<uint32_t>::max()) ? req.cpu : req.src_node;
					resp.msg_bytes = 64;
					warmup_bypass_responses_.push_back(resp);
				}
				return true;
			}
			if (!cxl_port_configured_) {
				return false;
			}
			auto* event = convert_request_to_event(req);
			if (!remote_port_.send(event)) {
				delete event;
				return false;
			}
			return true;
		}

	} // namespace csimCore
} // namespace SST
