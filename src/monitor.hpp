#pragma once
// The monitor: a background thread samples cpu/mem/disk/net on a fixed
// schedule and appends fixed-size records to an in-memory ring buffer capped
// at a byte budget (default ~100 MB). Response builders run under the same
// lock, so the stdio server thread always sees a consistent snapshot.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "json.hpp"
#include "metrics.hpp"
#include "procfs.hpp"

namespace lmtop {

class Monitor {
public:
  // interval_ms is clamped to [100, 60000]. cap_bytes bounds the history ring.
  bool start(int64_t interval_ms, size_t cap_bytes, std::string &error);
  void stop();
  bool running() const { return running_; }

  // All builders return one-line-ready Json objects.
  Json status_json() const;
  Json now_json() const;                        // {"error": ...} when warming up
  Json history_json(int64_t seconds, int64_t max_points) const;
  Json summary_json(int64_t seconds) const;

private:
  void loop();
  void tick(const std::chrono::steady_clock::time_point &t);

  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  bool running_ = false;

  std::chrono::milliseconds interval_{1000};
  std::chrono::steady_clock::time_point started_steady_{};
  int64_t started_unix_ms_ = 0;

  std::deque<Sample> history_;
  size_t cap_samples_ = 0;
  int64_t missed_ticks_ = 0;
  int64_t cpu_count_ = 0;

  // Previous snapshots for delta rates; validity flags mirror "have baseline".
  CpuCounters prev_cpu_total_{};
  std::map<int64_t, CpuCounters> prev_cpu_cores_;
  DiskSnapshot prev_disks_;
  NetSnapshot prev_links_;
  bool have_prev_ = false;
  std::chrono::steady_clock::time_point prev_time_{};
};

} // namespace lmtop
