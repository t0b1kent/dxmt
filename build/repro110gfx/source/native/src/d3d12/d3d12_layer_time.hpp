// SPDX-License-Identifier: LGPL-2.1-or-later
// CPU-LATE (23.10.2026): CPU wall time spent INSIDE the D3D12 layer, per thread, per method, per class. Observation only:
// nothing is reordered, waited for, hashed or allocated on the fast path. Default off (key unset or 0).
//
//   MACRUNNER_DX12_LAYER_TIME=P   every P presents a window report on stderr (a reporter thread prints it):
//     dx12_layer_time         window, frames, wall ms/frame, threads, counter frequency, QPC/CNTVCT pair (clock map)
//     dx12_layer_time_thread  per thread: tid, name, outermost layer calls/frame, layer ms/frame, per class ms/frame
//     dx12_layer_time_method  per thread and method: calls/frame and ms/frame (top methods of the thread)
//     dx12_layer_time_event   per thread and queue event (ExecuteCommandLists, the first one after a Present separately,
//                             queue Signal / Wait, Present, fence SetEventOnCompletion), split by the length of the interval
//                             since the previous event of the same thread: count, interval ms, of it in the layer (by class)
//                             and outside the layer (= code of the application + everything else), event duration
//   MACRUNNER_DX12_LAYER_TIME_DETAIL=W  at window W also the last kRing events of every thread that had events
//                             (dx12_layer_time_ev, with CNTVCT and QPC of entry, duration, interval, in-layer part).
//
// A scope stands at the entry of every COM method of the layer (generated: DXMT_LT(id) with the table in
// d3d12_layer_time_table.inc). Only the outermost layer call on a thread is timed; a nested layer call is part of its
// caller. Timer: CNTVCT_EL0 read directly (no syscall, no Win32 call, GetLastError untouched). Per-thread counters are
// written by the owning thread only (plain relaxed load/store), read by the reporter. Calls from the application into the
// layer that go through the translator's transition (x64 -> ARM64EC) are outside the scope: that cost is "outside".
#pragma once
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "d3d12_c17_stats.hpp"

