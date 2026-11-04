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
#include "d3d12_layer_time.hpp"   // CPU-LATE 23.10: DXMT_LT scopes
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_reuse_commands.hpp"
#include "d3d12_pso_work.hpp"
#include "d3d12_fine_locks.hpp"
#include "d3d12_compute_commands.hpp"
#include "d3d12_frame_trace.hpp"
#include "d3d12_gpu_timing.hpp"
#include "d3d12_gpu_gaps.hpp"
#include "d3d12_pipeline_timing.hpp"
#include "d3d12_shader_source_stats.hpp"
#include "d3d12_encoder_stats.hpp"
#include "d3d12_c17_stats.hpp"
#include "d3d12_enc_census.hpp"
#include "d3d12_attachment_audit.hpp"
#include "d3d12_render_fence.hpp"   // VS-STAGE RENDER_FENCE=3
#include "d3d12_ts_nodrain.hpp"     // GPU-PASS TS_NODRAIN
#include "d3d12_typed_safe.hpp"     // GPU-PASS TYPED_SAFE
#include "d3d12_gpu4_overlap.hpp"    // GPU4 TYPED_PRECISE / CLEAR_SINK
#include "d3d12_light_rename.hpp"    // LIGHTRN LIGHT_RENAME
#include "d3d12_post_chain.hpp"   // POSTCS 03.11 MACRUNNER_DX12_POST_CHAIN
#include "d3d12_prerast_precise.hpp"   // GAPS PRERASTER_PRECISE
#include "d3d12_tess_census.hpp"   // GPU-PASS TESS_CENSUS
#include "d3d12_tess_schedule.hpp"
#include <unordered_set>
#include <cstdarg>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
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

