/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pso_work.hpp"
#include "d3d12_fine_locks.hpp"
#include "d3d12_compute_commands.hpp"
#include "d3d12_frame_trace.hpp"
#include "d3d12_gpu_timing.hpp"
#include "d3d12_pipeline_timing.hpp"
#include "d3d12_shader_source_stats.hpp"
#include "d3d12_encoder_stats.hpp"
#include "d3d12_c17_stats.hpp"
#include "d3d12_attachment_audit.hpp"
#include <unordered_set>
#include "d3d12_command_capture.hpp"
#include "d3d12_submission_capture.hpp"
#include "d3d12_pageable.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <new>
#include <condition_variable>
#include <deque>
#include <functional>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

constexpr auto kCommandQueueSize = 32u;

// Committed, not yet completed command buffers the queue keeps before a commit blocks, and the limit of the Metal
// queue. MACRUNNER_DX12_INFLIGHT_DEPTH=33..1024 raises it; unset, 0 or anything else keeps 32.
static uint32_t
InflightDepth() {
  static const uint32_t depth = [] {
    const DWORD error = GetLastError();
    char value[8] = {};
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_INFLIGHT_DEPTH", value, sizeof(value));
    uint32_t parsed = 0;
    bool valid = n > 0 && n < sizeof(value);
    for (DWORD i = 0; valid && i < n; i++) {
      if (value[i] < '0' || value[i] > '9') valid = false;
      else parsed = parsed * 10 + uint32_t(value[i] - '0');
    }
    SetLastError(error);
    return valid && parsed > kCommandQueueSize && parsed <= 1024 ? parsed : kCommandQueueSize;
  }();
  return depth;
}

const GUID kD3D12CommandQueueDownlevelUUID = {
    0x38a8c5ef, 0x7ccb, 0x4e81, {0x91, 0x4f, 0xa6, 0xe9, 0xd0, 0x72, 0xc4, 0x94}
};

// --- MacRunner: render-pass coalescing for tile-based GPUs (diagnostic keys, default off) -------------------------
// Every Metal render pass boundary costs a store and a load of each attachment on a tile-based GPU, and with the
// queue-wide fence it is also a synchronisation point. D3D12 applications record one logical pass across several
// command lists and call ClearRenderTargetView / ResourceBarrier / OMSetRenderTargets freely, because on immediate
// GPUs these are free. The switches below keep one Metal encoder open across recorded passes with identical
// attachments and turn a full clear into the load action of the pass that follows it.
//   MACRUNNER_DX12_MERGE_PASSES=1  merge consecutive recorded render passes with identical attachments, but never
//                                  across a ResourceBarrier: a barrier may make an attachment readable as a shader
//                                  resource (read-only depth), and a tile-based GPU cannot sample an attachment of
//                                  the pass it is rendering
//   MACRUNNER_DX12_MERGE_PASSES=2  also merge across barriers (upper bound; NOT safe in general, diagnostics only)
//   MACRUNNER_DX12_FOLD_CLEARS=1   a full clear immediately followed by a pass using the view becomes LoadActionClear
//   MACRUNNER_DX12_PASS_STATS=1    counters only (also printed when either switch is on)
static bool
PassEnvironmentSwitch(const char *name) {
  const DWORD error = GetLastError();
  char value[4] = {};
  const bool active = GetEnvironmentVariableA(name, value, sizeof(value)) == 1 && value[0] == '1';
  SetLastError(error);
  return active;
}

static unsigned
MergePassesMode() {
  static const unsigned mode = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    unsigned parsed = 0;
    if (GetEnvironmentVariableA("MACRUNNER_DX12_MERGE_PASSES", value, sizeof(value)) == 1 && value[0] >= '0' && value[0] <= '2')
      parsed = unsigned(value[0] - '0');
    SetLastError(error);
    return parsed;
  }();
  return mode;
}

static bool
MergePassesEnabled() {
  return MergePassesMode() != 0;
}

static bool
FoldClearsEnabled() {
  static const bool enabled = PassEnvironmentSwitch("MACRUNNER_DX12_FOLD_CLEARS");
  return enabled;
}

// MACRUNNER_DX12_RENDER_FENCE (timing diagnostics ONLY, not ordering-safe in general: a vertex stage may read data
// written by an earlier blit, compute or draw pass): 0 default = a draw pass waits for the queue-wide fence before its
// vertex stage; 1 = it waits before its fragment stage only; 2 = it does not wait at all (it still updates the fence).
static unsigned
RenderFenceMode() {
  static const unsigned mode = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    unsigned parsed = 0;
    if (GetEnvironmentVariableA("MACRUNNER_DX12_RENDER_FENCE", value, sizeof(value)) == 1 && value[0] >= '0' && value[0] <= '2')
      parsed = unsigned(value[0] - '0');
    SetLastError(error);
    return parsed;
  }();
  return mode;
}

static bool
PassStatsEnabled() {
  static const bool enabled = PassEnvironmentSwitch("MACRUNNER_DX12_PASS_STATS") || MergePassesEnabled() ||
                              FoldClearsEnabled() || RenderFenceMode() != 0 || FineLocksEnabled() ||
                              InflightDepth() != kCommandQueueSize || fence_publication::Enabled();
  return enabled;
}

struct D3D12PassStats {
  std::atomic<uint64_t> executes{0}, lists{0};
  std::atomic<uint64_t> render_recorded{0}, render_encoders{0}, render_merged{0}, render_mergeable{0};
  std::atomic<uint64_t> render_mergeable_safe{0};   // same attachments AND no barrier since the previous pass
  std::atomic<uint64_t> clear_recorded{0}, clear_encoders{0}, clear_folded{0}, clear_foldable{0};
  std::atomic<uint64_t> blit_skipped{0}, other_encoders{0};
  // Disjoint partition of legacy other_encoders, plus explicitly excluded services.
  std::atomic<uint64_t> other_blit{0}, other_compute{0}, other_argument_compute{0}, other_acceleration{0}, other_resolve{0};
  std::atomic<uint64_t> blit_timestamp{0}, blit_counter_resolve{0}, blit_barrier_join{0}, compute_query_resolve{0};
  std::atomic<uint64_t> service_occlusion_blit{0}, service_occlusion_compute{0}, service_out_join_blit{0};
  std::atomic<uint64_t> service_readback_blit{0}, service_present_render{0}, presents_encoded{0};
  std::atomic<uint64_t> adjacent_plain_blit{0}, adjacent_plain_compute{0}, render_barrier_rejected{0};
  std::atomic<uint64_t> commits{0}, commit_stalls{0};   // commit_stalls: the in-flight ring was full at a commit
};
static D3D12PassStats g_pass_stats;

static void
PassStatsReport() {
  const uint64_t n = g_pass_stats.executes.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!PassStatsEnabled() || (n != 1 && n % 4096))
    return;
  const DWORD error = GetLastError();
  char line[4096];
  LARGE_INTEGER snapshot_qpc{};
  QueryPerformanceCounter(&snapshot_qpc);
  const auto get = [](const std::atomic<uint64_t> &v) { return (unsigned long long)v.load(std::memory_order_relaxed); };
  const int length = std::snprintf(line, sizeof(line),
      "dx12_pass_stats executes=%llu lists=%llu render_recorded=%llu render_encoders=%llu render_merged=%llu "
      "render_mergeable=%llu render_mergeable_safe=%llu clear_recorded=%llu clear_encoders=%llu clear_folded=%llu clear_foldable=%llu "
      "blit_skipped=%llu other_encoders=%llu commits=%llu commit_stalls=%llu merge=%u fold=%u fence=%u locks=%u "
      "inflight_depth=%u fence_publication=%u fence_cpu_signals=%llu fence_queue_signals=%llu "
      "fence_idle_signals=%llu fence_registrations=%llu fence_events_published=%llu "
      "fence_internal_wait_calls=%llu fence_internal_wait_pending=%llu fence_internal_wait_qpc=%llu fence_qpc_hz=%llu "
      "stats_version=2 snapshot_qpc=%llu presents_encoded=%llu other_blit=%llu other_compute=%llu "
      "other_argument_compute=%llu other_acceleration=%llu other_resolve=%llu blit_timestamp=%llu "
      "blit_counter_resolve=%llu blit_barrier_join=%llu compute_query_resolve=%llu "
      "service_occlusion_blit=%llu service_occlusion_compute=%llu service_out_join_blit=%llu "
      "service_readback_blit=%llu service_present_render=%llu adjacent_plain_blit=%llu adjacent_plain_compute=%llu "
      "render_barrier_rejected=%llu\n",
      (unsigned long long)n, get(g_pass_stats.lists), get(g_pass_stats.render_recorded),
      get(g_pass_stats.render_encoders), get(g_pass_stats.render_merged), get(g_pass_stats.render_mergeable),
      get(g_pass_stats.render_mergeable_safe), get(g_pass_stats.clear_recorded), get(g_pass_stats.clear_encoders), get(g_pass_stats.clear_folded),
      get(g_pass_stats.clear_foldable), get(g_pass_stats.blit_skipped), get(g_pass_stats.other_encoders),
      get(g_pass_stats.commits), get(g_pass_stats.commit_stalls),
      MergePassesMode(), unsigned(FoldClearsEnabled()), RenderFenceMode(), unsigned(FineLocksEnabled()),
      InflightDepth(), unsigned(fence_publication::Enabled()),
      get(fence_publication::Stats().cpu_signals), get(fence_publication::Stats().queue_signals),
      get(fence_publication::Stats().idle_signals), get(fence_publication::Stats().registrations),
      get(fence_publication::Stats().events_published), get(fence_publication::Stats().wait_calls),
      get(fence_publication::Stats().wait_pending), get(fence_publication::Stats().wait_ticks),
      (unsigned long long)fence_publication::Frequency(),
      (unsigned long long)snapshot_qpc.QuadPart, get(g_pass_stats.presents_encoded),
      get(g_pass_stats.other_blit), get(g_pass_stats.other_compute), get(g_pass_stats.other_argument_compute),
      get(g_pass_stats.other_acceleration), get(g_pass_stats.other_resolve), get(g_pass_stats.blit_timestamp),
      get(g_pass_stats.blit_counter_resolve), get(g_pass_stats.blit_barrier_join), get(g_pass_stats.compute_query_resolve),
      get(g_pass_stats.service_occlusion_blit), get(g_pass_stats.service_occlusion_compute),
      get(g_pass_stats.service_out_join_blit), get(g_pass_stats.service_readback_blit),
      get(g_pass_stats.service_present_render), get(g_pass_stats.adjacent_plain_blit),
      get(g_pass_stats.adjacent_plain_compute), get(g_pass_stats.render_barrier_rejected));
  DWORD written;
  if (length > 0 && size_t(length) < sizeof(line))
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
  SetLastError(error);
}

template <typename Attachment>
static bool
SameAttachment(Attachment &a, Attachment &b) {
  if (bool(a.attachment) != bool(b.attachment))
    return false;
  return !a.attachment || (a.attachment.texture().handle == b.attachment.texture().handle && a.level == b.level &&
                           a.slice == b.slice && a.depth_plane == b.depth_plane);
}

// True when the two recorded passes would produce the same Metal render pass descriptor.
static bool
SameRenderTargets(RenderEncoderData &a, RenderEncoderData &b) {
  if (a.render_target_width != b.render_target_width || a.render_target_height != b.render_target_height ||
      a.render_target_array_length != b.render_target_array_length ||
      a.default_raster_sample_count != b.default_raster_sample_count || a.dsv_planar_flags != b.dsv_planar_flags)
    return false;
  for (unsigned i = 0; i < std::size(a.colors); i++) {
    if (!SameAttachment(a.colors[i], b.colors[i]))
      return false;
    if (a.colors[i].attachment && (bool(a.colors[i].resolve_attachment) || bool(b.colors[i].resolve_attachment)))
      return false;
  }
  return SameAttachment(a.depth, b.depth) && SameAttachment(a.stencil, b.stencil);
}

// True when a full clear can become the load action of this pass: same view, and the pass covers the whole view.
static bool
CanFoldClear(ClearEncoderData &clear, RenderEncoderData &render) {
  if (clear.width != render.render_target_width || clear.height != render.render_target_height ||
      clear.array_length != render.render_target_array_length)
    return false;
  const auto handle = clear.attachment.texture().handle;
  if (clear.clear_dsv) {
    if ((clear.clear_dsv & 1) &&
        !(render.depth.attachment && render.depth.attachment.texture().handle == handle && !render.depth.depth_plane))
      return false;
    if ((clear.clear_dsv & 2) && !(render.stencil.attachment &&
                                   render.stencil.attachment.texture().handle == handle && !render.stencil.depth_plane))
      return false;
    return true;
  }
  for (auto &color : render.colors)
    if (color.attachment && color.attachment.texture().handle == handle && color.depth_plane == clear.depth_plane &&
        !color.level && !color.slice)
      return true;
  return false;
}

static void
FoldClear(ClearEncoderData &clear, RenderEncoderData &render, WMTRenderPassInfo &info) {
  const auto handle = clear.attachment.texture().handle;
  if (clear.clear_dsv) {
    if (clear.clear_dsv & 1) {
      info.depth.load_action = WMTLoadActionClear;
      info.depth.clear_depth = clear.depth_stencil.first;
    }
    if (clear.clear_dsv & 2) {
      info.stencil.load_action = WMTLoadActionClear;
      info.stencil.clear_stencil = clear.depth_stencil.second;
    }
    return;
  }
  for (unsigned i = 0; i < std::size(render.colors); i++) {
    auto &color = render.colors[i];
    if (color.attachment && color.attachment.texture().handle == handle && color.depth_plane == clear.depth_plane &&
        !color.level && !color.slice) {
      info.colors[i].load_action = WMTLoadActionClear;
      info.colors[i].clear_color = clear.color;
    }
  }
}
// --- ENCODERS (steps 4, 7 and the level-4 audit) -------------------------------------------------------------------
// Identity of an attachment / barrier object: the texture allocation (views of one texture share it) or the buffer.
static obj_handle_t
AttachmentObject(const TextureViewRef &view) {
  return view ? view->allocation->texture().handle : 0;
}
static bool
PassUsesObject(const RenderEncoderData &r, obj_handle_t object) {
  if (!object) return false;
  for (const auto &c : r.colors)
    if ((c.attachment && AttachmentObject(c.attachment) == object) ||
        (c.resolve_attachment && AttachmentObject(c.resolve_attachment) == object))
      return true;
  return (r.depth.attachment && AttachmentObject(r.depth.attachment) == object) ||
         (r.stencil.attachment && AttachmentObject(r.stencil.attachment) == object);
}
// Step 7: may two recorded passes with the same targets stay in one Metal encoder across this barrier? Only if every barrier
// is a transition of a known object that is not an attachment of the pass and leaves/enters no UAV / stream-out state
// (writes by the pass itself are possible only to attachments or UAVs; earlier encoders are complete by the queue fence).
static bool
BarrierTouchesPass(const BlitEncoderData &b, const RenderEncoderData &r) {
  if (!b.barrier_records_valid) return true;
  constexpr uint32_t writes = D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_STREAM_OUT;
  for (const auto &rec : b.barrier_records) {
    if (rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !rec.object) return true;
    if ((rec.before | rec.after) & writes) return true;
    if (PassUsesObject(r, rec.object)) return true;
  }
  return false;
}
// Step 4: does this barrier concern the texture of a pending clear (it may become readable)?
static bool
BarrierTouchesObject(const BlitEncoderData &b, obj_handle_t object) {
  if (!b.barrier_records_valid) return true;
  for (const auto &rec : b.barrier_records) {
    if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !rec.object) return true;
    if (rec.object == object) return true;
  }
  return false;
}
// c17 CLEAR_SUBRESOURCE: the subresources a view covers, from its key (mip_end == 0: unknown, i.e. the whole texture;
// the 4-bit fields cannot describe 16 mips, which also reads as unknown).
static bool
ViewRange(const TextureViewRef &view, uint32_t &mips, uint32_t &mip_begin, uint32_t &mip_end, uint32_t &slice_begin,
          uint32_t &slice_end) {
  if (!view) return false;
  const auto &key = view->key;
  mips = uint32_t(key.mip_count); mip_begin = uint32_t(key.mip_start); mip_end = uint32_t(key.mip_end);
  slice_begin = uint32_t(key.array_start); slice_end = uint32_t(key.array_end);
  return mips && mip_end > mip_begin && mip_end <= mips && slice_end > slice_begin;
}
// Does this barrier concern the subresources of a pending clear? Only a transition of the clear's texture whose subresource
// is another mip level than the clear's view covers is left out; everything else (all subresources, UAV or aliasing
// barriers, unknown view ranges) touches it, as in BarrierTouchesObject.
static bool
BarrierTouchesClear(const BlitEncoderData &b, const ClearEncoderData &clear) {
  const obj_handle_t object = AttachmentObject(clear.attachment);
  if (!b.barrier_records_valid) return true;
  uint32_t mips, mip_begin, mip_end, slice_begin, slice_end;
  const bool known = ViewRange(clear.attachment, mips, mip_begin, mip_end, slice_begin, slice_end);
  for (const auto &rec : b.barrier_records) {
    if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !rec.object) return true;
    if (rec.object != object) continue;
    if (rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || rec.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
        !known)
      return true;
    const uint32_t mip = rec.subresource % mips;   // subresource = mip + mips * (slice + slices * plane)
    if (mip >= mip_begin && mip < mip_end) return true;
  }
  return false;
}
// May two pending clears of the same texture be reordered? Yes when they cover different aspects (depth / stencil) or
// provably disjoint subresources (mip or array ranges of their views).
static bool
ClearsOverlap(const ClearEncoderData &a, const ClearEncoderData &b) {
  if (a.clear_dsv && b.clear_dsv && !(a.clear_dsv & b.clear_dsv)) return false;
  uint32_t am, ab, ae, as, aE, bm, bb, be, bs, bE;
  if (!ViewRange(a.attachment, am, ab, ae, as, aE) || !ViewRange(b.attachment, bm, bb, be, bs, bE) || am != bm) return true;
  if (ae <= bb || be <= ab) return false;
  if (aE <= bs || bE <= as) return false;
  return true;
}
// c17 MERGE_DEPENDENCIES: what a barrier between two passes with the same targets means for keeping one Metal encoder.
// Hazards inside one encoder come only from memory the chain itself writes: its attachments (any transition of an
// attachment object, of any subresource, ends the chain) and memory written by its draws (UAVs; stream-out is not
// supported by the layer). Everything recorded before the open encoder is complete for it (queue-wide fence), and every
// non-barrier pass in between ends the chain anyway. So a UAV-state transition, a UAV barrier or an aliasing barrier of
// other, named objects is crossed when no draw of the chain or of the next pass can write memory ("relaxed").
enum ChainBarrierReason : uint8_t {
  ChainBarrierNone = 0, ChainBarrierAttachment, ChainBarrierUnknownObject, ChainBarrierRecordsInvalid,
  ChainBarrierAliasingUnnamed
};
struct ChainBarrierVerdict {
  uint8_t reject = ChainBarrierNone;
  bool relaxed = false;                       // crossing needs "no memory writes in the chain and the next pass"
  bool plain = false, uav_state = false, uav_barrier = false, aliasing = false;
};
static ChainBarrierVerdict
ClassifyChainBarrier(const BlitEncoderData &b, const RenderEncoderData &r) {
  ChainBarrierVerdict v;
  if (!b.barrier_records_valid) { v.reject = ChainBarrierRecordsInvalid; return v; }
  constexpr uint32_t writes = D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_STREAM_OUT;
  for (const auto &rec : b.barrier_records) {
    switch (rec.type) {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
      if (!rec.object) { v.reject = ChainBarrierUnknownObject; return v; }
      if (PassUsesObject(r, rec.object)) { v.reject = ChainBarrierAttachment; return v; }
      if ((rec.before | rec.after) & writes) { v.relaxed = true; v.uav_state = true; }
      else v.plain = true;
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:   // a null resource orders every UAV access: none inside a relaxed chain
      if (rec.object && PassUsesObject(r, rec.object)) { v.reject = ChainBarrierAttachment; return v; }
      v.relaxed = true; v.uav_barrier = true;
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:   // a null side may be any placed resource, attachments included
      if (!rec.object || !rec.object2) { v.reject = ChainBarrierAliasingUnnamed; return v; }
      if (PassUsesObject(r, rec.object) || PassUsesObject(r, rec.object2)) { v.reject = ChainBarrierAttachment; return v; }
      v.relaxed = true; v.aliasing = true;
      break;
    default:
      v.reject = ChainBarrierUnknownObject;
      return v;
    }
  }
  return v;
}
// Level-4 audit: draws and state commands of a recorded render pass, and state commands that repeat the value already set
// in the same Metal encoder (what a diff-based recorder would not emit).
#include "empty-transition-merge.inc"

