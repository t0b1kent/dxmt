// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// VS-STAGE 23.10.2026: MACRUNNER_DX12_RENDER_FENCE, read once per process.
//   0 (default) a render pass waits for the queue-wide fence before its vertex stage (c17 behaviour)
//   1, 2        timing diagnostics only (wait before fragment / no wait): NOT ordering-safe
//   3           split: compute, blit and memory-writing render passes also update a second "pre-raster" fence. A render pass
//               whose pre-raster stages (VS/HS/DS/GS) read only buffers (vertex/index/indirect, CBV, raw or structured SRV)
//               waits for that fence before its pre-raster stages and for the queue-wide fence before its fragment stage,
//               so its vertex work may overlap the fragment work of the render passes before it. Every other render pass
//               (typed SRV or UAV in a pre-raster stage, tessellation, geometry) waits for the queue-wide fence before
//               all pre-raster stages. An aliasing barrier (or a barrier without readable records) joins both fences.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace dxmt::render_fence {
inline unsigned
Mode() {
  static const unsigned mode = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    unsigned parsed = 0;
    if (GetEnvironmentVariableA("MACRUNNER_DX12_RENDER_FENCE", value, sizeof(value)) == 1 && value[0] >= '0' && value[0] <= '3')
      parsed = unsigned(value[0] - '0');
    SetLastError(error);
    return parsed;
  }();
  return mode;
}

// MACRUNNER_DX12_FENCE_STATS=N (default 0, diagnostics; any RENDER_FENCE mode): every N presents one stderr line
// `dx12_fence_stats` with per-frame averages of render encoders by class (g = >=4 colour targets, s = depth-only >=2048 wide,
// o = other): encoders, merged passes, encoders whose pre-raster stages read only buffers (mode-3 'vertex-safe'), of those
// the ones encoded right after a render encoder (the overlap mode 3 can open) and the ones first in their encoding scope
// (a command list under EARLY_ENCODE: the encoder before them is in another command buffer, often a render one), the
// reasons for the full wait, and the
// recorded vertex work (vertex or index count x instances of ordinary draws). Counted at execution, no GPU readback.
inline unsigned
StatsPeriod() {
  static const unsigned n = [] {
    const DWORD error = GetLastError();
    char value[12] = {};
    unsigned parsed = 0;
    const DWORD len = GetEnvironmentVariableA("MACRUNNER_DX12_FENCE_STATS", value, sizeof(value));
    if (len && len < 8) for (DWORD i = 0; i < len && value[i] >= '0' && value[i] <= '9'; ++i) parsed = parsed * 10 + unsigned(value[i] - '0');
    SetLastError(error);
    return parsed;
  }();
  return n;
}
enum Class : unsigned { ClassG = 0, ClassS = 1, ClassO = 2, ClassCount = 3 };
struct Stats {
  std::atomic<uint64_t> encoders[ClassCount]{}, merged[ClassCount]{}, safe[ClassCount]{}, safe_after_render[ClassCount]{},
      after_render[ClassCount]{}, typed[ClassCount]{}, tess_geo[ClassCount]{}, alias[ClassCount]{}, work[ClassCount]{},
      work_safe_after_render[ClassCount]{}, safe_first[ClassCount]{}, work_safe_first[ClassCount]{}, joins{}, presents{},
      typed_free[ClassCount]{}, typed_exposed[ClassCount]{};   // GPU-PASS TYPED_SAFE (d3d12_typed_safe.hpp)
};
inline Stats &
Get() {
  static Stats s;
  return s;
}
inline void
Presented() {
  const unsigned n = StatsPeriod();
  if (!n) return;
  auto &s = Get();
  const uint64_t p = s.presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (p % n) return;
  const auto take = [&](std::atomic<uint64_t> &a) { return double(a.exchange(0, std::memory_order_relaxed)) / double(n); };
  const char *names = "gso";
  char line[2000];
  int at = std::snprintf(line, sizeof line, "dx12_fence_stats presents=%llu window=%u mode=%u joins=%.2f",
                                 (unsigned long long)p, n, Mode(), take(s.joins));
  for (unsigned c = 0; c < ClassCount && at > 0 && at < int(sizeof line); ++c)
    at += std::snprintf(line + at, sizeof line - at, " %c.enc=%.2f %c.merged=%.2f %c.safe=%.2f %c.safe_after_render=%.2f %c.after_render=%.2f"
                        " %c.typed=%.2f %c.tess_geo=%.2f %c.alias=%.2f %c.work=%.0f %c.work_safe_after_render=%.0f %c.safe_first=%.2f %c.work_safe_first=%.0f"
                        " %c.typed_free=%.2f %c.typed_exposed=%.2f",
                        names[c], take(s.encoders[c]), names[c], take(s.merged[c]), names[c], take(s.safe[c]), names[c],
                        take(s.safe_after_render[c]), names[c], take(s.after_render[c]), names[c], take(s.typed[c]), names[c],
                        take(s.tess_geo[c]), names[c], take(s.alias[c]), names[c], take(s.work[c]), names[c], take(s.work_safe_after_render[c]),
                        names[c], take(s.safe_first[c]), names[c], take(s.work_safe_first[c]), names[c], take(s.typed_free[c]),
                        names[c], take(s.typed_exposed[c]));
  std::fprintf(stderr, "%s\n", line);
}
} // namespace dxmt::render_fence
