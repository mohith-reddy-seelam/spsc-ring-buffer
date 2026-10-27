// SPDX-License-Identifier: MIT
#include "latency.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <stdexcept>

#include "platform.hpp"

namespace harness {
namespace {

// Histogram bucket i covers [2^(i/16), 2^((i+1)/16)) ns: 16 buckets per
// doubling, i.e. ~4.4 % relative width. See docs/HARNESS_SPEC.md.
constexpr int kSubBuckets = 16;

int bucket_of(double ns) {
  return static_cast<int>(std::floor(kSubBuckets * std::log2(std::max(ns, 1.0))));
}
double bucket_lo(int i) { return std::exp2(static_cast<double>(i) / kSubBuckets); }

// Percentile ladder for the "HDR" plot: q = 1 - 10^-k, k = 0, 0.05, ..., 6,
// plus the exact maximum. Dense where tails live.
std::vector<double> percentile_ladder() {
  std::vector<double> qs;
  for (int i = 0; i <= 120; ++i) qs.push_back(1.0 - std::pow(10.0, -i / 20.0));
  qs.front() = 0.0;
  return qs;
}

std::ofstream open_or_throw(const std::filesystem::path& p) {
  std::ofstream f(p);
  if (!f) throw std::runtime_error("cannot open " + p.string());
  f << std::setprecision(10);
  return f;
}

}  // namespace

double percentile_sorted(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) return 0.0;
  if (q <= 0.0) return sorted.front();
  const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(sorted.size())));
  return sorted[std::min(rank, sorted.size()) - 1];
}

std::vector<double> to_sorted_ns(const std::uint32_t* cycles, std::size_t n) {
  std::vector<double> ns(n);
  const double ghz = tsc_ghz();
  for (std::size_t i = 0; i < n; ++i) ns[i] = cycles[i] / ghz;
  std::sort(ns.begin(), ns.end());
  return ns;
}

LatencyStats compute_stats(const std::vector<double>& s) {
  LatencyStats st;
  st.count = s.size();
  if (s.empty()) return st;
  st.min = s.front();
  st.max = s.back();
  st.mean = std::accumulate(s.begin(), s.end(), 0.0) / static_cast<double>(s.size());
  st.p50 = percentile_sorted(s, 0.50);
  st.p90 = percentile_sorted(s, 0.90);
  st.p99 = percentile_sorted(s, 0.99);
  st.p999 = percentile_sorted(s, 0.999);
  st.p9999 = percentile_sorted(s, 0.9999);
  return st;
}

void ResultStore::add(const std::string& scenario, const std::string& queue,
                      const SampleBuffer& buf) {
  auto& v = pooled_[Key{scenario, queue}];
  v.insert(v.end(), buf.data(), buf.data() + buf.size());
}

void ResultStore::write(const std::filesystem::path& dir, const std::string& impl) const {
  namespace fs = std::filesystem;
  fs::create_directories(dir / "hist");
  fs::create_directories(dir / "percentiles");

  auto summary = open_or_throw(dir / "latency_summary.csv");
  summary << "impl,scenario,queue,samples,min_ns,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,"
             "max_ns\n";

  for (const auto& [key, cycles] : pooled_) {
    const auto sorted = to_sorted_ns(cycles.data(), cycles.size());
    const auto st = compute_stats(sorted);
    summary << impl << ',' << key.scenario << ',' << key.queue << ',' << st.count << ','
            << st.min << ',' << st.mean << ',' << st.p50 << ',' << st.p90 << ',' << st.p99 << ','
            << st.p999 << ',' << st.p9999 << ',' << st.max << '\n';

    const std::string stem = key.scenario + "__" + key.queue + ".csv";

    auto hist = open_or_throw(dir / "hist" / stem);
    hist << "bucket_lo_ns,bucket_hi_ns,count\n";
    std::size_t i = 0;
    while (i < sorted.size()) {
      const int b = bucket_of(sorted[i]);
      std::size_t j = i;
      while (j < sorted.size() && bucket_of(sorted[j]) == b) ++j;
      hist << bucket_lo(b) << ',' << bucket_lo(b + 1) << ',' << (j - i) << '\n';
      i = j;
    }

    auto pct = open_or_throw(dir / "percentiles" / stem);
    pct << "percentile,latency_ns\n";
    for (double q : percentile_ladder()) pct << q * 100.0 << ',' << percentile_sorted(sorted, q) << '\n';
    pct << 100.0 << ',' << st.max << '\n';
  }
}

ResultStore& results() {
  static ResultStore store;
  return store;
}

}  // namespace harness
