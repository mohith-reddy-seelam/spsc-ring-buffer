# Benchmark harness specification

This document is the contract between the C++ implementation in this repo and
any port (the Rust port is next). An implementation that follows it produces
files that `tools/analyze.py` can plot **on the same axes** as the C++ results:

```bash
python tools/analyze.py results/cpp_i5-6500T results/rust_i5-6500T --out results/compare
```

The C++ reference implementation of every rule below is in
[`bench/bench_main.cpp`](../bench/bench_main.cpp) and
[`harness/latency.cpp`](../harness/latency.cpp).

---

## 1. Fixed parameters

| Parameter | Value | Why |
|---|---|---|
| Message | 16 bytes: `seq: u64`, `tsc: u64` | Small enough that the queue's synchronisation, not memcpy, is measured |
| Capacity | 1024 slots (power of two) | Same for every queue, including the mutex baselines |
| Stop signal | message with `seq = u64::MAX` | No side channel needed to end a run |
| Producer | pinned to CPU `n-2` by default (`--producer_cpu`) | |
| Consumer / echo | pinned to CPU `n-1` by default (`--consumer_cpu`) | CPU 0 takes most interrupts on Windows |
| Priority | Windows: `HIGH_PRIORITY_CLASS` + `THREAD_PRIORITY_TIME_CRITICAL`; Linux: `SCHED_FIFO` max if permitted, else nice -20 | Record what was applied in `machine.json` |
| Spin policy | busy re-poll, no `PAUSE`, no yield | Unless the variant under test says otherwise |
| Warm-up | 100 000 operations per repetition, not recorded | Caches, TLB, branch predictors, CPU frequency |
| Repetitions | 5 | Samples are pooled across repetitions for percentiles |

## 2. Clock

* Timestamps are raw **TSC** reads. Start of a measured region: `lfence; rdtsc; lfence`.
  End: `rdtscp; lfence`. Pacing loops may use a bare `rdtsc`.
* TSC frequency is calibrated at start-up against the OS monotonic clock:
  best of three 100 ms windows (the highest rate wins, because a preemption can
  only make a window look slower).
* The run must check CPUID `0x80000007:EDX[8]` (invariant TSC) and record it.
  Without it, cross-core one-way numbers are invalid.

In Rust: `core::arch::x86_64::{_rdtsc, __rdtscp, _mm_lfence}`.

## 3. Scenarios

Every latency scenario records **one sample per message**, in TSC cycles, into
a preallocated buffer that is touched before the run starts (no page faults
while measuring).

### `rtt` - round trip
Two queues of the same type. The producer reads `t0`, pushes into A, then spins
until it pops from B, then reads `t1`. The echo thread pops from A and pushes
into B. Sample = `t1 - t0`. 1 000 000 samples per repetition.

### `oneway_<interval>ns` - paced one-way latency
The producer sends one message every `interval` ns (default 1000), on an
**absolute schedule**: `scheduled += interval` regardless of when the previous
send actually happened. The message's `tsc` field is the **scheduled** time,
not the actual send time. The consumer computes `rdtscp_now - msg.tsc`.

This makes the measurement *coordinated-omission safe*: if the producer stalls
(preemption, a full queue), every message that should have been sent during
the stall is charged the delay, as a real feed handler would experience it.
Using the actual send time instead hides exactly the stalls you care about.
1 000 000 samples per repetition.

### `throughput` - saturated
The producer pushes as fast as possible. The consumer pops as fast as possible
and **checks that `seq` increases by exactly one** (a failure is reported, never
ignored). Time is measured per batch of 65 536 messages, from first push until
the consumer acknowledges the last one, so the reported rate includes draining
the queue. Report messages per second.

### `throughput_batch32` - saturated, batched
As `throughput`, but the producer pushes arrays of 32 messages with
`try_push_n` (re-offering any part that did not fit) and the consumer drains
with `try_pop_n` into a 32-element buffer. Run for every queue that has a batch
API (in C++, all three headline queues).

