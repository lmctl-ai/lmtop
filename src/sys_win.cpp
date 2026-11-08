#include "procfs.hpp"
#include "metrics.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lmtop {

bool sampling_supported() { return true; }

namespace {

// PDH rates and percentage-gauges arrive between two collects; the rest of
// lmtop works on cumulative counters and one shared delta path. The bridge
// is a pseudo-counter: value * measured_dt, re-based at every collect, so
// (after - before) / dt reproduces the rate on any consumer. Percentage
// gauges scale by 1e7 (FILETIME ticks) so cpu_percentages normalizes them
// exactly as it does Linux counters.
uint64_t pseudo_rate(double rate, double dt_s) {
  if (!(rate >= 0.0) || !std::isfinite(rate)) return 0;
  return static_cast<uint64_t>(std::llround(rate * dt_s));
}
uint64_t pseudo_pct(double pct, double dt_s) {
  if (!(pct >= 0.0) || !std::isfinite(pct)) return 0;
  return static_cast<uint64_t>(std::llround(pct * 1e7 * dt_s));
}

std::string wide_to_utf8(const wchar_t *w) {
  if (!w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  return out;
}

// One PDH query over N wildcard counters. collect() returns one
// instance->value map per counter path.
class PdhQuery {
public:
  bool open(const std::vector<std::string> &paths, std::string &error) {
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
      error = "PdhOpenQueryW failed";
      return false;
    }
    for (const std::string &path : paths) {
      std::wstring wide(path.begin(), path.end());
      PDH_HCOUNTER counter = nullptr;
      if (PdhAddEnglishCounterW(query_, wide.c_str(), 0, &counter) !=
          ERROR_SUCCESS) {
        error = "PdhAddEnglishCounterW failed: " + path;
        close();
        return false;
      }
      counters_.push_back(counter);
    }
    return true;
  }

  // Collects and formats all counters. The first collect after open is the
  // baseline (dt = 0, values all zero); rates exist from the second collect.
  bool collect(std::vector<std::map<std::string, double>> &per_counter,
               std::string &error) {
    per_counter.assign(counters_.size(), {});
    const auto now = std::chrono::steady_clock::now();
    double dt_s = 0.0;
    if (!first_) dt_s = std::chrono::duration<double>(now - prev_).count();
    prev_ = now;
    first_ = false;

    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) {
      error = "PdhCollectQueryData failed";
      return false;
    }
    for (size_t i = 0; i < counters_.size(); ++i) {
      std::map<std::string, double> &values = per_counter[i];
      DWORD buffer_size = 0, item_count = 0;
      const PDH_STATUS status = PdhGetFormattedCounterArrayW(
          counters_[i], PDH_FMT_DOUBLE, &buffer_size, &item_count, nullptr);
      if (status == PDH_INVALID_ARGUMENT || buffer_size == 0) {
        // Non-wildcard counter: single formatted value.
        PDH_FMT_COUNTERVALUE value;
        if (PdhGetFormattedCounterValue(counters_[i], PDH_FMT_DOUBLE, nullptr,
                                        &value) == ERROR_SUCCESS &&
            value.CStatus == ERROR_SUCCESS) {
          values["_Total"] = value.doubleValue;
        }
        continue;
      }
      if (status != PDH_MORE_DATA) {
        error = "PdhGetFormattedCounterArrayW failed";
        return false;
      }
      std::vector<BYTE> buffer(buffer_size);
      auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
      if (PdhGetFormattedCounterArrayW(counters_[i], PDH_FMT_DOUBLE,
                                       &buffer_size, &item_count,
                                       items) != ERROR_SUCCESS) {
        error = "PdhGetFormattedCounterArrayW failed";
        return false;
      }
      for (DWORD k = 0; k < item_count; ++k) {
        if (items[k].FmtValue.CStatus != ERROR_SUCCESS) continue;
        values[wide_to_utf8(items[k].szName)] = items[k].FmtValue.doubleValue;
      }
    }
    // Scale into rate space with the measured interval.
    if (dt_s > 0.0) {
      for (auto &values : per_counter)
        for (auto &[name, value] : values) value *= dt_s;
    } else {
      for (auto &values : per_counter)
        for (auto &[name, value] : values) value = 0.0;
    }
    return true;
  }