// MACRUNNER_DX12_RENDER_FENCE (modes 1 and 2: timing diagnostics ONLY, not ordering-safe in general: a vertex stage may
// read data written by an earlier blit, compute or draw pass): 0 default = a draw pass waits for the queue-wide fence
// before its vertex stage; 1 = it waits before its fragment stage only; 2 = it does not wait at all (it still updates the
// fence); 3 = split fences (VS-STAGE 23.10.2026, d3d12_render_fence.hpp).
static unsigned
RenderFenceMode() {
  return render_fence::Mode();
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
// ENC-FIX BLIT_RUN: what a recorded blit pass reads and writes (resource handles as barrier records name them).
// False when a command is not a plain copy / fill / mip generation (then the pass is never batched).
static bool
BlitRunObjects(const BlitEncoderData &b, std::vector<obj_handle_t> &reads, std::vector<obj_handle_t> &writes) {
  for (auto cmd = static_cast<const wmtcmd_base *>(b.cmd_head.next.ptr); cmd; cmd = static_cast<const wmtcmd_base *>(cmd->next.ptr)) {
    switch (WMTBlitCommandType(cmd->type)) {
#define DXMT_BLIT_RW(T, CASE) case CASE: { auto c = reinterpret_cast<const T *>(cmd); reads.push_back(c->src); writes.push_back(c->dst); break; }
    DXMT_BLIT_RW(wmtcmd_blit_copy_from_buffer_to_buffer, WMTBlitCommandCopyFromBufferToBuffer)
    DXMT_BLIT_RW(wmtcmd_blit_copy_from_buffer_to_texture, WMTBlitCommandCopyFromBufferToTexture)
    DXMT_BLIT_RW(wmtcmd_blit_copy_from_buffer_to_texture_withblitoption, WMTBlitCommandCopyFromBufferToTextureWithBlitOption)
    DXMT_BLIT_RW(wmtcmd_blit_copy_from_texture_to_texture, WMTBlitCommandCopyFromTextureToTexture)
    DXMT_BLIT_RW(wmtcmd_blit_copy_from_texture_to_buffer, WMTBlitCommandCopyFromTextureToBuffer)
    DXMT_BLIT_RW(wmtcmd_blit_copy_from_texture_to_buffer_withblitoption, WMTBlitCommandCopyFromTextureToBufferWithBlitOption)
    DXMT_BLIT_RW(wmtcmd_blit_copy_texture, WMTBlitCommandCopyTexture)
#undef DXMT_BLIT_RW
    case WMTBlitCommandFillBuffer: writes.push_back(reinterpret_cast<const wmtcmd_blit_fillbuffer *>(cmd)->buffer); break;
    case WMTBlitCommandGenerateMipmaps: {
      auto c = reinterpret_cast<const wmtcmd_blit_generate_mipmaps *>(cmd); reads.push_back(c->texture); writes.push_back(c->texture); break;
    }
    case WMTBlitCommandNop: break;
    default: return false;   // fences, counters, indirect-command resets: not batched
    }
  }
  for (const auto &q : b.query_buffers) writes.push_back(q.handle);
  for (const auto &q : b.immediate_buffers) writes.push_back(q.handle);
  return true;
}
// ENC-CENSUS (diagnostics only): remember which barrier record made a pending clear be encoded on its own.
static void
CensusNoteClearBarrier(const BlitEncoderData &b, const ClearEncoderData &clear, obj_handle_t object) {
  clear.census_bar_type = 0xfe;   // touched, no record names the texture (records invalid / unnamed / aliasing)
  if (!b.barrier_records_valid) { clear.census_bar_flags = 1; return; }
  for (const auto &rec : b.barrier_records) {
    if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !rec.object || rec.object == object) {
      clear.census_bar_type = rec.type; clear.census_bar_before = rec.before; clear.census_bar_after = rec.after;
      clear.census_bar_sub = rec.subresource; clear.census_bar_flags = rec.flags;
      return;
    }
  }
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
#include "xl-hazard-observer.inc"
#include "enc-census-observer.inc"
#include "light-rename-plan.inc"   // LIGHTRN LIGHT_RENAME (encode-time planning)

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

// Mode-3 twin evidence. Retirement owns only native buffers and exact byte keys, never application COM objects.
struct TessTwinSnapshot {
  uint64_t frame=0,pso=0,patches=0,epoch=0,seq=0;
  bool single_queue=true;
  std::vector<uint8_t> shaders,key,contents;
  std::vector<std::pair<obj_handle_t,uint64_t>> versions;
  std::vector<std::shared_ptr<D3D12NativeArgumentBuffer>> keep;
  const uint8_t *data=nullptr;
  size_t bytes=0;
};
struct TessTwinObserver {
  template<class... Args> static void Emit(const char *format,Args... args) {
    const DWORD saved=GetLastError(); char line[1024]; DWORD written;
    const int n=std::snprintf(line,sizeof(line),format,args...);
    if (n>0 && size_t(n)<sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,DWORD(n),&written,nullptr);
    SetLastError(saved);
  }
  static std::atomic<const void *> &FirstWriter() { static std::atomic<const void *> q{nullptr}; return q; }
  static std::atomic<bool> &MultipleWriters() { static std::atomic<bool> q{false}; return q; }
  // Encoding and retirement each use their own maps; the snapshot is their only handoff.
  std::map<obj_handle_t,uint64_t> writes;
  uint64_t epoch=0,encoded_frame=UINT64_MAX,completed_frame=UINT64_MAX;
  std::map<std::vector<uint8_t>,TessTwinSnapshot> previous;
  void Written(obj_handle_t object) {
    if (object) ++writes[object];
    if (writes.size()>65536) { ++epoch; writes.clear(); }
  }
  void Note(const EncoderData *pass,const void *queue) {
    if (pass->type!=EncoderType::Null) {
      const void *empty=nullptr;
      FirstWriter().compare_exchange_strong(empty,queue);
      if (FirstWriter().load()!=queue) MultipleWriters().store(true);
    }
    switch (pass->type) {
    case EncoderType::Null: break;
    case EncoderType::Clear: Written(AttachmentObject(static_cast<const ClearEncoderData *>(pass)->attachment)); break;
    case EncoderType::Render: {
      const auto &r=*static_cast<const RenderEncoderData *>(pass);
      if (r.may_write_memory) ++epoch;
      for (const auto &c:r.colors) { Written(AttachmentObject(c.attachment)); Written(AttachmentObject(c.resolve_attachment)); }
      Written(AttachmentObject(r.depth.attachment)); Written(AttachmentObject(r.stencil.attachment)); break;
    }
    case EncoderType::Blit: {
      const auto &b=*static_cast<const BlitEncoderData *>(pass);
      if (b.barrier_only) {
        if (!b.barrier_records_valid) ++epoch;
        for (const auto &bar:b.barrier_records)
          if (bar.type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || bar.flags) ++epoch;
      } else {
        std::vector<obj_handle_t> reads,dest;
        if (!BlitRunObjects(b,reads,dest)) ++epoch;
        for (auto object:dest) Written(object);
      }
      break;
    }
    case EncoderType::Resolve: Written(AttachmentObject(static_cast<const ResolveEncoderData *>(pass)->dst)); break;
    default: ++epoch; break; // Unknown compute/UAV/acceleration writes never prove stability.
    }
  }
  void Complete(TessTwinSnapshot &s,WMTCommandBufferStatus status,const void *queue) {
    if (status!=WMTCommandBufferStatusCompleted) {
      Emit("dx12_tess_twins_exact status=FAILED frame=%llu pso=%llu reason=gpu_status_%u\n",
          (unsigned long long)s.frame,(unsigned long long)s.pso,unsigned(status)); return;
    }
    if (s.bytes) s.contents.assign(s.data,s.data+s.bytes);
    s.keep.clear(); s.data=nullptr;
    if (completed_frame!=s.frame) { previous.clear(); completed_frame=s.frame; }
    auto prior=previous.find(s.shaders);
    if (prior!=previous.end()) {
      const auto &p=prior->second;
      const bool key=p.key==s.key,cbv=p.contents==s.contents,versions=p.versions==s.versions;
      // A queue-local ledger cannot prove the ordering of writes executing on another queue.
      const bool known=p.epoch==s.epoch && p.single_queue && s.single_queue && !MultipleWriters().load();
      const bool equal=key && cbv && versions && known;
      Emit("dx12_tess_twins_exact status=%s queue=%p frame=%llu first=%llu second=%llu "
          "same_vs_hs_ds=1 key_equal=%u cbv_equal=%u cbv_bytes=%llu resource_versions_equal=%u "
          "writes_known=%u equal=%u patches=%llu seq=%llu\n",
          (!key||!cbv||known)?"PRESENT":"NOT_ENABLED",queue,(unsigned long long)s.frame,
          (unsigned long long)p.pso,(unsigned long long)s.pso,unsigned(key),unsigned(cbv),
          (unsigned long long)s.bytes,unsigned(versions),unsigned(known),unsigned(equal),
          (unsigned long long)s.patches,(unsigned long long)s.seq);
    } else Emit("dx12_tess_twins_exact status=PRESENT queue=%p frame=%llu first=%llu second=0 "
        "reason=first_shader_key cbv_bytes=%llu patches=%llu seq=%llu\n",queue,(unsigned long long)s.frame,
        (unsigned long long)s.pso,(unsigned long long)s.bytes,(unsigned long long)s.patches,(unsigned long long)s.seq);
    previous[s.shaders]=std::move(s);
  }
};

// The retirement worker must not own or access any public COM object.
struct D3D12CommandQueueState {
  WMT::Reference<WMT::Device> device_;
  WMT::Reference<WMT::ResidencySet> residency_set_;
  WMT::Reference<WMT::CommandQueue> queue_;
  WMT::Reference<WMT::Fence> fence_;
  WMT::Reference<WMT::Fence> immediate_out_fence_;
  // VS-STAGE RENDER_FENCE=3: updated by every compute and blit encoder and by render passes that may write memory.
  WMT::Reference<WMT::Fence> pre_raster_fence_;
  WMT::Reference<WMT::Fence> gpu4_vdone_fence_;   // GPU4 VSERIAL: updated after the vertex stage of a big depth-only pass
  std::vector<WMT::Reference<WMT::Fence>> tess_prepass_fences_;
  TessTwinObserver tess_twins_;
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
    gpu_gaps::Meta gap;   // XL-GAPS (MACRUNNER_DX12_GPU_GAPS): what produced this buffer, CPU times (QPC)
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
    std::vector<TessTwinSnapshot> tess_twins;
    std::vector<D3D12NativeComputeDispatch> argument_dispatches;
    std::vector<D3D12NativeAccelerationBuild> acceleration_builds;
    std::vector<std::shared_ptr<void>> lightrn_retained;   // LIGHTRN: scratch copies / table memory until completion
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
      if (gpu_gaps::Enabled()) {
        const DWORD gap_error = GetLastError();
        gpu_gaps::Record r;
        r.queue = uint64_t(uintptr_t(this)); r.seq = internal_seq; r.m = inflight.gap;
        r.ks = MTLCommandBuffer_property(inflight.cmdbuf.handle, WMTCommandBufferPropertyKernelStartTime);
        r.ke = MTLCommandBuffer_property(inflight.cmdbuf.handle, WMTCommandBufferPropertyKernelEndTime);
        r.gs = MTLCommandBuffer_property(inflight.cmdbuf.handle, WMTCommandBufferPropertyGPUStartTime);
        r.ge = inflight.cmdbuf.gpuEndTime();
        r.present = bool(inflight.present); r.status = uint8_t(status);
        gpu_gaps::Get().Push(r);
        SetLastError(gap_error);
      }
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
      for (auto &snapshot:inflight.tess_twins) tess_twins_.Complete(snapshot,status,this);
      inflight.tess_twins.clear();

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
  ::dxmt::layer_time::Tag("layer_retire");   // CPU-LATE
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
  ::dxmt::layer_time::Tag("layer_submit");   // CPU-LATE
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

  // ENC-FIX SIGNAL_TAIL (MACRUNNER_DX12_SIGNAL_TAIL=1): the Signal a public thread attached to an ExecuteCommandLists task
  // that the submission thread has not finished. `closed` = the call's last command buffer is committed (or no longer able
  // to take it); `done` = the signal went into that buffer.
  struct TailSignal {
    dxmt::mutex m;
    bool closed = false, claimed = false, done = false;
    std::shared_ptr<D3D12RemovableFence> fence;
    WMT::Reference<WMT::SharedEvent> event;
    UINT64 value = 0;
  };
  std::shared_ptr<TailSignal> next_tail_, last_tail_;   // producer side, under submission_->producer_mutex
  static inline thread_local TailSignal *t_tail = nullptr;   // submission thread: the task's slot
  static inline thread_local bool t_tail_arm = false;        // the next committed scope is the call's last

  template <typename F> HRESULT SubmitCPU(F &&operation) {
    try {
      // A private ref preserves lifetime without changing the application's COM reference count.
      Com<MTLD3D12CommandQueueImpl, false> keep(this);
      const uint64_t app = gpu_gaps::Enabled() ? gpu_gaps::Qpc() : 0;
      std::function<void()> work = [keep = std::move(keep), op = std::forward<F>(operation), app]() mutable {
        if (app) { gpu_gaps::t_app = app; gpu_gaps::t_dequeue = gpu_gaps::Qpc(); }
        try { op(); }
        catch (...) { keep->device_->RemoveDevice(); }
        gpu_gaps::t_app = 0; gpu_gaps::t_dequeue = 0; gpu_gaps::t_fence = 0; gpu_gaps::t_first = false;
      };
      {
        std::lock_guard lock(submission_->mutex);
        submission_->pending.push_back(std::move(work));
        ++submission_->submitted;
        submission_->peak = std::max<uint64_t>(submission_->peak, submission_->pending.size());
      }
      last_tail_ = std::move(next_tail_);   // ENC-FIX SIGNAL_TAIL: only the most recent task may take a Signal
      next_tail_.reset();
      submission_->ready.notify_one();
      return S_OK;
    } catch (...) {
      device_->RemoveDevice();
      return E_OUTOFMEMORY;
    }
  }

  // XL-GAPS: CPU-side facts of the command buffer that is being created on this thread (MACRUNNER_DX12_GPU_GAPS).
  static gpu_gaps::Meta GapBegin(uint64_t ring0) {
    gpu_gaps::Meta m;
    const uint64_t now = gpu_gaps::Qpc();
    m.app = gpu_gaps::t_app ? gpu_gaps::t_app : now;
    m.dequeue = gpu_gaps::t_dequeue;
    m.fence = gpu_gaps::t_fence; gpu_gaps::t_fence = 0;
    m.first = gpu_gaps::t_first; gpu_gaps::t_first = false;
    // a ring-capacity stall of at least 10 us: its end (QPC)
    if (ring0 && now - ring0 >= 100) m.ring = now;
    m.encoders_at_start = WMT::encoders_created;
    return m;
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
        const bool stall = queue->inflight_cmdbuf_count_.load(std::memory_order_relaxed) >= queue->inflight_depth_;
        if (stall)
          g_pass_stats.commit_stalls.fetch_add(1, std::memory_order_relaxed);
        const uint64_t ring0 = stall && gpu_gaps::Enabled() ? gpu_gaps::Qpc() : 0;
        queue->inflight_cmdbuf_count_.wait(queue->inflight_depth_, std::memory_order_acquire);
        // The slot is free only after the wait (retirement resets it before releasing capacity).
        if (gpu_gaps::Enabled()) inflight.gap = GapBegin(ring0);
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
        const bool stall = queue->inflight_cmdbuf_count_.load(std::memory_order_relaxed) >= queue->inflight_depth_;
        if (stall)
          g_pass_stats.commit_stalls.fetch_add(1, std::memory_order_relaxed);
        const uint64_t ring0 = stall && gpu_gaps::Enabled() ? gpu_gaps::Qpc() : 0;
        queue->inflight_cmdbuf_count_.wait(queue->inflight_depth_, std::memory_order_acquire);
        // The slot is free only after the wait (retirement resets it before releasing capacity).
        if (gpu_gaps::Enabled()) inflight.gap = GapBegin(ring0);
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
      if (gpu_gaps::Enabled()) {
        inflight.gap.encoders += WMT::encoders_created - inflight.gap.encoders_at_start;
        inflight.gap.commit = gpu_gaps::Qpc();
      }
      if (t_tail_arm) {   // ENC-FIX SIGNAL_TAIL: the last command buffer of the call takes the attached Signal
        t_tail_arm = false;
        if (auto *t = t_tail) {
          std::lock_guard guard(t->m);
          t->closed = true;
          if (t->claimed && !t->done && !inflight.retained_fence && !inflight.fence_wait && !inflight.present) {
            if (t->event) inflight.cmdbuf.encodeSignalEvent(t->event, t->value);
            inflight.retained_fence = t->fence;
            inflight.publishes_fence = t->fence->UsesOrderedPublication();
            inflight.fence_value = t->value;
            t->done = true;
            c17::Note(c17::Get().signal_tail_merged);
          }
        }
      }
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

  void EncodeTessTwin(CommittingScope &scope,const tess_prepass::TwinDraw &draw) {
    TessTwinSnapshot s; s.frame=draw.frame; s.pso=draw.id; s.patches=draw.patches; s.seq=scope.seq;
    std::vector<tess_prepass::TwinDraw::Range> cbvs;
    std::vector<obj_handle_t> reads;
    const auto unavailable=[&](const char *reason) {
      TessTwinObserver::Emit("dx12_tess_twins_exact status=NOT_ENABLED frame=%llu pso=%llu reason=%s\n",
          (unsigned long long)draw.frame,(unsigned long long)draw.id,reason);
    };
    if (!draw.Resolve(device_,s.key,cbvs,reads)) { unavailable("unresolved_inputs"); return; }
    auto &observer=state_->tess_twins_;
    if (observer.encoded_frame!=draw.frame) { observer.writes.clear(); observer.encoded_frame=draw.frame; }
    s.shaders=draw.pso->twin_shaders; s.epoch=observer.epoch;
    s.single_queue=!TessTwinObserver::MultipleWriters().load();
    for (auto object:reads) s.versions.emplace_back(object,observer.writes[object]);
    for (const auto &c:cbvs) s.bytes+=c.bytes;
    if (s.bytes) {
      WMTBufferInfo info{}; info.length=s.bytes; info.options=WMTResourceStorageModeShared;
      auto output=state_->device_.newBuffer(info);
      s.data=static_cast<const uint8_t *>(info.memory.get_accessible_or_null());
      if (!output || !s.data) { unavailable("cbv_readback_allocation"); return; }
      auto owner=device_->GetResidencyOwner();
      s.keep.push_back(std::make_shared<D3D12NativeArgumentBuffer>(output,info.gpu_address,owner));
      auto encoder=scope.inflight.cmdbuf.blitCommandEncoder();
      if (!encoder) { unavailable("cbv_blit_encoder"); return; }
      encoder.waitForFence(state_->fence_);
      uint64_t offset=0;
      for (const auto &c:cbvs) {
        s.keep.push_back(std::make_shared<D3D12NativeArgumentBuffer>(c.buffer,0,owner));
        wmtcmd_blit_copy_from_buffer_to_buffer copy{};
        copy.type=WMTBlitCommandCopyFromBufferToBuffer; copy.src=c.buffer; copy.src_offset=c.offset;
        copy.dst=output; copy.dst_offset=offset; copy.copy_length=c.bytes;
        MTLBlitCommandEncoder_encodeCommands(encoder,reinterpret_cast<wmtcmd_base *>(&copy)); offset+=c.bytes;
      }
      encoder.updateFence(state_->fence_);
      if (RenderFenceMode()==3) encoder.updateFence(state_->pre_raster_fence_);
      encoder.endEncoding();
    }
    scope.inflight.tess_twins.push_back(std::move(s));
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
    if (RenderFenceMode() == 3) encoder.updateFence(state_->pre_raster_fence_);
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
    if (RenderFenceMode() == 3) {
      state_->pre_raster_fence_ = metal_device.newFence();
      if (!state_->pre_raster_fence_) return E_OUTOFMEMORY;
      state_->gpu4_vdone_fence_ = metal_device.newFence();
      if (!state_->gpu4_vdone_fence_) return E_OUTOFMEMORY;
    }
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
    // Mode-3 input/write evidence belongs to submission order. Close() may execute concurrently on recording
    // threads and has neither the commit lock nor the retirement packet handoff. A mutex around writes alone
    // would remove the data race but would still describe Close order instead of GPU submission order.
    return !FaultCaptureEnabled() && !FrameReadbackEnabled() && !FullFrameCaptureEnabled() && !merge_trace::Target() &&
        !tess_prepass::Verify();
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
    if (ClearEdgeList(list)) {   // ENC-FIX CLEAR_CYCLE: its clears may fold into a pass of the next list of the same call
      c17::Note(c17::Get().clear_cycle_defer_lists);
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
      const uint32_t encoders0 = WMT::encoders_created;
      EncodeIntoScope(scope, 1, &one, true);
      list->gap_early_encoders = WMT::encoders_created - encoders0;
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
  // ENC-FIX CLEAR_CYCLE: a list whose first or last non-barrier pass is a full clear is not encoded at Close(): encoded at
  // ExecuteCommandLists together with its neighbours, a clear that a closed transition cycle carries over the list boundary can
  // still become the load action of the next list's pass (Elden Ring: a list of ten clears whose tail moves the targets to SRV,
  // the next list's head moves them back and opens the G-buffer pass).
  static bool ClearEdgeList(MTLD3D12GraphicsCommandList *list) {
    if (!enc_census::ClearCycle() || !encoder_stats::DeferClears()) return false;
    const EncoderData *first = nullptr, *last = nullptr;
    for (auto p = list->entry; p; p = p->next) {
      if (p->type == EncoderType::Null) continue;
      if (p->type == EncoderType::Blit && static_cast<BlitEncoderData *>(p)->barrier_only) continue;
      if (!first) first = p;
      last = p;
    }
    return (first && first->type == EncoderType::Clear) || (last && last->type == EncoderType::Clear);
  }
private:
  // The command buffer encoded at Close() for this queue, if it is still valid now; otherwise none (and it is dropped).
  WMT::Reference<WMT::CommandBuffer> TakeEarly(MTLD3D12GraphicsCommandList *list) {
    auto &st = encoder_stats::Get();
    WMT::Reference<WMT::CommandBuffer> out;
    if (!list->early_cmdbuf) {
      list->lightrn_early.reset();
      if (list->early_consumed) encoder_stats::Note(st.early_fallback_reexecute);
      return out;
    }
    // LIGHTRN: the CB words a Close-time rename relied on must still hold their values (else re-encode now)
    const auto *lightrn = static_cast<const light_rename::Stash *>(list->lightrn_early.get());
    if (list->early_queue_id != queue_id_) encoder_stats::Note(st.early_fallback_queue);
    else if (state_->immediate_out_pending_) encoder_stats::Note(st.early_fallback_out_pending);
    else if (lightrn && light_rename::E().snapcheck && !light_rename::SnapsHold(lightrn->snaps))
      light_rename::NoteReason(light_rename::REarlyCbChanged);
    else if (lightrn && light_rename::E().snapcheck && !light_rename::DescsHold(lightrn->descs))
      light_rename::NoteReason(light_rename::REarlyDescChanged);
    else out = std::move(list->early_cmdbuf);
    if (!out) list->lightrn_early.reset();
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
        // XL-MERGE observer: the verdict MERGE_REDUNDANT_TRANSITIONS=2 would give this boundary (nothing is merged here).
        EmptyPairVerdict e2;
        {
          auto *li = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
          auto *lj = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i + 1]);
          std::vector<const BlitEncoderData *> between(t.barriers.begin(), t.barriers.end());
          between.insert(between.end(), h.barriers.begin(), h.barriers.end());
          const bool binding = t.render->empty_binding_last != li->empty_binding_epoch || h.render->empty_binding_first != 0;
          e2 = between.empty() ? EmptyPairVerdict{} : EvaluateEmptyPair(*t.render, *h.render, binding,
                                                                          li->empty_indirect || lj->empty_indirect, between, 2);
          if (!between.empty()) NoteEmptyVerdict(e2, false);
        }
        if (audit) {
          char detail[512];
          std::snprintf(detail, sizeof(detail),
              "op=pair pair=%u same_targets=1 width=%u height=%u tail_writer=%u head_writer=%u tail_ticks=%llu head_ticks=%llu tail_bind_after=%llu head_bind_before=%llu indirect=%u e2_reject=%u e2_effect=%u e2_state=%u",
              i, t.render->render_target_width, t.render->render_target_height,
              unsigned(t.render->may_write_memory), unsigned(h.render->may_write_memory),
              (unsigned long long)static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->early_encode_ticks,
              (unsigned long long)static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i+1])->early_encode_ticks,
              (unsigned long long)(static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->empty_binding_epoch-t.render->empty_binding_last),
              (unsigned long long)h.render->empty_binding_first,
              unsigned(static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->empty_indirect ||
                       static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i+1])->empty_indirect),
              unsigned(e2.reject), unsigned(e2.effect), unsigned(e2.state));
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
    if (enc_census::Enabled()) census_scopes_.assign(Count, 0);   // ENC-CENSUS: Metal command buffer of each list
    uint32_t census_scope = 0;
    UINT i = 0;
    while (i < Count) {
      auto list = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i]);
      if (enc_census::Enabled()) census_scopes_[i] = ++census_scope;
      if (auto cmdbuf = TakeEarly(list)) {
        reuse::Publish(list->reuse_early_plan);
        CommittingScope scope(state_.get(), std::move(cmdbuf));   // commits in order, retired by the ring
        if (list->lightrn_early) {   // LIGHTRN
          scope.inflight.lightrn_retained.push_back(std::move(list->lightrn_early));
          light_rename::Note(light_rename::Get().early_committed);
        }
        scope.inflight.gap.kind = gpu_gaps::KindList; scope.inflight.gap.encoders = list->gap_early_encoders;
        if (t_tail && i + 1 == Count) t_tail_arm = true;   // ENC-FIX SIGNAL_TAIL
        encoder_stats::Note(st.early_committed);
        ++i;
        continue;
      }
      UINT j = i + 1;
      while (j < Count && !static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[j])->early_cmdbuf)
        ++j;
      encoder_stats::Note(st.early_fallback_lists, j - i);
      if (enc_census::Enabled()) for (UINT k = i; k < j; ++k) census_scopes_[k] = census_scope;
      if (t_tail && j == Count) t_tail_arm = true;   // ENC-FIX SIGNAL_TAIL: ExecuteRun commits exactly one scope
      ExecuteRun(j - i, ppCommandLists + i);
      t_tail_arm = false;
      i = j;
    }
  }
  std::vector<uint32_t> census_scopes_;   // ENC-CENSUS: per list of the current ExecuteCommandLists (encoding thread)
