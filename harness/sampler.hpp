// SPDX-License-Identifier: MIT
//
// In-process sampling profiler for Windows, producing input for a flame graph.
//
// Linux has `perf record -g`; Windows has nothing equivalent that works on
// MinGW/DWARF binaries without a GUI. So: a dedicated sampler thread wakes
// at a fixed rate, and for each registered thread
//
//   SuspendThread -> GetThreadContext -> unwind with RtlVirtualUnwind -> Resume
//
// x64 Windows requires every non-leaf function to have unwind data in .pdata,
// and GCC emits it (SEH), so unwinding works through our code, the static
// libstdc++/winpthreads, and system DLLs alike, without frame pointers.
//
// Output is one line per unique stack, root first, in a raw form that
// tools/flamegraph.py symbolises:
//
//   <thread>;<frame>;<frame>;... <count>
//
// where a frame in the main executable is `exe+0x<rva>` (resolved later with
// addr2line, which understands DWARF and inlining), and a frame elsewhere is
// `module!symbol` (resolved here with DbgHelp's export-table fallback).
//
// Caveat: the sampler stops the target while walking its stack. If the target
// were suspended while holding the loader lock, the walk could deadlock. Our
// targets never load modules while being sampled, so this cannot happen here.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace harness {

class Sampler {
 public:
  /// `max_seconds` sizes the preallocated sample buffer; sampling stops when
  /// it is full. Nothing is allocated while sampling.
  Sampler(int hz, double max_seconds, int max_threads = 4);
  ~Sampler();

  Sampler(const Sampler&) = delete;
  Sampler& operator=(const Sampler&) = delete;

  /// Called from the thread to be profiled, before start().
  void register_current_thread(const std::string& name);

  /// Starts the sampler thread, pinned to `cpu` (-1 = not pinned).
  void start(int cpu);
  void stop();

  /// Writes the collapsed raw stacks. Returns the number of samples.
  std::size_t write_raw(const std::string& path) const;

  [[nodiscard]] static bool supported();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace harness
