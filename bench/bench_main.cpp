// SPDX-License-Identifier: MIT
//
// Google Benchmark driver. Three scenarios, each a template over the queue:
//
//   rtt/<queue>         Round trip: producer pushes into queue A, an echo
//                       thread pops it and pushes into queue B, producer pops
//                       it. One sample per round trip. Measures the pure
//                       cross-core hand-off cost, twice, with no queueing.
//
//   oneway_1us/<queue>  One-way latency at a fixed offered load (one message
//                       per microsecond). The timestamp is the *scheduled*
//                       send time, so a stalled producer shows up as latency
//                       instead of silently sending fewer messages
//                       (coordinated-omission safe).
//
//   throughput/<queue>  Saturated: producer pushes as fast as it can, consumer
//                       pops as fast as it can, FIFO order verified.
//
//   throughput_batch32/<queue>
//                       Same, but with try_push_n / try_pop_n moving up to 32
//                       messages per synchronisation (every queue type has
//                       a batch API, so this is like for like).
//
// Producer = the Google Benchmark main thread, pinned to --producer_cpu.
// Consumer / echo = a worker thread, pinned to --consumer_cpu.
// Both run at the highest priority available (see platform.cpp).
#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include "describe.hpp"
#include "latency.hpp"
#include "platform.hpp"
#include "workloads.hpp"

