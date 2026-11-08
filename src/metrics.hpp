#pragma once
// Metric catalog and the in-memory sample type.
// The catalog mirrors lmdebug's sys counter paths (docs/design/sys-sampling.md
// §12.3/§12.4): host-normalized percentages, bytes (never KB/MB), per-second
// rates. A sample is a fixed-size record; NaN marks a missing value and is
// emitted as -1 in JSON, the lmdebug convention.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace lmtop {

struct MetricInfo {
  const char *path;  // e.g. "cpu.usage_pct"
  const char *unit;  // "pct" | "bytes" | "bytes/s" | "1/s"
  const char *kind;  // "gauge" | "rate"
  const char *group; // "cpu" | "mem" | "disk" | "net"
};

// Keep this table in sync with MetricId below.
inline constexpr MetricInfo kCatalog[] = {
    {"cpu.usage_pct", "pct", "rate", "cpu"},
    {"cpu.user_pct", "pct", "rate", "cpu"},
    {"cpu.system_pct", "pct", "rate", "cpu"},
    {"cpu.iowait_pct", "pct", "rate", "cpu"},
    {"cpu.max_core_busy_pct", "pct", "rate", "cpu"},
    {"cpu.psi_some_avg10", "pct", "gauge", "cpu"},
    {"mem.used_pct", "pct", "gauge", "mem"},
    {"mem.available_bytes", "bytes", "gauge", "mem"},
    {"mem.swap_used_pct", "pct", "gauge", "mem"},
    {"mem.psi_some_avg10", "pct", "gauge", "mem"},
    {"disk.read_bytes_rate", "bytes/s", "rate", "disk"},
    {"disk.write_bytes_rate", "bytes/s", "rate", "disk"},
    {"disk.read_ops_rate", "1/s", "rate", "disk"},
    {"disk.write_ops_rate", "1/s", "rate", "disk"},
    {"disk.max_util_pct", "pct", "rate", "disk"},
    {"disk.psi_some_avg10", "pct", "gauge", "disk"},
    {"net.rx_bytes_rate", "bytes/s", "rate", "net"},
    {"net.tx_bytes_rate", "bytes/s", "rate", "net"},
    {"net.rx_packets_rate", "1/s", "rate", "net"},
    {"net.tx_packets_rate", "1/s", "rate", "net"},
    {"net.rx_errors_rate", "1/s", "rate", "net"},
    {"net.tx_errors_rate", "1/s", "rate", "net"},
    {"net.rx_dropped_rate", "1/s", "rate", "net"},
    {"net.tx_dropped_rate", "1/s", "rate", "net"},
};

inline constexpr size_t kMetricCount = sizeof(kCatalog) / sizeof(kCatalog[0]);

enum MetricId : size_t {
  M_CPU_USAGE = 0,
  M_CPU_USER,
  M_CPU_SYSTEM,
  M_CPU_IOWAIT,
  M_CPU_MAX_CORE,
  M_CPU_PSI,
  M_MEM_USED,
  M_MEM_AVAILABLE,
  M_MEM_SWAP_USED,
  M_MEM_PSI,
  M_DISK_READ_BPS,
  M_DISK_WRITE_BPS,
  M_DISK_READ_IOPS,
  M_DISK_WRITE_IOPS,
  M_DISK_MAX_UTIL,
  M_DISK_PSI,
  M_NET_RX_BPS,
  M_NET_TX_BPS,
  M_NET_RX_PPS,
  M_NET_TX_PPS,
  M_NET_RX_ERR_PS,
  M_NET_TX_ERR_PS,
  M_NET_RX_DROP_PS,
  M_NET_TX_DROP_PS,
};

inline double missing() { return std::numeric_limits<double>::quiet_NaN(); }

struct Sample {
  int64_t unix_ms = 0;       // wall clock at the tick
  double dt_s = 0;           // measured seconds since the previous snapshot
  std::array<double, kMetricCount> value{};
  std::string cpu_error;     // "" = ok; the reader's message otherwise
  std::string mem_error;
  std::string disk_error;
  std::string net_error;

  Sample() { value.fill(missing()); }
};

inline constexpr size_t kSampleBytes = sizeof(Sample);

} // namespace lmtop
