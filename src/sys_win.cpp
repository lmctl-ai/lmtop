#include "procfs.hpp"
#include "metrics.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lmtop {

bool sampling_supported() { return true; }

namespace {

// PDH CStatus: 0 = valid data, 1 = new data (first collect after add). Both
// are usable; anything else means the value is not ready.
constexpr uint32_t kPdhValidData = 0x00000000u;
constexpr uint32_t kPdhNewData = 0x00000001u;
bool pdh_status_ok(DWORD status) {
  return status == kPdhValidData || status == kPdhNewData;
}

// "_Total", plus the ",_total" suffix some PDH builds append to instance
// names (e.g. "0 C:,_total"). Case-insensitive, per lmdebug sample_win.cpp.
bool is_total_instance(const std::string &name) {
  std::string lower;
  lower.reserve(name.size());
  for (unsigned char c : name) lower += static_cast<char>(std::tolower(c));
  if (lower == "_total") return true;
  return lower.size() > 7 && lower.compare(lower.size() - 7, 7, ",_total") == 0;
}

// Processor instances are "N" on single-group systems and "group,N" on
// multi-socket/NUMA systems (the \Processor and \Processor Information sets
// both do this). Returns a stable core id.
bool parse_processor_instance(const std::string &name, int64_t &core_id) {
  if (name.empty() || is_total_instance(name)) return false;
  const auto comma = name.find(',');
  int group = 0, number = 0;
  auto parse_int = [](const std::string &s, int &out) {
    if (s.empty() || s.size() > 9) return false;
    int v = 0;
    for (char c : s) {
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    out = v;
    return true;
  };
  if (comma == std::string::npos) {
    if (!parse_int(name, number)) return false;
  } else if (!parse_int(name.substr(0, comma), group) ||
             !parse_int(name.substr(comma + 1), number)) {
    return false;
  }
  core_id = static_cast<int64_t>(group) * 64 + number;
  return true;
}

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

// Cumulative counters are monotone by construction; PDH pseudo-counters are
// not (the measured collect interval jitters, so llround can step a value
// down between ticks). The shared delta path rejects decreasing counters,
// so every emitted pseudo-counter is clamped to its last value per key.
class MonotonicCounters {
public:
  uint64_t next(const std::string &key, uint64_t value) {
    uint64_t &last = last_[key];
    if (value < last) value = last;
    last = value;
    return value;
  }

private:
  std::map<std::string, uint64_t> last_;
};
MonotonicCounters &monotonic() {
  static MonotonicCounters counters;
  return counters;
}
uint64_t mono_rate(const std::string &key, double rate) {
  return monotonic().next(key, pseudo_rate(rate, 1.0));
}
uint64_t mono_pct(const std::string &key, double pct) {
  return monotonic().next(key, pseudo_pct(pct, 1.0));
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
      // First collect after PdhAddCounter has no formatted data yet; a few
      // counters also lack array semantics. Either way: skip the counter
      // instead of failing the whole tick.
      if (status == PDH_INVALID_DATA) continue;
      if (status == PDH_INVALID_ARGUMENT || buffer_size == 0) {
        PDH_FMT_COUNTERVALUE value;
        if (PdhGetFormattedCounterValue(counters_[i], PDH_FMT_DOUBLE, nullptr,
                                        &value) == ERROR_SUCCESS &&
            pdh_status_ok(value.CStatus)) {
          values["_Total"] = value.doubleValue;
        }
        continue;
      }
      if (status != PDH_MORE_DATA) continue;
      std::vector<BYTE> buffer(buffer_size);
      auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buffer.data());
      if (PdhGetFormattedCounterArrayW(counters_[i], PDH_FMT_DOUBLE,
                                       &buffer_size, &item_count,
                                       items) != ERROR_SUCCESS) {
        continue;
      }
      for (DWORD k = 0; k < item_count; ++k) {
        if (!pdh_status_ok(items[k].FmtValue.CStatus)) continue;
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

PdhQuery &cpu_query(std::string &open_error) {
  static PdhQuery query;
  static const std::string init_error = [] {
    std::string error;
    query.open(
        {"\\\\Processor(_Total)\\% User Time",
         "\\\\Processor(_Total)\\% Privileged Time",
         "\\\\Processor(_Total)\\% Idle Time",
         "\\\\Processor(_Total)\\% Interrupt Time",
         "\\\\Processor(_Total)\\% DPC Time",
         "\\\\Processor(*)\\% User Time",
         "\\\\Processor(*)\\% Privileged Time",
         "\\\\Processor(*)\\% Idle Time"},
        error);
    return error;
  }();
  open_error = init_error;
  return query;
}

PdhQuery &disk_query(std::string &open_error) {
  static PdhQuery query;
  static const std::string init_error = [] {
    std::string error;
    // Utilization comes from % Idle Time, not % Disk Time: the latter is
    // unreliable (can exceed 100% on fast/RAID storage). Same choice as
    // lmdebug sample_win.cpp.
    return query.open(
        {"\\\\PhysicalDisk(*)\\Disk Read Bytes/sec",
         "\\\\PhysicalDisk(*)\\Disk Write Bytes/sec",
         "\\\\PhysicalDisk(*)\\Disk Reads/sec",
         "\\\\PhysicalDisk(*)\\Disk Writes/sec",
         "\\\\PhysicalDisk(*)\\% Idle Time"},
        error);
    return error;
  }();
  open_error = init_error;
  return query;
}

PdhQuery &net_query(std::string &open_error) {
  static PdhQuery query;
  static const std::string init_error = [] {
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
    return error;
  }();
  open_error = init_error;
  return query;
}

} // namespace

// --- CPU --------------------------------------------------------------------
// PDH percentage gauges are converted to FILETIME-scaled pseudo-counters so
// cpu_percentages normalizes them like Linux counters. iowait/PSI have no
// Windows equivalent: iowait reports 0, PSI reports missing.

bool read_cpu_snapshot(std::map<int64_t, CpuCounters> &cores, CpuCounters &total,
                       std::string &error) {
  std::vector<std::map<std::string, double>> rows;
  std::string open_error;
  auto &query = cpu_query(open_error);
  if (!open_error.empty()) {
    error = open_error;
    return false;
  }
  if (!query.collect(rows, error) || rows.size() < 8) return false;

  auto gauge = [&](size_t i, const std::string &name) -> double {
    const auto it = rows[i].find(name);
    return it == rows[i].end() ? 0.0 : it->second;
  };
  const double dt = 1.0; // values already scaled to dt by collect()

  total = CpuCounters{};
  total.values[0] = mono_pct("cpu.total.user", gauge(0, "_Total"));
  total.values[2] = mono_pct("cpu.total.priv", gauge(1, "_Total"));
  total.values[3] = mono_pct("cpu.total.idle", gauge(2, "_Total"));
  total.values[5] = mono_pct("cpu.total.irq", gauge(3, "_Total"));
  total.values[6] = mono_pct("cpu.total.dpc", gauge(4, "_Total"));

  cores.clear();
  for (const auto &[name, idle] : rows[7]) { // \Processor(*)\% Idle Time
    int64_t id = 0;
    if (!parse_processor_instance(name, id)) continue;
    CpuCounters core;
    const auto user = rows[5].find(name);
    const auto priv = rows[6].find(name);
    core.values[0] = mono_pct("cpu.core." + name + ".user",
                              user == rows[5].end() ? 0.0 : user->second);
    core.values[2] = mono_pct("cpu.core." + name + ".priv",
                              priv == rows[6].end() ? 0.0 : priv->second);
    core.values[3] = mono_pct("cpu.core." + name + ".idle", idle);
    cores[id] = core;
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
  std::string open_error;
  auto &query = disk_query(open_error);
  if (!open_error.empty()) {
    error = open_error;
    return false;
  }
  if (!query.collect(rows, error) || rows.size() < 5) return false;

  static std::map<std::string, int> name_to_slot; // stable (major, minor) keys
  out.clear();
  for (const auto &[name, read_bytes] : rows[0]) {
    if (is_total_instance(name)) continue;
    const auto &write_bytes = rows[1].count(name) ? rows[1].at(name) : 0.0;
    const auto &read_ops = rows[2].count(name) ? rows[2].at(name) : 0.0;
    const auto &write_ops = rows[3].count(name) ? rows[3].at(name) : 0.0;
    // util = 100 - idle (idle gauge already clamped by pdh_pct semantics).
    const auto idle_it = rows[4].find(name);
    const double idle = idle_it == rows[4].end() ? 100.0 : idle_it->second;
    const double util = std::clamp(100.0 - std::clamp(idle, 0.0, 100.0), 0.0, 100.0);
    const int slot = name_to_slot.emplace(name, static_cast<int>(name_to_slot.size())).first->second;
    DiskCounters counters;
    counters.name = name;
    counters.reads = mono_rate("disk." + name + ".reads", read_ops);
    counters.read_sectors = mono_rate("disk." + name + ".rbytes", read_bytes / 512.0);
    counters.writes = mono_rate("disk." + name + ".writes", write_ops);
    counters.write_sectors = mono_rate("disk." + name + ".wbytes", write_bytes / 512.0);
    counters.io_ms = mono_rate("disk." + name + ".util", util * 10000.0);
    out[{0, slot}] = counters;
  }
  return !out.empty();
}

// --- Network ----------------------------------------------------------------

bool read_net_dev(NetSnapshot &out, std::string &error) {
  std::vector<std::map<std::string, double>> rows;
  std::string open_error;
  auto &query = net_query(open_error);
  if (!open_error.empty()) {
    error = open_error;
    return false;
  }
  if (!query.collect(rows, error) || rows.size() < 8) return false;

  out.clear();
  for (const auto &[name, rx_bytes] : rows[0]) {
    if (is_total_instance(name) || name.find("Loopback") != std::string::npos)
      continue;
    auto get = [&](size_t i) -> double {
      const auto it = rows[i].find(name);
      return it == rows[i].end() ? 0.0 : it->second;
    };
    NetCounters counters;
    counters.rx_bytes = mono_rate("net." + name + ".rxbytes", rx_bytes);
    counters.rx_packets = mono_rate("net." + name + ".rxpkt", get(2));
    counters.rx_errors = mono_rate("net." + name + ".rxerr", get(4));
    counters.rx_dropped = mono_rate("net." + name + ".rxdrop", get(6));
    counters.tx_bytes = mono_rate("net." + name + ".txbytes", get(1));
    counters.tx_packets = mono_rate("net." + name + ".txpkt", get(3));
    counters.tx_errors = mono_rate("net." + name + ".txerr", get(5));
    counters.tx_dropped = mono_rate("net." + name + ".txdrop", get(7));
    out[name] = counters;
  }
  return !out.empty();
}

} // namespace lmtop

#endif // _WIN32
