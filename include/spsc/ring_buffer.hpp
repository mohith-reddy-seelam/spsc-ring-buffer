// SPDX-License-Identifier: MIT
//
// Bounded, wait-free, single-producer / single-consumer ring buffer (C++26).
//
//   * One thread may call the producer API (try_push / try_emplace / try_push_n).
//   * One (other) thread may call the consumer API (front / pop / try_pop / try_pop_n).
//   * Every operation completes in a bounded number of steps: no locks, no
//     CAS loops, no allocation after construction.
//
// Design notes (see docs/DESIGN.md for the long version and measurements):
//
//   1. Monotonic 64-bit indices. head/tail are never wrapped; the slot is
//      `index & mask`. Full is `tail - head == capacity`, so all `capacity`
//      slots are usable (no "one empty slot" sentinel). A 64-bit counter at
//      1e9 ops/s overflows after ~585 years.
//
//   2. Each thread owns one 128-byte block. The producer's block holds `tail`
//      and its private cache of `head`; the consumer's block holds `head` and
//      its private cache of `tail`. The blocks are never written by the same
//      thread, so there is no false sharing, including the "adjacent line"
//      kind caused by the L2 spatial prefetcher (see kFalseSharingRange).
//
//   3. Cached remote index. The producer only reloads the consumer's `head`
//      (a cache miss: the line lives in the other core's L1) when its cached
//      copy says the buffer is full. In steady state this turns one cross-core
//      miss per operation into one per ~capacity operations.
//
//   4. Per-slot sequence numbers (Signal::slot_sequence, the default). With
//      Signal::index the consumer learns about new data by reading `tail`,
//      then reads the slot: two dependent cross-core misses per message. With
//      slot_sequence, each slot carries `seq = index + 1`, written with release
//      right after the payload in the same cache line; the consumer polls the
//      slot itself, so a hand-off costs one line transfer instead of two.
//
//   5. acquire/release only. On x86-64 these compile to plain MOVs: no fences,
//      no locked instructions.
//
// C++26: preconditions are contracts (`pre(...)`). They are checked in tests
// and debug builds and compiled out of benchmark builds
// (-fcontract-evaluation-semantic=ignore), so they cost nothing on the hot path.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace spsc {

/// Cache line size on x86-64.
inline constexpr std::size_t kCacheLine = 64;

/// Distance at which two writers stop interfering on Intel since Sandy
/// Bridge: the L2 spatial prefetcher fetches lines in 128-byte-aligned pairs,
/// so two blocks in the same pair still ping-pong even when they are on
/// different 64-byte lines. We deliberately do not use
/// std::hardware_destructive_interference_size: GCC warns that its value can
/// change with -mtune, and the layout should be explicit and stable.
inline constexpr std::size_t kFalseSharingRange = 2 * kCacheLine;

/// How the consumer learns that a slot has been filled.
enum class Signal : unsigned char {
  index,          ///< read the producer's `tail` (classic Lamport queue)
  slot_sequence,  ///< read a sequence number stored inside the slot itself
};

/// Compile-time layout knobs. They exist so the benchmark can switch each
/// optimisation off in isolation (an ablation study); the defaults are what
/// production code should use. Being an aggregate, it is also introspected
/// with C++26 reflection to name benchmark variants (harness/describe.hpp).
struct Tuning {
  /// Alignment in bytes of each thread-owned block. 0 packs everything
  /// together, so producer and consumer indices share a cache line.
  std::size_t padding = kFalseSharingRange;
  /// Keep a thread-private copy of the other side's index and reload the
  /// shared atomic only when the copy says full (producer) / empty (consumer).
  bool cache_remote_index = true;
  /// How "data is ready" travels from producer to consumer.
  Signal signal = Signal::slot_sequence;
};

namespace detail {

template <class T>
struct IndexSlot {
  alignas(T) std::byte bytes[sizeof(T)];
};

template <class T>
struct SeqSlotLayout {
  std::atomic<std::size_t> seq{0};
  alignas(T) std::byte bytes[sizeof(T)];
};

/// Sequence slots are padded to a power of two (up to a cache line) so a slot
/// never straddles two lines: the sequence number and the payload must arrive
/// in the same line transfer, or the optimisation is lost.
template <class T>
inline constexpr std::size_t kSeqSlotAlign =
    sizeof(SeqSlotLayout<T>) <= kCacheLine
        ? std::max(std::bit_ceil(sizeof(SeqSlotLayout<T>)), alignof(SeqSlotLayout<T>))
        : std::max(kCacheLine, alignof(SeqSlotLayout<T>));

template <class T>
struct alignas(kSeqSlotAlign<T>) SeqSlot : SeqSlotLayout<T> {};

}  // namespace detail

