// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// GAPS 02.11.2026 (curator 06:30, after the LR3 census: Elden Ring GXWorker repeats IASetIndexBuffer 64 %, OMSetStencilRef 85 %,
// SetPipelineState 31 % of calls with the arguments of the previous call of the same method in the same list; 1.19 ms a frame).
//
// MACRUNNER_DX12_SETTER_DEDUP=1 (default 0): a setter whose arguments equal the state the list holds RIGHT NOW returns before
// RequireOpenRecording. The comparison is against the list's own state fields (not a shadow of the previous argument), so every
// path that changes that state - Reset / ClearState (ResetState), SetPipelineState of the other kind, an internal write - makes
// the comparison fail by construction. The skip is taken only when the original would have done nothing observable:
//   OMSetStencilRef    stencil_ref_ == value                       (the original returns early too)
//   IASetIndexBuffer   address, size, format equal, an allocation is held, and the VA registry did not change since that lookup
//                      (va_generation: every Register/UnregisterResidencyAndVA bumps it under the registry lock)
//   SetPipelineState   same interface pointer as the last successful call of this list and its resolved pipeline is still the
//                      bound one (graphics or compute) and no async failure is pending (the original: Resolve -> "unchanged")
//   IASetVertexBuffers every view equal to the slot's current view (the original copies the same bytes + sets VertexBuffer dirty;
//                      a pass begin sets every dirty bit, and inside a pass the emitted table already holds these views)
//   RSSetViewports / RSSetScissorRects  count and values equal (same reasoning: the dirty bit only re-emits identical values)
// Only while the recording is open and the recording trace is off (the trace would lose its "unchanged" lines).
// _MASK (default 63) selects methods: 1 IB, 2 stencil, 4 PSO, 8 VB, 16 viewports, 32 scissors.
// =9 is the NEGATIVE CONTROL: skip every call of a selected method after its first one in the list, WITHOUT comparing
// (pixels must differ wherever a list changes that state).
//
// MACRUNNER_DX12_VA_CACHE=1 (default 0): IASetIndexBuffer resolves the GPU address through a per-list cache of 8 registry intervals
// (base, logical length, base of the next interval, Rc of the allocation). A hit gives exactly what the registry's upper_bound
// would (the address lies in [base, next base)), and the cache is dropped as a whole when va_generation moved.
//
// MACRUNNER_DX12_OPEN_FAST=1 (default 0): RequireOpenRecording classifies the operation name at COMPILE time (the 56 call sites
// pass literals) instead of 3x strstr + 2x strcmp on every recorded call while c17 / xl_hazard is on (Elden Ring: MERGE 2 -> c17
// on, ~15 000 recorded calls a frame).
//
// MACRUNNER_DX12_SETTER_DEDUP_STATS=1: per-method calls / skips and cache hits, flushed at Close; a line `dx12_layer_time_dedup ...` at
// closes 1, 2, 4 .. 4096, then every 4096 (cumulative; the prefix makes Elden Ring's observer stamp it into ltime.jsonl).
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace dxmt::setter_dedup {

inline unsigned ReadKey(const char *name, unsigned fallback) {
  const DWORD saved = GetLastError();
  char value[16] = {};
  const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
  SetLastError(saved);
  if (!n || n >= sizeof(value)) return fallback;
  return unsigned(std::strtoul(value, nullptr, 0));
}
inline unsigned Mode() { static const unsigned v = [] { const unsigned m = ReadKey("MACRUNNER_DX12_SETTER_DEDUP", 0); return m == 1 || m == 9 ? m : 0u; }(); return v; }
inline unsigned Mask() { static const unsigned v = ReadKey("MACRUNNER_DX12_SETTER_DEDUP_MASK", 63) & 63; return v; }
inline bool VACache() { static const bool v = ReadKey("MACRUNNER_DX12_VA_CACHE", 0) == 1; return v; }
inline bool OpenFast() { static const bool v = ReadKey("MACRUNNER_DX12_OPEN_FAST", 0) == 1; return v; }
inline bool Stats() { static const bool v = ReadKey("MACRUNNER_DX12_SETTER_DEDUP_STATS", 0) == 1; return v; }

enum Method : unsigned { IB, SR, PSO, VB, VP, SC, Methods };
enum Cache : unsigned { VaHit, VaMiss, VaFlush, VaFill, CacheCounters };

struct Counters {
  std::atomic<uint64_t> calls[Methods]{}, skips[Methods]{}, cache[CacheCounters]{}, closes{0};
};
inline Counters &Data() { static Counters c; return c; }

struct Local {   // per command list, plain counters, flushed at Close
  uint64_t calls[Methods] = {}, skips[Methods] = {}, cache[CacheCounters] = {};
  void Flush() {
    auto &d = Data();
    for (unsigned i = 0; i < Methods; ++i) {
      if (calls[i]) d.calls[i].fetch_add(calls[i], std::memory_order_relaxed);
      if (skips[i]) d.skips[i].fetch_add(skips[i], std::memory_order_relaxed);
      calls[i] = skips[i] = 0;
    }
    for (unsigned i = 0; i < CacheCounters; ++i) {
      if (cache[i]) d.cache[i].fetch_add(cache[i], std::memory_order_relaxed);
      cache[i] = 0;
    }
    const uint64_t n = d.closes.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) != 0 && (n & 4095) != 0) return;   // closes 1, 2, 4 .. 4096, then every 4096
    char line[512];
    const auto c = [&](unsigned i) { return (unsigned long long)d.calls[i].load(std::memory_order_relaxed); };
    const auto s = [&](unsigned i) { return (unsigned long long)d.skips[i].load(std::memory_order_relaxed); };
    const auto k = [&](unsigned i) { return (unsigned long long)d.cache[i].load(std::memory_order_relaxed); };
    const int len = std::snprintf(line, sizeof(line),
        "dx12_layer_time_dedup closes=%llu mode=%u mask=%u va_cache=%u open_fast=%u ib=%llu/%llu sr=%llu/%llu pso=%llu/%llu vb=%llu/%llu "
        "vp=%llu/%llu sc=%llu/%llu va_hit=%llu va_miss=%llu va_flush=%llu va_fill=%llu\n",
        (unsigned long long)n, Mode(), Mask(), unsigned(VACache()), unsigned(OpenFast()), s(IB), c(IB), s(SR), c(SR), s(PSO), c(PSO),
        s(VB), c(VB), s(VP), c(VP), s(SC), c(SC), k(VaHit), k(VaMiss), k(VaFlush), k(VaFill));
    if (len > 0 && size_t(len) < sizeof(line)) {
      const DWORD saved = GetLastError();
      DWORD written;
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(len), &written, nullptr);
      SetLastError(saved);
    }
  }
};

