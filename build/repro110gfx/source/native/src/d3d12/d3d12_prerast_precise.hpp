// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// GAPS 31.10.2026: MACRUNNER_DX12_PRERASTER_PRECISE=1 (default 0; only with RENDER_FENCE=3).
// In mode 3 every compute and blit encoder updates the pre-raster fence, and every "vertex-safe" render pass waits for it
// before its pre-raster stages. A vertex-safe pass reads in its pre-raster stages only BUFFERS (vertex / index / indirect
// buffers, CBVs, raw and structured SRVs: d3d12_render_fence.hpp, StagePreRasterReadsTyped), so an encoder that writes no
// buffer memory cannot have produced anything such a stage reads. With the key, a compute encoder whose pipelines declare only
// TEXTURE UAVs (DXIL PSV0 resource kind Texture*, DXBC dcl_uav_typed with a texture dimension) and a blit encoder whose
// commands write only textures do not update the pre-raster fence. Everything else is unchanged: the queue-wide fence is
// still waited and updated (the fragment stage of the next pass still waits for them), typed / tessellation / geometry
// passes still wait for the queue-wide fence before their pre-raster stages, and a pending aliasing join is kept until an
// encoder that does update both fences (the skip leaves alias_join set and TYPED_SAFE's 'exposed' state untouched).
// Unreadable bytecode, SM 6.6 descriptor-heap indexing, buffer UAVs (typed buffer, raw, structured, counters), unknown blit
// commands, counter resolves, OUT joins and every other encoder kind keep the update (conservative).
// FENCE_STATS (any period) reports pre_skip_c / pre_skip_b / pre_keep_c / pre_keep_b per frame.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Metal.hpp"
#include "d3d12_render_fence.hpp"
#include "d3d12_copy_gather.hpp"   // GAPS COPY_GATHER counters in the trace line

namespace dxmt::prerast_precise {

inline bool
Enabled() {
  static const bool on = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool v = GetEnvironmentVariableA("MACRUNNER_DX12_PRERASTER_PRECISE", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return v && render_fence::Mode() == 3;
  }();
  return on;
}

// Do these blit commands write buffer memory? true = yes or unknown.
template <typename Cmd>
inline bool
BlitWritesBuffers(const Cmd *head) {
  for (auto *c = reinterpret_cast<const wmtcmd_base *>(head); c; c = static_cast<const wmtcmd_base *>(c->next.ptr)) {
    switch ((enum WMTBlitCommandType)c->type) {
    case WMTBlitCommandNop:
    case WMTBlitCommandWaitForFence:
    case WMTBlitCommandUpdateFence:
    case WMTBlitCommandCopyFromBufferToTexture:
    case WMTBlitCommandCopyFromBufferToTextureWithBlitOption:
    case WMTBlitCommandCopyFromTextureToTexture:
    case WMTBlitCommandCopyTexture:
    case WMTBlitCommandGenerateMipmaps:
      break;
    default:   // buffer destinations (copies to buffers, fills, counter resolves, ICB resets) and anything unknown
      return true;
    }
  }
  return false;
}

struct Stats {
  std::atomic<uint64_t> skip_compute{}, skip_blit{}, keep_compute{}, keep_blit{};
};
inline Stats &
Get() {
  static Stats s;
  return s;
}
// MACRUNNER_DX12_PRERASTER_PRECISE_TRACE=1 (probes without a swapchain): cumulative counters after every ExecuteCommandLists.
inline bool
Trace() {
  static const bool on = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool v = GetEnvironmentVariableA("MACRUNNER_DX12_PRERASTER_PRECISE_TRACE", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return v;
  }();
  return on;
}
inline void
Note(bool skipped, bool compute) {
  if (!render_fence::StatsPeriod() && !Trace()) return;
  auto &s = Get();
  (compute ? (skipped ? s.skip_compute : s.keep_compute) : (skipped ? s.skip_blit : s.keep_blit)).fetch_add(1, std::memory_order_relaxed);
}

inline void
ExecuteTrace() {
  if (!Trace()) return;
  static std::atomic<uint64_t> calls{0};
  auto &s = Get();
  auto &g = copy_gather::Get();
  std::fprintf(stderr, "dx12_prerast_precise_trace execute=%llu enabled=%u skip_c=%llu skip_b=%llu keep_c=%llu keep_b=%llu"
               " gather=%u gather_lists=%llu gather_moved=%llu gather_conflict=%llu gather_stop=%llu gather_out=%llu\n",
               (unsigned long long)(calls.fetch_add(1, std::memory_order_relaxed) + 1), unsigned(Enabled()),
               (unsigned long long)s.skip_compute.load(), (unsigned long long)s.skip_blit.load(),
               (unsigned long long)s.keep_compute.load(), (unsigned long long)s.keep_blit.load(), unsigned(copy_gather::Enabled()),
               (unsigned long long)g.lists.load(), (unsigned long long)g.moved.load(), (unsigned long long)g.kept_conflict.load(),
               (unsigned long long)g.kept_stop.load(), (unsigned long long)g.kept_out.load());
}
// Once per present (next to render_fence::Presented): one stderr line per FENCE_STATS window, per-frame averages.
inline void
Presented() {
  const unsigned n = render_fence::StatsPeriod();
  if (!n || !(Enabled() || copy_gather::Enabled()) || Trace()) return;
  static std::atomic<uint64_t> presents{0};
  const uint64_t p = presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (p % n) return;
  auto &s = Get();
  const auto take = [&](std::atomic<uint64_t> &a) { return double(a.exchange(0, std::memory_order_relaxed)) / double(n); };
  auto &g = copy_gather::Get();
  std::fprintf(stderr, "dx12_prerast_precise presents=%llu window=%u precise=%u pre_skip_c=%.2f pre_skip_b=%.2f pre_keep_c=%.2f pre_keep_b=%.2f"
               " gather=%u gather_lists=%.2f gather_moved=%.2f gather_conflict=%.2f gather_stop=%.2f gather_out=%.2f\n",
               (unsigned long long)p, n, unsigned(Enabled()), take(s.skip_compute), take(s.skip_blit), take(s.keep_compute), take(s.keep_blit),
               unsigned(copy_gather::Enabled()), take(g.lists), take(g.moved), take(g.kept_conflict), take(g.kept_stop), take(g.kept_out));
}

} // namespace dxmt::prerast_precise
