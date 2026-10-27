// SPDX-License-Identifier: MIT
#include "sampler.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <inplace_vector>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h must come first.
#include <dbghelp.h>
#endif

namespace harness {

#if defined(_WIN32)

struct Sampler::Impl {
  static constexpr std::size_t kMaxDepth = 96;
  // C++26 std::inplace_vector: a fixed-capacity vector with inline storage, so
  // recording a stack never allocates (the target thread is suspended and may
  // hold the heap lock; allocating here could deadlock).
  using Stack = std::inplace_vector<DWORD64, kMaxDepth>;  // innermost first

  struct Target {
    HANDLE handle;
    std::string name;
  };
  struct Sample {
    std::uint16_t thread;
    Stack pcs;
  };

  int hz;
  std::vector<Sample> samples;
  std::size_t used = 0;
  std::vector<Target> targets;
  std::mutex targets_mutex;
  std::atomic<bool> running{false};
  std::thread thread;

  Impl(int rate, double seconds, int max_threads)
      : hz(rate),
        samples(static_cast<std::size_t>(rate * seconds * max_threads) + 1) {}

  // Walks the suspended thread's stack using the x64 unwind tables.
  static void walk(CONTEXT ctx, Stack& out) {
    out.clear();
    while (ctx.Rip != 0 && out.try_push_back(ctx.Rip).has_value()) {
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
      if (fn == nullptr) {
        // Leaf function: no prologue, so the return address is at [RSP].
        if (ctx.Rsp == 0 || (ctx.Rsp & 7) != 0) break;
        ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      } else {
        PVOID handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                         &establisher_frame, nullptr);
      }
    }
  }

  bool sample_one(std::uint16_t index, Sample& s) {
    const HANDLE h = targets[index].handle;
    if (SuspendThread(h) == static_cast<DWORD>(-1)) return false;
    // SuspendThread is asynchronous; GetThreadContext waits until the target
    // has actually stopped.
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    const bool ok = GetThreadContext(h, &ctx) != 0;
    if (ok) walk(ctx, s.pcs);
    else s.pcs.clear();
    ResumeThread(h);
    s.thread = index;
    return !s.pcs.empty();
  }

  void run(int cpu) {
    if (cpu >= 0) SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    const LONGLONG period = freq.QuadPart / hz;
    QueryPerformanceCounter(&now);
    LONGLONG next = now.QuadPart + period;

    while (running.load(std::memory_order_relaxed)) {
      // Busy-wait: Sleep() granularity is ~1 ms even with timeBeginPeriod(1),
      // and the sampler has a core of its own.
      do {
        QueryPerformanceCounter(&now);
      } while (now.QuadPart < next);
      next += period;

      for (std::uint16_t i = 0; i < targets.size(); ++i) {
        if (used == samples.size()) {
          running.store(false);
          return;
        }
        if (sample_one(i, samples[used])) ++used;
      }
    }
  }
};

Sampler::Sampler(int hz, double max_seconds, int max_threads)
    : impl_(std::make_unique<Impl>(hz, max_seconds, max_threads)) {}

Sampler::~Sampler() {
  stop();
  for (auto& t : impl_->targets) CloseHandle(t.handle);
}

bool Sampler::supported() { return true; }

void Sampler::register_current_thread(const std::string& name) {
  HANDLE real = nullptr;
  // GetCurrentThread() is a pseudo-handle meaning "the caller"; duplicate it
  // into a real handle that the sampler thread can use.
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real,
                  THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                  0);
  std::scoped_lock lock(impl_->targets_mutex);
  impl_->targets.push_back({real, name});
}

void Sampler::start(int cpu) {
  impl_->running.store(true);
  impl_->thread = std::thread([this, cpu] { impl_->run(cpu); });
}

void Sampler::stop() {
  impl_->running.store(false);
  if (impl_->thread.joinable()) impl_->thread.join();
}

std::size_t Sampler::write_raw(const std::string& path) const {
  const HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);

  const auto exe_base = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
  std::map<DWORD64, std::string> names;

  auto symbolise = [&](DWORD64 addr) -> const std::string& {
    auto [it, inserted] = names.try_emplace(addr);
    if (!inserted) return it->second;
    HMODULE mod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(addr), &mod);
    char buf[512];
    const auto base = reinterpret_cast<DWORD64>(mod);
    if (base == exe_base) {
      std::snprintf(buf, sizeof buf, "exe+0x%llx",
                    static_cast<unsigned long long>(addr - exe_base));
      it->second = buf;
      return it->second;
    }
    std::string module = "?";
    if (mod != nullptr) {
      char file[MAX_PATH];
      GetModuleFileNameA(mod, file, MAX_PATH);
      module = file;
      module = module.substr(module.find_last_of("\\/") + 1);
    }
    alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    // Without PDBs DbgHelp falls back to the export table and names an
    // internal function after the nearest preceding export. Only trust the
    // name when the address is plausibly inside that export.
    constexpr DWORD64 kMaxPlausibleDisp = 0x400;
    if (SymFromAddr(process, addr, &disp, sym) && disp < kMaxPlausibleDisp) {
      it->second = module + "!" + sym->Name;
    } else {
      std::snprintf(buf, sizeof buf, "%s+0x%llx", module.c_str(),
                    static_cast<unsigned long long>(addr - base));
      it->second = buf;
    }
    return it->second;
  };

  std::map<std::string, std::size_t> folded;
  for (std::size_t i = 0; i < impl_->used; ++i) {
    const auto& s = impl_->samples[i];
    std::string line = impl_->targets[s.thread].name;
    for (std::size_t d = s.pcs.size(); d-- > 0;) {
      // Caller frames hold return addresses, which point one past the call
      // instruction (possibly into the next source line); step back into it.
      const DWORD64 pc = d == 0 ? s.pcs[d] : s.pcs[d] - 1;
      line += ';';
      line += symbolise(pc);
    }
    ++folded[line];
  }
  SymCleanup(process);

  std::ofstream out(path);
  for (const auto& [stack, count] : folded) out << stack << ' ' << count << '\n';
  return impl_->used;
}

#else  // !_WIN32: use `perf record -g` instead (see scripts/run_all.sh).

struct Sampler::Impl {};
Sampler::Sampler(int, double, int) : impl_(std::make_unique<Impl>()) {}
Sampler::~Sampler() = default;
bool Sampler::supported() { return false; }
void Sampler::register_current_thread(const std::string&) {}
void Sampler::start(int) {}
void Sampler::stop() {}
std::size_t Sampler::write_raw(const std::string&) const { return 0; }

#endif

}  // namespace harness
