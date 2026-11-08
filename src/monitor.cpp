#include "monitor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <unistd.h>
#endif

#include "stats.hpp"

namespace lmtop {
namespace {

// Missing values are emitted as -1, the lmdebug convention.
Json emit(double value) {
  return Json(std::isfinite(value) ? value : -1.0);
}

int64_t now_unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Json catalog_json() {
  Json::Array metrics;
  for (const auto &m : kCatalog)
    metrics.push_back(Json::obj({{"path", m.path},
                                 {"unit", m.unit},
                                 {"kind", m.kind},
                                 {"group", m.group}}));
  return Json(std::move(metrics));
}

} // namespace

bool Monitor::start(int64_t interval_ms, size_t cap_bytes, std::string &error) {
  interval_ = std::chrono::milliseconds(std::clamp<int64_t>(interval_ms, 100, 60000));
  cap_samples_ = std::max<size_t>(1, cap_bytes / kSampleBytes);

  started_steady_ = std::chrono::steady_clock::now();
  started_unix_ms_ = now_unix_ms();
  prev_time_ = started_steady_;

  if (!sampling_supported()) {
    supported_ = false; // server stays up; methods report the envelope
    running_ = true;
    return true;
  }

  // Baseline snapshot for the delta rates; the first Sample appears after
  // one interval, so deltas always have a measured dt.
  CpuCounters cpu_total;
  std::map<int64_t, CpuCounters> cpu_cores;
  std::string err;
  if (!read_cpu_snapshot(cpu_cores, cpu_total)) {
    error = "cannot read /proc/stat";
    return false;
  }
  DiskSnapshot disks;
  if (!read_disk_snapshot(disks, err)) err.clear(); // tolerate
  NetSnapshot links;
  if (!read_net_dev(links, err)) err.clear();       // tolerate
  cpu_count_ = static_cast<int64_t>(cpu_cores.size());

  {
    std::lock_guard<std::mutex> lock(mu_);
    prev_cpu_total_ = cpu_total;
    prev_cpu_cores_ = std::move(cpu_cores);
    prev_disks_ = std::move(disks);
    prev_links_ = std::move(links);
    have_prev_ = true;
  }
  if (!err.empty()) fprintf(stderr, "lmtop: warning: %s\n", err.c_str());

  stop_ = false;
  running_ = true;
  thread_ = std::thread([this] { loop(); });
  return true;
}

void Monitor::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void Monitor::loop() {
#if defined(__linux__)
  // Route SIGINT/SIGTERM to the server's main thread (which is blocked on the
  // stdin read): keep them blocked here for the lifetime of the thread.
  sigset_t blocked;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGINT);
  sigaddset(&blocked, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
#endif

  auto next = started_steady_ + interval_;
  while (!stop_) {
    std::this_thread::sleep_until(next);
    if (stop_) break;
    const auto t = std::chrono::steady_clock::now();
    tick(t);
    next += interval_;
    if (next <= t) {
      // Missed one or more deadlines: skip ahead, never burst-catch-up.
      const auto behind_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(t - next).count();
      const int64_t skipped = behind_ms / interval_.count() + 1;
      missed_ticks_ += skipped;
      next += std::chrono::milliseconds(skipped * interval_.count());
    }
  }
}

void Monitor::tick(const std::chrono::steady_clock::time_point &t) {
  Sample s;
  s.unix_ms = now_unix_ms();
  const double dt = std::chrono::duration<double>(t - prev_time_).count();
  s.dt_s = dt;

  CpuCounters cpu_total;
  std::map<int64_t, CpuCounters> cpu_cores;
  if (read_cpu_snapshot(cpu_cores, cpu_total)) {
    s.cpu_ok = true;
    double user = missing(), system = missing(), iowait = missing(), idle = missing();
    if (have_prev_ && dt > 0 &&
        cpu_percentages(prev_cpu_total_, cpu_total, user, system, iowait, idle)) {
      if (std::isfinite(idle)) {
        s.value[M_CPU_USAGE] = 100.0 - idle;
        s.value[M_CPU_USER] = user;
        s.value[M_CPU_SYSTEM] = system;
        s.value[M_CPU_IOWAIT] = iowait;
      }
      double max_busy = missing();
      bool any_core = false;
      for (const auto &[id, after] : cpu_cores) {
        const auto before = prev_cpu_cores_.find(id);
        double cu = 0, cs = 0, ci = 0, cidle = 0;
        if (before != prev_cpu_cores_.end() &&
            cpu_percentages(before->second, after, cu, cs, ci, cidle) &&
            std::isfinite(cidle)) {
          const double busy = 100.0 - cidle;
          max_busy = any_core ? std::max(max_busy, busy) : busy;
          any_core = true;
        }
      }
      if (any_core) s.value[M_CPU_MAX_CORE] = max_busy;
    }
    s.value[M_CPU_PSI] = psi_avg10("/proc/pressure/cpu", "some");
    {
      std::lock_guard<std::mutex> lock(mu_);
      prev_cpu_total_ = cpu_total;
      prev_cpu_cores_ = std::move(cpu_cores);
      cpu_count_ = static_cast<int64_t>(prev_cpu_cores_.size());
    }
  }

  MemInfo mem;
  std::string err;
  if (read_meminfo(mem, err)) {
    s.mem_ok = true;
    s.value[M_MEM_USED] =
        100.0 * static_cast<double>(mem.total_bytes - mem.available_bytes) /
        static_cast<double>(mem.total_bytes);
    s.value[M_MEM_AVAILABLE] = static_cast<double>(mem.available_bytes);
    if (mem.swap_total_bytes > 0)
      s.value[M_MEM_SWAP_USED] =
          100.0 *
          static_cast<double>(mem.swap_total_bytes - mem.swap_free_bytes) /
          static_cast<double>(mem.swap_total_bytes);
    s.value[M_MEM_PSI] = psi_avg10("/proc/pressure/memory", "some");
  }

  DiskSnapshot disks;
  if (read_disk_snapshot(disks, err)) {
    s.disk_ok = true;
    if (have_prev_ && dt > 0) {
      double sum_rb = 0, sum_wb = 0, sum_ri = 0, sum_wi = 0, max_util = 0;
      bool any = false;
      for (const auto &[key, after] : disks) {
        const auto before = prev_disks_.find(key);
        if (before == prev_disks_.end()) continue; // appeared mid-window
        double rb = 0, wb = 0, ri = 0, wi = 0, util = 0;
        if (!disk_delta(before->second, after, dt, rb, wb, ri, wi, util)) continue;
        sum_rb += rb;
        sum_wb += wb;
        sum_ri += ri;
        sum_wi += wi;
        max_util = any ? std::max(max_util, util) : util;
        any = true;
      }
      if (any) {
        s.value[M_DISK_READ_BPS] = sum_rb;
        s.value[M_DISK_WRITE_BPS] = sum_wb;
        s.value[M_DISK_READ_IOPS] = sum_ri;
        s.value[M_DISK_WRITE_IOPS] = sum_wi;
        s.value[M_DISK_MAX_UTIL] = max_util;
      }
    }
    s.value[M_DISK_PSI] = psi_avg10("/proc/pressure/io", "some");
    std::lock_guard<std::mutex> lock(mu_);
    prev_disks_ = std::move(disks);
  }

  NetSnapshot links;
  if (read_net_dev(links, err)) {
    s.net_ok = true;
    if (have_prev_ && dt > 0) {
      double rx_b = 0, tx_b = 0, rx_p = 0, tx_p = 0, rx_e = 0, tx_e = 0,
             rx_d = 0, tx_d = 0;
      bool any = false;
      for (const auto &[name, after] : links) {
        const auto before = prev_links_.find(name);
        if (before == prev_links_.end()) continue;
        const auto rate = [dt](uint64_t before, uint64_t after) -> double {
          if (after < before) return missing();
          return static_cast<double>(after - before) / dt;
        };
        const double vals[8] = {
            rate(before->second.rx_bytes, after.rx_bytes),
            rate(before->second.tx_bytes, after.tx_bytes),
            rate(before->second.rx_packets, after.rx_packets),
            rate(before->second.tx_packets, after.tx_packets),
            rate(before->second.rx_errors, after.rx_errors),
            rate(before->second.tx_errors, after.tx_errors),
            rate(before->second.rx_dropped, after.rx_dropped),
            rate(before->second.tx_dropped, after.tx_dropped),
        };
        bool valid = true;
        for (double v : vals)
          if (!std::isfinite(v)) valid = false;
        if (!valid) continue; // counter reset on this link: exclude interval
        rx_b += vals[0]; tx_b += vals[1]; rx_p += vals[2]; tx_p += vals[3];
        rx_e += vals[4]; tx_e += vals[5]; rx_d += vals[6]; tx_d += vals[7];
        any = true;
      }
      if (any) {
        s.value[M_NET_RX_BPS] = rx_b;
        s.value[M_NET_TX_BPS] = tx_b;
        s.value[M_NET_RX_PPS] = rx_p;
        s.value[M_NET_TX_PPS] = tx_p;
        s.value[M_NET_RX_ERR_PS] = rx_e;
        s.value[M_NET_TX_ERR_PS] = tx_e;
        s.value[M_NET_RX_DROP_PS] = rx_d;
        s.value[M_NET_TX_DROP_PS] = tx_d;
      }
    }
    std::lock_guard<std::mutex> lock(mu_);
    prev_links_ = std::move(links);
  }

  have_prev_ = true;
  prev_time_ = t;
  {
    std::lock_guard<std::mutex> lock(mu_);
    while (history_.size() >= cap_samples_) history_.pop_front();
    history_.push_back(s);
    cv_.notify_all();
  }
}

Json Monitor::status_json() const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started_steady_)
                             .count();
  std::string host = "unknown";
  int64_t pid = 0;