template <class T, Tuning Cfg = Tuning{}>
class RingBuffer {
  static constexpr bool kSeq = Cfg.signal == Signal::slot_sequence;
  static constexpr std::size_t kAlign =
      Cfg.padding == 0 ? alignof(std::atomic<std::size_t>) : Cfg.padding;
  static_assert(std::has_single_bit(kAlign), "padding must be a power of two");
  static_assert(std::is_nothrow_destructible_v<T>);
  static_assert(std::atomic<std::size_t>::is_always_lock_free);

  using Slot = std::conditional_t<kSeq, detail::SeqSlot<T>, detail::IndexSlot<T>>;

 public:
  using value_type = T;
  static constexpr Tuning tuning = Cfg;

  /// `capacity` is rounded up to the next power of two. Throws on 0: the
  /// capacity usually comes from configuration, so this is a recoverable
  /// error, not a contract violation.
  explicit RingBuffer(std::size_t capacity)
      : capacity_(round_up_capacity(capacity)), mask_(capacity_ - 1), slots_(allocate(capacity_)) {}

  ~RingBuffer() {
    // Destructor runs with no concurrent producer/consumer by contract.
    const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
    for (std::size_t i = cons_.head.load(std::memory_order_relaxed); i != tail; ++i) {
      std::destroy_at(element(i));
    }
    std::destroy_n(slots_, capacity_);
    ::operator delete(slots_, storage_bytes(capacity_), std::align_val_t{kStorageAlign});
  }

  RingBuffer(const RingBuffer&) = delete("a ring buffer owns its slots and is shared by reference");
  RingBuffer& operator=(const RingBuffer&) = delete("a ring buffer owns its slots and is shared by reference");

  // ---- producer -----------------------------------------------------------

  /// Constructs an element in place. Returns false (and constructs nothing)
  /// if the buffer is full. If T's constructor throws, the buffer is unchanged.
  template <class... Args>
    requires std::constructible_from<T, Args&&...>
  [[nodiscard]] bool try_emplace(Args&&... args) noexcept(
      std::is_nothrow_constructible_v<T, Args&&...>) {
    const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
    if (free_slots(tail, 1) == 0) [[unlikely]] return false;
    std::construct_at(element(tail), std::forward<Args>(args)...);
    publish(tail, 1);
    return true;
  }

