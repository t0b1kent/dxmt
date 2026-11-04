// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// CPU stage (GAPS, 01.11.2026). All keys default to 0: with every key 0 the layer does exactly what it did before.
// Diagnostics print totals to stderr (WriteFile on STD_ERROR_HANDLE), never through Wine debug channels.
//
//   MACRUNNER_DX12_ALLOC_RESET_SPLIT=<N>  every N ID3D12CommandAllocator::Reset calls (process-wide) one line
//                                         "dx12_alloc_reset_split" with the cumulative wall time of each part of Reset
//                                         (destructor walk of the recorded passes, spilled CPU heap frees, release of the
//                                         old GPU heap buffer, newBuffer of the new one, ICB clear, tessellation scratch
//                                         reset, copy temp free) and the number of passes by kind. Observation only.
//   MACRUNNER_DX12_ALLOC_RESET_FAST=1     Reset keeps the allocator's GPU heap MTLBuffer (same memory, same GPU address)
//                                         instead of releasing it and wrapping the same memory again with newBuffer, and
//                                         keeps spilled 4 MB CPU heap blocks for reuse instead of free + malloc.
//                                         Valid because D3D12 requires the GPU to have finished the allocator's commands
//                                         before Reset; the memory was reused in place before the key as well.
//   MACRUNNER_DX12_ALLOC_RESET_FAST=9     NEGATIVE CONTROL: keeps the buffer, but every second Reset gives the CPU side
//                                         a different 4 MB block, so the GPU reads argument data written two Resets
//                                         earlier. A correctness stand must fail with it.
//   MACRUNNER_DX12_FENCE_WAIT_EVENT=1     ID3D12Fence::GetCompletedValue: when one thread polls the same fence and gets
//                                         the same value FENCE_WAIT_SPIN times in a row (default 16), each poll less than
//                                         FENCE_WAIT_GAP_US after the previous one (default 10 us), the next poll blocks
//                                         on the fence's MTLSharedEvent until the value passes the one it saw, for at most
//                                         1 ms, then returns the current value. Only without FENCE_PUBLICATION (the
//                                         ordered path keeps polling). A poll that returns a new value or a gap above the
//                                         limit starts counting from zero.
//   MACRUNNER_DX12_FENCE_WAIT_EVENT=2     observation only: the same detection, never blocks; counts the polls that would
//                                         have blocked ("spin_polls") and the gap histogram (with FENCE_WAIT_STATS=<N>: one
//                                         line every N spin polls).
//   MACRUNNER_DX12_FENCE_WAIT_EVENT=3     as 1, but the wait goes through the device's MTLSharedEventListener and a per-thread
//                                         Win32 event (the SetEventOnCompletion path) instead of MTLSharedEvent waitUntilSignaledValue,
//                                         which on GPU-signalled values woke only at the 1 ms timeout in about half of the waits (stand).
//   MACRUNNER_DX12_FENCE_WAIT_EVENT=4     as 1, but instead of a wake-up wait the poll sleeps in quanta of FENCE_WAIT_QUANTUM_US
//                                         (default 50 us; NtDelayExecution, a select() in Wine's unix side) and re-reads the value
//                                         after each, for at most 1 ms: no wake-up mechanism to be late, added delay <= one quantum
//                                         plus timer slack, CPU only for the wake-ups.
//   MACRUNNER_DX12_FENCE_WAIT_EVENT=9     NEGATIVE CONTROL: the blocking poll sleeps 2 ms without waking on the signal.
//                                         A latency stand must show the added delay.
//   MACRUNNER_DX12_FENCE_WAIT_STATS=<N>   every N blocking polls one line "dx12_fence_wait" (polls seen, blocks, woken by
//                                         the signal, timeouts, wait time, gap histogram of repeated polls).
//   MACRUNNER_DX12_FENCE_CALLERS=<N>      (GAPS 01.11, observation only) every N-th GetCompletedValue of a thread walks the stack
//                                         (RtlWalkFrameChain) to the first x64 frame (not RtlIsEcCode) = the return address in
//                                         the caller, and counts it per (caller, fence): calls, value changes seen at that caller,
//                                         and whether the returned value had reached the fence's last Signal ("idle": nothing
//                                         pending, the caller cannot be waiting on it) or not ("pending"). Every 4096 samples one
//                                         block of "dx12_fence_callers" lines (top 16, module+offset of the caller and of its caller).
#pragma once
#include <windows.h>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "d3d12_c17_stats.hpp"