#if defined(__linux__)
  char buf[256] = {0};
  if (gethostname(buf, sizeof buf - 1) == 0 && buf[0] != '\0') host = buf;
  pid = static_cast<int64_t>(getpid());
#elif defined(_WIN32)
  pid = static_cast<int64_t>(GetCurrentProcessId());
#endif
  return Json::obj({
      {"protocol", "lmtop/1"},
      {"pid", pid},
      {"hostname", host},
      {"platform", platform_name()},
      {"supported", supported_},
      {"uptime_ms", uptime_ms},
      {"started_unix_ms", started_unix_ms_},
      {"interval_ms", interval_.count()},
      {"samples_kept", static_cast<int64_t>(history_.size())},
      {"missed_ticks", missed_ticks_},
      {"cpu_count", cpu_count_},
      {"sample_bytes", static_cast<int64_t>(kSampleBytes)},
      {"memory_bytes_used",
       static_cast<int64_t>(history_.size() * kSampleBytes)},
      {"memory_bytes_cap", static_cast<int64_t>(cap_samples_ * kSampleBytes)},
      {"metrics", catalog_json()},
  });
}

Json Monitor::now_json() const {
  std::unique_lock<std::mutex> lock(mu_);
  if (!supported_) {
    return Json::obj(
        {{"error", "unsupported platform: " + platform_name()}});
  }
  if (history_.empty()) {
    // Warm-up: a freshly started server has no delta window yet. Wait for the
    // first tick (bounded by ~2 intervals) so one-shot callers get a sample.
    cv_.wait_for(lock, interval_ * 2 + std::chrono::milliseconds(100),
                 [&] { return !history_.empty(); });
  }
  if (history_.empty()) return Json::obj({{"error", "no samples yet"}});
  const Sample &s = history_.back();
  Json::Object values;
  for (size_t i = 0; i < kMetricCount; ++i)
    values.emplace_back(kCatalog[i].path, emit(s.value[i]));
  return Json::obj({
      {"unix_ms", s.unix_ms},
      {"dt_s", emit(s.dt_s)},
      {"cpu_count", cpu_count_},
      {"group_status", Json::obj({{"cpu", s.cpu_ok ? "ok" : "error"},
                                  {"mem", s.mem_ok ? "ok" : "error"},
                                  {"disk", s.disk_ok ? "ok" : "error"},
                                  {"net", s.net_ok ? "ok" : "error"}})},
      {"values", Json(std::move(values))},
  });
}