  void close() {
    if (query_) PdhCloseQuery(query_);
    query_ = nullptr;
    counters_.clear();
  }

private:
  PDH_HQUERY query_ = nullptr;
  std::vector<PDH_HCOUNTER> counters_;
  bool first_ = true;
  std::chrono::steady_clock::time_point prev_{};
};

PdhQuery &cpu_query() {
  static PdhQuery query;
  static bool opened = [] {
    std::string error;
    return query.open(
        {"\\\\Processor(_Total)\\% User Time",
         "\\\\Processor(_Total)\\% Privileged Time",
         "\\\\Processor(_Total)\\% Idle Time",
         "\\\\Processor(_Total)\\% Interrupt Time",
         "\\\\Processor(_Total)\\% DPC Time",
         "\\\\Processor(*)\\% User Time",
         "\\\\Processor(*)\\% Privileged Time",
         "\\\\Processor(*)\\% Idle Time"},
        error);
  }();
  (void)opened;
  return query;
}

PdhQuery &disk_query() {
  static PdhQuery query;
  static bool opened = [] {
    std::string error;
    return query.open(
        {"\\\\PhysicalDisk(*)\\Disk Read Bytes/sec",
         "\\\\PhysicalDisk(*)\\Disk Write Bytes/sec",
         "\\\\PhysicalDisk(*)\\Disk Reads/sec",
         "\\\\PhysicalDisk(*)\\Disk Writes/sec",
         "\\\\PhysicalDisk(*)\\% Disk Time"},
        error);
  }();
  (void)opened;
  return query;
}

PdhQuery &net_query() {
  static PdhQuery query;
  static bool opened = [] {
    std::string error;
    return query.open(
        {"\\\\Network Interface(*)\\Bytes Received/sec",
         "\\\\Network Interface(*)\\Bytes Sent/sec",
         "\\\\Network Interface(*)\\Packets Received/sec",
         "\\\\Network Interface(*)\\Packets Sent/sec",
         "\\\\Network Interface(*)\\Packets Received Errors",
         "\\\\Network Interface(*)\\Packets Outbound Errors",
         "\\\\Network Interface(*)\\Packets Received Discarded",
         "\\\\Network Interface(*)\\Packets Outbound Discarded"},
        error);
  }();
  (void)opened;
  return query;
}

} // namespace

// --- CPU --------------------------------------------------------------------
// PDH percentage gauges are converted to FILETIME-scaled pseudo-counters so
// cpu_percentages normalizes them like Linux counters. iowait/PSI have no
// Windows equivalent: iowait reports 0, PSI reports missing.

bool read_cpu_snapshot(std::map<int64_t, CpuCounters> &cores, CpuCounters &total) {
  std::vector<std::map<std::string, double>> rows;
  std::string error;
  if (!cpu_query().collect(rows, error) || rows.size() < 8) return false;

  auto gauge = [&](size_t i, const std::string &name) -> double {
    const auto it = rows[i].find(name);
    return it == rows[i].end() ? 0.0 : it->second;
  };
  const double dt = 1.0; // values already scaled to dt by collect()

  total = CpuCounters{};
  total.values[0] = pseudo_pct(gauge(0, "_Total"), 1.0);   // user
  total.values[2] = pseudo_pct(gauge(1, "_Total"), 1.0);   // privileged
  total.values[3] = pseudo_pct(gauge(2, "_Total"), 1.0);   // idle
  total.values[5] = pseudo_pct(gauge(3, "_Total"), 1.0);   // interrupt
  total.values[6] = pseudo_pct(gauge(4, "_Total"), 1.0);   // dpc

  cores.clear();
  for (const auto &[name, idle] : rows[7]) { // \Processor(*)\% Idle Time
    if (name == "_Total") continue;
    try {
      size_t used = 0;
      const int64_t id = std::stoll(name, &used);
      if (used != name.size()) continue;
      CpuCounters core;
      const auto user = rows[5].find(name);
      const auto priv = rows[6].find(name);
      core.values[0] =
          pseudo_pct(user == rows[5].end() ? 0.0 : user->second, 1.0);
      core.values[2] =
          pseudo_pct(priv == rows[6].end() ? 0.0 : priv->second, 1.0);
      core.values[3] = pseudo_pct(idle, 1.0);
      cores[id] = core;
    } catch (...) {
      continue;
    }
  }
  (void)dt;
  return !cores.empty();
}