namespace dxmt::cpu_stage {

inline uint64_t Ticks() { uint64_t v; asm volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
inline uint64_t TickFrequency() { uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

inline void
Print(const char *text, int n) {
  if (n <= 0) return;
  const DWORD saved = GetLastError();
  DWORD written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, DWORD(n), &written, nullptr);
  SetLastError(saved);
}

inline unsigned
Key(const char *name, unsigned limit, const char *marker) {
  const unsigned v = c17::ReadKey(name, limit);
  if (v) {
    char line[160];
    Print(line, snprintf(line, sizeof(line), "%s=%u\n", marker, v));
  }
  return v;
}

inline unsigned AllocResetFast() {
  static const unsigned v = [] { const unsigned k = Key("MACRUNNER_DX12_ALLOC_RESET_FAST", 9, "dx12_alloc_reset_fast enabled");
                                 return (k == 1 || k == 9) ? k : 0u; }();
  return v;
}
inline unsigned AllocResetSplit() {
  static const unsigned v = Key("MACRUNNER_DX12_ALLOC_RESET_SPLIT", 100000000, "dx12_alloc_reset_split period");
  return v;
}
inline unsigned FenceWaitEvent() {
  static const unsigned v = [] { const unsigned k = Key("MACRUNNER_DX12_FENCE_WAIT_EVENT", 9, "dx12_fence_wait_event enabled");
                                 return (k == 1 || k == 2 || k == 3 || k == 4 || k == 9) ? k : 0u; }();
  return v;
}
inline unsigned FenceWaitSpin() {
  static const unsigned v = [] { const unsigned k = c17::ReadKey("MACRUNNER_DX12_FENCE_WAIT_SPIN", 1000000); return k ? k : 16u; }();
  return v;
}
inline uint64_t FenceWaitGapTicks() {
  static const uint64_t v = [] {
    const unsigned us = c17::ReadKey("MACRUNNER_DX12_FENCE_WAIT_GAP_US", 100000);
    return uint64_t(us ? us : 10u) * TickFrequency() / 1000000u;
  }();
  return v;
}
inline unsigned FenceWaitQuantumUs() {
  static const unsigned v = [] { const unsigned k = c17::ReadKey("MACRUNNER_DX12_FENCE_WAIT_QUANTUM_US", 1000); return k ? k : 50u; }();
  return v;
}
// NtDelayExecution with a relative timeout in 100 ns units (Sleep() has 1 ms granularity).
inline void SleepMicroseconds(unsigned us) {
  using Fn = LONG(NTAPI *)(BOOLEAN, PLARGE_INTEGER);
  static const Fn fn = [] {
    HMODULE m = GetModuleHandleA("ntdll.dll");
    return m ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(m, "NtDelayExecution"))) : nullptr;
  }();
  if (!fn) { Sleep(us >= 1000 ? us / 1000 : 0); return; }
  LARGE_INTEGER t; t.QuadPart = -LONGLONG(us) * 10;
  fn(FALSE, &t);
}
inline unsigned FenceWaitStats() {
  static const unsigned v = c17::ReadKey("MACRUNNER_DX12_FENCE_WAIT_STATS", 100000000);
  return v;
}

