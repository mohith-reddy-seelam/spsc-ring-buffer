// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <numeric>
#include <span>
#include <memory>
#include <string>
#include <thread>

#include "spsc/mutex_queue.hpp"
#include "spsc/queue_concept.hpp"
#include "spsc/ring_buffer.hpp"

namespace {

using spsc::RingBuffer;
using spsc::Signal;
using spsc::Tuning;

using IndexRing = RingBuffer<std::uint64_t, Tuning{.signal = Signal::index}>;

// ---- compile-time properties ------------------------------------------------

static_assert(spsc::BoundedQueue<RingBuffer<int>>);
static_assert(spsc::BoundedQueue<spsc::MutexQueue<int>>);
static_assert(spsc::BoundedQueue<spsc::MutexRing<int>>);
static_assert(spsc::BatchQueue<RingBuffer<int>>);
static_assert(spsc::BatchQueue<spsc::MutexQueue<int>>);
static_assert(spsc::BatchQueue<spsc::MutexRing<int>>);

// The default signals readiness through a per-slot sequence number.
static_assert(RingBuffer<int>::tuning.signal == Signal::slot_sequence);

// A sequence slot must never straddle two cache lines (sequence number and
// payload have to arrive in the same line transfer): power-of-two size <= 64.
struct Msg16 {
  std::uint64_t a, b;
};
static_assert(sizeof(spsc::detail::SeqSlot<Msg16>) == 32);
static_assert(alignof(spsc::detail::SeqSlot<Msg16>) == 32);
static_assert(sizeof(spsc::detail::SeqSlot<std::uint64_t>) == 16);
static_assert(64 % sizeof(spsc::detail::SeqSlot<std::array<char, 40>>) == 0);

// The padded layout must put producer and consumer state 128 bytes apart.
static_assert(alignof(RingBuffer<int>) == 128);
static_assert(sizeof(RingBuffer<int>) == 3 * 128);
static_assert(alignof(RingBuffer<int, Tuning{.padding = 64}>) == 64);
static_assert(sizeof(RingBuffer<int, Tuning{.padding = 0}>) <= 64,
              "packed layout should fit in one line (that is the point of the ablation)");

// ---- typed tests over every layout variant ----------------------------------

template <class Q>
class RingTest : public ::testing::Test {};

// Every combination of the three knobs, plus both lock-based baselines.
using Variants = ::testing::Types<
    RingBuffer<std::uint64_t>, IndexRing, RingBuffer<std::uint64_t, Tuning{.padding = 64}>,
    RingBuffer<std::uint64_t, Tuning{.cache_remote_index = false}>,
    RingBuffer<std::uint64_t, Tuning{.cache_remote_index = false, .signal = Signal::index}>,
    RingBuffer<std::uint64_t, Tuning{.padding = 0}>,
    RingBuffer<std::uint64_t, Tuning{.padding = 0, .signal = Signal::index}>,
    RingBuffer<std::uint64_t,
               Tuning{.padding = 0, .cache_remote_index = false, .signal = Signal::index}>,
    spsc::MutexQueue<std::uint64_t>, spsc::MutexRing<std::uint64_t>>;
TYPED_TEST_SUITE(RingTest, Variants);

TYPED_TEST(RingTest, StartsEmpty) {
  TypeParam q(8);
  std::uint64_t v = 0;
  EXPECT_FALSE(q.try_pop(v));
}

TYPED_TEST(RingTest, FifoOrder) {
  TypeParam q(8);
  for (std::uint64_t i = 0; i < 5; ++i) ASSERT_TRUE(q.try_push(i));
  for (std::uint64_t i = 0; i < 5; ++i) {
    std::uint64_t v = 99;
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, i);
  }
}

TYPED_TEST(RingTest, FullAtExactlyCapacity) {
  TypeParam q(8);
  ASSERT_EQ(q.capacity(), 8u);
  for (std::uint64_t i = 0; i < 8; ++i) ASSERT_TRUE(q.try_push(i)) << i;
  EXPECT_FALSE(q.try_push(8u));
  std::uint64_t v;
  ASSERT_TRUE(q.try_pop(v));
  EXPECT_TRUE(q.try_push(8u));  // one slot freed -> one push succeeds
  EXPECT_FALSE(q.try_push(9u));
}