namespace {

using namespace bench;

struct RunConfig {
  int producer_cpu = -1;
  int consumer_cpu = -1;
  std::int64_t rtt_samples = 1'000'000;
  std::int64_t oneway_samples = 1'000'000;
  std::int64_t warmup = 100'000;
  int repetitions = 5;
  double noise_seconds = 5.0;
  std::string out_dir = "results/latest";
  std::string priority;
};

RunConfig g_cfg;

/// Applied by every thread that takes part in a measurement.
void prepare_thread(int cpu) {
  if (!harness::pin_current_thread(cpu)) {
    std::fprintf(stderr, "warning: failed to pin thread to CPU %d\n", cpu);
  }
  harness::raise_current_thread_priority();
}

/// Turns one repetition's samples into Google Benchmark counters and adds
/// them to the pooled result set.
void report_latency(benchmark::State& state, const char* scenario, const std::string& queue,
                    const harness::SampleBuffer& buf) {
  const auto sorted = harness::to_sorted_ns(buf.data(), buf.size());
  const auto st = harness::compute_stats(sorted);
  state.counters["p50_ns"] = st.p50;
  state.counters["p99_ns"] = st.p99;
  state.counters["p99.9_ns"] = st.p999;
  state.counters["p99.99_ns"] = st.p9999;
  state.counters["max_ns"] = st.max;
  harness::results().add(scenario, queue, buf);
}

// ---- scenarios --------------------------------------------------------------

template <class Q, class Spin = BusySpin>
void bm_rtt(benchmark::State& state, const std::string& name) {
  auto ping = std::make_unique<Q>(kCapacity);
  auto pong = std::make_unique<Q>(kCapacity);
  std::thread echo([&] {
    prepare_thread(g_cfg.consumer_cpu);
    echo_loop<Spin>(*ping, *pong);
  });

  std::uint64_t seq = 0;
  for (std::int64_t i = 0; i < g_cfg.warmup; ++i) {
    push_blocking<Spin>(*ping, Msg{seq++, 0});
    Msg back = pop_blocking<Spin>(*pong);
    benchmark::DoNotOptimize(back);
  }

  harness::SampleBuffer samples(static_cast<std::size_t>(state.max_iterations));
  for (auto _ : state) {
    const std::uint64_t t0 = harness::tsc_begin();
    push_blocking<Spin>(*ping, Msg{seq++, t0});
    Msg back = pop_blocking<Spin>(*pong);
    const std::uint64_t t1 = harness::tsc_end();
    benchmark::DoNotOptimize(back);
    samples.record(t1 - t0);
  }

  push_blocking<Spin>(*ping, Msg{kStop, 0});
  echo.join();
  report_latency(state, "rtt", name, samples);
}

template <class Q>
void bm_oneway(benchmark::State& state, const std::string& name) {
  const auto interval = harness::ns_to_cycles(static_cast<double>(state.range(0)));
  const auto warmup = static_cast<std::uint64_t>(g_cfg.warmup);
  auto q = std::make_unique<Q>(kCapacity);
  harness::SampleBuffer samples(static_cast<std::size_t>(state.max_iterations));

  std::thread consumer([&] {
    prepare_thread(g_cfg.consumer_cpu);
    for (;;) {
      const Msg m = pop_blocking(*q);
      const std::uint64_t now = harness::tsc_end();
      if (m.seq == kStop) return;
      if (m.seq >= warmup) samples.record(now - m.tsc);
    }
  });

  std::uint64_t seq = 0;
  std::uint64_t scheduled = harness::tsc_now() + interval;
  auto send_next = [&] {
    while (harness::tsc_now() < scheduled) {
    }
    push_blocking(*q, Msg{seq++, scheduled});
    scheduled += interval;
  };

  for (std::uint64_t i = 0; i < warmup; ++i) send_next();
  for (auto _ : state) send_next();

  push_blocking(*q, Msg{kStop, 0});
  consumer.join();
  report_latency(state, ("oneway_" + std::to_string(state.range(0)) + "ns").c_str(), name,
                 samples);
}

/// `Batched` = false: one try_push / try_pop per message.
/// `Batched` = true: try_push_n / try_pop_n with up to kBatch (32) messages,
/// one synchronisation per batch on both sides.
template <class Q, bool Batched>
void bm_throughput(benchmark::State& state) {
  constexpr std::uint64_t kAckEvery = 1 << 16;  // messages per timed iteration
  auto q = std::make_unique<Q>(kCapacity);
  std::atomic<std::uint64_t> acked{0};
  std::atomic<bool> violated{false};
  std::thread consumer([&] {
    prepare_thread(g_cfg.consumer_cpu);
    if constexpr (Batched) drain_batch_loop(*q, kAckEvery, acked, violated);
    else drain_loop(*q, kAckEvery, acked, violated);
  });

  std::uint64_t seq = 0;
  std::array<Msg, kBatch> batch;
  for (auto _ : state) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t end = seq + kAckEvery;
    if constexpr (Batched) {
      while (seq < end) {
        for (auto& m : batch) m = Msg{seq++, 0};
        push_all(*q, std::span<const Msg>(batch));
      }
    } else {
      for (; seq < end; ++seq) push_blocking(*q, Msg{seq, 0});
    }
    while (acked.load(std::memory_order_acquire) < end) {
    }
    const auto t1 = std::chrono::steady_clock::now();
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
  }

  push_blocking(*q, Msg{kStop, 0});
  consumer.join();
  if (violated.load()) state.SkipWithError("FIFO order violated");
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kAckEvery));
}

/// Platform noise ("hiccup meter"): a pinned thread reads the TSC in a tight
/// loop and records every gap longer than 1 us. Any such gap is time the core
/// was taken away (interrupt, DPC, SMI, hypervisor exit, preemption). This is
/// the floor under every tail percentile the queue benchmarks report.
void bm_noise(benchmark::State& state) {
  const int cpu = static_cast<int>(state.range(0));
  const std::uint64_t threshold = harness::ns_to_cycles(1000);
  const std::uint64_t duration = harness::ns_to_cycles(g_cfg.noise_seconds * 1e9);
  harness::SampleBuffer gaps(16'000'000);
  std::uint64_t stolen = 0, elapsed = 0;

  for (auto _ : state) {
    std::thread probe([&] {
      prepare_thread(cpu);
      const std::uint64_t start = harness::tsc_now();
      std::uint64_t prev = start;
      while (prev - start < duration) {
        const std::uint64_t now = harness::tsc_now();
        if (now - prev > threshold) {
          gaps.record(now - prev);
          stolen += now - prev;
        }
        prev = now;
      }
      elapsed += prev - start;
    });
    probe.join();
  }

  const double seconds = harness::cycles_to_ns(static_cast<double>(elapsed)) / 1e9;
  state.counters["gaps_per_s"] = static_cast<double>(gaps.size()) / seconds;
  state.counters["stolen_pct"] = 100.0 * static_cast<double>(stolen) / static_cast<double>(elapsed);
  report_latency(state, "noise", "cpu" + std::to_string(cpu), gaps);
}