// ---- MACRUNNER_DX12_FENCE_CACHE_US=<T> (GAPS 01.11 12:xx, default 0) ----
// GetCompletedValue returns the value read from the fence's MTLSharedEvent at most T microseconds ago without taking the fence
// mutex and without the unix call; older than T (or invalidated) -> the normal read, which refreshes the cache. A value seen <= T us
// ago is what a slightly earlier call would have returned (the GPU may complete right after any read). Kept exact: a CPU Signal or a
// rewind on the fence invalidates the cache (generation), values are stored with an atomic max (two readers cannot move it back).
// Elden Ring's main thread walks a deferred-free list before Present calling GetCompletedValue once per node (~4 800 per frame,
// eldenring.exe+0x1eb1260): the number of calls is the game's, only their price changes.
inline unsigned FenceCacheUs() {
  static const unsigned v = Key("MACRUNNER_DX12_FENCE_CACHE_US", 100000, "dx12_fence_cache_us");
  return v;
}
inline uint64_t FenceCacheTicks() {
  static const uint64_t v = uint64_t(FenceCacheUs()) * TickFrequency() / 1000000u;
  return v;
}
// NEGATIVE CONTROL (stand only): MACRUNNER_DX12_FENCE_CACHE_NOINVAL=1 skips the invalidation on Signal / rewind; a stand must catch it.
inline bool FenceCacheNoInval() {
  static const bool v = Key("MACRUNNER_DX12_FENCE_CACHE_NOINVAL", 1, "dx12_fence_cache_noinval NEGATIVE CONTROL") == 1;
  return v;
}
struct FenceCacheStats { std::atomic<uint64_t> hits{0}, misses{0}, invalidations{0}; };
inline FenceCacheStats &CacheStats() { static FenceCacheStats s; return s; }

// ---- GetCompletedValue callers (observation only) ----
inline unsigned FenceCallers() {
  static const unsigned v = Key("MACRUNNER_DX12_FENCE_CALLERS", 100000000, "dx12_fence_callers period");
  return v;
}
struct CallerEntry { uint64_t ret = 0, ret2 = 0; const void *fence = nullptr; uint64_t count = 0, changes = 0, last = 0, idle = 0, pending = 0; };
struct CallerTable {
  std::mutex lock;
  CallerEntry e[64];
  uint64_t samples = 0, no_x64 = 0, overflow = 0, walk_failed = 0;
};
inline CallerTable &Callers() { static CallerTable t; return t; }
inline void ModuleOffset(uint64_t addr, char *out, size_t size) {
  HMODULE m = nullptr;
  if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(addr), &m) && m) {
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, sizeof(path));
    const char *base = path;
    for (const char *p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    snprintf(out, size, "%s+0x%llx", base, (unsigned long long)(addr - reinterpret_cast<uint64_t>(m)));
  } else snprintf(out, size, "?+0x%llx", (unsigned long long)addr);
}
inline void ReportCallers(CallerTable &t) {   // called with t.lock held
  char line[512];
  Print(line, snprintf(line, sizeof(line), "dx12_fence_callers samples=%llu no_x64=%llu walk_failed=%llu overflow=%llu period=%u\n",
                       (unsigned long long)t.samples, (unsigned long long)t.no_x64, (unsigned long long)t.walk_failed,
                       (unsigned long long)t.overflow, FenceCallers()));
  bool used[64] = {};
  for (int n = 0; n < 16; ++n) {
    int best = -1;
    for (int i = 0; i < 64; ++i) if (!used[i] && t.e[i].count && (best < 0 || t.e[i].count > t.e[best].count)) best = i;
    if (best < 0) break;
    used[best] = true;
    const CallerEntry &x = t.e[best];
    char a[160], b[160];
    ModuleOffset(x.ret, a, sizeof(a)); ModuleOffset(x.ret2, b, sizeof(b));
    Print(line, snprintf(line, sizeof(line), "dx12_fence_callers_site rank=%d caller=%s caller2=%s fence=%p count=%llu changes=%llu idle=%llu pending=%llu last=%llu\n",
                         n, a, b, x.fence, (unsigned long long)x.count, (unsigned long long)x.changes, (unsigned long long)x.idle,
                         (unsigned long long)x.pending, (unsigned long long)x.last));
  }
}
// Sampled from GetCompletedValue: value = what it returns, last_signal() = the fence's last Signal value (under its lock).
template <typename LastSignal>
inline void NoteCaller(const void *fence, uint64_t value, LastSignal &&last_signal) {
  static thread_local unsigned calls = 0;
  const unsigned period = FenceCallers();
  if (++calls < period) return;
  calls = 0;
  using Walk = ULONG(WINAPI *)(void **, ULONG, ULONG);
  using IsEc = BOOLEAN(WINAPI *)(ULONG_PTR);
  static const Walk walk = [] { HMODULE m = GetModuleHandleA("ntdll.dll"); return m ? reinterpret_cast<Walk>(reinterpret_cast<void *>(GetProcAddress(m, "RtlWalkFrameChain"))) : nullptr; }();
  static const IsEc is_ec = [] { HMODULE m = GetModuleHandleA("ntdll.dll"); return m ? reinterpret_cast<IsEc>(reinterpret_cast<void *>(GetProcAddress(m, "RtlIsEcCode"))) : nullptr; }();
  void *frames[24] = {};
  const ULONG n = walk ? walk(frames, 24, 0) : 0;
  uint64_t ret = 0, ret2 = 0;
  for (ULONG i = 0; i < n; ++i) {
    const ULONG_PTR pc = reinterpret_cast<ULONG_PTR>(frames[i]);
    if (is_ec && is_ec(pc)) continue;
    if (!ret) ret = pc; else { ret2 = pc; break; }
  }
  const uint64_t signal = last_signal();
  auto &t = Callers();
  std::lock_guard<std::mutex> guard(t.lock);
  ++t.samples;
  if (!n) ++t.walk_failed;
  if (!ret) ++t.no_x64;
  CallerEntry *slot = nullptr;
  for (auto &x : t.e) {
    if (x.count && x.ret == ret && x.fence == fence) { slot = &x; break; }
    if (!x.count) { slot = &x; slot->ret = ret; slot->fence = fence; slot->last = value; break; }
  }
  if (!slot) ++t.overflow;
  else {
    slot->ret2 = ret2;
    if (slot->count && value != slot->last) ++slot->changes;
    slot->last = value;
    ++slot->count;
    (value >= signal ? slot->idle : slot->pending) += 1;
  }
  if (t.samples % 4096 == 0) ReportCallers(t);
}

