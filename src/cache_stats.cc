#include "cache_stats.h"

cache_stats operator-(cache_stats lhs, cache_stats rhs)
{
  cache_stats result;
  result.pf_requested = lhs.pf_requested - rhs.pf_requested;
  result.pf_issued = lhs.pf_issued - rhs.pf_issued;
  result.pf_useful = lhs.pf_useful - rhs.pf_useful;
  result.pf_useless = lhs.pf_useless - rhs.pf_useless;
  result.pf_fill = lhs.pf_fill - rhs.pf_fill;

  result.hits = lhs.hits - rhs.hits;
  result.misses = lhs.misses - rhs.misses;

  result.total_miss_latency_cycles = lhs.total_miss_latency_cycles - rhs.total_miss_latency_cycles;
  result.pool_accesses = lhs.pool_accesses - rhs.pool_accesses;
  result.pool_completed = lhs.pool_completed - rhs.pool_completed;
  result.pool_latency_sum = lhs.pool_latency_sum - rhs.pool_latency_sum;
  result.pool_demand_miss_count = lhs.pool_demand_miss_count - rhs.pool_demand_miss_count;
  result.pool_demand_miss_latency_sum = lhs.pool_demand_miss_latency_sum - rhs.pool_demand_miss_latency_sum;
  for (std::size_t idx = 0; idx < result.pool_latency_hist.size(); ++idx) {
    result.pool_latency_hist[idx] = lhs.pool_latency_hist[idx] - rhs.pool_latency_hist[idx];
  }
  for (std::size_t idx = 0; idx < result.miss_latency_hist.size(); ++idx) {
    result.miss_latency_hist[idx] = lhs.miss_latency_hist[idx] - rhs.miss_latency_hist[idx];
  }
  return result;
}
