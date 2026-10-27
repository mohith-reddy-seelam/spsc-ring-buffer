// SPDX-License-Identifier: MIT
#pragma once

#include <concepts>
#include <cstddef>
#include <span>

namespace spsc {

/// The interface every queue under test must provide. The benchmark harness
/// is written against these concepts, not against a concrete queue.
template <class Q>
concept BoundedQueue = requires(Q q, typename Q::value_type v) {
  { q.try_push(v) } -> std::same_as<bool>;
  { q.try_pop(v) } -> std::same_as<bool>;
  { q.capacity() } -> std::convertible_to<std::size_t>;
};

/// A queue that can move many elements per synchronisation.
template <class Q>
concept BatchQueue = BoundedQueue<Q> && requires(Q q, std::span<const typename Q::value_type> in,
                                                 std::span<typename Q::value_type> out) {
  { q.try_push_n(in) } -> std::same_as<std::size_t>;
  { q.try_pop_n(out) } -> std::same_as<std::size_t>;
};

}  // namespace spsc
