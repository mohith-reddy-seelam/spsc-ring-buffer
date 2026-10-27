# Design of `spsc::RingBuffer` (v2, C++26)

The whole queue is one header, [`include/spsc/ring_buffer.hpp`](../include/spsc/ring_buffer.hpp).
This document explains each decision and points to the measurement in
[RESULTS.md](RESULTS.md) that justifies it.

## 1. Memory layout

```
 offset 0     ┌───────────────────────────────────────────────┐
              │ capacity_, mask_, slots_   (read-only)        │  shared, never written
 offset 128   ├───────────────────────────────────────────────┤
              │ tail        atomic<size_t>  written by P      │  producer-owned block
              │ head_cache  size_t          P only            │
 offset 256   ├───────────────────────────────────────────────┤
              │ head        atomic<size_t>  written by C      │  consumer-owned block
              │ tail_cache  size_t          C only            │
 offset 384   └───────────────────────────────────────────────┘

 slots_ ──►  ┌─────────────── 64-byte cache line ───────────────┐
             │ seq │ Msg (16 B) │ pad ││ seq │ Msg (16 B) │ pad │   two 32-byte slots
             └──────────────────────────────────────────────────┘
```

Each block is `alignas(128)`, so `sizeof(RingBuffer) == 384` (checked by a
`static_assert` in the tests).

**Why 128 and not 64?** A cache line is 64 bytes, but Intel's L2 *spatial
prefetcher* fetches lines in 128-byte-aligned pairs. When the consumer touches
its line, the prefetcher can pull in the producer's line next to it. Measured
(`ring_pad64` vs `ring`): 188 vs 156 ns p50 round trip, with no overlap
between repetitions. folly and crossbeam also use 128 on x86-64. We do not use
`std::hardware_destructive_interference_size` because GCC warns that its value
can change with `-mtune`.

## 2. Indices

`head` and `tail` are 64-bit counters that only ever increase. The slot is
`index & mask`. Consequences:

* `size = tail - head` is always correct across wrap-around (unsigned arithmetic).
* **Full** is `tail - head == capacity`, **empty** is `head == tail`, so all
  `capacity` slots are usable; none is sacrificed to tell full from empty.
* Overflow: at 10⁹ operations/s a 64-bit counter wraps after ~585 years.

Capacity is rounded up to a power of two so `% capacity` becomes `& mask`
(a division is 20-40 cycles; the AND is 1).

## 3. The ready signal lives in the slot (v2's main speed-up)

`Tuning::signal` selects how the consumer learns that a slot is filled.

**`Signal::index` (v1, the classic Lamport queue).** The producer writes the
payload, then stores `tail + 1` with release. The consumer loads `tail` with
acquire, sees it moved, then reads the slot. `tail` and the slot are in
*different* cache lines, both last written by the producer's core, so the
consumer takes **two dependent cross-core misses** per message: it cannot
start fetching the slot until the `tail` load returns.

**`Signal::slot_sequence` (v2 default).** Each slot is `{atomic seq; T value}`,
padded to a power of two (32 bytes for a 16-byte message) so it never
straddles two lines. The producer writes the payload, then stores
`seq = index + 1` with release, *in the same line*. The consumer polls only
`slot[head].seq == head + 1`, with acquire, and then reads the payload from
the line it already has: **one line transfer per hand-off**.

Measured: round-trip p50 228 → 156 ns, p99 298 → 178 ns, throughput 42 → 61
M msg/s (RESULTS.md §5).

Why it is correct:

* **Release/acquire on `seq`** orders the payload write before the consumer's
  payload read, which is exactly what release/acquire on `tail` did.
* **Lap safety.** Slot `k` is reused every `capacity` messages. On lap `L`
  it holds `seq = k + L·capacity + 1`. The consumer, at `head = k + L·capacity`,
  accepts only that exact value. The previous lap's value is smaller by
  `capacity`, and the initial value (0) never equals `index + 1`. A stale slot
  can never be mistaken for a fresh one (tested over 100 000 laps of a
  2-slot queue).
* **Producer side unchanged.** The producer still learns about free space from
  `head` (via its cache, §4). `tail` is still kept, with a relaxed store into
  the producer's own line, for `size_approx()` and the destructor. No thread
  waits on it, so it costs nothing.

The cost is memory: 32 bytes per 16-byte message, i.e. 32 KiB for 1024 slots,
which still fits comfortably in L1/L2. This is the same idea as the LMAX
Disruptor's per-slot sequence numbers and FastForward-style queues.

## 4. Cached remote index

The producer keeps `head_cache`, a possibly stale copy of the consumer's
`head`. A stale copy can only *understate* how far the consumer has got, so
the producer can only *underestimate* free space, and that is always safe. It
reloads the real `head` (a cross-core miss) **only when the stale copy says
the buffer is full**. With `Signal::index`, the consumer does the same with
`tail_cache`. With `slot_sequence` the consumer needs no cache at all, because
it reads the slot.

Measured (`ring_nocache` vs `ring`): round-trip p50 227 vs 156 ns. One
measured result I cannot fully explain yet is in RESULTS.md §5:
`ring_nocache` has *higher* saturated throughput (67 vs 61 M/s). The default
stays tuned for latency.