Json Monitor::history_json(int64_t seconds, int64_t max_points) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!supported_) {
    return Json::obj(
        {{"error", "unsupported platform: " + platform_name()}});
  }
  Json::Array columns;
  columns.push_back(Json("unix_ms"));
  columns.push_back(Json("dt_s"));
  for (const auto &m : kCatalog) columns.push_back(Json(m.path));

  // Window: newest samples within `seconds` of the newest sample (0 = all).
  size_t total = history_.size();
  if (seconds > 0 && !history_.empty()) {
    const int64_t cutoff = history_.back().unix_ms - seconds * 1000;
    total = 0;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
      if (it->unix_ms < cutoff) break;
      ++total;
    }
  }
  const int64_t stride =
      max_points > 0
          ? std::max<int64_t>(1, (static_cast<int64_t>(total) + max_points - 1) /
                                     max_points)
          : 1;

  Json::Array rows;
  rows.reserve(static_cast<size_t>(total / static_cast<size_t>(stride)) + 1);
  const size_t first = history_.size() - total;
  for (size_t k = 0; k < total; k += static_cast<size_t>(stride)) {
    const Sample &s = history_[first + k];
    Json::Array row;
    row.push_back(Json(s.unix_ms));
    row.push_back(emit(s.dt_s));
    for (size_t i = 0; i < kMetricCount; ++i) row.push_back(emit(s.value[i]));
    rows.push_back(Json(std::move(row)));
  }
  if (total > 0 && (total - 1) % static_cast<size_t>(stride) != 0) {
    const Sample &s = history_.back();
    Json::Array row;
    row.push_back(Json(s.unix_ms));
    row.push_back(emit(s.dt_s));
    for (size_t i = 0; i < kMetricCount; ++i) row.push_back(emit(s.value[i]));
    rows.push_back(Json(std::move(row)));
  }
  return Json::obj({{"columns", Json(std::move(columns))},
                    {"stride", stride},
                    {"total_rows", static_cast<int64_t>(total)},
                    {"rows", Json(std::move(rows))}});
}