// Compile-time form of RequireOpenRecording's name tests (std::strstr / std::strcmp on the operation literal).
constexpr bool CtContains(const char *s, const char *n) {
  if (!*n) return true;
  for (; *s; ++s) {
    const char *a = s, *b = n;
    while (*a && *b && *a == *b) { ++a; ++b; }
    if (!*b) return true;
  }
  return false;
}
constexpr bool CtEqual(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
// bit 0: the call bumps empty_binding_epoch; bit 1: it marks empty_indirect.
constexpr unsigned OpClass(const char *op) {
  return ((CtContains(op, "RootDescriptorTable") || CtContains(op, "RootShaderResourceView") ||
           CtContains(op, "RootUnorderedAccessView") || CtEqual(op, "SetDescriptorHeaps")) ? 1u : 0u) |
         (CtEqual(op, "ExecuteIndirect") ? 2u : 0u);
}
static_assert(OpClass("SetGraphicsRootDescriptorTable") == 1 && OpClass("SetComputeRootShaderResourceView") == 1 &&
              OpClass("SetGraphicsRootUnorderedAccessView") == 1 && OpClass("SetDescriptorHeaps") == 1 &&
              OpClass("ExecuteIndirect") == 2 && OpClass("IASetIndexBuffer") == 0 && OpClass("SetDescriptorHeapsX") == 0 &&
              OpClass("OMSetStencilRef") == 0, "OpClass");

} // namespace dxmt::setter_dedup

#define DXMT_OPCLASS(op) (std::integral_constant<unsigned, ::dxmt::setter_dedup::OpClass(op)>::value)