double psi_avg10(const char *, const char *) { return missing(); }

// --- Memory -----------------------------------------------------------------

bool read_meminfo(MemInfo &out, std::string &error) {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status) == FALSE) {
    error = "GlobalMemoryStatusEx failed";
    return false;
  }
  out.total_bytes = status.ullTotalPhys;
  out.free_bytes = 0; // not exposed; available is the meaningful gauge
  out.available_bytes = status.ullAvailPhys;
  const uint64_t swap_total =
      status.ullTotalPageFile - status.ullTotalPhys; // pagefile beyond RAM
  out.swap_total_bytes = swap_total;
  const uint64_t swap_free =
      status.ullAvailPageFile > status.ullAvailPhys
          ? status.ullAvailPageFile - status.ullAvailPhys
          : 0;
  out.swap_free_bytes = swap_free > swap_total ? swap_total : swap_free;
  return true;
}

// --- Disk -------------------------------------------------------------------
// PDH per-disk rates -> per-tick pseudo counters.

bool read_disk_snapshot(DiskSnapshot &out, std::string &error) {
  std::vector<std::map<std::string, double>> rows;
  if (!disk_query().collect(rows, error) || rows.size() < 5) return false;

  static std::map<std::string, int> name_to_slot; // stable (major, minor) keys
  out.clear();
  for (const auto &[name, read_bytes] : rows[0]) {
    if (name == "_Total") continue;
    const auto &write_bytes = rows[1].count(name) ? rows[1].at(name) : 0.0;
    const auto &read_ops = rows[2].count(name) ? rows[2].at(name) : 0.0;
    const auto &write_ops = rows[3].count(name) ? rows[3].at(name) : 0.0;
    const auto &util = rows[4].count(name) ? rows[4].at(name) : 0.0;
    const int slot = name_to_slot.emplace(name, static_cast<int>(name_to_slot.size())).first->second;
    DiskCounters counters;
    counters.name = name;
    counters.reads = pseudo_rate(read_ops, 1.0);
    counters.read_sectors = pseudo_rate(read_bytes / 512.0, 1.0);
    counters.writes = pseudo_rate(write_ops, 1.0);
    counters.write_sectors = pseudo_rate(write_bytes / 512.0, 1.0);
    counters.io_ms = pseudo_rate(util * 10000.0, 1.0);
    out[{0, slot}] = counters;
  }
  return !out.empty();
}

// --- Network ----------------------------------------------------------------

bool read_net_dev(NetSnapshot &out, std::string &error) {
  std::vector<std::map<std::string, double>> rows;
  if (!net_query().collect(rows, error) || rows.size() < 8) return false;

  out.clear();
  for (const auto &[name, rx_bytes] : rows[0]) {
    if (name == "_Total" || name.find("Loopback") != std::string::npos) continue;
    auto get = [&](size_t i) -> double {
      const auto it = rows[i].find(name);
      return it == rows[i].end() ? 0.0 : it->second;
    };
    NetCounters counters;
    counters.rx_bytes = pseudo_rate(rx_bytes, 1.0);
    counters.rx_packets = pseudo_rate(get(2), 1.0);
    counters.rx_errors = pseudo_rate(get(4), 1.0);
    counters.rx_dropped = pseudo_rate(get(6), 1.0);
    counters.tx_bytes = pseudo_rate(get(1), 1.0);
    counters.tx_packets = pseudo_rate(get(3), 1.0);
    counters.tx_errors = pseudo_rate(get(5), 1.0);
    counters.tx_dropped = pseudo_rate(get(7), 1.0);
    out[name] = counters;
  }
  return !out.empty();
}

} // namespace lmtop

#endif // _WIN32