struct RenderStateAudit {
  struct Slot { uint8_t len = 0; uint8_t bytes[48]; };
  std::unordered_map<uint32_t, Slot> slots;
  bool Same(uint32_t key, const void *payload, size_t n) {
    n = std::min<size_t>(n, sizeof(Slot::bytes));
    auto [it, fresh] = slots.try_emplace(key);
    auto &slot = it->second;
    if (!fresh && slot.len == n && !std::memcmp(slot.bytes, payload, n)) return true;
    slot.len = uint8_t(n);
    std::memcpy(slot.bytes, payload, n);
    return false;
  }
};
static uint64_t
HashBytes(const void *p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) { h ^= static_cast<const uint8_t *>(p)[i]; h *= 1099511628211ull; }
  return h;
}
static void
AuditRenderCommands(const wmtcmd_base *cmd, RenderStateAudit &audit) {
  uint64_t draws = 0, state = 0, redundant = 0, other = 0;
  constexpr size_t header = sizeof(wmtcmd_base);
  for (; cmd; cmd = static_cast<const wmtcmd_base *>(cmd->next.ptr)) {
    const auto type = WMTRenderCommandType(cmd->type);
    const auto *raw = reinterpret_cast<const uint8_t *>(cmd);
    auto plain = [&](size_t size, uint32_t index) {
      ++state;
      if (audit.Same((uint32_t(type) << 8) | index, raw + header, size - header)) ++redundant;
    };
    auto hashed = [&](const void *p, size_t n, uint32_t index) {
      ++state;
      const uint64_t h = HashBytes(p, n);
      if (audit.Same((uint32_t(type) << 8) | index, &h, sizeof(h))) ++redundant;
    };
    switch (type) {
    case WMTRenderCommandDraw: case WMTRenderCommandDrawIndexed: case WMTRenderCommandDrawIndirect:
    case WMTRenderCommandDrawIndexedIndirect: case WMTRenderCommandDrawMeshThreadgroups:
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: case WMTRenderCommandDXMTGeometryDraw:
    case WMTRenderCommandDXMTGeometryDrawIndexed: case WMTRenderCommandDXMTGeometryDrawIndirect:
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: case WMTRenderCommandDXMTTessellationMeshDraw:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: case WMTRenderCommandExecuteCommandsInBuffer:
      ++draws;
      break;
    case WMTRenderCommandSetVertexBuffer: case WMTRenderCommandSetFragmentBuffer: case WMTRenderCommandSetMeshBuffer:
    case WMTRenderCommandSetObjectBuffer:
      plain(sizeof(wmtcmd_render_setbuffer), reinterpret_cast<const wmtcmd_render_setbuffer *>(cmd)->index);
      break;
    case WMTRenderCommandSetVertexBufferOffset: case WMTRenderCommandSetFragmentBufferOffset:
    case WMTRenderCommandSetMeshBufferOffset: case WMTRenderCommandSetObjectBufferOffset:
      plain(sizeof(wmtcmd_render_setbufferoffset), reinterpret_cast<const wmtcmd_render_setbufferoffset *>(cmd)->index);
      break;
    case WMTRenderCommandSetFragmentBytes: {
      auto c = reinterpret_cast<const wmtcmd_render_setbytes *>(cmd);
      hashed(c->bytes.ptr, size_t(c->length), c->index);
      break;
    }
    case WMTRenderCommandSetFragmentTexture:
      plain(sizeof(wmtcmd_render_settexture), reinterpret_cast<const wmtcmd_render_settexture *>(cmd)->index);
      break;
    case WMTRenderCommandSetRasterizerState: plain(sizeof(wmtcmd_render_setrasterizerstate), 0); break;
    case WMTRenderCommandSetPSO: plain(sizeof(wmtcmd_render_setpso), 0); break;
    case WMTRenderCommandSetDSSO: plain(sizeof(wmtcmd_render_setdsso), 0); break;
    case WMTRenderCommandSetDepthStencilState: plain(sizeof(wmtcmd_render_setdepthstencilstate), 0); break;
    case WMTRenderCommandSetBlendFactorAndStencilRef: case WMTRenderCommandSetBlendFactor:
      plain(sizeof(wmtcmd_render_setblendcolor), 0);
      break;
    case WMTRenderCommandSetStencilRef: plain(sizeof(wmtcmd_render_setstencilref), 0); break;
    case WMTRenderCommandSetViewport: plain(sizeof(wmtcmd_render_setviewport), 0); break;
    case WMTRenderCommandSetScissorRect: plain(sizeof(wmtcmd_render_setscissorrect), 0); break;
    case WMTRenderCommandSetViewports: {
      auto c = reinterpret_cast<const wmtcmd_render_setviewports *>(cmd);
      hashed(c->viewports.ptr, size_t(c->viewport_count) * sizeof(WMTViewport), 0);
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      auto c = reinterpret_cast<const wmtcmd_render_setscissorrects *>(cmd);
      hashed(c->scissor_rects.ptr, size_t(c->rect_count) * sizeof(WMTScissorRect), 0);
      break;
    }
    default:
      ++other;
      break;
    }
  }
  auto &st = encoder_stats::Get();
  st.draws.fetch_add(draws, std::memory_order_relaxed);
  st.state_cmds.fetch_add(state, std::memory_order_relaxed);
  st.state_redundant.fetch_add(redundant, std::memory_order_relaxed);
  st.other_render_cmds.fetch_add(other, std::memory_order_relaxed);
}
// --- end of MacRunner render-pass coalescing helpers ---------------------------------------------------------------

static bool
FrameReadbackEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_READBACK", value, sizeof(value)) == 1 &&
                        value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

static bool
FullFrameCaptureEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_CAPTURE", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

// Explicit diagnostics only. This clock includes work from all queues between observed Present calls.
static std::atomic<uint64_t> capture_next_present{1};
static std::atomic<uint32_t> capture_lines{0};

static bool
FaultCaptureEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FAULT_CAPTURE", value, sizeof(value)) == 1 &&
                        value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

// First observed GPU failure across queues, independent of sampled frame trace windows.
static std::atomic<bool> fault_capture_dumped{false};

struct D3D12FrameReadback {
  static constexpr uint8_t sentinel = 0xa5;
  uint64_t present_index = 0, width = 0, height = 0;
  uint64_t render_index = 0;
  WMTPixelFormat format{};
  uint32_t pitch = 0;
  bool bgra = false, copied = false;
  const uint8_t *data = nullptr;
  WMT::Reference<WMT::Buffer> buffer;
  WMT::Reference<WMT::Texture> source_allocation;
  WMT::Reference<WMT::Texture> source_texture;
  WMT::Reference<WMT::ResidencySet> residency;

  void Log(const void *queue, uint64_t seq, const char *stage, const char *detail) const {
    const DWORD error = GetLastError();
    char line[640];
    const int length = std::snprintf(
        line, sizeof(line),
        "dx12_readback source=%s stage=%s queue=%p present=%llu render=%llu seq=%llu texture=%llx format=%u width=%llu height=%llu %s\n",
        render_index ? "render_target" : "actual_backbuffer", stage, queue,
        static_cast<unsigned long long>(present_index), static_cast<unsigned long long>(render_index),
        static_cast<unsigned long long>(seq), static_cast<unsigned long long>(source_texture.handle),
        unsigned(format), static_cast<unsigned long long>(width), static_cast<unsigned long long>(height), detail);
    DWORD written;
    if (length > 0 && size_t(length) < sizeof(line))
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
    SetLastError(error);
  }

  void Complete(const void *queue, uint64_t seq, WMTCommandBufferStatus status) {
    if (!present_index && !render_index)
      return;
    char detail[320];
    std::snprintf(detail, sizeof(detail), "status=%u copied=%u", unsigned(status), unsigned(copied));
    // This stream is indexed by presents, not by all command buffer completions.
    Log(queue, seq, render_index ? "render.complete" : "present.complete", detail);
    if (copied && status == WMTCommandBufferStatusCompleted) {
      uint64_t nonzero = 0, hash = 14695981039346656037ull;
      bool unchanged_sentinel = true;
      unsigned rgb_min[3] = {255, 255, 255}, rgb_max[3] = {};
      for (uint64_t y = 0; y < height; ++y) {
        const auto *row = data + y * pitch;
        for (uint64_t x = 0; x < width; ++x) {
          const auto *pixel = row + x * 4;
          unchanged_sentinel &= pixel[0] == sentinel && pixel[1] == sentinel && pixel[2] == sentinel;
          nonzero += (pixel[0] | pixel[1] | pixel[2]) != 0;
          for (unsigned c = 0; c < 3; ++c) {
            const unsigned value = pixel[bgra ? 2 - c : c];
            rgb_min[c] = std::min(rgb_min[c], value);
            rgb_max[c] = std::max(rgb_max[c], value);
            hash = (hash ^ value) * 1099511628211ull;
          }
        }
      }
      // Hash canonical RGB bytes only: alpha and row padding never affect the black test or hash.
      if (unchanged_sentinel) {
        // A real image could match the sentinel too; neither case establishes a valid capture.
        Log(queue, seq, "backbuffer.ambiguous", "reason=unchanged_sentinel_rgb sentinel_byte=0xa5 black=unknown");
      } else {
        const bool constant = rgb_min[0] == rgb_max[0] && rgb_min[1] == rgb_max[1] && rgb_min[2] == rgb_max[2];
        // Nonzero RGB (including a constant clear) is not evidence of a rendered menu.
        std::snprintf(detail, sizeof(detail),
                    "status=%u rgb_nonzero_pixels=%llu rgb_min=%u,%u,%u rgb_max=%u,%u,%u "
                    "rgb_fnv1a64=%016llx black=%u rgb_constant=%u",
                    unsigned(status), static_cast<unsigned long long>(nonzero),
                    rgb_min[0], rgb_min[1], rgb_min[2], rgb_max[0], rgb_max[1], rgb_max[2],
                    static_cast<unsigned long long>(hash), unsigned(nonzero == 0), unsigned(constant));
        Log(queue, seq, "backbuffer.rgb", detail);
      }
    }
    if (residency) {
      residency.removeAllAllocations();
      residency.commit();
    }
  }
};

// The retirement worker must not own or access any public COM object.
struct D3D12CommandQueueState {
  WMT::Reference<WMT::Device> device_;
  WMT::Reference<WMT::ResidencySet> residency_set_;
  WMT::Reference<WMT::CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;
  WMT::Reference<WMT::Fence> immediate_out_fence_;
  bool immediate_out_pending_ = false;
  WMT::Reference<WMT::Buffer> visibility_scratch_;
  WMT::Reference<WMT::SharedEvent> timestamp_event_;
  uint64_t timestamp_serial_ = 0;
  WMT::Reference<WMT::MappingCommandQueue> mapping_queue_;
  WMT::Reference<WMT::SharedEvent> mapping_event_;
  uint64_t mapping_serial_ = 0;
  uint64_t readback_present_index_ = 0; // Accessed only under mutex_commit_; saturates after the last sample.
  uint64_t readback_render_index_ = 0;
  unsigned transfer_trace_lines_ = 0;
  // A native enqueue failure may leave work in another queue. Quarantine rather
  // than releasing that work's allocations based on an unrelated CB completion.
  std::shared_ptr<D3D12CommandQueueState> failed_mapping_keepalive_;
  std::vector<std::shared_ptr<D3D12SparseBufferState>> failed_mapping_resources_;

  std::atomic_uint64_t inflight_cmdbuf_seq_ = 1;
  std::atomic_uint64_t inflight_cmdbuf_count_ = 0;
  // ENCODERS 4.1: command buffers encoded at Close() and not committed yet (bounded: the native queue is created with
  // room for them, so commandBuffer() never blocks a recording thread), and early encoders running right now.
  std::atomic<uint32_t> early_outstanding{0};
  std::atomic<uint32_t> early_users{0};
  std::atomic<uint64_t> early_last_execute{0};   // when this queue last executed lists (several queues of one type)
  std::atomic_uint64_t inflight_cmdbuf_stop_ = 0;

  struct InflightCommandBuffer {
    WMT::Reference<WMT::CommandBuffer> cmdbuf{};
    GpuTimingRecord gpu_timing;
    uint64_t pipeline_ticket = 0;
    std::shared_ptr<D3D12PresentState> present;
    WMT::Reference<WMT::Texture> present_texture;
    std::shared_ptr<D3D12RemovableFence> retained_fence;
    bool publishes_fence = false;
    UINT64 fence_value = 0;
    std::unique_ptr<D3D12FencePublicationWait> fence_wait;
    std::vector<std::shared_ptr<D3D12SparseBufferState>> sparse_resources;
    std::vector<WMT::Reference<WMT::Buffer>> timestamp_scratch;
    D3D12FrameReadback readback;
    std::vector<D3D12FrameReadback> render_readbacks;
    std::vector<D3D12NativeComputeDispatch> argument_dispatches;
    std::vector<D3D12NativeAccelerationBuild> acceleration_builds;
    D3D12SubmissionCapture *fault_capture = nullptr;
  };

  const uint32_t inflight_depth_ = InflightDepth();
  std::vector<InflightCommandBuffer> inflight_cmdbuf_pool_ = std::vector<InflightCommandBuffer>(inflight_depth_);
  std::vector<std::unique_ptr<D3D12SubmissionCapture>> fault_capture_pool_ =
      std::vector<std::unique_ptr<D3D12SubmissionCapture>>(inflight_depth_);
  dxmt::mutex mutex_commit_;

  void
  CommandBufferWaitingThread() {
    env::setThreadName("dxmt-cmdbuf-waiting-thread");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    uint64_t internal_seq = 1;
    for (;;) {
      inflight_cmdbuf_seq_.wait(internal_seq, std::memory_order_acquire);
      if (inflight_cmdbuf_stop_.load() == internal_seq)
        break;
      auto pool = WMT::MakeAutoreleasePool();
      auto &inflight = inflight_cmdbuf_pool_[internal_seq % inflight_depth_];

      const uint64_t wait_begin = inflight.gpu_timing.ticket ? GpuTimingTrace::Clock() : 0;
      const auto pipeline_wait = inflight.pipeline_ticket ? pipeline_timing::Get().Emit(
          pipeline_timing::Retire, pipeline_timing::Enter, this, internal_seq,
          inflight.cmdbuf.handle, pipeline_timing::Pointer(inflight.present.get()), 0, 0, 0, true) : 0;
      if (inflight.cmdbuf.status() <= WMTCommandBufferStatusScheduled)
        inflight.cmdbuf.waitUntilCompleted();
      const auto status = inflight.cmdbuf.status();
      if (pipeline_wait) {
        pipeline_timing::PreserveError error;
        pipeline_timing::Get().Emit(pipeline_timing::Retire, pipeline_timing::Leave, this,
            pipeline_wait, internal_seq, 0, 0, 0, uint32_t(status), true);
        const auto gpu_begin = MTLCommandBuffer_property(inflight.cmdbuf.handle, WMTCommandBufferPropertyGPUStartTime);
        const auto gpu_end = inflight.cmdbuf.gpuEndTime();
        pipeline_timing::Get().Emit(pipeline_timing::GpuTimes, pipeline_timing::Point, this,
            internal_seq, gpu_begin, gpu_end, inflight.cmdbuf.handle,
            pipeline_timing::Pointer(inflight.present.get()), uint32_t(status), true);
      }
      GpuTiming().Heartbeat(bool(inflight.present), unsigned(status));
      if (inflight.gpu_timing.ticket) {
        const DWORD timing_error = GetLastError();
        const uint64_t wait_end = GpuTimingTrace::Clock();
        const uint64_t gpu_begin = MTLCommandBuffer_property(inflight.cmdbuf.handle, WMTCommandBufferPropertyGPUStartTime);
        const uint64_t gpu_end = inflight.cmdbuf.gpuEndTime();
        GpuTiming().Complete(inflight.gpu_timing, this, internal_seq, bool(inflight.present), unsigned(status),
                             wait_begin, wait_end, gpu_begin, gpu_end);
        SetLastError(timing_error);
      }
      static std::atomic<uint32_t> trace_complete{0}, trace_error{0};
      TraceFrame(trace_complete, "cmdbuf.complete", this, "seq=%llu status=%u present=%u",
                 static_cast<unsigned long long>(internal_seq), unsigned(status), unsigned(bool(inflight.present)));
      if (status == WMTCommandBufferStatusError) {
        TraceFrame(trace_error, "cmdbuf.error", this, "seq=%llu status=%u present=%u",
                   static_cast<unsigned long long>(internal_seq), unsigned(status), unsigned(bool(inflight.present)));
        ERR("Device error: ", inflight.cmdbuf.error().description().getUTF8String());
        if (FaultCaptureEnabled() && !fault_capture_dumped.exchange(true, std::memory_order_relaxed)) {
          const DWORD error = GetLastError();
          auto emit = [&](const char *detail) {
            char line[2304];
            const int n = std::snprintf(line, sizeof(line), "dx12_fault queue=%p seq=%llu status=%u %s\n",
                this, (unsigned long long)internal_seq, unsigned(status), detail);
            DWORD written;
            if (n > 0 && std::size_t(n) < sizeof(line))
              WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
          };
          if (inflight.fault_capture) inflight.fault_capture->Dump(emit);
          else emit("event=unavailable reason=capture_allocation_failed");
          SetLastError(error);
        }
      }

      // A later queue completion cannot overtake Win32 event publication for
      // an earlier Signal. Cross-queue Wait also retires the CPU publication,
      // using a one-shot registration that survives subsequent fence rewinds.
      if (inflight.publishes_fence || inflight.fence_wait) {
        if (status == WMTCommandBufferStatusError)
          inflight.retained_fence->Remove();
        else if (status == WMTCommandBufferStatusCompleted) {
          if (inflight.fence_wait)
            fence_publication::Wait(inflight.fence_wait->event);
          if (inflight.publishes_fence)
            inflight.retained_fence->CompleteQueueSignal(inflight.fence_value);
        }
      }
      inflight.readback.Complete(this, internal_seq, status);
      for (auto &readback : inflight.render_readbacks)
        readback.Complete(this, internal_seq, status);

      if (inflight.present && inflight.present->semaphore) {
        const auto ticket = inflight.pipeline_ticket ? pipeline_timing::Get().Emit(
            pipeline_timing::SemaphoreRelease, pipeline_timing::Enter, this, internal_seq,
            pipeline_timing::Pointer(inflight.present->semaphore),
            pipeline_timing::Pointer(inflight.present.get()), 0, 0, 0, true) : 0;
        const BOOL released = ReleaseSemaphore(inflight.present->semaphore, 1, nullptr);
        if (ticket) pipeline_timing::Get().Emit(pipeline_timing::SemaphoreRelease, pipeline_timing::Leave,
            this, ticket, internal_seq, 0, 0, 0, released ? 0 : GetLastError(), true);
      }

      inflight = {};

      inflight_cmdbuf_count_.fetch_sub(1, std::memory_order_release);
      inflight_cmdbuf_count_.notify_one();

      internal_seq++;
    }
  };
};

struct D3D12QueueRetirement {
  std::shared_ptr<D3D12CommandQueueState> state;
  HMODULE module;
};

static DWORD WINAPI
RetireD3D12Queue(void *context) {
  HMODULE module;
  {
    std::unique_ptr<D3D12QueueRetirement> retirement(static_cast<D3D12QueueRetirement *>(context));
    module = retirement->module;
    retirement->state->CommandBufferWaitingThread();
  }
  // No C++ objects or return address may remain in the module being released.
  FreeLibraryAndExitThread(module, 0);
}

// CPU FIFO, independent of the bounded Metal ring. Only this thread may wait for ring capacity.
// Tasks own private references until encoding is finished; retirement still owns native state only.
struct D3D12SubmissionQueue {
  dxmt::mutex producer_mutex;
  dxmt::mutex mutex;
  std::condition_variable_any ready;
  std::deque<std::function<void()>> pending;
  bool stop = false;
  uint64_t submitted = 0, completed = 0, peak = 0;
};

struct D3D12SubmissionWorker {
  std::shared_ptr<D3D12SubmissionQueue> fifo;
  HMODULE module;
};

