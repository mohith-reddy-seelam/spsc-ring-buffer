// SPDX-License-Identifier: MIT
//
// Lock-based baselines with the same bounded interface as spsc::RingBuffer,
// so every benchmark is a template over the queue type.
//
//   MutexQueue  - std::mutex around std::queue (std::deque). The "obvious"
//                 implementation most codebases start with. Allocates a new
//                 deque block every N pushes and frees one every N pops.
//
//   MutexRing   - std::mutex around a preallocated ring. Same lock as
//                 MutexQueue but no allocation, so the difference between the
//                 two isolates the container cost from the lock cost.
//
// Both offer batch operations that take the lock once per batch, so batching
// is compared like for like and is not an advantage only the ring gets.
#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <mutex>
#include <queue>
#include <span>
#include <utility>
#include <vector>

namespace spsc {

template <class T>
class MutexQueue {
 public:
  using value_type = T;

  explicit MutexQueue(std::size_t capacity) : capacity_(capacity) {}
  MutexQueue(const MutexQueue&) = delete("owns a std::mutex; share it by reference");
  MutexQueue& operator=(const MutexQueue&) = delete("owns a std::mutex; share it by reference");

  template <class... Args>
  [[nodiscard]] bool try_emplace(Args&&... args) {
    std::scoped_lock lock(mutex_);
    if (queue_.size() >= capacity_) return false;
    queue_.emplace(std::forward<Args>(args)...);
    return true;
  }
  [[nodiscard]] bool try_push(const T& v) { return try_emplace(v); }
  [[nodiscard]] bool try_push(T&& v) { return try_emplace(std::move(v)); }

  [[nodiscard]] std::size_t try_push_n(std::span<const T> items) {
    std::scoped_lock lock(mutex_);
    const std::size_t n = std::min(items.size(), capacity_ - queue_.size());
    for (std::size_t i = 0; i < n; ++i) queue_.push(items[i]);
    return n;
  }

  [[nodiscard]] bool try_pop(T& out) {
    std::scoped_lock lock(mutex_);
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  [[nodiscard]] std::size_t try_pop_n(std::span<T> out) {
    std::scoped_lock lock(mutex_);
    const std::size_t n = std::min(out.size(), queue_.size());
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = std::move(queue_.front());
      queue_.pop();
    }
    return n;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  std::mutex mutex_;
  std::queue<T> queue_;
  const std::size_t capacity_;
};

template <class T>
class MutexRing {
 public:
  using value_type = T;

  explicit MutexRing(std::size_t capacity)
      : slots_(std::bit_ceil(capacity)), mask_(slots_.size() - 1) {}
  MutexRing(const MutexRing&) = delete("owns a std::mutex; share it by reference");
  MutexRing& operator=(const MutexRing&) = delete("owns a std::mutex; share it by reference");

  template <class... Args>
  [[nodiscard]] bool try_emplace(Args&&... args) {
    std::scoped_lock lock(mutex_);
    if (tail_ - head_ == slots_.size()) return false;
    slots_[tail_++ & mask_] = T(std::forward<Args>(args)...);
    return true;
  }
  [[nodiscard]] bool try_push(const T& v) { return try_emplace(v); }
  [[nodiscard]] bool try_push(T&& v) { return try_emplace(std::move(v)); }

  [[nodiscard]] std::size_t try_push_n(std::span<const T> items) {
    std::scoped_lock lock(mutex_);
    const std::size_t n = std::min(items.size(), slots_.size() - (tail_ - head_));
    for (std::size_t i = 0; i < n; ++i) slots_[tail_++ & mask_] = items[i];
    return n;
  }

  [[nodiscard]] bool try_pop(T& out) {
    std::scoped_lock lock(mutex_);
    if (head_ == tail_) return false;
    out = std::move(slots_[head_++ & mask_]);
    return true;
  }

  [[nodiscard]] std::size_t try_pop_n(std::span<T> out) {
    std::scoped_lock lock(mutex_);
    const std::size_t n = std::min(out.size(), tail_ - head_);
    for (std::size_t i = 0; i < n; ++i) out[i] = std::move(slots_[head_++ & mask_]);
    return n;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

 private:
  std::mutex mutex_;
  std::vector<T> slots_;  // T must be default-constructible; fine for a baseline
  const std::size_t mask_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
};

}  // namespace spsc
