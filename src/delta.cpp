// Platform-independent delta helpers shared by every sys backend
// (Linux /proc counters, Windows PDH pseudo-counters, macOS mach counters).
// Kept out of the per-OS files so each backend links the same functions.

#include "procfs.hpp"
#include "metrics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lmtop {

bool cpu_percentages(const CpuCounters &before, const CpuCounters &after,
                     double &user, double &system, double &iowait, double &idle) {
  uint64_t d[8]{};
  for (int i = 0; i < 8; ++i) {
    if (after.values[i] < before.values[i]) {
      if (i == 4) d[i] = 0; // iowait can legitimately decrease
      else return false;
    } else d[i] = after.values[i] - before.values[i];
  }
  uint64_t total = 0;
  for (auto value : d) {
    if (std::numeric_limits<uint64_t>::max() - total < value) return false;
    total += value;
  }
  if (total == 0) {
    user = system = iowait = idle = missing();
    return true;
  }
  const auto pct = [total](uint64_t value) {
    return 100.0 * static_cast<double>(value) / static_cast<double>(total);
  };
  user = pct(d[0] + d[1]);
  system = pct(d[2] + d[5] + d[6] + d[7]);
  iowait = pct(d[4]);
  idle = pct(d[3]);
  return true;
}

bool disk_delta(const DiskCounters &before, const DiskCounters &after,
                double seconds, double &read_bps, double &write_bps,
                double &read_iops, double &write_iops, double &util_pct) {
  if (!std::isfinite(seconds) || seconds <= 0 || before.major != after.major ||
      before.minor != after.minor || before.name != after.name ||
      before.diskseq != after.diskseq || after.reads < before.reads ||
      after.writes < before.writes || after.read_sectors < before.read_sectors ||
      after.write_sectors < before.write_sectors || after.read_ms < before.read_ms ||
      after.write_ms < before.write_ms || after.io_ms < before.io_ms)
    return false;
  const double dr = static_cast<double>(after.reads - before.reads);
  const double dw = static_cast<double>(after.writes - before.writes);
  if (!std::isfinite(dr / seconds) || !std::isfinite(dw / seconds)) return false;
  read_bps = static_cast<double>(after.read_sectors - before.read_sectors) * 512.0 / seconds;
  write_bps = static_cast<double>(after.write_sectors - before.write_sectors) * 512.0 / seconds;
  read_iops = dr / seconds;
  write_iops = dw / seconds;
  util_pct = std::min(100.0, 100.0 * static_cast<double>(after.io_ms - before.io_ms) /
                                 (seconds * 1000.0));
  return true;
}

} // namespace lmtop
