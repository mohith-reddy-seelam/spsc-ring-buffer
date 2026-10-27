// SPDX-License-Identifier: MIT
//
// Platform layer for the benchmark harness: thread pinning, scheduling
// priority, TSC timestamps and a description of the machine under test.
// Windows and Linux on x86-64.
#pragma once

#include <cstdint>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace harness {

// ---- timestamps -------------------------------------------------------------
//
// The TSC is the only clock cheap and precise enough for ~50 ns intervals
// (QueryPerformanceCounter / clock_gettime cost 20-30 ns and tick at 10 MHz on
// Windows). On CPUs with an invariant TSC (checked in machine_info()) the
// counter runs at a constant rate and is synchronised across cores, so a
// timestamp taken on the producer core can be subtracted from one taken on
// the consumer core.

/// Timestamp at the start of a measured region. The LFENCE before RDTSC stops
/// the read from executing before earlier instructions; the one after stops
/// later instructions from starting before it.
inline std::uint64_t tsc_begin() noexcept {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

/// Timestamp at the end of a measured region. RDTSCP waits for all earlier
/// instructions to retire; the LFENCE keeps later ones from overtaking it.
inline std::uint64_t tsc_end() noexcept {
  unsigned aux;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

/// Unfenced read, for pacing loops where a few cycles of skew do not matter.
inline std::uint64_t tsc_now() noexcept { return __rdtsc(); }

inline void cpu_pause() noexcept { _mm_pause(); }

/// Measures TSC frequency against the OS steady clock. Called once at start-up.
double calibrate_tsc_ghz();

/// Cycles-to-nanoseconds conversion, fixed after the first call.
double tsc_ghz();
inline double cycles_to_ns(double cycles) { return cycles / tsc_ghz(); }
inline std::uint64_t ns_to_cycles(double ns) {
  return static_cast<std::uint64_t>(ns * tsc_ghz());
}

// ---- scheduling -------------------------------------------------------------

/// Pins the calling thread to one logical CPU. Returns false on failure.
bool pin_current_thread(int cpu);

/// Gives the calling thread the highest non-realtime priority the OS allows
/// without admin rights (Windows: TIME_CRITICAL; Linux: SCHED_FIFO if
/// permitted, else nice -20 if permitted, else unchanged). Returns a string
/// describing what was actually applied, for the run record.
std::string raise_current_thread_priority();

/// Windows: HIGH_PRIORITY_CLASS. Linux: no-op (per-thread policy is used).
std::string raise_process_priority();

int logical_cpu_count();

// ---- machine description ----------------------------------------------------

/// Everything a results directory needs to say where it came from. Written to
/// machine.json by harness::to_json (C++26 reflection, harness/describe.hpp),
/// so a new field here appears in the output with no serialiser to update.
/// Member names are the JSON keys in docs/HARNESS_SPEC.md: do not rename.
struct MachineInfo {
  std::string cpu;
  std::string os;
  std::string compiler;
  std::string build_flags;
  int logical_cpus = 0;
  bool invariant_tsc = false;
  double tsc_ghz = 0.0;
  // Filled in by the benchmark driver.
  int producer_cpu = -1;
  int consumer_cpu = -1;
  std::string priority;
};

MachineInfo machine_info();

}  // namespace harness
