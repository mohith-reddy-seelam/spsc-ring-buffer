// SPDX-License-Identifier: MIT
#include "platform.hpp"

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#ifndef SPSC_BUILD_FLAGS
#define SPSC_BUILD_FLAGS "unknown"
#endif

namespace harness {
namespace {

void cpuid(unsigned leaf, unsigned regs[4]) {
#if defined(_MSC_VER)
  __cpuid(reinterpret_cast<int*>(regs), static_cast<int>(leaf));
#else
  __cpuid(leaf, regs[0], regs[1], regs[2], regs[3]);
#endif
}

std::string cpu_brand_string() {
  unsigned regs[4];
  cpuid(0x80000000u, regs);
  if (regs[0] < 0x80000004u) return "unknown";
  char brand[49] = {};
  for (unsigned i = 0; i < 3; ++i) {
    cpuid(0x80000002u + i, regs);
    std::memcpy(brand + i * 16, regs, 16);
  }
  std::string s(brand);
  const auto first = s.find_first_not_of(' ');
  return first == std::string::npos ? s : s.substr(first);
}

// CPUID.80000007H:EDX[8] - TSC runs at a constant rate in all ACPI P/C-states.
bool has_invariant_tsc() {
  unsigned regs[4];
  cpuid(0x80000000u, regs);
  if (regs[0] < 0x80000007u) return false;
  cpuid(0x80000007u, regs);
  return (regs[3] >> 8) & 1u;
}

std::string os_string() {
#if defined(_WIN32)
  // GetVersionEx lies on Windows 10+, RtlGetVersion does not.
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW vi{};
  vi.dwOSVersionInfoSize = sizeof(vi);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (fn != nullptr && fn(&vi) == 0) {
      std::ostringstream os;
      os << (vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10") << " build "
         << vi.dwBuildNumber;
      return os.str();
    }
  }
  return "Windows";
#else
  utsname u{};
  if (uname(&u) != 0) return "Linux";
  return std::string(u.sysname) + " " + u.release;
#endif
}

std::string compiler_string() {
  std::ostringstream os;
#if defined(__clang__)
  os << "clang " << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(__GNUC__)
  os << "gcc " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
  os << "msvc " << _MSC_VER;
#endif
#if defined(__MINGW64__)
  os << " (mingw-w64)";
#endif
  return os.str();
}

}  // namespace

double calibrate_tsc_ghz() {
  using clock = std::chrono::steady_clock;
  // Best of three 100 ms windows; each window brackets the TSC reads tightly
  // between two clock reads so a preemption only makes a window look slower.
  double best = 0.0;
  for (int i = 0; i < 3; ++i) {
    const auto c0 = clock::now();
    const std::uint64_t t0 = tsc_begin();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint64_t t1 = tsc_end();
    const auto c1 = clock::now();
    const double ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
    const double ghz = static_cast<double>(t1 - t0) / ns;
    if (ghz > best) best = ghz;
  }
  return best;
}

double tsc_ghz() {
  static const double ghz = calibrate_tsc_ghz();
  return ghz;
}

bool pin_current_thread(int cpu) {
  if (cpu < 0) return false;
#if defined(_WIN32)
  const DWORD_PTR mask = DWORD_PTR{1} << cpu;
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#endif
}

std::string raise_current_thread_priority() {
#if defined(_WIN32)
  if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
    return "THREAD_PRIORITY_TIME_CRITICAL";
  return "THREAD_PRIORITY_NORMAL (raise failed)";
#else
  sched_param sp{};
  sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return "SCHED_FIFO max";
  if (setpriority(PRIO_PROCESS, 0, -20) == 0) return "SCHED_OTHER nice -20";
  return "SCHED_OTHER nice 0 (raise not permitted)";
#endif
}

std::string raise_process_priority() {
#if defined(_WIN32)
  // Not REALTIME_PRIORITY_CLASS: two spinning threads at realtime priority on
  // a 4-core box can starve the kernel's own worker threads.
  if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) return "HIGH_PRIORITY_CLASS";
  return "NORMAL_PRIORITY_CLASS (raise failed)";
#else
  return "n/a";
#endif
}

int logical_cpu_count() {
  const unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 1 : static_cast<int>(n);
}

MachineInfo machine_info() {
  MachineInfo m;
  m.cpu = cpu_brand_string();
  m.os = os_string();
  m.compiler = compiler_string();
  m.build_flags = SPSC_BUILD_FLAGS;
  m.logical_cpus = logical_cpu_count();
  m.invariant_tsc = has_invariant_tsc();
  m.tsc_ghz = tsc_ghz();
  return m;
}

}  // namespace harness
