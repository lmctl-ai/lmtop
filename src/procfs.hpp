#pragma once
// Linux resource readers over /proc and /sys, ported from lmdebug
// (cpp/sys/cpu_linux.cpp, mem_linux.cpp, disk_linux.cpp, net link counters).
// All readers take a single snapshot; the monitor computes deltas.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace lmtop {

// --- Platform ---------------------------------------------------------------

// "linux" | "windows" | "macos" | "unknown".
inline std::string platform_name() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

// True when this build can sample (all OSes; readers differ per platform).
bool sampling_supported();

// --- CPU (/proc/stat) -------------------------------------------------------

// user nice system idle iowait irq softirq steal
struct CpuCounters {
  std::array<uint64_t, 8> values{};
};

bool read_cpu_snapshot(std::map<int64_t, CpuCounters> &cores, CpuCounters &total,
                       std::string &error);

// Percentages over a delta window. user = user+nice, system = system+irq+
// softirq+steal (lmdebug sys cpu normalization). Returns false when a counter
// decreased (reset/wrap) and the interval is invalid.
bool cpu_percentages(const CpuCounters &before, const CpuCounters &after,
                     double &user, double &system, double &iowait, double &idle);

// --- PSI (/proc/pressure/*) -------------------------------------------------

// The avg10= value of the "some"/"full" line, or NaN when unavailable.
double psi_avg10(const char *path, const char *kind);

// --- Memory (/proc/meminfo) -------------------------------------------------

struct MemInfo {
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  uint64_t available_bytes = 0;
  uint64_t swap_total_bytes = 0;
  uint64_t swap_free_bytes = 0;
};

// Returns false and fills `error` on a malformed/unreadable file.
bool read_meminfo(MemInfo &out, std::string &error);

// --- Disk (/proc/diskstats + /sys/dev/block eligibility) --------------------

struct DiskCounters {
  int64_t major = 0, minor = 0;
  std::string name;
  uint64_t reads = 0, read_sectors = 0, read_ms = 0;
  uint64_t writes = 0, write_sectors = 0, write_ms = 0;
  uint64_t io_ms = 0;
  uint64_t diskseq = 0;  // identity guard, 0 when /sys has none
  bool util_supported = true; // false where the OS exposes no per-disk busy
};

using DiskSnapshot = std::map<std::pair<int64_t, int64_t>, DiskCounters>;

// Eligibility matches lmdebug sys disk: loop/ram/zram rows, partitions, and
// stacked devices (a partition backing a mapper, or its aggregate parent) are
// excluded so I/O is never counted twice.
bool read_disk_snapshot(DiskSnapshot &out, std::string &error);

// Rates over `seconds` between two snapshots of one device. Returns false when
// the interval is invalid (counter decrease, identity change).
bool disk_delta(const DiskCounters &before, const DiskCounters &after,
                double seconds, double &read_bps, double &write_bps,
                double &read_iops, double &write_iops, double &util_pct);

// --- Network (/proc/net/dev) ------------------------------------------------

struct NetCounters {
  uint64_t rx_bytes = 0, rx_packets = 0, rx_errors = 0, rx_dropped = 0;
  uint64_t tx_bytes = 0, tx_packets = 0, tx_errors = 0, tx_dropped = 0;
};

// Loopback links are excluded, matching lmdebug's link eligibility.
using NetSnapshot = std::map<std::string, NetCounters>;

bool read_net_dev(NetSnapshot &out, std::string &error);

} // namespace lmtop
