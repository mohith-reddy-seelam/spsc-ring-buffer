// SPDX-License-Identifier: MIT
//
// Latency sample storage, exact percentiles, and the on-disk result format.
//
// The file format is the contract with other implementations (the Rust port
// writes the same files), so it is specified in docs/HARNESS_SPEC.md and kept
// deliberately simple: CSV, nanoseconds, nearest-rank percentiles, log-linear
// histogram buckets defined by a closed-form formula.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace harness {

/// C++26 saturation arithmetic. The function was renamed late in the
/// standardisation process (saturate_cast -> saturating_cast, P3816); GCC 16
/// pre-release snapshots ship the old name, GCC 16.2 the final one.
inline std::uint32_t saturate_u32(std::uint64_t v) noexcept {
#if defined(__cpp_lib_saturation_arithmetic) && __cpp_lib_saturation_arithmetic >= 202603L
  return std::saturating_cast<std::uint32_t>(v);
#else
  return std::saturate_cast<std::uint32_t>(v);
#endif
}

/// Pre-allocated, pre-faulted sample buffer. record() is a bounds check and a
/// store, so it can sit inside a ~50 ns measured loop without page faults.
class SampleBuffer {
 public:
  explicit SampleBuffer(std::size_t capacity) : samples_(capacity, 0) {}

  void record(std::uint64_t cycles) noexcept {
    if (size_ < samples_.size()) {
      samples_[size_++] = saturate_u32(cycles);
    }
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] const std::uint32_t* data() const noexcept { return samples_.data(); }

 private:
  std::vector<std::uint32_t> samples_;  // TSC cycles; saturates at ~1.7 s @ 2.5 GHz
  std::size_t size_ = 0;
};

struct LatencyStats {
  std::size_t count = 0;
  double min = 0, mean = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;  // ns
};

/// Nearest-rank percentile of a sorted vector: the smallest sample such that at
/// least q*100 % of samples are <= it.
double percentile_sorted(const std::vector<double>& sorted, double q);

/// Converts cycles to ns and sorts. Input is left untouched.
std::vector<double> to_sorted_ns(const std::uint32_t* cycles, std::size_t n);

LatencyStats compute_stats(const std::vector<double>& sorted_ns);

/// Collects samples across benchmark repetitions, keyed by "scenario/queue",
/// and writes the pooled results when the run ends.
class ResultStore {
 public:
  void add(const std::string& scenario, const std::string& queue, const SampleBuffer& buf);

  /// Writes <dir>/latency_summary.csv, <dir>/hist/*.csv, <dir>/percentiles/*.csv.
  void write(const std::filesystem::path& dir, const std::string& impl) const;

 private:
  struct Key {
    std::string scenario, queue;
    auto operator<=>(const Key&) const = default;
  };
  std::map<Key, std::vector<std::uint32_t>> pooled_;
};

ResultStore& results();

}  // namespace harness
