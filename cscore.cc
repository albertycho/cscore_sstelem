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
#include <array>
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
#include "response_timeline.h"

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

double parse_clock_ghz(std::string clock_str) {
    clock_str.erase(
        std::remove_if(clock_str.begin(), clock_str.end(), [](unsigned char c) { return std::isspace(c); }),
        clock_str.end());
    if (clock_str.empty()) {
        return 0.0;
    }
    std::size_t idx = 0;
    double value = 0.0;
    try {
        value = std::stod(clock_str, &idx);
    } catch (...) {
        return 0.0;
    }
    if (idx >= clock_str.size()) {
        return 0.0;
    }
    std::string unit = clock_str.substr(idx);
    for (auto& ch : unit) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (unit == "ghz") return value;
    if (unit == "mhz") return value / 1'000.0;
    if (unit == "khz") return value / 1'000'000.0;
    if (unit == "hz") return value / 1'000'000'000.0;
    if (unit == "thz") return value * 1'000.0;
    return 0.0;
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
            const int64_t legacy_cxl_link_queue_size = params.find<int64_t>("cxl_link_queue_size", 0);
            cxl_link_egress_buffer_size_ =
                params.find<int64_t>("cxl_link_egress_buffer_size", legacy_cxl_link_queue_size);
            cxl_link_credit_window_size_ =
                params.find<int64_t>("cxl_link_credit_window_size", legacy_cxl_link_queue_size);
            const auto l1d_mshr_size_override =
                static_cast<std::size_t>(params.find<uint64_t>("l1d_mshr_size_override", 16));
            const auto llc_mshr_size_override =
                static_cast<std::size_t>(params.find<uint64_t>("llc_mshr_size_override", 64));
            const auto llc_tag_bandwidth_override =
                champsim::bandwidth::maximum_type{
                    params.find<uint64_t>("llc_tag_bandwidth_override", 1)};
            const auto llc_fill_bandwidth_override =
                champsim::bandwidth::maximum_type{
                    params.find<uint64_t>("llc_fill_bandwidth_override", 1)};
            const bool complete_stores_after_issue =
                params.find<int>("complete_stores_after_issue", 0) != 0;
            const bool inject_enable = params.find<int>("inject_enable", 0) != 0;
            const double inject_bandwidth_gbps = params.find<double>("inject_bandwidth_gbps", 0.0);
            const uint64_t inject_load_pct = std::min<uint64_t>(params.find<uint64_t>("inject_load_pct", 100), 100);

			// Older version registered this as primary component
			registerAsPrimaryComponent();
		    primaryComponentDoNotEndSim();
			

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
					.mshr_size(llc_mshr_size_override)
					.latency(20)
					.fill_latency(1)
					.tag_bandwidth(llc_tag_bandwidth_override)
					.fill_bandwidth(llc_fill_bandwidth_override)
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
				.mshr_size(l1d_mshr_size_override)
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
			cores.back().complete_stores_after_issue = complete_stores_after_issue;
			
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
            if (inject_enable) {
                const double clock_ghz = parse_clock_ghz(clock_frequency_str);
                if (clock_ghz <= 0.0) {
                    throw std::runtime_error("csimCore: inject_enable requires a parseable clock frequency.");
                }
                if (address_map_path.empty()) {
                    throw std::runtime_error("csimCore: inject_enable requires address_map_config.");
                }
                const auto inject_entry = address_map.lookup(static_cast<uint32_t>(node_id), pool_pa_base);
                if (!inject_entry.has_value() || inject_entry->type != AddressType::Pool) {
                    throw std::runtime_error("csimCore: inject_enable requires pool_pa_base to map to a Pool entry.");
                }
                injector_.configure(inject_bandwidth_gbps / (8.0 * clock_ghz),
                                    inject_load_pct,
                                    static_cast<uint32_t>(node_id),
                                    inject_entry->target,
                                    inject_entry->start,
                                    inject_entry->size);
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
                                   cxl_link_egress_buffer_size_,
                                   cxl_link_credit_window_size_,
                                   std::nullopt);
            remote_port_.set_debug_label("node." + std::to_string(node_id) + ".remote_port");
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
            remote_port_.try_receive(cycle_u, [this](csEvent* ev) {
                return handle_remote_event(ev);
            });
            injector_.tick([this](const sst_request& req) {
                return enqueue_remote_request(req);
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
                    stats_start_cycle_ = heartbeat_count;
                    remote_port_.reset_stats(heartbeat_count);
                    if (cxl_port_configured_ && node_id == 0) {
                        auto* reset_ev = make_reset_util_event(static_cast<uint64_t>(node_id), kControlBroadcast);
                        if (!remote_port_.send(reset_ev)) {
                            delete reset_ev;
                        }
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

            if (lightweight_output_) {
                for (std::size_t cpu_idx = 0; cpu_idx < cores.size(); ++cpu_idx) {
                    const auto& st = warmup_done ? cores[cpu_idx].roi_stats : cores[cpu_idx].sim_stats;
                    const auto prefix = std::string("stat.node.") + std::to_string(node_id) + ".cpu." + std::to_string(cpu_idx) + ".";
                    const double avg_load_issue_to_complete_lat = (st.load_issue_to_complete_count > 0)
                        ? static_cast<double>(st.load_issue_to_complete_sum_cycles) / static_cast<double>(st.load_issue_to_complete_count)
                        : 0.0;
                    std::cout << prefix << "load_issue_to_complete_count = " << st.load_issue_to_complete_count << '\n';
                    std::cout << prefix << "avg_load_issue_to_complete_lat = " << avg_load_issue_to_complete_lat << '\n';
                }
            }

            // LLC demand-miss summary (LOAD+RFO only), per original miss.
            for (const auto& cache : caches) {
                if (cache.NAME != "LLC") {
                    continue;
                }
                const auto& st = warmup_done ? cache.roi_stats : cache.sim_stats;
                const uint64_t total_demand_miss = st.completed_demand_miss_count;
                const uint64_t cxl_demand_miss = st.pool_demand_miss_count;
                const double avg_miss_lat = (total_demand_miss > 0)
                    ? static_cast<double>(st.total_miss_latency_cycles) / static_cast<double>(total_demand_miss)
                    : 0.0;
                const double avg_cxl_miss_lat = (cxl_demand_miss > 0)
                    ? static_cast<double>(st.pool_demand_miss_latency_sum) / static_cast<double>(cxl_demand_miss)
                    : 0.0;
                if (lightweight_output_) {
                    const auto prefix = std::string("stat.node.") + std::to_string(node_id) + ".llc.";
                    std::cout << prefix << "pool_accesses = " << st.pool_accesses << '\n';
                    std::cout << prefix << "cxl_miss = " << cxl_demand_miss << '\n';
                    std::cout << prefix << "total_miss = " << total_demand_miss << '\n';
                    std::cout << prefix << "avg_miss_lat = " << avg_miss_lat << '\n';
                    std::cout << prefix << "avg_cxl_miss_lat = " << avg_cxl_miss_lat << '\n';
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
                    std::cout << "LLC miss lat: " << avg_miss_lat << ", cxl miss lat: " << avg_cxl_miss_lat << std::endl;
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
            const uint64_t bw_interval_cycles = warmup_done
                ? (heartbeat_count - stats_start_cycle_)
                : heartbeat_count;
            const uint64_t host_to_switch_bytes = remote_port_.tx_bytes_total();
            const uint64_t switch_to_host_bytes = remote_port_.rx_bytes_total();
            const uint64_t host_link_total_bytes = host_to_switch_bytes + switch_to_host_bytes;
            double clock_ghz = parse_clock_ghz(clock_frequency_str);
            const double bw_interval_cycles_d = static_cast<double>(bw_interval_cycles);
            const double host_to_switch_gbps = (bw_interval_cycles > 0)
                ? (static_cast<double>(host_to_switch_bytes) / bw_interval_cycles_d) * clock_ghz
                : 0.0;
            const double switch_to_host_gbps = (bw_interval_cycles > 0)
                ? (static_cast<double>(switch_to_host_bytes) / bw_interval_cycles_d) * clock_ghz
                : 0.0;
            const double host_link_total_gbps = host_to_switch_gbps + switch_to_host_gbps;

            if (lightweight_output_) {
                const auto prefix = std::string("stat.node.") + std::to_string(node_id) + ".";
                static constexpr std::array<std::pair<FabricPort::TrafficClass, const char*>, 4> kTrafficClasses{{
                    {FabricPort::TrafficClass::DemandReq, "demand_req"},
                    {FabricPort::TrafficClass::WriteReq, "write_req"},
                    {FabricPort::TrafficClass::Response, "response"},
                    {FabricPort::TrafficClass::OtherReq, "other_req"},
                }};
                auto print_class_fabric_stats = [&](const std::string& stat_prefix, const FabricPort& port) {
                    for (const auto& [cls, cls_name] : kTrafficClasses) {
                        std::cout << stat_prefix << "ingress_wait_avg_cycles." << cls_name
                                  << " = " << port.ingress_wait_avg_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_wait_max_cycles." << cls_name
                                  << " = " << port.ingress_wait_max_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_queue_wait_avg_cycles." << cls_name
                                  << " = " << port.ingress_queue_wait_avg_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_queue_wait_max_cycles." << cls_name
                                  << " = " << port.ingress_queue_wait_max_cycles(cls) << '\n';
                        std::cout << stat_prefix << "egress_wait_avg_cycles." << cls_name
                                  << " = " << port.egress_wait_avg_cycles(cls) << '\n';
                        std::cout << stat_prefix << "egress_wait_max_cycles." << cls_name
                                  << " = " << port.egress_wait_max_cycles(cls) << '\n';
                        std::cout << stat_prefix << "egress_occ_avg_bytes." << cls_name
                                  << " = " << port.egress_occ_avg_bytes(cls) << '\n';
                        std::cout << stat_prefix << "egress_occ_max_bytes." << cls_name
                                  << " = " << port.egress_occ_max_bytes(cls) << '\n';
                        std::cout << stat_prefix << "egress_blocked_cycles." << cls_name
                                  << " = " << port.egress_blocked_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_arrival_empty_packets." << cls_name
                                  << " = " << port.ingress_arrival_empty_packets(cls) << '\n';
                        std::cout << stat_prefix << "ingress_arrival_nonempty_packets." << cls_name
                                  << " = " << port.ingress_arrival_nonempty_packets(cls) << '\n';
                        std::cout << stat_prefix << "ingress_arrival_nonempty_packet_frac." << cls_name
                                  << " = " << port.ingress_arrival_nonempty_packet_frac(cls) << '\n';
                        std::cout << stat_prefix << "ingress_nonempty_arrival_pre_occ_avg_bytes." << cls_name
                                  << " = " << port.ingress_nonempty_arrival_pre_occ_avg_bytes(cls) << '\n';
                        std::cout << stat_prefix << "ingress_nonempty_arrival_pre_occ_max_bytes." << cls_name
                                  << " = " << port.ingress_nonempty_arrival_pre_occ_max_bytes(cls) << '\n';
                        std::cout << stat_prefix << "ingress_release_after_empty_arrival_packets." << cls_name
                                  << " = " << port.ingress_release_after_empty_arrival_packets(cls) << '\n';
                        std::cout << stat_prefix << "ingress_release_after_nonempty_arrival_packets." << cls_name
                                  << " = " << port.ingress_release_after_nonempty_arrival_packets(cls) << '\n';
                        std::cout << stat_prefix << "ingress_wait_after_empty_arrival_avg_cycles." << cls_name
                                  << " = " << port.ingress_wait_after_empty_arrival_avg_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_wait_after_nonempty_arrival_avg_cycles." << cls_name
                                  << " = " << port.ingress_wait_after_nonempty_arrival_avg_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_queue_wait_after_empty_arrival_avg_cycles." << cls_name
                                  << " = " << port.ingress_queue_wait_after_empty_arrival_avg_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_queue_wait_after_nonempty_arrival_avg_cycles." << cls_name
                                  << " = " << port.ingress_queue_wait_after_nonempty_arrival_avg_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_queue_wait_after_empty_arrival_max_cycles." << cls_name
                                  << " = " << port.ingress_queue_wait_after_empty_arrival_max_cycles(cls) << '\n';
                        std::cout << stat_prefix << "ingress_queue_wait_after_nonempty_arrival_max_cycles." << cls_name
                                  << " = " << port.ingress_queue_wait_after_nonempty_arrival_max_cycles(cls) << '\n';
                        std::cout << stat_prefix << "rx_bytes." << cls_name << " = " << port.rx_bytes(cls) << '\n';
                        std::cout << stat_prefix << "tx_bytes." << cls_name << " = " << port.tx_bytes(cls) << '\n';
                        std::cout << stat_prefix << "rx_pkts." << cls_name << " = " << port.rx_packets(cls) << '\n';
                        std::cout << stat_prefix << "tx_pkts." << cls_name << " = " << port.tx_packets(cls) << '\n';
                    }
                    port.emit_deep_diagnostics(std::cout, stat_prefix);
                };
                std::cout << prefix << "util.dram_avg = " << MYDRAM.queue_average_utilization(0) << '\n';
                std::cout << prefix << "bw.interval_cycles = " << bw_interval_cycles << '\n';
                std::cout << prefix << "bw.host_to_switch_bytes = " << host_to_switch_bytes << '\n';
                std::cout << prefix << "bw.switch_to_host_bytes = " << switch_to_host_bytes << '\n';
                std::cout << prefix << "bw.host_link_total_bytes = " << host_link_total_bytes << '\n';
                std::cout << prefix << "bw.host_to_switch_gbps = " << host_to_switch_gbps << '\n';
                std::cout << prefix << "bw.switch_to_host_gbps = " << switch_to_host_gbps << '\n';
                std::cout << prefix << "bw.host_link_total_gbps = " << host_link_total_gbps << '\n';
                std::cout << prefix << "fabric.ingress_wait_avg_cycles = " << remote_port_.ingress_wait_avg_cycles() << '\n';
                std::cout << prefix << "fabric.egress_wait_avg_cycles = " << remote_port_.egress_wait_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_wait_max_cycles = " << remote_port_.ingress_wait_max_cycles() << '\n';
                std::cout << prefix << "fabric.egress_wait_max_cycles = " << remote_port_.egress_wait_max_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_queue_wait_avg_cycles = " << remote_port_.ingress_queue_wait_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_queue_wait_max_cycles = " << remote_port_.ingress_queue_wait_max_cycles() << '\n';
                std::cout << prefix << "fabric.egress_occ_avg_bytes = " << remote_port_.egress_occ_avg_bytes() << '\n';
                std::cout << prefix << "fabric.egress_occ_stddev_bytes = " << remote_port_.egress_occ_stddev_bytes() << '\n';
                std::cout << prefix << "fabric.egress_occ_max_bytes = " << remote_port_.egress_occ_max_bytes() << '\n';
                std::cout << prefix << "fabric.egress_occ_nonempty_frac = " << remote_port_.egress_occ_nonempty_frac() << '\n';
                std::cout << prefix << "fabric.egress_blocked_cycles = " << remote_port_.egress_blocked_cycles() << '\n';
                std::cout << prefix << "fabric.egress_blocked_nonempty_frac = " << remote_port_.egress_blocked_nonempty_frac() << '\n';
                std::cout << prefix << "fabric.egress_blocked_avg_occ_bytes = " << remote_port_.egress_blocked_avg_occ_bytes() << '\n';
                std::cout << prefix << "fabric.egress_blocked_max_occ_bytes = " << remote_port_.egress_blocked_max_occ_bytes() << '\n';
                std::cout << prefix << "fabric.egress_send_burst_max_pkts = " << remote_port_.egress_send_burst_max_pkts() << '\n';
                std::cout << prefix << "fabric.egress_send_burst_max_bytes = " << remote_port_.egress_send_burst_max_bytes() << '\n';
                std::cout << prefix << "fabric.egress_send_burst_avg_pkts = " << remote_port_.egress_send_burst_avg_pkts() << '\n';
                std::cout << prefix << "fabric.egress_send_burst_avg_bytes = " << remote_port_.egress_send_burst_avg_bytes() << '\n';
                std::cout << prefix << "fabric.egress_send_burst_stddev_pkts = " << remote_port_.egress_send_burst_stddev_pkts() << '\n';
                std::cout << prefix << "fabric.egress_send_burst_stddev_bytes = " << remote_port_.egress_send_burst_stddev_bytes() << '\n';
                std::cout << prefix << "fabric.egress_send_nonempty_frac = " << remote_port_.egress_send_nonempty_frac() << '\n';
                std::cout << prefix << "fabric.ingress_occ_bytes = " << remote_port_.ingress_occupancy() << '\n';
                std::cout << prefix << "fabric.ready_wait_avg_cycles = " << remote_port_.ready_wait_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ready_wait_max_cycles = " << remote_port_.ready_wait_max_cycles() << '\n';
                std::cout << prefix << "fabric.ready_occ_avg_pkts = " << remote_port_.ready_occupancy_avg() << '\n';
                std::cout << prefix << "fabric.ready_occ_pkts = " << remote_port_.ready_occupancy() << '\n';
                std::cout << prefix << "fabric.ready_occ_max_pkts = " << remote_port_.ready_occupancy_max() << '\n';
                std::cout << prefix << "fabric.ready_retry_count = " << remote_port_.ready_retry_count() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_burst_max_pkts = " << remote_port_.ingress_arrival_burst_max_pkts() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_burst_max_bytes = " << remote_port_.ingress_arrival_burst_max_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_burst_avg_pkts = " << remote_port_.ingress_arrival_burst_avg_pkts() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_burst_avg_bytes = " << remote_port_.ingress_arrival_burst_avg_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_burst_stddev_pkts = " << remote_port_.ingress_arrival_burst_stddev_pkts() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_burst_stddev_bytes = " << remote_port_.ingress_arrival_burst_stddev_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_nonempty_frac = " << remote_port_.ingress_arrival_nonempty_frac() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_run_max_cycles = " << remote_port_.ingress_arrival_run_max_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_run_avg_cycles = " << remote_port_.ingress_arrival_run_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_run_stddev_cycles = " << remote_port_.ingress_arrival_run_stddev_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_gap_max_cycles = " << remote_port_.ingress_arrival_gap_max_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_gap_avg_cycles = " << remote_port_.ingress_arrival_gap_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_arrival_gap_stddev_cycles = " << remote_port_.ingress_arrival_gap_stddev_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_release_burst_max_pkts = " << remote_port_.ingress_release_burst_max_pkts() << '\n';
                std::cout << prefix << "fabric.ingress_release_burst_max_bytes = " << remote_port_.ingress_release_burst_max_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_release_burst_avg_pkts = " << remote_port_.ingress_release_burst_avg_pkts() << '\n';
                std::cout << prefix << "fabric.ingress_release_burst_avg_bytes = " << remote_port_.ingress_release_burst_avg_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_release_burst_stddev_pkts = " << remote_port_.ingress_release_burst_stddev_pkts() << '\n';
                std::cout << prefix << "fabric.ingress_release_burst_stddev_bytes = " << remote_port_.ingress_release_burst_stddev_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_release_nonempty_frac = " << remote_port_.ingress_release_nonempty_frac() << '\n';
                std::cout << prefix << "fabric.ingress_release_run_max_cycles = " << remote_port_.ingress_release_run_max_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_release_run_avg_cycles = " << remote_port_.ingress_release_run_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_release_run_stddev_cycles = " << remote_port_.ingress_release_run_stddev_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_release_gap_max_cycles = " << remote_port_.ingress_release_gap_max_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_release_gap_avg_cycles = " << remote_port_.ingress_release_gap_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_release_gap_stddev_cycles = " << remote_port_.ingress_release_gap_stddev_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_occ_avg_bytes = " << remote_port_.ingress_occ_avg_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_occ_stddev_bytes = " << remote_port_.ingress_occ_stddev_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_occ_max_bytes = " << remote_port_.ingress_occ_max_bytes() << '\n';
                std::cout << prefix << "fabric.ingress_occ_nonempty_frac = " << remote_port_.ingress_occ_nonempty_frac() << '\n';
                std::cout << prefix << "fabric.ingress_occ_run_max_cycles = " << remote_port_.ingress_occ_run_max_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_occ_run_avg_cycles = " << remote_port_.ingress_occ_run_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_occ_run_stddev_cycles = " << remote_port_.ingress_occ_run_stddev_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_occ_gap_max_cycles = " << remote_port_.ingress_occ_gap_max_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_occ_gap_avg_cycles = " << remote_port_.ingress_occ_gap_avg_cycles() << '\n';
                std::cout << prefix << "fabric.ingress_occ_gap_stddev_cycles = " << remote_port_.ingress_occ_gap_stddev_cycles() << '\n';
                print_class_fabric_stats(prefix + "fabric.", remote_port_);
                std::cout << prefix << "walltime_s = " << total_sec << '\n';
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
            if (injector_.owns_response(resp)) {
                delete ev;
                return true;
            }
            if (!deliver_remote_response(resp)) {
                return false;
            }
            delete ev;
            return true;
        }

        bool csimCore::deliver_remote_response(const sst_response& resp)
        {
            response_timeline::log_response("node." + std::to_string(node_id),
                                            "node.response_receive",
                                            heartbeat_count,
                                            resp);
            for (auto& cache : caches) {
                if (cache.NAME == "LLC" && cache.handle_remote_response(resp)) {
                    return true;
                }
            }
            return false;
        }

		bool csimCore::enqueue_remote_request(const sst_request& req)
		{
            const bool injected_req = (req.trace_tag & TrafficInjector::kTraceTagBit) != 0;
			const uint64_t retired = (!cores.empty()) ? static_cast<uint64_t>(cores.front().num_retired) : 0;
			const bool bypass_phase =
                !injected_req &&
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
            sst_request tagged_req = req;
            if (tagged_req.response_requested && tagged_req.trace_tag == 0) {
                tagged_req.trace_tag = next_remote_trace_tag_++;
            }
			auto* event = convert_request_to_event(tagged_req);
			if (!remote_port_.send(event)) {
				delete event;
				return false;
			}
            if (tagged_req.response_requested) {
                response_timeline::log_request("node." + std::to_string(node_id),
                                               "node.request_issue",
                                               heartbeat_count,
                                               tagged_req);
            }
			return true;
		}

	} // namespace csimCore
} // namespace SST