// ---- Allocator::Reset split (observation only) ----
enum ResetPart : unsigned { PartDestroy, PartSpill, PartRelease, PartNewBuffer, PartIcb, PartTess, PartCopy, PartCount };
struct ResetSplit {
  std::atomic<uint64_t> resets{0}, total{0}, part[PartCount]{}, passes[8]{}, spilled_blocks{0}, reused_blocks{0}, kept_buffers{0};
};
inline ResetSplit &Split() { static ResetSplit s; return s; }

inline void
ReportSplit(uint64_t resets) {
  auto &s = Split();
  const double us = 1e6 / double(TickFrequency()), n = double(resets ? resets : 1);
  static const char *names[PartCount] = {"destroy", "spill_free", "release", "new_buffer", "icb", "tess", "copy"};
  char line[1024];
  int k = snprintf(line, sizeof(line), "dx12_alloc_reset_split resets=%llu fast=%u total_us=%.2f", (unsigned long long)resets,
                   AllocResetFast(), double(s.total.load()) * us / n);
  for (unsigned i = 0; i < PartCount && k < int(sizeof(line)); ++i)
    k += snprintf(line + k, sizeof(line) - k, " %s_us=%.2f", names[i], double(s.part[i].load()) * us / n);
  if (k < int(sizeof(line)))
    k += snprintf(line + k, sizeof(line) - k,
                  " passes_null=%.2f clear=%.2f render=%.2f blit=%.2f compute=%.2f argcompute=%.2f accel=%.2f resolve=%.2f"
                  " spilled_per_reset=%.3f reused_blocks=%llu kept_buffers=%llu\n",
                  s.passes[0].load() / n, s.passes[1].load() / n, s.passes[2].load() / n, s.passes[3].load() / n,
                  s.passes[4].load() / n, s.passes[5].load() / n, s.passes[6].load() / n, s.passes[7].load() / n,
                  s.spilled_blocks.load() / n, (unsigned long long)s.reused_blocks.load(),
                  (unsigned long long)s.kept_buffers.load());
  Print(line, k < int(sizeof(line)) ? k : int(sizeof(line)) - 1);
}

// ---- Fence wait instead of a polling spin ----
struct FenceWaitCounters {
  std::atomic<uint64_t> polls{0}, repeats{0}, blocks{0}, woke{0}, timeouts{0}, wait_ticks{0}, gap[5]{}, spin_polls{0};
};
inline FenceWaitCounters &FenceStats() { static FenceWaitCounters c; return c; }

