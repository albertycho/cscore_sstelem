#include "core_stats.h"

cpu_stats operator-(cpu_stats lhs, cpu_stats rhs)
{
  lhs.begin_instrs -= rhs.begin_instrs;
  lhs.begin_cycles -= rhs.begin_cycles;
  lhs.end_instrs -= rhs.end_instrs;
  lhs.end_cycles -= rhs.end_cycles;
  lhs.total_rob_occupancy_at_branch_mispredict -= rhs.total_rob_occupancy_at_branch_mispredict;
  lhs.load_issue_to_complete_sum_cycles -= rhs.load_issue_to_complete_sum_cycles;
  lhs.load_issue_to_complete_count -= rhs.load_issue_to_complete_count;
  lhs.retired_load_ops -= rhs.retired_load_ops;
  lhs.retired_store_ops -= rhs.retired_store_ops;

  lhs.total_branch_types -= rhs.total_branch_types;
  lhs.branch_type_misses -= rhs.branch_type_misses;

  return lhs;
}
