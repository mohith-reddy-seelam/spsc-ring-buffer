// SPDX-License-Identifier: MIT
//
// Runs one queue under a fixed-duration workload for profiling.
//
//   spsc_profile --queue=ring --workload=throughput --seconds=5 --out=stacks.txt
//
// On Windows the built-in sampler (harness/sampler.hpp) writes raw stacks to
// --out. On Linux pass --no-sampler and wrap the process in `perf record -g`.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "platform.hpp"
#include "sampler.hpp"
#include "workloads.hpp"

namespace {

using namespace bench;

struct Options {
  std::string queue = "ring";
  std::string workload = "throughput";
  double seconds = 5.0;
  int hz = 2000;
  int producer_cpu = -1;
  int consumer_cpu = -1;
  int sampler_cpu = -1;
  bool use_sampler = true;
  std::string out = "stacks.txt";
};

/// Counts threads that have registered with the sampler and are ready.
std::atomic<int> g_ready{0};
std::atomic<bool> g_stop{false};

// noinline + distinct names so each role is a recognisable root frame.
template <class Q>
[[gnu::noinline]] std::uint64_t producer_throughput(Q& q) {
  std::uint64_t seq = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    for (int i = 0; i < 1024; ++i) push_blocking(q, Msg{seq++, 0});
  }
  push_blocking(q, Msg{kStop, 0});
  return seq;
}

template <class Q>
[[gnu::noinline]] void consumer_throughput(Q& q) {
  std::atomic<std::uint64_t> acked{0};
  std::atomic<bool> bad{false};
  drain_loop(q, 1 << 16, acked, bad);
  if (bad.load()) std::fprintf(stderr, "FIFO order violated\n");
}

template <class Q>
[[gnu::noinline]] std::uint64_t producer_rtt(Q& ping, Q& pong) {
  std::uint64_t seq = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    push_blocking(ping, Msg{seq++, 0});
    (void)pop_blocking(pong);
  }
  push_blocking(ping, Msg{kStop, 0});
  return seq;
}

template <class Q>
[[gnu::noinline]] void consumer_rtt(Q& ping, Q& pong) {
  echo_loop<BusySpin>(ping, pong);
}

template <class Q>
int run(const Options& o, harness::Sampler& sampler) {
  auto a = std::make_unique<Q>(kCapacity);
  auto b = std::make_unique<Q>(kCapacity);
  const bool rtt = o.workload == "rtt";

  std::thread consumer([&] {
    harness::pin_current_thread(o.consumer_cpu);
    harness::raise_current_thread_priority();
    sampler.register_current_thread("consumer");
    g_ready.fetch_add(1);
    if (rtt) consumer_rtt(*a, *b);
    else consumer_throughput(*a);
  });

  std::uint64_t messages = 0;
  std::thread producer([&] {
    harness::pin_current_thread(o.producer_cpu);
    harness::raise_current_thread_priority();
    sampler.register_current_thread("producer");
    g_ready.fetch_add(1);
    while (g_ready.load() < 3) {
    }
    messages = rtt ? producer_rtt(*a, *b) : producer_throughput(*a);
  });

  while (g_ready.load() < 2) std::this_thread::yield();
  if (o.use_sampler) sampler.start(o.sampler_cpu);
  g_ready.fetch_add(1);  // release the producer

  std::this_thread::sleep_for(std::chrono::duration<double>(o.seconds));
  g_stop.store(true);
  producer.join();
  consumer.join();
  if (o.use_sampler) sampler.stop();

  std::printf("%s/%s: %.1f M messages in %.1f s\n", o.workload.c_str(), o.queue.c_str(),
              static_cast<double>(messages) / 1e6, o.seconds);
  if (o.use_sampler) {
    const auto n = sampler.write_raw(o.out);
    std::printf("wrote %zu samples to %s\n", n, o.out.c_str());
  }
  return 0;
}

bool take(std::string_view arg, std::string_view name, std::string& value) {
  const std::string prefix = "--" + std::string(name) + "=";
  if (arg.substr(0, prefix.size()) != prefix) return false;
  value = std::string(arg.substr(prefix.size()));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    const std::string_view a = argv[i];
    if (take(a, "queue", v)) o.queue = v;
    else if (take(a, "workload", v)) o.workload = v;
    else if (take(a, "seconds", v)) o.seconds = std::stod(v);
    else if (take(a, "hz", v)) o.hz = std::stoi(v);
    else if (take(a, "producer_cpu", v)) o.producer_cpu = std::stoi(v);
    else if (take(a, "consumer_cpu", v)) o.consumer_cpu = std::stoi(v);
    else if (take(a, "sampler_cpu", v)) o.sampler_cpu = std::stoi(v);
    else if (take(a, "out", v)) o.out = v;
    else if (a == "--no-sampler") o.use_sampler = false;
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  const int ncpu = harness::logical_cpu_count();
  if (o.producer_cpu < 0) o.producer_cpu = ncpu >= 2 ? ncpu - 2 : 0;
  if (o.consumer_cpu < 0) o.consumer_cpu = ncpu - 1;
  if (o.sampler_cpu < 0 && ncpu >= 3) o.sampler_cpu = ncpu - 3;
  if (o.use_sampler && !harness::Sampler::supported()) {
    std::fprintf(stderr, "built-in sampler is Windows-only; use --no-sampler with perf\n");
    return 2;
  }
  harness::raise_process_priority();

  harness::Sampler sampler(o.hz, o.seconds + 1.0, 2);
  if (o.queue == "ring") return run<Ring>(o, sampler);
  if (o.queue == "mutex_queue") return run<MutexQueue>(o, sampler);
  if (o.queue == "mutex_ring") return run<MutexRing>(o, sampler);
  if (o.queue == "ring_naive") return run<RingNaive>(o, sampler);
  if (o.queue == "ring_index") return run<RingIndex>(o, sampler);
  std::fprintf(stderr, "unknown queue: %s\n", o.queue.c_str());
  return 2;
}