public:

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) { DXMT_LT(61);
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
  ) { DXMT_LT(62);
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
        scope.inflight.gap.kind = gpu_gaps::KindMapping;
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
        scope.inflight.gap.kind = gpu_gaps::KindMapping;
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
  ) { DXMT_LT(63);
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
        scope.inflight.gap.kind = gpu_gaps::KindMapping;
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
        scope.inflight.gap.kind = gpu_gaps::KindMapping;
    std::scoped_lock lock(dst->mutex, src->mutex);
    for (auto &heap : src->heaps) RetainBacking(*dst, heap);
    scope.inflight.sparse_resources = {src, dst};
    WMT4CopySparseBufferMappingOperation operation{{src_offset, src_count}, dst_offset};
    SubmitMapping(scope, [&] {
      return state_->mapping_queue_.copyBufferMappings(src->buffer, dst->buffer, &operation, 1);
    });
  };

  void STDMETHODCALLTYPE
  ExecuteCommandLists(UINT Count, ID3D12CommandList *const *ppCommandLists) { DXMT_LT(64);
    ::dxmt::layer_time::Aux(uint32_t(desc_.Type) << 16 | (Count & 0xffffu));   // CPU-LATE
    reuse::PublicSubmission reuse_order(!submission_ || DeferSubmission());
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
    // Observe descriptors/UPLOAD once on the public caller, before the async worker.
    if (!submission_ || DeferSubmission()) reuse::Execute(this, Count, ppCommandLists);
    if (!submission_ || DeferSubmission()) xl_hazard_observer::Execute(Count, ppCommandLists);
    // ENC-CENSUS: what each render pass can read, observed here (descriptors as of this call), judged after encoding.
    std::shared_ptr<enc_census_observer::EclReads> census_reads;
    if (enc_census::Enabled() && (!submission_ || DeferSubmission()))
      census_reads = enc_census_observer::PublicExecute(Count, ppCommandLists);
    if (DeferSubmission()) {
      std::lock_guard order(submission_->producer_mutex);
      try {
        std::vector<Com<MTLD3D12GraphicsCommandList>> recordings;
        recordings.reserve(Count);
        for (UINT i = 0; i < Count; ++i)
          recordings.push_back(static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[i])->SnapshotSubmission());
        std::shared_ptr<TailSignal> tail;
        if (enc_census::SignalTail()) tail = std::make_shared<TailSignal>();
        next_tail_ = tail;
        SubmitCPU([this, recordings = std::move(recordings), census_reads, tail] {
          std::vector<ID3D12CommandList *> lists;
          lists.reserve(recordings.size());
          for (const auto &list : recordings) lists.push_back(list.ptr());
          enc_census_observer::t_handover = census_reads;
          t_tail = tail.get();
          ExecuteCommandLists(UINT(lists.size()), lists.data());
          t_tail = nullptr;
          t_tail_arm = false;
          if (tail) FinishTail(*tail);
        });
      } catch (...) { next_tail_.reset(); device_->RemoveDevice(); }
      return;
    }
    // ENCODERS: submit-thread wall time of the whole call (both paths), for the per-frame audit.
    const bool timed = encoder_stats::Enabled();
    LARGE_INTEGER total_t0{};
    if (timed) QueryPerformanceCounter(&total_t0);
    const bool gap_own_app = gpu_gaps::Enabled() && !gpu_gaps::t_app;
    if (gap_own_app) gpu_gaps::t_app = gpu_gaps::Qpc();
    gpu_gaps::t_first = gpu_gaps::Enabled();
    if (census_reads) enc_census_observer::t_handover = std::move(census_reads);   // synchronous path
    if (EarlyEncodeActive()) {
      state_->early_last_execute.store(g_execute_clock.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
      ExecuteWithEarly(Count, ppCommandLists);
    }
    else {
      if (enc_census::Enabled()) census_scopes_.assign(Count, 1);
      if (t_tail) t_tail_arm = true;   // ENC-FIX SIGNAL_TAIL
      ExecuteRun(Count, ppCommandLists);
      t_tail_arm = false;
    }
    if (enc_census::Enabled()) enc_census_observer::Executed(state_.get(), Count, ppCommandLists, census_scopes_);
    if (timed) {
      LARGE_INTEGER t1{};
      QueryPerformanceCounter(&t1);
      encoder_stats::Note(encoder_stats::Get().execute_total_ticks, uint64_t(t1.QuadPart - total_t0.QuadPart));
    }
    encoder_stats::Executed();   // once per call, whichever path encoded the lists
    c17::Executed();
    if (gap_own_app) gpu_gaps::t_app = 0;
    gpu_gaps::t_first = false;
  }

  // One Metal command buffer for lists [0, Count): the recorded passes are encoded here, on the calling thread.
  void ExecuteRun(UINT Count, ID3D12CommandList *const *ppCommandLists) {
    auto scope = StartCommitting();
    scope.inflight.gap.kind = gpu_gaps::KindRun;
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
    // LIGHTRN LIGHT_RENAME (d3d12_light_rename.hpp): runs of light <-> compute batches renamed for THIS encoding; every patched
    // word is restored when `lightrn` goes out of scope (after the last pass is encoded). Before every other planner, so they
    // see the planned pass list.
    light_rename::Applied lightrn;
    if (early && Count == 1) static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[0])->lightrn_early.reset();   // a stale stash
    if (const unsigned lightrn_key = light_rename::Key();
        lightrn_key && !scope.inflight.fault_capture && !FrameReadbackEnabled() && !FullFrameCaptureEnabled() && !reuse::Enabled()) {
      light_rename::Plan(device_, Count, ppCommandLists, lightrn_key, early && Count == 1, lightrn);
      if (!lightrn.leases.empty()) {
        if (early && Count == 1) {
          auto stash = std::make_shared<light_rename::Stash>();
          stash->leases = std::move(lightrn.leases); stash->snaps = std::move(lightrn.snaps); stash->descs = std::move(lightrn.descs);
          light_rename::Note(light_rename::Get().desc_ranges, stash->descs.size());
          static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[0])->lightrn_early = std::move(stash);
          light_rename::Note(light_rename::Get().early_stash);
        } else {
          for (auto &l : lightrn.leases) scope.inflight.lightrn_retained.push_back(std::move(l));
          lightrn.leases.clear();
        }
      }
    }
    reuse::PlanBuilder reuse_plan;
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
    // ENC-FIX LOAD_DONTCARE (c18): a coverage probe counts only into an encoder whose visibility buffer is its own probe
    // buffer; otherwise its two visibility commands are encoded as nops (and restored for the next encoding of the list).
    const auto ldc_probe_mute = [&](RenderEncoderData &r, obj_handle_t visibility, bool mute) {
      if (!r.ldc_probe_on) return false;
      const bool off = mute || r.ldc_probe.handle != visibility;
      const auto type = off ? WMTRenderCommandNop : WMTRenderCommandSetVisibilityMode;
      r.ldc_probe_on->type = type;
      if (r.ldc_probe_off) r.ldc_probe_off->type = type;
      return off;
    };
    bool crossed_safe_barrier = false;            // a barrier that touches no attachment was skipped since last_render
    std::vector<uint32_t> open_slots;             // direct query slots counted in the open merged encoder
    // c17 MERGE_DEPENDENCIES (implies MERGE_SAFE's merge mode): relaxed barriers since last_render, why the last barrier
    // stopped the merge, and whether a draw of the open encoder can write memory.
    const bool merge_deps = c17::MergeDependencies() && coalescing_allowed;
    const bool c17_stats = c17::Enabled();
    const bool clear_subresource = c17::ClearSubresource() && coalescing_allowed;
    // ENC-FIX (default 0): CLEAR_BATCH shares one encoder among clears due at the same point; CLEAR_CYCLE keeps a pending
    // clear waiting across a closed transition cycle of its texture (see d3d12_enc_census.hpp).
    const bool clear_batch = enc_census::ClearBatch() && coalescing_allowed && !reuse::Enabled();
    const bool clear_cycle = enc_census::ClearCycle() && coalescing_allowed && defer_clears;
    std::unordered_set<const ClearEncoderData *> cycle_kept;   // pending clears kept by the current barrier run
    bool relaxed_since_render = false;
    uint8_t barrier_reason = ChainBarrierNone;
    bool open_writes = false;
    // VS-STAGE RENDER_FENCE=3 (split fences, d3d12_render_fence.hpp).
    const bool split_fence = RenderFenceMode() == 3;
    bool open_vertex_safe = false;   // the open render encoder waited only for the pre-raster fence before its pre-raster stages
    bool alias_join = false;         // an aliasing barrier (or one without records) since both fences were last joined
    const bool fence_stats = render_fence::StatsPeriod() != 0;   // VS-STAGE FENCE_STATS (any mode)
    bool prev_render = false;        // FENCE_STATS: the encoder encoded just before is a render encoder
    bool scope_first = true;         // FENCE_STATS: nothing encoded yet in this scope
    const auto fence_class = [](const RenderEncoderData &d) {
      if (d.render_target_count >= 4) return render_fence::ClassG;
      if (!d.render_target_count && d.depth.attachment && d.render_target_width >= 2048) return render_fence::ClassS;
      return render_fence::ClassO;
    };
    // GPU-PASS TYPED_SAFE (d3d12_typed_safe.hpp): since the last pre-raster fence update in this command buffer, did a barrier
    // make something readable by non-pixel stages? true until the first update (implicit promotions at the buffer start).
    const bool typed_safe_on = split_fence && typed_safe::Enabled();
    bool typed_exposed = true;
    // GPU4 TYPED_PRECISE (d3d12_gpu4_overlap.hpp): attachments written by render / clear / resolve encoders encoded since the
    // last pre-raster fence update in this command buffer; whether one happened yet; whether a compute / blit encoder skipped it
    // (PRERASTER_PRECISE) since; an aliasing barrier among the barriers a merge look-ahead crosses.
    const unsigned gpu4_typed = split_fence ? gpu4::TypedPrecise() : 0u;
    const unsigned gpu4_sink = gpu4::ClearSink();
    // GPU4 TS_NEIGHBOR: the timestamp pass waiting for the next copy pass's encoder (its start sample), and that pass
    const bool gpu4_ts = gpu4::TsNeighbor() && ts_nodrain::Enabled() && RenderFenceMode() != 2;
    BlitEncoderData *gpu4_ts_pass = nullptr; const EncoderData *gpu4_ts_next = nullptr;
    // GPU4 VSERIAL: the open (or just encoded) render encoder is a big depth-only pass whose vertex-done fence the next split-waiting
    // render encoder waits
    const bool gpu4_vserial = split_fence && gpu4::VSerial();
    bool gpu4_vdone_pending = false, gpu4_open_big_depth = false;
    const auto gpu4_vdone_update = [&](auto &enc) {
      if (!gpu4_open_big_depth) return;
      enc.updateFence(state_->gpu4_vdone_fence_, WMTRenderStageVertex);
      gpu4_vdone_pending = true; gpu4_open_big_depth = false;
      gpu4::Note(gpu4::Get().vserial_marked);
    };
    std::vector<obj_handle_t> gpu4_written;   // resource handles as barrier records name them (allocation textures)
    bool gpu4_pre_seen = false, gpu4_skip = false, gpu4_look_alias = false;
    const auto gpu4_pre_reset = [&] { gpu4_written.clear(); gpu4_pre_seen = true; gpu4_skip = false; };
    const auto gpu4_tex = [](const TextureViewRef &v) -> const Texture * { return v && v->allocation ? v->allocation->descriptor : nullptr; };
    const auto gpu4_add_handle = [&](obj_handle_t h) {
      if (gpu4_typed && h && std::find(gpu4_written.begin(), gpu4_written.end(), h) == gpu4_written.end()) gpu4_written.push_back(h);
    };
    const auto gpu4_add = [&](const TextureViewRef &v) { if (v) gpu4_add_handle(AttachmentObject(v)); };
    const auto gpu4_add_pass = [&](const RenderEncoderData &r) {
      if (!gpu4_typed) return;
      for (unsigned k = 0; k < r.render_target_count && k < r.colors.size(); ++k) { gpu4_add(r.colors[k].attachment); gpu4_add(r.colors[k].resolve_attachment); }
      gpu4_add(r.depth.attachment); gpu4_add(r.stencil.attachment);   // read-only planes too (conservative)
    };
    // Does any descriptor of these spans name a texture of `set` (CLEAR_SINK: textures) / of `handles` (TYPED_PRECISE: resource
    // handles)? Descriptors are read at encoding, as the hazard observer does.
    const auto gpu4_scan = [](const std::vector<gpu4::Span> &spans, auto &&hit) {
      for (const auto &sp : spans) {
        auto *heap = static_cast<MTLD3D12DescriptorHeap *>(static_cast<ID3D12DescriptorHeap *>(const_cast<void *>(sp.heap)));
        for (uint32_t k = 0; k < sp.count; ++k) {
          const auto &d = heap->GetDescriptor(sp.start + k);
          if (d.type != ShaderVisibleDescriptorType::SRVTexture && d.type != ShaderVisibleDescriptorType::UAVTexture) continue;
          if (d.SRVTexture.texture && hit(d.SRVTexture.texture)) return true;
        }
      }
      return false;
    };
    const auto gpu4_touches = [&](const std::vector<gpu4::Span> &spans, const std::vector<const Texture *> &set) {
      return !set.empty() && gpu4_scan(spans, [&](const Texture *t) { return std::find(set.begin(), set.end(), t) != set.end(); });
    };
    const auto gpu4_touches_handles = [&](const std::vector<gpu4::Span> &spans, const std::vector<obj_handle_t> &handles) {
      return !handles.empty() && gpu4_scan(spans, [&](const Texture *t) {
        auto *a = const_cast<Texture *>(t)->current();
        const obj_handle_t h = a ? a->texture().handle : 0;
        return !h || std::find(handles.begin(), handles.end(), h) != handles.end();   // no allocation: unknown -> touches
      });
    };
    // GPU4 SHADOW_LATE (d3d12_gpu4_overlap.hpp): the deferred chain (a big depth-only pass and the passes right after it on the same
    // targets), the pass to come back to after encoding it, and the chain's attachments (textures for read sets, handles for barriers).
    const unsigned gpu4_late_key = split_fence && defer_clears && coalescing_allowed && !reuse::Enabled() ? gpu4::ShadowLate() : 0u;
    std::vector<RenderEncoderData *> gpu4_late;
    std::vector<const Texture *> gpu4_late_tex;
    std::vector<obj_handle_t> gpu4_late_obj;
    EncoderData *gpu4_late_resume = nullptr;
    size_t gpu4_late_at = 0;
    bool gpu4_late_flushing = false;
    const auto gpu4_pass_textures = [&](const RenderEncoderData &r, std::vector<const Texture *> &t, std::vector<obj_handle_t> &h) {
      const auto add = [&](const TextureViewRef &v) {
        if (!v) return;
        if (auto x = gpu4_tex(v)) t.push_back(x);
        h.push_back(AttachmentObject(v));
      };
      for (unsigned k = 0; k < r.colors.size(); ++k) { add(r.colors[k].attachment); add(r.colors[k].resolve_attachment); }
      add(r.depth.attachment); add(r.stencil.attachment);
    };
    const auto gpu4_late_plain = [&](const RenderEncoderData &r) {
      return r.visibility_targets.empty() && !r.visibility_direct && !r.may_write_memory && !r.prepass_tail && !r.verify_mesh_draw &&
             !r.use_tessellation && !r.use_geometry && r.mesh_indirect_buffers.empty() && !r.ldc_probe && r.gpu4_reads &&
             r.gpu4_reads->want_all && !r.gpu4_reads->all_unknown && !r.gpu4_all_missed;
    };
    const auto gpu4_late_big_depth = [](const RenderEncoderData &r) {
      return !r.render_target_count && r.depth.attachment && r.render_target_width >= 2048;
    };
    const auto gpu4_late_holds = [&](obj_handle_t o) {   // a pending clear of the deferred chain's attachment stays with the chain
      return !gpu4_late.empty() && std::find(gpu4_late_obj.begin(), gpu4_late_obj.end(), o) != gpu4_late_obj.end();
    };
    // 0 = the chain may go past this pass; else why it is encoded first: 1 attachment, 2 read, 3 unknown, 4 barrier, 5 other
    const auto gpu4_late_verdict = [&](const EncoderData *p) -> unsigned {
      switch (p->type) {
      case EncoderType::Null: return 0;
      case EncoderType::Clear: {
        auto c = static_cast<const ClearEncoderData *>(p);
        const auto o = AttachmentObject(c->attachment);
        if (!o || std::find(gpu4_late_obj.begin(), gpu4_late_obj.end(), o) != gpu4_late_obj.end()) return 1;
        const Texture *t = gpu4_tex(c->attachment);
        if (!t) return 3;
        for (auto r : gpu4_late) if (gpu4_touches(r->gpu4_reads->all, {t})) return 2;
        return 0;
      }
      case EncoderType::Blit: {
        auto b = static_cast<const BlitEncoderData *>(p);
        if (!b->barrier_only) return 5;
        if (out_pending || !b->barrier_records_valid) return 4;   // OUT join: barrier passes are encoded
        constexpr uint32_t writes = D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_STREAM_OUT;
        for (const auto &rec : b->barrier_records)
          if (rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !rec.object || rec.flags || ((rec.before | rec.after) & writes)) return 4;
        return 0;
      }
      case EncoderType::Render: {
        auto r = static_cast<const RenderEncoderData *>(p);
        for (auto o : gpu4_late_obj) if (PassUsesObject(*r, o)) return 1;
        if (gpu4_late_key == 9) return 0;   // NEGATIVE CONTROL (probes): no read check
        if (!(r->render_target_count >= 4 || gpu4_late_big_depth(*r)) || !gpu4_late_plain(*r)) return 3;
        if (gpu4_touches(r->gpu4_reads->all, gpu4_late_tex)) return 2;
        std::vector<const Texture *> t; std::vector<obj_handle_t> h;
        gpu4_pass_textures(*r, t, h);
        if (t.size() != h.size()) return 3;
        for (auto c : gpu4_late) if (gpu4_touches(c->gpu4_reads->all, t)) return 2;
        return 0;
      }
      default: return 5;
      }
    };
    // 0 = free (split wait allowed), else why not: 1 reads unknown, 2 no pre-raster update yet in this command buffer,
    // 3 a PRERASTER_PRECISE skip since it, 4 aliasing, 5 reads an attachment written since it.
    const auto gpu4_typed_verdict = [&](const RenderEncoderData &d) -> unsigned {
      if (!gpu4_typed || !d.pre_raster_typed || d.pre_raster_hard || !d.gpu4_reads || d.gpu4_reads->pre_unknown || d.gpu4_pre_missed) return 1;
      if (gpu4_typed == 9) return 0;   // NEGATIVE CONTROL (probes): no proof
      if (!gpu4_pre_seen) return 2;
      if (gpu4_skip) return 3;
      if (alias_join || gpu4_look_alias) return 4;
      return gpu4_touches_handles(d.gpu4_reads->pre, gpu4_written) ? 5u : 0u;
    };
    const auto pre_update = [&](auto &enc) { if (split_fence) { enc.updateFence(state_->pre_raster_fence_); typed_exposed = false; gpu4_pre_reset(); } };
    const auto typed_free = [&](const RenderEncoderData &d, bool exposed) {   // eligible for the TYPED_SAFE split wait
      return d.pre_raster_typed && !d.pre_raster_hard && !exposed;
    };
    const auto vertex_safe = [&](const RenderEncoderData &d, bool exposed) {
      return split_fence && (!d.pre_raster_typed || (typed_safe_on && typed_free(d, exposed)) || (gpu4_typed && !gpu4_typed_verdict(d))) &&
             !d.use_tessellation && !d.use_geometry && !alias_join;
    };
    const auto encode_alias_join = [&] {
      if (!split_fence) { alias_join = false; return; }
      if (!alias_join) return;
      if (fence_stats) render_fence::Get().joins.fetch_add(1, std::memory_order_relaxed);
      // Everything before the aliasing barrier is complete before the pre-raster stages of anything after it.
      auto join = cmdbuf.blitCommandEncoder();
      join.waitForFence(state_->fence_);
      join.updateFence(state_->fence_);
      join.updateFence(state_->pre_raster_fence_);
      join.endEncoding();
      alias_join = false;
      typed_exposed = false;
      gpu4_pre_reset();
    };
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
    // ENC-FIX BLIT_RUN (default 0): a blit encoder kept open across plain copy passes that touch disjoint resources,
    // separated only by barrier passes (no aliasing / UAV barriers); reads / writes of everything encoded into it.
    const bool blit_run = enc_census::BlitRun() && coalescing_allowed && !reuse::Enabled();
    WMT::BlitCommandEncoder open_blit{};
    bool blit_open = false;
    std::vector<obj_handle_t> blit_reads, blit_writes;
    // GAPS PRERASTER_PRECISE (d3d12_prerast_precise.hpp): a compute / blit encoder that writes no buffer memory does not
    // update the pre-raster fence (vertex-safe passes read only buffers before rasterization). The skip keeps alias_join
    // (and TYPED_SAFE's exposed state) as they are: only an encoder that updates both fences completes a join.
    const bool prerast_precise_on = split_fence && prerast_precise::Enabled();
    bool blit_run_buffers = true;   // the open BLIT_RUN encoder holds a command that writes buffer memory
    const auto close_open_blit = [&] {
      if (!blit_open) return;
      open_blit.updateFence(state_->fence_);
      // C18-PRE (BLIT_RUN x RENDER_FENCE=3): the epilogue of a plain blit pass, which a kept-open run skipped - a run is
      // a producer of pre-raster reads like any blit (copy -> vertex buffer), and it waited the common fence when opened.
      if (!prerast_precise_on || blit_run_buffers) {
        pre_update(open_blit);
        alias_join = false;
        if (prerast_precise_on) prerast_precise::Note(false, false);
      } else {
        for (auto h : blit_writes) gpu4_add_handle(h);   // GPU4 TYPED_PRECISE: the run's texture writes are not on the pre-raster fence
        prerast_precise::Note(true, false);
      }
      prev_render = false; scope_first = false;
      open_blit.endEncoding();
      blit_open = false;
      blit_reads.clear(); blit_writes.clear();
    };
    bool render_open = false;
    RenderEncoderData *last_render = nullptr;     // last recorded pass with nothing encoded after it
    struct PendingClear {
      ClearEncoderData *data;
      const void *list;
      bool sunk = false;   // GPU4 CLEAR_SINK: its texture is in shader-read states (carried past proven non-readers only)
    };
    std::vector<PendingClear> pending_clears;     // full clears not yet encoded (fold only)
    // GPU4 CLEAR_SINK (d3d12_gpu4_overlap.hpp). What a barrier pass does to a pending clear's texture: 0 not named, 1 RENDER_TARGET
    // -> shader-read states only, 2 shader-read states only -> RENDER_TARGET, 3 anything else (or aliasing / unreadable records).
    const auto gpu4_barrier_kind = [&](const BlitEncoderData &b, const ClearEncoderData &clear) -> int {
      if (!b.barrier_records_valid) return 3;
      const obj_handle_t object = AttachmentObject(clear.attachment);
      // subresource 0 names the whole texture when it has a single mip and slice (Elden Ring's census: `sub=0`)
      const Texture *tex = gpu4_tex(clear.attachment);
      const bool single = tex && const_cast<Texture *>(tex)->miplevelCount() == 1 && tex->arrayLength() == 1;
      constexpr uint32_t kRead = 0x40 | 0x80;   // NON_PIXEL_ | PIXEL_SHADER_RESOURCE
      int kind = 0;
      for (const auto &rec : b.barrier_records) {
        if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) return 3;
        if (!rec.object || (rec.object != object && rec.object2 != object)) continue;
        if (rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || rec.flags || !(rec.subresource == 0xffffffffu || (single && rec.subresource == 0))) return 3;
        int k = 3;
        if (rec.before == 0x4 && rec.after && !(rec.after & ~kRead)) k = 1;
        else if (rec.after == 0x4 && rec.before && !(rec.before & ~kRead)) k = 2;
        if (k == 3 || (kind && kind != k)) return 3;
        kind = k;
      }
      return kind;
    };
    std::vector<const ClearEncoderData *> gpu4_keep;   // pending clears the current barrier keeps (sunk or returned)
    const auto gpu4_barrier_pre = [&](const BlitEncoderData &b) {
      gpu4_keep.clear();
      if (!gpu4_sink) return;
      for (auto &pc : pending_clears) {
        if (pc.data->clear_dsv) continue;   // colour clears only
        const int kind = gpu4_barrier_kind(b, *pc.data);
        if (!kind) continue;
        if (!pc.sunk && kind == 1) { pc.sunk = true; gpu4_keep.push_back(pc.data); gpu4::Note(gpu4::Get().sink_marked); }
        else if (pc.sunk && kind == 2) { pc.sunk = false; gpu4_keep.push_back(pc.data); gpu4::Note(gpu4::Get().sink_unsunk); }
        else if (pc.sunk) gpu4::Note(gpu4::Get().stop_barrier);   // encoded by the caller (touched)
      }
    };
    const auto gpu4_kept = [&](const ClearEncoderData *c) { return std::find(gpu4_keep.begin(), gpu4_keep.end(), c) != gpu4_keep.end(); };
    // GPU4 TS_NEIGHBOR: encode the waiting timestamp pass as before (its own blit encoder, end-of-encoder sample, after the fence)
    const auto gpu4_ts_flush = [&] {
      auto *t = gpu4_ts_pass; gpu4_ts_pass = nullptr; gpu4_ts_next = nullptr;
      if (!t) return;
      gpu4::Note(gpu4::Get().ts_flushed);
      WMTSampleBufferAttachmentInfo sample{};
      sample.sample_buffer = t->timestamp_samples;
      sample.start_of_encoder_sample_index = ~0ull;
      sample.end_of_encoder_sample_index = t->timestamp_index;
      auto encoder = cmdbuf.blitCommandEncoderWithSampleBuffers(&sample, 1);
      encoder.waitForFence(state_->fence_);
      encoder.encodeCommands(&t->cmd_head);
      encoder.updateFence(state_->fence_);
      pre_update(encoder);
      alias_join = false;
      prev_render = false; scope_first = false;
      encoder.endEncoding();
    };
    std::vector<ClearEncoderData *> virtual_clears; // counters only: clears a following pass could have absorbed
    unsigned previous_plain_kind = 0; // diagnostics only; reset by every barrier or other pass
    // ENC-CENSUS (diagnostics only): kind of the last encoder this scope created (0 none, 1 render, 2 clear, 3 other)
    // and whether a clear closed the open render encoder since.
    uint8_t census_prev = 0;
    bool census_clear_closed = false;
    const auto close_open_render = [&] {
      close_open_blit();   // ENC-FIX BLIT_RUN: any other encoder, or the end, closes an open blit run
      if (!render_open)
        return;
      open_render.updateFence(state_->fence_, WMTRenderStageFragment);
      if (split_fence && open_writes) { open_render.updateFence(state_->pre_raster_fence_, WMTRenderStageFragment); typed_exposed = false; gpu4_pre_reset(); }
      gpu4_vdone_update(open_render);   // GPU4 VSERIAL
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
      close_open_blit();
      if (render_open) {   // ENCODERS step 4: a carried clear can be encoded while a merged encoder is open
        close_open_render();
        last_render = nullptr;
      }
      data->census_begin = enc_census::BeginYes;
      data->census_why = uint8_t(enc_census::WhyClearDirect + why);
      census_prev = 2;
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
      gpu4_add(data->attachment);   // GPU4 TYPED_PRECISE
      auto encoder = cmdbuf.renderCommandEncoder(info);
      reuse_plan.Add(data);
      encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
      encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
      encoder.updateFence(state_->fence_, WMTRenderStageFragment);
      encoder.endEncoding();
      prev_render = true; scope_first = false;
      g_pass_stats.clear_encoders.fetch_add(1, std::memory_order_relaxed);
      encoder_stats::Count(encoder_stats::KindClear, data->close_reason);
    };
    // ENC-FIX CLEAR_BATCH: one encoder for a group of clears (same size / layers / samples, each texture once, at most 8
    // colours and one depth/stencil texture). The first clear is the pass of record (census, capture, reuse plan).
    const auto clear_samples = [](const ClearEncoderData &c) {
      return c.attachment && c.attachment->allocation && c.attachment->allocation->descriptor
                 ? c.attachment->allocation->descriptor->sampleCount() : 0u;
    };
    const auto encode_clear_group = [&](std::vector<PendingClear> &group, uint8_t why) {
      if (group.size() == 1) { encode_clear_pass(group[0].data, group[0].list, why); return; }
      close_open_blit();
      if (render_open) { close_open_render(); last_render = nullptr; }
      WMTRenderPassInfo info;
      WMT::InitializeRenderPassInfo(info);
      unsigned slot = 0;
      for (size_t k = 0; k < group.size(); ++k) {
        auto *data = group[k].data;
        if (c17_stats) {
          auto &cs = c17::Get();
          switch (why) {
          case ClearWhyBarrier: c17::Note(cs.clear_enc_barrier); break;
          case ClearWhyResolve: c17::Note(cs.clear_enc_resolve); break;
          case ClearWhyBreak: c17::Note(cs.clear_enc_break); break;
          case ClearWhyScopeEnd: c17::Note(cs.clear_enc_scope_end); break;
          default: c17::Note(cs.clear_enc_direct); break;
          }
          if (k) c17::Note(cs.clear_batched);
        }
        data->census_begin = k ? enc_census::BeginNo : enc_census::BeginYes;
        data->census_why = k ? uint8_t(enc_census::WhyClearBatched) : uint8_t(enc_census::WhyClearDirect + why);
        audit_clear_object(AttachmentObject(data->attachment));
        if (audit) encoder_stats::Note(encoder_stats::Get().att_clear);
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
        } else {
          auto &color = info.colors[slot++];
          color.clear_color = data->color;
          color.texture = data->attachment.texture();
          color.load_action = WMTLoadActionClear;
          color.store_action = WMTStoreActionStore;
          color.depth_plane = data->depth_plane;
        }
      }
      info.render_target_width = group[0].data->width;
      info.render_target_height = group[0].data->height;
      info.render_target_array_length = group[0].data->array_length;
      CaptureRenderPass(scope, group[0].list, group[0].data, info);
      for (auto &g : group) gpu4_add(g.data->attachment);   // GPU4 TYPED_PRECISE
      auto encoder = cmdbuf.renderCommandEncoder(info);
      reuse_plan.Add(group[0].data);
      encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
      encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
      encoder.updateFence(state_->fence_, WMTRenderStageFragment);
      encoder.endEncoding();
      g_pass_stats.clear_encoders.fetch_add(1, std::memory_order_relaxed);
      encoder_stats::Count(encoder_stats::KindClear, group[0].data->close_reason);
      if (c17_stats) c17::Note(c17::Get().clear_batch_groups);
      census_prev = 2;
    };
    // Encode clears that are due at the same point, in recorded order; with CLEAR_BATCH consecutive fitting ones share one
    // encoder (clears of different textures commute; a texture seen again starts a new encoder).
    const auto encode_clears = [&](std::vector<PendingClear> list, uint8_t why) {
      if (!clear_batch) { for (auto &pending : list) encode_clear_pass(pending.data, pending.list, why); return; }
      std::vector<PendingClear> group;
      std::vector<obj_handle_t> objects;
      unsigned colors = 0; bool ds = false;
      const auto flush = [&] { if (!group.empty()) encode_clear_group(group, why); group.clear(); objects.clear(); colors = 0; ds = false; };
      for (auto &pending : list) {
        const auto &c = *pending.data;
        const auto object = AttachmentObject(c.attachment);
        const auto &g = group.empty() ? c : *group[0].data;
        const bool fits = !group.empty() && object && g.width == c.width && g.height == c.height &&
                          g.array_length == c.array_length && clear_samples(g) == clear_samples(c) && clear_samples(c) &&
                          std::find(objects.begin(), objects.end(), object) == objects.end() && (c.clear_dsv ? !ds : colors < 8);
        if (!fits) flush();
        group.push_back(pending); objects.push_back(object);
        if (c.clear_dsv) ds = true; else ++colors;
      }
      flush();
    };
    // ENC-FIX CLEAR_CYCLE: does the run of barrier-only passes starting at `p` (list `li`, on into the next lists of this
    // call) move every subresource of the clear's texture it names out of the clear's write state and back, naming the
    // texture only in transitions, before the next pass of any other kind?
    const auto cycle_closes = [&](UINT li, EncoderData *p, const ClearEncoderData &clear, obj_handle_t object) {
      if (out_pending) return false;   // barrier passes would be encoded (OUT join): keep the plain rule
      const uint32_t write = clear.clear_dsv ? uint32_t(D3D12_RESOURCE_STATE_DEPTH_WRITE) : uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET);
      std::vector<std::pair<uint32_t, uint32_t>> state;   // subresource -> current state (starts at `write`)
      while (true) {
        if (!p) {
          if (++li >= Count) return false;   // the call ends inside the cycle: not closed here
          p = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[li])->entry;
          continue;
        }
        if (p->type == EncoderType::Null) { p = p->next; continue; }
        if (p->type != EncoderType::Blit || !static_cast<BlitEncoderData *>(p)->barrier_only) break;
        const auto &b = *static_cast<BlitEncoderData *>(p);
        if (!b.barrier_records_valid) return false;
        for (const auto &rec : b.barrier_records) {
          if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !rec.object) return false;
          if (rec.object != object) continue;
          if (rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || rec.flags) return false;
          auto it = std::find_if(state.begin(), state.end(), [&](const auto &x) { return x.first == rec.subresource; });
          if (it == state.end()) {
            // ALL mixed with single subresources would need per-subresource expansion: not handled (conservative)
            if (!state.empty() && ((rec.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) !=
                                   (state[0].first == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)))
              return false;
            state.push_back({rec.subresource, write});
            it = state.end() - 1;
          }
          if (it->second != rec.before) return false;
          it->second = rec.after;
        }
        p = p->next;
      }
      if (state.empty()) return false;
      for (const auto &x : state) if (x.second != write) return false;
      return true;
    };
    // Step 4: encode now the pending clears whose texture `touches` returns true for (order kept).
    const auto encode_pending_if = [&](auto &&touches, uint8_t why) {
      std::vector<PendingClear> keep, now;
      for (auto &pending : pending_clears) {
        if (touches(AttachmentObject(pending.data->attachment), *pending.data))
          now.push_back(pending);
        else
          keep.push_back(pending);
      }
      encode_clears(std::move(now), why);
      pending_clears.swap(keep);
    };
    // Anything that is not a mergeable draw pass ends the sequence: earlier draws and clears must be encoded first.
    // ENCODERS step 4: other passes cannot use a render target without a barrier, so pending clears may go on waiting.
    const auto break_sequence = [&](bool flush_clears = true) {
      close_open_render();
      if (flush_clears || !defer_clears) {
        encode_clears(std::move(pending_clears), flush_clears ? ClearWhyScopeEnd : ClearWhyBreak);
        pending_clears.clear();
      }
      virtual_clears.clear();
      last_render = nullptr;
      barrier_since_render = false;
      relaxed_since_render = false;
      barrier_reason = ChainBarrierNone;
    };
    size_t tess_fence_cursor = 0;
    const auto encode_tess_payload = [&](auto &encoder, RenderEncoderData *data, WMT::Fence ready) {
      encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&data->prepass_head));
      wmtcmd_compute_memory_barrier phase_barrier{};
      phase_barrier.type = WMTComputeCommandMemoryBarrier; phase_barrier.scope = WMTBarrierScopeBuffers;
      encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&phase_barrier));
      encoder.encodeCommands(&data->prepass_scan_head);
      encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&phase_barrier));
      encoder.encodeCommands(&data->prepass_materialize_head);
      if (ready) {
        // Publish only to the consumer. Updating the common fence here would serialize every independent pass after us.
        encoder.updateFence(ready);
      } else {
        encoder.updateFence(state_->fence_); pre_update(encoder);
        prev_render = false; scope_first = false;
      }
    };
    const auto encode_tess_prepass = [&](RenderEncoderData *data, WMT::Fence ready) {
      auto encoder = cmdbuf.computeCommandEncoder(data->prepass_read_only);
      encoder.waitForFence(state_->fence_);
      encode_tess_payload(encoder,data,ready);
      encoder.endEncoding();
    };