  [[nodiscard]] bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    return try_emplace(v);
  }
  [[nodiscard]] bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
    return try_emplace(std::move(v));
  }

  /// Pushes as many of `items` as fit, in order; returns how many. The index
  /// is published once for the whole batch, so N messages cost one release
  /// store and (amortised) far fewer cache-line transfers than N pushes.
  [[nodiscard]] std::size_t try_push_n(std::span<const T> items) noexcept(
      std::is_nothrow_copy_constructible_v<T>)
    requires std::copy_constructible<T>
  {
    const std::size_t tail = prod_.tail.load(std::memory_order_relaxed);
    const std::size_t n = std::min(items.size(), free_slots(tail, items.size()));
    for (std::size_t i = 0; i < n; ++i) std::construct_at(element(tail + i), items[i]);
    if (n != 0) publish(tail, n);
    return n;
  }

  // ---- consumer -----------------------------------------------------------

  /// Zero-copy peek: pointer to the oldest element, or nullptr if empty.
  /// The pointer stays valid until the consumer calls pop().
  [[nodiscard]] T* front() noexcept {
    const std::size_t head = cons_.head.load(std::memory_order_relaxed);
    if (!ready(head)) return nullptr;
    return element(head);
  }

  /// Destroys the oldest element.
  void pop() noexcept pre(readable()) {
    const std::size_t head = cons_.head.load(std::memory_order_relaxed);
    std::destroy_at(element(head));
    cons_.head.store(head + 1, std::memory_order_release);
  }

  /// Moves the oldest element into `out`. Returns false if empty.
  [[nodiscard]] bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
    T* p = front();
    if (p == nullptr) return false;
    out = std::move(*p);
    pop();
    return true;
  }

  /// Moves up to out.size() elements into `out`, oldest first; returns how
  /// many. The consumer index is published once for the whole batch.
  [[nodiscard]] std::size_t try_pop_n(std::span<T> out) noexcept(
      std::is_nothrow_move_assignable_v<T>) {
    const std::size_t head = cons_.head.load(std::memory_order_relaxed);
    const std::size_t n = ready_count(head, out.size());
    for (std::size_t i = 0; i < n; ++i) {
      T* p = element(head + i);
      out[i] = std::move(*p);
      std::destroy_at(p);
    }
    if (n != 0) cons_.head.store(head + n, std::memory_order_release);
    return n;
  }

  // ---- observers (approximate when called concurrently) --------------------

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::size_t head = cons_.head.load(std::memory_order_acquire);
    const std::size_t tail = prod_.tail.load(std::memory_order_acquire);
    return tail >= head ? tail - head : 0;
  }

  [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

 private:
  static constexpr std::size_t kStorageAlign = std::max(alignof(Slot), kFalseSharingRange);

  static std::size_t round_up_capacity(std::size_t n) {
    if (n == 0) throw std::invalid_argument("RingBuffer capacity must be > 0");
    if (n > (std::size_t{1} << (sizeof(std::size_t) * 8 - 2)))
      throw std::length_error("RingBuffer capacity too large");
    return std::bit_ceil(n);
  }

  // Round the allocation up to whole 128-byte blocks so no other heap object
  // can share the first or last block of the slot array (false sharing with
  // unrelated data that we would never see in a profile of this class).
  static std::size_t storage_bytes(std::size_t cap) {
    const std::size_t bytes = cap * sizeof(Slot);
    return (bytes + kStorageAlign - 1) / kStorageAlign * kStorageAlign;
  }

  static Slot* allocate(std::size_t cap) {
    auto* p = static_cast<Slot*>(::operator new(storage_bytes(cap), std::align_val_t{kStorageAlign}));
    std::uninitialized_value_construct_n(p, cap);  // seq = 0 for sequence slots
    return p;
  }

  Slot& slot(std::size_t index) const noexcept { return slots_[index & mask_]; }

  T* element(std::size_t index) const noexcept {
    return std::launder(reinterpret_cast<T*>(slot(index).bytes));
  }

  /// Producer: makes [tail, tail + n) visible to the consumer.
  void publish(std::size_t tail, std::size_t n) noexcept {
    if constexpr (kSeq) {
      for (std::size_t i = 0; i < n; ++i)
        slot(tail + i).seq.store(tail + i + 1, std::memory_order_release);
      // Nobody waits on `tail` in this mode; it is kept for size_approx() and
      // the destructor, and stays in the producer's own cache line.
      prod_.tail.store(tail + n, std::memory_order_relaxed);
    } else {
      prod_.tail.store(tail + n, std::memory_order_release);
    }
  }

  /// Producer: number of free slots, looking at the consumer's real `head`
  /// only if the cached copy shows fewer than `wanted`.
  std::size_t free_slots(std::size_t tail, std::size_t wanted) noexcept {
    if constexpr (Cfg.cache_remote_index) {
      std::size_t free = capacity_ - (tail - prod_.head_cache);
      if (free >= wanted) [[likely]] return free;
      prod_.head_cache = cons_.head.load(std::memory_order_acquire);
      return capacity_ - (tail - prod_.head_cache);
    } else {
      return capacity_ - (tail - cons_.head.load(std::memory_order_acquire));
    }
  }

  /// Consumer: is the element at `head` published?
  bool ready(std::size_t head) noexcept {
    if constexpr (kSeq) {
      return slot(head).seq.load(std::memory_order_acquire) == head + 1;
    } else if constexpr (Cfg.cache_remote_index) {
      if (head != cons_.tail_cache) [[likely]] return true;
      cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);
      return head != cons_.tail_cache;
    } else {
      return head != prod_.tail.load(std::memory_order_acquire);
    }
  }

  /// Consumer: number of consecutive published elements from `head`, up to `max`.
  std::size_t ready_count(std::size_t head, std::size_t max) noexcept {
    if constexpr (kSeq) {
      std::size_t n = 0;
      while (n < max && slot(head + n).seq.load(std::memory_order_acquire) == head + n + 1) ++n;
      return n;
    } else {
      if constexpr (Cfg.cache_remote_index) {
        if (cons_.tail_cache - head < max)
          cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);
        return std::min(max, cons_.tail_cache - head);
      } else {
        return std::min(max, prod_.tail.load(std::memory_order_acquire) - head);
      }
    }
  }

  /// Contract helper: side-effect-free version of ready() for the consumer.
  bool readable() const noexcept {
    const std::size_t head = cons_.head.load(std::memory_order_relaxed);
    if constexpr (kSeq) return slot(head).seq.load(std::memory_order_acquire) == head + 1;
    else return head != prod_.tail.load(std::memory_order_acquire);
  }

  // Read-only after construction: shared by both threads in S state, never
  // invalidated. Kept on its own block so index traffic cannot evict it.
  alignas(kAlign) const std::size_t capacity_;
  const std::size_t mask_;
  Slot* const slots_;

  // Written only by the producer.
  struct alignas(kAlign) ProducerBlock {
    std::atomic<std::size_t> tail{0};  // next index to write
    std::size_t head_cache{0};         // producer's stale view of cons_.head
  } prod_;

  // Written only by the consumer.
  struct alignas(kAlign) ConsumerBlock {
    std::atomic<std::size_t> head{0};  // next index to read
    std::size_t tail_cache{0};         // consumer's stale view of prod_.tail
  } cons_;
};

}  // namespace spsc
