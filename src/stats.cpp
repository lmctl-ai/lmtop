#include "stats.hpp"

#include <algorithm>
#include <cmath>

namespace lmtop {

double quantile_type7(std::vector<double> sorted, double q) {
  if (sorted.empty()) return missing();
  std::sort(sorted.begin(), sorted.end());
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  const double frac = pos - static_cast<double>(lo);
  if (lo + 1 < sorted.size())
    return sorted[lo] + frac * (sorted[lo + 1] - sorted[lo]);
  return sorted[lo];
}

MetricStats summarize(const MetricInfo &info, const std::vector<double> &values) {
  MetricStats out;
  out.name = info.path;
  out.unit = info.unit;
  out.kind = info.kind;

  std::vector<double> valid;
  valid.reserve(values.size());
  for (double v : values)
    if (std::isfinite(v)) valid.push_back(v);
  out.n = static_cast<int64_t>(valid.size());
  if (valid.empty()) {
    out.error = "no valid samples";
    return out;
  }

  out.min = *std::min_element(valid.begin(), valid.end());
  out.max = *std::max_element(valid.begin(), valid.end());
  out.first = valid.front();
  out.last = valid.back();

  double mean = 0.0, m2 = 0.0;
  for (size_t i = 0; i < valid.size(); ++i) {
    const double delta = valid[i] - mean;
    mean += delta / static_cast<double>(i + 1);
    m2 += delta * (valid[i] - mean);
  }
  out.mean = mean;
  out.stddev = valid.size() > 1
                   ? std::sqrt(m2 / static_cast<double>(valid.size() - 1))
                   : 0.0;

  out.median = quantile_type7(valid, 0.50);
  out.p95 = quantile_type7(valid, 0.95);
  out.p99 = quantile_type7(valid, 0.99);
  return out;
}

} // namespace lmtop
