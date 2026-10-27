// SPDX-License-Identifier: MIT
//
// Queue-agnostic workloads shared by the benchmark and the profiler driver.
// Everything here is a template over spsc::BoundedQueue / spsc::BatchQueue.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <debugging>
#include <span>
#include <string_view>
#include <tuple>

#include "platform.hpp"
#include "spsc/mutex_queue.hpp"
#include "spsc/queue_concept.hpp"
#include "spsc/ring_buffer.hpp"

namespace bench {

/// 16-byte message: a sequence number (checked for FIFO order) and a TSC
/// timestamp. Small on purpose, so the benchmark measures the queue's
/// synchronisation cost rather than memcpy bandwidth.
struct Msg {
  std::uint64_t seq = 0;
  std::uint64_t tsc = 0;
};

inline constexpr std::uint64_t kStop = ~std::uint64_t{0};
inline constexpr std::size_t kCapacity = 1024;
inline constexpr std::size_t kBatch = 32;

// ---- queues under test ------------------------------------------------------

using spsc::Signal;
using spsc::Tuning;

/// The default: 128 B padding, cached indices, per-slot sequence numbers.
using Ring = spsc::RingBuffer<Msg>;
// Ablations: each changes one knob of the default.
using RingIndex = spsc::RingBuffer<Msg, Tuning{.signal = Signal::index}>;
using RingPad64 = spsc::RingBuffer<Msg, Tuning{.padding = 64}>;
using RingNoCache = spsc::RingBuffer<Msg, Tuning{.cache_remote_index = false}>;
using RingPacked = spsc::RingBuffer<Msg, Tuning{.padding = 0}>;
/// Textbook Lamport queue: every optimisation off.
using RingNaive = spsc::RingBuffer<
    Msg, Tuning{.padding = 0, .cache_remote_index = false, .signal = Signal::index}>;
using MutexQueue = spsc::MutexQueue<Msg>;
using MutexRing = spsc::MutexRing<Msg>;

// ---- spin policies ----------------------------------------------------------

/// Re-poll immediately. Lowest latency when each thread owns a core.
struct BusySpin {
  static void wait() noexcept {}
};

/// PAUSE between polls: the textbook advice for spin loops. ~140 cycles per
/// PAUSE on Skylake and later (it was ~10 on Broadwell).
struct PauseSpin {
  static void wait() noexcept { harness::cpu_pause(); }
};

// ---- the variant table --------------------------------------------------------

/// One row of the benchmark matrix: a queue type, a spin policy and the name
/// used in result files. The tables below are expanded at compile time with
/// C++26 `template for` (bench_main.cpp), so adding a variant is one line.
template <class Q, class Spin = BusySpin>
struct Variant {
  using queue = Q;
  using spin = Spin;
  std::string_view name;
};

/// The lock-free ring against the two lock-based baselines.
inline constexpr std::tuple kHeadline{
    Variant<Ring>{"ring"},
    Variant<MutexQueue>{"mutex_queue"},
    Variant<MutexRing>{"mutex_ring"},
};

/// Ring variants, each with one design decision changed.
inline constexpr std::tuple kAblations{
    Variant<RingIndex>{"ring_index"},     Variant<RingPad64>{"ring_pad64"},
    Variant<RingNoCache>{"ring_nocache"}, Variant<RingPacked>{"ring_packed"},
    Variant<RingNaive>{"ring_naive"},
};

inline constexpr auto kAllVariants = std::tuple_cat(kHeadline, kAblations);

// ---- blocking helpers -------------------------------------------------------

template <class Spin = BusySpin, spsc::BoundedQueue Q>
inline void push_blocking(Q& q, const typename Q::value_type& m) {
  while (!q.try_push(m)) Spin::wait();
}

template <class Spin = BusySpin, spsc::BoundedQueue Q>
inline typename Q::value_type pop_blocking(Q& q) {
  typename Q::value_type m;
  while (!q.try_pop(m)) Spin::wait();
  return m;
}

/// Pushes every element of `items`, batch-at-a-time, spinning while full.
template <spsc::BatchQueue Q>
inline void push_all(Q& q, std::span<const typename Q::value_type> items) {
  while (!items.empty()) items = items.subspan(q.try_push_n(items));
}

/// Echo side of the round-trip benchmark: pop from `in`, push to `out`.
template <class Spin, spsc::BoundedQueue Q>
void echo_loop(Q& in, Q& out) {
  for (;;) {
    const Msg m = pop_blocking<Spin>(in);
    if (m.seq == kStop) return;
    push_blocking<Spin>(out, m);
  }
}

/// Reports a FIFO violation. Stops in the debugger if one is attached (C++26
/// <debugging>), which is exactly when you want to look at the queue state.
inline void order_violated(std::atomic<bool>& flag) {
  flag.store(true, std::memory_order_relaxed);
  std::breakpoint_if_debugging();
}

/// Consumer side of the throughput benchmark. Verifies FIFO order and
/// publishes progress every `ack_every` messages (a power of two).
template <spsc::BoundedQueue Q>
void drain_loop(Q& q, std::uint64_t ack_every, std::atomic<std::uint64_t>& acked,
                std::atomic<bool>& violated) {
  std::uint64_t expected = 0;
  Msg m;
  for (;;) {
    if (!q.try_pop(m)) continue;
    if (m.seq == kStop) return;
    if (m.seq != expected) [[unlikely]] order_violated(violated);
    ++expected;
    if ((expected & (ack_every - 1)) == 0) acked.store(expected, std::memory_order_release);
  }
}

/// Batched consumer: up to kBatch messages per synchronisation.
template <spsc::BatchQueue Q>
void drain_batch_loop(Q& q, std::uint64_t ack_every, std::atomic<std::uint64_t>& acked,
                      std::atomic<bool>& violated) {
  std::uint64_t expected = 0;
  std::array<Msg, kBatch> buf;
  for (;;) {
    const std::size_t n = q.try_pop_n(buf);
    for (const Msg& m : std::span(buf).first(n)) {
      if (m.seq == kStop) return;
      if (m.seq != expected) [[unlikely]] order_violated(violated);
      ++expected;
      if ((expected & (ack_every - 1)) == 0) acked.store(expected, std::memory_order_release);
    }
  }
}

}  // namespace bench