// ---- registration -----------------------------------------------------------

template <class Q, class Spin = BusySpin>
void register_rtt(const std::string& name) {
  benchmark::RegisterBenchmark(("rtt/" + name).c_str(),
                               [name](benchmark::State& s) { bm_rtt<Q, Spin>(s, name); })
      ->Iterations(g_cfg.rtt_samples)
      ->Repetitions(g_cfg.repetitions)
      ->DisplayAggregatesOnly(true)
      ->UseRealTime()
      ->Unit(benchmark::kNanosecond);
}

template <class Q>
void register_oneway(const std::string& name, std::int64_t interval_ns) {
  benchmark::RegisterBenchmark(("oneway/" + name).c_str(),
                               [name](benchmark::State& s) { bm_oneway<Q>(s, name); })
      ->Arg(interval_ns)
      ->Iterations(g_cfg.oneway_samples)
      ->Repetitions(g_cfg.repetitions)
      ->DisplayAggregatesOnly(true)
      ->UseRealTime()
      ->Unit(benchmark::kNanosecond);
}

template <class Q, bool Batched = false>
void register_throughput(const std::string& name) {
  const std::string scenario = Batched ? "throughput_batch32/" : "throughput/";
  benchmark::RegisterBenchmark((scenario + name).c_str(), bm_throughput<Q, Batched>)
      ->Repetitions(g_cfg.repetitions)
      ->DisplayAggregatesOnly(true)
      ->UseManualTime()
      ->Unit(benchmark::kMillisecond);
}

// GCC 16 reports every `template for` expansion's loop variable as shadowing
// the previous expansion's. They are separate scopes; silence the false alarm.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"

void register_all() {
  // Measure the machine first: everything below sits on top of this noise.
  benchmark::RegisterBenchmark("noise/cpu", bm_noise)
      ->Arg(g_cfg.producer_cpu)
      ->Arg(g_cfg.consumer_cpu)
      ->Iterations(1)
      ->UseRealTime()
      ->Unit(benchmark::kMillisecond);

  // The variant tables live in workloads.hpp; C++26 `template for` expands
  // each tuple element (a distinct queue type) into its own registration.
  template for (constexpr auto v : kAllVariants) {
    using V = std::remove_cvref_t<decltype(v)>;
    register_rtt<typename V::queue, typename V::spin>(std::string(v.name));
  }
  register_rtt<Ring, PauseSpin>("ring_pause");

  template for (constexpr auto v : kHeadline) {
    register_oneway<typename std::remove_cvref_t<decltype(v)>::queue>(std::string(v.name), 1000);
  }

  template for (constexpr auto v : kAllVariants) {
    register_throughput<typename std::remove_cvref_t<decltype(v)>::queue>(std::string(v.name));
  }
  template for (constexpr auto v : kHeadline) {
    register_throughput<typename std::remove_cvref_t<decltype(v)>::queue, true>(
        std::string(v.name));
  }
}

/// Records each ring variant's compile-time layout in the run context, e.g.
/// "variant ring: padding=128 cache_remote_index=true signal=slot_sequence".
/// Generated by C++26 reflection over spsc::Tuning, so it cannot go stale.
void describe_variants() {
  template for (constexpr auto v : kAllVariants) {
    using Q = typename std::remove_cvref_t<decltype(v)>::queue;
    if constexpr (requires { Q::tuning; }) {
      benchmark::AddCustomContext("variant " + std::string(v.name), harness::describe(Q::tuning));
    }
  }
}

