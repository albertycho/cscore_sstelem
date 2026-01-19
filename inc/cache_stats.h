#ifndef CACHE_STATS_H
#define CACHE_STATS_H

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "channel.h"
#include "event_counter.h"

struct cache_stats {
  static constexpr std::size_t POOL_LAT_HIST_BINS = 32;

  std::string name;
  // prefetch stats
  uint64_t pf_requested = 0;
  uint64_t pf_issued = 0;
  uint64_t pf_useful = 0;
  uint64_t pf_useless = 0;
  uint64_t pf_fill = 0;

  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> hits = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> misses = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> mshr_merge = {};
  champsim::stats::event_counter<std::pair<access_type, std::remove_cv_t<decltype(NUM_CPUS)>>> mshr_return = {};

  long total_miss_latency_cycles{};

  uint64_t pool_accesses = 0;
  uint64_t pool_completed = 0;
  uint64_t pool_latency_sum = 0;
  std::array<uint64_t, POOL_LAT_HIST_BINS> pool_latency_hist{};
};

cache_stats operator-(cache_stats lhs, cache_stats rhs);

#endif