static DWORD WINAPI
SubmitD3D12Queue(void *context) {
  HMODULE module;
  {
    std::unique_ptr<D3D12SubmissionWorker> worker(static_cast<D3D12SubmissionWorker *>(context));
    module = worker->module;
    auto fifo = worker->fifo;
    env::setThreadName("dxmt-cpu-submit-thread");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
      std::function<void()> work;
      {
        std::unique_lock lock(fifo->mutex);
        fifo->ready.wait(lock, [&] { return fifo->stop || !fifo->pending.empty(); });
        if (fifo->pending.empty()) break;
        work = std::move(fifo->pending.front());
        fifo->pending.pop_front();
      }
      work();
      // Releasing the last private queue reference may destroy the COM wrapper here.
      work = {};
      {
        std::lock_guard lock(fifo->mutex);
        ++fifo->completed;
      }
    }
    if (PassEnvironmentSwitch("MACRUNNER_DX12_ASYNC_SUBMIT_STATS")) {
      char line[256];
      const int n = std::snprintf(line, sizeof(line),
          "dx12_async_submit submitted=%llu completed=%llu peak_pending=%llu pending=%llu\n",
          (unsigned long long)fifo->submitted, (unsigned long long)fifo->completed,
          (unsigned long long)fifo->peak, (unsigned long long)fifo->pending.size());
      DWORD written;
      if (n > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
    }
  }
  FreeLibraryAndExitThread(module, 0);
}

// ENCODERS 4.1: queues by device and list type, so Close() can find the one queue a list will most likely run on.
class MTLD3D12CommandQueueImpl;
struct EarlyQueueEntry {
  MTLD3D12Device *device;
  D3D12_COMMAND_LIST_TYPE type;
  MTLD3D12CommandQueueImpl *queue;
  std::shared_ptr<D3D12CommandQueueState> state;
};
static dxmt::mutex &EarlyRegistryLock() { static dxmt::mutex lock; return lock; }
static std::vector<EarlyQueueEntry> &EarlyRegistry() { static std::vector<EarlyQueueEntry> entries; return entries; }
static std::atomic<uint64_t> g_next_queue_id{1};
static std::atomic<uint64_t> g_execute_clock{1};
static constexpr uint32_t kEarlyMax = 256;   // command buffers encoded at Close() and not yet committed, per queue

class MTLD3D12CommandQueueImpl : public MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory> {
  D3D12_COMMAND_QUEUE_DESC desc_;
  std::shared_ptr<D3D12CommandQueueState> state_ = std::make_shared<D3D12CommandQueueState>();
  HANDLE inflight_cmdbuf_wait_thread_ = nullptr;
  std::shared_ptr<D3D12SubmissionQueue> submission_;
  HANDLE submission_thread_ = nullptr;
  DWORD submission_thread_id_ = 0;

  bool DeferSubmission() const {
    return submission_ && GetCurrentThreadId() != submission_thread_id_;
  }

  template <typename F> HRESULT SubmitCPU(F &&operation) {
    try {
      // A private ref preserves lifetime without changing the application's COM reference count.
      Com<MTLD3D12CommandQueueImpl, false> keep(this);
      std::function<void()> work = [keep = std::move(keep), op = std::forward<F>(operation)]() mutable {
        try { op(); }
        catch (...) { keep->device_->RemoveDevice(); }
      };
      {
        std::lock_guard lock(submission_->mutex);
        submission_->pending.push_back(std::move(work));
        ++submission_->submitted;
        submission_->peak = std::max<uint64_t>(submission_->peak, submission_->pending.size());
      }
      submission_->ready.notify_one();
      return S_OK;
    } catch (...) {
      device_->RemoveDevice();
      return E_OUTOFMEMORY;
    }
  }

  struct CommittingScope {
    D3D12CommandQueueState *queue;
    std::unique_lock<dxmt::mutex> lock;
    uint64_t seq;
    D3D12CommandQueueState::InflightCommandBuffer own;   // ENCODERS 4.1: packet of an early (Close-time) encoding
    D3D12CommandQueueState::InflightCommandBuffer &inflight;
    WMT::Reference<WMT::Object> pool;
    bool discard = false;
    bool early = false;

    // ENCODERS 4.1: encode at Close() on the recording thread; no ring slot, no commit lock, never commits itself.
    struct Early {};
    CommittingScope(D3D12CommandQueueState *queue, Early) :
        queue(queue), lock(), seq(0), inflight(own), pool(WMT::MakeAutoreleasePool()), early(true) {
      inflight.cmdbuf = queue->queue_.commandBuffer();
    }
    // ENCODERS 4.1: commit a command buffer encoded at Close(), in submission order, through the normal ring.
    CommittingScope(D3D12CommandQueueState *queue, WMT::Reference<WMT::CommandBuffer> &&existing) :
        queue(queue),
        lock(queue->mutex_commit_),
        seq(queue->inflight_cmdbuf_seq_.load(std::memory_order_relaxed)),
        inflight(queue->inflight_cmdbuf_pool_[seq % queue->inflight_depth_]),
        pool(WMT::MakeAutoreleasePool()) {
      {
        pipeline_timing::Scope capacity(pipeline_timing::QueueCapacity, queue, seq, queue->inflight_depth_);
        g_pass_stats.commits.fetch_add(1, std::memory_order_relaxed);
        if (queue->inflight_cmdbuf_count_.load(std::memory_order_relaxed) >= queue->inflight_depth_)
          g_pass_stats.commit_stalls.fetch_add(1, std::memory_order_relaxed);
        queue->inflight_cmdbuf_count_.wait(queue->inflight_depth_, std::memory_order_acquire);
      }
      inflight.cmdbuf = std::move(existing);
    }

    CommittingScope(D3D12CommandQueueState *queue) :
        queue(queue),
        lock(queue->mutex_commit_),
        seq(queue->inflight_cmdbuf_seq_.load(std::memory_order_relaxed)),
        inflight(queue->inflight_cmdbuf_pool_[seq % queue->inflight_depth_]),
        pool(WMT::MakeAutoreleasePool()) {
      // Serialize the capacity check with all other producers of this ring.
      {
        pipeline_timing::Scope capacity(pipeline_timing::QueueCapacity, queue, seq, queue->inflight_depth_);
        g_pass_stats.commits.fetch_add(1, std::memory_order_relaxed);
        if (queue->inflight_cmdbuf_count_.load(std::memory_order_relaxed) >= queue->inflight_depth_)
          g_pass_stats.commit_stalls.fetch_add(1, std::memory_order_relaxed);
        queue->inflight_cmdbuf_count_.wait(queue->inflight_depth_, std::memory_order_acquire);
      }
      inflight.cmdbuf = queue->queue_.commandBuffer();
      if (FaultCaptureEnabled() && !fault_capture_dumped.load(std::memory_order_relaxed)) {
        const DWORD error = GetLastError();
        auto &capture = queue->fault_capture_pool_[seq % queue->inflight_depth_];
        if (!capture) capture.reset(new (std::nothrow) D3D12SubmissionCapture);
        if (capture) {
          capture->Reset();
          inflight.fault_capture = capture.get();
        }
        SetLastError(error);
      }
    };

    ~CommittingScope() {
      if (discard || early) {
        inflight = {};
        return;
      }
      static std::atomic<uint32_t> trace_enter{0}, trace_return{0};
      TraceFrame(trace_enter, "cmdbuf.commit.enter", queue, "seq=%llu present=%u",
                 static_cast<unsigned long long>(seq), unsigned(bool(inflight.present)));
      inflight.gpu_timing = GpuTiming().Begin();
      inflight.pipeline_ticket = pipeline_timing::Get().Emit(pipeline_timing::Commit,
          pipeline_timing::Enter, queue, seq, inflight.cmdbuf.handle,
          pipeline_timing::Pointer(inflight.present.get()));
      inflight.cmdbuf.commit();
      if (inflight.pipeline_ticket) pipeline_timing::Get().Emit(pipeline_timing::Commit,
          pipeline_timing::Leave, queue, inflight.pipeline_ticket, seq, 0, 0, 0, 0, true);
      if (inflight.gpu_timing.ticket) {
        const DWORD timing_error = GetLastError();
        inflight.gpu_timing.commit_end = GpuTimingTrace::Clock();
        SetLastError(timing_error);
      }
      TraceFrame(trace_return, "cmdbuf.commit.return", queue, "seq=%llu present=%u",
                 static_cast<unsigned long long>(seq), unsigned(bool(inflight.present)));
      queue->inflight_cmdbuf_count_.fetch_add(1, std::memory_order_relaxed);
      queue->inflight_cmdbuf_seq_.fetch_add(1, std::memory_order_release);
      queue->inflight_cmdbuf_seq_.notify_one();
    }
  };

  CommittingScope
  StartCommitting() {
    return CommittingScope(state_.get());
  }


  static HRESULT NativeMetalBinding(const D3D12NativeDescriptorCapture &capture,
                                    const metal_argument::Member &member, WMTArgumentBinding &binding) {
    binding = {};
    if (capture.counter) return DXGI_ERROR_UNSUPPORTED;
    binding.index = member.index;
    binding.kind = member.kind;
    if (metal_argument::Acceleration(member.kind)) {
      if (!capture.acceleration) return E_INVALIDARG;
      const auto kind = capture.acceleration->Type() == D3D12NativeAccelerationCapture::Kind::Primitive ?
          WMTArgumentKindPrimitiveAccelerationStructure : WMTArgumentKindInstanceAccelerationStructure;
      if (member.kind != kind) return E_INVALIDARG;
      binding.resource = capture.acceleration->Native().resource;
    } else if (capture.acceleration) {
      return E_INVALIDARG;
    } else if (member.kind == WMTArgumentKindBuffer) {
      if (capture.type != ShaderVisibleDescriptorType::ConstantBuffer &&
          capture.type != ShaderVisibleDescriptorType::SRVBuffer &&
          capture.type != ShaderVisibleDescriptorType::UAVBuffer) return E_INVALIDARG;
      if (!capture.resource || !capture.resource->buffer_allocation || !capture.byte_length)
        return E_INVALIDARG;
      binding.resource = capture.resource->buffer_allocation->buffer();
      binding.offset = capture.buffer_offset;
      binding.length = capture.byte_length;
    } else if (member.kind == WMTArgumentKindTexture) {
      if (capture.type == ShaderVisibleDescriptorType::SRVTexture ||
          capture.type == ShaderVisibleDescriptorType::UAVTexture)
        binding.resource = capture.texture.texture();
      else if (capture.type == ShaderVisibleDescriptorType::SRVTexelBuffer ||
               capture.type == ShaderVisibleDescriptorType::UAVTexelBuffer)
        binding.resource = capture.texel;
      else return E_INVALIDARG;
    } else {
      if (capture.type != ShaderVisibleDescriptorType::Sampler || !capture.sampler ||
          capture.sampler->lod_bias != 0) return DXGI_ERROR_UNSUPPORTED;
      binding.resource = capture.sampler->sampler_state;
    }
    return binding.resource ? S_OK : E_INVALIDARG;
  }

  static void PrepareResourceUses(D3D12NativeComputeDispatch &packet) {
    auto add = [&](obj_handle_t handle, WMTResourceUsage usage) {
      if (!handle) return;
      wmtcmd_compute_useresource command{};
      command.type = WMTComputeCommandUseResource;
      command.resource = handle;
      command.usage = usage;
      packet.resource_uses.push_back(command);
    };
    for (const auto &r : packet.resources) {
      if (r.acceleration) r.acceleration->ForEach([&](const auto &allocation) {
        add(allocation.resource, WMTResourceUsageRead);
      });
      const bool writable = r.type == ShaderVisibleDescriptorType::UAVBuffer ||
          r.type == ShaderVisibleDescriptorType::UAVTexelBuffer || r.type == ShaderVisibleDescriptorType::UAVTexture;
      const auto usage = writable ? WMTResourceUsage(WMTResourceUsageRead | WMTResourceUsageWrite) : WMTResourceUsageRead;
      if (r.resource) {
        if (r.resource->buffer_allocation) add(r.resource->buffer_allocation->buffer(), usage);
        if (r.resource->texture_allocation) add(r.resource->texture_allocation->texture(), usage);
      }
      if (r.texture) add(r.texture.texture(), usage);
      if (r.texel) add(r.texel, usage);
      if (r.counter && r.counter->buffer_allocation)
        add(r.counter->buffer_allocation->buffer(), WMTResourceUsage(WMTResourceUsageRead | WMTResourceUsageWrite));
    }
  }

  HRESULT PrepareAccelerationBuild(const std::shared_ptr<const D3D12AccelerationBuildPlan> &plan,
      D3D12SubmissionScenes &scenes,uint64_t &bytes,D3D12NativeAccelerationBuild &out) {
    out={};
    if (!plan || plan->device_identity!=static_cast<ID3D12Device *>(device_) ||
        (desc_.Type!=D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type!=D3D12_COMMAND_LIST_TYPE_COMPUTE)) return E_INVALIDARG;
    D3D12NativeAccelerationBuild next;next.plan=plan;
    std::vector<D3D12NativeAccelerationCapture::Ptr> children;
    for (const auto &input:plan->children) {
      auto child=input.producer?scenes.Find(input.producer.get()):input.existing;
      if (!child || child->Type()!=D3D12NativeAccelerationCapture::Kind::Primitive) return E_INVALIDARG;
      next.child_handles.push_back(child->Native().resource);
      children.push_back(std::move(child));
    }
    WMTASSizeInfo sizes{};
    if (state_->device_.accelerationStructureSizes(next.Descriptor(),sizes)!=WMTArgumentStatusReady)
      return E_INVALIDARG;
    if (!scene_build::Charge(sizes.structure_size,sizes.build_scratch_size,bytes)) return DXGI_ERROR_UNSUPPORTED;
    WMTArgumentStatus status=WMTArgumentStatusFailed;
    auto target=state_->device_.newAccelerationStructure(sizes.structure_size,status);
    if (!target || status!=WMTArgumentStatusReady) return status==WMTArgumentStatusOutOfMemory?E_OUTOFMEMORY:E_FAIL;
    WMTBufferInfo info{};info.length=sizes.build_scratch_size;info.options=WMTResourceStorageModePrivate;
    auto scratch=state_->device_.newBuffer(info);
    if (!scratch) return E_OUTOFMEMORY;
    auto owner=device_->GetResidencyOwner();
    next.scratch=std::make_shared<D3D12NativeArgumentBuffer>(std::move(scratch),info.gpu_address,owner);
    auto allocation=std::make_shared<D3D12NativeAccelerationAllocation>(std::move(target),owner);
    const auto result=D3D12NativeAccelerationCapture::Create(
        plan->kind==WMTASBuildTriangles?D3D12NativeAccelerationCapture::Kind::Primitive:D3D12NativeAccelerationCapture::Kind::Instance,
        std::move(allocation),std::move(children),next.result);
    if (result!=capture::Status::Ready) return CaptureResult(result);
    scenes.Publish(plan.get(),next.result);
    out=std::move(next);return S_OK;
  }

  HRESULT PrepareMetalArgumentDispatch(const D3D12RecordedComputeDispatch &record,
                                       const D3D12SubmissionScenes &scenes,D3D12NativeComputeDispatch &out) {
    const auto &plan = *record.metal_arguments;
    auto valid = CaptureResult(metal_argument::Match(plan, record.bindings.size(), record.entry_layout));
    if (FAILED(valid)) return valid;
    if (!record.function || !record.pipeline) return E_INVALIDARG;
    D3D12NativeComputeDispatch next;
    next.pipeline = record.pipeline;
    next.threads = record.threads;
    next.groups = record.groups;
    next.resources.resize(record.bindings.size());
    for (size_t i = 0; i < record.bindings.size(); ++i) {
      const auto &binding=record.bindings[i];
      if (binding.acceleration_producer) {
        if (binding.acceleration || binding.root_buffer || binding.compiler_footprint ||
            binding.recorded_device!=record.device || binding.acceleration_device.ptr()!=record.device) return E_INVALIDARG;
        next.resources[i].acceleration=scenes.Find(binding.acceleration_producer.get());
        if (!next.resources[i].acceleration) return E_INVALIDARG;
      } else {
        const auto hr=binding.ResolveForSubmission(next.resources[i]);
        if (FAILED(hr)) return hr;
      }
    }

    std::vector<WMTComputeBinding> requirements;
    for (const auto &member : plan.direct) {
      const WMTComputeBinding *requirement = nullptr;
      for (size_t i = 0; i < record.entry_layout.count; ++i) {
        const auto &r = record.entry_layout.bindings[i];
        if (r.kind != WMTComputeBindingArgumentBuffer && r.index == member.index &&
            uint64_t(r.kind) == uint64_t(member.kind)) requirement = &r;
      }
      if (!requirement) return E_INVALIDARG;
      const auto &capture = next.resources[member.source];
      if (requirement->access != 0 && capture.type != ShaderVisibleDescriptorType::UAVBuffer &&
          capture.type != ShaderVisibleDescriptorType::UAVTexture &&
          capture.type != ShaderVisibleDescriptorType::UAVTexelBuffer) return E_INVALIDARG;
      WMTArgumentBinding binding{};
      const auto hr = NativeMetalBinding(capture, member, binding);
      if (FAILED(hr)) return hr;
      requirements.push_back(*requirement);
      next.direct_bindings.push_back(binding);
    }
    const auto direct_status = MTLDevice_validateComputeBindings(state_->device_,
        requirements.data(), next.direct_bindings.data(), requirements.size());
    if (direct_status != WMTArgumentStatusReady)
      return direct_status == WMTArgumentStatusInvalid ? E_INVALIDARG :
          direct_status == WMTArgumentStatusUnsupported ? DXGI_ERROR_UNSUPPORTED : E_FAIL;
    uint64_t total = 0;
    for (const auto &block : plan.blocks) {
      std::vector<WMTArgumentBinding> bindings;
      bindings.reserve(block.members.size());
      for (const auto &member : block.members) {
        WMTArgumentBinding binding{};
        const auto hr = NativeMetalBinding(next.resources[member.source], member, binding);
        if (FAILED(hr)) return hr;
        bindings.push_back(binding);
      }
      WMTBufferInfo info{};
      WMTArgumentStatus status = WMTArgumentStatusFailed;
      auto buffer = record.function.newArgumentBuffer(block.index, bindings.data(), bindings.size(), info, status);
      if (!buffer || status != WMTArgumentStatusReady)
        return status == WMTArgumentStatusOutOfMemory ? E_OUTOFMEMORY :
               status == WMTArgumentStatusInvalid ? E_INVALIDARG :
               status == WMTArgumentStatusUnsupported ? DXGI_ERROR_UNSUPPORTED : E_FAIL;
      if (!info.length || info.length > 16 * 1024 * 1024 || total > 64 * 1024 * 1024 - info.length)
        return DXGI_ERROR_UNSUPPORTED;
      total += info.length;
      next.buffers.push_back(std::make_shared<D3D12NativeArgumentBuffer>(
          std::move(buffer), info.gpu_address, device_->GetResidencyOwner()));
      next.indices.push_back(block.index);
    }
    PrepareResourceUses(next);
    out = std::move(next);
    return S_OK;
  }

  HRESULT PrepareArgumentDispatch(const D3D12RecordedComputeDispatch &record,
                                  const D3D12SubmissionScenes &scenes,D3D12NativeComputeDispatch &out) {
    out = {};
    if (record.device != static_cast<ID3D12Device *>(device_) ||
        (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE))
      return E_INVALIDARG;
    if (record.metal_arguments) return PrepareMetalArgumentDispatch(record, scenes, out);
    for (const auto &binding : record.bindings)
      if (binding.acceleration || binding.acceleration_producer) return DXGI_ERROR_UNSUPPORTED;
    argument_upload::Upload<D3D12NativeDescriptorCapture, std::shared_ptr<D3D12NativeArgumentBuffer>> upload;
    std::vector<void *> mapped;
    mapped.reserve(record.arguments.blocks.size());
    const auto status = argument_upload::Prepare(record.arguments, record.bindings.size(),
      [&](size_t index, D3D12NativeDescriptorCapture &capture) {
        // Resolve public heap state only here, on the caller's Wine thread.
        const HRESULT hr = record.bindings[index].ResolveForSubmission(capture);
        if (SUCCEEDED(hr)) return capture::Status::Ready;
        return hr == E_OUTOFMEMORY ? capture::Status::OutOfMemory :
               hr == E_INVALIDARG ? capture::Status::Invalid : capture::Status::Unsupported;
      },
      [&](const argument_upload::Block &block, std::shared_ptr<D3D12NativeArgumentBuffer> &buffer,
          uint64_t &address) {
        WMTBufferInfo info{};
        info.length = block.initial.size();
        info.options = WMTResourceStorageModeShared;
        auto native = state_->device_.newBuffer(info);
        auto *memory = info.memory.get_accessible_or_null();
        if (!native || !memory) return capture::Status::OutOfMemory;
        address = info.gpu_address;
        buffer = std::make_shared<D3D12NativeArgumentBuffer>(std::move(native), address, device_->GetResidencyOwner());
        mapped.push_back(memory);
        return capture::Status::Ready;
      }, upload);
    if (status != capture::Status::Ready) return CaptureResult(status);
    D3D12NativeComputeDispatch next;
    next.pipeline = record.pipeline;
    next.threads = record.threads;
    next.groups = record.groups;
    next.buffers = std::move(upload.buffers);
    next.resources = std::move(upload.sources);
    next.indices.reserve(record.arguments.blocks.size());
    for (size_t i = 0; i < upload.bytes.size(); ++i) {
      memcpy(mapped[i], upload.bytes[i].data(), upload.bytes[i].size());
      next.indices.push_back(record.arguments.blocks[i].index);
    }
    PrepareResourceUses(next);
    out = std::move(next);
    return S_OK;
  }