## 5. Memory ordering

| Operation | Ordering | What it guarantees |
|---|---|---|
| P loads its own `tail` | relaxed | Only P writes it |
| P loads `head` (refresh) | **acquire** | C's read of slot `i` *happens-before* P overwrites slot `i` |
| P stores `slot.seq` | **release** | The payload *happens-before* C sees the slot as ready |
| P stores `tail` | relaxed (slot_sequence) / release (index) | Nobody waits on it in slot_sequence mode |
| C loads `slot.seq` | **acquire** | Pairs with P's release: the payload is visible |
| C stores `head + 1` | **release** | Pairs with P's acquire: C is done with the slot |

On x86-64 (TSO), acquire loads and release stores are plain `mov`s: no fences,
no `lock` prefix. The orderings still matter. They stop the *compiler* from
reordering, and they make the code correct on ARM (`ldar`/`stlr`). `seq_cst`
would make each store an `xchg` (a full barrier, ~20 cycles) for nothing.

## 6. API

```cpp
template <class... Args> bool try_emplace(Args&&...);   // producer
bool try_push(const T&); bool try_push(T&&);             // producer
std::size_t try_push_n(std::span<const T>);              // producer, batch
T*   front();                                            // consumer, zero-copy peek
void pop() pre(readable());                              // consumer (C++26 contract)
bool try_pop(T&);                                        // consumer
std::size_t try_pop_n(std::span<T>);                     // consumer, batch
```

* **Non-blocking only.** Blocking (spin, spin-then-yield, futex) is a policy
  that belongs to the caller. The benchmark's spin policies show why: the
  "obvious" policy (`PAUSE`) is measurably wrong on this CPU.
* **Batch operations** publish once per batch: 61 → 186 M msg/s. They are a
  throughput tool; a message waiting to fill a batch pays for it in latency.
* **`front()`/`pop()`** let a consumer process a large message in place.
* **Strong exception guarantee** on `try_emplace`: if `T`'s constructor throws,
  nothing is published (tested).
* **Any `T`**: move-only, non-default-constructible and over-aligned types all
  work (tested). Storage is raw bytes, with `std::construct_at` /
  `std::destroy_at`. The destructor destroys exactly the live elements.

## 7. C++26 features used, and why

| Feature | Where | What it buys |
|---|---|---|
| **Contracts** (`pre(...)`) | `RingBuffer::pop() pre(readable())` | The precondition is part of the signature, checked in tests (`enforce`, with a death test proving it fires), and compiled out of benchmarks (`ignore`) so the hot path pays nothing. It replaces an `assert`. |
| **Static reflection** (`^^T`, `[: :]`, `std::meta`) | `harness/describe.hpp` | `machine.json` and each variant's description (`padding=128 cache_remote_index=true signal=slot_sequence`) are generated from the struct definitions. Add a field and it appears in the output, with no serialiser to forget. |
| **Expansion statements** (`template for`) | `bench_main.cpp`, `describe.hpp` | The benchmark matrix is a `std::tuple` of variant types, expanded at compile time. Adding a queue variant is one line in `workloads.hpp`. |
| **`= delete("reason")`** | copy constructors | The compiler error says *why* a queue can't be copied. |
| **`std::inplace_vector`** | `harness/sampler.cpp` | Fixed-capacity stacks with inline storage: the profiler never allocates while the target thread is suspended, which could deadlock on the heap lock. |
| **`std::saturating_cast`** | `harness/latency.hpp` | Clamps a 64-bit cycle count into the 32-bit sample buffer without a hand-written branch. |
| **`std::breakpoint_if_debugging`** | `bench/workloads.hpp` | A FIFO violation in a benchmark stops in the debugger, if one is attached. |

GCC 16 notes: reflection needs `-freflection`; contracts and `<debugging>`
need `-lstdc++exp` (on MinGW, `collect2` hides the resulting "undefined
reference" message, so a missing `-lstdc++exp` shows up as a silent link
failure). Both are set in `CMakeLists.txt`.

## 8. What was tried and rejected

| Idea | Result | Kept? |
|---|---|---|
| Ready flag in the slot (`slot_sequence`) | -32 % p50 RTT, +46 % throughput | **yes**, v2 default |
| Batch API | 3× throughput | **yes**, as an additional API |
| 128-byte padding instead of 64 | 156 vs 188 ns p50 RTT | **yes** |
| `PAUSE` in the spin loops | p99 worse in all 15 repetitions over three runs | no (RESULTS.md §6) |
| `std::deque` → preallocated ring *under the mutex* | Round trip unchanged | baseline only: the lock, not allocation, is the cost |

## 9. Not done (yet)

* **Explain `ring_nocache`'s higher throughput** with cache-line transfer
  counters (`perf c2c` on Linux).
* **Huge pages** for the slot array: irrelevant at 32 KiB, worth testing at 1 M slots.
* **Linux with `isolcpus`/`nohz_full`**: the tails here are dominated by Windows
  platform noise (RESULTS.md §4).