### `noise` - platform noise (run first)
A thread pinned to each benchmark CPU reads the TSC in a tight loop for 5 s and
records every gap longer than 1 µs. Record the gap durations as samples, plus
`gaps_per_s` and `stolen_pct`. This is the floor under every tail percentile,
and it must be published alongside them.

## 4. Statistics

* Percentiles are **nearest rank** over the pooled, sorted samples (in ns):
  `value = sorted[ceil(q * n) - 1]`, with `q = 0` giving the minimum.
  No interpolation, so a reported percentile is always an observed sample.
* Report at least: min, mean, p50, p90, p99, p99.9, p99.99, max.

## 5. Output files

All in one results directory. CSV is comma-separated with a header row, `.`
as the decimal point, and latencies in **nanoseconds** as floating point.

### `machine.json`
```json
{
  "cpu": "Intel(R) Core(TM) i5-6500T CPU @ 2.50GHz",
  "os": "Windows 11 build 22631",
  "compiler": "gcc 16.2.0 (mingw-w64)",          // Rust: "rustc 1.xx.y"
  "build_flags": "-O3 -march=native ...",         // Rust: profile + RUSTFLAGS
  "logical_cpus": 4,
  "invariant_tsc": true,
  "tsc_ghz": 2.496,
  "producer_cpu": 2,
  "consumer_cpu": 3,
  "priority": "HIGH_PRIORITY_CLASS / THREAD_PRIORITY_TIME_CRITICAL"
}
```

### `latency_summary.csv`
```
impl,scenario,queue,samples,min_ns,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns
cpp,rtt,ring,5000000,182.7,263.9,250.0,278.0,354.2,1178.7,17729.5,1366898.7
```
`impl` is `cpp`, `rust`, ... ; `queue` names are listed in section 6.

### `percentiles/<scenario>__<queue>.csv`
```
percentile,latency_ns
0,182.69
10.87,240.38
...
100,1366898.73
```
Rows for `q = 1 - 10^(-k/20)` for `k = 0..120` (so 0 %, ..., 99.9999 %), with
`k = 0` replaced by `q = 0`, plus a final row `100,<max>`.

### `hist/<scenario>__<queue>.csv`
```
bucket_lo_ns,bucket_hi_ns,count
181.02,186.68,12
```
Log-linear buckets: bucket `i = floor(16 * log2(max(ns, 1)))` covers
`[2^(i/16), 2^((i+1)/16))` ns, i.e. 16 buckets per doubling (about 4.4 %
wide). Only non-empty buckets are written, in increasing order.

### `throughput.csv` (ports) or `benchmark.json` (Google Benchmark)
Ports that do not use Google Benchmark write
```
impl,scenario,queue,repetition,msgs_per_s
rust,throughput,ring,0,61234567.0
rust,throughput_batch32,ring,0,185000000.0
```
`tools/analyze.py` reads `throughput.csv` if present, else
`benchmark.json`.

## 6. Queue names

| Name | Meaning |
|---|---|
| `ring` | the lock-free SPSC ring with default tuning (per-slot sequence numbers, 128 B padding, cached index) |
| `ring_index` | consumer learns about new data from the producer's `tail` (classic Lamport; the v1 design) |
| `mutex_queue` | the language's standard mutex around its standard queue (`std::mutex` + `std::queue`; Rust: `Mutex<VecDeque>`) |
| `mutex_ring` | standard mutex around a preallocated ring |
| `ring_pad64` | 64 B padding instead of 128 B |
| `ring_nocache` | no cached remote index |
| `ring_packed` | no padding |
| `ring_naive` | textbook Lamport queue: no padding, no cached index, signal via `tail` |
| `ring_pause` | default ring, `PAUSE` in every spin loop (`std::hint::spin_loop()` in Rust) |

A port may add queues (for example `crossbeam_array`, `rtrb`); give them new
names and do not reuse the ones above for anything else.