  #include "submit-merge-trace.inc"

  bool TransferWindowEnabled() const {
    if (FullFrameCaptureEnabled()) {
      const auto n = capture_next_present.load(std::memory_order_relaxed);
      return FrameReadbackEnabled() && FrameTraceEnabled() && capture_lines.load(std::memory_order_relaxed) <= 40960 &&
             (n <= 3 || (n >= 512 && n <= 514) || (n >= 4096 && n <= 4098));
    }
    const auto n = state_->readback_present_index_;
    return FrameReadbackEnabled() && FrameTraceEnabled() && state_->transfer_trace_lines_ < 1024 &&
           ((n >= 511 && n <= 513) || (n >= 4095 && n <= 4097));
  }

  template<typename... Args>
  void TraceTransferAt(CommittingScope &scope, const void *list, const EncoderData *pass, uint64_t frame,
                     const char *op, const char *format, Args... args) {
    const DWORD error = GetLastError();
    const bool full = FullFrameCaptureEnabled();
    const unsigned row = full ? capture_lines.fetch_add(1, std::memory_order_relaxed) + 1 : ++state_->transfer_trace_lines_;
    if (full && row > 40961) { SetLastError(error); return; }
    char detail[2304], line[3072];
    const int detail_size = std::snprintf(detail, sizeof(detail), format, args...);
    if (detail_size < 0 || size_t(detail_size) >= sizeof(detail)) {
      op = "capture.format_limit";
      std::snprintf(detail, sizeof(detail), "truncated=1 reason=detail_size");
    }
    if (full && row == 40961) { op = "capture.limit"; std::snprintf(detail, sizeof(detail), "truncated=1 row_limit=40960"); }
    const int n = std::snprintf(line, sizeof(line),
        "dx12_transfer line=%u queue=%p before_present=%llu seq=%llu list=%p pass=%p id=%llu op=%s %s\n",
        row, state_.get(),
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(scope.seq), list, static_cast<const void *>(pass),
        static_cast<unsigned long long>(pass ? pass->id : 0), op, detail);
    DWORD written;
    if (n > 0 && size_t(n) < sizeof(line))
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
    SetLastError(error);
  }

  template<typename... Args>
  void TraceTransfer(CommittingScope &scope, const void *list, const EncoderData *pass,
                     const char *op, const char *format, Args... args) {
    if (!TransferWindowEnabled()) return;
    const auto frame = FullFrameCaptureEnabled() ? capture_next_present.load(std::memory_order_relaxed) : state_->readback_present_index_ + 1;
    TraceTransferAt(scope, list, pass, frame, op, format, args...);
  }

  static D3D12SubmissionCapture::Context FaultContext(const void *list, const EncoderData *pass) {
    return {reinterpret_cast<std::uintptr_t>(list), reinterpret_cast<std::uintptr_t>(pass),
            pass ? pass->id : 0, pass ? unsigned(pass->type) : 0};
  }

  static void CaptureRenderPass(CommittingScope &scope, const void *list, EncoderData *pass,
                                const WMTRenderPassInfo &info) {
    if (scope.inflight.fault_capture)
      scope.inflight.fault_capture->RenderPass(FaultContext(list, pass), info);
  }

  static void CaptureFaultCommands(CommittingScope &scope, const void *list, EncoderData *pass,
                                   WMTCommandCaptureFamily family, const wmtcmd_base *cmd) {
    if (scope.inflight.fault_capture)
      scope.inflight.fault_capture->Commands(FaultContext(list, pass), family, cmd);
  }

  void TraceTransferCommands(CommittingScope &scope, const void *list, EncoderData *pass, wmtcmd_base *cmd) {
    const auto family = pass->type == EncoderType::Render ? WMTCommandCaptureFamily::Render :
                        pass->type == EncoderType::Compute ? WMTCommandCaptureFamily::Compute :
                        WMTCommandCaptureFamily::Blit;
    CaptureFaultCommands(scope, list, pass, family, cmd);
    if (!TransferWindowEnabled())
      return;
    if (FullFrameCaptureEnabled()) {
      const auto frame = capture_next_present.load(std::memory_order_relaxed);
      CaptureWMTCommands(family, cmd, [&](const char *detail) {
        // Complete an admitted pass even if another queue advances the capture window meanwhile.
        TraceTransferAt(scope, list, pass, frame, "command", "%s", detail);
      });
      return;
    }
    unsigned count = 0;
    // Walk only the already-owned command chain, with a hard bound independent of the log cap.
    for (; cmd && count < 256; cmd = static_cast<wmtcmd_base *>(cmd->next.get()), ++count) {
      if (pass->type == EncoderType::Blit) {
        if (cmd->type == WMTBlitCommandCopyFromTextureToTexture) {
          auto c = reinterpret_cast<wmtcmd_blit_copy_from_texture_to_texture *>(cmd);
          TraceTransfer(scope, list, pass, "copy.region", "src=%llx dst=%llx src_level=%u src_slice=%u dst_level=%u dst_slice=%u size=%llu,%llu,%llu origin=%llu,%llu,%llu",
                        (unsigned long long)c->src, (unsigned long long)c->dst, c->src_level, c->src_slice,
                        c->dst_level, c->dst_slice, (unsigned long long)c->src_size.width,
                        (unsigned long long)c->src_size.height, (unsigned long long)c->src_size.depth,
                        (unsigned long long)c->dst_origin.x, (unsigned long long)c->dst_origin.y,
                        (unsigned long long)c->dst_origin.z);
        } else if (cmd->type == WMTBlitCommandCopyTexture) {
          auto c = reinterpret_cast<wmtcmd_blit_copy_texture *>(cmd);
          TraceTransfer(scope, list, pass, "copy.texture", "src=%llx dst=%llx",
                        (unsigned long long)c->src, (unsigned long long)c->dst);
        }
      } else if (pass->type == EncoderType::Render) {
        if (cmd->type == WMTRenderCommandSetPSO) {
          auto c = reinterpret_cast<wmtcmd_render_setpso *>(cmd);
          TraceTransfer(scope, list, pass, "render.pso", "pso=%llx", (unsigned long long)c->pso);
        } else if (cmd->type == WMTRenderCommandSetFragmentTexture) {
          auto c = reinterpret_cast<wmtcmd_render_settexture *>(cmd);
          TraceTransfer(scope, list, pass, "render.fragment_texture", "texture=%llx slot=%u",
                        (unsigned long long)c->texture, unsigned(c->index));
        } else if (cmd->type == WMTRenderCommandDraw || cmd->type == WMTRenderCommandDrawIndexed) {
          TraceTransfer(scope, list, pass, "render.draw", "type=%u", unsigned(cmd->type));
        }
      }
    }
    TraceTransfer(scope, list, pass, "commands.end", "scanned=%u truncated=%u", count, unsigned(cmd != nullptr));
  }

  void EncodeFrameReadback(CommittingScope &scope, MTLD3D12Resource *resource, TextureView &view) {
    if (!FrameReadbackEnabled() || state_->readback_present_index_ > 8192)
      return;
    const uint64_t frame = ++state_->readback_present_index_;
    if (frame != 1 && frame != 64 && frame != 512 && frame != 4096 && frame != 8192)
      return;
    auto &readback = scope.inflight.readback;
    readback.present_index = frame;
    EncodeTextureReadback(scope, view, readback, resource->texture->textureType(view.key),
                          resource->texture->sampleCount());
  }

  void EncodeRenderReadback(CommittingScope &scope, const RenderEncoderData &render) {
    if (!FrameReadbackEnabled() || state_->readback_render_index_ > 8192)
      return;
    const uint64_t index = ++state_->readback_render_index_;
    if (index != 1 && index != 64 && index != 512 && index != 4096 && index != 8192)
      return;
    auto &readback = scope.inflight.render_readbacks.emplace_back();
    readback.render_index = index;
    const auto &color = render.colors[0];
    // Sample the stored color attachment immediately after its render pass, before later passes can overwrite it.
    if (!color.attachment || color.level || color.slice || color.depth_plane ||
        color.store_action != WMTStoreActionStore) {
      readback.Log(state_.get(), scope.seq, "backbuffer.skip", "reason=unsupported_render_attachment");
      return;
    }
    auto &view = *color.attachment.ptr();
    auto *texture = view.allocation->descriptor;
    EncodeTextureReadback(scope, view, readback, texture->textureType(view.key), texture->sampleCount());
  }

  void EncodeTextureReadback(CommittingScope &scope, TextureView &view, D3D12FrameReadback &readback,
                            WMTTextureType type, unsigned samples) {
    readback.source_texture = view.texture;
    readback.format = view.texture.pixelFormat();
    readback.width = view.texture.width();
    readback.height = view.texture.height();
    const auto skip = [&](const char *reason) { readback.Log(state_.get(), scope.seq, "backbuffer.skip", reason); };
    switch (readback.format) {
    case WMTPixelFormatRGBA8Unorm:
    case WMTPixelFormatRGBA8Unorm_sRGB:
      break;
    case WMTPixelFormatBGRA8Unorm:
    case WMTPixelFormatBGRA8Unorm_sRGB:
      readback.bgra = true;
      break;
    default: {
      char reason[96];
      std::snprintf(reason, sizeof(reason), "reason=unsupported_format metal_pixel_format=0x%08x", unsigned(readback.format));
      skip(reason);
      return;
    }
    }
    if (type != WMTTextureType2D || samples != 1 ||
        view.texture.depth() != 1 || view.texture.arrayLength() != 1 ||
        view.allocation->flags().test(TextureAllocationFlag::AllocatedOnHeap)) {
      skip("reason=unsupported_texture_or_heap_allocation");
      return;
    }
    constexpr uint64_t max_dimension = 16384, max_bytes = 64ull * 1024 * 1024;
    if (!readback.width || !readback.height || readback.width > max_dimension || readback.height > max_dimension) {
      skip("reason=dimensions");
      return;
    }
    const uint64_t pitch = (readback.width * 4 + 255) & ~uint64_t(255);
    if (pitch > UINT32_MAX || readback.height > max_bytes / pitch) {
      skip("reason=readback_size");
      return;
    }
    readback.pitch = uint32_t(pitch);
    WMTBufferInfo info{};
    info.length = pitch * readback.height;
    info.options = WMTResourceStorageModeShared;
    readback.buffer = state_->device_.newBuffer(info);
    readback.data = static_cast<const uint8_t *>(info.memory.get_accessible_or_null());
    if (!readback.buffer || !readback.data) {
      skip("reason=buffer_allocation_or_mapping");
      return;
    }
    std::memset(info.memory.get_accessible_or_null(), D3D12FrameReadback::sentinel, size_t(info.length));
    WMT::Reference<WMT::Error> error;
    readback.residency = state_->device_.newResidencySet(2, error);
    if (!readback.residency) {
      skip("reason=residency_allocation");
      return;
    }
    readback.source_allocation = view.allocation->texture();
    WMT::Allocation allocations[] = {readback.source_allocation, readback.buffer};
    readback.residency.addAllocations(allocations, 2);
    readback.residency.commit();
    // Queue attachment provides execution-time residency; retirement empties the set.
    // At most ten empty sets remain attached for this queue's lifetime (five presents and five render passes).
    state_->queue_.addResidencySet(readback.residency);
    auto encoder = scope.inflight.cmdbuf.blitCommandEncoder();
    if (PassStatsEnabled()) g_pass_stats.service_readback_blit.fetch_add(1, std::memory_order_relaxed);
    encoder_stats::Count(encoder_stats::KindServiceReadback, encoder_stats::ReasonUnknown);
    if (!encoder) {
      skip("reason=blit_encoder_allocation");
      return;
    }
    encoder.waitForFence(state_->fence_);
    wmtcmd_blit_copy_from_texture_to_buffer copy{};
    copy.type = WMTBlitCommandCopyFromTextureToBuffer;
    copy.src = view.texture;
    copy.size = {uint32_t(readback.width), uint32_t(readback.height), 1};
    copy.dst = readback.buffer;
    copy.bytes_per_row = readback.pitch;
    MTLBlitCommandEncoder_encodeCommands(encoder, reinterpret_cast<const wmtcmd_base *>(&copy));
    encoder.updateFence(state_->fence_);
    encoder.endEncoding();
    readback.copied = true;
  }

  bool SameDevice(ID3D12DeviceChild *object) {
    if (!object) return false;
    Com<ID3D12Device> owner;
    return SUCCEEDED(object->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) &&
           owner.ptr() == static_cast<ID3D12Device *>(device_);
  }

  void RejectMapping() {
    ERR("Tile mappings: unsupported or invalid buffer-only mapping request");
    device_->RemoveDevice();
  }

  template<typename Operation>
  void SubmitMapping(CommittingScope &scope, Operation operation) {
    if (!state_->mapping_queue_) {
      state_->mapping_queue_ = state_->device_.newMappingCommandQueue();
      state_->mapping_event_ = state_->device_.newSharedEvent();
      if (state_->mapping_queue_ && !state_->mapping_queue_.addResidencySet(state_->residency_set_)) {
        state_->mapping_queue_ = {};
        RejectMapping();
        return;
      }
    }
    bool ok = state_->mapping_queue_ && state_->mapping_event_;
    // One attached global set includes reference-counted sparse backing leases.
    // requestResidency alone is not a substitute for execution-time residency.
    if (!ok) { RejectMapping(); return; }
    uint64_t ready = ++state_->mapping_serial_, done = ++state_->mapping_serial_;
    scope.inflight.cmdbuf.encodeSignalEvent(state_->mapping_event_, ready);
    ok = state_->mapping_queue_.waitForEvent(state_->mapping_event_, ready) && operation() &&
         state_->mapping_queue_.signalEvent(state_->mapping_event_, done);
    if (!ok) {
      state_->failed_mapping_resources_ = scope.inflight.sparse_resources;
      state_->failed_mapping_keepalive_ = state_;
      RejectMapping();
      return;
    }
    scope.inflight.cmdbuf.encodeWaitForEvent(state_->mapping_event_, done);
  }

  static bool LinearRegion(const D3D12_TILED_RESOURCE_COORDINATE *start, const D3D12_TILE_REGION_SIZE *size,
                           uint32_t capacity, uint64_t &offset, uint64_t &count) {
    offset = start ? start->X : 0;
    if (start && (start->Y || start->Z || start->Subresource)) return false;
    count = size ? (size->UseBox ? size->Width : size->NumTiles) : capacity;
    if (size && size->UseBox && (size->Height != 1 || size->Depth != 1)) return false;
    return count && offset <= capacity && count <= capacity - offset;
  }

  static void RetainBacking(D3D12SparseBufferState &resource, WMT::Heap heap) {
    for (const auto &old : resource.heaps) if (old.handle == heap.handle) return;
    resource.heaps.emplace_back(heap);
    resource.residency->Retain(heap);
  }

public:
  MTLD3D12CommandQueueImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12CommandQueue, IMTLSwapChainFactory>(pDevice) {}

  ~MTLD3D12CommandQueueImpl() {
    if (submission_) {
      {
        std::lock_guard lock(submission_->mutex);
        submission_->stop = true;
      }
      submission_->ready.notify_one();
      if (submission_thread_) CloseHandle(submission_thread_);
    }
    if (encoder_stats::EarlyEncode()) {
      {
        std::lock_guard<dxmt::mutex> lock(EarlyRegistryLock());
        auto &entries = EarlyRegistry();
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const EarlyQueueEntry &e) { return e.queue == this; }),
                      entries.end());
      }
      while (state_->early_users.load(std::memory_order_acquire))
        SwitchToThread();
    }
    if (!inflight_cmdbuf_wait_thread_)
      return;
    std::lock_guard<dxmt::mutex> lock(state_->mutex_commit_);
    state_->inflight_cmdbuf_stop_.store(state_->inflight_cmdbuf_seq_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    state_->inflight_cmdbuf_seq_.fetch_add(1, std::memory_order_release);
    state_->inflight_cmdbuf_seq_.notify_one();
    // A user GPU wait may never signal. The worker drains native state only;
    // final COM release must not join it or keep the public device alive.
    CloseHandle(inflight_cmdbuf_wait_thread_);
  }

  HRESULT
  Initialize(const D3D12_COMMAND_QUEUE_DESC *pDesc) {
    // TODO: validate and normalize
    desc_ = *pDesc;
    desc_.NodeMask = 1; // typically 1 GPU only

    auto metal_device = device_->GetMTLDevice();
    state_->device_ = metal_device;
    state_->residency_set_ = device_->GetGlobalResidencySet();
    // ENCODERS 4.1: room for the command buffers encoded at Close() and not committed yet (kEarlyMax), so that creating
    // one never blocks a recording thread behind work only the submit thread can commit.
    state_->queue_ = metal_device.newCommandQueue(state_->inflight_depth_ + (encoder_stats::EarlyEncode() ? kEarlyMax : 0));
    if (!state_->queue_)
      return E_FAIL;
    state_->queue_.addResidencySet(state_->residency_set_);

    state_->fence_ = metal_device.newFence();
    state_->immediate_out_fence_ = metal_device.newFence();
    if (!state_->fence_ || !state_->immediate_out_fence_)
      return E_OUTOFMEMORY;
    state_->timestamp_event_ = metal_device.newSharedEvent();
    if (!state_->timestamp_event_)
      return E_OUTOFMEMORY;
    WMTBufferInfo visibility_info{};
    visibility_info.length = sizeof(uint64_t);
    visibility_info.options = WMTResourceStorageModePrivate;
    state_->visibility_scratch_ = metal_device.newBuffer(visibility_info);
    if (!state_->visibility_scratch_)
      return E_OUTOFMEMORY;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&RetireD3D12Queue), &module))
      return HRESULT_FROM_WIN32(GetLastError());
    auto retirement = std::make_unique<D3D12QueueRetirement>(D3D12QueueRetirement{state_, module});
    inflight_cmdbuf_wait_thread_ = CreateThread(nullptr, 0x100000, RetireD3D12Queue, retirement.get(),
                                              STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!inflight_cmdbuf_wait_thread_) {
      const DWORD error = GetLastError();
      FreeLibrary(module);
      return HRESULT_FROM_WIN32(error);
    }
    retirement.release();
    if (PassEnvironmentSwitch("MACRUNNER_DX12_ASYNC_SUBMIT")) {
      submission_ = std::make_shared<D3D12SubmissionQueue>();
      HMODULE submit_module = nullptr;
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(&SubmitD3D12Queue), &submit_module))
        return HRESULT_FROM_WIN32(GetLastError());
      auto worker = std::make_unique<D3D12SubmissionWorker>(D3D12SubmissionWorker{submission_, submit_module});
      submission_thread_ = CreateThread(nullptr, 0x100000, SubmitD3D12Queue, worker.get(),
          STACK_SIZE_PARAM_IS_A_RESERVATION, &submission_thread_id_);
      if (!submission_thread_) {
        const DWORD error = GetLastError();
        FreeLibrary(submit_module);
        return HRESULT_FROM_WIN32(error);
      }
      worker.release();
    }
    if (encoder_stats::EarlyEncode()) {
      std::lock_guard<dxmt::mutex> lock(EarlyRegistryLock());
      EarlyRegistry().push_back({device_, desc_.Type, this, state_});
    }

    return S_OK;
  }

  // ---- ENCODERS 4.1: encode at Close() on the recording thread, commit in order at ExecuteCommandLists ----------------
  const uint64_t queue_id_ = g_next_queue_id.fetch_add(1);

  static bool EarlyDiagnosticsOff() {
    return !FaultCaptureEnabled() && !FrameReadbackEnabled() && !FullFrameCaptureEnabled() && !merge_trace::Target();
  }
  bool EarlyEncodeActive() const { return encoder_stats::EarlyEncode() && EarlyDiagnosticsOff(); }
  // What an early encoding cannot do: read queue runtime state (OUT writes, timestamp events), or use packets the submit
  // path prepares per execution (argument dispatches, scene builds, counter-resolve scratch).
  static bool EarlyEligible(MTLD3D12GraphicsCommandList *list) {
    for (auto pass = list->entry; pass; pass = pass->next) {
      switch (pass->type) {
      case EncoderType::Null: case EncoderType::Clear: case EncoderType::Render: case EncoderType::Compute:
      case EncoderType::Resolve:
        break;
      case EncoderType::Blit: {
        auto b = static_cast<BlitEncoderData *>(pass);
        if (b->timestamp_samples || b->resolve_samples || b->immediate_out || b->wait_for_prior_work) return false;
        break;
      }
      default:
        return false;
      }
    }
    return true;
  }
