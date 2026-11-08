#include "procfs.hpp"
#include "metrics.hpp"

#if defined(__linux__)
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#endif

namespace lmtop {

bool sampling_supported() { return true; }

#if defined(__linux__)
namespace {

bool u64_token(const std::string &s, uint64_t &value) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  try {
    size_t used = 0;
    value = std::stoull(s, &used);
    return used == s.size();
  } catch (...) {
    return false;
  }
}

} // namespace

// --- CPU --------------------------------------------------------------------

bool read_cpu_snapshot(std::map<int64_t, CpuCounters> &cores, CpuCounters &total) {
  std::ifstream file("/proc/stat");
  if (!file) return false;
  cores.clear();
  std::string line;
  bool have_total = false;
  while (std::getline(file, line)) {
    std::istringstream in(line);
    std::string name;
    if (!(in >> name) || name.rfind("cpu", 0) != 0) continue;
    int64_t id = -1;
    if (name != "cpu") {
      const std::string suffix = name.substr(3);
      if (suffix.empty()) return false;
      for (char c : suffix)
        if (c < '0' || c > '9') return false;
      try {
        size_t used = 0;
        id = std::stoll(suffix, &used);
        if (used != suffix.size()) return false;
      } catch (...) {
        return false;
      }
    }
    CpuCounters row;
    for (auto &value : row.values) {
      std::string token;
      if (!(in >> token) || !u64_token(token, value)) return false;
    }
    if (id < 0) {
      total = row;
      have_total = true;
    } else {
      cores[id] = row;
    }
  }
  return have_total && !cores.empty() && !file.bad();
}

// cpu_percentages and disk_delta live in src/delta.cpp (shared by all
// backends) so each per-OS file links the same helpers.

// --- PSI --------------------------------------------------------------------

double psi_avg10(const char *path, const char *kind) {
  std::ifstream file(path);
  if (!file) return missing();
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream in(line);
    std::string token;
    if (!(in >> token) || token != kind) continue;
    while (in >> token) {
      if (token.rfind("avg10=", 0) == 0) {
        try {
          size_t used = 0;
          const double value = std::stod(token.substr(6), &used);
          return used == token.size() - 6 && std::isfinite(value) && value >= 0
                     ? value
                     : missing();
        } catch (...) {
          return missing();
        }
      }
    }
  }
  return missing();
}

// --- Memory -----------------------------------------------------------------

bool read_meminfo(MemInfo &out, std::string &error) {
  std::ifstream file("/proc/meminfo");
  if (!file) {
    error = "cannot read /proc/meminfo";
    return false;
  }
  bool have_total = false, have_free = false, have_swap_total = false;
  bool have_swap_free = false, have_available = false;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream row(line);
    std::string key, number, unit;
    if (!(row >> key) || key.empty() || key.back() != ':') continue;
    key.pop_back();
    if (key != "MemTotal" && key != "MemFree" && key != "MemAvailable" &&
        key != "SwapTotal" && key != "SwapFree")
      continue;
    if (!(row >> number >> unit) || unit != "kB") {
      error = "malformed /proc/meminfo units";
      return false;
    }
    uint64_t kb = 0;
    if (!u64_token(number, kb)) {
      error = "malformed /proc/meminfo";
      return false;
    }
    const uint64_t bytes = kb * 1024;
    if (key == "MemTotal") { out.total_bytes = bytes; have_total = true; }
    if (key == "MemFree") { out.free_bytes = bytes; have_free = true; }
    if (key == "MemAvailable") { out.available_bytes = bytes; have_available = true; }
    if (key == "SwapTotal") { out.swap_total_bytes = bytes; have_swap_total = true; }
    if (key == "SwapFree") { out.swap_free_bytes = bytes; have_swap_free = true; }
  }
  if (file.bad()) {
    error = "cannot read /proc/meminfo";
    return false;
  }
  if (!have_total || !have_free || !have_swap_total) {
    error = "required memory fields missing";
    return false;
  }
  if (out.total_bytes == 0 || out.free_bytes > out.total_bytes) {
    error = "invalid memory totals";
    return false;
  }
  if (!have_available) out.available_bytes = out.free_bytes;
  if (out.available_bytes > out.total_bytes) {
    error = "invalid MemAvailable";
    return false;
  }
  if (!have_swap_free || out.swap_free_bytes > out.swap_total_bytes) {
    error = "invalid SwapFree";
    return false;
  }
  return true;
}

// --- Disk -------------------------------------------------------------------