#include "post-chain-encode.inc"   // POSTCS 03.11 MACRUNNER_DX12_POST_CHAIN
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
      std::unordered_map<const EncoderData *,std::vector<RenderEncoderData *>> tess_starts;
      std::unordered_map<const RenderEncoderData *,WMT::Fence> tess_ready;
      if (tess_prepass::Schedule() && !out_pending && !early) {
        std::vector<EncoderData *> passes;
        for (auto p = current; p && passes.size() <= 4096; p = p->next) passes.push_back(p);
        uint64_t candidates = 0, moved = 0, distance = 0, unknown = 0;
        // GAPS 01.11: what stopped each pre-pass (the pass right above its start), for the map of the tess remainder
        uint64_t stop_top = 0, stop_scratch = 0, stop_mem = 0, stop_attach = 0, stop_barrier = 0, stop_blit = 0, stop_resolve = 0,
                 stop_clear = 0, stop_other = 0;
        if (passes.size() <= 4096) for (size_t at = 0; at < passes.size(); ++at) {
          if (passes[at]->type != EncoderType::Render) continue;
          auto target = static_cast<RenderEncoderData *>(passes[at]);
          if (!target->prepass_tail) continue;
          ++candidates;
          std::vector<obj_handle_t> reads;
          bool known = target->prepass_read_only && target->prepass_inputs.size() == target->prepass_draws;
          for (const auto &input : target->prepass_inputs) if (!input || !input->Resolve(reads)) known = false;
          if (!known) { ++unknown; continue; }
          const auto uses = [&](obj_handle_t object) {
            return object && std::find(reads.begin(),reads.end(),object) != reads.end();
          };
          size_t start = at;
          int why = 0;   // 1 scratch 2 mem 3 attach 4 barrier 5 blit 6 resolve 7 clear 8 other
          while (start) {
            auto prior = passes[start-1]; bool conflict = false;
            switch (prior->type) {
            case EncoderType::Null: break;
            case EncoderType::Clear:
              conflict = uses(AttachmentObject(static_cast<ClearEncoderData *>(prior)->attachment)); if (conflict) why = 7; break;
            case EncoderType::Render: {
              auto r = static_cast<RenderEncoderData *>(prior);
              conflict = r->may_write_memory; if (conflict) why = 2;
              bool attach = false;
              for (const auto &c : r->colors)
                attach |= uses(AttachmentObject(c.attachment)) || uses(AttachmentObject(c.resolve_attachment));
              attach |= uses(AttachmentObject(r->depth.attachment)) || uses(AttachmentObject(r->stencil.attachment));
              if (attach && !why) why = 3;
              conflict |= attach;
              // The packed buffers are deliberately reused by sequential passes. They cannot be overwritten before
              // the preceding consumer retires; this is a WAR dependency even when all application inputs are disjoint.
              // (GAPS 01.11: with TESS_PREPASS_RENAME=1 no two draws of a recording share them; =9 = negative control.)
              if (tess_prepass::RenameMode() != 9) {
                bool scratch = false;
                for (auto b : target->prepass_scratch)
                  scratch |= std::find(r->prepass_scratch.begin(),r->prepass_scratch.end(),b) != r->prepass_scratch.end();
                if (scratch && !why) why = 1;
                conflict |= scratch;
              }
              break;
            }
            case EncoderType::Blit: {
              auto b = static_cast<BlitEncoderData *>(prior);
              if (b->barrier_only) {
                conflict = !b->barrier_records_valid;
                for (const auto &bar : b->barrier_records)
                  conflict |= bar.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || bar.flags != 0;
                if (conflict) why = 4;
              } else {
                std::vector<obj_handle_t> br,bw;
                conflict = b->timestamp_samples || b->resolve_samples || b->wait_for_prior_work || b->immediate_out ||
                           b->join_immediate_out || !BlitRunObjects(*b,br,bw);
                for (auto w : bw) conflict |= uses(w);
                if (conflict) why = 5;
              }
              break;
            }
            case EncoderType::Resolve:
              conflict = uses(AttachmentObject(static_cast<ResolveEncoderData *>(prior)->dst)); if (conflict) why = 6; break;
            default: conflict = true; why = 8; break; // Unknown compute/UAV/acceleration writes: preserve their boundary.
            }
            if (conflict) break;
            --start;
          }
          switch (start ? why : 0) {
          case 0: ++stop_top; break; case 1: ++stop_scratch; break; case 2: ++stop_mem; break; case 3: ++stop_attach; break;
          case 4: ++stop_barrier; break; case 5: ++stop_blit; break; case 6: ++stop_resolve; break; case 7: ++stop_clear; break;
          default: ++stop_other; break;
          }
          if (start == at) continue;
          if (tess_fence_cursor == state_->tess_prepass_fences_.size())
            state_->tess_prepass_fences_.push_back(state_->device_.newFence());
          auto fence = state_->tess_prepass_fences_[tess_fence_cursor++];
          if (!fence) continue;
          tess_ready.emplace(target,fence);
          tess_starts[passes[start]].push_back(target);
          ++moved; distance += at-start;
        }
        if (tess_prepass::StatsPeriod() && candidates) {
          static std::atomic<uint64_t> samples{0};
          if (++samples % tess_prepass::StatsPeriod() == 0)
            fprintf(stderr,"dx12_tess_schedule candidates=%llu moved=%llu crossed_passes=%llu unknown=%llu compute_batches=%llu max_batch=%llu"
                " stop_top=%llu stop_scratch=%llu stop_mem=%llu stop_attach=%llu stop_barrier=%llu stop_blit=%llu stop_resolve=%llu"
                " stop_clear=%llu stop_other=%llu rename=%u\n",
                (unsigned long long)candidates,(unsigned long long)moved,(unsigned long long)distance,(unsigned long long)unknown,
                (unsigned long long)tess_starts.size(),(unsigned long long)[&] {
                  size_t largest=0; for (const auto &start:tess_starts) largest=std::max(largest,start.second.size()); return largest;
                }(),(unsigned long long)stop_top,(unsigned long long)stop_scratch,(unsigned long long)stop_mem,(unsigned long long)stop_attach,
                (unsigned long long)stop_barrier,(unsigned long long)stop_blit,(unsigned long long)stop_resolve,(unsigned long long)stop_clear,
                (unsigned long long)stop_other,tess_prepass::RenameMode());
        }
      }
      encoder_stats::SeqAppend(i ? "|" : "/");
      // GPU4 SHADOW_LATE: at the end of the last list a deferred chain is encoded (the end of the ExecuteCommandLists call)
      const auto gpu4_late_end = [&]() -> bool {
        if (gpu4_late.empty() || gpu4_late_flushing || i + 1 != Count) return false;
        gpu4::Note(gpu4::Get().late_flush_end);
        close_open_render();
        gpu4_late_resume = nullptr; gpu4_late_at = 0; gpu4_late_flushing = true; current = gpu4_late[0];
        return true;
      };
      while (current || gpu4_late_end()) {
        // POSTCS 03.11: a pass a converted run already encoded (its member, or a barrier / null pass between members)
        if (!pc_consumed.empty() && !gpu4_late_flushing && pc_consumed.count(current)) { current = current->next; continue; }
        // GPU4 SHADOW_LATE: encode the deferred chain before a pass that needs it, or defer a new chain
        if (gpu4_late_key && !gpu4_late_flushing) {
          if (!gpu4_late.empty()) {
            if (const unsigned why = gpu4_late_verdict(current)) {
              auto &gs = gpu4::Get();
              gpu4::Note(why == 1 ? gs.late_flush_attach : why == 2 ? gs.late_flush_read : why == 3 ? gs.late_flush_unknown :
                         why == 4 ? gs.late_flush_barrier : gs.late_flush_other);
              close_open_render();
              gpu4_late_resume = current; gpu4_late_at = 0; gpu4_late_flushing = true; current = gpu4_late[0];
            } else if (current->type != EncoderType::Null) {
              auto &gs = gpu4::Get();
              gpu4::Note(current->type == EncoderType::Render ? gs.late_past_render :
                         current->type == EncoderType::Clear ? gs.late_past_clear : gs.late_past_barrier);
            }
          } else if (current->type == EncoderType::Render && gpu4_late_big_depth(*static_cast<RenderEncoderData *>(current))) {
            auto r = static_cast<RenderEncoderData *>(current);
            bool ok = gpu4_late_plain(*r) && !alias_join && !out_pending && tess_starts.find(current) == tess_starts.end();
            std::vector<RenderEncoderData *> chain;
            EncoderData *after = current->next;
            if (ok) {
              chain.push_back(r);
              while (after && after->type == EncoderType::Render && SameRenderTargets(*r, *static_cast<RenderEncoderData *>(after))) {
                auto n = static_cast<RenderEncoderData *>(after);
                if (!gpu4_late_plain(*n) || tess_starts.find(after) != tess_starts.end()) { ok = false; break; }
                chain.push_back(n); after = after->next;
              }
            }
            if (ok) {
              close_open_render();
              gpu4_late = std::move(chain); gpu4_late_tex.clear(); gpu4_late_obj.clear();
              gpu4_pass_textures(*r, gpu4_late_tex, gpu4_late_obj);
              gpu4::Note(gpu4::Get().late_deferred);
              current = after;
              continue;
            }
            gpu4::Note(gpu4::Get().late_refused);
          }
        }
        if (tess_prepass::Verify() && tess_census::Enabled()) state_->tess_twins_.Note(current,state_.get());
        if (const auto starts = tess_starts.find(current); starts != tess_starts.end()) {
          break_sequence(); // Drain pending clears/writers at the chosen input boundary, before the compute wait.
          encode_alias_join();
          // The scheduler has already kept application writes and scratch WAR dependencies before this boundary.
          // Every target here reads application resources only, so share one encoder and publish each consumer's fence.
          bool read_only=true;
          for (auto target:starts->second) read_only &= target->prepass_read_only;
          auto encoder=cmdbuf.computeCommandEncoder(read_only);
          encoder.waitForFence(state_->fence_);
          for (auto target:starts->second)
            encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&target->prepass_head));
          wmtcmd_compute_memory_barrier phase_barrier{};
          phase_barrier.type=WMTComputeCommandMemoryBarrier; phase_barrier.scope=WMTBarrierScopeBuffers;
          encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&phase_barrier));
          for (auto target:starts->second) encoder.encodeCommands(&target->prepass_scan_head);
          encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&phase_barrier));
          for (auto target:starts->second) encoder.encodeCommands(&target->prepass_materialize_head);
          for (auto target:starts->second) encoder.updateFence(tess_ready.at(target));
          encoder.endEncoding();
        }
        TraceMergePass(scope, pCommandList, current);
        if (!cycle_kept.empty() && current->type != EncoderType::Null &&
            !(current->type == EncoderType::Blit && static_cast<BlitEncoderData *>(current)->barrier_only))
          cycle_kept.clear();   // ENC-FIX CLEAR_CYCLE: a run of barrier passes ends at any other pass
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
        // GPU4 TS_NEIGHBOR: any pass other than the awaited copy (or the end) gets the timestamp encoded first, as before
        if (gpu4_ts_pass && current->type != EncoderType::Null && current != gpu4_ts_next) gpu4_ts_flush();
        // GPU4 CLEAR_SINK: a sunk clear goes on only past render passes proven not to read its texture; any other pass (and a
        // clear of the same texture) gets it first. Barrier passes decide in the barrier path.
        if (gpu4_sink && current->type != EncoderType::Null && !pending_clears.empty() &&
            !(current->type == EncoderType::Blit && static_cast<BlitEncoderData *>(current)->barrier_only) &&
            std::any_of(pending_clears.begin(), pending_clears.end(), [](const PendingClear &pc) { return pc.sunk; })) {
          auto &gs = gpu4::Get();
          std::vector<PendingClear> now, keep;
          bool passed = false;
          for (auto &pc : pending_clears) {
            if (!pc.sunk) { keep.push_back(pc); continue; }
            const obj_handle_t object = AttachmentObject(pc.data->attachment);
            bool go = true;
            if (current->type == EncoderType::Render) {
              const auto *r = static_cast<const RenderEncoderData *>(current);
              const Texture *t = gpu4_tex(pc.data->attachment);
              if (PassUsesObject(*r, object)) gpu4::Note(gs.stop_attach);
              else if (gpu4_sink == 9) go = false;   // NEGATIVE CONTROL (probes): no read check
              else if (!t || !r->gpu4_reads || !r->gpu4_reads->want_all || r->gpu4_reads->all_unknown || r->gpu4_all_missed) gpu4::Note(gs.stop_unknown);
              else if (gpu4_touches(r->gpu4_reads->all, {t})) gpu4::Note(gs.stop_read);
              else go = false;
            } else if (current->type == EncoderType::Clear &&
                       AttachmentObject(static_cast<const ClearEncoderData *>(current)->attachment) != object) {
              go = false;   // a clear of another texture
            } else {
              gpu4::Note(gs.stop_other);
            }
            (go ? now : keep).push_back(pc);
            passed |= !go;
          }
          if (passed && current->type == EncoderType::Render) gpu4::Note(gs.sink_passed);
          if (!now.empty()) {
            pending_clears.swap(keep);
            encode_clears(std::move(now), ClearWhyBarrier);
          }
        }
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
            census_clear_closed |= render_open;
            close_open_render();
            last_render = nullptr;
            barrier_since_render = false;
            relaxed_since_render = false;
            barrier_reason = ChainBarrierNone;
          }
          if (fold_clears) {
            data->census_begin = enc_census::BeginNo;   // folded unless encoded later (encode_clear_pass)
            data->census_why = enc_census::WhyFolded;
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
          if (pc_key && post_chain_try(i, data, tess_starts)) break;   // POSTCS 03.11: a run encoded as one compute encoder
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
            if (render_open && (merge_mode == 2 || !barrier_since_render) && !writer_conflict &&
                !data->prepass_tail &&   // GPU-PASS TESS_PREPASS: its compute pre-pass is encoded before it, in its own encoder
                !(split_fence && (alias_join || (open_vertex_safe && !vertex_safe(*data, typed_exposed))))) {
              // Same attachments and nothing encoded in between: continue in the open Metal encoder.
              // Every recorded pass re-emits its complete state, so the command chains can simply follow each other.
              g_pass_stats.render_merged.fetch_add(1, std::memory_order_relaxed);
              encoder_stats::Note(encoder_stats::Get().merged_render);
              if (empty_plan.heads.count(data)) c17::Note(c17::Get().empty_merged);
              if (crossed_safe_barrier)
                encoder_stats::Note(encoder_stats::Get().merged_safe);
              crossed_safe_barrier = false;
              open_slots.insert(open_slots.end(), data->direct_slots.begin(), data->direct_slots.end());
              // ENC-FIX LOAD_DONTCARE (c18): a merged pass loads nothing at all (its flags are irrelevant); its probe counts
              // only when this encoder already counts into the probe buffer.
              if (data->ldc_dontcare_mask && c17_stats) c17::Note(c17::Get().ldc_merged_tail);
              if (ldc_probe_mute(*data, open_visibility, false) && c17_stats) c17::Note(c17::Get().ldc_probe_nop);
              if (audit) AuditRenderCommands(reinterpret_cast<const wmtcmd_base *>(&data->cmd_head), state_audit);
              TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
              open_render.encodeCommands(&data->cmd_head);
              data->census_begin = enc_census::BeginNo;
              data->census_why = enc_census::WhyMerged;
              reuse_plan.Add(data,true);
              last_render = data;
              barrier_since_render = false;
              relaxed_since_render = false;
              barrier_reason = ChainBarrierNone;
              open_writes |= data->may_write_memory;
              if (fence_stats) {
                auto &fs = render_fence::Get(); const auto c = fence_class(*data);
                fs.merged[c].fetch_add(1, std::memory_order_relaxed);
                fs.work[c].fetch_add(data->vertex_work, std::memory_order_relaxed);
              }
              virtual_clears.clear();
              break;
            }
          }
          // ENC-CENSUS (diagnostics only): why this pass opens a new encoder; the order follows the merge test above.
          uint8_t census_why = enc_census::WhyUnset;
          if (!plain_pass) census_why = enc_census::WhyNotPlain;
          else if (census_prev == 0) census_why = enc_census::WhyScopeStart;
          else if (census_prev != 1) census_why = enc_census::WhyAfterOther;
          else if (!last_render) census_why = census_clear_closed ? enc_census::WhyPendingClear : enc_census::WhyNotPlain;
          else if (!SameRenderTargets(*last_render, *data)) census_why = enc_census::WhyDiffTargets;
          else if (!(pending_clears.empty() || (defer_clears && !pending_touch()))) census_why = enc_census::WhyPendingClear;
          else if (!visibility_fits()) census_why = enc_census::WhyVisibility;
          else if (open_dead_mask) census_why = enc_census::WhyDeadStore;
          else if (barrier_since_render && merge_mode != 2)
            census_why = merge_deps && barrier_reason == ChainBarrierAttachment ? enc_census::WhyBarrierAttach
                                                                                : enc_census::WhyBarrierOther;
          else if (writer_conflict) census_why = enc_census::WhyWriter;
          else if (!render_open) census_why = enc_census::WhyNotOpen;
          close_open_render();
          const bool alias_now = alias_join;
          encode_alias_join();
          if (data->prepass_tail && tess_ready.find(data) == tess_ready.end()) {
            // GPU-PASS TESS_PREPASS: the tessellation pre-pass of this pass's draws. It waits everything before it (like the
            // pass's own pre-raster wait, which tessellation passes always take on the common fence) and publishes on both
            // fences, so the pass's vertex stage reads the finished vertex records, indices and arguments.
            // Each draw owns its scratch. Read-only hulls, atomic scans and materializers can overlap within a phase;
            // the two explicit barriers still order every producer before its consumers. Keep side-effecting hulls serial.
            auto encoder = cmdbuf.computeCommandEncoder(data->prepass_read_only);
            encoder.waitForFence(state_->fence_);
            encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&data->prepass_head));
            // Scans and materialization only touch each draw's private scratch
            // (the per-PSO need counters are atomic). Two phase barriers per pass suffice for every draw.
            wmtcmd_compute_memory_barrier phase_barrier{};
            phase_barrier.type = WMTComputeCommandMemoryBarrier;
            phase_barrier.scope = WMTBarrierScopeBuffers;
            encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&phase_barrier));
            encoder.encodeCommands(&data->prepass_scan_head);
            encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&phase_barrier));
            encoder.encodeCommands(&data->prepass_materialize_head);
            encoder.updateFence(state_->fence_);
            pre_update(encoder);
            prev_render = false; scope_first = false;
            encoder.endEncoding();
          }
          WMTRenderPassInfo render_pass_info;
          WMT::InitializeRenderPassInfo(render_pass_info);
          if (!data->visibility_targets.empty()) {
            data->census_svc = 3;   // occlusion scratch blit before, reduction compute after
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
            pre_update(encoder);
            prev_render = false; scope_first = false;
            encoder.endEncoding();
            render_pass_info.visibility_buffer = state_->visibility_scratch_;
          } else if (data->visibility_direct) {
            // ENCODERS: queries of this pass count straight into the query heap (offset = index * 8).
            render_pass_info.visibility_buffer = data->visibility_direct;
          } else if (data->ldc_probe) {
            // ENC-FIX LOAD_DONTCARE (c18): the coverage probe of a pass that opens an encoder counts into its own buffer.
            render_pass_info.visibility_buffer = data->ldc_probe;
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
          if (data->ldc_dontcare_mask) {
            // ENC-FIX LOAD_DONTCARE (c18): the pass opens this encoder, so its proven full overwrite replaces a remaining Load
            // (a folded clear stays Clear). Decided after the merge test: tails never reach here.
            unsigned applied = 0;
            for (unsigned k = 0; k < 8; ++k)
              if (((data->ldc_dontcare_mask >> k) & 1) && render_pass_info.colors[k].load_action == WMTLoadActionLoad) {
                render_pass_info.colors[k].load_action = WMTLoadActionDontCare;
                ++applied;
              }
            if (((data->ldc_dontcare_mask >> 8) & 1) && render_pass_info.depth.load_action == WMTLoadActionLoad) {
              render_pass_info.depth.load_action = WMTLoadActionDontCare;
              if (c17_stats) c17::Note(c17::Get().ldc_depth_dontcare);
            }
            if (applied && c17_stats) c17::Note(c17::Get().ldc_att_dontcare, applied);
          }
          for (unsigned k = 0; k < 8; ++k) {
            data->census_load[k] = uint8_t(render_pass_info.colors[k].load_action);
            data->census_store[k] = uint8_t(render_pass_info.colors[k].store_action);
          }
          data->census_load[8] = uint8_t(render_pass_info.depth.load_action);
          data->census_store[8] = uint8_t(render_pass_info.depth.store_action);
          data->census_load[9] = uint8_t(render_pass_info.stencil.load_action);
          data->census_store[9] = uint8_t(render_pass_info.stencil.store_action);
          data->census_begin = enc_census::BeginYes;
          data->census_why = census_why;
          census_prev = 1;
          census_clear_closed = false;
          CaptureRenderPass(scope, pCommandList, current, render_pass_info);
          WMT::Reference<WMT::Object> tess_verify;
          if (data->verify_mesh_draw) {
            if (data->twin_draw) EncodeTessTwin(scope,*data->twin_draw);
            tess_verify = WMT::Reference<WMT::Object>(MTLCommandBuffer_tessVerifyBegin(cmdbuf.handle, &render_pass_info,
                &data->cmd_head, data->verify_prefix_end, data->verify_mesh_draw, state_->fence_.handle, data->verify_pso));
          }
          auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
          reuse_plan.Add(data);
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
          bool pass_vertex_safe = vertex_safe(*data, typed_exposed);
          // VS-STAGE: the passes that may continue this encoder (same targets, only barriers between) decide with it, so a
          // pass whose pre-raster stages read textures does not have to break the merge (the refusal below stays a safety net).
          // VS-STAGE 0002 (23.10, from the ER pair F3: G-buffer 1 -> 2 encoders): the merge continues across the command lists
          // of this scope (mode 2 encodes mergeable lists together at submission), so look ahead into the following lists too.
          if (pass_vertex_safe && merge_passes) {
            EncoderData *next = current->next;
            unsigned list = i;
            bool ahead_exposed = typed_exposed;   // GPU-PASS TYPED_SAFE: the skipped barriers in between count too
            gpu4_look_alias = false;              // GPU4 TYPED_PRECISE: an aliasing barrier in between joins nothing here
            for (;;) {
              while (!next && ++list < Count) next = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[list])->entry;
              if (!next) break;
              if (next->type == EncoderType::Blit && static_cast<BlitEncoderData *>(next)->barrier_only) {
                const auto *b = static_cast<BlitEncoderData *>(next);
                if (!b->barrier_records_valid) { ahead_exposed = true; gpu4_look_alias = true; }
                for (const auto &rec : b->barrier_records) if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) gpu4_look_alias = true;
                for (const auto &rec : b->barrier_records)
                  if (typed_safe::Exposes(rec.type, rec.object, rec.before, rec.after)) ahead_exposed = true;
              }
              // Null markers, clears (fragment-only, other targets or folded) and skipped barriers do not end the look-ahead.
              if (next->type == EncoderType::Null || next->type == EncoderType::Clear ||
                  (next->type == EncoderType::Blit && static_cast<BlitEncoderData *>(next)->barrier_only)) { next = next->next; continue; }
              if (next->type != EncoderType::Render) break;
              auto *r = static_cast<RenderEncoderData *>(next);
              if (!SameRenderTargets(*data, *r)) break;
              if (!vertex_safe(*r, ahead_exposed)) { pass_vertex_safe = false; break; }
              next = next->next;
            }
          }
          gpu4_look_alias = false;
          open_vertex_safe = pass_vertex_safe;
          // GPU4 TYPED_PRECISE counters: the verdict for the pass that opens this encoder (typed SRV-only pre-raster reads)
          if (gpu4_typed && data->pre_raster_typed && !data->pre_raster_hard) {
            auto &gs = gpu4::Get();
            switch (gpu4_typed_verdict(*data)) {
            case 0: gpu4::Note(pass_vertex_safe ? gs.typed_free : gs.typed_w); break;   // free but a merged follower kept the full wait: typed_w
            case 1: gpu4::Note(gs.typed_unknown); break;
            case 2: gpu4::Note(gs.typed_start); break;
            case 3: gpu4::Note(gs.typed_skip); break;
            case 4: gpu4::Note(gs.typed_alias); break;
            default: gpu4::Note(gs.typed_w); break;
            }
          } else if (gpu4_typed && pass_vertex_safe && data->pre_raster_typed) {
            gpu4::Note(gpu4::Get().typed_free);
          }
          if (fence_stats) {
            auto &fs = render_fence::Get(); const auto c = fence_class(*data);
            const bool typed = data->pre_raster_typed, tg = data->use_tessellation || data->use_geometry, safe = !typed && !tg;
            fs.encoders[c].fetch_add(1, std::memory_order_relaxed);
            if (typed && split_fence && !data->pre_raster_hard)   // GPU-PASS TYPED_SAFE (either key)
              (typed_exposed ? fs.typed_exposed[c] : fs.typed_free[c]).fetch_add(1, std::memory_order_relaxed);
            fs.work[c].fetch_add(data->vertex_work, std::memory_order_relaxed);
            if (typed) fs.typed[c].fetch_add(1, std::memory_order_relaxed);
            if (tg) fs.tess_geo[c].fetch_add(1, std::memory_order_relaxed);
            if (alias_now) fs.alias[c].fetch_add(1, std::memory_order_relaxed);
            if (safe) fs.safe[c].fetch_add(1, std::memory_order_relaxed);
            if (prev_render) fs.after_render[c].fetch_add(1, std::memory_order_relaxed);
            if (safe && prev_render) {
              fs.safe_after_render[c].fetch_add(1, std::memory_order_relaxed);
              fs.work_safe_after_render[c].fetch_add(data->vertex_work, std::memory_order_relaxed);
            }
            if (safe && scope_first) {
              fs.safe_first[c].fetch_add(1, std::memory_order_relaxed);
              fs.work_safe_first[c].fetch_add(data->vertex_work, std::memory_order_relaxed);
            }
          }
          prev_render = true; scope_first = false;
          if (const auto ready = tess_ready.find(data); ready != tess_ready.end())
            encoder.waitForFence(ready->second,WMTRenderStagePreRaster);
          if (split_fence) {
            if (pass_vertex_safe) {
              // Its pre-raster stages read only buffers: only compute/blit/memory-writing passes can have produced them.
              encoder.waitForFence(state_->pre_raster_fence_, WMTRenderStagePreRaster);
              encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
              if (gpu4_vdone_pending) { encoder.waitForFence(state_->gpu4_vdone_fence_, WMTRenderStagePreRaster); gpu4_vdone_pending = false; }
            } else
              encoder.waitForFence(state_->fence_, WMTRenderStagePreRaster);
          } else if (const unsigned fence_mode = RenderFenceMode(); fence_mode != 2)
            encoder.waitForFence(state_->fence_, fence_mode == 1 ? WMTRenderStageFragment : WMTRenderStageVertex);
          gpu4_add_pass(*data);   // GPU4 TYPED_PRECISE: written by this encoder from now on
          // GPU4 VSERIAL: a big depth-only encoder signals the end of its vertex stage at its close ("work so far enqueued")
          gpu4_open_big_depth = gpu4_vserial && !data->render_target_count && data->depth.attachment && data->render_target_width >= 2048;
          TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
          if (ldc_probe_mute(*data, render_pass_info.visibility_buffer, false) && c17_stats)
            c17::Note(c17::Get().ldc_probe_nop);
          encoder.encodeCommands(&data->cmd_head);
          last_render = plain_pass ? data : nullptr;
          barrier_since_render = false;
          relaxed_since_render = false;
          barrier_reason = ChainBarrierNone;
          if (merge_passes && plain_pass && !data->verify_mesh_draw) {
            // Leave the encoder open: the next recorded pass may continue it.
            open_render = encoder;
            render_open = true;
            break;
          }
          encoder.updateFence(state_->fence_, WMTRenderStageFragment);
          if (split_fence && data->may_write_memory) { encoder.updateFence(state_->pre_raster_fence_, WMTRenderStageFragment); typed_exposed = false; gpu4_pre_reset(); }
          gpu4_vdone_update(encoder);   // GPU4 VSERIAL
          encoder.endEncoding();
          if (tess_verify)
            MTLCommandBuffer_tessVerifyEnd(cmdbuf.handle, tess_verify.handle, state_->fence_.handle);
          EncodeRenderReadback(scope, *data);
          if (!data->visibility_targets.empty()) {
            // All samples in this pass belong to the same active query set.
            // The scratch is queue-local and cannot be reused until reduction finishes.
            auto reduction = cmdbuf.computeCommandEncoder(false);
            reuse_plan.Add(nullptr);
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
            pre_update(reduction);
            prev_render = false; scope_first = false;
            reduction.endEncoding();
          }
          break;
        }
        case EncoderType::Blit: {
          auto data = static_cast<BlitEncoderData *>(current);
          if (data->barrier_only && !out_pending) {
            data->census_begin = enc_census::BeginNo;
            data->census_why = enc_census::WhySkippedBarrier;
            if (blit_open) {   // ENC-FIX BLIT_RUN: only plain transitions of named resources may stand inside a run
              bool plain = data->barrier_records_valid;
              for (const auto &rec : data->barrier_records)
                if (rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !rec.object || rec.flags) plain = false;
              if (!plain) close_open_blit();
            }
            // C18-PRE: after the BLIT_RUN close above (closing a run joins both fences and clears alias_join).
            if (split_fence || fence_stats) {   // VS-STAGE: memory reinterpretation is invisible to the per-stage rule; join both fences
              bool aliasing = !data->barrier_records_valid;
              for (const auto &rec : data->barrier_records) aliasing |= rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
              alias_join |= aliasing;
              // GPU-PASS TYPED_SAFE: a transition that makes something readable by non-pixel stages (or an unrecorded barrier)
              if (!data->barrier_records_valid) typed_exposed = true;
              for (const auto &rec : data->barrier_records)
                if (typed_safe::Exposes(rec.type, rec.object, rec.before, rec.after)) typed_exposed = true;
            }
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
              if (defer_clears) gpu4_barrier_pre(*data);   // GPU4 CLEAR_SINK
              if (defer_clears)
                encode_pending_if([&](obj_handle_t object, const ClearEncoderData &clear) {
                  if (gpu4_kept(&clear)) return false;   // GPU4 CLEAR_SINK: sunk (or returned to an ordinary pending clear)
                  if (gpu4_late_holds(object)) return false;   // GPU4 SHADOW_LATE: folds into the deferred pass
                  const bool touches = clear_subresource ? BarrierTouchesClear(*data, clear) : BarrierTouchesObject(*data, object);
                  if (touches && clear_cycle && (cycle_kept.count(&clear) || cycle_closes(i, current, clear, object))) {
                    if (cycle_kept.insert(&clear).second && c17_stats) c17::Note(c17::Get().clear_cycle_kept);
                    return false;   // the cycle closes before any other pass: the clear keeps waiting
                  }
                  if (touches) CensusNoteClearBarrier(*data, clear, object);
                  return touches;
                }, ClearWhyBarrier);
              else {
                encode_clears(std::move(pending_clears), ClearWhyBarrier);
                pending_clears.clear();
              }
            }
            virtual_clears.clear();
            break;
          }
          if (defer_clears && data->barrier_only && !pending_clears.empty()) gpu4_barrier_pre(*data);   // GPU4 CLEAR_SINK
          if (defer_clears && data->barrier_only && !pending_clears.empty())
            encode_pending_if([&](obj_handle_t object, const ClearEncoderData &clear) {
              if (gpu4_kept(&clear)) return false;   // GPU4 CLEAR_SINK
              if (gpu4_late_holds(object)) return false;   // GPU4 SHADOW_LATE
              const bool touches = clear_subresource ? BarrierTouchesClear(*data, clear) : BarrierTouchesObject(*data, object);
              if (touches) CensusNoteClearBarrier(*data, clear, object);
              return touches;
            }, ClearWhyBarrier);
          // ENC-FIX BLIT_RUN: a plain copy pass joins the open run when it neither writes what the run reads or writes
          // nor reads what the run writes (then no ordering inside the encoder is needed); otherwise it starts a run.
          std::vector<obj_handle_t> run_reads, run_writes;
          const bool run_plain = blit_run && !data->barrier_only && !data->timestamp_samples && !data->resolve_samples &&
                                 !data->wait_for_prior_work && !data->immediate_out && !data->join_immediate_out &&
                                 BlitRunObjects(*data, run_reads, run_writes);
          // break_sequence(false) below would encode pending clears without DEFER_CLEARS: then the pass is not merged
          if (run_plain && blit_open && (defer_clears || pending_clears.empty())) {
            const auto in = [](const std::vector<obj_handle_t> &v, obj_handle_t o) { return std::find(v.begin(), v.end(), o) != v.end(); };
            bool disjoint = true;
            for (auto o : run_writes) if (in(blit_reads, o) || in(blit_writes, o)) disjoint = false;
            for (auto o : run_reads) if (in(blit_writes, o)) disjoint = false;
            if (disjoint) {
              TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
              if (prerast_precise_on) blit_run_buffers |= prerast_precise::BlitWritesBuffers(&data->cmd_head);
              open_blit.encodeCommands(&data->cmd_head);
              blit_reads.insert(blit_reads.end(), run_reads.begin(), run_reads.end());
              blit_writes.insert(blit_writes.end(), run_writes.begin(), run_writes.end());
              data->census_begin = enc_census::BeginNo;
              data->census_why = enc_census::WhyBlitMerged;
              if (c17_stats) c17::Note(c17::Get().blit_run_merged);
              break;
            }
          }
          break_sequence(false);
          // GPU4 TS_NEIGHBOR: a plain timestamp pass followed (in this list) by a plain copy pass: no encoder of its own; the copy's
          // encoder samples its start. Out-of-order OUT writes pending, markers, resolves: as before.
          if (gpu4_ts && data->timestamp_samples && !data->wait_for_prior_work && !data->resolve_samples && !data->immediate_out &&
              !data->join_immediate_out && !data->barrier_only && !out_pending) {
            const EncoderData *nx = current->next;
            while (nx && nx->type == EncoderType::Null) nx = nx->next;
            const auto *nb = nx && nx->type == EncoderType::Blit ? static_cast<const BlitEncoderData *>(nx) : nullptr;
            // GPU4 16:5x (G4S1: ts_moved 0): in Elden Ring the pass after T_begin is a ResolveQueryData of OTHER queries; its
            // encoder may carry T's start sample unless it resolves T's own sample (same sample buffer and T's index in its range).
            const bool resolve_other = nb && nb->resolve_samples &&
                (nb->resolve_samples.handle != data->timestamp_samples.handle || data->timestamp_index < nb->resolve_start ||
                 data->timestamp_index >= nb->resolve_start + nb->resolve_count);
            if (nb && !nb->barrier_only && !nb->timestamp_samples && (!nb->resolve_samples || resolve_other) && !nb->wait_for_prior_work &&
                !nb->immediate_out && !nb->join_immediate_out) {
              gpu4_ts_pass = data; gpu4_ts_next = nx;
              data->census_begin = enc_census::BeginNo; data->census_why = enc_census::WhyBlitTimestamp;
              break;
            }
          }
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
          // GPU-PASS TS_NODRAIN=1: a timestamp pass relies on the fences below instead of a queue-wide drain.
          const bool ts_drain = data->wait_for_prior_work || !ts_nodrain::Enabled() || RenderFenceMode() == 2;
          if ((data->timestamp_samples && ts_drain) || data->wait_for_prior_work)
            timestamp_barrier();
          WMTSampleBufferAttachmentInfo sample{};
          sample.sample_buffer = data->timestamp_samples;
          sample.start_of_encoder_sample_index = ~0ull;
          sample.end_of_encoder_sample_index = data->timestamp_index;
          const bool ts_start = gpu4_ts_pass && current == gpu4_ts_next && !data->timestamp_samples;   // GPU4 TS_NEIGHBOR
          if (ts_start) {
            sample.sample_buffer = gpu4_ts_pass->timestamp_samples;
            sample.start_of_encoder_sample_index = gpu4_ts_pass->timestamp_index;
            sample.end_of_encoder_sample_index = ~0ull;
            gpu4::Note(gpu4::Get().ts_moved);
            gpu4_ts_pass = nullptr; gpu4_ts_next = nullptr;
          }
          auto encoder = (data->timestamp_samples || ts_start) ? cmdbuf.blitCommandEncoderWithSampleBuffers(&sample, 1)
                                                               : cmdbuf.blitCommandEncoder();
          data->census_begin = enc_census::BeginYes;
          data->census_why = data->timestamp_samples || data->resolve_samples ? enc_census::WhyBlitTimestamp
                             : data->barrier_only ? enc_census::WhyBlitOutJoin
                             : data->immediate_out ? enc_census::WhyBlitImmediate : enc_census::WhyBlitPlain;
          census_prev = 3;
          encoder_stats::Count(encoder_stats::KindBlit, data->close_reason);
          encoder.waitForFence(state_->fence_);
          if ((data->join_immediate_out || (data->timestamp_samples && !ts_drain)) && out_pending)
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
          // GAPS PRERASTER_PRECISE: only plain copy work into textures may skip the pre-raster update (timestamps, counter
          // resolves, OUT writes / joins and barrier passes keep it).
          const bool blit_buffers = !prerast_precise_on || data->resolve_samples || data->timestamp_samples ||
                                    data->immediate_out || data->join_immediate_out || data->barrier_only ||
                                    data->wait_for_prior_work || prerast_precise::BlitWritesBuffers(&data->cmd_head);
          if (run_plain && !data->resolve_samples) {
            // ENC-FIX BLIT_RUN: keep the encoder open for the next plain copy pass (closed by any other encoder)
            open_blit = encoder; blit_open = true;
            blit_reads = std::move(run_reads); blit_writes = std::move(run_writes);
            blit_run_buffers = blit_buffers;
            break;
          }
          if (data->immediate_out) {
            // Do not put OUT on the common fence: independent later passes
            // must not acquire a dependency on this deferred write.
            encoder.updateFence(state_->immediate_out_fence_);
            out_pending = true;
          } else {
            encoder.updateFence(state_->fence_);
            if (blit_buffers) {
              pre_update(encoder);
              alias_join = false;
              if (prerast_precise_on) prerast_precise::Note(false, false);
            } else {
              // GPU4 TYPED_PRECISE: what this copy pass writes is not on the pre-raster fence (unknown commands: no proof)
              { std::vector<obj_handle_t> r_, w_; if (BlitRunObjects(*data, r_, w_)) for (auto h : w_) gpu4_add_handle(h); else gpu4_skip = true; }
              prerast_precise::Note(true, false);
            }
            prev_render = false; scope_first = false;
            if (data->join_immediate_out)
              out_pending = false;
          }
          encoder.endEncoding();
          if (data->timestamp_samples && ts_drain)
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
          if (data->lightrn_copyin) {
            // LIGHTRN copy-in run (03.11): S_1..S_n-1 := S on the subresources the batches do not fully write, before any batch
            // writes S; the compute encoder below waits for it on the common fence. Same pre-raster rule as the compute pass.
            auto copyin = cmdbuf.blitCommandEncoder();
            copyin.waitForFence(state_->fence_);
            MTLBlitCommandEncoder_encodeCommands(copyin, data->lightrn_copyin);
            copyin.updateFence(state_->fence_);
            if (!prerast_precise_on || data->may_write_buffers || data->query_pipeline) { pre_update(copyin); alias_join = false; }
            copyin.endEncoding();
          }
          // LIGHTRN: a run's dispatches write different textures and read only what no pass of the run writes: concurrent
          auto encoder = cmdbuf.computeCommandEncoder(data->lightrn_concurrent != 0);
          data->census_begin = enc_census::BeginYes;
          data->census_why = enc_census::WhyCompute;
          census_prev = 3;
          encoder_stats::Count(encoder_stats::KindCompute, data->close_reason);
          reuse_plan.Add(data);
          encoder.waitForFence(state_->fence_);
          TraceTransferCommands(scope, pCommandList, current, reinterpret_cast<wmtcmd_base *>(&data->cmd_head));
          encoder.encodeCommands(&data->cmd_head);
          encoder.updateFence(state_->fence_);
          // GAPS PRERASTER_PRECISE: a pass whose dispatches write only textures leaves the pre-raster fence (and a pending
          // aliasing join) alone; query resolves and helper passes keep may_write_buffers = true.
          if (!prerast_precise_on || data->may_write_buffers || data->query_pipeline) {
            pre_update(encoder);
            alias_join = false;
            if (prerast_precise_on) prerast_precise::Note(false, true);
          } else {
            gpu4_skip = true; prerast_precise::Note(true, true);
          }
          prev_render = false; scope_first = false;
          encoder.endEncoding();
          break;
        }
        case EncoderType::AccelerationBuild: {
          break_sequence(false);
          g_pass_stats.other_encoders.fetch_add(1, std::memory_order_relaxed);
          if (pass_stats) g_pass_stats.other_acceleration.fetch_add(1, std::memory_order_relaxed);
          encoder_stats::Count(encoder_stats::KindAcceleration, current->close_reason);
          current->census_begin = enc_census::BeginYes;
          current->census_why = enc_census::WhyAcceleration;
          census_prev = 3;
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
          current->census_begin = enc_census::BeginYes;
          current->census_why = enc_census::WhyArgument;
          census_prev = 3;
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
          reuse_plan.Add(current);
          encoder.encodeCommands(&head);
          encoder.updateFence(state_->fence_);
          pre_update(encoder);
          prev_render = false; scope_first = false;
          alias_join = false;
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
          current->census_begin = enc_census::BeginYes;
          current->census_why = enc_census::WhyResolve;
          census_prev = 3;
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
          gpu4_add(data->dst);   // GPU4 TYPED_PRECISE
          auto encoder = cmdbuf.renderCommandEncoder(info);
          encoder.waitForFence(state_->fence_, WMTRenderStageFragment);
          encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
          reuse_plan.Add(current);
          encoder.updateFence(state_->fence_, WMTRenderStageFragment);
          encoder.endEncoding();

          break;
        }
        }
        if (gpu4_late_flushing) {   // GPU4 SHADOW_LATE: the next pass of the chain, then back where the chain was needed
          if (++gpu4_late_at < gpu4_late.size()) current = gpu4_late[gpu4_late_at];
          else {
            close_open_render();
            gpu4_late_flushing = false; gpu4_late.clear(); gpu4_late_tex.clear(); gpu4_late_obj.clear();
            current = gpu4_late_resume; gpu4_late_resume = nullptr;
          }
        } else
          current = current->next;
      }
      g_pass_stats.lists.fetch_add(1, std::memory_order_relaxed);
    }
    if (gpu4_sink)
      for (const auto &pc : pending_clears) if (pc.sunk) gpu4::Note(gpu4::Get().stop_end);
    if (gpu4_ts_pass) gpu4_ts_flush();
    break_sequence(true);
    encode_alias_join();   // VS-STAGE: an aliasing barrier at the end of this scope still joins before the next one
    if (reuse::Enabled()) {
      if (early && Count==1) static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[0])->reuse_early_plan=reuse_plan.plan;
      else reuse::Publish(reuse_plan.plan);
    }
    merge_trace::Emit(state_.get(), scope.seq, nullptr, 0, "submit.end", "lists=%u", Count);
    prerast_precise::ExecuteTrace();   // GAPS PRERASTER_PRECISE_TRACE (probes)
    if (!early) post_chain::Executed();   // POSTCS 03.11 MACRUNNER_DX12_POST_CHAIN_TRACE (probes)
    gpu4::ExecuteTrace();              // GPU4_TRACE (probes)
    light_rename::ExecuteTrace();      // LIGHTRN_TRACE (probes)
    if (!early) PassStatsReport();
    if (!early && out_pending) {
      // Separate ExecuteCommandLists calls are ordered even when the next
      // submission relies on buffer decay/promotion instead of a barrier.
      auto encoder = cmdbuf.blitCommandEncoder();
      if (pass_stats) g_pass_stats.service_out_join_blit.fetch_add(1, std::memory_order_relaxed);
      encoder_stats::Count(encoder_stats::KindServiceOutJoin, encoder_stats::ReasonUnknown);
      enc_census_observer::NoteOutJoin();
      encoder.waitForFence(state_->fence_);
      encoder.waitForFence(state_->immediate_out_fence_);
      encoder.updateFence(state_->fence_);
      pre_update(encoder);
      prev_render = false; scope_first = false;
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

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) { DXMT_LT(65);};

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) { DXMT_LT(66);};

  void STDMETHODCALLTYPE EndEvent() { DXMT_LT(67);};

  HRESULT STDMETHODCALLTYPE
  Signal(ID3D12Fence *pFence, UINT64 Value) { DXMT_LT(68);
    ::dxmt::layer_time::Aux(uint32_t(desc_.Type) << 16);   // CPU-LATE
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
      if (auto tail = std::move(last_tail_)) {   // ENC-FIX SIGNAL_TAIL: ride in the last command buffer of that call
        last_tail_.reset();
        std::lock_guard guard(tail->m);
        if (!tail->closed && !tail->claimed) {
          tail->claimed = true;
          tail->fence = fence;
          tail->event = event;
          tail->value = Value;
          return pipeline.Result(S_OK);
        }
      }
      return pipeline.Result(SubmitCPU([this, fence, event, Value] {
        if (FAILED(device_->GetDeviceRemovedReason())) return;
        enc_census_observer::Signalled(state_.get());
        auto scope = StartCommitting();
        scope.inflight.gap.kind = gpu_gaps::KindSignal;
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
    enc_census_observer::Signalled(state_.get());
    auto scope = StartCommitting();
    scope.inflight.gap.kind = gpu_gaps::KindSignal;
    auto &cmdbuf = scope.inflight.cmdbuf;
    scope.inflight.retained_fence = static_cast<MTLD3D12Fence *>(pFence)->GetRemovalState();
    pipeline_timing::Get().Emit(pipeline_timing::QueueIdentity, pipeline_timing::Point, this,
        scope.seq, pipeline_timing::Pointer(state_.get()), pipeline_timing::Pointer(pFence), Value, cmdbuf.handle);
    const HRESULT signal_hr = static_cast<MTLD3D12Fence *>(pFence)->SignalQueue(cmdbuf, Value);
    scope.inflight.publishes_fence = SUCCEEDED(signal_hr) && scope.inflight.retained_fence->UsesOrderedPublication();
    scope.inflight.fence_value = Value;
    return pipeline.Result(signal_hr);
  };

  // ENC-FIX SIGNAL_TAIL: after the call's task. A Signal attached too late for the last command buffer (or a call that
  // committed nothing) gets its own command buffer here, in the same order the separate task would have had.
  void FinishTail(TailSignal &t) {
    bool own;
    {
      std::lock_guard guard(t.m);
      t.closed = true;
      own = t.claimed && !t.done;
    }
    if (!t.claimed) return;
    enc_census_observer::Signalled(state_.get());
    if (!own || FAILED(device_->GetDeviceRemovedReason())) return;
    c17::Note(c17::Get().signal_tail_late);
    auto scope = StartCommitting();
    scope.inflight.gap.kind = gpu_gaps::KindSignal;
    if (t.event) scope.inflight.cmdbuf.encodeSignalEvent(t.event, t.value);
    scope.inflight.retained_fence = t.fence;
    scope.inflight.publishes_fence = t.fence->UsesOrderedPublication();
    scope.inflight.fence_value = t.value;
  }

  HRESULT STDMETHODCALLTYPE
  Wait(ID3D12Fence *pFence, UINT64 Value) { DXMT_LT(69);
    ::dxmt::layer_time::Aux(uint32_t(desc_.Type) << 16);   // CPU-LATE
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
        const uint64_t fence0 = gpu_gaps::Enabled() ? gpu_gaps::Qpc() : 0;
        while (!wait->event.waitUntilSignaledValue(Value, 100))
          if (FAILED(device_->GetDeviceRemovedReason())) return;
        if (fence0) {   // a CPU fence wait of at least 10 us: its end (QPC)
          const uint64_t fence1 = gpu_gaps::Qpc();
          gpu_gaps::t_fence = fence1 - fence0 >= 100 ? fence1 : 0;
        }
        if (FAILED(device_->GetDeviceRemovedReason())) return;
        enc_census_observer::Signalled(state_.get());
        auto scope = StartCommitting();
        scope.inflight.gap.kind = gpu_gaps::KindWait;
        scope.inflight.cmdbuf.encodeWaitForEvent(wait->event, Value);
        scope.inflight.retained_fence = fence;
        scope.inflight.fence_wait = std::move(wait->publication);
      }));
    }
    enc_census_observer::Signalled(state_.get());
    auto scope = StartCommitting();
    scope.inflight.gap.kind = gpu_gaps::KindWait;
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
  GetTimestampFrequency(UINT64 *pFrequency) { DXMT_LT(70);
    if (!pFrequency)
      return E_INVALIDARG;
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc_.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return E_FAIL;
    // Apple GPU timestamp scale; validated by the native clock/readback probe.
    *pFrequency = 1000000000ull;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) { DXMT_LT(71);
    return E_NOTIMPL;
  };

  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) { DXMT_LT(72);
    *__ret = desc_;
    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
  ) { DXMT_LT(73);
    return dxmt::CreateSwapChain(pFactory, device_, this, hWnd, pDesc, pFullscreenDesc, ppSwapChain);
  }

  HRESULT
  Present(const std::shared_ptr<D3D12PresentState> &present, ID3D12Resource *backbuffer, double after) override {
    reuse::PublicSubmission reuse_order(!submission_ || DeferSubmission());
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
      if (SUCCEEDED(queued)) reuse::Presented();
      if (SUCCEEDED(queued)) xl_hazard_observer::Presented();
      if (SUCCEEDED(queued)) enc_census_observer::PublicPresented();
      return FAILED(queued) ? queued : result;
    }
    if (!submission_) reuse::Presented();
    if (!submission_) xl_hazard_observer::Presented();
    if (!submission_) enc_census_observer::PublicPresented();
    prerast_precise::Presented();   // GAPS PRERASTER_PRECISE counters (with FENCE_STATS)
    gpu4::Presented();              // GPU4 TYPED_PRECISE / CLEAR_SINK counters (with FENCE_STATS)
    light_rename::Presented();      // LIGHTRN counters (LIGHTRN_STATS / GPU4_STATS / FENCE_STATS period)
    post_chain::Presented();        // POSTCS 03.11: witness word, counters (POST_CHAIN_STATS / LIGHTRN_STATS / GPU4_STATS period)
    render_fence::Presented();   // VS-STAGE FENCE_STATS (no-op unless =N); here, past the deferred re-entry: once per present
    tess_census::Presented();    // GPU-PASS TESS_CENSUS (no-op unless =N)
    pipeline_timing::Scope pipeline(pipeline_timing::QueuePresent, state_.get(),
        pipeline_timing::Pointer(present.get()), pipeline_timing::Pointer(this));
    static std::atomic<uint32_t> trace_enter{0}, trace_encoded{0};
    TraceFrame(trace_enter, "queue_present.enter", this, "type=%u state=%p backbuffer=%p after=%.6f",
               unsigned(desc_.Type), static_cast<const void *>(state_.get()), static_cast<const void *>(backbuffer), after);
    LARGE_INTEGER present_t0{};
    if (encoder_stats::Enabled()) QueryPerformanceCounter(&present_t0);
    auto scope = StartCommitting();
    scope.inflight.gap.kind = gpu_gaps::KindPresent;
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
    enc_census_observer::Presented(state_.get(), g->texture->current() ? g->texture->current()->texture().handle : 0);
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