public:
  void EncodeEarly(MTLD3D12GraphicsCommandList *list) {
    auto &st = encoder_stats::Get();
    if (EmptyDeferBoundary(list)) {
      c17::Note(c17::Get().empty_defer_lists);
      return;
    }
    if (!EarlyDiagnosticsOff() || !EarlyEligible(list)) { encoder_stats::Note(st.early_ineligible); return; }
    if (state_->early_outstanding.fetch_add(1, std::memory_order_acq_rel) >= kEarlyMax) {
      state_->early_outstanding.fetch_sub(1, std::memory_order_acq_rel);
      encoder_stats::Note(st.early_limit);
      return;
    }
    LARGE_INTEGER t0{}, t1{};
    QueryPerformanceCounter(&t0);
    {
      CommittingScope scope(state_.get(), CommittingScope::Early{});
      ID3D12CommandList *one = list;
      EncodeIntoScope(scope, 1, &one, true);
      if (scope.discard || !scope.inflight.cmdbuf) {
        state_->early_outstanding.fetch_sub(1, std::memory_order_acq_rel);
        encoder_stats::Note(st.early_ineligible);
        return;
      }
      list->early_cmdbuf = std::move(scope.inflight.cmdbuf);
    }
    list->early_queue_id = queue_id_;
    list->early_consumed = false;
    auto state = state_;
    list->early_token = std::shared_ptr<void>(reinterpret_cast<void *>(1), [state](void *) {
      state->early_outstanding.fetch_sub(1, std::memory_order_acq_rel);
    });
    QueryPerformanceCounter(&t1);
    encoder_stats::Note(st.early_ticks, uint64_t(t1.QuadPart - t0.QuadPart));
    encoder_stats::Note(st.early_encoded);
    list->early_encode_ticks = uint64_t(t1.QuadPart - t0.QuadPart);
  }
private:
  // The command buffer encoded at Close() for this queue, if it is still valid now; otherwise none (and it is dropped).
  WMT::Reference<WMT::CommandBuffer> TakeEarly(MTLD3D12GraphicsCommandList *list) {
    auto &st = encoder_stats::Get();
    WMT::Reference<WMT::CommandBuffer> out;
    if (!list->early_cmdbuf) {
      if (list->early_consumed) encoder_stats::Note(st.early_fallback_reexecute);
      return out;
    }
    if (list->early_queue_id != queue_id_) encoder_stats::Note(st.early_fallback_queue);
    else if (state_->immediate_out_pending_) encoder_stats::Note(st.early_fallback_out_pending);
    else out = std::move(list->early_cmdbuf);
    list->early_cmdbuf = {};
    list->early_token.reset();
    list->early_consumed = true;
    return out;
  }
  // c17 (MACRUNNER_DX12_C17_STATS): with EARLY_ENCODE every list is its own Metal command buffer, so a chain of passes
  // with the same targets across lists cannot share an encoder. Counted here, for the lists of one ExecuteCommandLists:
  // the last render pass of list i (only barriers after it) and the first render pass of list i+1 (only barriers before
  // it) with the same targets = one cross-list pair; whether the barriers between them would let MERGE_DEPENDENCIES merge
  // them (xl_mergeable) or why not; runs of lists linked by mergeable pairs (xl_runs, lists in them xl_run_lists).
  static void NoteCrossListChains(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    if (!c17::Enabled() || Count < 2) return;
    struct Edge { RenderEncoderData *render = nullptr; std::vector<const BlitEncoderData *> barriers; };
    const auto barrier_pass = [](EncoderData *pass) {
      return pass->type == EncoderType::Blit && static_cast<BlitEncoderData *>(pass)->barrier_only;
    };
    const auto head = [&](MTLD3D12GraphicsCommandList *list) {
      Edge e;
      for (auto pass = list->entry; pass; pass = pass->next) {
        if (pass->type == EncoderType::Null) continue;
        if (barrier_pass(pass)) { e.barriers.push_back(static_cast<BlitEncoderData *>(pass)); continue; }
        if (pass->type == EncoderType::Render) e.render = static_cast<RenderEncoderData *>(pass);
        break;
      }
      return e;
    };
    const auto tail = [&](MTLD3D12GraphicsCommandList *list) {
      Edge e;
      for (auto pass = list->entry; pass; pass = pass->next) {
        if (pass->type == EncoderType::Null) continue;
        if (barrier_pass(pass)) { if (e.render) e.barriers.push_back(static_cast<BlitEncoderData *>(pass)); continue; }
        e.render = pass->type == EncoderType::Render ? static_cast<RenderEncoderData *>(pass) : nullptr;
        e.barriers.clear();
      }
      return e;
    };
    auto &cs = c17::Get();
    // xl_pairs has already passed attachment equality; xl_reject_attachment means an attachment BARRIER.
    // Keep diagnostics separate from MERGE_TRACE_FRAME: that older full trace disables EARLY_ENCODE globally.
    const bool audit = c17::BarrierAudit() && cs.frames.load(std::memory_order_relaxed) == c17::BarrierAuditFrame();
    static std::atomic<uint64_t> audit_submit{0}, audit_rows{0};
    const auto audit_id = audit ? audit_submit.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    const auto emit_audit = [&](const char *details) {
      if (!audit) return;
      const auto row = audit_rows.fetch_add(1, std::memory_order_relaxed);
      if (row > 4096) return;
      const DWORD error = GetLastError();
      char line[640]; DWORD written;
      const int n = std::snprintf(line, sizeof(line), "dx12_xl_barrier frame=%u submit=%llu row=%llu %s\n",
          c17::BarrierAuditFrame(), (unsigned long long)audit_id, (unsigned long long)row,
          row == 4096 ? "op=limit cap=4096 incomplete=1" : details);
      if (n > 0 && size_t(n) < sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
      SetLastError(error);
    };
    bool previous_linked = false;
    RenderEncoderData *previous_head = nullptr;   // first render pass of list i (head of the previous pair's right side)
    for (UINT i = 0; i + 1 < Count; ++i) {
      const auto t = tail(static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]));
      const auto h = head(static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i + 1]));
      // a run goes on through list i only if list i is one render pass: its head is its tail
      const bool continues = previous_linked && previous_head && previous_head == t.render;
      bool linked = false;
      if (t.render && h.render && SameRenderTargets(*t.render, *h.render)) {
        c17::Note(cs.xl_pairs);
        if (audit) {
          char detail[512];
          std::snprintf(detail, sizeof(detail),
              "op=pair pair=%u same_targets=1 width=%u height=%u tail_writer=%u head_writer=%u tail_ticks=%llu head_ticks=%llu tail_bind_after=%llu head_bind_before=%llu indirect=%u",
              i, t.render->render_target_width, t.render->render_target_height,
              unsigned(t.render->may_write_memory), unsigned(h.render->may_write_memory),
              (unsigned long long)static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->early_encode_ticks,
              (unsigned long long)static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i+1])->early_encode_ticks,
              (unsigned long long)(static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->empty_binding_epoch-t.render->empty_binding_last),
              (unsigned long long)h.render->empty_binding_first,
              unsigned(static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->empty_indirect ||
                       static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i+1])->empty_indirect));
          emit_audit(detail);
          const auto attachment = [&](const auto &a, unsigned slot) {
            if (!a.attachment) return;
            std::snprintf(detail, sizeof(detail),
                "op=attachment pair=%u slot=%u object=%llx view=%llx key=%llx level=%u slice=%u depth_plane=%u",
                i, slot, (unsigned long long)AttachmentObject(a.attachment),
                (unsigned long long)a.attachment.texture().handle, (unsigned long long)uint64_t(a.attachment->key),
                unsigned(a.level), unsigned(a.slice), unsigned(a.depth_plane));
            emit_audit(detail);
          };
          for (unsigned slot=0;slot<std::size(t.render->colors);++slot) attachment(t.render->colors[slot],slot);
          attachment(t.render->depth,8);attachment(t.render->stencil,9);
          for (unsigned side=0;side<2;++side) for (const auto *b : side ? h.barriers : t.barriers) {
            std::snprintf(detail, sizeof(detail), "op=barrier_pass pair=%u side=%u valid=%u records=%llu", i, side,
                unsigned(b->barrier_records_valid), (unsigned long long)b->barrier_records.size());
            emit_audit(detail);
            for (const auto &rec : b->barrier_records) {
              std::snprintf(detail, sizeof(detail),
                  "op=barrier pair=%u side=%u type=%u flags=%u object=%llx object2=%llx subresource=%u before=%u after=%u touches_attachment=%u",
                  i, side, unsigned(rec.type), unsigned(rec.flags), (unsigned long long)rec.object,
                  (unsigned long long)rec.object2, rec.subresource, rec.before, rec.after,
                  unsigned(PassUsesObject(*t.render,rec.object)||PassUsesObject(*t.render,rec.object2)));
              emit_audit(detail);
            }
          }
        }
        const bool plain = t.render->visibility_targets.empty() && h.render->visibility_targets.empty() &&
                           (!h.render->visibility_direct || h.render->visibility_direct.handle == t.render->visibility_direct.handle);
        uint8_t reject = ChainBarrierNone;
        bool relaxed = false;
        for (int side = 0; side < 2 && !reject; ++side)
          for (const auto *b : side ? h.barriers : t.barriers) {
            const auto verdict = ClassifyChainBarrier(*b, *t.render);
            if (verdict.reject) { reject = verdict.reject; break; }
            relaxed |= verdict.relaxed;
          }
        if (!plain) c17::Note(cs.xl_reject_not_plain);
        else if (reject == ChainBarrierAttachment) c17::Note(cs.xl_reject_attachment);
        else if (reject == ChainBarrierUnknownObject) c17::Note(cs.xl_reject_unknown_object);
        else if (reject == ChainBarrierRecordsInvalid) c17::Note(cs.xl_reject_records_invalid);
        else if (reject == ChainBarrierAliasingUnnamed) c17::Note(cs.xl_reject_aliasing_unnamed);
        else if (relaxed && (t.render->may_write_memory || h.render->may_write_memory)) c17::Note(cs.xl_reject_writer);
        else {
          linked = true;
          c17::Note(cs.xl_mergeable);
          if (relaxed) c17::Note(cs.xl_relaxed);
        }
      }
      if (linked) {
        // CPU price of moving these lists out of EARLY_ENCODE (options b/c): their own early-encoding time, QPC ticks
        const auto ticks = [&](UINT k) { return static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[k])->early_encode_ticks; };
        if (!continues) {
          c17::Note(cs.xl_runs); c17::Note(cs.xl_run_lists, 2);
          c17::Note(cs.xl_run_early_ticks, ticks(i) + ticks(i + 1));
        } else {
          c17::Note(cs.xl_run_lists);
          c17::Note(cs.xl_run_early_ticks, ticks(i + 1));
        }
      }
      previous_linked = linked;
      previous_head = h.render;
    }
  }
  void ExecuteWithEarly(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    auto &st = encoder_stats::Get();
    NoteCrossListChains(Count, ppCommandLists);
    UINT i = 0;
    while (i < Count) {
      auto list = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
      if (auto cmdbuf = TakeEarly(list)) {
        CommittingScope scope(state_.get(), std::move(cmdbuf));   // commits in order, retired by the ring
        encoder_stats::Note(st.early_committed);
        ++i;
        continue;
      }
      UINT j = i + 1;
      while (j < Count && !static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[j])->early_cmdbuf)
        ++j;
      encoder_stats::Note(st.early_fallback_lists, j - i);
      ExecuteRun(j - i, ppCommandLists + i);
      i = j;
    }
  }