inline void
ReportFenceWait() {
  auto &c = FenceStats();
  const double us = 1e6 / double(TickFrequency());
  char line[512];
  const int k = snprintf(line, sizeof(line),
      "dx12_fence_wait mode=%u spin=%u gap_us=%.1f polls=%llu repeats=%llu spin_polls=%llu blocks=%llu woke=%llu timeouts=%llu wait_us_per_block=%.2f"
      " gap_lt2=%llu lt5=%llu lt10=%llu lt50=%llu ge50=%llu\n",
      FenceWaitEvent(), FenceWaitSpin(), double(FenceWaitGapTicks()) * us, (unsigned long long)c.polls.load(),
      (unsigned long long)c.repeats.load(), (unsigned long long)c.spin_polls.load(), (unsigned long long)c.blocks.load(), (unsigned long long)c.woke.load(),
      (unsigned long long)c.timeouts.load(), c.blocks.load() ? double(c.wait_ticks.load()) * us / double(c.blocks.load()) : 0.0,
      (unsigned long long)c.gap[0].load(), (unsigned long long)c.gap[1].load(), (unsigned long long)c.gap[2].load(),
      (unsigned long long)c.gap[3].load(), (unsigned long long)c.gap[4].load());
  Print(line, k < int(sizeof(line)) ? k : int(sizeof(line)) - 1);
}

struct PollState {
  const void *fence = nullptr;
  uint64_t value = 0, last = 0;
  uint32_t repeats = 0;
};
inline thread_local PollState t_poll;

// Called with the value GetCompletedValue is about to return. Returns the value to return. wait(seen, ms) blocks until the
// fence passes 'seen' or 'ms' elapse and reports whether it was signalled; read() re-reads the completed value.
template <typename Wait, typename Read>
inline uint64_t
Poll(const void *fence, uint64_t value, Wait &&wait, Read &&read) {
  auto &c = FenceStats();
  auto &t = t_poll;
  const unsigned mode = FenceWaitEvent();
  const unsigned stats = FenceWaitStats();
  const uint64_t now = Ticks();
  c.polls.fetch_add(1, std::memory_order_relaxed);
  if (t.fence != fence || t.value != value) {
    t.fence = fence; t.value = value; t.repeats = 0; t.last = now;
    return value;
  }
  const uint64_t gap = now - t.last;
  if (stats) {
    const uint64_t f = TickFrequency();
    const unsigned b = gap * 1000000u < 2 * f ? 0 : gap * 1000000u < 5 * f ? 1 : gap * 1000000u < 10 * f ? 2 : gap * 1000000u < 50 * f ? 3 : 4;
    c.gap[b].fetch_add(1, std::memory_order_relaxed);
  }
  c.repeats.fetch_add(1, std::memory_order_relaxed);
  t.last = now;
  if (gap > FenceWaitGapTicks()) { t.repeats = 0; return value; }
  if (++t.repeats < FenceWaitSpin()) return value;
  if (mode == 2) {   // observation only
    const uint64_t n = c.spin_polls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (stats && n % stats == 0) ReportFenceWait();
    return value;
  }
  // A tight spin on one value: block.
  const uint64_t w0 = Ticks();
  bool signalled;
  if (mode == 9) { Sleep(2); signalled = false; }
  else if (mode == 4) {   // sleep in quanta and re-read; at most 1 ms
    const uint64_t limit = TickFrequency() / 1000;
    signalled = false;
    while (Ticks() - w0 < limit) {
      SleepMicroseconds(FenceWaitQuantumUs());
      if (read() != value) { signalled = true; break; }
    }
  }
  else signalled = wait(value, 1);
  const uint64_t w1 = Ticks();
  const uint64_t blocks = c.blocks.fetch_add(1, std::memory_order_relaxed) + 1;
  (signalled ? c.woke : c.timeouts).fetch_add(1, std::memory_order_relaxed);
  c.wait_ticks.fetch_add(w1 - w0, std::memory_order_relaxed);
  const uint64_t now_value = read();
  t.last = w1;
  if (now_value != value) { t.value = now_value; t.repeats = 0; }
  if (stats && blocks % stats == 0) ReportFenceWait();
  return now_value;
}

} // namespace dxmt::cpu_stage
