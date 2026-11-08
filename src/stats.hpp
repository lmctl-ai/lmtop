#pragma once
// Summary statistics over a window of samples, following lmdebug's
// sys-sampling design (docs/design/sys-sampling.md §4.4): NaN values are
// skipped, stddev is the sample stddev (Welford), percentiles use
// linear interpolation between closest ranks (Hyndman-Fan type 7).

#include <cstdint>
#include <string>
#include <vector>

#include "metrics.hpp"

namespace lmtop {

struct MetricStats {
  std::string name, unit, kind;
  int64_t n = 0;
  double min = -1, mean = -1, median = -1, p95 = -1, p99 = -1, max = -1;
  double stddev = -1, first = -1, last = -1;
  std::string error; // "" or "no valid samples"
};

double quantile_type7(std::vector<double> sorted, double q);

// `values` may contain NaN (missing); those entries are excluded.
MetricStats summarize(const MetricInfo &info, const std::vector<double> &values);

} // namespace lmtop