public:

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLSwapChainFactory)) {
      *ppvObject = ref_and_cast<IMTLSwapChainFactory>(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) || riid == __uuidof(ID3D11Device2) ||
        riid == __uuidof(ID3D11Device3) || riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5))
      return E_NOINTERFACE;

    if (riid == kD3D12CommandQueueDownlevelUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid)) {
      WARN("D3D12CommandQueue: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE UpdateTileMappings(
      ID3D12Resource *resource, UINT region_count, const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
      const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap, UINT range_count,
      const D3D12_TILE_RANGE_FLAGS *range_flags, const UINT *heap_range_offsets, const UINT *range_tile_counts,
      D3D12_TILE_MAPPING_FLAGS flags
  ) {
    if (FAILED(device_->GetDeviceRemovedReason())) return;
    if (!SameDevice(resource) || !SameDevice(heap) || region_count != 1 || !range_count ||
        !region_start_coordinates || !region_sizes || !heap_range_offsets || !range_tile_counts ||
        flags != D3D12_TILE_MAPPING_FLAG_NONE) { RejectMapping(); return; }
    auto sparse = static_cast<MTLD3D12Resource *>(resource)->sparse;
    auto backing = static_cast<MTLD3D12Heap *>(heap);
    if (!sparse || !backing->placement_sparse_compatible) { RejectMapping(); return; }
    uint64_t offset, count;
    if (!LinearRegion(region_start_coordinates, region_sizes, sparse->tiles, offset, count) || range_count > count) {
      RejectMapping(); return;
    }
    const uint64_t heap_tiles = backing->GetDesc().SizeInBytes / 65536;
    std::vector<WMT4UpdateSparseBufferMappingOperation> operations;
    uint64_t consumed = 0;
    for (UINT i = 0; i < range_count; ++i) {
      const auto mode = range_flags ? range_flags[i] : D3D12_TILE_RANGE_FLAG_NONE;
      const uint64_t n = range_tile_counts ? range_tile_counts[i] : count;
      const uint64_t h = heap_range_offsets ? heap_range_offsets[i] : 0;
      if (!n || consumed > count || n > count - consumed ||
          (mode != D3D12_TILE_RANGE_FLAG_NONE && mode != D3D12_TILE_RANGE_FLAG_SKIP &&
           mode != D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE)) { RejectMapping(); return; }
      if (mode != D3D12_TILE_RANGE_FLAG_SKIP) {
        const uint64_t needed = mode == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE ? 1 : n;
        if (h > heap_tiles || needed > heap_tiles - h) { RejectMapping(); return; }
        if (mode == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) {
          for (uint64_t j = 0; j < n; ++j)
            operations.push_back({WMTSparseTextureMappingModeMap, {offset + consumed + j, 1}, h});
        } else operations.push_back({WMTSparseTextureMappingModeMap, {offset + consumed, n}, h});
      }
      consumed += n;
    }
    if (consumed != count) { RejectMapping(); return; }
    if (operations.empty()) return;
    if (DeferSubmission()) {
      auto native_heap = WMT::Reference<WMT::Heap>(backing->heap);
      std::lock_guard order(submission_->producer_mutex);
      SubmitCPU([this, sparse, native_heap, operations = std::move(operations)] {
        if (FAILED(device_->GetDeviceRemovedReason())) return;
        auto scope = StartCommitting();
        std::lock_guard lock(sparse->mutex);
        RetainBacking(*sparse, native_heap);
        scope.inflight.sparse_resources.push_back(sparse);
        SubmitMapping(scope, [&] {
          return state_->mapping_queue_.updateBufferMappings(sparse->buffer, native_heap, operations.data(), operations.size());
        });
      });
      return;
    }
    auto scope = StartCommitting();
    std::lock_guard lock(sparse->mutex);
    RetainBacking(*sparse, backing->heap);
    scope.inflight.sparse_resources.push_back(sparse);
    SubmitMapping(scope, [&] {
      return state_->mapping_queue_.updateBufferMappings(sparse->buffer, backing->heap, operations.data(), operations.size());
    });
  };

  void STDMETHODCALLTYPE CopyTileMappings(
      ID3D12Resource *dst_resource, const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
      ID3D12Resource *src_resource, const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags
  ) {
    if (FAILED(device_->GetDeviceRemovedReason())) return;
    if (!SameDevice(dst_resource) || !SameDevice(src_resource) || !dst_region_start_coordinate ||
        !src_region_start_coordinate || !region_size || flags != D3D12_TILE_MAPPING_FLAG_NONE) {
      RejectMapping(); return;
    }
    auto dst = static_cast<MTLD3D12Resource *>(dst_resource)->sparse;
    auto src = static_cast<MTLD3D12Resource *>(src_resource)->sparse;
    uint64_t dst_offset, src_offset, dst_count, src_count;
    if (!dst || !src || dst == src ||
        !LinearRegion(dst_region_start_coordinate, region_size, dst->tiles, dst_offset, dst_count) ||
        !LinearRegion(src_region_start_coordinate, region_size, src->tiles, src_offset, src_count) || dst_count != src_count) {
      RejectMapping(); return;
    }
    if (DeferSubmission()) {
      std::lock_guard order(submission_->producer_mutex);
      SubmitCPU([this, dst, src, dst_offset, src_offset, src_count] {
        if (FAILED(device_->GetDeviceRemovedReason())) return;
        auto scope = StartCommitting();
        std::scoped_lock lock(dst->mutex, src->mutex);
        for (auto &heap : src->heaps) RetainBacking(*dst, heap);
        scope.inflight.sparse_resources = {src, dst};
        WMT4CopySparseBufferMappingOperation operation{{src_offset, src_count}, dst_offset};
        SubmitMapping(scope, [&] {
          return state_->mapping_queue_.copyBufferMappings(src->buffer, dst->buffer, &operation, 1);
        });
      });
      return;
    }
    auto scope = StartCommitting();
    std::scoped_lock lock(dst->mutex, src->mutex);
    for (auto &heap : src->heaps) RetainBacking(*dst, heap);
    scope.inflight.sparse_resources = {src, dst};
    WMT4CopySparseBufferMappingOperation operation{{src_offset, src_count}, dst_offset};
    SubmitMapping(scope, [&] {
      return state_->mapping_queue_.copyBufferMappings(src->buffer, dst->buffer, &operation, 1);
    });
  };

  void STDMETHODCALLTYPE
  ExecuteCommandLists(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    if (FAILED(device_->GetDeviceRemovedReason())) return;
    pipeline_timing::Scope pipeline(pipeline_timing::Execute, state_.get(), 0, Count,
                                   pipeline_timing::Pointer(this));
    static std::atomic<uint32_t> trace_enter{0}, trace_accepted{0}, trace_rejected{0};
    TraceFrame(trace_enter, "execute.enter", this, "type=%u count=%u state=%p", unsigned(desc_.Type), Count,
               static_cast<const void *>(state_.get()));
    for (UINT i = 0; i < Count; ++i) {
      auto list = ppCommandLists ? static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]) : nullptr;
      if (!list || FAILED(list->close_result) || list->encoder_count == std::numeric_limits<size_t>::max()) {
        ERR("ExecuteCommandLists: invalid or unclosed command list");
        TraceFrame(trace_rejected, "execute.rejected", this,
                   "type=%u count=%u index=%u array=%p list=%p reason=%s close_hr=%08x encoders=%llu tid=%lu",
                   unsigned(desc_.Type), Count, i, static_cast<const void *>(ppCommandLists),
                   static_cast<const void *>(list), !ppCommandLists ? "null_array" : !list ? "null_entry" :
                   FAILED(list->close_result) ? "Close_failed" : "not_closed",
                   unsigned(list ? list->close_result : E_POINTER),
                   static_cast<unsigned long long>(list ? list->encoder_count : 0),
                   static_cast<unsigned long>(GetCurrentThreadId()));
        device_->RemoveDevice();
        return;
      }
    }
    TraceFrame(trace_accepted, "execute.accepted", this, "type=%u count=%u state=%p", unsigned(desc_.Type), Count,
               static_cast<const void *>(state_.get()));
    if (DeferSubmission()) {
      std::lock_guard order(submission_->producer_mutex);
      try {
        std::vector<Com<MTLD3D12GraphicsCommandList>> recordings;
        recordings.reserve(Count);
        for (UINT i = 0; i < Count; ++i)
          recordings.push_back(static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->SnapshotSubmission());
        SubmitCPU([this, recordings = std::move(recordings)] {
          std::vector<ID3D12CommandList *> lists;
          lists.reserve(recordings.size());
          for (const auto &list : recordings) lists.push_back(list.ptr());
          ExecuteCommandLists(UINT(lists.size()), lists.data());
        });
      } catch (...) { device_->RemoveDevice(); }
      return;
    }
    // ENCODERS: submit-thread wall time of the whole call (both paths), for the per-frame audit.
    const bool timed = encoder_stats::Enabled();
    LARGE_INTEGER total_t0{};
    if (timed) QueryPerformanceCounter(&total_t0);
    if (EarlyEncodeActive()) {
      state_->early_last_execute.store(g_execute_clock.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
      ExecuteWithEarly(Count, ppCommandLists);
    }
    else
      ExecuteRun(Count, ppCommandLists);
    if (timed) {
      LARGE_INTEGER t1{};
      QueryPerformanceCounter(&t1);
      encoder_stats::Note(encoder_stats::Get().execute_total_ticks, uint64_t(t1.QuadPart - total_t0.QuadPart));
    }
    encoder_stats::Executed();   // once per call, whichever path encoded the lists
    c17::Executed();
  }

  // One Metal command buffer for lists [0, Count): the recorded passes are encoded here, on the calling thread.
  void ExecuteRun(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    auto scope = StartCommitting();
    D3D12SubmissionScenes scenes;
    uint64_t acceleration_bytes=0;
    // Prepare every occurrence separately, including the same list submitted twice.
    // No COM-bearing recording survives into the native retirement packet.
    try {
      for (UINT i = 0; i < Count; ++i) {
        auto list = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
        for (auto pass = list->entry; pass; pass = pass->next) {
          if (pass->type==EncoderType::AccelerationBuild) {
            D3D12NativeAccelerationBuild native;
            const auto hr=PrepareAccelerationBuild(static_cast<AccelerationBuildEncoderData *>(pass)->build,
                                                  scenes,acceleration_bytes,native);
            if (FAILED(hr)) {
              scope.discard=true;
              ERR("ExecuteCommandLists: scene build preparation failed, hr=",str::format(hr));
              device_->RemoveDevice();return;
            }
            scope.inflight.acceleration_builds.push_back(std::move(native));
            continue;
          }
          if (pass->type != EncoderType::ArgumentCompute) continue;
          const auto &record = static_cast<ArgumentComputeEncoderData *>(pass)->dispatch;
          D3D12NativeComputeDispatch native;
          const HRESULT hr = record ? PrepareArgumentDispatch(*record, scenes, native) : E_INVALIDARG;
          if (FAILED(hr)) {
            scope.discard = true;
            ERR("ExecuteCommandLists: argument preparation failed, hr=", str::format(hr));
            device_->RemoveDevice();
            return;
          }
          scope.inflight.argument_dispatches.push_back(std::move(native));
        }
      }
    } catch (const std::bad_alloc &) {
      scope.discard = true;
      ERR("ExecuteCommandLists: argument preparation exhausted memory");
      device_->RemoveDevice();
      return;
    }
    EncodeIntoScope(scope, Count, ppCommandLists, false);
  }

  // Encodes the recorded passes of lists [0, Count) into scope.inflight.cmdbuf. `early` = at Close() on the recording
  // thread (ENCODERS 4.1): no queue runtime state is read or written then (eligibility excludes what would need it).
  void EncodeIntoScope(CommittingScope &scope, UINT Count, ID3D12CommandList *const *ppCommandLists, bool early) {
    bool early_out_pending = false;
    bool &out_pending = early ? early_out_pending : state_->immediate_out_pending_;
    size_t argument_index = 0, acceleration_index=0;
    auto &cmdbuf = scope.inflight.cmdbuf;
    // Pass coalescing state lives for one ExecuteCommandLists call, i.e. one Metal command buffer.
    const bool pass_stats = PassStatsEnabled();
    merge_trace::Emit(state_.get(), scope.seq, nullptr, 0, "submit.begin", "lists=%u", Count);
    const bool coalescing_allowed = !scope.inflight.fault_capture && !FrameReadbackEnabled() && !FullFrameCaptureEnabled();
    // ENCODERS: step 7 (MERGE_SAFE) merges like MERGE_PASSES=1 but across barriers that do not touch the pass;
    // step 4 (DEFER_CLEARS) carries full clears to the first pass that uses their texture (needs the fold machinery).
    const bool merge_safe = (encoder_stats::MergeSafe() || c17::MergeDependencies() || c17::EmptyTransitions()) && coalescing_allowed;
    const auto empty_plan = coalescing_allowed ? PlanEmptyTransitions(Count, ppCommandLists) : EmptyTransitionPlan{};
    const bool defer_clears = encoder_stats::DeferClears() && coalescing_allowed;
    const unsigned merge_mode = coalescing_allowed ? std::max(MergePassesMode(), merge_safe ? 1u : 0u) : 0;
    const bool merge_passes = merge_mode != 0;
    bool barrier_since_render = false;            // a (skipped) ResourceBarrier pass was seen after last_render
    const bool fold_clears = (FoldClearsEnabled() || defer_clears) && coalescing_allowed;
    obj_handle_t open_visibility = 0;             // visibility buffer of the open merged encoder (merge_safe)
    bool crossed_safe_barrier = false;            // a barrier that touches no attachment was skipped since last_render
    std::vector<uint32_t> open_slots;             // direct query slots counted in the open merged encoder
    // c17 MERGE_DEPENDENCIES (implies MERGE_SAFE's merge mode): relaxed barriers since last_render, why the last barrier
    // stopped the merge, and whether a draw of the open encoder can write memory.
    const bool merge_deps = c17::MergeDependencies() && coalescing_allowed;
    const bool c17_stats = c17::Enabled();
    const bool clear_subresource = c17::ClearSubresource() && coalescing_allowed;
    bool relaxed_since_render = false;
    uint8_t barrier_reason = ChainBarrierNone;
    bool open_writes = false;
    // Level-4 audit (stats only): attachments stored and not read since, to count stores a later full clear makes dead.
    const bool audit = encoder_stats::Enabled();
    std::unordered_map<obj_handle_t, bool> stored_unread;
    RenderStateAudit state_audit;
    LARGE_INTEGER execute_t0{};
    if (audit) QueryPerformanceCounter(&execute_t0);
    const auto audit_clear_object = [&](obj_handle_t object) {
      if (!audit || !object) return;
      auto it = stored_unread.find(object);
      if (it != stored_unread.end() && it->second)
        encoder_stats::Note(encoder_stats::Get().att_store_dead);
      stored_unread[object] = true;   // the clear itself is stored
    };
    // ENCODERS 4.2: a store whose next event in this submission is a full clear of the same view (or a discard, seen as a
    // DontCare load) with nothing in between that could read it (a pass loading the texture, a barrier about it, any
    // copy / compute / resolve work) becomes DontCare. Mask bits: 0-7 colours, 8 depth, 9 stencil.
    std::unordered_map<const RenderEncoderData *, uint16_t> dead_stores;
    uint16_t open_dead_mask = 0;   // dead stores of the open merged encoder: nothing may continue it
    if (encoder_stats::TileMemory() && coalescing_allowed) {
      struct Stored { RenderEncoderData *pass; uint16_t bit; obj_handle_t view; uint32_t plane; };
      std::unordered_map<obj_handle_t, std::vector<Stored>> pending;
      for (UINT l = 0; l < Count; ++l)
        for (auto pass = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[l])->entry; pass; pass = pass->next) {
          switch (pass->type) {
          case EncoderType::Null:
            break;
          case EncoderType::Render: {
            auto r = static_cast<RenderEncoderData *>(pass);
            bool resolve = false;
            for (auto &c : r->colors) resolve |= bool(c.resolve_attachment);
            const auto each = [&](auto &&f) {
              for (unsigned k = 0; k < 8; ++k)
                if (r->colors[k].attachment)
                  f(r->colors[k].attachment, r->colors[k].load_action, r->colors[k].store_action, uint16_t(1u << k),
                    r->colors[k].depth_plane);
              if (r->depth.attachment) f(r->depth.attachment, r->depth.load_action, r->depth.store_action, uint16_t(1u << 8), 0u);
              if (r->stencil.attachment)
                f(r->stencil.attachment, r->stencil.load_action, r->stencil.store_action, uint16_t(1u << 9), 0u);
            };
            each([&](const TextureViewRef &v, WMTLoadAction load, WMTStoreAction, uint16_t, uint32_t) {
              auto it = pending.find(AttachmentObject(v));
              if (it == pending.end()) return;
              if (load == WMTLoadActionDontCare)   // a discard at recording: the earlier stores are never observed
                for (auto &st : it->second) dead_stores[st.pass] |= st.bit;
              pending.erase(it);
            });
            if (resolve || !r->visibility_targets.empty()) break;   // keep these simple: their stores stay
            each([&](const TextureViewRef &v, WMTLoadAction, WMTStoreAction store, uint16_t bit, uint32_t plane) {
              if (store == WMTStoreActionStore) pending[AttachmentObject(v)].push_back({r, bit, v.texture().handle, plane});
            });
            break;
          }
          case EncoderType::Clear: {
            auto c = static_cast<ClearEncoderData *>(pass);
            auto it = pending.find(AttachmentObject(c->attachment));
            if (it == pending.end()) break;
            const auto view = c->attachment.texture().handle;
            const uint16_t planes = c->clear_dsv ? uint16_t(((c->clear_dsv & 1) ? 1u << 8 : 0u) | ((c->clear_dsv & 2) ? 1u << 9 : 0u))
                                                 : uint16_t(0xff);
            auto &stored = it->second;
            for (size_t k = 0; k < stored.size();) {
              if (stored[k].view == view && (stored[k].bit & planes) && stored[k].plane == (c->clear_dsv ? 0u : c->depth_plane)) {
                dead_stores[stored[k].pass] |= stored[k].bit;
                stored.erase(stored.begin() + k);
              } else {
                ++k;
              }
            }
            if (stored.empty()) pending.erase(it);
            break;
          }
          case EncoderType::Blit: {
            auto b = static_cast<BlitEncoderData *>(pass);
            if (b->barrier_only && b->barrier_records_valid) {
              bool all = false;
              for (const auto &rec : b->barrier_records) {
                if (!rec.object || rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) all = true;
                else pending.erase(rec.object);
              }
              if (all) pending.clear();
            } else {
              pending.clear();
            }
            break;
          }
          default:
            pending.clear();   // compute, resolve, argument, scene builds: possible readers
            break;
          }
        }
    }
    WMT::RenderCommandEncoder open_render{};      // Metal encoder kept open across recorded passes (merge only)
    bool render_open = false;
    RenderEncoderData *last_render = nullptr;     // last recorded pass with nothing encoded after it
    struct PendingClear {
      ClearEncoderData *data;
      const void *list;
    };
    std::vector<PendingClear> pending_clears;     // full clears not yet encoded (fold only)
    std::vector<ClearEncoderData *> virtual_clears; // counters only: clears a following pass could have absorbed
    unsigned previous_plain_kind = 0; // diagnostics only; reset by every barrier or other pass
    const auto close_open_render = [&] {
      if (!render_open)
        return;
      open_render.updateFence(state_->fence_, WMTRenderStageFragment);
      open_render.endEncoding();
      render_open = false;
    };
    // c17: why a clear becomes its own encoder (reasons for MACRUNNER_DX12_C17_STATS).
    enum : uint8_t { ClearWhyDirect, ClearWhyBarrier, ClearWhyResolve, ClearWhyUnfoldable, ClearWhyOrder, ClearWhyBreak,
                     ClearWhyScopeEnd, ClearWhyPrefix };
    const auto encode_clear_pass = [&](ClearEncoderData *data, const void *list, uint8_t why = ClearWhyDirect) {
      if (c17_stats) {
        auto &cs = c17::Get();
        switch (why) {
        case ClearWhyBarrier: c17::Note(cs.clear_enc_barrier); break;
        case ClearWhyResolve: c17::Note(cs.clear_enc_resolve); break;
        case ClearWhyUnfoldable: c17::Note(cs.clear_enc_unfoldable); break;
        case ClearWhyOrder: c17::Note(cs.clear_enc_order); break;
        case ClearWhyBreak: c17::Note(cs.clear_enc_break); break;
        case ClearWhyScopeEnd: c17::Note(cs.clear_enc_scope_end); break;
        case ClearWhyPrefix: c17::Note(cs.clear_enc_prefix); break;
        default: c17::Note(cs.clear_enc_direct); break;
        }
      }
      if (render_open) {   // ENCODERS step 4: a carried clear can be encoded while a merged encoder is open
        close_open_render();
        last_render = nullptr;
      }
      audit_clear_object(AttachmentObject(data->attachment));
      if (audit) encoder_stats::Note(encoder_stats::Get().att_clear);
      WMTRenderPassInfo info;
      WMT::InitializeRenderPassInfo(info);
      if (data->clear_dsv) {
        if (data->clear_dsv & 1) {
          info.depth.clear_depth = data->depth_stencil.first;
          info.depth.texture = data->attachment.texture();
          info.depth.load_action = WMTLoadActionClear;
          info.depth.store_action = WMTStoreActionStore;
          info.depth.depth_plane = data->depth_plane;
        }
        if (data->clear_dsv & 2) {
          info.stencil.clear_stencil = data->depth_stencil.second;
          info.stencil.texture = data->attachment.texture();
          info.stencil.load_action = WMTLoadActionClear;
          info.stencil.store_action = WMTStoreActionStore;
          info.stencil.depth_plane = data->depth_plane;
        }
        info.render_target_width = data->width;
        info.render_target_height = data->height;
      } else {
        info.colors[0].clear_color = data->color;
        info.colors[0].texture = data->attachment.texture();
        info.colors[0].load_action = WMTLoadActionClear;
        info.colors[0].store_action = WMTStoreActionStore;
        info.colors[0].depth_plane = data->depth_plane;
      }
      info.render_target_array_length = data->array_length;
      CaptureRenderPass(scope, list, data, info);
      auto encoder = cmdbuf.renderCommandEncoder(info);
      encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
      encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
      encoder.updateFence(state_->fence_, WMTRenderStageFragment);
      encoder.endEncoding();
      g_pass_stats.clear_encoders.fetch_add(1, std::memory_order_relaxed);
      encoder_stats::Count(encoder_stats::KindClear, data->close_reason);
    };
    // Step 4: encode now the pending clears whose texture `touches` returns true for (order kept).
    const auto encode_pending_if = [&](auto &&touches, uint8_t why) {
      std::vector<PendingClear> keep;
      for (auto &pending : pending_clears) {
        if (touches(AttachmentObject(pending.data->attachment), *pending.data))
          encode_clear_pass(pending.data, pending.list, why);
        else
          keep.push_back(pending);
      }
      pending_clears.swap(keep);
    };
    // Anything that is not a mergeable draw pass ends the sequence: earlier draws and clears must be encoded first.
    // ENCODERS step 4: other passes cannot use a render target without a barrier, so pending clears may go on waiting.
    const auto break_sequence = [&](bool flush_clears = true) {
      close_open_render();
      if (flush_clears || !defer_clears) {
        for (auto &pending : pending_clears)
          encode_clear_pass(pending.data, pending.list, flush_clears ? ClearWhyScopeEnd : ClearWhyBreak);
        pending_clears.clear();
      }
      virtual_clears.clear();
      last_render = nullptr;
      barrier_since_render = false;
      relaxed_since_render = false;
      barrier_reason = ChainBarrierNone;
    };
    for (unsigned i = 0; i < Count; i++) {
      auto pCommandList = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
      if (!early) pipeline_timing::Get().Emit(pipeline_timing::ExecuteList, pipeline_timing::Point, state_.get(),
          scope.seq, pipeline_timing::Pointer(pCommandList), i, Count, pCommandList->GetRecordingTraceEpoch());
      if (FullFrameCaptureEnabled() && TransferWindowEnabled()) {
        const auto frame = capture_next_present.load(std::memory_order_relaxed);
        const auto &recording = pCommandList->GetRecordingTrace();
        TraceTransferAt(scope, pCommandList, nullptr, frame, "recording.begin", "epoch=%llu count=%llu dropped=%llu",
                        (unsigned long long)pCommandList->GetRecordingTraceEpoch(), (unsigned long long)recording.size(),
                        (unsigned long long)pCommandList->GetRecordingTraceDropped());
        for (const auto &detail : recording)
          TraceTransferAt(scope, pCommandList, nullptr, frame, "recording.call", "%s", detail.c_str());
        TraceTransferAt(scope, pCommandList, nullptr, frame, "recording.end", "epoch=%llu count=%llu truncated=%u",
                        (unsigned long long)pCommandList->GetRecordingTraceEpoch(), (unsigned long long)recording.size(),
                        unsigned(pCommandList->GetRecordingTraceDropped() != 0));
      }
      EncoderData *current = pCommandList->entry;
      encoder_stats::SeqAppend(i ? "|" : "/");
      while (current) {
        TraceMergePass(scope, pCommandList, current);
        if (pass_stats && current->type != EncoderType::Null) {
          unsigned plain_kind = 0;
          if (current->type == EncoderType::Blit) {
            auto data = static_cast<BlitEncoderData *>(current);
            if (!data->barrier_only && !data->timestamp_samples && !data->resolve_samples &&
                !data->wait_for_prior_work && !data->immediate_out && !data->join_immediate_out)
              plain_kind = 1;
          } else if (current->type == EncoderType::Compute &&
                     !static_cast<ComputeEncoderData *>(current)->query_pipeline) {
            plain_kind = 2;
          }
          if (plain_kind && previous_plain_kind == plain_kind) {
            auto &counter = plain_kind == 1 ? g_pass_stats.adjacent_plain_blit : g_pass_stats.adjacent_plain_compute;
            counter.fetch_add(1, std::memory_order_relaxed);
          }
          previous_plain_kind = plain_kind;
        }
        if (scope.inflight.fault_capture)
          scope.inflight.fault_capture->Add(FaultContext(pCommandList, current), "pass.begin", nullptr, 0);
        TraceTransfer(scope, pCommandList, current, "pass.dispatch", "type=%u", unsigned(current->type));
        switch (current->type) {
        case EncoderType::Null:
          break;
        case EncoderType::Clear: {
          auto data = static_cast<ClearEncoderData *>(current);
          TraceTransfer(scope, pCommandList, current, "clear.target", "texture=%llx dsv=%u size=%u,%u",
                        (unsigned long long)data->attachment.texture().handle, data->clear_dsv, data->width, data->height);
          g_pass_stats.clear_recorded.fetch_add(1, std::memory_order_relaxed);
          // A clear takes effect after the draws already recorded, so it always ends the open pass.
          // ENCODERS step 4: unless it is carried (its texture is not used by the open pass): then the pass may go on.
          if (!(defer_clears && render_open && last_render &&
                !PassUsesObject(*last_render, AttachmentObject(data->attachment)))) {
            close_open_render();
            last_render = nullptr;
            barrier_since_render = false;
            relaxed_since_render = false;
            barrier_reason = ChainBarrierNone;
          }
          if (fold_clears) {
            pending_clears.push_back({data, pCommandList});
          } else {
            if (pass_stats)
              virtual_clears.push_back(data);
            encode_clear_pass(data, pCommandList);
          }
          break;
        }
        case EncoderType::Render: {
          auto data = static_cast<RenderEncoderData *>(current);
          if (attachment_audit::Enabled()) {
            const auto use = [&](const auto &a, unsigned slot) {
              if (a.attachment) attachment_audit::Use(AttachmentObject(a.attachment), uint64_t(a.attachment->key),
                  slot, a.level, a.slice, a.depth_plane);
            };
            for (unsigned slot=0;slot<data->colors.size();++slot) use(data->colors[slot],slot);
            use(data->depth,8); use(data->stencil,9);
          }
          g_pass_stats.render_recorded.fetch_add(1, std::memory_order_relaxed);
          // ENCODERS step 7: a pass counting direct queries may continue an encoder with the same visibility buffer.
          const bool plain_pass = merge_safe ? data->visibility_targets.empty()
                                             : data->visibility_targets.empty() && !data->visibility_direct;
          const auto pending_touch = [&] {
            for (const auto &pending : pending_clears)
              if (PassUsesObject(*data, AttachmentObject(pending.data->attachment)))
                return true;
            return false;
          };
          const auto visibility_fits = [&] {
            if (!merge_safe) return true;
            if (data->visibility_direct && data->visibility_direct.handle != open_visibility) return false;
            for (auto slot : data->direct_slots)
              if (std::find(open_slots.begin(), open_slots.end(), slot) != open_slots.end()) return false;
            return true;
          };
          merge_trace::Emit(state_.get(), scope.seq, pCommandList,
              uint64_t(reinterpret_cast<uintptr_t>(current)), "candidate",
              "previous=%llu plain=%u pending_clears=%llu same_attachments=%u barrier=%u merge_mode=%u",
              (unsigned long long)reinterpret_cast<uintptr_t>(last_render), unsigned(plain_pass),
              (unsigned long long)pending_clears.size(), unsigned(last_render && SameRenderTargets(*last_render, *data)),
              unsigned(barrier_since_render), merge_mode);
          // c17: a relaxed barrier was crossed, so neither the open encoder nor this pass may write memory.
          const bool writer_conflict = relaxed_since_render && (open_writes || data->may_write_memory);
          if (c17_stats && last_render && SameRenderTargets(*last_render, *data)) {
            // c17 reasons: why a pass with the same targets as the previous one does (not) continue its encoder.
            auto &cs = c17::Get();
            c17::Note(cs.merge_pairs);
            if (!plain_pass) c17::Note(cs.reject_not_plain);
            else if (!(pending_clears.empty() || (defer_clears && !pending_touch()))) c17::Note(cs.reject_pending_clear);
            else if (!visibility_fits()) c17::Note(cs.reject_visibility);
            else if (open_dead_mask) c17::Note(cs.reject_dead_store);
            else if (barrier_since_render && merge_mode != 2) {
              switch (merge_deps ? barrier_reason : uint8_t(0xff)) {
              case ChainBarrierAttachment: c17::Note(cs.reject_attachment); break;
              case ChainBarrierUnknownObject: c17::Note(cs.reject_unknown_object); break;
              case ChainBarrierRecordsInvalid: c17::Note(cs.reject_records_invalid); break;
              case ChainBarrierAliasingUnnamed: c17::Note(cs.reject_aliasing_unnamed); break;
              default: c17::Note(cs.reject_barrier_mode1); break;
              }
            } else if (writer_conflict) c17::Note(cs.reject_writer);
            else if (!render_open) c17::Note(cs.reject_not_open);
            else {
              c17::Note(cs.merge_merged);
              if (relaxed_since_render) c17::Note(cs.merge_merged_relaxed);
            }
          }
          if (last_render && plain_pass && (pending_clears.empty() || (defer_clears && !pending_touch())) &&
              SameRenderTargets(*last_render, *data) && visibility_fits() && !open_dead_mask) {
            g_pass_stats.render_mergeable.fetch_add(1, std::memory_order_relaxed);
            if (!barrier_since_render)
              g_pass_stats.render_mergeable_safe.fetch_add(1, std::memory_order_relaxed);
            else if (pass_stats)
              g_pass_stats.render_barrier_rejected.fetch_add(1, std::memory_order_relaxed);
            if (render_open && (merge_mode == 2 || !barrier_since_render) && !writer_conflict) {
              // Same attachments and nothing encoded in between: continue in the open Metal encoder.
              // Every recorded pass re-emits its complete state, so the command chains can simply follow each other.
              g_pass_stats.render_merged.fetch_add(1, std::memory_order_relaxed);
              encoder_stats::Note(encoder_stats::Get().merged_render);
              if (empty_plan.heads.count(data)) c17::Note(c17::Get().empty_merged);
              if (crossed_safe_barrier)
                encoder_stats::Note(encoder_stats::Get().merged_safe);
              crossed_safe_barrier = false;
              open_slots.insert(open_slots.end(), data->direct_slots.begin(), data->direct_slots.end());
              if (audit) AuditRenderCommands(reinterpret_cast<const wmtcmd_base *>(&data->cmd_head), state_audit);
              TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
              open_render.encodeCommands(&data->cmd_head);
              last_render = data;
              barrier_since_render = false;
              relaxed_since_render = false;
              barrier_reason = ChainBarrierNone;
              open_writes |= data->may_write_memory;
              virtual_clears.clear();
              break;
            }
          }
          close_open_render();
          WMTRenderPassInfo render_pass_info;
          WMT::InitializeRenderPassInfo(render_pass_info);
          if (!data->visibility_targets.empty()) {
            break_sequence(false);   // pending clears of this pass's targets are handled below
            wmtcmd_blit_nop head{};
            wmtcmd_blit_fillbuffer clear{};
            head.type = WMTBlitCommandNop;
            head.next.set(&clear);
            clear.type = WMTBlitCommandFillBuffer;
            clear.buffer = state_->visibility_scratch_;
            clear.length = sizeof(uint64_t);
            auto encoder = cmdbuf.blitCommandEncoder();
            if (pass_stats) g_pass_stats.service_occlusion_blit.fetch_add(1, std::memory_order_relaxed);
            encoder_stats::Count(encoder_stats::KindServiceOcclusionBlit, data->close_reason);
            encoder.waitForFence(state_->fence_);
            CaptureFaultCommands(scope, pCommandList, current, WMTCommandCaptureFamily::Blit,
                                 reinterpret_cast<wmtcmd_base *>(&head));
            encoder.encodeCommands(&head);
            encoder.updateFence(state_->fence_);
            encoder.endEncoding();
            render_pass_info.visibility_buffer = state_->visibility_scratch_;
          } else if (data->visibility_direct) {
            // ENCODERS: queries of this pass count straight into the query heap (offset = index * 8).
            render_pass_info.visibility_buffer = data->visibility_direct;
          }
          {
            for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
              auto &color_data = data->colors[i];
              if (!color_data.attachment)
                continue;
              auto &color_info = render_pass_info.colors[i];
              color_info.texture = color_data.attachment.texture();
              color_info.load_action = color_data.load_action;
              color_info.store_action = color_data.store_action;
              color_info.level = color_data.level;
              color_info.slice = color_data.slice;
              color_info.depth_plane = color_data.depth_plane;
              color_info.clear_color = color_data.clear_color;
              color_info.resolve_texture = color_data.resolve_attachment.texture();
              color_info.resolve_level = color_data.resolve_level;
              color_info.resolve_slice = color_data.resolve_slice;
              color_info.resolve_depth_plane = color_data.resolve_depth_plane;
              TraceTransfer(scope, pCommandList, current, "render.target", "slot=%u texture=%llx size=%u,%u load=%u store=%u",
                            i, (unsigned long long)color_data.attachment.texture().handle,
                            data->render_target_width, data->render_target_height,
                            unsigned(color_data.load_action), unsigned(color_data.store_action));
            }
            if (data->depth.attachment) {
              auto &depth_info = render_pass_info.depth;
              auto &depth_data = data->depth;
              depth_info.texture = depth_data.attachment.texture();
              depth_info.load_action = depth_data.load_action;
              depth_info.store_action = depth_data.store_action;
              depth_info.level = depth_data.level;
              depth_info.slice = depth_data.slice;
              depth_info.depth_plane = depth_data.depth_plane;
              depth_info.clear_depth = depth_data.clear_depth;
            }
            if (data->stencil.attachment) {
              auto &stencil_info = render_pass_info.stencil;
              auto &stencil_data = data->stencil;
              stencil_info.texture = stencil_data.attachment.texture();
              stencil_info.load_action = stencil_data.load_action;
              stencil_info.store_action = stencil_data.store_action;
              stencil_info.level = stencil_data.level;
              stencil_info.slice = stencil_data.slice;
              stencil_info.depth_plane = stencil_data.depth_plane;
              stencil_info.clear_stencil = stencil_data.clear_stencil;
            }
            render_pass_info.default_raster_sample_count = data->default_raster_sample_count;
            render_pass_info.render_target_array_length = data->render_target_array_length;
            render_pass_info.render_target_width = data->render_target_width;
            render_pass_info.render_target_height = data->render_target_height;
          }
          if (fold_clears && defer_clears && !pending_clears.empty()) {
            // ENCODERS step 4: clears of textures this pass does not use wait (carried); for the textures it uses, a clear
            // folds into the load action unless a later clear of the same texture cannot fold (then it is encoded first).
            const size_t n = pending_clears.size();
            std::vector<uint8_t> touch(n), fold(n);
            std::vector<obj_handle_t> object(n);
            for (size_t k = 0; k < n; ++k) {
              object[k] = AttachmentObject(pending_clears[k].data->attachment);
              touch[k] = PassUsesObject(*data, object[k]);
              fold[k] = touch[k] && CanFoldClear(*pending_clears[k].data, *data);
            }
            std::vector<uint8_t> foldable(fold);
            for (size_t k = 0; k < n; ++k)
              for (size_t m = k + 1; m < n && fold[k]; ++m)
                if (touch[m] && !fold[m] && object[m] == object[k] &&
                    (!clear_subresource || ClearsOverlap(*pending_clears[k].data, *pending_clears[m].data)))
                  fold[k] = 0;
            std::vector<PendingClear> carry;
            for (size_t k = 0; k < n; ++k) {
              if (!touch[k]) { carry.push_back(pending_clears[k]); continue; }
              if (!fold[k])
                encode_clear_pass(pending_clears[k].data, pending_clears[k].list,
                                  foldable[k] ? ClearWhyOrder : ClearWhyUnfoldable);
            }
            for (size_t k = 0; k < n; ++k)
              if (fold[k]) {
                FoldClear(*pending_clears[k].data, *data, render_pass_info);
                g_pass_stats.clear_folded.fetch_add(1, std::memory_order_relaxed);
                encoder_stats::Note(encoder_stats::Get().clears_folded_deferred);
              }
            encoder_stats::Note(encoder_stats::Get().clears_carried, carry.size());
            pending_clears.swap(carry);
          } else if (fold_clears && !pending_clears.empty()) {
            // Only a suffix may fold: a clear that cannot fold must still precede every later clear (views can alias).
            size_t first_fold = pending_clears.size();
            while (first_fold && CanFoldClear(*pending_clears[first_fold - 1].data, *data))
              first_fold--;
            for (size_t n = 0; n < first_fold; n++)
              encode_clear_pass(pending_clears[n].data, pending_clears[n].list, ClearWhyPrefix);
            for (size_t n = first_fold; n < pending_clears.size(); n++) {
              FoldClear(*pending_clears[n].data, *data, render_pass_info);
              g_pass_stats.clear_folded.fetch_add(1, std::memory_order_relaxed);
            }
            pending_clears.clear();
          } else if (!virtual_clears.empty()) {
            size_t first_fold = virtual_clears.size();
            while (first_fold && CanFoldClear(*virtual_clears[first_fold - 1], *data))
              first_fold--;
            g_pass_stats.clear_foldable.fetch_add(virtual_clears.size() - first_fold, std::memory_order_relaxed);
            virtual_clears.clear();
          }
          open_dead_mask = 0;
          if (!dead_stores.empty()) {
            if (auto it = dead_stores.find(data); it != dead_stores.end()) {
              open_dead_mask = it->second;
              const auto drop = [&](WMTStoreAction &store) {
                if (store != WMTStoreActionStore) return;
                store = WMTStoreActionDontCare;
                encoder_stats::Note(encoder_stats::Get().att_store_avoided);
              };
              for (unsigned k = 0; k < 8; ++k)
                if (it->second & (1u << k)) drop(render_pass_info.colors[k].store_action);
              if (it->second & (1u << 8)) drop(render_pass_info.depth.store_action);
              if (it->second & (1u << 9)) drop(render_pass_info.stencil.store_action);
            }
          }
          CaptureRenderPass(scope, pCommandList, current, render_pass_info);
          auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
          g_pass_stats.render_encoders.fetch_add(1, std::memory_order_relaxed);
          encoder_stats::Count(encoder_stats::KindRender, data->close_reason);
          open_visibility = render_pass_info.visibility_buffer;
          open_slots = data->direct_slots;
          crossed_safe_barrier = false;
          open_writes = data->may_write_memory;
          if (audit) {
            // Level-4 audit: attachment traffic of this encoder and its command stream.
            state_audit.slots.clear();
            auto &st = encoder_stats::Get();
            const auto account = [&](obj_handle_t object, WMTLoadAction load, WMTStoreAction store) {
              if (!object) return;
              (load == WMTLoadActionLoad ? st.att_load : load == WMTLoadActionClear ? st.att_clear : st.att_dontcare)
                  .fetch_add(1, std::memory_order_relaxed);
              if (load == WMTLoadActionLoad) stored_unread[object] = false;
              else if (load == WMTLoadActionClear) {
                auto it = stored_unread.find(object);
                if (it != stored_unread.end() && it->second) st.att_store_dead.fetch_add(1, std::memory_order_relaxed);
              }
              if (store != WMTStoreActionDontCare) {
                st.att_store.fetch_add(1, std::memory_order_relaxed);
                stored_unread[object] = true;
              }
            };
            for (unsigned k = 0; k < std::size(render_pass_info.colors); ++k)
              if (data->colors[k].attachment)
                account(AttachmentObject(data->colors[k].attachment), render_pass_info.colors[k].load_action,
                        render_pass_info.colors[k].store_action);
            if (data->depth.attachment)
              account(AttachmentObject(data->depth.attachment), render_pass_info.depth.load_action, render_pass_info.depth.store_action);
            if (data->stencil.attachment && (!data->depth.attachment ||
                AttachmentObject(data->stencil.attachment) != AttachmentObject(data->depth.attachment)))
              account(AttachmentObject(data->stencil.attachment), render_pass_info.stencil.load_action, render_pass_info.stencil.store_action);
            AuditRenderCommands(reinterpret_cast<const wmtcmd_base *>(&data->cmd_head), state_audit);
          }
          if (const unsigned fence_mode = RenderFenceMode(); fence_mode != 2)
            encoder.waitForFence(state_->fence_, fence_mode == 1 ? WMTRenderStageFragment : WMTRenderStageVertex);
          TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
          encoder.encodeCommands(&data->cmd_head);
          last_render = plain_pass ? data : nullptr;
          barrier_since_render = false;
          relaxed_since_render = false;
          barrier_reason = ChainBarrierNone;
          if (merge_passes && plain_pass) {
            // Leave the encoder open: the next recorded pass may continue it.
            open_render = encoder;
            render_open = true;
            break;
          }
          encoder.updateFence(state_->fence_, WMTRenderStageFragment);
          encoder.endEncoding();
          EncodeRenderReadback(scope, *data);
          if (!data->visibility_targets.empty()) {
            // All samples in this pass belong to the same active query set.
            // The scratch is queue-local and cannot be reused until reduction finishes.
            auto reduction = cmdbuf.computeCommandEncoder(false);
            if (pass_stats) g_pass_stats.service_occlusion_compute.fetch_add(1, std::memory_order_relaxed);
            encoder_stats::Count(encoder_stats::KindServiceOcclusionCompute, data->close_reason);
            reduction.waitForFence(state_->fence_);
            for (const auto &target : data->visibility_targets) {
              wmtcmd_compute_nop head{};
              wmtcmd_compute_setpso pipeline{};
              wmtcmd_compute_setbuffer src{}, dst{};
              wmtcmd_compute_dispatch dispatch{};
              head.type = WMTComputeCommandNop;
              head.next.set(&pipeline);
              pipeline.type = WMTComputeCommandSetPSO;
              pipeline.next.set(&src);
              pipeline.pso = data->visibility_accumulate;
              pipeline.threadgroup_size = {1, 1, 1};
              src.type = WMTComputeCommandSetBuffer;
              src.next.set(&dst);
              src.buffer = state_->visibility_scratch_;
              src.index = 0;
              dst.type = WMTComputeCommandSetBuffer;
              dst.next.set(&dispatch);
              dst.buffer = target.results;
              dst.offset = target.offset;
              dst.index = 1;
              dispatch.type = WMTComputeCommandDispatch;
              dispatch.size = {1, 1, 1};
              CaptureFaultCommands(scope, pCommandList, current, WMTCommandCaptureFamily::Compute,
                                   reinterpret_cast<wmtcmd_base *>(&head));
              reduction.encodeCommands(&head);
            }
            reduction.updateFence(state_->fence_);
            reduction.endEncoding();
          }
          break;
        }
        case EncoderType::Blit: {
          auto data = static_cast<BlitEncoderData *>(current);
          if (data->barrier_only && !out_pending) {
            TraceTransfer(scope, pCommandList, current, "blit.skip", "barrier_only=%u", 1u);
            g_pass_stats.blit_skipped.fetch_add(1, std::memory_order_relaxed);
            encoder_stats::Note(encoder_stats::Get().skipped_barrier_blits);
            // ENCODERS step 7: a barrier that touches none of the open pass's attachments (and no UAV / stream-out state)
            // does not stop the merge.
            if (last_render && empty_plan.barriers.count(data)) {
              crossed_safe_barrier = true;
            } else if (merge_deps && last_render) {
              // c17 MERGE_DEPENDENCIES: crossed unless it concerns the chain's attachments or names no object.
              const auto verdict = ClassifyChainBarrier(*data, *last_render);
              if (verdict.reject) {
                barrier_since_render = true;
                if (!barrier_reason) barrier_reason = verdict.reject;
              } else {
                crossed_safe_barrier = true;
                relaxed_since_render |= verdict.relaxed;
                if (c17_stats) {
                  auto &cs = c17::Get();
                  if (verdict.plain) c17::Note(cs.cross_plain);
                  if (verdict.uav_state) c17::Note(cs.cross_uav_state);
                  if (verdict.uav_barrier) c17::Note(cs.cross_uav_barrier);
                  if (verdict.aliasing) c17::Note(cs.cross_aliasing);
                }
              }
            } else if (merge_safe && last_render && !BarrierTouchesPass(*data, *last_render))
              crossed_safe_barrier = true;
            else
              barrier_since_render = true;   // the open pass stays open; mode 1 will not continue it past this point
            if (audit) {
              if (!data->barrier_records_valid) stored_unread.clear();
              else for (const auto &rec : data->barrier_records)
                if (!rec.object || rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) stored_unread.clear();
                else stored_unread[rec.object] = false;   // may be read after this barrier
            }
            // A barrier may make a cleared attachment readable by the next pass (read-only depth sampled in the shader):
            // a clear that is still pending must reach memory first, so it cannot become that pass's load action.
            // ENCODERS step 4: only the pending clears of the textures this barrier is about.
            if (!pending_clears.empty()) {
              if (defer_clears)
                encode_pending_if([&](obj_handle_t object, const ClearEncoderData &clear) {
                  return clear_subresource ? BarrierTouchesClear(*data, clear) : BarrierTouchesObject(*data, object);
                }, ClearWhyBarrier);
              else {
                for (auto &pending : pending_clears)
                  encode_clear_pass(pending.data, pending.list, ClearWhyBarrier);
                pending_clears.clear();
              }
            }
            virtual_clears.clear();
            break;
          }
          if (defer_clears && data->barrier_only && !pending_clears.empty())
            encode_pending_if([&](obj_handle_t object, const ClearEncoderData &clear) {
              return clear_subresource ? BarrierTouchesClear(*data, clear) : BarrierTouchesObject(*data, object);
            }, ClearWhyBarrier);
          break_sequence(false);
          g_pass_stats.other_encoders.fetch_add(1, std::memory_order_relaxed);
          // Counter samples use queue-wide ordering. Completing prior work
          if (pass_stats) {
            g_pass_stats.other_blit.fetch_add(1, std::memory_order_relaxed);
            if (data->timestamp_samples) g_pass_stats.blit_timestamp.fetch_add(1, std::memory_order_relaxed);
            if (data->resolve_samples) g_pass_stats.blit_counter_resolve.fetch_add(1, std::memory_order_relaxed);
            if (data->barrier_only) g_pass_stats.blit_barrier_join.fetch_add(1, std::memory_order_relaxed);
          }
          // also satisfies the stronger-than-required MARKER_IN barrier.
          const auto timestamp_barrier = [&] {
            const auto value = ++state_->timestamp_serial_;
            cmdbuf.encodeSignalEvent(state_->timestamp_event_, value);
            cmdbuf.encodeWaitForEvent(state_->timestamp_event_, value);
          };
          if (data->timestamp_samples || data->wait_for_prior_work)
            timestamp_barrier();
          WMTSampleBufferAttachmentInfo sample{};
          sample.sample_buffer = data->timestamp_samples;
          sample.start_of_encoder_sample_index = ~0ull;
          sample.end_of_encoder_sample_index = data->timestamp_index;
          auto encoder = data->timestamp_samples ? cmdbuf.blitCommandEncoderWithSampleBuffers(&sample, 1)
                                                : cmdbuf.blitCommandEncoder();
          encoder_stats::Count(encoder_stats::KindBlit, data->close_reason);
          encoder.waitForFence(state_->fence_);
          if (data->join_immediate_out && out_pending)
            encoder.waitForFence(state_->immediate_out_fence_);
          TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
          encoder.encodeCommands(&data->cmd_head);
          if (data->resolve_samples) {
            // Metal resolve offsets have stricter alignment than D3D12's 8 bytes.
            // Allocate per execution so concurrent replays never share scratch.
            WMTBufferInfo info{};
            info.length = uint64_t(data->resolve_count) * sizeof(uint64_t);
            info.options = WMTResourceStorageModePrivate;
            auto scratch = state_->device_.newBuffer(info);
            if (!scratch) {
              encoder.endEncoding();
              device_->RemoveDevice();
              return;
            }
            scope.inflight.timestamp_scratch.push_back(scratch);
            encoder.resolveCounters(data->resolve_samples, data->resolve_start, data->resolve_count, scratch, 0);
            wmtcmd_blit_copy_from_buffer_to_buffer copy{};
            copy.type = WMTBlitCommandCopyFromBufferToBuffer;
            copy.next.set(nullptr);
            copy.src = scratch;
            copy.dst = data->resolve_destination;
            copy.dst_offset = data->resolve_offset;
            copy.copy_length = info.length;
            CaptureFaultCommands(scope, pCommandList, current, WMTCommandCaptureFamily::Blit,
                                 reinterpret_cast<wmtcmd_base *>(&copy));
            MTLBlitCommandEncoder_encodeCommands(encoder, reinterpret_cast<wmtcmd_base *>(&copy));
          }
          if (data->immediate_out) {
            // Do not put OUT on the common fence: independent later passes
            // must not acquire a dependency on this deferred write.
            encoder.updateFence(state_->immediate_out_fence_);
            out_pending = true;
          } else {
            encoder.updateFence(state_->fence_);
            if (data->join_immediate_out)
              out_pending = false;
          }
          encoder.endEncoding();
          if (data->timestamp_samples)
            timestamp_barrier();
          break;
        }
        case EncoderType::Compute: {
          break_sequence(false);
          g_pass_stats.other_encoders.fetch_add(1, std::memory_order_relaxed);
          auto data = static_cast<ComputeEncoderData *>(current);
          if (pass_stats) {
            g_pass_stats.other_compute.fetch_add(1, std::memory_order_relaxed);
            if (data->query_pipeline) g_pass_stats.compute_query_resolve.fetch_add(1, std::memory_order_relaxed);
          }
          auto encoder = cmdbuf.computeCommandEncoder(false);
          encoder_stats::Count(encoder_stats::KindCompute, data->close_reason);
          encoder.waitForFence(state_->fence_);
          TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(state_->fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::AccelerationBuild: {
          break_sequence(false);
          g_pass_stats.other_encoders.fetch_add(1, std::memory_order_relaxed);
          if (pass_stats) g_pass_stats.other_acceleration.fetch_add(1, std::memory_order_relaxed);
          encoder_stats::Count(encoder_stats::KindAcceleration, current->close_reason);
          auto &build=scope.inflight.acceleration_builds[acceleration_index++];
          const auto status=cmdbuf.buildAccelerationStructure(build.Descriptor(),build.result->Native().resource,
              build.scratch->buffer,0,state_->fence_);
          if (status!=WMTArgumentStatusReady) {
            scope.discard=true;
            ERR("ExecuteCommandLists: scene build encoding failed");
            device_->RemoveDevice();return;
          }
          break;
        }
        case EncoderType::ArgumentCompute: {
          break_sequence(false);
          g_pass_stats.other_encoders.fetch_add(1, std::memory_order_relaxed);
          if (pass_stats) g_pass_stats.other_argument_compute.fetch_add(1, std::memory_order_relaxed);
          encoder_stats::Count(encoder_stats::KindArgumentCompute, current->close_reason);
          auto &dispatch = scope.inflight.argument_dispatches[argument_index++];
          wmtcmd_compute_nop head{};
          head.type = WMTComputeCommandNop;
          wmtcmd_compute_setpso pipeline{};
          head.next.set(&pipeline);
          pipeline.type = WMTComputeCommandSetPSO;
          pipeline.pso = dispatch.pipeline;
          pipeline.threadgroup_size = dispatch.threads;
          std::array<wmtcmd_compute_setbuffer, 31> buffers{};
          std::array<wmtcmd_compute_settexture, 128> textures{};
          std::array<wmtcmd_compute_setaccelerationstructure, 31> scenes{};
          size_t buffer_count = 0, texture_count = 0, scene_count = 0;
          for (size_t i = 0; i < dispatch.buffers.size(); ++i) {
            auto &b = buffers[buffer_count++];
            b.type = WMTComputeCommandSetBuffer;
            b.buffer = dispatch.buffers[i]->buffer;
            b.index = uint8_t(dispatch.indices[i]);
          }
          for (const auto &binding : dispatch.direct_bindings) {
            if (binding.kind == WMTArgumentKindBuffer) {
              auto &b = buffers[buffer_count++];
              b.type = WMTComputeCommandSetBuffer;
              b.buffer = binding.resource;
              b.offset = binding.offset;
              b.index = uint8_t(binding.index);
            } else if (metal_argument::Acceleration(binding.kind)) {
              auto &a = scenes[scene_count++];
              a.type = WMTComputeCommandSetAccelerationStructure;
              a.acceleration_structure = binding.resource;
              a.index = uint8_t(binding.index);
            } else {
              auto &t = textures[texture_count++];
              t.type = WMTComputeCommandSetTexture;
              t.texture = binding.resource;
              t.index = uint8_t(binding.index);
            }
          }
          wmtcmd_compute_dispatch launch{};
          launch.type = WMTComputeCommandDispatch;
          launch.size = dispatch.groups;
          compute_commands::Link(pipeline.next, dispatch.resource_uses,
              std::span(buffers.data(), buffer_count), std::span(textures.data(), texture_count), launch,
              std::span(scenes.data(), scene_count));
          auto encoder = cmdbuf.computeCommandEncoder(false);
          encoder.waitForFence(state_->fence_);
          CaptureFaultCommands(scope, pCommandList, current, WMTCommandCaptureFamily::Compute,
                               reinterpret_cast<wmtcmd_base *>(&head));
          encoder.encodeCommands(&head);
          encoder.updateFence(state_->fence_);
          encoder.endEncoding();
          break;
        }
        case EncoderType::Resolve: {
          if (defer_clears && !pending_clears.empty()) {
            auto r = static_cast<ResolveEncoderData *>(current);
            const auto src = AttachmentObject(r->src), dst = AttachmentObject(r->dst);
            encode_pending_if([&](obj_handle_t object, const ClearEncoderData &) { return object == src || object == dst; },
                              ClearWhyResolve);
          }
          break_sequence(false);
          g_pass_stats.other_encoders.fetch_add(1, std::memory_order_relaxed);
          if (pass_stats) g_pass_stats.other_resolve.fetch_add(1, std::memory_order_relaxed);
          encoder_stats::Count(encoder_stats::KindResolve, current->close_reason);
          auto data = static_cast<ResolveEncoderData *>(current);
          TraceTransfer(scope, pCommandList, current, "resolve", "src=%llx dst=%llx",
                        (unsigned long long)data->src.texture().handle, (unsigned long long)data->dst.texture().handle);

          WMTRenderPassInfo info;
          WMT::InitializeRenderPassInfo(info);
          info.colors[0].texture = data->src.texture();
          info.colors[0].load_action = WMTLoadActionLoad;
          info.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
          info.colors[0].resolve_texture = data->dst.texture();

          CaptureRenderPass(scope, pCommandList, current, info);
          auto encoder = cmdbuf.renderCommandEncoder(info);
          encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
          encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
          encoder.updateFence(state_->fence_, WMTRenderStageFragment);
          encoder.endEncoding();

          break;
        }
        }
        current = current->next;
      }
      g_pass_stats.lists.fetch_add(1, std::memory_order_relaxed);
    }
    break_sequence(true);
    merge_trace::Emit(state_.get(), scope.seq, nullptr, 0, "submit.end", "lists=%u", Count);
    if (!early) PassStatsReport();
    if (!early && out_pending) {
      // Separate ExecuteCommandLists calls are ordered even when the next
      // submission relies on buffer decay/promotion instead of a barrier.
      auto encoder = cmdbuf.blitCommandEncoder();
      if (pass_stats) g_pass_stats.service_out_join_blit.fetch_add(1, std::memory_order_relaxed);
      encoder_stats::Count(encoder_stats::KindServiceOutJoin, encoder_stats::ReasonUnknown);
      encoder.waitForFence(state_->fence_);
      encoder.waitForFence(state_->immediate_out_fence_);
      encoder.updateFence(state_->fence_);
      encoder.endEncoding();
      out_pending = false;
    }
    if (audit) {
      LARGE_INTEGER t1{}, hz{};
      QueryPerformanceCounter(&t1);
      QueryPerformanceFrequency(&hz);
      encoder_stats::Get().qpc_hz.store(uint64_t(hz.QuadPart), std::memory_order_relaxed);
      encoder_stats::Note(early ? encoder_stats::Get().early_ticks : encoder_stats::Get().execute_ticks,
                          uint64_t(t1.QuadPart - execute_t0.QuadPart));
    }
  };

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) {};

  void STDMETHODCALLTYPE EndEvent() {};

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D12Fence *pFence, UINT64 Value) {
    pipeline_timing::Scope pipeline(pipeline_timing::QueueSignal, state_.get(),
                                   pipeline_timing::Pointer(pFence), Value);
    if (!pFence)
      return E_INVALIDARG;
    const HRESULT hr = device_->GetDeviceRemovedReason();
    if (FAILED(hr))
      return hr;
    if (DeferSubmission()) {
      std::lock_guard order(submission_->producer_mutex);
      auto fence = static_cast<MTLD3D12Fence *>(pFence)->GetRemovalState();
      WMT::Reference<WMT::SharedEvent> event;
      const auto prepared = fence->PrepareNativeSignal(Value, event);
      if (FAILED(prepared)) return pipeline.Result(prepared);
      return pipeline.Result(SubmitCPU([this, fence, event, Value] {
        if (FAILED(device_->GetDeviceRemovedReason())) return;
        auto scope = StartCommitting();
        if (event) scope.inflight.cmdbuf.encodeSignalEvent(event, Value);
        scope.inflight.retained_fence = fence;
        scope.inflight.publishes_fence = fence->UsesOrderedPublication();
        scope.inflight.fence_value = Value;
      }));
    }
    if (fence_publication::Enabled()) {
      // The commit lock excludes a concurrent enqueue; acquire pairs with the
      // retirement worker's release after all prior event publications.
      std::lock_guard<dxmt::mutex> lock(state_->mutex_commit_);
      if (state_->inflight_cmdbuf_count_.load(std::memory_order_acquire) == 0) {
        const HRESULT immediate_hr = pFence->Signal(Value);
        if (SUCCEEDED(immediate_hr))
          fence_publication::Stats().idle_signals.fetch_add(1, std::memory_order_relaxed);
        return pipeline.Result(immediate_hr);
      }
    }
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight.cmdbuf;
    scope.inflight.retained_fence = static_cast<MTLD3D12Fence *>(pFence)->GetRemovalState();
    pipeline_timing::Get().Emit(pipeline_timing::QueueIdentity, pipeline_timing::Point, this,
        scope.seq, pipeline_timing::Pointer(state_.get()), pipeline_timing::Pointer(pFence), Value, cmdbuf.handle);
    const HRESULT signal_hr = static_cast<MTLD3D12Fence *>(pFence)->SignalQueue(cmdbuf, Value);
    scope.inflight.publishes_fence = SUCCEEDED(signal_hr) && scope.inflight.retained_fence->UsesOrderedPublication();
    scope.inflight.fence_value = Value;
    return pipeline.Result(signal_hr);
  };

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D12Fence *pFence, UINT64 Value) {
    pipeline_timing::Scope pipeline(pipeline_timing::QueueWait, state_.get(),
                                   pipeline_timing::Pointer(pFence), Value);
    if (!pFence)
      return E_INVALIDARG;
    const HRESULT hr = device_->GetDeviceRemovedReason();
    if (FAILED(hr))
      return hr;
    if (DeferSubmission()) {
      std::lock_guard order(submission_->producer_mutex);
      struct PreparedWait {
        WMT::Reference<WMT::SharedEvent> event;
        std::unique_ptr<D3D12FencePublicationWait> publication;
      };
      auto wait = std::make_shared<PreparedWait>();
      auto fence = static_cast<MTLD3D12Fence *>(pFence)->GetRemovalState();
      const auto prepared = fence->PrepareNativeWait(Value, wait->event, wait->publication);
      if (FAILED(prepared)) return pipeline.Result(prepared);
      return pipeline.Result(SubmitCPU([this, fence, wait, Value] {
        // Do not park a Metal command buffer behind an unsatisfied CPU fence: on Apple GPUs a
        // prolonged event wait can trip the GPU watchdog. Earlier FIFO work is already committed;
        // this private submitter may wait while every public queue operation remains nonblocking.
        while (!wait->event.waitUntilSignaledValue(Value, 100))
          if (FAILED(device_->GetDeviceRemovedReason())) return;
        if (FAILED(device_->GetDeviceRemovedReason())) return;
        auto scope = StartCommitting();
        scope.inflight.cmdbuf.encodeWaitForEvent(wait->event, Value);
        scope.inflight.retained_fence = fence;
        scope.inflight.fence_wait = std::move(wait->publication);
      }));
    }
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight.cmdbuf;
    scope.inflight.retained_fence = static_cast<MTLD3D12Fence *>(pFence)->GetRemovalState();
    const HRESULT publication_hr = scope.inflight.retained_fence->PrepareQueueWait(Value, scope.inflight.fence_wait);
    if (FAILED(publication_hr)) {
      scope.discard = true;
      return pipeline.Result(publication_hr);
    }
    pipeline_timing::Get().Emit(pipeline_timing::QueueIdentity, pipeline_timing::Point, this,
        scope.seq, pipeline_timing::Pointer(state_.get()), pipeline_timing::Pointer(pFence), Value, cmdbuf.handle);
    return pipeline.Result(static_cast<MTLD3D12Fence *>(pFence)->WaitQueue(cmdbuf, Value));
  };

  HRESULT STDMETHODCALLTYPE
  GetTimestampFrequency(UINT64 *pFrequency) {
    if (!pFrequency)
      return E_INVALIDARG;
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return E_FAIL;
    // Apple GPU timestamp scale; validated by the native clock/readback probe.
    *pFrequency = 1000000000ull;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) {
    return E_NOTIMPL;
  };

  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
  ) {
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  HRESULT
  Present(const std::shared_ptr<D3D12PresentState> &present, ID3D12Resource *backbuffer, double after) override {
    if (!submission_ || DeferSubmission()) pso_work::Present();
    if (DeferSubmission()) {
      // Present is allowed to wait, but its wait never holds the FIFO producer lock.
      HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!done) return HRESULT_FROM_WIN32(GetLastError());
      HRESULT result = E_FAIL;
      std::unique_lock order(submission_->producer_mutex);
      const auto queued = SubmitCPU([this, present, backbuffer, after, done, &result] {
        try { result = Present(present, backbuffer, after); }
        catch (...) { device_->RemoveDevice(); }
        SetEvent(done);
      });
      order.unlock();
      if (SUCCEEDED(queued)) WaitForSingleObject(done, INFINITE);
      CloseHandle(done);
      return FAILED(queued) ? queued : result;
    }
    pipeline_timing::Scope pipeline(pipeline_timing::QueuePresent, state_.get(),
        pipeline_timing::Pointer(present.get()), pipeline_timing::Pointer(this));
    static std::atomic<uint32_t> trace_enter{0}, trace_encoded{0};
    TraceFrame(trace_enter, "queue_present.enter", this, "type=%u state=%p backbuffer=%p after=%.6f",
               unsigned(desc_.Type), static_cast<const void *>(state_.get()), static_cast<const void *>(backbuffer), after);
    LARGE_INTEGER present_t0{};
    if (encoder_stats::Enabled()) QueryPerformanceCounter(&present_t0);
    auto scope = StartCommitting();
    auto &cmdbuf = scope.inflight.cmdbuf;

    auto g = reinterpret_cast<MTLD3D12Resource *>(backbuffer);
    auto &view = g->texture->view(g->texture->fullView);
    static std::atomic<uint32_t> trace_texture{0};
    TraceFrame(trace_texture, "present.texture", this, "texture=%llx resource=%p",
               static_cast<unsigned long long>(view.texture.handle), static_cast<const void *>(backbuffer));
    scope.inflight.present = present;
    scope.inflight.present_texture = view.texture;
    TraceTransfer(scope, nullptr, nullptr, "present", "texture=%llx size=%llu,%llu",
                  (unsigned long long)view.texture.handle, (unsigned long long)view.texture.width(),
                  (unsigned long long)view.texture.height());
    merge_trace::Emit(state_.get(), 0, nullptr, 0, "frame.end", "coverage=RECORDED_BINDINGS_ONLY");
    if (merge_trace::Target()) merge_trace::next_present.fetch_add(1, std::memory_order_relaxed);
    if (FullFrameCaptureEnabled()) capture_next_present.fetch_add(1, std::memory_order_relaxed);
    EncodeFrameReadback(scope, g, view);
    auto presenter = present->presenter.ptr();

    auto state = presenter->synchronizeLayerProperties();
    auto drawable = presenter->encodeCommands(
        cmdbuf, view.texture, state.metadata,
        [&](auto encoder) {
          if (PassStatsEnabled()) g_pass_stats.service_present_render.fetch_add(1, std::memory_order_relaxed);
          encoder_stats::Count(encoder_stats::KindServicePresent, encoder_stats::ReasonUnknown);
          encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
        },
        [&](auto encoder) { encoder.updateFence(state_->fence_, WMTRenderStageFragment); }
    );

    if (after > 0)
      cmdbuf.presentDrawableAfterMinimumDuration(drawable, after);
    else
      cmdbuf.presentDrawable(drawable);
    if (PassStatsEnabled()) g_pass_stats.presents_encoded.fetch_add(1, std::memory_order_relaxed);
    if (encoder_stats::Enabled()) {
      LARGE_INTEGER t1{};
      QueryPerformanceCounter(&t1);
      encoder_stats::Note(encoder_stats::Get().present_ticks, uint64_t(t1.QuadPart - present_t0.QuadPart));
    }
    encoder_stats::Presented();
    c17::Presented();
    shader_source::stats::Tick(); // MACRUNNER_DX12_OWN_STATS: the counters line is due on a frame as well
    // Commit occurs in scope's destructor; Present1 logs the result after that return.
    TraceFrame(trace_encoded, "queue_present.encoded", this, "seq=%llu hr=%08x",
               static_cast<unsigned long long>(scope.seq), unsigned(S_OK));
    return S_OK;
  }
};