#pragma GCC diagnostic pop

// ---- command line -----------------------------------------------------------

bool take_flag(std::string_view arg, std::string_view name, std::string& value) {
  const std::string prefix = "--" + std::string(name) + "=";
  if (arg.substr(0, prefix.size()) != prefix) return false;
  value = std::string(arg.substr(prefix.size()));
  return true;
}

/// Consumes harness flags from argv, leaving Google Benchmark's own flags.
void parse_harness_flags(int& argc, char** argv) {
  int out = 1;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    const std::string_view a = argv[i];
    if (take_flag(a, "producer_cpu", v)) g_cfg.producer_cpu = std::stoi(v);
    else if (take_flag(a, "consumer_cpu", v)) g_cfg.consumer_cpu = std::stoi(v);
    else if (take_flag(a, "rtt_samples", v)) g_cfg.rtt_samples = std::stoll(v);
    else if (take_flag(a, "oneway_samples", v)) g_cfg.oneway_samples = std::stoll(v);
    else if (take_flag(a, "warmup", v)) g_cfg.warmup = std::stoll(v);
    else if (take_flag(a, "repetitions", v)) g_cfg.repetitions = std::stoi(v);
    else if (take_flag(a, "noise_seconds", v)) g_cfg.noise_seconds = std::stod(v);
    else if (take_flag(a, "out_dir", v)) g_cfg.out_dir = v;
    else if (a == "--quick") {
      g_cfg.noise_seconds = 1.0;
      g_cfg.rtt_samples = g_cfg.oneway_samples = 100'000;
      g_cfg.warmup = 10'000;
      g_cfg.repetitions = 1;
    } else argv[out++] = argv[i];
  }
  argc = out;
}

}  // namespace

int main(int argc, char** argv) {
  parse_harness_flags(argc, argv);
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  // Default placement: the two highest-numbered CPUs. CPU 0 services most
  // device interrupts and DPCs on Windows, so keep the hot threads off it.
  const int ncpu = harness::logical_cpu_count();
  if (g_cfg.producer_cpu < 0) g_cfg.producer_cpu = ncpu >= 2 ? ncpu - 2 : 0;
  if (g_cfg.consumer_cpu < 0) g_cfg.consumer_cpu = ncpu - 1;

  const std::string proc_prio = harness::raise_process_priority();
  harness::pin_current_thread(g_cfg.producer_cpu);
  const std::string thread_prio = harness::raise_current_thread_priority();
  g_cfg.priority = proc_prio + " / " + thread_prio;

  auto m = harness::machine_info();
  m.producer_cpu = g_cfg.producer_cpu;
  m.consumer_cpu = g_cfg.consumer_cpu;
  m.priority = g_cfg.priority;
  benchmark::AddCustomContext("cpu", m.cpu);
  benchmark::AddCustomContext("os", m.os);
  benchmark::AddCustomContext("compiler", m.compiler);
  benchmark::AddCustomContext("build_flags", m.build_flags);
  benchmark::AddCustomContext("invariant_tsc", m.invariant_tsc ? "yes" : "NO");
  benchmark::AddCustomContext("tsc_ghz", std::to_string(m.tsc_ghz));
  benchmark::AddCustomContext("producer_cpu", std::to_string(g_cfg.producer_cpu));
  benchmark::AddCustomContext("consumer_cpu", std::to_string(g_cfg.consumer_cpu));
  benchmark::AddCustomContext("priority", g_cfg.priority);
  if (!m.invariant_tsc) {
    std::fprintf(stderr, "warning: no invariant TSC; cross-core one-way latency is unreliable\n");
  }

  describe_variants();
  register_all();
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  const std::filesystem::path out(g_cfg.out_dir);
  harness::results().write(out, "cpp");
  std::ofstream(out / "machine.json") << harness::to_json(m);  // reflection-generated
  std::printf("latency results written to %s\n", out.string().c_str());
  return 0;
}