TYPED_TEST(RingTest, WrapsAroundManyTimes) {
  TypeParam q(4);
  std::uint64_t next_in = 0, next_out = 0;
  for (int round = 0; round < 10'000; ++round) {
    const int burst = 1 + round % 4;
    for (int i = 0; i < burst; ++i) ASSERT_TRUE(q.try_push(next_in++));
    for (int i = 0; i < burst; ++i) {
      std::uint64_t v;
      ASSERT_TRUE(q.try_pop(v));
      ASSERT_EQ(v, next_out++);
    }
  }
}

// Two threads, tiny capacity to force constant full/empty transitions (the
// interesting paths for the cached indices), every value checked.
TYPED_TEST(RingTest, ConcurrentStressPreservesOrder) {
  constexpr std::uint64_t kN = 2'000'000;
  TypeParam q(16);
  std::atomic<bool> failed{false};
  std::thread consumer([&] {
    for (std::uint64_t expected = 0; expected < kN;) {
      std::uint64_t v;
      if (!q.try_pop(v)) continue;
      if (v != expected) {
        failed = true;
        return;
      }
      ++expected;
    }
  });
  for (std::uint64_t i = 0; i < kN; ++i) {
    while (!q.try_push(i)) {
    }
  }
  consumer.join();
  EXPECT_FALSE(failed.load());
}

TYPED_TEST(RingTest, BatchPushPopKeepsOrderAndRespectsCapacity) {
  TypeParam q(8);
  std::array<std::uint64_t, 12> in;
  std::iota(in.begin(), in.end(), 0);
  EXPECT_EQ(q.try_push_n(in), 8u);  // only 8 fit
  EXPECT_EQ(q.try_push_n(std::span(in).subspan(8)), 0u);

  std::array<std::uint64_t, 5> out{};
  ASSERT_EQ(q.try_pop_n(out), 5u);
  for (std::uint64_t i = 0; i < 5; ++i) EXPECT_EQ(out[i], i);

  EXPECT_EQ(q.try_push_n(std::span(in).subspan(8)), 4u);  // wraps around
  std::array<std::uint64_t, 16> rest{};
  ASSERT_EQ(q.try_pop_n(rest), 7u);
  for (std::uint64_t i = 0; i < 7; ++i) EXPECT_EQ(rest[i], i + 5);
  EXPECT_EQ(q.try_pop_n(rest), 0u);
}

TYPED_TEST(RingTest, ConcurrentBatchStressPreservesOrder) {
  constexpr std::uint64_t kN = 2'000'000;
  TypeParam q(16);
  std::atomic<bool> failed{false};
  std::thread consumer([&] {
    std::array<std::uint64_t, 7> buf;  // odd size: batches straddle the wrap point
    for (std::uint64_t expected = 0; expected < kN;) {
      for (std::uint64_t v : std::span(buf).first(q.try_pop_n(buf))) {
        if (v != expected++) failed = true;
      }
    }
  });
  std::array<std::uint64_t, 5> batch;
  for (std::uint64_t next = 0; next < kN;) {
    const std::size_t n = std::min<std::uint64_t>(batch.size(), kN - next);
    for (std::size_t i = 0; i < n; ++i) batch[i] = next + i;
    std::span<const std::uint64_t> pending(batch.data(), n);
    while (!pending.empty()) pending = pending.subspan(q.try_push_n(pending));
    next += n;
  }
  consumer.join();
  EXPECT_FALSE(failed.load());
}

// ---- RingBuffer-specific behaviour ------------------------------------------

TEST(RingBuffer, CapacityRoundsUpToPowerOfTwo) {
  EXPECT_EQ(RingBuffer<int>(1).capacity(), 1u);
  EXPECT_EQ(RingBuffer<int>(5).capacity(), 8u);
  EXPECT_EQ(RingBuffer<int>(1024).capacity(), 1024u);
  EXPECT_EQ(RingBuffer<int>(1025).capacity(), 2048u);
  EXPECT_THROW(RingBuffer<int>(0), std::invalid_argument);
}