// ENCODERS 4.1: called by Close() on the recording thread. Encodes the list for the queue of its type on this device
// (with several: the one that executed lists most recently); a wrong guess is dropped at ExecuteCommandLists.
void
D3D12EarlyEncode(MTLD3D12Device *device, D3D12_COMMAND_LIST_TYPE type, MTLD3D12GraphicsCommandList *list) {
  if (!encoder_stats::EarlyEncode()) return;
  MTLD3D12CommandQueueImpl *queue = nullptr;
  std::shared_ptr<D3D12CommandQueueState> state;
  unsigned matches = 0;
  {
    std::lock_guard<dxmt::mutex> lock(EarlyRegistryLock());
    uint64_t best = 0;
    for (auto &e : EarlyRegistry())
      if (e.device == device && e.type == type) {
        const uint64_t t = e.state->early_last_execute.load(std::memory_order_relaxed);
        if (!matches++ || t > best) { queue = e.queue; state = e.state; best = t; }
      }
    if (matches) state->early_users.fetch_add(1, std::memory_order_acq_rel);
  }
  if (!matches) {
    encoder_stats::Note(encoder_stats::Get().early_fallback_queue);
    return;
  }
  queue->EncodeEarly(list);
  state->early_users.fetch_sub(1, std::memory_order_acq_rel);
}

HRESULT
CreateCommandQueue(MTLD3D12Device *pDevice, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
  auto command_queue = Com(new MTLD3D12CommandQueueImpl(pDevice));
  HRESULT hr = command_queue->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  return command_queue->QueryInterface(riid, ppCommandQueue);
};

} // namespace dxmt