namespace {

bool has_holders(const std::filesystem::path &path) {
  std::error_code ec;
  const std::filesystem::directory_iterator it(path / "holders", ec);
  return !ec && it != std::filesystem::directory_iterator{};
}

// Same rules as lmdebug detail::excluded (cpp/sys/disk_linux.cpp).
bool disk_excluded(const DiskCounters &d, const std::string &sys_root) {
  if (d.name.rfind("loop", 0) == 0 || d.name.rfind("ram", 0) == 0 ||
      d.name.rfind("zram", 0) == 0)
    return true;
  const auto path = std::filesystem::path(sys_root) /
                    (std::to_string(d.major) + ":" + std::to_string(d.minor));
  std::error_code ec;
  if (std::filesystem::exists(path / "partition", ec) || has_holders(path))
    return true;
  // If a partition backs a mapping, suppress its aggregate physical parent as
  // well; keep only the upper mapped row so I/O is not counted twice.
  const std::filesystem::directory_iterator end;
  for (std::filesystem::directory_iterator it(path, ec); !ec && it != end;
       it.increment(ec)) {
    std::error_code child_ec;
    if (std::filesystem::exists(it->path() / "partition", child_ec) &&
        has_holders(it->path()))
      return true;
  }
  return false;
}

} // namespace

bool read_disk_snapshot(DiskSnapshot &out, std::string &error) {
  out.clear();
  std::ifstream file("/proc/diskstats");
  if (!file) {
    error = "cannot read /proc/diskstats";
    return false;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    std::istringstream in(line);
    std::string a[14];
    bool full = true;
    for (auto &x : a) {
      if (!(in >> x)) {
        full = false;
        break;
      }
    }
    if (!full) {
      std::istringstream probe(line);
      std::string first;
      probe >> first;
      if (!first.empty() && std::isdigit(static_cast<unsigned char>(first[0]))) {
        error = "malformed /proc/diskstats";
        return false;
      }
      continue;
    }
    uint64_t v[11]{};
    bool numeric = true;
    for (int i = 0; i < 11; ++i)
      if (!u64_token(a[i + 3], v[i])) numeric = false;
    for (const char *s : {a[0].c_str(), a[1].c_str()})
      for (const char *c = s; *c; ++c)
        if (!std::isdigit(static_cast<unsigned char>(*c))) numeric = false;
    if (!numeric) continue;
    DiskCounters d;
    try {
      d.major = std::stoll(a[0]);
      d.minor = std::stoll(a[1]);
    } catch (...) {
      continue;
    }
    d.name = a[2];
    d.reads = v[0];
    d.read_sectors = v[2];
    d.read_ms = v[3];
    d.writes = v[4];
    d.write_sectors = v[6];
    d.write_ms = v[7];
    d.io_ms = v[9];
    if (disk_excluded(d, "/sys/dev/block")) continue;
    const std::string id = std::to_string(d.major) + ":" + std::to_string(d.minor);
    std::ifstream seq("/sys/dev/block/" + id + "/diskseq");
    std::string text;
    if (seq >> text) u64_token(text, d.diskseq);
    std::ifstream dm("/sys/dev/block/" + id + "/dm/name");
    std::string mapped;
    if (dm && std::getline(dm, mapped) && !mapped.empty()) d.name = mapped;
    out[{d.major, d.minor}] = d;
  }
  if (file.bad()) {
    error = "cannot read /proc/diskstats";
    return false;
  }
  return true;
}

// --- Network ----------------------------------------------------------------

bool read_net_dev(NetSnapshot &out, std::string &error) {
  std::ifstream file("/proc/net/dev");
  if (!file) {
    error = "cannot read /proc/net/dev";
    return false;
  }
  out.clear();
  std::string line;
  // Two header lines ("Inter-|..." and " face |...").
  if (!std::getline(file, line) || !std::getline(file, line)) {
    error = "malformed /proc/net/dev header";
    return false;
  }
  while (std::getline(file, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    const auto not_space = name.find_first_not_of(" \t");
    if (not_space == std::string::npos) continue;
    name = name.substr(not_space);
    std::istringstream in(line.substr(colon + 1));
    uint64_t v[16]{};
    bool ok = true;
    for (auto &x : v) {
      std::string token;
      if (!(in >> token) || !u64_token(token, x)) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      error = "malformed /proc/net/dev";
      return false;
    }
    if (name == "lo") continue; // loopback excluded, matching lmdebug links()
    NetCounters &c = out[name];
    c.rx_bytes = v[0];
    c.rx_packets = v[1];
    c.rx_errors = v[2];
    c.rx_dropped = v[3];
    c.tx_bytes = v[8];
    c.tx_packets = v[9];
    c.tx_errors = v[10];
    c.tx_dropped = v[11];
  }
  if (file.bad()) {
    error = "cannot read /proc/net/dev";
    return false;
  }
  return true;
}

#endif // __linux__

} // namespace lmtop