TEST(RingBuffer, CapacityOneWorks) {
  RingBuffer<int> q(1);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_FALSE(q.try_push(2));
  int v = 0;
  EXPECT_TRUE(q.try_pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_FALSE(q.try_pop(v));
}

TEST(RingBuffer, FrontAndPopAreZeroCopy) {
  RingBuffer<std::string> q(4);
  ASSERT_TRUE(q.try_emplace(100, 'x'));
  std::string* p = q.front();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->size(), 100u);
  EXPECT_EQ(q.front(), p);  // peeking twice yields the same object
  q.pop();
  EXPECT_EQ(q.front(), nullptr);
}

TEST(RingBuffer, SupportsMoveOnlyTypes) {
  RingBuffer<std::unique_ptr<int>> q(4);
  ASSERT_TRUE(q.try_push(std::make_unique<int>(7)));
  std::unique_ptr<int> out;
  ASSERT_TRUE(q.try_pop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 7);
}

struct Counted {
  static inline int live = 0;
  Counted() { ++live; }
  Counted(const Counted&) { ++live; }
  Counted& operator=(const Counted&) = default;
  ~Counted() { --live; }
};

template <Signal S>
void check_destroys_exactly_the_elements_it_holds() {
  Counted::live = 0;
  {
    RingBuffer<Counted, Tuning{.signal = S}> q(8);
    for (int i = 0; i < 6; ++i) ASSERT_TRUE(q.try_emplace());
    EXPECT_EQ(Counted::live, 6);
    q.pop();
    q.pop();
    EXPECT_EQ(Counted::live, 4);
  }
  EXPECT_EQ(Counted::live, 0);  // destructor cleans up the remaining four
}

TEST(RingBuffer, DestroysExactlyTheElementsItHolds) {
  check_destroys_exactly_the_elements_it_holds<Signal::slot_sequence>();
  check_destroys_exactly_the_elements_it_holds<Signal::index>();
}

struct ThrowsOnConstruct {
  explicit ThrowsOnConstruct(bool do_throw) {
    if (do_throw) throw std::runtime_error("boom");
  }
};

TEST(RingBuffer, ThrowingConstructorLeavesBufferUnchanged) {
  RingBuffer<ThrowsOnConstruct> q(4);
  EXPECT_THROW((void)q.try_emplace(true), std::runtime_error);
  EXPECT_EQ(q.size_approx(), 0u);
  EXPECT_TRUE(q.try_emplace(false));
  EXPECT_EQ(q.size_approx(), 1u);
}

TEST(RingBuffer, OverAlignedElements) {
  struct alignas(128) Big {
    std::uint64_t v;
  };
  RingBuffer<Big> q(4);
  ASSERT_TRUE(q.try_push(Big{42}));
  Big* p = q.front();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 128, 0u);
  EXPECT_EQ(p->v, 42u);
}

TEST(RingBuffer, SequenceNumbersSurviveManyLaps) {
  // Each slot is reused every `capacity` messages; the consumer must never
  // mistake the previous lap's sequence number for a fresh one.
  RingBuffer<std::uint64_t> q(2);
  for (std::uint64_t i = 0; i < 100'000; ++i) {
    ASSERT_TRUE(q.try_push(i));
    std::uint64_t v;
    ASSERT_TRUE(q.try_pop(v));
    ASSERT_EQ(v, i);
    ASSERT_FALSE(q.try_pop(v)) << "stale slot read as full at lap " << i;
  }
}

// C++26 contracts: pop() has `pre(readable())`. Tests are built with
// -fcontract-evaluation-semantic=enforce, so a violation reports the
// predicate and terminates.
TEST(RingBufferDeathTest, PopOnEmptyViolatesPrecondition) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  RingBuffer<int> q(4);
  EXPECT_DEATH(q.pop(), "contract violation.*readable");
  IndexRing q2(4);
  EXPECT_DEATH(q2.pop(), "contract violation.*readable");
}

}  // namespace