namespace dxmt::layer_time {

#define DXMT_LT_CLASSES(X)                                                                                             \
  X(record) X(draw) X(barrier) X(clear) X(copy) X(close) X(reset) X(execute) X(signal) X(wait) X(fence_poll)          \
  X(fence_event) X(fence_signal) X(map) X(descriptor) X(create) X(resource_get) X(present) X(swap_other) X(residency)  \
  X(device_query) X(other)

enum Class : uint8_t {
#define DXMT_LT_ENUM(n) Cls_##n,
  DXMT_LT_CLASSES(DXMT_LT_ENUM)
#undef DXMT_LT_ENUM
  ClsCount
};
inline const char *ClassName(unsigned c) {
  static const char *const names[] = {
#define DXMT_LT_NAME(n) #n,
    DXMT_LT_CLASSES(DXMT_LT_NAME)
#undef DXMT_LT_NAME
  };
  return c < ClsCount ? names[c] : "?";
}

struct MethodInfo { const char *name; uint8_t cls; };
inline constexpr MethodInfo kTable[] = {
#include "d3d12_layer_time_table.inc"
};
constexpr unsigned kMethods = sizeof(kTable) / sizeof(kTable[0]);

// Queue events: the timeline of the threads that submit.
enum Event : uint8_t { EvExecute, EvExecuteFirst, EvSignal, EvWait, EvPresent, EvFenceEvent, EvCount, EvNone = 0xff };
inline const char *EventName(unsigned e) {
  static const char *const names[EvCount] = {"execute", "execute_first", "signal", "wait", "present", "fence_event"};
  return e < EvCount ? names[e] : "?";
}
constexpr uint8_t EventOf(unsigned cls) {
  return cls == Cls_execute ? EvExecute : cls == Cls_signal ? EvSignal : cls == Cls_wait ? EvWait :
         cls == Cls_present ? EvPresent : cls == Cls_fence_event ? EvFenceEvent : EvNone;
}
constexpr unsigned kBuckets = 6;   // interval since the previous event: <0.1, <0.5, <2, <5, <10, >=10 ms
inline const char *BucketName(unsigned b) {
  static const char *const names[kBuckets] = {"lt0.1ms", "0.1_0.5ms", "0.5_2ms", "2_5ms", "5_10ms", "ge10ms"};
  return b < kBuckets ? names[b] : "?";
}
constexpr unsigned kAggFields = 4 + ClsCount;   // count, interval, duration, in-layer, in-layer per class
constexpr unsigned kRing = 512;

inline const unsigned g_period = c17::ReadKey("MACRUNNER_DX12_LAYER_TIME", 100000);
// DESC_RACE (CPU-LATE 23.10, diagnostic): overlapping operations on one descriptor slot seen by the DESC_FAST path
// (d3d12_descriptor_heap.cpp). Printed by the reporter as dx12_desc_race (cumulative).
inline std::atomic<uint64_t> g_race_ops{0}, g_race_write_write{0}, g_race_write_read{0};
inline const unsigned g_detail = c17::ReadKey("MACRUNNER_DX12_LAYER_TIME_DETAIL", 100000);

inline uint64_t Ticks() { uint64_t v; asm volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
inline uint64_t Frequency() { uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

using Counter = std::atomic<uint64_t>;
inline void Bump(Counter &c, uint64_t d) { c.store(c.load(std::memory_order_relaxed) + d, std::memory_order_relaxed); }

struct Ev {
  uint64_t t0, t1, presents, inlayer;
  uint32_t id, aux, calls, kind;
  uint64_t cls[ClsCount];
};

struct Block {
  uint32_t tid = 0;
  uint32_t depth = 0;
  const char *volatile tag = nullptr;
  uint64_t start = 0;
  uint32_t cur = 0, aux = 0;
  // owner only
  uint64_t last_event_end = 0, last_presents = ~0ull, since_calls = 0;
  uint64_t since_cls[ClsCount] = {};
  // owner writes, reporter reads
  Counter ticks[kMethods] = {}, calls[kMethods] = {};
  Counter agg[EvCount][kBuckets][kAggFields] = {};
  Ev *ring = nullptr;
  std::atomic<uint64_t> head{0};
  // reporter only: values at the previous report
  uint64_t prev_ticks[kMethods] = {}, prev_calls[kMethods] = {};
  uint64_t prev_agg[EvCount][kBuckets][kAggFields] = {};
};

struct Global {
  std::mutex mutex;
  std::vector<Block *> blocks;
  std::atomic<uint64_t> presents{0};
  HANDLE wake = nullptr;
  bool started = false;
};
inline Global &G() { static Global *g = new Global(); return *g; }
inline thread_local Block *t_block = nullptr;

// d3d12_layer_time.cpp
Block *Register();
void Leave(Block *b);
void Tag(const char *tag);   // names a thread the layer itself created (submit, PSO workers, reporter)

struct Scope {
  Block *b = nullptr;
  explicit Scope(unsigned id) {
    if (__builtin_expect(g_period == 0, 1)) return;
    Block *x = t_block;
    if (!x && !(x = Register())) return;
    b = x;
    if (x->depth++ == 0) { x->cur = id; x->aux = 0; x->start = Ticks(); }
  }
  ~Scope() { if (b) Leave(b); }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};

// Extra value for the current outermost event (ExecuteCommandLists: queue type << 16 | list count).
inline void Aux(uint32_t v) {
  if (__builtin_expect(g_period == 0, 1)) return;
  if (Block *b = t_block; b && b->depth == 1) b->aux = v;
}

} // namespace dxmt::layer_time

#define DXMT_LT(id) ::dxmt::layer_time::Scope dxmt_layer_time_scope_(id)
