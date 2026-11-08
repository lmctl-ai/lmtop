#include "procfs.hpp"
#include "metrics.hpp"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>

#include <ifaddrs.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace lmtop {

bool sampling_supported() { return true; }

// --- CPU (host_processor_info) ----------------------------------------------
// cpu_ticks = { user, system, idle, nice } (CPU_STATE_*), cumulative.

bool read_cpu_snapshot(std::map<int64_t, CpuCounters> &cores, CpuCounters &total) {
  natural_t cpu_count = 0;
  processor_info_array_t info = nullptr;
  mach_msg_type_number_t info_count = 0;
  if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cpu_count,
                          &info, &info_count) != KERN_SUCCESS)
    return false;
  const auto *loads = reinterpret_cast<processor_cpu_load_info_data_t *>(info);
  total = CpuCounters{};
  for (natural_t i = 0; i < cpu_count; ++i) {
    CpuCounters core;
    // mach order is USER, SYSTEM, IDLE, NICE; our layout is
    // user, nice, system, idle (slots 4..7 stay zero -> iowait reports 0).
    core.values[0] = loads[i].cpu_ticks[CPU_STATE_USER];
    core.values[1] = loads[i].cpu_ticks[CPU_STATE_NICE];
    core.values[2] = loads[i].cpu_ticks[CPU_STATE_SYSTEM];
    core.values[3] = loads[i].cpu_ticks[CPU_STATE_IDLE];
    const int64_t id = static_cast<int64_t>(i);
    cores[id] = core;
    for (int s = 0; s < 8; ++s)
      total.values[static_cast<size_t>(s)] += core.values[static_cast<size_t>(s)];
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info),
                info_count * sizeof(natural_t));
  return cpu_count > 0;
}

// No per-CPU iowait on macOS: cpu.iowait_pct reports 0 (CPU_STATE slots 4..7
// stay zero, which cpu_percentages turns into iowait=0).

// --- Memory (sysctl + host_statistics64) ------------------------------------
// Available = free + inactive (documented approximation; purgeable/wired are
// not counted as available). Swap from vm.swapusage.

bool read_meminfo(MemInfo &out, std::string &error) {
  uint64_t total = 0;
  size_t len = sizeof(total);
  const int mib[2] = {CTL_HW, HW_MEMSIZE};
  if (sysctl(mib, 2, &total, &len, nullptr, 0) != 0) {
    error = "sysctl HW_MEMSIZE failed";
    return false;
  }
  vm_size_t page = 0;
  if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS || page == 0) {
    error = "host_page_size failed";
    return false;
  }
  vm_statistics64_data_t vm;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm),
                        &count) != KERN_SUCCESS) {
    error = "host_statistics64 failed";
    return false;
  }
  out.total_bytes = total;
  out.free_bytes = static_cast<uint64_t>(vm.free_count) * page;
  out.available_bytes =
      (static_cast<uint64_t>(vm.free_count) + vm.inactive_count) * page;
  struct xsw_usage swap {};
  size_t swap_len = sizeof(swap);
  if (sysctlbyname("vm.swapusage", &swap, &swap_len, nullptr, 0) == 0) {
    out.swap_total_bytes = swap.xsu_total;
    out.swap_free_bytes = swap.xsu_avail;
  } else {
    out.swap_total_bytes = 0;
    out.swap_free_bytes = 0; // monitor reports -1
  }
  return true;
}

// --- Disk (IOKit IOBlockStorageDriver statistics) ---------------------------
// Bytes/operations are cumulative per driver instance. macOS exposes no
// public per-disk busy time, so util_supported stays false and
// disk.max_util_pct reports -1 rather than a fabricated 0.

namespace {

// Modern SDKs no longer export these as macros; the dictionary keys are the
// stable string literals documented in IOKit (IOBlockStorageDriver.h).
const CFStringRef kStatsKeyBytesRead = CFSTR("Bytes (Read)");
const CFStringRef kStatsKeyBytesWritten = CFSTR("Bytes (Written)");
const CFStringRef kStatsKeyOpsRead = CFSTR("Operations (Read)");
const CFStringRef kStatsKeyOpsWritten = CFSTR("Operations (Written)");

uint64_t stat_u64(CFDictionaryRef dict, CFStringRef key) {
  const CFNumberRef value =
      static_cast<CFNumberRef>(CFDictionaryGetValue(dict, key));
  uint64_t out = 0;
  if (value) CFNumberGetValue(value, kCFNumberSInt64Type, &out);
  return out;
}

} // namespace

bool read_disk_snapshot(DiskSnapshot &out, std::string &error) {
  out.clear();
  io_iterator_t iter = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("IOBlockStorageDriver"),
                                   &iter) != KERN_SUCCESS) {
    error = "IOServiceGetMatchingServices failed";
    return false;
  }
  int index = 0;
  io_service_t device = 0;
  while ((device = IOIteratorNext(iter)) != 0) {
    CFTypeRef prop = IORegistryEntryCreateCFProperty(
        device, CFSTR(kIOBlockStorageDriverStatisticsKey), kCFAllocatorDefault,
        0);
    if (prop) {
      if (CFGetTypeID(prop) == CFDictionaryGetTypeID()) {
        const CFDictionaryRef dict = static_cast<CFDictionaryRef>(prop);
        DiskCounters counters;
        counters.util_supported = false;
        char name[128] = {0};
        if (CFStringRef bsd = static_cast<CFStringRef>(
                IORegistryEntryCreateCFProperty(device, CFSTR(kIOBSDNameKey),
                                                kCFAllocatorDefault, 0))) {
          CFStringGetCString(bsd, name, sizeof name, kCFStringEncodingUTF8);
          CFRelease(bsd);
        } else {
          std::snprintf(name, sizeof name, "disk%d", index);
        }
        counters.name = name;
        counters.reads = stat_u64(dict, kStatsKeyOpsRead);
        counters.read_sectors = stat_u64(dict, kStatsKeyBytesRead) / 512;
        counters.writes = stat_u64(dict, kStatsKeyOpsWritten);
        counters.write_sectors = stat_u64(dict, kStatsKeyBytesWritten) / 512;
        out[{0, index++}] = counters;
      }
      CFRelease(prop);
    }
    IOObjectRelease(device);
  }
  IOObjectRelease(iter);
  return !out.empty();
}

// --- Network (getifaddrs, AF_LINK statistics) -------------------------------

bool read_net_dev(NetSnapshot &out, std::string &error) {
  ifaddrs *interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    error = "getifaddrs failed";
    return false;
  }
  out.clear();
  for (const ifaddrs *ifa = interfaces; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
    if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
    if (!ifa->ifa_data) continue;
    const std::string name = ifa->ifa_name;
    if (out.count(name) != 0) continue; // first AF_LINK entry wins
    const auto *data = static_cast<const if_data *>(ifa->ifa_data);
    NetCounters counters;
    counters.rx_bytes = data->ifi_ibytes;
    counters.rx_packets = data->ifi_ipackets;
    counters.rx_errors = data->ifi_ierrors;
    counters.rx_dropped = data->ifi_iqdrops;
    counters.tx_bytes = data->ifi_obytes;
    counters.tx_packets = data->ifi_opackets;
    counters.tx_errors = data->ifi_oerrors;
    counters.tx_dropped = 0; // Darwin's if_data tracks no TX-drop counter
    out[name] = counters;
  }
  freeifaddrs(interfaces);
  return true;
}

// No PSI on macOS: the gauges report missing (-1).

double psi_avg10(const char *, const char *) { return missing(); }

} // namespace lmtop

#endif // __APPLE__