Json Monitor::summary_json(int64_t seconds) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!supported_) {
    return Json::obj(
        {{"error", "unsupported platform: " + platform_name()}});
  }
  size_t total = history_.size();
  if (seconds > 0 && !history_.empty()) {
    const int64_t cutoff = history_.back().unix_ms - seconds * 1000;
    total = 0;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
      if (it->unix_ms < cutoff) break;
      ++total;
    }
  }
  const size_t first = history_.size() - total;

  std::vector<std::vector<double>> per_metric(kMetricCount);
  for (size_t i = 0; i < kMetricCount; ++i) per_metric[i].reserve(total);
  for (size_t k = first; k < history_.size(); ++k)
    for (size_t i = 0; i < kMetricCount; ++i)
      per_metric[i].push_back(history_[k].value[i]);

  Json::Array metrics;
  for (size_t i = 0; i < kMetricCount; ++i) {
    const MetricStats st = summarize(kCatalog[i], per_metric[i]);
    metrics.push_back(Json::obj({
        {"name", st.name},
        {"unit", st.unit},
        {"kind", st.kind},
        {"n", st.n},
        {"min", emit(st.min)},
        {"mean", emit(st.mean)},
        {"median", emit(st.median)},
        {"p95", emit(st.p95)},
        {"p99", emit(st.p99)},
        {"max", emit(st.max)},
        {"stddev", emit(st.stddev)},
        {"first", emit(st.first)},
        {"last", emit(st.last)},
        {"error", st.error},
    }));
  }
  return Json::obj({
      {"window_seconds", seconds},
      {"samples", static_cast<int64_t>(total)},
      {"started_unix_ms", total ? history_[first].unix_ms : 0},
      {"ended_unix_ms", total ? history_.back().unix_ms : 0},
      {"metrics", Json(std::move(metrics))},
  });
}

} // namespace lmtop
