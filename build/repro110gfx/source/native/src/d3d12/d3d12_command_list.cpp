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
#include "d3d12_object_grid.hpp"
#include "d3d12_layer_time.hpp"   // CPU-LATE 23.10: DXMT_LT scopes
#include "indirect-mesh-contract.hpp"
#include "indirect-mesh-sync.hpp"

#include "d3d12_command_allocator.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_render_fence.hpp"   // VS-STAGE FENCE_STATS
#include "d3d12_gpu4_overlap.hpp"    // GPU4 TYPED_PRECISE / CLEAR_SINK
#include "d3d12_prerast_precise.hpp"   // GAPS PRERASTER_PRECISE
#include "d3d12_copy_gather.hpp"   // GAPS COPY_GATHER
#include "d3d12_pso_work.hpp"
#include "d3d12_binding_elision.hpp"
#include "d3d12_setter_dedup.hpp"   // GAPS 02.11 SETTER_DEDUP / VA_CACHE / OPEN_FAST
#include "d3d12_frame_trace.hpp"
#include "d3d12_pipeline_timing.hpp"
#include "d3d12_diagnostic_counters.hpp"
#include "d3d12_command_failure_trace.hpp"
#include "d3d12_recording_guard.hpp"
#include "d3d12_encoder_stats.hpp"
#include "d3d12_c17_stats.hpp"
#include "d3d12_vertex_audit.hpp"
#include "d3d12_reuse_commands.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_format.hpp"
#include "d3d12_enc_census.hpp"   // ENC-FIX LOAD_DONTCARE key
#include "d3d12_tess_census.hpp"   // GPU-PASS TESS_CENSUS
#include "d3d12_tess_prepass.hpp"   // GPU-PASS TESS_PREPASS
#include "d3d12_tess_schedule.hpp"
#include "d3d12_light_rename.hpp"   // LIGHTRN
#include "d3d12_post_chain.hpp"   // POSTCS 03.11 MACRUNNER_DX12_POST_CHAIN
#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <cstring>

namespace dxmt {

// ENC-FIX LOAD_DONTCARE: coverage proofs, one per (pipeline, viewport, start vertex / instance, render area) for the process.
// A proof is the visibility count of the key's first draw in a result slot of one shared buffer (Reset mode: the pass writes
// the slot); DontCare only once the slot was read back equal to the expected sample count.
namespace ldc_proof {
struct Key {
  uint64_t pso;
  float vp[6];
  uint32_t w, h, start_vertex, start_instance;
  bool operator==(const Key &o) const { return !std::memcmp(this, &o, sizeof(Key)); }
};
struct KeyHash {
  size_t operator()(const Key &k) const {
    const auto *b = reinterpret_cast<const uint8_t *>(&k);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(Key); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return size_t(h);
  }
};
enum State : uint8_t { Pending, Proven, Disproven };
struct Entry { State state; uint32_t slot; uint32_t pending_uses; uint64_t expected; };
constexpr uint32_t kSlots = 16384;
constexpr uint64_t kSentinel = ~0ull;
struct Registry {
  dxmt::mutex m;
  const void *device = nullptr;
  WMT::Reference<WMT::Buffer> buffer;
  uint64_t *slots = nullptr;
  uint32_t next = 0;
  std::unordered_map<Key, Entry, KeyHash> map;
};
inline Registry &Get() { static Registry r; return r; }
} // namespace ldc_proof

static bool
RecordingTraceEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_CAPTURE", value, sizeof(value)) == 1 &&
                        value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

enum class DirtyState {
  VertexBuffer,
  GraphicsRootArguments,
  GraphicsRootSignature,
  Viewport,
  ScissorRect,
  ComputeRootArguments,
  ComputeRootSignature,
  BlendFactor,
  StencilRef,
  GraphicsPipelineState,
  ComputePipelineState,
};

enum class DrawCallStatus {
  Invalid,
  Ordinary,
  Tessellation,
  Geometry,
};

inline bool
to_metal_primitive_type(D3D12_PRIMITIVE_TOPOLOGY topo, WMTPrimitiveType &primitive, uint32_t &control_point_num) {
  control_point_num = 0;
  switch (topo) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    primitive = WMTPrimitiveTypeLine;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    primitive = WMTPrimitiveTypeLineStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    primitive = WMTPrimitiveTypeTriangle;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    primitive = WMTPrimitiveTypeTriangleStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    // geometry
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST:
    primitive = WMTPrimitiveTypePoint;
    control_point_num = topo - 32;
    break;
  default:
    return false;
  }
  return true;
}

// `Graphics`CommandList is a really confusing name
class MTLD3D12GraphicsCommandListImpl : public MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList> {

  Com<MTLD3D12CommandAllocatorImpl, false> allocator_;
  EncoderData submission_head_{EncoderType::Null, nullptr};

  /* state */

  Flags<DirtyState> dirty_state_;

  std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex_buffers_;

  uint64_t index_buffer_address;
  WMT::Buffer index_buffer;
  Rc<BufferAllocation> index_allocation_;
  std::array<Com<ID3D12DescriptorHeap>, 2> bound_heaps_;
  std::array<root_argument::HeapRange, 2> bound_heap_ranges_{};
  WMTIndexType index_type;
  uint64_t index_offset;
  uint32_t index_buffer_size = 0;
  uint32_t mesh_index_variant_ = 0;
  // GPU-PASS TESS_PREPASS: what the last emitted root-argument / static-sampler / vertex-buffer-table bindings were (buffer +
  // offset), so the pre-pass kernels can bind the same; valid only for the pass they were emitted in.
  obj_handle_t tp_root_buffer_ = 0, tp_sampler_buffer_ = 0, tp_vb_buffer_ = 0;
  uint64_t tp_root_offset_ = 0, tp_sampler_offset_ = 0, tp_vb_offset_ = 0;
  EncoderData *tp_root_pass_ = nullptr, *tp_sampler_pass_ = nullptr, *tp_vb_pass_ = nullptr;
  // A later tess prepass waits for the preceding render pass's fence before writing these buffers again.
  // Draws of one pass have distinct slots. Different command-list recordings never share these handles.
  struct TessPassBuffer { WMT::Buffer buffer{}; size_t bytes = 0; };
  std::vector<TessPassBuffer> tp_pass_buffers_;
  EncoderData *tp_scratch_pass_ = nullptr;
  size_t tp_scratch_cursor_ = 0;
  // LIGHTRN (d3d12_light_rename.hpp, only while MACRUNNER_DX12_LIGHT_RENAME may be used): a candidate dispatch was recorded in
  // this list (render passes after it record their root-argument blocks); the root-argument command of the open compute pass.
  bool lightrn_seen_ = false;
  wmtcmd_compute_setbuffer *lightrn_croot_cmd_ = nullptr;
  const uint64_t *lightrn_croot_words_ = nullptr;
  EncoderData *lightrn_croot_pass_ = nullptr;

  UINT num_rtvs;
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;

  D3D12_PRIMITIVE_TOPOLOGY topology_;

  UINT num_viewports;
  D3D12_VIEWPORT
  viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  UINT num_scissors;
  D3D12_RECT
  scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};

  Com<MTLD3D12GraphicsPipelineState, false> pso_graphics_;
  Com<MTLD3D12RootSignature, false> rootsig_graphics_;
  root_argument::State rootarg_graphics_staging_;

  Com<MTLD3D12ComputePipelineState, false> pso_compute_;
  Com<MTLD3D12RootSignature, false> rootsig_compute_;
  root_argument::State rootarg_compute_staging_;

  FLOAT blend_factor_[4];
  UINT8 stencil_ref_;

  struct ActiveQuery {
    MTLD3D12QueryHeap *heap;
    D3D12_QUERY_TYPE type;
    UINT index;
    // ENCODERS (MACRUNNER_DX12_QUERY_INPASS=1): 0 = legacy (scratch + accumulate, results already valid),
    // 1 = direct, not yet selected by any pass (results stale), 2 = direct, counting in `pass`.
    uint8_t mode = 0;
    EncoderData *pass = nullptr;
  };
  std::vector<ActiveQuery> active_queries_;

  // ENCODERS level 4.3 (MACRUNNER_DX12_DIFF_STATE=1): state last emitted into the open recorded render pass; a dirty
  // state whose value equals it is not emitted again. Reset whenever a new render pass starts and after draws whose
  // encoding may change encoder state behind our back (indirect, tessellation, geometry, mesh).
  struct EmittedState {
    bool pso = false, vb = false, root = false, sampler = false, vp = false, sc = false, blend = false, stencil = false;
    obj_handle_t pso_handle = 0, dss_handle = 0;
    uint8_t raster[sizeof(wmtcmd_render_setrasterizerstate)] = {};
    bool vb_mesh = false, root_mesh = false, sampler_mesh = false;
    uint32_t vb_mask = 0;
    std::array<D3D12_VERTEX_BUFFER_VIEW, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vb_views{};
    const void *root_sig = nullptr;
    uint32_t root_qwords = 0;
    uint64_t root_words[64] = {};
    const void *sampler_sig = nullptr;
    UINT vp_count = 0, sc_count = 0, sc_vp_count = 0;
    D3D12_VIEWPORT vp_values[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    D3D12_RECT sc_values[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    FLOAT blend_values[4] = {};
    UINT8 stencil_value = 0;
  } emitted_;
  void ForgetEmittedState() { emitted_ = EmittedState{}; }
  void DropDiscards() { pending_discards_.clear(); }

  // ENCODERS level 4.2 (MACRUNNER_DX12_TILE_MEMORY=1): textures discarded since the last pass; the next new render pass
  // that has one of them as an attachment loads it with DontCare. Any other operation that could observe them drops the list.
  std::vector<obj_handle_t> pending_discards_;
  // ENC-FIX LOAD_DONTCARE: set by PreDraw when it opened a new render pass (consumed by the very next draw); the view sizes
  // and slice counts of the pass's attachments (0-7 colours, 8 depth).
  bool ldc_first_draw_ = false;
  bool ldc_probe_after_ = false;
  uint32_t ldc_start_vertex_ = 0, ldc_start_instance_ = 0;   // the draw just encoded is a coverage probe: stop counting after it
  uint32_t ldc_probe_slot_ = 0;
  uint32_t ldc_w_[9] = {}, ldc_h_[9] = {}, ldc_layers_[9] = {};

  // Reason recorded on passes ended inside the scope; the outermost scope wins.
  struct CloseReasonScope {
    MTLD3D12CommandAllocatorImpl *allocator;
    uint8_t previous;
    CloseReasonScope(MTLD3D12CommandAllocatorImpl *a, uint8_t reason) : allocator(a), previous(a ? a->close_reason_hint : 0) {
      if (allocator && !previous) allocator->close_reason_hint = reason;
    }
    ~CloseReasonScope() { if (allocator) allocator->close_reason_hint = previous; }
  };
#define ENCODERS_REASON(r) CloseReasonScope close_reason_scope_(allocator_.ptr(), encoder_stats::r)

  // ENCODERS: work that floats to the next pass boundary.
  //  - resolves of occlusion results (MACRUNNER_DX12_QUERY_INPASS=1), recorded while a pass is open;
  //  - copies and immediate writes on resources in copy states (MACRUNNER_DX12_DEFER_COPIES=1 inside an open render pass,
  //    MACRUNNER_DX12_BLIT_CHAIN=1 anywhere), recorded into side blit passes that are not linked yet.
  // D3D12 state rules make this safe: a resource in COPY_DEST is not accessible to shaders, COPY_SOURCE only for reads,
  // and any later shader use needs a barrier to a non-copy state, which ends the pass and flushes these items first.
  // At the flush the items are packed into as few blit passes as their byte ranges allow (conflict = same object,
  // overlapping range, one of the two writes; a conflicting item goes to a later blit pass, i.e. behind a fence).
  struct DeferredResolve {
    D3D12_QUERY_TYPE type;
    WMT::Reference<WMT::Buffer> results;
    WMT::Reference<WMT::ComputePipelineState> binary_resolve;
    WMT::Reference<WMT::Buffer> dst;
    UINT start, count;
    uint64_t dst_offset;
  };
  struct AccessRange {
    obj_handle_t object;
    uint64_t begin, end;
    bool write;
  };
  struct DeferredItem {
    uint8_t kind = 0;             // 0 occlusion resolve, 1 binary resolve, 2 side blit pass
    DeferredResolve resolve{};
    BlitEncoderData *side = nullptr;
    std::vector<AccessRange> access;
    bool marker = false;          // c17 DEFER_MARKERS: a side pass holding exactly one 4-byte marker write (access[0])
  };
  std::vector<DeferredItem> deferred_;
  uint32_t absorbed_barriers_ = 0;   // copy-state barriers that did not end a pass (OUT join kept at the flush)
  bool absorbed_common_ = false;     // GAPS COPY_GATHER: one of them had StateBefore COMMON
  bool side_active_ = false;        // a side blit pass is allocator_->encoder_current

  static bool Conflict(const DeferredItem &a, const DeferredItem &b) {
    for (const auto &x : a.access)
      for (const auto &y : b.access)
        if (x.object == y.object && (x.write || y.write) && x.begin < y.end && y.begin < x.end)
          return true;
    return false;
  }
  // c17 DEFER_MARKERS: an earlier floating marker write to exactly these 4 bytes, with no floating item after it touching
  // them, is never observed (nothing may read the destination before the flush, and the flush keeps recording order): drop it.
  void DropDeadMarker(const AccessRange &range) {
    for (size_t k = deferred_.size(); k-- > 0;) {
      auto &item = deferred_[k];
      if (item.marker && item.access.size() == 1 && item.access[0].object == range.object &&
          item.access[0].begin == range.begin && item.access[0].end == range.end) {
        item.side->~BlitEncoderData();
        deferred_.erase(deferred_.begin() + k);
        c17::Note(c17::Get().markers_dropped);
        return;
      }
      for (const auto &x : item.access)
        if (x.object == range.object && x.begin < range.end && range.begin < x.end)
          return;   // something in between reads or writes these bytes: the earlier write stays
    }
  }
  // c17 QUERY_NO_COLOR: at Close(), a render pass whose draws all used pipelines with a colourless twin (no pixel shader)
  // and that has a depth or stencil attachment loses its colour attachments: they are neither loaded nor stored, the draws
  // use the twins. Decided once per recording, so every execution encodes the same pass.
  void DropColorsOfColorlessPasses() {
    for (auto pass = entry; pass; pass = pass->next) {
      if (pass->type != EncoderType::Render) continue;
      auto r = static_cast<RenderEncoderData *>(pass);
      bool colors = false, resolve = false;
      for (auto &c : r->colors) { colors |= bool(c.attachment); resolve |= bool(c.resolve_attachment); }
      if (!colors) continue;
      if (!r->no_color_ok || r->no_color_cmds.empty()) { c17::Note(c17::Get().noc_reject_pipeline); continue; }
      if (!r->depth.attachment && !r->stencil.attachment) { c17::Note(c17::Get().noc_reject_no_depth); continue; }
      if (resolve) { c17::Note(c17::Get().noc_reject_resolve); continue; }
      // Every pipeline command of the pass must be one with a twin (helpers or indirect buffers would bring their own).
      bool own = true;
      for (auto cmd = reinterpret_cast<wmtcmd_base *>(&r->cmd_head); cmd && own; cmd = static_cast<wmtcmd_base *>(cmd->next.ptr)) {
        const auto type = WMTRenderCommandType(cmd->type);
        if (type == WMTRenderCommandExecuteCommandsInBuffer) own = false;
        else if (type == WMTRenderCommandSetPSO)
          own = std::any_of(r->no_color_cmds.begin(), r->no_color_cmds.end(),
                            [&](const auto &x) { return reinterpret_cast<wmtcmd_base *>(x.first) == cmd; });
      }
      if (!own) { c17::Note(c17::Get().noc_reject_pipeline); continue; }
      for (auto &c : r->colors) c = RenderEncoderColorAttachmentData{};
      for (auto &[cmd, twin] : r->no_color_cmds) cmd->pso = twin;
      r->no_color_cmds.clear();
      c17::Note(c17::Get().noc_converted);
      if (!r->visibility_targets.empty() || r->visibility_direct) c17::Note(c17::Get().noc_converted_query);
    }
  }
  void PushResolve(const DeferredResolve &r) {
    DeferredItem item;
    item.kind = r.type == D3D12_QUERY_TYPE_OCCLUSION ? 0 : 1;
    item.resolve = r;
    item.access.push_back({r.results.handle, uint64_t(r.start) * 8, uint64_t(r.start + r.count) * 8, false});
    item.access.push_back({r.dst.handle, r.dst_offset, r.dst_offset + uint64_t(r.count) * 8, true});
    deferred_.push_back(std::move(item));
  }
  BlitEncoderData *NewBlitPass() {
    allocator_->InvalidateCurrentPass();
    auto blit = allocator_->AllocatePass<BlitEncoderData>();
    blit->type = EncoderType::Blit;
    blit->cmd_head.type = WMTBlitCommandNop;
    blit->cmd_head.next.set(0);
    blit->cmd_tail = (wmtcmd_base *)&blit->cmd_head;
    return blit;
  }
  void EmitBinaryResolve(const DeferredResolve &d) {
    allocator_->InvalidateCurrentPass();
    auto compute = allocator_->AllocatePass<ComputeEncoderData>();
    compute->type = EncoderType::Compute;
    compute->cmd_head.type = WMTComputeCommandNop;
    compute->cmd_head.next.set(0);
    compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
    compute->query_buffers.push_back(d.results);
    compute->query_buffers.push_back(d.dst);
    compute->query_pipeline = d.binary_resolve;
    auto &pipeline = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    pipeline.type = WMTComputeCommandSetPSO;
    pipeline.pso = d.binary_resolve;
    pipeline.threadgroup_size = {1, 1, 1};
    auto &src_buffer = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    src_buffer.type = WMTComputeCommandSetBuffer;
    src_buffer.buffer = d.results;
    src_buffer.offset = uint64_t(d.start) * sizeof(uint64_t);
    src_buffer.index = 0;
    auto &dst_buffer = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    dst_buffer.type = WMTComputeCommandSetBuffer;
    dst_buffer.buffer = d.dst;
    dst_buffer.offset = d.dst_offset;
    dst_buffer.index = 1;
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {d.count, 1, 1};
    // Internal compute state must never leak into an application's next dispatch.
    allocator_->InvalidateCurrentPass();
  }
  // Items [b, e) of one segment (no binary resolve inside): fewest blit passes the conflicts allow, list order kept per pass.
  void EmitSegment(std::vector<DeferredItem> &items, size_t b, size_t e) {
    if (b == e) return;
    std::vector<uint16_t> phase(e - b, 0);
    uint16_t last = 0;
    for (size_t i = b; i < e; ++i) {
      uint16_t p = 0;
      for (size_t j = b; j < i; ++j)
        if (phase[j - b] + 1 > p && Conflict(items[j], items[i]))
          p = phase[j - b] + 1;
      phase[i - b] = p;
      last = std::max(last, p);
    }
    for (uint16_t ph = 0; ph <= last; ++ph) {
      auto pass = NewBlitPass();
      if (copy_gather::Enabled()) {   // GAPS COPY_GATHER: only plain deferred copy work with known objects may move at Close
        bool movable = true;
        for (size_t i = b; i < e && movable; ++i) {
          if (phase[i - b] != ph) continue;
          const auto &item = items[i];
          if (item.kind != 2 || !item.side || item.marker || !item.side->query_buffers.empty() ||
              !item.side->immediate_buffers.empty() || item.side->join_immediate_out || item.access.empty()) movable = false;
          for (const auto &a : item.access) {
            if (!a.object) { movable = false; continue; }
            pass->gaps_objects.push_back(a.object);
            if (a.write) pass->gaps_writes.push_back(a.object);
          }
        }
        pass->gaps_movable = movable;
        if (!movable) { pass->gaps_objects.clear(); pass->gaps_writes.clear(); }
      }
      for (size_t i = b; i < e; ++i) {
        if (phase[i - b] != ph) continue;
        auto &item = items[i];
        if (item.kind == 0) {
          const auto &d = item.resolve;
          pass->query_buffers.push_back(d.results);
          pass->query_buffers.push_back(d.dst);
          auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          copy.type = WMTBlitCommandCopyFromBufferToBuffer;
          copy.src = d.results;
          copy.src_offset = uint64_t(d.start) * sizeof(uint64_t);
          copy.dst = d.dst;
          copy.dst_offset = d.dst_offset;
          copy.copy_length = uint64_t(d.count) * sizeof(uint64_t);
        } else if (item.side) {
          auto side = item.side;
          if (side->cmd_head.next.ptr) {
            pass->cmd_tail->next.set(side->cmd_head.next.ptr);
            pass->cmd_tail = side->cmd_tail;
          }
          for (auto &q : side->query_buffers) pass->query_buffers.push_back(std::move(q));
          for (auto &q : side->immediate_buffers) pass->immediate_buffers.push_back(std::move(q));
          pass->join_immediate_out |= side->join_immediate_out;
          side->~BlitEncoderData();
          item.side = nullptr;
        }
      }
      if (ph) encoder_stats::Note(encoder_stats::Get().deferred_phase_splits);
    }
    allocator_->InvalidateCurrentPass();
  }
  // Emit deferred work as passes at the current position (after the open pass).
  void FlushDeferredResolves() {
    if ((deferred_.empty() && !absorbed_barriers_) || side_active_ || !allocator_.ptr() || !allocator_->encoder_last)
      return;
    encoder_stats::Note(encoder_stats::Get().resolve_flushes);
    auto items = std::move(deferred_);
    deferred_.clear();
    const auto absorbed = absorbed_barriers_;
    absorbed_barriers_ = 0;
    const bool absorbed_common = absorbed_common_;
    absorbed_common_ = false;
    if (absorbed) {
      // The absorbed barriers still join a pending MARKER_OUT write (skipped at execution when none is pending).
      // The join goes BEFORE the floating work: a copy behind an absorbed barrier may read what that MARKER_OUT wrote,
      // and an OUT write is ordered only by its own fence (c17 TESS-GS 17.10: joined after the copies, the copy raced it).
      auto pass = NewBlitPass();
      pass->join_immediate_out = true;
      pass->barrier_only = true;
      pass->barrier_records_valid = true;   // copy-state barriers: they touch no attachment, no shader state
      pass->gaps_join = true;               // GAPS COPY_GATHER: moves with the copies that follow it
      pass->gaps_common = absorbed_common;  // ... unless an absorbed barrier came from COMMON: then they stay
      allocator_->InvalidateCurrentPass();
    }
    size_t segment = 0;
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].kind != 1) continue;
      EmitSegment(items, segment, i);
      EmitBinaryResolve(items[i].resolve);
      segment = i + 1;
    }
    EmitSegment(items, segment, items.size());
  }
  // End the open pass; deferred work goes out first (it was recorded inside it).
  void EndPass() {
    FlushDeferredResolves();
    allocator_->InvalidateCurrentPass();
  }
  // May blit work on copy-state resources float to the next pass boundary now?
  bool CanDeferBlit() const {
    if (!allocator_.ptr() || !allocator_->encoder_last || side_active_)
      return false;
    const auto current = allocator_->encoder_current;
    // Not while a blit pass is open: its own commands have no declared ranges, so a floating copy could not join it
    // and would only add a pass (measured 15.10: +1 per list in mtprobe).
    if (encoder_stats::BlitChain())
      return !current || current->type == EncoderType::Render || current->type == EncoderType::Compute;
    return encoder_stats::DeferCopies() && current && current->type == EncoderType::Render;
  }
  // Records the blit commands of one copy API call into a side pass (when allowed), then restores the open pass.
  struct SideBlitScope {
    MTLD3D12GraphicsCommandListImpl *list;
    EncoderData *saved = nullptr;
    BlitEncoderData *side = nullptr;
    std::vector<AccessRange> access;
    SideBlitScope(MTLD3D12GraphicsCommandListImpl *l, std::vector<AccessRange> &&a) : list(l) {
      if (a.empty() || !list->CanDeferBlit())
        return;
      access = std::move(a);
      saved = list->allocator_->encoder_current;
      side = list->allocator_->AllocatePass<BlitEncoderData>();
      side->type = EncoderType::Blit;
      side->cmd_head.type = WMTBlitCommandNop;
      side->cmd_head.next.set(0);
      side->cmd_tail = (wmtcmd_base *)&side->cmd_head;
      list->side_active_ = true;
    }
    ~SideBlitScope() {
      if (!side)
        return;
      list->side_active_ = false;
      list->allocator_->encoder_current = saved;
      if (!side->cmd_head.next.ptr) {
        side->~BlitEncoderData();
        return;
      }
      DeferredItem item;
      item.kind = 2;
      item.side = side;
      item.access = std::move(access);
      list->deferred_.push_back(std::move(item));
      encoder_stats::Note(encoder_stats::Get().deferred_blits);
    }
  };
  static obj_handle_t ResourceObject(ID3D12Resource *resource) {
    auto r = static_cast<MTLD3D12Resource *>(resource);
    if (!r) return 0;
    if (r->buffer) return r->buffer->current()->buffer().handle;
    if (r->texture) return r->texture->current()->texture().handle;
    return 0;
  }
  static bool IsCopyState(D3D12_RESOURCE_STATES s) {
    return s == D3D12_RESOURCE_STATE_COPY_DEST || s == D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  // Transitions between copy states only (COMMON allowed as the state before): no shader can be affected.
  static bool CopyStateBarriers(UINT count, const D3D12_RESOURCE_BARRIER *barriers) {
    for (UINT i = 0; i < count; ++i) {
      const auto &b = barriers[i];
      if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
          !b.Transition.pResource)
        return false;
      if (!IsCopyState(b.Transition.StateAfter) ||
          !(IsCopyState(b.Transition.StateBefore) || b.Transition.StateBefore == D3D12_RESOURCE_STATE_COMMON))
        return false;
    }
    return count != 0;
  }
  // UAV barriers and transitions among shader-access states: a memory barrier inside one compute encoder suffices.
  static bool ComputeChainBarriers(UINT count, const D3D12_RESOURCE_BARRIER *barriers) {
    constexpr UINT shader = D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (UINT i = 0; i < count; ++i) {
      const auto &b = barriers[i];
      if (b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
        return false;
      if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
        continue;
      if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        return false;
      const UINT before = UINT(b.Transition.StateBefore), after = UINT(b.Transition.StateAfter);
      if (!before || !after || (before & ~shader) || (after & ~shader))
        return false;
    }
    return count != 0;
  }
  bool DeferredResolveReads(const MTLD3D12QueryHeap *heap, UINT index) const {
    for (const auto &item : deferred_) {
      if (item.kind == 2) continue;
      const auto &d = item.resolve;
      if (d.results.handle == heap->results.handle && index >= d.start && index - d.start < d.count)
        return true;
    }
    return false;
  }
  void EmitVisibilityMode(WMTVisibilityResultMode mode, UINT index) {
    auto &visibility = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
    visibility.type = WMTRenderCommandSetVisibilityMode;
    visibility.mode = mode;
    visibility.offset = uint64_t(index) * sizeof(uint64_t);
  }
  // Start counting a sole direct query in the open render pass; false = the pass cannot take it.
  bool SelectDirectQuery(ActiveQuery &query) {
    auto current = allocator_->encoder_current;
    if (!current || current->type != EncoderType::Render)
      return false;
    auto render = static_cast<RenderEncoderData *>(current);
    if (!render->visibility_targets.empty() || render->ldc_probe)   // ENC-FIX LDC: no application query beside a probe
      return false;
    if (render->visibility_direct && render->visibility_direct.handle != query.heap->results.handle)
      return false;
    for (auto slot : render->direct_slots)
      if (slot == query.index)
        return false;   // Reset mode would add the second activation to the first one in the same pass
    render->visibility_direct = query.heap->results;
    render->direct_slots.push_back(query.index);
    // Counting also for BINARY_OCCLUSION: the results buffer keeps sample counts, as on the legacy path;
    // the binary resolve turns them into 0/1.
    EmitVisibilityMode(WMTVisibilityResultModeCounting, query.index);
    query.mode = 2;
    query.pass = render;
    return true;
  }
  HRESULT recording_error_ = S_OK;
  void AuditReuseWrite(ID3D12Resource *resource) {
    if (!reuse::Active() || !allocator_ || !allocator_->encoder_current) return;
    auto pass=allocator_->encoder_current;
    try {
      if (!pass->reuse_record) pass->reuse_record=std::make_shared<reuse::RecordedPass>();
      pass->reuse_record->unknown|=reuse::UnsupportedCommand;
      if (resource) pass->reuse_record->extra_writes.push_back(static_cast<MTLD3D12Resource *>(resource)->reuse_resource);
    } catch (...) { if (pass->reuse_record) pass->reuse_record->unknown|=reuse::MissingResource; }
  }
  void AuditReuseDraw(bool compute, std::initializer_list<uint64_t> args, bool indexed = false,
                      MTLD3D12CommandSignature *indirect = nullptr, uint64_t address = 0,
                      uint64_t count_address = 0, uint32_t max_count = 0) {
    if (!reuse::Active() || !allocator_ || !allocator_->encoder_current) return;
    auto pass = allocator_->encoder_current;
    try {
      if (!pass->reuse_record) pass->reuse_record = std::make_shared<reuse::RecordedPass>();
      auto &record = *pass->reuse_record; ++record.count;
      if (record.draws.size() >= 4096) { record.unknown |= reuse::DrawLimit; return; }
      reuse::RecordedDraw d;
      d.pso = compute ? (pso_compute_ ? pso_compute_->reuse_id : 0) : (pso_graphics_ ? pso_graphics_->reuse_id : 0);
      d.shader = compute ? (pso_compute_ ? pso_compute_->reuse_shader : nullptr) : (pso_graphics_ ? pso_graphics_->reuse_shader : nullptr);
      d.root = compute ? rootsig_compute_ : rootsig_graphics_;
      d.arguments = compute ? rootarg_compute_staging_ : rootarg_graphics_staging_;
      d.heaps = bound_heaps_; d.heap_ranges = bound_heap_ranges_;
      d.state.Word(reuse::Pipeline, d.pso);
      d.state.Word(reuse::Root, d.root ? d.root->reuse_id : 0);
      for (auto value : args) d.state.Word(reuse::Arguments, value);
      if (indirect) {
        d.state.Word(reuse::Arguments,indirect->reuse_id);
        d.state.Word(reuse::Arguments,indirect->reuse_stride);
        d.inputs.push_back(reuse::AddressBinding(address,uint64_t(indirect->reuse_stride)*max_count));
        if (count_address) d.inputs.push_back(reuse::AddressBinding(count_address,4));
        // GPU-generated root/VB/IB addresses cannot be reconstructed on the CPU.
        // Still account the command and all known inputs, never count it as a hit.
        if (indirect->UpdateRootArguments || indirect->UpdateVertexBuffers || indirect->UpdateIndexBuffer)
          d.state.unknown |= reuse::IndirectMutation;
      }
      if (!compute) {
        d.state.Word(reuse::Raster, topology_); d.state.Word(reuse::Raster, stencil_ref_);
        d.state.Bytes(reuse::Raster, blend_factor_, sizeof(blend_factor_));
        d.state.Bytes(reuse::Raster, viewports, size_t(num_viewports) * sizeof(*viewports));
        d.state.Bytes(reuse::Raster, scissors, size_t(num_scissors) * sizeof(*scissors));
        const uint32_t mask = pso_graphics_ ? pso_graphics_->slot_mask : 0;
        d.state.Word(reuse::Descriptors, mask);
        for (unsigned i = 0; i < vertex_buffers_.size(); ++i) if (mask & (1u << i)) {
          const auto &v = vertex_buffers_[i];
          d.state.Word(reuse::Descriptors, v.StrideInBytes);
          d.inputs.push_back(reuse::AddressBinding(v.BufferLocation, v.SizeInBytes));
        }
        if (indexed) {
          d.state.Word(reuse::Descriptors, index_type);
          d.inputs.push_back(reuse::AddressBinding(index_buffer_address, index_buffer_size));
        }
        if (!active_queries_.empty()) d.state.unknown |= reuse::UnsupportedCommand;
      }
      record.Append(std::move(d));
    } catch (...) {
      if (pass->reuse_record) pass->reuse_record->unknown |= reuse::DrawLimit;
    }
  }
  // XL-GAPS (MACRUNNER_DX12_XL_HAZARD=1): the descriptor spans this graphics draw can read, once per change of the
  // binding epoch / pipeline / root signature inside the pass. Observation only; the draw itself is unchanged.
  void AuditHazardDraw(bool indirect_mutation = false) {
    if (!xl_hazard::Enabled() || !allocator_ || !allocator_->encoder_current ||
        allocator_->encoder_current->type != EncoderType::Render) return;
    auto &render = *static_cast<RenderEncoderData *>(allocator_->encoder_current);
    try {
      if (!render.xl_hazard) render.xl_hazard = std::make_shared<xl_hazard::PassBindings>();
      auto &pass = *render.xl_hazard;
      ++pass.draws;
      const void *pso = pso_graphics_.ptr(), *root = rootsig_graphics_.ptr();
      if (!indirect_mutation && !pass.snapshots.empty() && pass.last_epoch == empty_binding_epoch && pass.last_pso == pso &&
          pass.last_root == root) { ++pass.snapshots.back().draws; return; }
      pass.last_epoch = empty_binding_epoch; pass.last_pso = pso; pass.last_root = root;
      xl_hazard::Snapshot snap; snap.draws = 1; snap.first_span = uint32_t(pass.spans.size());
      if (indirect_mutation) snap.flags |= 2;
      const auto *decl = pso_graphics_ ? pso_graphics_->xl_hazard_decl.get() : nullptr;
      const bool known = decl && decl->known;
      if (!rootsig_graphics_) snap.flags |= 1;
      else {
        const auto &layout = rootsig_graphics_->ArgumentLayout;
        const auto &parameters = rootsig_graphics_->reuse_parameters;
        if (parameters.size() != layout.count) snap.flags |= 1;
        else for (uint32_t index = 0; index < layout.count; ++index) {
          const auto &p = parameters[index];
          if (layout.parameters[index].kind != root_argument::Table) continue;
          for (const auto &range : p.ranges) {
            if (range.type > 1) continue;   // CBV / sampler ranges: never an attachment
            const auto &heap_range = bound_heap_ranges_[0];
            const uint64_t lo = range.binding;
            const uint64_t hi = range.count == UINT32_MAX ? UINT64_MAX : uint64_t(range.binding) + range.count - 1;
            // registers of this range the pipeline declares (all of them when the declarations are unknown)
            std::vector<std::pair<uint64_t, uint64_t>> regs;
            if (!known) regs.push_back({lo, hi});
            else for (const auto &d : decl->ranges) {
              if (d.uav != (range.type == 1) || d.space != range.space) continue;
              if (p.visibility != 0 && p.visibility != d.visibility) continue;
              const uint64_t a = std::max<uint64_t>(lo, d.lower), b = std::min<uint64_t>(hi, d.upper);
              if (a <= b) regs.push_back({a, b});
            }
            root_argument::TableLocation loc;
            if (!bound_heaps_[0] || !root_argument::ReadTable(layout, rootarg_graphics_staging_, index, heap_range,
                                                             range.offset, 1, loc)) {
              if (!regs.empty()) snap.flags |= 1;   // the shader may read it, but the table cannot be read
              continue;
            }
            const uint64_t room = uint64_t(heap_range.count) - loc.index;
            const uint64_t count = std::min<uint64_t>(range.count, room);
            if (!count) continue;
            xl_hazard::Span span; span.heap = bound_heaps_[0].ptr(); span.start = loc.index; span.count = uint32_t(count);
            span.uav = uint8_t(range.type == 1); span.declared = known; span.first_decl = uint32_t(pass.decls.size());
            for (const auto &[a, b] : regs) {
              // register r -> descriptor loc.index + (r - range.binding), clamped to the heap
              const uint64_t first = uint64_t(loc.index) + (a - lo);
              if (first >= heap_range.count) continue;
              const uint64_t n = std::min<uint64_t>(b - a + 1, uint64_t(heap_range.count) - first);
              pass.decls.push_back({uint32_t(first), uint32_t(n)}); ++span.decl_count;
            }
            pass.spans.push_back(span); ++snap.span_count;
          }
        }
      }
      if (snap.span_count && (pass.heaps.empty() || pass.heaps.back().ptr() != bound_heaps_[0].ptr()))
        pass.heaps.push_back(bound_heaps_[0]);
      pass.snapshots.push_back(snap);
    } catch (...) {}
  }
  // GPU4 (в): geometry census of the big passes (GPU4_STATS only; counting, the draw is unchanged).
  // kind: 0 non-indexed, 1 indexed, 2 indirect
  void CountGpu4Geometry(int kind, uint64_t count, uint64_t instances) {
    if (!gpu4::StatsPeriod() || !gpu4::Armed() || !allocator_ || !allocator_->encoder_current ||
        allocator_->encoder_current->type != EncoderType::Render) return;
    const auto &render = *static_cast<const RenderEncoderData *>(allocator_->encoder_current);
    const bool g = render.render_target_count >= 4;
    const bool sh = !render.render_target_count && render.depth.attachment && render.render_target_width >= 2048;
    if (!g && !sh) return;
    auto &st = gpu4::Get();
    if (kind == 2) { gpu4::Note(sh ? st.ind_s : st.ind_g); return; }
    if (instances > 1) gpu4::Note(sh ? st.inst_s : st.inst_g);
    const uint64_t n = count * instances;
    if (kind == 0) { gpu4::Note(sh ? st.vx_s : st.vx_g, n); return; }
    const bool i32 = index_type == WMTIndexTypeUInt32;
    gpu4::Note(sh ? (i32 ? st.ix_s32 : st.ix_s16) : (i32 ? st.ix_g32 : st.ix_g16), n);
    gpu4::Note(sh ? (i32 ? st.dr_s32 : st.dr_s16) : (i32 ? st.dr_g32 : st.dr_g16));
  }
  // LIGHTRN (d3d12_light_rename.hpp): recording while MACRUNNER_DX12_LIGHT_RENAME may be used. A compute pass counts its dispatches
  // and records the one with a form (with its root-argument block); render passes recorded after such a dispatch in this list
  // record every root-argument block they bind. Nothing here changes what is encoded.
  void LightrnCountDispatch() {
    auto *cur = allocator_->encoder_current;
    if (!cur || cur->type != EncoderType::Compute) return;
    auto &c = *static_cast<ComputeEncoderData *>(cur);
    try {
      if (!c.lightrn) c.lightrn = std::make_shared<light_rename::PassDispatch>();
      ++c.lightrn->dispatches;
    } catch (...) {}
  }
  void LightrnRecordDispatch(UINT X, UINT Y, UINT Z) {
    auto *cur = allocator_->encoder_current;
    if (!cur || cur->type != EncoderType::Compute) return;
    auto &c = *static_cast<ComputeEncoderData *>(cur);
    if (!c.lightrn) return;
    if (lightrn_croot_pass_ != cur || !lightrn_croot_cmd_ || !rootsig_compute_) { c.lightrn->unknown = true; return; }
    try {
      light_rename::DispatchRec d;
      d.root_cmd = lightrn_croot_cmd_; d.words = lightrn_croot_words_; d.root = rootsig_compute_.ptr();
      d.heap = bound_heap_ranges_[0]; d.heap_object = bound_heaps_[0].ptr();
      d.groups[0] = X; d.groups[1] = Y; d.groups[2] = Z;
      d.threads[0] = pso_compute_->threadgroup_size.width; d.threads[1] = pso_compute_->threadgroup_size.height;
      d.threads[2] = pso_compute_->threadgroup_size.depth;
      d.form = pso_compute_->lightrn_form;
      c.lightrn->d.push_back(std::move(d));
      lightrn_seen_ = true;
    } catch (...) { c.lightrn->unknown = true; }
  }
  light_rename::PassRoots *LightrnRoots() {
    auto *cur = allocator_ ? allocator_->encoder_current : nullptr;
    if (!cur || cur->type != EncoderType::Render) return nullptr;
    auto &r = *static_cast<RenderEncoderData *>(cur);
    if (!r.lightrn_roots) r.lightrn_roots = std::make_shared<light_rename::PassRoots>();
    return r.lightrn_roots.get();
  }
  void LightrnRecordRoot(wmtcmd_render_setbuffer *a, wmtcmd_render_setbuffer *b, wmtcmd_render_setbuffer *c, uint64_t offset) {
    light_rename::PassRoots *pr = nullptr;
    try {
      pr = LightrnRoots();
      if (!pr) return;
      light_rename::RootRec rr;
      for (auto *x : {a, b, c}) if (x) rr.cmd[rr.ncmd++] = x;
      rr.words = reinterpret_cast<const uint64_t *>(static_cast<const uint8_t *>(allocator_->gpu_heap_) + offset);
      rr.root = rootsig_graphics_.ptr(); rr.heap = bound_heap_ranges_[0]; rr.heap_object = bound_heaps_[0].ptr();
      pr->roots.push_back(rr);
    } catch (...) { if (pr) pr->unknown = true; }
  }
  void LightrnRootsUnknown() {
    if (!lightrn_seen_ || !light_rename::Armed()) return;
    try { if (auto *pr = LightrnRoots()) pr->unknown = true; } catch (...) {}
  }
  // GPU4 (d3d12_gpu4_overlap.hpp): the descriptor spans this draw can read — the SRV registers of the pre-raster stages for
  // TYPED_PRECISE (only pipelines whose typed pre-raster reads are all SRVs) and of every stage for CLEAR_SINK (only big
  // passes). Once per change of binding epoch / pipeline / root signature inside the pass; the draw itself is unchanged.
  void RecordGpu4Reads(bool indirect_mutation = false) {
    if (!gpu4::Armed() || !allocator_ || !allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render ||
        !pso_graphics_) return;
    auto &render = *static_cast<RenderEncoderData *>(allocator_->encoder_current);
    const bool typed_draw = pso_graphics_->pre_raster_reads_typed && pso_graphics_->pre_raster_srv_only;
    const bool big = render.render_target_count >= 4 ||
                     (!render.render_target_count && render.depth.attachment && render.render_target_width >= 2048);
    const bool want_pre = gpu4::TypedPrecise() && typed_draw;
    const bool want_all = (gpu4::ClearSink() || gpu4::ShadowLate()) && big;   // SHADOW_LATE needs the same set
    // keys switched on the fly: a draw of this pass not recorded makes its set incomplete (= unknown)
    if (typed_draw && !want_pre) render.gpu4_pre_missed = true;
    if (big && !want_all) render.gpu4_all_missed = true;
    if (!want_pre && !want_all) return;
    try {
      if (!render.gpu4_reads) render.gpu4_reads = std::make_shared<gpu4::Reads>();
      auto &r = *render.gpu4_reads;
      r.want_all |= want_all;
      const void *pso = pso_graphics_.ptr(), *root = rootsig_graphics_.ptr();
      if (!indirect_mutation && r.last_epoch == empty_binding_epoch && r.last_pso == pso && r.last_root == root) return;
      r.last_epoch = empty_binding_epoch; r.last_pso = pso; r.last_root = root;
      const auto unknown = [&] { if (want_pre) r.pre_unknown = true; if (want_all) r.all_unknown = true; };
      if (indirect_mutation) { unknown(); return; }
      if (want_all && pso_graphics_->gpu4_heap_indexing) r.all_unknown = true;
      const auto *decl = pso_graphics_->xl_hazard_decl.get();
      if (!decl || !decl->known || !rootsig_graphics_) { unknown(); return; }
      const auto &layout = rootsig_graphics_->ArgumentLayout;
      const auto &parameters = rootsig_graphics_->reuse_parameters;
      if (parameters.size() != layout.count) { unknown(); return; }
      bool heap_added = false;
      for (uint32_t index = 0; index < layout.count; ++index) {
        const auto &p = parameters[index];
        if (layout.parameters[index].kind != root_argument::Table) continue;
        for (const auto &range : p.ranges) {
          if (range.type != 0) continue;   // SRV ranges only (UAV: the texture would be in UAV state; CBV / sampler: never a texture)
          const auto &heap_range = bound_heap_ranges_[0];
          const uint64_t lo = range.binding;
          const uint64_t hi = range.count == UINT32_MAX ? UINT64_MAX : uint64_t(range.binding) + range.count - 1;
          for (int pass = 0; pass < 2; ++pass) {
            if ((pass == 0 && !want_pre) || (pass == 1 && !want_all)) continue;
            std::vector<std::pair<uint64_t, uint64_t>> regs;
            for (const auto &d : decl->ranges) {
              if (d.uav || d.space != range.space) continue;
              if (pass == 0 && (d.visibility < 1 || d.visibility > 4)) continue;   // VS / HS / DS / GS
              if (p.visibility != 0 && p.visibility != d.visibility) continue;
              const uint64_t a = std::max<uint64_t>(lo, d.lower), b = std::min<uint64_t>(hi, d.upper);
              if (a <= b) regs.push_back({a, b});
            }
            if (regs.empty()) continue;
            root_argument::TableLocation loc;
            if (!bound_heaps_[0] || !root_argument::ReadTable(layout, rootarg_graphics_staging_, index, heap_range, range.offset, 1, loc)) {
              (pass == 0 ? r.pre_unknown : r.all_unknown) = true;
              continue;
            }
            for (const auto &[a, b] : regs) {
              const uint64_t first = uint64_t(loc.index) + (a - lo);
              if (first >= heap_range.count) { (pass == 0 ? r.pre_unknown : r.all_unknown) = true; continue; }
              const uint64_t n = std::min<uint64_t>(b - a + 1, uint64_t(heap_range.count) - first);
              if (n > gpu4::kSpanCap) { (pass == 0 ? r.pre_unknown : r.all_unknown) = true; continue; }
              gpu4::Span span; span.heap = bound_heaps_[0].ptr(); span.start = uint32_t(first); span.count = uint32_t(n);
              auto &dst = pass == 0 ? r.pre : r.all;
              if (std::none_of(dst.begin(), dst.end(), [&](const gpu4::Span &x) { return x.heap == span.heap && x.start == span.start && x.count == span.count; }))
                dst.push_back(span);
              heap_added = true;
            }
          }
        }
      }
      if (heap_added && (r.heaps.empty() || r.heaps.back().ptr() != bound_heaps_[0].ptr())) r.heaps.push_back(bound_heaps_[0]);
    } catch (...) {
      if (render.gpu4_reads) { if (want_pre) render.gpu4_reads->pre_unknown = true; if (want_all) render.gpu4_reads->all_unknown = true; }
    }
  }
  // GPU-PASS TESS_CENSUS (d3d12_tess_census.hpp): one tessellation draw — patches, instances and the four input hashes.
  // GPU-PASS TESS_PREPASS: the draw as a compute pre-pass (in front of this pass) + two ordinary draws (see
  // d3d12_tess_prepass.hpp). False = not recorded (the caller encodes the mesh draw as before).
  bool RecordTessPrepass(uint32_t groups, uint32_t instances, uint32_t patches, uint64_t args_offset, unsigned variant,
                         obj_handle_t ib, uint64_t ib_offset) {
    auto *tp = pso_graphics_->tess_prepass.get();
    if (!tp || variant > 2) return false;
    EncoderData *pass_data = allocator_->encoder_current;
    // the bindings the mesh draw would use must be the ones emitted in this pass (a skipped binding is not one of them)
    if (!pass_data || pass_data->type != EncoderType::Render || tp_root_pass_ != pass_data) return false;
    auto *pass = static_cast<RenderEncoderData *>(pass_data);
    const bool samplers = tp_sampler_pass_ == pass_data, vb = tp_vb_pass_ == pass_data;
    auto scan = tess_prepass::ScanPso(device_->GetMTLDevice());
    if (!scan) return false;
    const uint32_t G = groups * instances;
    auto [cap_w, cap_i] = tp->Capacity(patches * instances);
    const unsigned test = tess_prepass::TestMode();
    if (test) cap_w = cap_i = 1;
    auto record_buffer = tp->record_disabled;
    if (tess_prepass::VerifyRecords() && instances == 1 && !test &&
        tess_prepass::RecordPso(pso_graphics_->tess_census_info ? pso_graphics_->tess_census_info->id : 0, patches)) {
      static std::atomic<unsigned> captures{0};
      // One draw in this process, at most 32 MiB. No persistent application-data capture.
      if (captures.fetch_add(1, std::memory_order_relaxed) == 0) {
        const uint64_t per_group = uint64_t(tp->workloads_per_group) * tp->record_vertices * tp->record_stride * 4;
        const uint32_t captured_groups = per_group ? uint32_t(std::min<uint64_t>(G, (32u*1024*1024-128)/per_group)) : 0;
        if (captured_groups) {
          WMTBufferInfo info{}; info.length = 128 + captured_groups * per_group; info.options = WMTResourceStorageModeShared;
          auto buffer = device_->GetMTLDevice().newBuffer(info);
          auto *words = static_cast<uint32_t *>(info.memory.get_accessible_or_null());
          if (buffer && words) {
            memset(words,0,size_t(info.length));
            words[0]=1; words[1]=captured_groups; words[2]=tp->workloads_per_group;
            words[3]=tp->record_vertices; words[4]=tp->record_stride; words[7]=0x54524431;
            words[6]=tess_prepass::RecordNegative() ? 1 : 0; // record-only corruption, never changes rendered values
            record_buffer=buffer;
            // The localization draw uses the indexed pull, including on the first cold allocation.
            cap_w=G*tp->workloads_per_group;
            cap_i=cap_w*tp->record_vertices*3;
            fprintf(stderr,"dx12_tess_records status=PREPARED groups=%u captured_groups=%u bytes=%llu stride=%u vertices=%u\n",
                G,captured_groups,(unsigned long long)info.length,tp->record_stride,tp->record_vertices);
          }
        } else fprintf(stderr,"dx12_tess_records status=NOT_ENABLED reason=group_exceeds_32MiB\n");
      }
    }
    size_t scratch_bytes = 0;
    const auto reserve = [&](size_t bytes, size_t alignment) {
      scratch_bytes = (scratch_bytes + alignment - 1) & ~(alignment - 1);
      const size_t offset = scratch_bytes;
      scratch_bytes += bytes;
      return offset;
    };
    const uint64_t slots_off = reserve(size_t(G) * tp->slot_stride, 16);
    const uint64_t gcount_off = reserve(size_t(G) * 16, 16), offsets_off = reserve(size_t(G) * 16, 16);
    const uint64_t table_off = reserve(size_t(cap_w) * tess_prepass::kEntryBytes + 64, 64);
    const uint64_t indices_off = reserve(size_t(cap_i) * 4 + 16, 16), args_off = reserve(tess_prepass::kArgsBytes, 64);
    if (tp_scratch_pass_ != pass_data) {   // GAPS 01.11 TESS_PREPASS_RENAME=1: one scratch per draw of the whole recording
      tp_scratch_pass_ = pass_data;
      if (tess_prepass::RenameMode() != 1) tp_scratch_cursor_ = 0;
    }
    if (tp_scratch_cursor_ == tp_pass_buffers_.size()) tp_pass_buffers_.emplace_back();
    auto &scratch = tp_pass_buffers_[tp_scratch_cursor_++];
    if (scratch.bytes < scratch_bytes) {
      auto [buffer, offset] = allocator_->AllocateTessTempBuffer(scratch_bytes, 64);
      assert(!offset);
      scratch = {buffer, scratch_bytes};
    }
    const auto slots = scratch.buffer;
    if (tess_prepass::Schedule()) {
      auto inputs = std::make_shared<tess_prepass::DrawInputs>();
      inputs->Root(*tp,device_,rootsig_graphics_.ptr(),rootarg_graphics_staging_,bound_heaps_,bound_heap_ranges_);
      for (unsigned i = 0; i < vertex_buffers_.size(); ++i) if (pso_graphics_->slot_mask & (1u << i)) {
        const auto &v = vertex_buffers_[i]; inputs->Buffer(device_,v.BufferLocation,v.SizeInBytes);
      }
      if (variant) inputs->Buffer(device_,index_buffer_address,index_buffer_size);
      pass->prepass_inputs.push_back(std::move(inputs));
      pass->prepass_scratch.push_back(slots.handle);
    }
    const auto gcount = slots, offsets = slots, table = slots, indices = slots, args = slots;
    // Mode3 keeps indexed IA but removes repeated vertex IDs. The immutable
    // identity indices live in this allocator's fenced GPU heap, independently
    // of the compute-written encoded stream. Diagnostic only; no default cost.
    obj_handle_t identity_indices = 0;
    uint64_t identity_indices_off = 0;
    if (tess_prepass::NoIndexMode() == 3) {
      auto [identity, offset] = allocator_->AllocateGPUHeap(size_t(cap_i) * 4, 16);
      for (uint32_t i = 0; i < cap_i; ++i) static_cast<uint32_t *>(identity)[i] = i;
      identity_indices = allocator_->gpu_heap_buffer_;
      identity_indices_off = offset;
    }
    auto [mapped, constants_off] = allocator_->AllocateGPUHeap(32, 16);
    const uint32_t constants[8] = {G, tess_prepass::NoIndexMode(), 0, 0, cap_w, cap_i, G, 0};   // fallback/diagnostic then scan caps
    memcpy(mapped, constants, sizeof(constants));
    const obj_handle_t heap = allocator_->gpu_heap_buffer_;

    if (!pass->prepass_tail) {
      pass->prepass_tail = reinterpret_cast<wmtcmd_base *>(&pass->prepass_head);
      pass->prepass_scan_tail = reinterpret_cast<wmtcmd_base *>(&pass->prepass_scan_head);
      pass->prepass_materialize_tail = reinterpret_cast<wmtcmd_base *>(&pass->prepass_materialize_head);
    }
    wmtcmd_base **phase_tail = &pass->prepass_tail;
    const auto link = [&](auto *cmd) {
      cmd->next.set(nullptr);
      (*phase_tail)->next.set(cmd);
      *phase_tail = reinterpret_cast<wmtcmd_base *>(cmd);
    };
    const auto cpso = [&](obj_handle_t pso, WMTSize size) {
      auto *c = allocator_->AllocateCommandData<wmtcmd_compute_setpso>(1);
      *c = {}; c->type = WMTComputeCommandSetPSO; c->pso = pso; c->threadgroup_size = size; link(c);
    };
    const auto cbuf = [&](obj_handle_t buffer, uint64_t offset, uint8_t index) {
      auto *c = allocator_->AllocateCommandData<wmtcmd_compute_setbuffer>(1);
      *c = {}; c->type = WMTComputeCommandSetBuffer; c->buffer = buffer; c->offset = offset; c->index = index; link(c);
    };
    // hull kernel: the object function's grid and bindings
    cpso(tp->hull[variant], {tp->threads_per_patch, tp->patches_per_group, 1});
    cbuf(tp_root_buffer_, tp_root_offset_, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
    if (samplers) cbuf(tp_sampler_buffer_, tp_sampler_offset_, SM50_BINDING_INDEX_STATIC_SAMPLERS);
    if (vb) cbuf(tp_vb_buffer_, tp_vb_offset_, SM50_BINDING_INDEX_VERTEX_BUFFER);
    if (variant) cbuf(ib, ib_offset, SM50_BINDING_INDEX_INDEX_BUFFER);
    cbuf(heap, args_offset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
    cbuf(slots, slots_off, tess_prepass::kSlots);
    cbuf(gcount, gcount_off, tess_prepass::kGroups);
    { auto *c = allocator_->AllocateCommandData<wmtcmd_compute_dispatch>(1);
      *c = {}; c->type = WMTComputeCommandDispatch; c->size = {groups, instances, 1}; link(c); }
    phase_tail = &pass->prepass_scan_tail;
    // scan: bases + the arguments of the three indirect calls
    cpso(scan, {1024, 1, 1});
    cbuf(gcount, gcount_off, tess_prepass::kGroups);
    cbuf(offsets, offsets_off, tess_prepass::kOffsets);
    cbuf(heap, constants_off + 16, tess_prepass::kScanCaps);
    cbuf(args, args_off, tess_prepass::kScanArgs);
    cbuf(tp->need, 0, tess_prepass::kScanNeed);
    { auto *c = allocator_->AllocateCommandData<wmtcmd_compute_dispatch>(1);
      *c = {}; c->type = WMTComputeCommandDispatch; c->size = {1, 1, 1}; link(c); }
    phase_tail = &pass->prepass_materialize_tail;
    // materialize: one threadgroup per group (0 when the table or the index buffer is too small)
    cpso(tp->materialize, {32, 1, 1});
    cbuf(slots, slots_off, tess_prepass::kSlots);
    cbuf(gcount, gcount_off, tess_prepass::kGroups);
    cbuf(offsets, offsets_off, tess_prepass::kOffsets);
    cbuf(table, table_off, tess_prepass::kTable);
    cbuf(indices, indices_off, tess_prepass::kIndices);
    { auto *c = allocator_->AllocateCommandData<wmtcmd_compute_dispatch_indirect>(1);
      *c = {}; c->type = WMTComputeCommandDispatchIndirect; c->indirect_args_buffer = args;
      c->indirect_args_offset = args_off + tess_prepass::kArgsDispatch; link(c); }

    // in the pass: the ready triangles, then the fallback (0 vertices unless overflowed), then the mesh pipeline again
    const auto rpso = [&](obj_handle_t pso) {
      auto &c = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>(); c.type = WMTRenderCommandSetPSO; c.pso = pso;
    };
    const auto vbuf = [&](obj_handle_t buffer, uint64_t offset, uint8_t index) {
      auto &c = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      c.type = WMTRenderCommandSetVertexBuffer; c.buffer = buffer; c.offset = offset; c.index = index;
    };
    if (tess_prepass::VerifyRecords()) {
      pass->tess_record_buffer = record_buffer;
      auto &b = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      b.type=WMTRenderCommandSetMeshBuffer; b.buffer=record_buffer; b.offset=0; b.index=26;
      rpso(tp->record_mesh[variant]);
    }
    auto *state_tail = pass->cmd_tail;
    rpso(tp->pull);
    if (tess_prepass::Verify()) {
      pass->verify_pso = pso_graphics_->tess_census_info ? pso_graphics_->tess_census_info->id :
          uint64_t(reinterpret_cast<uintptr_t>(pso_graphics_.ptr()));
      if (pass->may_write_memory) {
        fprintf(stderr, "dx12_tess_verify status=NOT_ENABLED pso=%llu reason=shader_memory_side_effect\n",
                (unsigned long long)pass->verify_pso);
      } else {
        pass->verify_prefix_end = static_cast<const wmtcmd_base *>(state_tail->next.ptr);
        if (variant) {
          auto *m = allocator_->AllocateCommandData<wmtcmd_render_dxmt_tessellation_mesh_draw_indexed>(1);
          *m = {}; m->type = WMTRenderCommandDXMTTessellationMeshDrawIndexed;
          m->draw_arguments_offset = args_offset; m->instance_count = instances;
          m->threads_per_patch = tp->threads_per_patch; m->patch_per_group = tp->patches_per_group;
          m->patch_per_mesh_instance = groups; m->index_buffer = ib; m->index_buffer_offset = ib_offset;
          pass->verify_mesh_draw = reinterpret_cast<wmtcmd_base *>(m);
        } else {
          auto *m = allocator_->AllocateCommandData<wmtcmd_render_dxmt_tessellation_mesh_draw>(1);
          *m = {}; m->type = WMTRenderCommandDXMTTessellationMeshDraw;
          m->draw_arguments_offset = args_offset; m->instance_count = instances;
          m->threads_per_patch = tp->threads_per_patch; m->patch_per_group = tp->patches_per_group;
          m->patch_per_mesh_instance = groups;
          pass->verify_mesh_draw = reinterpret_cast<wmtcmd_base *>(m);
        }
      }
    }
    if (tess_prepass::VerifyMeshControl() && pass->verify_mesh_draw) {
      rpso(variant ? pso_graphics_->mesh_indexed_pso[variant - 1] : pso_graphics_->pso);
      if (!variant) {
        auto &c=allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw>();
        c=*reinterpret_cast<const wmtcmd_render_dxmt_tessellation_mesh_draw *>(pass->verify_mesh_draw);
      } else {
        auto &c=allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw_indexed>();
        c=*reinterpret_cast<const wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *>(pass->verify_mesh_draw);
      }
    } else {
    if (tess_prepass::VerifyRecords()) vbuf(record_buffer,0,26);
    vbuf(tp_root_buffer_, tp_root_offset_, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
    if (samplers) vbuf(tp_sampler_buffer_, tp_sampler_offset_, SM50_BINDING_INDEX_STATIC_SAMPLERS);
    vbuf(slots, slots_off, tess_prepass::kSlots);
    vbuf(table, table_off, tess_prepass::kTable);
    if (tess_prepass::NoIndex()) {
      vbuf(indices, indices_off, SM50_TESS_PREPASS_BINDING_INDICES);
      vbuf(heap, constants_off, SM50_TESS_PREPASS_BINDING_CONSTANTS);
    }
    if (tess_prepass::NoIndexMode() == 1) {
      // The first four indexed-argument words are {ti,1,0,0}, also valid for
      // a non-indexed indirect draw. The fifth word is zero and not consumed.
      auto &c = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indirect>();
      c.type = WMTRenderCommandDrawIndirect; c.primitive_type = WMTPrimitiveTypeTriangle;
      c.indirect_args_buffer = args; c.indirect_args_offset = args_off + tess_prepass::kArgsIndexed;
    } else { auto &c = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed_indirect>();
      c.type = WMTRenderCommandDrawIndexedIndirect; c.primitive_type = WMTPrimitiveTypeTriangle; c.index_type = WMTIndexTypeUInt32;
      c.index_buffer = identity_indices ? identity_indices : indices;
      c.index_buffer_offset = identity_indices ? identity_indices_off : indices_off; c.indirect_args_buffer = args;
      c.indirect_args_offset = args_off + tess_prepass::kArgsIndexed; }
    if (test != 2) {   // the negative control drops the fallback
      rpso(tp->fallback);
      vbuf(slots, slots_off, tess_prepass::kSlots);
      vbuf(gcount, gcount_off, tess_prepass::kGroups);
      vbuf(offsets, offsets_off, tess_prepass::kOffsets);
      vbuf(heap, constants_off, tess_prepass::kConstants);
      { auto &c = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indirect>();
        c.type = WMTRenderCommandDrawIndirect; c.primitive_type = WMTPrimitiveTypeTriangle; c.indirect_args_buffer = args;
        c.indirect_args_offset = args_off + tess_prepass::kArgsPull; }
    }
    }
    auto *pull_end = pass->cmd_tail;
    rpso(variant ? pso_graphics_->mesh_indexed_pso[variant - 1] : pso_graphics_->pso);
    if (tess_prepass::VerifyPullControl() && pass->verify_mesh_draw) {
      // Shadow replays the exact pull + fallback command prefix, up to (but
      // excluding) the final mesh-state restore. No application shader changes.
      pass->verify_prefix_end = static_cast<const wmtcmd_base *>(pull_end->next.ptr);
      auto *noop = allocator_->AllocateCommandData<wmtcmd_render_nop>(1);
      *noop = {}; noop->type = WMTRenderCommandNop;
      pass->verify_mesh_draw = reinterpret_cast<wmtcmd_base *>(noop);
    }
    ForgetEmittedState();
    ++pass->prepass_draws;
    pass->prepass_read_only &= pso_graphics_->pre_raster_srv_only;
    if (const unsigned period = tess_prepass::StatsPeriod()) {
      static std::atomic<uint64_t> draws{0};
      if (++draws % period == 0)
        fprintf(stderr, "dx12_tess_prepass draws=%llu pso=%p groups=%u cap_w=%u cap_i=%u need_w=%u need_i=%u overflowed=%u seen=%u "
                "draw_bytes=%llu live_bytes=%llu peak_bytes=%llu noindex=%u pool_bytes=%llu pool_takes=%llu pool_hits=%llu pool_drops=%llu\n",
                (unsigned long long)draws.load(), static_cast<void *>(pso_graphics_.ptr()), G, cap_w, cap_i,
                tp->need_cpu ? tp->need_cpu[0] : 0u, tp->need_cpu ? tp->need_cpu[1] : 0u, tp->need_cpu ? tp->need_cpu[2] : 0u,
                tp->need_cpu ? tp->need_cpu[3] : 0u,
                (unsigned long long)(size_t(G) * (tp->slot_stride + 32) + size_t(cap_w) * tess_prepass::kEntryBytes + size_t(cap_i) * 4),
                (unsigned long long)tess_prepass::LiveBytes().load(), (unsigned long long)tess_prepass::PeakBytes().load(),
                tess_prepass::NoIndexMode(), (unsigned long long)tess_prepass::PoolBytes().load(),
                (unsigned long long)tess_prepass::Pool().takes, (unsigned long long)tess_prepass::Pool().hits,
                (unsigned long long)tess_prepass::Pool().drops);
    }
    return true;
  }

  void TessCensusDraw(uint64_t patches, uint64_t instances, uint64_t indirect_max, std::initializer_list<uint64_t> args, bool indexed) {
    if (!tess_census::Enabled() || !pso_graphics_ || !pso_graphics_->tess_census_info) return;
    if (tess_prepass::Verify()) {
      // One expected ID per logical tess draw, including unsupported/fallback paths. GPU receipts carry it in pso[63:32].
      // A positive number of successful receipts alone must not hide an unverified draw elsewhere in the frame.
      static std::atomic<uint64_t> expected_draws{0};
      const uint64_t id=++expected_draws, pso=pso_graphics_->tess_census_info->id;
      auto pass=allocator_ && allocator_->encoder_current && allocator_->encoder_current->type==EncoderType::Render?
          static_cast<RenderEncoderData *>(allocator_->encoder_current):nullptr;
      const bool verified=pass && pass->verify_mesh_draw && id<=UINT32_MAX;
      if (verified) pass->verify_pso=(id<<32)|pso;
      const DWORD saved=GetLastError(); char line[256]; DWORD written;
      const int n=std::snprintf(line,sizeof(line),"dx12_tess_expected id=%llu frame=%llu pso=%llu patches=%llu verified_path=%u control=%u\n",
          (unsigned long long)id,(unsigned long long)tess_census::Frame(),(unsigned long long)pso,
          (unsigned long long)(patches*instances),unsigned(verified),tess_prepass::VerifyControl());
      if (n>0 && size_t(n)<sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,DWORD(n),&written,nullptr);
      SetLastError(saved);
    }
    try {
      if (tess_prepass::Verify() && allocator_ && allocator_->encoder_current &&
          allocator_->encoder_current->type==EncoderType::Render) {
        auto twin=std::make_shared<tess_prepass::TwinDraw>();
        twin->pso=pso_graphics_->tess_prepass; twin->root=rootsig_graphics_.ptr();
        twin->args=rootarg_graphics_staging_; twin->heaps=bound_heaps_; twin->ranges=bound_heap_ranges_;
        twin->frame=tess_census::Frame(); twin->id=pso_graphics_->tess_census_info->id; twin->patches=patches*instances;
        twin->known=!indirect_max;
        for (const auto a:args) twin->Word(twin->draw_key,a);
        for (unsigned i=0;i<vertex_buffers_.size();++i) if (pso_graphics_->slot_mask&(1u<<i)) {
          const auto &v=vertex_buffers_[i]; twin->Word(twin->draw_key,i); twin->Word(twin->draw_key,v.StrideInBytes);
          twin->Vertex(device_,v.BufferLocation,v.SizeInBytes);
        }
        if (indexed) { twin->Word(twin->draw_key,index_type); twin->Vertex(device_,index_buffer_address,index_buffer_size); }
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->twin_draw=std::move(twin);
      }
      using tess_census::Word;
      tess_census::Sig sig;
      for (auto a : args) sig.v = Word(sig.v, a);
      const uint32_t mask = pso_graphics_->slot_mask;
      for (unsigned i = 0; i < vertex_buffers_.size(); ++i) if (mask & (1u << i)) {
        const auto &v = vertex_buffers_[i];
        sig.v = Word(Word(Word(sig.v, v.BufferLocation), v.SizeInBytes), v.StrideInBytes);
      }
      if (indexed) sig.v = Word(Word(Word(sig.v, index_buffer_address), index_buffer_size), uint64_t(index_type));
      if (!rootsig_graphics_) sig.tex_unknown = true;
      else {
        const auto &layout = rootsig_graphics_->ArgumentLayout;
        const auto &words = rootarg_graphics_staging_.words;
        const auto &vis = rootsig_graphics_->reuse_parameters;
        for (uint32_t i = 0; i < layout.count && i < 64; ++i) {
          const auto &p = layout.parameters[i];
          if (p.qword >= words.size()) continue;
          const bool pre = i >= vis.size() || vis[i].visibility != 5;   // visible to some pre-raster stage (or unknown)
          if (p.kind == root_argument::Table) {
            sig.tb = Word(sig.tb, words[p.qword]);
            if (pre) sig.tbp = Word(sig.tbp, words[p.qword]);
          } else if (p.kind == root_argument::Constants) {
            if (p.qword + (p.count + 1) / 2 <= words.size()) {
              sig.c = tess_census::Hash(sig.c, words.data() + p.qword, size_t(p.count) * 4);
              if (pre) sig.cp = tess_census::Hash(sig.cp, words.data() + p.qword, size_t(p.count) * 4);
            }
          } else {
            sig.c = Word(sig.c, words[p.qword]);
            if (pre) sig.cp = Word(sig.cp, words[p.qword]);
          }
        }
        // textures the pre-raster stages can read: declared SRV registers of tables visible to VS/HS/DS/GS
        const auto *decl = pso_graphics_->xl_hazard_decl.get();
        const auto &parameters = rootsig_graphics_->reuse_parameters;
        auto *heap = bound_heaps_[0] ? static_cast<MTLD3D12DescriptorHeap *>(bound_heaps_[0].ptr()) : nullptr;
        if (!decl || !decl->known || parameters.size() != layout.count) sig.tex_unknown = true;
        else for (uint32_t index = 0; index < layout.count; ++index) {
          const auto &p = parameters[index];
          if (layout.parameters[index].kind != root_argument::Table || p.visibility == 5) continue;
          for (const auto &range : p.ranges) {
            if (range.type != 0) continue;   // SRV ranges only
            const uint64_t lo = range.binding;
            const uint64_t hi = range.count == UINT32_MAX ? UINT64_MAX : uint64_t(range.binding) + range.count - 1;
            for (const auto &d : decl->ranges) {
              if (d.uav || d.space != range.space || d.visibility < 1 || d.visibility > 4) continue;
              if (p.visibility != 0 && p.visibility != d.visibility) continue;
              const uint64_t a = std::max<uint64_t>(lo, d.lower), b = std::min<uint64_t>(hi, d.upper);
              if (a > b) continue;
              root_argument::TableLocation loc;
              if (!heap || b - a > 255 ||
                  !root_argument::ReadTable(layout, rootarg_graphics_staging_, index, bound_heap_ranges_[0], range.offset, 1, loc)) {
                sig.tex_unknown = true; continue;
              }
              for (uint64_t r = a; r <= b; ++r) {
                const uint64_t at = uint64_t(loc.index) + (r - lo);
                if (at >= bound_heap_ranges_[0].count) { sig.tex_unknown = true; break; }
                const auto &desc = heap->GetDescriptor(UINT(at));
                uint64_t id = 0;
                if (desc.type == ShaderVisibleDescriptorType::SRVTexture || desc.type == ShaderVisibleDescriptorType::UAVTexture)
                  id = uint64_t(reinterpret_cast<uintptr_t>(desc.SRVTexture.texture));   // identity only: never dereferenced
                else if ((desc.type == ShaderVisibleDescriptorType::SRVTexelBuffer || desc.type == ShaderVisibleDescriptorType::UAVTexelBuffer) && desc.SRVTexelBuffer.buffer)
                  id = uint64_t(reinterpret_cast<uintptr_t>(desc.SRVTexelBuffer.buffer));
                sig.t = Word(Word(sig.t, uint64_t(desc.type)), id);
              }
            }
          }
        }
      }
      tess_census::Draw(*pso_graphics_->tess_census_info, patches, instances, indirect_max, sig);
    } catch (...) {}
  }
  static uint32_t CensusHeapType(ID3D12Resource *r) {
    D3D12_HEAP_PROPERTIES hp{}; D3D12_HEAP_FLAGS flags{};
    if (!r || FAILED(r->GetHeapProperties(&hp, &flags))) return 0;
    return uint32_t(hp.Type);
  }
  // GPU-PASS TESS_CENSUS: one copy command (kind, heaps, bytes, destination key, CPU view of a CPU-visible source buffer).
  void CensusCopy(const char *kind, ID3D12Resource *src, uint64_t src_offset, ID3D12Resource *dst, uint64_t dst_key, uint64_t bytes) {
    if (!tess_census::Enabled()) return;
    try {
      const uint32_t sh = CensusHeapType(src), dh = CensusHeapType(dst);
      const void *cpu = nullptr;
      uint64_t readable = 0;
      auto *s = static_cast<MTLD3D12Resource *>(src);
      if (s && s->buffer && bytes && sh != 0 && sh != D3D12_HEAP_TYPE_DEFAULT) {
        // 25.10 TC crash: a footprint's RowPitch x Height overstates the bytes (last row, block-compressed rows) - never read
        // past the source buffer.
        const uint64_t width = s->GetDesc().Width;
        if (src_offset < width) {
          readable = std::min<uint64_t>(bytes, width - src_offset);
          if (void *base = s->buffer->current()->mappedMemory(0)) cpu = static_cast<const char *>(base) + src_offset;
        }
      }
      tess_census::Copy(kind, sh, dh, bytes, dst, dst_key, cpu, cpu ? readable : 0);
    } catch (...) {}
  }
  CommandRecordingState recording_state_;
  FirstRecordingFailure first_recording_failure_;
  uint64_t recording_invocation_ = 0;
  const uint64_t max_object_threadgroups_;

  void SetRecordingError(HRESULT hr, const char *operation, unsigned line) {
    recording_error_ = hr;
    first_recording_failure_.Capture(hr, operation, line, unsigned(topology_), pso_graphics_.ptr(), recording_invocation_);
  }

  bool RequireOpenRecording(const char *operation, unsigned op_class = ~0u) {
    ++recording_invocation_;
    if (c17::Enabled() || xl_hazard::Enabled() || gpu4::Armed()) {   // GPU4: its read sets dedup on the epoch too
      if (open_fast_ && op_class != ~0u) {   // GAPS 02.11 OPEN_FAST: the same tests, done by the compiler (DXMT_OPCLASS)
        if (op_class & 1) ++empty_binding_epoch;
        if (op_class & 2) empty_indirect = true;
      } else {
      // Binding is observable to the proof even if no Metal command is emitted.
      // Tables are conservatively treated as possible SRVs, including mutable descriptors.
      if (std::strstr(operation, "RootDescriptorTable") || std::strstr(operation, "RootShaderResourceView") ||
          std::strstr(operation, "RootUnorderedAccessView") || !std::strcmp(operation, "SetDescriptorHeaps"))
        ++empty_binding_epoch;
      if (!std::strcmp(operation, "ExecuteIndirect")) empty_indirect = true;
      }
    }
    if (recording_state_.Allows(allocator_.ptr() && allocator_->encoder_last)) return true;
    TraceCommandFailure(CommandFailureOperation::RecordAfterClose, "command.record.failure", this,
                        FAILED(close_result) ? close_result : E_FAIL, operation, allocator_.ptr());
    return false;
  }

  const bool recording_trace_enabled_ = RecordingTraceEnabled();
  D3D12RecordingTrace recording_trace_;
  uint64_t recording_operation_ = 0;
  char recording_rtvs_[8][128] = {};
  const char *recording_predraw_reason_ = nullptr;

  template<typename... Args>
  void AppendRecordingTrace(const char *format, Args... args) {
    if (!recording_trace_enabled_)
      return;
    if (recording_trace_.entries_.size() >= 2048) {
      ++recording_trace_.dropped_;
      return;
    }
    const DWORD error = GetLastError();
    char line[512];
    const int length = std::snprintf(line, sizeof(line), format, args...);
    if (length < 0) {
      ++recording_trace_.dropped_;
    } else {
      if (size_t(length) >= sizeof(line)) {
        constexpr char marker[] = " [truncated]";
        memcpy(line + sizeof(line) - sizeof(marker), marker, sizeof(marker));
      }
      try {
        recording_trace_.entries_.emplace_back(line);
      } catch (...) {
        ++recording_trace_.dropped_;
      }
    }
    SetLastError(error);
  }

  template<typename... Args>
  void TraceRecording(const char *operation, const char *outcome, const char *reason,
                      const char *format, Args... args) {
    if (!std::strcmp(outcome, "rejected"))
      first_recording_failure_.Detail(recording_invocation_, operation, reason, format, args...);
    if (!recording_trace_enabled_)
      return;
    const auto id = ++recording_operation_;
    const auto rtv_count = std::min(num_rtvs, 8u);
    // Do not format or resolve anything once the bounded snapshot is full.
    if (recording_trace_.entries_.size() >= 2048) {
      recording_trace_.dropped_ += 1 + rtv_count;
      return;
    }
    const DWORD error = GetLastError();
    char detail[256];
    const int detail_length = std::snprintf(detail, sizeof(detail), format, args...);
    if (detail_length < 0) {
      detail[0] = 0;
    } else if (size_t(detail_length) >= sizeof(detail)) {
      constexpr char marker[] = " [truncated]";
      memcpy(detail + sizeof(detail) - sizeof(marker), marker, sizeof(marker));
    }
    const void *graphics_pso = static_cast<const void *>(pso_graphics_.ptr());
    const void *compute_pso = static_cast<const void *>(pso_compute_.ptr());
    AppendRecordingTrace("epoch=%llu call_id=%llu api=%s outcome=%s reason=%s graphics_pso=%p compute_pso=%p rtvs=%u %s",
                         static_cast<unsigned long long>(recording_trace_.epoch_),
                         static_cast<unsigned long long>(id), operation, outcome, reason, graphics_pso, compute_pso, num_rtvs, detail);
    for (UINT i = 0; i < rtv_count; ++i) {
      AppendRecordingTrace("epoch=%llu call_id=%llu api=%s outcome=metadata reason=render_target_state graphics_pso=%p target_slot=%u descriptor=%llx %s",
                           static_cast<unsigned long long>(recording_trace_.epoch_),
                           static_cast<unsigned long long>(id), operation, graphics_pso, i,
                           static_cast<unsigned long long>(rtvs[i].ptr), recording_rtvs_[i][0] ? recording_rtvs_[i] :
                           (rtvs[i].ptr ? "resolution=deferred_until_PreDraw" : "resolution=unbound"));
    }
    SetLastError(error);
  }

  bool ValidateQuery(MTLD3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index, UINT count, const char *operation) {
    if (encoder_count != std::numeric_limits<size_t>::max()) {
      WARN("Query command ignored on a closed command list");
      return false;
    }
    if (heap && (heap->type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS || heap->type == D3D12_QUERY_HEAP_TYPE_SO_STATISTICS)) {
      static std::atomic_uint trace_count{0};
      if (trace_count.fetch_add(1, std::memory_order_relaxed) < 16)
        fprintf(stderr, "dx12_statistics_storage_use operation=%s type=%u index=%u count=%u hr=80004001\n",
                operation, unsigned(type), index, count);
      SetRecordingError(E_NOTIMPL, __func__, __LINE__);
      return false;
    }
    const bool timestamp = type == D3D12_QUERY_TYPE_TIMESTAMP;
    const bool queue_valid = allocator_->type_ == D3D12_COMMAND_LIST_TYPE_DIRECT ||
        (timestamp && allocator_->type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE);
    if (!heap || heap->owner != device_ || !queue_valid ||
        (timestamp ? heap->type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP :
         (heap->type != D3D12_QUERY_HEAP_TYPE_OCCLUSION ||
          (type != D3D12_QUERY_TYPE_OCCLUSION && type != D3D12_QUERY_TYPE_BINARY_OCCLUSION))) ||
        index > heap->count || count > heap->count - index) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return false;
    }
    return true;
  }

public:
  MTLD3D12GraphicsCommandListImpl(MTLD3D12Device *pDevice) : MTLD3D12DeviceChild<MTLD3D12GraphicsCommandList>(pDevice),
      max_object_threadgroups_(ObjectGridLimit(pDevice->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7))) {}

  ~MTLD3D12GraphicsCommandListImpl() {
    pipeline_timing::Get().Emit(pipeline_timing::ListDestroy, pipeline_timing::Point, this,
        recording_trace_.epoch_, 0, 0, 0, 0, 0, true);
  }

  const std::vector<std::string> &GetRecordingTrace() const override { return recording_trace_.GetRecordingTrace(); }
  Com<MTLD3D12GraphicsCommandList> SnapshotSubmission() override {
    auto copy = Com(new MTLD3D12GraphicsCommandListImpl(device_));
    copy->allocator_ = allocator_;
    // Allocator's list-head vector can grow while another list is recorded with the same allocator.
    // The actual passes live in its stable CPU arena; copy the small sentinel, not its vector address.
    copy->submission_head_ = *entry;
    copy->entry = &copy->submission_head_;
    copy->encoder_count = encoder_count;
    copy->close_result = close_result;
    copy->recording_trace_ = recording_trace_;
    copy->early_cmdbuf = std::move(early_cmdbuf);
    copy->early_token = std::move(early_token);
    copy->lightrn_early = std::move(lightrn_early);   // LIGHTRN: travels with early_cmdbuf
    copy->reuse_early_plan = reuse_early_plan;
    copy->early_queue_id = early_queue_id;
    copy->early_consumed = early_consumed;
    copy->early_encode_ticks = early_encode_ticks;
    copy->empty_binding_epoch = empty_binding_epoch;
    copy->empty_indirect = empty_indirect;
    copy->gap_early_encoders = gap_early_encoders;
    early_consumed = true;
    return Com<MTLD3D12GraphicsCommandList>(copy.ptr());
  }
  uint64_t GetRecordingTraceEpoch() const override { return recording_trace_.GetRecordingTraceEpoch(); }
  uint64_t GetRecordingTraceDropped() const override { return recording_trace_.GetRecordingTraceDropped(); }

  // GAPS 02.11 SETTER_DEDUP / VA_CACHE / OPEN_FAST (d3d12_setter_dedup.hpp). Keys read once per list.
  const unsigned dd_mode_ = setter_dedup::Mode();
  const unsigned dd_mask_ = setter_dedup::Mode() ? setter_dedup::Mask() : 0u;
  const bool dd_stats_ = setter_dedup::Stats();
  const bool va_cache_on_ = setter_dedup::VACache();
  const bool open_fast_ = setter_dedup::OpenFast();
  unsigned dd_seen_ = 0;                       // mode 9: selected methods called since ResetState
  uint64_t dd_ib_gen_ = 0;                     // va_generation read BEFORE the lookup that set the current index buffer
  ID3D12PipelineState *dd_pso_arg_ = nullptr;  // argument of the last SetPipelineState of this list that resolved
  MTLD3D12PipelineState *dd_pso_resolved_ = nullptr;
  Com<ID3D12PipelineState> dd_pso_hold_;       // async PSO only (argument != resolved): keeps the argument's address unique
  setter_dedup::Local dd_local_;
  struct VaEntry { uint64_t base = 0, logical = 0, next = 0; Rc<BufferAllocation> allocation; };
  std::array<VaEntry, 8> va_cache_{};
  uint64_t va_cache_gen_ = ~uint64_t(0);
  unsigned va_cache_next_ = 0;

  // true = the call is skipped (the caller returns). `equal`: the arguments equal the state the list holds now.
  bool DedupSkip(unsigned m, bool equal) {
    if (recording_trace_enabled_ || !recording_state_.Allows(allocator_.ptr() && allocator_->encoder_last)) return false;
    if (dd_stats_) ++dd_local_.calls[m];
    const bool skip = dd_mode_ == 9 ? ((dd_seen_ >> m) & 1u) != 0 : equal;
    dd_seen_ |= 1u << m;
    if (!skip) return false;
    ++recording_invocation_;   // as RequireOpenRecording would
    if (dd_stats_) ++dd_local_.skips[m];
    return true;
  }
  void DedupForgetPSO() {
    dd_pso_arg_ = nullptr; dd_pso_resolved_ = nullptr;
    if (dd_pso_hold_.ptr()) dd_pso_hold_ = nullptr;
  }
  void VACacheDrop() { for (auto &e : va_cache_) e = VaEntry{}; }
  // Same result as device_->LookupBufferByVA (allocation and offset) - see d3d12_setter_dedup.hpp.
  Rc<BufferAllocation> VACacheLookup(uint64_t va, uint64_t length, uint64_t &offset) {
    offset = 0;
    const uint64_t gen = device_->va_generation.load(std::memory_order_acquire);
    if (gen != va_cache_gen_) {
      if (dd_stats_ && va_cache_gen_ != ~uint64_t(0)) ++dd_local_.cache[setter_dedup::VaFlush];
      VACacheDrop();
      va_cache_gen_ = gen;
    }
    for (auto &e : va_cache_) {
      if (!e.allocation || va < e.base || va >= e.next) continue;
      if (dd_stats_) ++dd_local_.cache[setter_dedup::VaHit];
      const uint64_t delta = va - e.base;
      if (delta > e.logical || length > e.logical - delta) return {};
      offset = delta;
      return e.allocation;
    }
    if (dd_stats_) ++dd_local_.cache[setter_dedup::VaMiss];
    uint64_t base = 0, logical = 0, next = 0, mgen = 0;
    auto result = device_->LookupBufferIntervalByVA(va, length, &offset, &base, &logical, &next, &mgen);
    if (result && mgen == gen) {
      auto &e = va_cache_[va_cache_next_++ & 7u];
      e.base = base; e.logical = logical; e.next = next; e.allocation = result;
      if (dd_stats_) ++dd_local_.cache[setter_dedup::VaFill];
    }
    return result;
  }

  bool failed_async_pso_ = false;
  void
  ResetState(ID3D12PipelineState *pInitialPipelineState) {
    dd_seen_ = 0; DedupForgetPSO();   // GAPS 02.11 SETTER_DEDUP
    if (va_cache_on_ && va_cache_gen_ != device_->va_generation.load(std::memory_order_acquire)) { VACacheDrop(); va_cache_gen_ = ~uint64_t(0); }
    pso_graphics_ = nullptr;
    pso_compute_ = nullptr;
    auto resolved = pso_work::Resolve(static_cast<MTLD3D12PipelineState *>(pInitialPipelineState));
    failed_async_pso_ = pInitialPipelineState && !resolved;
    if (auto pso = resolved) {
      if (!pso->IsComputePipelineState)
        pso_graphics_ = static_cast<MTLD3D12GraphicsPipelineState *>(pso);
      else
        pso_compute_ = static_cast<MTLD3D12ComputePipelineState *>(pso);
    }

    num_rtvs = {};
    memset(rtvs, 0, sizeof(rtvs));
    dsv = {};

    topology_ = {};

    num_viewports = {};
    memset(viewports, 0, sizeof(viewports));

    num_scissors = {};
    memset(scissors, 0, sizeof(scissors));

    blend_factor_[0] = 1.0f;
    blend_factor_[1] = 1.0f;
    blend_factor_[2] = 1.0f;
    blend_factor_[3] = 1.0f;
    stencil_ref_ = 0;

    rootsig_graphics_ = nullptr;
    rootarg_graphics_staging_ = {};
    diagnostic::Add(diagnostic::Counter::RootStateZeroCalls);
    diagnostic::Add(diagnostic::Counter::RootStateZeroBytes, sizeof(rootarg_graphics_staging_));

    rootsig_compute_ = nullptr;
    rootarg_compute_staging_ = {};
    diagnostic::Add(diagnostic::Counter::RootStateZeroCalls);
    diagnostic::Add(diagnostic::Counter::RootStateZeroBytes, sizeof(rootarg_compute_staging_));

    bound_heaps_ = {};
    bound_heap_ranges_ = {};
    memset(vertex_buffers_.data(), 0, sizeof(vertex_buffers_));

    index_buffer_address = 0;
    index_buffer = {};
    index_allocation_ = nullptr;
    index_type = {};
    index_offset = 0;
    index_buffer_size = 0;
    mesh_index_variant_ = 0;

    dirty_state_.clrAll();
    if (recording_trace_enabled_)
      memset(recording_rtvs_, 0, sizeof(recording_rtvs_));
  }

  HRESULT
  Initialize(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialPipelineState) {
    auto allocator = static_cast<MTLD3D12CommandAllocatorImpl *>(pAllocator);

    if (allocator_ != allocator)
      allocator_ = allocator;

    ResetState(pInitialPipelineState);

    tp_pass_buffers_.clear();
    tp_scratch_pass_ = nullptr;
    tp_scratch_cursor_ = 0;
    lightrn_seen_ = false; lightrn_croot_cmd_ = nullptr; lightrn_croot_words_ = nullptr; lightrn_croot_pass_ = nullptr;

    empty_binding_epoch = 0;
    empty_indirect = false;

    active_queries_.clear();
    for (auto &item : deferred_)
      if (item.side) item.side->~BlitEncoderData();
    deferred_.clear();
    ForgetEmittedState();
    pending_discards_.clear();
    early_cmdbuf = {};
    early_token.reset();
    reuse_early_plan.reset();
    early_consumed = false;
    absorbed_barriers_ = 0;
    absorbed_common_ = false;   // GAPS COPY_GATHER
    side_active_ = false;
    recording_state_.End();
    first_recording_failure_.Clear();
    recording_error_ = S_OK;
    recording_trace_.entries_.clear();
    ++recording_trace_.epoch_;
    recording_trace_.dropped_ = 0;
    recording_operation_ = 0;

    encoder_count = std::numeric_limits<size_t>::max();
    close_result = E_FAIL;
    const HRESULT hr = allocator_->StartRecord(&entry);
    recording_state_.Begin(SUCCEEDED(hr));
    pipeline_timing::Get().Emit(pipeline_timing::ListReset, pipeline_timing::Point, this,
        recording_trace_.epoch_, pipeline_timing::Pointer(pAllocator), 0, 0, 0, uint32_t(hr));
    TraceRecording("Reset", FAILED(hr) ? "rejected" : "accepted", FAILED(hr) ? "StartRecord_failed" : "new_recording",
                   "hr=%08x initial_pso=%p", unsigned(hr), static_cast<const void *>(pInitialPipelineState));
    return TraceCommandFailure(CommandFailureOperation::ListInitialize, "list.initialize.failure", this,
                               hr, "StartRecord", pAllocator);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) { DXMT_LT(0);
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12GraphicsCommandList), riid)) {
      WARN("D3D12GraphicsCommandList: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE
  GetType() { DXMT_LT(1);
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
  }

  // GAPS COPY_GATHER (d3d12_copy_gather.hpp): move the deferred copy passes of this list up to its first deferred copy pass.
  void GatherCopies() {
    if (!copy_gather::Enabled() || !entry) return;
    std::vector<EncoderData *> v;
    for (auto p = entry->next; p; p = p->next) { v.push_back(p); if (v.size() > 8192) return; }
    const auto blit = [](EncoderData *p) { return p->type == EncoderType::Blit ? static_cast<BlitEncoderData *>(p) : nullptr; };
    const auto join = [&](EncoderData *p) { auto b = blit(p); return b && b->gaps_join && !b->gaps_common && b->barrier_only &&
                                                               b->barrier_records_valid && b->barrier_records.empty(); };
    const auto pinned_join = [&](EncoderData *p) { auto b = blit(p); return b && b->gaps_join && b->gaps_common; };
    bool pin_next = false;   // the copies after a COMMON-absorbing join keep their place
    size_t a = 0;
    while (a < v.size() && !(blit(v[a]) && blit(v[a])->gaps_movable)) ++a;
    if (a >= v.size()) return;
    auto &st = copy_gather::Get();
    std::vector<EncoderData *> head(v.begin(), v.begin() + a + 1), moved, rest;
    // named: objects of barriers in between (any access conflicts); kept_w / kept_r: writes / reads of copies that stay.
    std::vector<obj_handle_t> named, kept_w, kept_r;
    const auto has = [](const std::vector<obj_handle_t> &v, obj_handle_t o) { return std::find(v.begin(), v.end(), o) != v.end(); };
    bool stop = false, out_seen = false;
    std::vector<EncoderData *> pending;   // join passes waiting for the copy pass after them
    for (size_t i = a + 1; i < v.size(); ++i) {
      auto p = v[i];
      if (join(p)) { pending.push_back(p); continue; }
      if (pinned_join(p)) pin_next = true;
      auto b = blit(p);
      if (b && b->gaps_movable) {
        bool conflict = pin_next;
        pin_next = false;
        for (auto o : b->gaps_objects) {
          const bool w = has(b->gaps_writes, o);
          conflict |= has(named, o) || has(kept_w, o) || (w && has(kept_r, o));   // read-read with a kept copy is no conflict
        }
        const bool immediate = copy_gather::TouchesImmediateTarget(b->gaps_objects);
        if (immediate) st.kept_out.fetch_add(1, std::memory_order_relaxed);
        if (!stop && !out_seen && !conflict && !immediate) {
          moved.insert(moved.end(), pending.begin(), pending.end()); moved.push_back(p); pending.clear();
          st.moved.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (!immediate) (stop || out_seen ? st.kept_stop : st.kept_conflict).fetch_add(1, std::memory_order_relaxed);
        for (auto o : b->gaps_objects)   // later copies keep their order to it
          (has(b->gaps_writes, o) ? kept_w : kept_r).push_back(o);
      } else if (b && b->barrier_only) {
        if (!b->barrier_records_valid) stop = true;
        for (const auto &rec : b->barrier_records) {
          if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !rec.object) { stop = true; continue; }
          named.push_back(rec.object);
          if (rec.object2) named.push_back(rec.object2);
        }
      } else if (b) {
        if (b->immediate_out) out_seen = true;
        stop = true;   // timestamps, resolves, OUT writes, copies recorded outside the deferral
      } else if (p->type != EncoderType::Null && p->type != EncoderType::Render && p->type != EncoderType::Clear &&
                 p->type != EncoderType::Compute) {
        stop = true;
      }
      rest.insert(rest.end(), pending.begin(), pending.end()); pending.clear();
      rest.push_back(p);
    }
    rest.insert(rest.end(), pending.begin(), pending.end());
    if (moved.empty()) return;
    st.lists.fetch_add(1, std::memory_order_relaxed);
    EncoderData *last = entry;
    for (auto *group : {&head, &moved, &rest})
      for (auto p : *group) { last->next = p; last = p; }
    last->next = nullptr;
  }

  HRESULT STDMETHODCALLTYPE
  Close() { DXMT_LT(2);
    pipeline_timing::Scope pipeline(pipeline_timing::ListClose, this, recording_trace_.epoch_);
    ++recording_invocation_;
    if (dd_mode_) { dd_seen_ = 0; DedupForgetPSO(); }   // GAPS 02.11 SETTER_DEDUP: nothing is skipped on a closed list anyway
    if (dd_stats_) dd_local_.Flush();
    if (!recording_state_.open || encoder_count < std::numeric_limits<size_t>::max()) {
      pipeline.Result(E_FAIL);
      TraceRecording("Close", "rejected", "already_closed", "hr=%08x", unsigned(E_FAIL));
      return TraceCommandFailure(CommandFailureOperation::ListClose, "list.close.failure", this,
                                 E_FAIL, "already_closed", allocator_.ptr(), encoder_count);
    }
    if (!active_queries_.empty())
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
    ENCODERS_REASON(ReasonClose);
    FlushDeferredResolves();
    DropDiscards();
    HRESULT hr = allocator_->EndRecord(&encoder_count);
    if (SUCCEEDED(hr) && c17::QueryNoColor())
      DropColorsOfColorlessPasses();
    if (SUCCEEDED(hr)) GatherCopies();   // GAPS COPY_GATHER (key 0: no-op)
    recording_state_.End();
    close_result = FAILED(hr) ? hr : recording_error_;
    pipeline.Result(close_result);
    TraceRecording("Close", FAILED(close_result) ? "rejected" : "accepted",
                   FAILED(hr) ? "EndRecord_failed" : !active_queries_.empty() ? "active_queries" :
                   FAILED(recording_error_) ? "recording_error" : "closed",
                   "hr=%08x end_record_hr=%08x encoders=%llu", unsigned(close_result), unsigned(hr),
                   static_cast<unsigned long long>(encoder_count));
    if (FAILED(close_result)) first_recording_failure_.Trace(this);
    // ENCODERS 4.1: encode now, on this (recording) thread, for the queue the list will most likely run on.
    early_cmdbuf = {};
    early_token.reset();
    early_consumed = false;
    early_encode_ticks = 0;
    reuse_early_plan.reset();
    if (SUCCEEDED(close_result) && encoder_stats::EarlyEncode())
      D3D12EarlyEncode(device_, allocator_->type_, this);
    return TraceCommandFailure(CommandFailureOperation::ListClose, "list.close.failure", this,
                               close_result, FAILED(hr) ? "EndRecord" : !active_queries_.empty() ?
                               "active_queries" : "recording_error", allocator_.ptr(), encoder_count);
  };

  HRESULT STDMETHODCALLTYPE
  Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) { DXMT_LT(3);
    pipeline_timing::Scope pipeline(pipeline_timing::ListReset, this, recording_trace_.epoch_,
        pipeline_timing::Pointer(pAllocator));
    ++recording_invocation_;
    if (encoder_count == std::numeric_limits<size_t>::max()) {
      pipeline.Result(E_FAIL);
      TraceRecording("Reset", "rejected", "still_recording", "hr=%08x", unsigned(E_FAIL));
      return TraceCommandFailure(CommandFailureOperation::ListReset, "list.reset.failure", this,
                                 E_FAIL, "still_recording", pAllocator);
    }
    if (FAILED(close_result)) {
      pipeline.Result(close_result);
      TraceRecording("Reset", "rejected", "previous_Close_failed", "hr=%08x", unsigned(close_result));
      return TraceCommandFailure(CommandFailureOperation::ListReset, "list.reset.failure", this,
                                 close_result, "previous_Close_failed", pAllocator);
    }
    return pipeline.Result(Initialize(pAllocator, pInitialState));
  };

  void STDMETHODCALLTYPE
  ClearState(ID3D12PipelineState *pPipelineState) { DXMT_LT(4);
    if (!RequireOpenRecording("ClearState", DXMT_OPCLASS("ClearState"))) return; DropDiscards(); ENCODERS_REASON(ReasonClearState);
    try {
    EndPass();
    ResetState(pPipelineState);
    TraceRecording("ClearState", "accepted", "state_reset", "initial_pso=%p", static_cast<const void *>(pPipelineState));
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearState", __LINE__);
      TraceRecording("ClearState", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  std::tuple<uint64_t, uint64_t>
  PopulateVertexBufferTable(uint32_t Count) {
    auto slot_mask = pso_graphics_ ? pso_graphics_->slot_mask : 0;
    if (!slot_mask)
      return {0, 0};
    uint32_t max_slot = 32 - __builtin_clz(slot_mask);
    struct VERTEX_BUFFER_ENTRY {
      uint64_t buffer_handle;
      uint32_t stride;
      uint32_t length;
    };
    auto stride = align(sizeof(VERTEX_BUFFER_ENTRY) * max_slot, 16);

    auto [mapped, offset] = allocator_->AllocateGPUHeap(stride * Count, 16);

    for (unsigned i = 0; i < Count; i++) {
      VERTEX_BUFFER_ENTRY *entries = (VERTEX_BUFFER_ENTRY *)(reinterpret_cast<char *>(mapped) + i * stride);
      for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
        if (!(slot_mask & (1 << slot)))
          continue;
        auto &state = vertex_buffers_[slot];
        entries[index].buffer_handle = state.BufferLocation;
        entries[index].stride = state.StrideInBytes;
        entries[index++].length = state.SizeInBytes;
      };
    }

    return {offset, stride};
  }

  #include "record-merge-trace.inc"
  #include "post-chain-list.inc"   // POSTCS 03.11

  void AuditVertexInputs(const char *kind, unsigned indirect_ia = 0) {
    if (!pso_graphics_ || !pso_graphics_->vertex_input_audit) return;
    const unsigned ib = index_buffer_address ? (index_type == WMTIndexTypeUInt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT) : 0;
    vertex_audit::Emit(*pso_graphics_->vertex_input_audit, pso_graphics_.ptr(), pso_graphics_->pso.handle,
        pso_graphics_->slot_mask, vertex_buffers_.data(), ib, kind, indirect_ia,
        pso_graphics_->geometry_pipeline ? 2 : pso_graphics_->tess_control_points ? 1 : 0);
  }

  DrawCallStatus
  PreDraw(bool SkipResourceBinding = false, uint32_t MeshIndexVariant = 0) {
    ldc_first_draw_ = false;   // ENC-FIX LOAD_DONTCARE: only the draw right after a pass opened may decide
    if (recording_trace_enabled_)
      recording_predraw_reason_ = nullptr;
    if (!pso_graphics_) {
      if (recording_trace_enabled_)
        recording_predraw_reason_ = "PreDraw_invalid_missing_graphics_pso";
      return DrawCallStatus::Invalid;
    }
    const bool mesh_pipeline = pso_graphics_->tess_control_points || pso_graphics_->geometry_pipeline;
    if (MeshIndexVariant > 2 || (MeshIndexVariant &&
        (!mesh_pipeline || !pso_graphics_->mesh_indexed_pso[MeshIndexVariant - 1]))) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      if (recording_trace_enabled_)
        recording_predraw_reason_ = MeshIndexVariant > 2 ? "PreDraw_invalid_mesh_index_variant" :
                                   "PreDraw_unavailable_mesh_indexed_pso";
      return DrawCallStatus::Invalid;
    }
    if (mesh_index_variant_ != MeshIndexVariant) {
      mesh_index_variant_ = MeshIndexVariant;
      dirty_state_.set(DirtyState::GraphicsPipelineState);
    }
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render) {
      ldc_first_draw_ = enc_census::LoadDontCare();
      for (auto &v : ldc_w_) v = 0;

      ENCODERS_REASON(ReasonSwitchRender);
      EndPass();
      auto render = allocator_->AllocatePass<RenderEncoderData>();
      render->type = EncoderType::Render;
      render->cmd_head.type = WMTRenderCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
      render->dsv_planar_flags = 0;
      render->dsv_readonly_flags = 0;
      render->xl_dsv_flags = 0;
      render->render_target_count = num_rtvs;

      if (!active_queries_.empty()) {
        // ENCODERS: a direct query whose pass has ended continues here on the legacy path; its results
        // already hold the count of that pass (Reset mode writes every selected offset).
        for (auto &query : active_queries_)
          if (query.mode == 2)
            query.mode = 0;
        if (active_queries_.size() == 1 && active_queries_.front().mode == 1 && SelectDirectQuery(active_queries_.front())) {
          // counted straight into the query heap by this pass
        } else {
          render->visibility_accumulate = active_queries_.front().heap->accumulate;
          for (const auto &query : active_queries_)
            render->visibility_targets.push_back({query.heap->results, uint64_t(query.index) * sizeof(uint64_t)});
          auto &visibility = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
          visibility.type = WMTRenderCommandSetVisibilityMode;
          visibility.mode = WMTVisibilityResultModeCounting;
          visibility.offset = 0;
        }
      }

      unsigned render_target_width = 16384, render_target_height = 16384, render_target_array_length = 0;

      unsigned effective_rtvs = 0;
      if (recording_trace_enabled_)
        memset(recording_rtvs_, 0, sizeof(recording_rtvs_));
      for (unsigned i = 0; i < num_rtvs; i++) {
        if (!rtvs[i].ptr)
          continue;
        effective_rtvs++;
        auto [Heap, Index] = GetRenderTargetHeap(device_, rtvs[i]);
        auto AttachmentDesc = Heap->GetRenderTarget(Index);
        if (!AttachmentDesc.Texture) {
          if (recording_trace_enabled_ && i < 8)
            std::snprintf(recording_rtvs_[i], sizeof(recording_rtvs_[i]), "resolution=no_texture");
          continue;
        }
        auto &rt = render->colors[i];
        rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
        rt.depth_plane = AttachmentDesc.DepthPlane;
        rt.load_action = WMTLoadActionLoad;
        rt.store_action = WMTStoreActionStore;
        // Copy only values from the view already resolved for the actual command.
        if (recording_trace_enabled_ && i < 8) {
          const DWORD error = GetLastError();
          std::snprintf(recording_rtvs_[i], sizeof(recording_rtvs_[i]), "resolution=resolved texture=%llx width=%u height=%u",
                        static_cast<unsigned long long>(rt.attachment.texture().handle),
                        unsigned(AttachmentDesc.Width), unsigned(AttachmentDesc.Height));
          SetLastError(error);
        }
        static std::atomic<uint32_t> trace_target{0};
        TraceFrame(trace_target, "render.target", this, "slot=%u texture=%llx width=%u height=%u pso=%p",
                   i, static_cast<unsigned long long>(rt.attachment.texture().handle),
                   unsigned(AttachmentDesc.Width), unsigned(AttachmentDesc.Height),
                   static_cast<const void *>(pso_graphics_.ptr()));
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
        ldc_w_[i] = AttachmentDesc.Width; ldc_h_[i] = AttachmentDesc.Height; ldc_layers_[i] = AttachmentDesc.RenderTargetArrayLength;
      }
      while (dsv.ptr) {
        auto [Heap, Index] = GetRenderTargetHeap(device_, dsv);
        auto AttachmentDesc = Heap->GetRenderTarget(Index);
        // C18-PRE (C18-QUEUE 2.6): a null DSV (CreateDepthStencilView(nullptr, ...)) binds no depth/stencil; `continue`
        // here looped forever (dsv does not change in the loop). Not counted as an attachment.
        if (!AttachmentDesc.Texture)
          break;
        effective_rtvs++;
        auto dsv_planar_flags = DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
        if (dsv_planar_flags & 1) {
          auto &rt = render->depth;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        if (dsv_planar_flags & 2) {
          auto &rt = render->stencil;
          rt.attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
          rt.depth_plane = 0; // DSV cannot be 3D
          rt.load_action = WMTLoadActionLoad;
          rt.store_action = WMTStoreActionStore;
        }
        ldc_w_[8] = AttachmentDesc.Width; ldc_h_[8] = AttachmentDesc.Height; ldc_layers_[8] = AttachmentDesc.RenderTargetArrayLength;
        render->dsv_planar_flags = dsv_planar_flags;
        // XL-DEFAULT (c18, 23.10.2026): apply D3D12_DSV_FLAG_READ_ONLY_DEPTH/STENCIL of the bound DSV, limited to the
        // planes the format has. GetDepthStencilState then selects the state without depth writes / with KEEP stencil
        // operations for a read-only plane (as DXMT's D3D11 path and vkd3d-proton do), and the redundant-transition
        // merge vetoes such passes (empty_effect_readonly). Load/Store of the attachment stay Load/Store: nothing
        // writes the plane, so the stored contents are the loaded ones (MoltenVK maps STORE_OP_NONE to Store).
        render->dsv_readonly_flags =
            uint8_t(AttachmentDesc.Flags & dsv_planar_flags & (D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL));
        render->xl_dsv_flags = uint8_t(AttachmentDesc.Flags & (D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL));
        render_target_width = std::min(render_target_width, AttachmentDesc.Width);
        render_target_height = std::min(render_target_height, AttachmentDesc.Height);
        render_target_array_length = std::max(render_target_array_length, AttachmentDesc.RenderTargetArrayLength);
        break;
      }
      render->render_target_width = render_target_width;
      render->render_target_height = render_target_height;
      render->render_target_array_length = render_target_array_length;
      if (effective_rtvs == 0) {
        render->default_raster_sample_count = std::max(1u, pso_graphics_->forced_sample_count);
      }

      dirty_state_.set(DirtyState::VertexBuffer, DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
      dirty_state_.set(DirtyState::Viewport, DirtyState::ScissorRect);
      dirty_state_.set(DirtyState::BlendFactor, DirtyState::StencilRef);
      dirty_state_.set(DirtyState::GraphicsPipelineState);
      ForgetEmittedState();   // a new Metal encoder starts with no state
      PostChainForget();      // POSTCS 03.11: the setbuffer commands of this pass are emitted below
      if (!pending_discards_.empty()) {
        // ENCODERS 4.2: a discarded attachment needs no load; its contents are undefined by D3D12 rules.
        const auto discarded = [&](const TextureViewRef &view) {
          if (!view) return false;
          const auto object = view->allocation->texture().handle;
          return std::find(pending_discards_.begin(), pending_discards_.end(), object) != pending_discards_.end();
        };
        for (auto &color : render->colors)
          if (discarded(color.attachment)) {
            color.load_action = WMTLoadActionDontCare;
            encoder_stats::Note(encoder_stats::Get().att_load_avoided);
          }
        if (discarded(render->depth.attachment)) {
          render->depth.load_action = WMTLoadActionDontCare;
          encoder_stats::Note(encoder_stats::Get().att_load_avoided);
        }
        if (discarded(render->stencil.attachment)) {
          render->stencil.load_action = WMTLoadActionDontCare;
          encoder_stats::Note(encoder_stats::Get().att_load_avoided);
        }
        pending_discards_.clear();
      }
    }

    if (!pso_graphics_) {
      if (recording_trace_enabled_)
        recording_predraw_reason_ = "PreDraw_invalid_missing_graphics_pso_after_pass";
      return DrawCallStatus::Invalid;
    }
    // c17 MERGE_DEPENDENCIES: remember that this pass may write memory besides its attachments.
    static_cast<RenderEncoderData *>(allocator_->encoder_current)->may_write_memory |= pso_graphics_->may_write_memory;
    // VS-STAGE RENDER_FENCE=3: and whether its pre-raster stages may read something other than buffers.
    static_cast<RenderEncoderData *>(allocator_->encoder_current)->pre_raster_typed |= pso_graphics_->pre_raster_reads_typed;
    // GPU-PASS TYPED_SAFE: and whether those reads are all SRVs.
    static_cast<RenderEncoderData *>(allocator_->encoder_current)->pre_raster_hard |=
        pso_graphics_->pre_raster_reads_typed && !pso_graphics_->pre_raster_srv_only;
    // c17 QUERY_NO_COLOR: a draw whose pipeline has no colourless twin keeps the colours of this pass.
    if (c17::QueryNoColor() && (!pso_graphics_->no_color_pso || MeshIndexVariant || mesh_pipeline))
      static_cast<RenderEncoderData *>(allocator_->encoder_current)->no_color_ok = false;

    const bool diff = encoder_stats::DiffState();
    if (dirty_state_.test(DirtyState::GraphicsPipelineState)) {
      const obj_handle_t pso_handle =
          (MeshIndexVariant ? pso_graphics_->mesh_indexed_pso[MeshIndexVariant - 1] : pso_graphics_->pso).handle;
      const obj_handle_t dss_handle = pso_graphics_->GetDepthStencilState(
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_planar_flags,
        static_cast<RenderEncoderData *>(allocator_->encoder_current)->dsv_readonly_flags
      ).handle;
      wmtcmd_render_setrasterizerstate rs{};
      rs.type = WMTRenderCommandSetRasterizerState;
      rs.cull_mode = pso_graphics_->cull_mode;
      rs.depth_clip_mode = pso_graphics_->depth_clip_mode;
      rs.fill_mode = pso_graphics_->fill_mode;
      rs.depth_bias = pso_graphics_->depth_bias;
      rs.depth_bias_clamp = pso_graphics_->depth_bias_clamp;
      rs.scole_scale = pso_graphics_->scole_scale;
      rs.winding = pso_graphics_->winding;

      if (!diff || !emitted_.pso || emitted_.pso_handle != pso_handle) {
        auto &cmd_setpso = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
        cmd_setpso.type = WMTRenderCommandSetPSO;
        cmd_setpso.pso = pso_handle;
        if (c17::QueryNoColor() && pso_graphics_->no_color_pso && !MeshIndexVariant)
          static_cast<RenderEncoderData *>(allocator_->encoder_current)->no_color_cmds.push_back(
              {&cmd_setpso, pso_graphics_->no_color_pso.handle});
      } else encoder_stats::Note(encoder_stats::Get().state_skipped);
      if (!diff || !emitted_.pso || emitted_.dss_handle != dss_handle) {
        auto &cmd_setdsso = allocator_->EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
        cmd_setdsso.type = WMTRenderCommandSetDepthStencilState;
        cmd_setdsso.depth_stencil_state = dss_handle;
      } else encoder_stats::Note(encoder_stats::Get().state_skipped);
      const auto *rs_payload = reinterpret_cast<const uint8_t *>(&rs) + sizeof(wmtcmd_base);
      if (!diff || !emitted_.pso || std::memcmp(emitted_.raster, rs_payload, sizeof(rs) - sizeof(wmtcmd_base))) {
        auto &cmd_setrs = allocator_->EncodeRenderCommand<wmtcmd_render_setrasterizerstate>();
        auto next = cmd_setrs.next;
        cmd_setrs = rs;
        cmd_setrs.next = next;
      } else encoder_stats::Note(encoder_stats::Get().state_skipped);
      emitted_.pso = true;
      emitted_.pso_handle = pso_handle;
      emitted_.dss_handle = dss_handle;
      std::memcpy(emitted_.raster, rs_payload, sizeof(rs) - sizeof(wmtcmd_base));

      dirty_state_.clr(DirtyState::GraphicsPipelineState);
    }
    if (dirty_state_.test(DirtyState::VertexBuffer) && diff && emitted_.vb && emitted_.vb_mesh == mesh_pipeline &&
        emitted_.vb_mask == (pso_graphics_ ? pso_graphics_->slot_mask : 0) && [&] {
          const uint32_t mask = emitted_.vb_mask;
          for (unsigned slot = 0; slot < 32; ++slot)
            if ((mask & (1u << slot)) && std::memcmp(&emitted_.vb_views[slot], &vertex_buffers_[slot], sizeof(D3D12_VERTEX_BUFFER_VIEW)))
              return false;
          return true;
        }()) {
      encoder_stats::Note(encoder_stats::Get().state_skipped);   // the same table is still bound in this pass
      dirty_state_.clr(DirtyState::VertexBuffer);
    }
    if (dirty_state_.test(DirtyState::VertexBuffer)) {
      emitted_.vb = true;
      emitted_.vb_mesh = mesh_pipeline;
      emitted_.vb_mask = pso_graphics_ ? pso_graphics_->slot_mask : 0;
      emitted_.vb_views = vertex_buffers_;
      auto [Offset, Stride] = PopulateVertexBufferTable(1);
      tp_vb_pass_ = nullptr;
      if (Stride) {
        auto &cmd_vsvb = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsvb.type = mesh_pipeline ? WMTRenderCommandSetObjectBuffer : WMTRenderCommandSetVertexBuffer;
        cmd_vsvb.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsvb.offset = Offset;
        cmd_vsvb.index = SM50_BINDING_INDEX_VERTEX_BUFFER;
        tp_vb_buffer_ = cmd_vsvb.buffer; tp_vb_offset_ = Offset; tp_vb_pass_ = allocator_->encoder_current;   // GPU-PASS TESS_PREPASS
        pc_vs_vb_ = &cmd_vsvb; pc_vb_pass_ = allocator_->encoder_current;   // POSTCS 03.11
      }
      dirty_state_.clr(DirtyState::VertexBuffer);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootArguments) && !SkipResourceBinding && diff && rootsig_graphics_ &&
        emitted_.root && emitted_.root_sig == rootsig_graphics_.ptr() && emitted_.root_mesh == mesh_pipeline &&
        emitted_.root_qwords == rootsig_graphics_->UploadQwords && emitted_.root_qwords <= 64 &&
        !std::memcmp(emitted_.root_words, rootarg_graphics_staging_.words.data(), emitted_.root_qwords * sizeof(uint64_t))) {
      encoder_stats::Note(encoder_stats::Get().state_skipped);   // identical root arguments are already bound
      dirty_state_.clr(DirtyState::GraphicsRootArguments);
    }
    if (dirty_state_.test(DirtyState::GraphicsRootArguments) && !SkipResourceBinding) {
      if (rootsig_graphics_) {
        emitted_.root = rootsig_graphics_->UploadQwords <= 64;
        emitted_.root_sig = rootsig_graphics_.ptr();
        emitted_.root_mesh = mesh_pipeline;
        emitted_.root_qwords = rootsig_graphics_->UploadQwords;
        if (emitted_.root)
          std::memcpy(emitted_.root_words, rootarg_graphics_staging_.words.data(), emitted_.root_qwords * sizeof(uint64_t));
        auto Offset = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_.words.data());
        auto &cmd_vsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsargbuf.type = mesh_pipeline ? WMTRenderCommandSetObjectBuffer : WMTRenderCommandSetVertexBuffer;
        cmd_vsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsargbuf.offset = Offset;
        cmd_vsargbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        tp_root_buffer_ = cmd_vsargbuf.buffer; tp_root_offset_ = Offset; tp_root_pass_ = allocator_->encoder_current;   // GPU-PASS TESS_PREPASS
        wmtcmd_render_setbuffer *lightrn_mesh = nullptr;   // LIGHTRN
        if (mesh_pipeline) {
          auto &cmd_dsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          cmd_dsargbuf.type = WMTRenderCommandSetMeshBuffer;
          cmd_dsargbuf.buffer = allocator_->gpu_heap_buffer_;
          cmd_dsargbuf.offset = Offset;
          cmd_dsargbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
          lightrn_mesh = &cmd_dsargbuf;
        }
        auto &cmd_fsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_fsargbuf.type = WMTRenderCommandSetFragmentBuffer;
        cmd_fsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_fsargbuf.offset = Offset;
        cmd_fsargbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        pc_vs_root_ = &cmd_vsargbuf; pc_fs_root_ = &cmd_fsargbuf; pc_root_pass_ = allocator_->encoder_current;   // POSTCS 03.11
        if (lightrn_seen_ && light_rename::Armed())   // LIGHTRN: a block a later rename may redirect
          LightrnRecordRoot(&cmd_vsargbuf, lightrn_mesh, &cmd_fsargbuf, Offset);
      }
      dirty_state_.clr(DirtyState::GraphicsRootArguments);
    }

    if (dirty_state_.test(DirtyState::GraphicsRootSignature) && !SkipResourceBinding && diff && rootsig_graphics_ &&
        emitted_.sampler && emitted_.sampler_sig == rootsig_graphics_.ptr() && emitted_.sampler_mesh == mesh_pipeline) {
      encoder_stats::Note(encoder_stats::Get().state_skipped);
      dirty_state_.clr(DirtyState::GraphicsRootSignature);
    }
    if (dirty_state_.test(DirtyState::GraphicsRootSignature) && !SkipResourceBinding) {
      if (rootsig_graphics_ && (!binding_elision::SkipEmptyStatic() || rootsig_graphics_->NumStaticSamplers)) {
        emitted_.sampler = true;
        emitted_.sampler_sig = rootsig_graphics_.ptr();
        emitted_.sampler_mesh = mesh_pipeline;
        auto Offset = EncodeStaticSamplers(rootsig_graphics_.ptr());
        auto &cmd_vsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_vsargbuf.type = mesh_pipeline ? WMTRenderCommandSetObjectBuffer : WMTRenderCommandSetVertexBuffer;
        cmd_vsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_vsargbuf.offset = Offset;
        cmd_vsargbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
        tp_sampler_buffer_ = cmd_vsargbuf.buffer; tp_sampler_offset_ = Offset; tp_sampler_pass_ = allocator_->encoder_current;   // GPU-PASS TESS_PREPASS
        if (mesh_pipeline) {
          auto &cmd_dsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
          cmd_dsargbuf.type = WMTRenderCommandSetMeshBuffer;
          cmd_dsargbuf.buffer = allocator_->gpu_heap_buffer_;
          cmd_dsargbuf.offset = Offset;
          cmd_dsargbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
        }
        auto &cmd_fsargbuf = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd_fsargbuf.type = WMTRenderCommandSetFragmentBuffer;
        cmd_fsargbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_fsargbuf.offset = Offset;
        cmd_fsargbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
        pc_vs_samplers_ = &cmd_vsargbuf; pc_fs_samplers_ = &cmd_fsargbuf; pc_sampler_pass_ = allocator_->encoder_current;   // POSTCS 03.11
      }
      dirty_state_.clr(DirtyState::GraphicsRootSignature);
    }

    if (dirty_state_.test(DirtyState::Viewport) && diff && emitted_.vp && emitted_.vp_count == num_viewports &&
        !std::memcmp(emitted_.vp_values, viewports, sizeof(D3D12_VIEWPORT) * num_viewports)) {
      encoder_stats::Note(encoder_stats::Get().state_skipped);
      dirty_state_.clr(DirtyState::Viewport);
    }
    if (dirty_state_.test(DirtyState::Viewport)) {
      emitted_.vp = num_viewports <= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
      emitted_.vp_count = num_viewports;
      if (emitted_.vp) std::memcpy(emitted_.vp_values, viewports, sizeof(D3D12_VIEWPORT) * num_viewports);
      static std::atomic<uint32_t> trace_viewport{0};
      TraceFrame(trace_viewport, "render.viewport", this, "count=%u first=%f,%f,%f,%f depth=%f,%f",
                 num_viewports, double(viewports[0].TopLeftX), double(viewports[0].TopLeftY),
                 double(viewports[0].Width), double(viewports[0].Height),
                 double(viewports[0].MinDepth), double(viewports[0].MaxDepth));
      auto metal_viewport = allocator_->AllocateCommandData<WMTViewport>(num_viewports);
      for (auto i = 0u; i < num_viewports; i++) {
        auto &viewport = viewports[i];
        metal_viewport[i] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                             viewport.Height,   viewport.MinDepth, viewport.MaxDepth};
      }
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setviewports>();
      cmd.type = WMTRenderCommandSetViewports;
      cmd.viewports.set(metal_viewport);
      cmd.viewport_count = num_viewports;
      dirty_state_.clr(DirtyState::Viewport);
    }

    if (dirty_state_.test(DirtyState::ScissorRect) && diff && emitted_.sc && emitted_.sc_vp_count == num_viewports &&
        emitted_.sc_count == num_scissors && !std::memcmp(emitted_.sc_values, scissors, sizeof(D3D12_RECT) * num_scissors)) {
      encoder_stats::Note(encoder_stats::Get().state_skipped);
      dirty_state_.clr(DirtyState::ScissorRect);
    }
    if (dirty_state_.test(DirtyState::ScissorRect)) {
      emitted_.sc = num_scissors <= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
      emitted_.sc_vp_count = num_viewports;
      emitted_.sc_count = num_scissors;
      if (emitted_.sc) std::memcpy(emitted_.sc_values, scissors, sizeof(D3D12_RECT) * num_scissors);
      static std::atomic<uint32_t> trace_scissor{0};
      TraceFrame(trace_scissor, "render.scissor", this, "count=%u first=%d,%d,%d,%d",
                 num_scissors, int(scissors[0].left), int(scissors[0].top), int(scissors[0].right), int(scissors[0].bottom));
      auto metal_scissors = allocator_->AllocateCommandData<WMTScissorRect>(num_viewports /* yes */);
      for (auto i = 0u; i < num_viewports; i++) {
        if (i < num_scissors) {
          auto &d3d_rect = scissors[i];
          LONG left = std::clamp(d3d_rect.left, (LONG)0, (LONG)16384);
          LONG top = std::clamp(d3d_rect.top, (LONG)0, (LONG)16384);
          LONG right = std::clamp(d3d_rect.right, left, (LONG)16384);
          LONG bottom = std::clamp(d3d_rect.bottom, top, (LONG)16384);
          metal_scissors[i] = {uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
        } else {
          metal_scissors[i] = {0, 0, 16384, 16384};
        }
      }
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setscissorrects>();
      cmd.type = WMTRenderCommandSetScissorRects;
      cmd.scissor_rects.set(metal_scissors);
      cmd.rect_count = num_viewports;
      dirty_state_.clr(DirtyState::ScissorRect);
    }

    if (dirty_state_.test(DirtyState::BlendFactor) && diff && emitted_.blend &&
        !std::memcmp(emitted_.blend_values, blend_factor_, sizeof(blend_factor_))) {
      encoder_stats::Note(encoder_stats::Get().state_skipped);
      dirty_state_.clr(DirtyState::BlendFactor);
    }
    if (dirty_state_.test(DirtyState::BlendFactor)) {
      emitted_.blend = true;
      std::memcpy(emitted_.blend_values, blend_factor_, sizeof(blend_factor_));
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setblendcolor>();
      cmd.type = WMTRenderCommandSetBlendFactor;
      cmd.red = blend_factor_[0];
      cmd.green = blend_factor_[1];
      cmd.blue = blend_factor_[2];
      cmd.alpha = blend_factor_[3];
      dirty_state_.clr(DirtyState::BlendFactor);
    }

    if (dirty_state_.test(DirtyState::StencilRef) && diff && emitted_.stencil && emitted_.stencil_value == stencil_ref_) {
      encoder_stats::Note(encoder_stats::Get().state_skipped);
      dirty_state_.clr(DirtyState::StencilRef);
    }
    if (dirty_state_.test(DirtyState::StencilRef)) {
      emitted_.stencil = true;
      emitted_.stencil_value = stencil_ref_;
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setstencilref>();
      cmd.type = WMTRenderCommandSetStencilRef;
      cmd.stencil_ref = stencil_ref_;
      dirty_state_.clr(DirtyState::StencilRef);
    }

    ++static_cast<RenderEncoderData *>(allocator_->encoder_current)->post_chain_draws;   // POSTCS 03.11: draws of this pass
    RecordMergeDraw(SkipResourceBinding);
    if (c17::Enabled() || xl_hazard::Enabled()) {
      auto &render = *static_cast<RenderEncoderData *>(allocator_->encoder_current);
      if (render.empty_binding_first == UINT64_MAX) render.empty_binding_first = empty_binding_epoch;
      render.empty_binding_last = empty_binding_epoch;
    }
    // ENCODERS 4.3: the mesh paths of tessellation / geometry (and indirect draws) set encoder state of their own.
    if (pso_graphics_->geometry_pipeline || pso_graphics_->tess_control_points || SkipResourceBinding)
      ForgetEmittedState();
    if (pso_graphics_->geometry_pipeline)
      return DrawCallStatus::Geometry;
    return pso_graphics_->tess_control_points ? DrawCallStatus::Tessellation : DrawCallStatus::Ordinary;
  }

  void STDMETHODCALLTYPE
  DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) { DXMT_LT(5);
    if (!RequireOpenRecording("DrawInstanced", DXMT_OPCLASS("DrawInstanced"))) return;
    if (failed_async_pso_) { pso_work::Skipped("draw"); return; }
    try {
    static std::atomic<uint32_t> trace_enter{0}, trace_record{0};
    TraceFrame(trace_enter, "draw.enter", this, "count=%u instances=%u pso=%p topology=%u hr=%08x",
               VertexCountPerInstance, InstanceCount, static_cast<const void *>(pso_graphics_.ptr()),
               unsigned(topology_), unsigned(recording_error_));
    const auto trace_outcome = [&](const char *outcome, const char *reason) {
      TraceRecording("DrawInstanced", outcome, reason,
                     "count=%u instances=%u start_vertex=%u start_instance=%u topology=%u zero_count=%u zero_instances=%u recording_hr=%08x",
                     VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation,
                     unsigned(topology_), unsigned(!VertexCountPerInstance), unsigned(!InstanceCount), unsigned(recording_error_));
    };
    const auto trace_recorded = [&](const char *type) {
      if (VertexCountPerInstance && InstanceCount) AuditVertexInputs("draw");
      AuditReuseDraw(false, {0, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation});
      AuditHazardDraw();
      RecordGpu4Reads();
      CountGpu4Geometry(0, VertexCountPerInstance, InstanceCount);
      trace_outcome("emitted", type);
      TraceFrame(trace_record, "draw.recorded", this, "type=%s count=%u instances=%u pso=%p hr=%08x", type,
                 VertexCountPerInstance, InstanceCount, static_cast<const void *>(pso_graphics_.ptr()), unsigned(recording_error_));
    };
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "invalid_topology");
      return;
    }
    if (!pso_graphics_) {
      trace_outcome("rejected", "missing_graphics_pso");
      return;
    }
    if (cp_count != pso_graphics_->tess_control_points) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "patch_control_point_mismatch");
      return;
    }
    if (pso_graphics_->geometry_pipeline) {
      const auto expected = pso_graphics_->geometry_input_vertices == 1 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST :
                            pso_graphics_->geometry_input_vertices == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      if (topology_ != expected) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        trace_outcome("rejected", "geometry_input_topology_mismatch");
        return;
      }
      if (VertexCountPerInstance < pso_graphics_->geometry_input_vertices || !InstanceCount) {
        trace_outcome("rejected", !VertexCountPerInstance ? "zero_vertex_count" : !InstanceCount ? "zero_instance_count" :
                      "geometry_incomplete_primitive");
        return;
      }
      if (VertexCountPerInstance % pso_graphics_->geometry_input_vertices) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "geometry_partial_primitive_unsupported");
        return;
      }
      const uint32_t vertices_per_group = pso_graphics_->geometry_vertices_per_group;
      const auto groups = 1 + (VertexCountPerInstance - 1) / vertices_per_group;
      if (!ObjectGridFits(groups, InstanceCount, max_object_threadgroups_)) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "geometry_object_group_limit");
        return;
      }
      if (PreDraw() != DrawCallStatus::Geometry) {
        trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_expected_geometry");
        return;
      }
      const D3D12_DRAW_ARGUMENTS arguments = {
          VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation};
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(arguments), 32);
      memcpy(mapped, &arguments, sizeof(arguments));
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = allocator_->gpu_heap_buffer_;
      binding.offset = 0;
      binding.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_geometry_draw>();
      draw.type = WMTRenderCommandDXMTGeometryDraw;
      draw.draw_arguments_offset = offset;
      draw.instance_count = InstanceCount;
      draw.warp_count = groups;
      draw.vertex_per_warp = vertices_per_group;
      trace_recorded("geometry");
      return;
    }
    if (cp_count) {
      const auto threads = pso_graphics_->tess_threads_per_patch;
      const auto patches = VertexCountPerInstance / cp_count;
      if (!patches || !InstanceCount) {
        trace_outcome("rejected", !VertexCountPerInstance ? "zero_vertex_count" : !InstanceCount ? "zero_instance_count" :
                      "tessellation_zero_complete_patches");
        return;
      }
      if (!threads || threads > 32 || 32 % threads) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        trace_outcome("rejected", "invalid_tessellation_threads_per_patch");
        return;
      }
      const auto patches_per_group = 32 / threads;
      const auto groups = 1 + (patches - 1) / patches_per_group;
      // Object-grid policy is device-specific; child mesh limits remain in the PSO.
      if (!ObjectGridFits(groups, InstanceCount, max_object_threadgroups_)) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "tessellation_object_group_limit");
        return;
      }
      if (tess_prepass::Verify()) EndPass();
      if (PreDraw() != DrawCallStatus::Tessellation) {
        trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_expected_tessellation");
        return;
      }
      const D3D12_DRAW_ARGUMENTS arguments = {
          VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation};
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(arguments), 32);
      memcpy(mapped, &arguments, sizeof(arguments));
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = allocator_->gpu_heap_buffer_;
      binding.offset = 0;
      binding.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      if (tess_prepass::Enabled() && RecordTessPrepass(groups, InstanceCount, patches, offset, 0, 0, 0)) {   // GPU-PASS TESS_PREPASS
        TessCensusDraw(patches, InstanceCount, 0, {VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation}, false);
        trace_recorded("tessellation_prepass");
        if (tess_prepass::Verify()) EndPass();
        return;
      }
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw>();
      draw.type = WMTRenderCommandDXMTTessellationMeshDraw;
      draw.draw_arguments_offset = offset;
      draw.instance_count = InstanceCount;
      draw.threads_per_patch = threads;
      draw.patch_per_group = patches_per_group;
      draw.patch_per_mesh_instance = groups;
      TessCensusDraw(patches, InstanceCount, 0, {VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation}, false);
      trace_recorded("tessellation");
      return;
    }
    DrawCallStatus status = PreDraw();
    if (status == DrawCallStatus::Invalid) {
      trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_invalid");
      return;
    }
    if (ldc_first_draw_) {
      ldc_start_vertex_ = StartVertexLocation; ldc_start_instance_ = StartInstanceLocation;
      LoadDontCareFirstDraw(VertexCountPerInstance, InstanceCount);
    }

    if (render_fence::StatsPeriod())   // VS-STAGE FENCE_STATS
      static_cast<RenderEncoderData *>(allocator_->encoder_current)->vertex_work += uint64_t(VertexCountPerInstance) * InstanceCount;
    auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
    cmd_draw.type = WMTRenderCommandDraw;
    cmd_draw.primitive_type = primitive_type;
    cmd_draw.base_instance = StartInstanceLocation;
    cmd_draw.instance_count = InstanceCount;
    cmd_draw.vertex_start = StartVertexLocation;
    cmd_draw.vertex_count = VertexCountPerInstance;
    if (post_chain::Armed())   // POSTCS 03.11: the side record of a candidate draw (MACRUNNER_DX12_POST_CHAIN armed only)
      PostChainRecord(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation, primitive_type);
    if (ldc_probe_after_) {   // ENC-FIX LOAD_DONTCARE: the probe counted exactly this draw
      ldc_probe_after_ = false;
      auto &off = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
      off.type = WMTRenderCommandSetVisibilityMode;
      off.mode = WMTVisibilityResultModeDisabled;
      off.offset = uint64_t(ldc_probe_slot_) * sizeof(uint64_t);
      static_cast<RenderEncoderData *>(allocator_->encoder_current)->ldc_probe_off = &off;
    }
    trace_recorded(!VertexCountPerInstance || !InstanceCount ? "ordinary_zero_work" : "ordinary");
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "DrawInstanced", __LINE__);
      TraceRecording("DrawInstanced", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  // ENC-FIX LOAD_DONTCARE (MACRUNNER_DX12_LOAD_DONTCARE=1): the first draw of a new pass decides whether an attachment
  // that would be loaded is overwritten at every pixel by that draw (rule: d3d12_enc_census.hpp); then it starts DontCare.
  void LoadDontCareFirstDraw(UINT vertices, UINT instances) {
    ldc_first_draw_ = false;
    auto *pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    if (!pass || pass->type != EncoderType::Render || !pso_graphics_) return;
    unsigned loads = 0;
    for (unsigned k = 0; k < 8; ++k) loads += pass->colors[k].attachment && pass->colors[k].load_action == WMTLoadActionLoad;
    const bool depth_load = pass->depth.attachment && pass->depth.load_action == WMTLoadActionLoad;
    if (!loads && !depth_load) return;
    auto &st = c17::Get();
    c17::Note(st.ldc_first_draws);
    c17::Note(st.ldc_att_load, loads);
    std::atomic<uint64_t> *const pso_fail[] = {nullptr, &st.ldc_fail_stage, &st.ldc_fail_layout, &st.ldc_fail_topo,
        &st.ldc_fail_raster, &st.ldc_fail_sample, &st.ldc_fail_ds, &st.ldc_fail_vs, &st.ldc_fail_ps, &st.ldc_fail_bytecode,
        &st.ldc_fail_rt};
    const uint8_t r = pso_graphics_->ldc_reason;
    if (r) {
      if (r < std::size(pso_fail)) c17::Note(*pso_fail[r]);
      return;
    }
    const bool shape = instances == 1 && vertices == 3 && topology_ == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (!shape) {
      c17::Note(st.ldc_fail_draw);
      return;
    }
    const uint32_t W = pass->render_target_width, H = pass->render_target_height;
    const auto &vp = viewports[0];
    const auto &sc = scissors[0];
    if (!num_viewports || vp.TopLeftX > 0.f || vp.TopLeftY > 0.f || vp.TopLeftX + vp.Width < float(W) || vp.TopLeftY + vp.Height < float(H)) {
      c17::Note(st.ldc_fail_viewport); return;
    }
    if (!num_scissors || sc.left > 0 || sc.top > 0 || sc.right < LONG(W) || sc.bottom < LONG(H)) { c17::Note(st.ldc_fail_scissor); return; }
    // every attachment exactly the render area, one slice (outside the render area a DontCare load would leave garbage)
    for (unsigned k = 0; k < 9; ++k) {
      const bool present = k < 8 ? bool(pass->colors[k].attachment) : bool(pass->depth.attachment) || bool(pass->stencil.attachment);
      if (!present) continue;
      if (ldc_layers_[k] > 1) { c17::Note(st.ldc_fail_array); return; }
      if (ldc_w_[k] != W || ldc_h_[k] != H) { c17::Note(st.ldc_fail_size); return; }
    }
    if (!pass->visibility_targets.empty() || pass->visibility_direct || !active_queries_.empty()) { c17::Note(st.ldc_fail_query); return; }
    // the proof
    auto &reg = ldc_proof::Get();
    ldc_proof::Key key{};
    key.pso = pso_graphics_->ldc_id;
    if (!key.pso) return;
    key.vp[0] = vp.TopLeftX; key.vp[1] = vp.TopLeftY; key.vp[2] = vp.Width; key.vp[3] = vp.Height; key.vp[4] = vp.MinDepth; key.vp[5] = vp.MaxDepth;
    key.w = W; key.h = H; key.start_vertex = ldc_start_vertex_; key.start_instance = ldc_start_instance_;
    bool proven = false, probe = false;
    uint32_t slot = 0;
    {
      std::lock_guard guard(reg.m);
      if (!reg.device) {
        WMTBufferInfo info{};
        info.length = uint64_t(ldc_proof::kSlots) * sizeof(uint64_t);
        info.options = WMTResourceStorageModeShared;
        reg.buffer = device_->GetMTLDevice().newBuffer(info);
        reg.slots = static_cast<uint64_t *>(info.memory.get_accessible_or_null());
        reg.device = device_;
        if (reg.slots) for (uint32_t i = 0; i < ldc_proof::kSlots; ++i) reg.slots[i] = ldc_proof::kSentinel;
      }
      if (reg.device != device_ || !reg.buffer || !reg.slots) return;
      const auto start = [&](ldc_proof::Entry &e) {
        if (reg.next >= ldc_proof::kSlots) return false;
        e.slot = reg.next++; e.state = ldc_proof::Pending; e.pending_uses = 0;
        e.expected = uint64_t(W) * H * pso_graphics_->ldc_samples;
        probe = true; slot = e.slot;
        return true;
      };
      auto it = reg.map.find(key);
      if (it == reg.map.end()) {
        ldc_proof::Entry e{};
        if (!start(e)) return;
        reg.map.emplace(key, e);
      } else {
        auto &e = it->second;
        if (e.state == ldc_proof::Pending) {
          const uint64_t v = __atomic_load_n(&reg.slots[e.slot], __ATOMIC_ACQUIRE);
          if (v != ldc_proof::kSentinel) {
            e.state = v == e.expected ? ldc_proof::Proven : ldc_proof::Disproven;
            c17::Note(e.state == ldc_proof::Proven ? st.ldc_proven : st.ldc_disproven);
            static std::atomic<uint32_t> printed{0};
            if (printed.fetch_add(1, std::memory_order_relaxed) < 32)
              fprintf(stderr, "dx12_ldc_proof pso=%llu w=%u h=%u samples=%llu expected=%llu verdict=%s\n",
                      (unsigned long long)key.pso, W, H, (unsigned long long)v, (unsigned long long)e.expected,
                      e.state == ldc_proof::Proven ? "covers" : "partial");
          } else if (++e.pending_uses > 512) {
            // the probing list never ran (reset before execution): probe again in a fresh slot
            if (!start(e)) return;
          } else {
            c17::Note(st.ldc_probe_pending);
          }
        }
        if (e.state == ldc_proof::Proven) proven = true;
        else if (e.state == ldc_proof::Disproven) { c17::Note(st.ldc_fail_coverage); return; }
      }
    }
    if (probe) {
      // count the samples of this one draw into the slot; the attachments keep Load until the count has been read back.
      // A service query: not visibility_direct / direct_slots, so it vetoes no merge (the queue decides where it counts).
      pass->ldc_probe = reg.buffer;
      pass->ldc_probe_slot = slot;
      auto &on = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
      on.type = WMTRenderCommandSetVisibilityMode;
      on.mode = WMTVisibilityResultModeCounting;
      on.offset = uint64_t(slot) * sizeof(uint64_t);
      pass->ldc_probe_on = &on;
      ldc_probe_after_ = true; ldc_probe_slot_ = slot;
      c17::Note(st.ldc_probe_started);
      return;
    }
    if (!proven) return;
    // Proven: the attachments are FLAGGED; the queue turns a flag into DontCare only when this pass opens an encoder.
    unsigned flagged = 0;
    for (unsigned k = 0; k < 8; ++k) {
      auto &c = pass->colors[k];
      if (!c.attachment || c.load_action != WMTLoadActionLoad) continue;
      if (!((pso_graphics_->ldc_color_mask >> k) & 1)) { c17::Note(st.ldc_att_rt_fail); continue; }
      pass->ldc_dontcare_mask |= uint16_t(1u << k);
      ++flagged;
    }
    // C18-PRE: a read-only depth plane (XL-DEFAULT DSV flags) is not written by the draw, so its load must stay Load.
    if (depth_load && pso_graphics_->ldc_depth && !(pass->dsv_readonly_flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH)) {
      pass->ldc_dontcare_mask |= uint16_t(1u << 8);
      ++flagged;
    }
    if (flagged) c17::Note(st.ldc_att_flagged, flagged);
  }

  bool RetainIndexAllocation() {
    auto *pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    try {
      if (pass->index_allocations.empty() || pass->index_allocations.back() != index_allocation_)
        pass->index_allocations.push_back(index_allocation_);
      return true;
    } catch (const std::bad_alloc &) {
      SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);
      return false;
    }
  }

  void STDMETHODCALLTYPE
  DrawIndexedInstanced(
      UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
      UINT StartInstanceLocation
  ) { DXMT_LT(6);
    if (!RequireOpenRecording("DrawIndexedInstanced", DXMT_OPCLASS("DrawIndexedInstanced"))) return;
    if (failed_async_pso_) { pso_work::Skipped("draw_indexed"); return; }
    try {
    static std::atomic<uint32_t> trace_enter{0}, trace_record{0};
    TraceFrame(trace_enter, "draw_indexed.enter", this, "count=%u instances=%u pso=%p topology=%u hr=%08x",
               IndexCountPerInstance, InstanceCount, static_cast<const void *>(pso_graphics_.ptr()),
               unsigned(topology_), unsigned(recording_error_));
    const auto trace_outcome = [&](const char *outcome, const char *reason) {
      TraceRecording("DrawIndexedInstanced", outcome, reason,
                     "count=%u instances=%u start_index=%u base_vertex=%d start_instance=%u topology=%u zero_count=%u zero_instances=%u recording_hr=%08x",
                     IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation,
                     unsigned(topology_), unsigned(!IndexCountPerInstance), unsigned(!InstanceCount), unsigned(recording_error_));
    };
    const auto trace_recorded = [&](const char *type) {
      if (IndexCountPerInstance && InstanceCount) AuditVertexInputs("indexed");
      AuditReuseDraw(false, {1, IndexCountPerInstance, InstanceCount, StartIndexLocation, uint32_t(BaseVertexLocation), StartInstanceLocation}, true);
      AuditHazardDraw();
      RecordGpu4Reads();
      CountGpu4Geometry(1, IndexCountPerInstance, InstanceCount);
      trace_outcome("emitted", type);
      TraceFrame(trace_record, "draw_indexed.recorded", this, "type=%s count=%u instances=%u pso=%p hr=%08x", type,
                 IndexCountPerInstance, InstanceCount, static_cast<const void *>(pso_graphics_.ptr()), unsigned(recording_error_));
    };
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "invalid_topology");
      return;
    }
    if (!pso_graphics_ || cp_count != pso_graphics_->tess_control_points) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", !pso_graphics_ ? "missing_graphics_pso" : "patch_control_point_mismatch");
      return;
    }
    if (!IndexCountPerInstance || !InstanceCount) {
      trace_outcome("rejected", !IndexCountPerInstance ? "zero_index_count" : "zero_instance_count");
      return;
    }
    const uint32_t index_size = index_type == WMTIndexTypeUInt32 ? 4 : 2;
    if (!index_buffer || uint64_t(StartIndexLocation) + IndexCountPerInstance > index_buffer_size / index_size) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", !index_buffer ? "missing_index_buffer" : "index_buffer_range_exceeded");
      return;
    }
    if (pso_graphics_->geometry_pipeline) {
      const auto vertices = pso_graphics_->geometry_input_vertices;
      const auto expected = vertices == 1 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST :
                            vertices == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      if (topology_ != expected) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        trace_outcome("rejected", "geometry_input_topology_mismatch");
        return;
      }
      if (IndexCountPerInstance % vertices) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "geometry_partial_primitive_unsupported");
        return;
      }
      const auto vertices_per_group = pso_graphics_->geometry_vertices_per_group;
      const auto groups = 1 + (IndexCountPerInstance - 1) / vertices_per_group;
      if (!ObjectGridFits(groups, InstanceCount, max_object_threadgroups_)) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "geometry_object_group_limit");
        return;
      }
      if (PreDraw(false, index_size == 4 ? 2 : 1) != DrawCallStatus::Geometry) {
        trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_expected_geometry");
        return;
      }
      if (!RetainIndexAllocation()) return;
      const D3D12_DRAW_INDEXED_ARGUMENTS arguments = {
          IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation};
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(arguments), 32);
      memcpy(mapped, &arguments, sizeof(arguments));
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = allocator_->gpu_heap_buffer_;
      binding.offset = 0;
      binding.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_geometry_draw_indexed>();
      draw.type = WMTRenderCommandDXMTGeometryDrawIndexed;
      draw.draw_arguments_offset = offset;
      draw.instance_count = InstanceCount;
      draw.warp_count = groups;
      draw.vertex_per_warp = vertices_per_group;
      draw.index_buffer = index_buffer;
      // The object shader applies StartIndexLocation; bind only the view base.
      draw.index_buffer_offset = index_offset;
      trace_recorded("geometry");
      return;
    }
    if (cp_count) {
      const auto patches = IndexCountPerInstance / cp_count;
      const auto threads = pso_graphics_->tess_threads_per_patch;
      if (!patches) {
        trace_outcome("rejected", "tessellation_zero_complete_patches");
        return;
      }
      if (!threads || threads > 32 || 32 % threads) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        trace_outcome("rejected", "invalid_tessellation_threads_per_patch");
        return;
      }
      const auto patches_per_group = 32 / threads;
      const auto groups = 1 + (patches - 1) / patches_per_group;
      if (!ObjectGridFits(groups, InstanceCount, max_object_threadgroups_)) {
        SetRecordingError(E_NOTIMPL, __func__, __LINE__);
        trace_outcome("rejected", "tessellation_object_group_limit");
        return;
      }
      if (tess_prepass::Verify()) EndPass();
      if (PreDraw(false, index_size == 4 ? 2 : 1) != DrawCallStatus::Tessellation) {
        trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_expected_tessellation");
        return;
      }
      const D3D12_DRAW_INDEXED_ARGUMENTS arguments = {
          IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation};
      auto [mapped, offset] = allocator_->AllocateGPUHeap(sizeof(arguments), 32);
      memcpy(mapped, &arguments, sizeof(arguments));
      auto &binding = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      binding.type = WMTRenderCommandSetObjectBuffer;
      binding.buffer = allocator_->gpu_heap_buffer_;
      binding.offset = 0;
      binding.index = SM50_BINDING_INDEX_DRAW_ARGUMENTS;
      if (!RetainIndexAllocation()) return;
      if (tess_prepass::Enabled() &&   // GPU-PASS TESS_PREPASS (the hull applies StartIndex; bind only the view's base)
          RecordTessPrepass(groups, InstanceCount, patches, offset, index_size == 4 ? 2 : 1, index_buffer, index_offset)) {
        TessCensusDraw(patches, InstanceCount, 0, {IndexCountPerInstance, InstanceCount, StartIndexLocation, uint64_t(int64_t(BaseVertexLocation)),
                                                     StartInstanceLocation}, true);
        trace_recorded("tessellation_prepass");
        if (tess_prepass::Verify()) EndPass();
        return;
      }
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw_indexed>();
      draw.type = WMTRenderCommandDXMTTessellationMeshDrawIndexed;
      draw.draw_arguments_offset = offset;
      draw.instance_count = InstanceCount;
      draw.threads_per_patch = threads;
      draw.patch_per_group = patches_per_group;
      draw.patch_per_mesh_instance = groups;
      draw.index_buffer = index_buffer;
      // The hull shader applies StartIndex; bind only the index view's base.
      draw.index_buffer_offset = index_offset;
      TessCensusDraw(patches, InstanceCount, 0, {IndexCountPerInstance, InstanceCount, StartIndexLocation, uint64_t(int64_t(BaseVertexLocation)),
                                                   StartInstanceLocation}, true);
      trace_recorded("tessellation");
      return;
    }
    DrawCallStatus status = PreDraw();
    if (status == DrawCallStatus::Invalid) {
      trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_invalid");
      return;
    }
    if (!RetainIndexAllocation()) return;
    if (render_fence::StatsPeriod())   // VS-STAGE FENCE_STATS
      static_cast<RenderEncoderData *>(allocator_->encoder_current)->vertex_work += uint64_t(IndexCountPerInstance) * InstanceCount;
    auto &cmd_draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_indexed>();
    cmd_draw.type = WMTRenderCommandDrawIndexed;
    cmd_draw.primitive_type = primitive_type;
    cmd_draw.index_type = index_type;
    cmd_draw.index_count = IndexCountPerInstance;
    cmd_draw.index_buffer = index_buffer;
    cmd_draw.index_buffer_offset = index_offset + uint64_t(StartIndexLocation) * index_size;
    cmd_draw.instance_count = InstanceCount;
    cmd_draw.base_vertex = BaseVertexLocation;
    cmd_draw.base_instance = StartInstanceLocation;
    trace_recorded("ordinary");
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "DrawIndexedInstanced", __LINE__);
      TraceRecording("DrawIndexedInstanced", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  uint64_t
  EncodeRootArgument(MTLD3D12RootSignature *pRootSig, uint64_t const pStaging[64], UINT Count = 1) {
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(sizeof(uint64_t) * pRootSig->UploadQwords * Count, 64);
    diagnostic::Add(diagnostic::Counter::RootUploadCalls);
    diagnostic::Add(diagnostic::Counter::RootUploadBytes, sizeof(uint64_t) * pRootSig->UploadQwords * Count);
    for (unsigned i = 0; i < Count; i++)
      memcpy(
          reinterpret_cast<uint64_t *>(Ptr) + i * pRootSig->UploadQwords, pStaging,
          pRootSig->UploadQwords * sizeof(uint64_t)
      );
    return Offset;
  }

  uint64_t
  EncodeStaticSamplers(MTLD3D12RootSignature *pRootSig) {
    auto static_sampler_encode_size = sizeof(uint64_t) * pRootSig->NumStaticSamplers * 4;
    auto [Ptr, Offset] = allocator_->AllocateGPUHeap(static_sampler_encode_size, 64);
    diagnostic::Add(diagnostic::Counter::StaticSamplerBytes, static_sampler_encode_size);
    memcpy(Ptr, pRootSig->EncodedStaticSamplers, static_sampler_encode_size);
    return Offset;
  }

  bool
  PreDispatch(bool SkipResourceBinding = false) {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Compute) {
      ENCODERS_REASON(ReasonSwitchCompute);
      EndPass();
      auto compute = allocator_->AllocatePass<ComputeEncoderData>();
      compute->type = EncoderType::Compute;
      compute->cmd_head.type = WMTComputeCommandNop;
      compute->cmd_head.next.set(0);
      compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
      compute->may_write_buffers = !prerast_precise::Enabled();   // GAPS PRERASTER_PRECISE: from the dispatches below
      dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
      dirty_state_.set(DirtyState::ComputePipelineState);
    }

    if (!pso_compute_)
        return false;
    if (light_rename::Armed()) LightrnCountDispatch();   // LIGHTRN: every dispatch of the pass (a candidate has exactly one)
    // GAPS PRERASTER_PRECISE: every dispatch recorded into this pass goes through here (direct and indirect).
    static_cast<ComputeEncoderData *>(allocator_->encoder_current)->may_write_buffers |= pso_compute_->may_write_buffers;

    if (dirty_state_.test(DirtyState::ComputePipelineState)) {
      auto &cmd_setpso = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
      cmd_setpso.type = WMTComputeCommandSetPSO;
      cmd_setpso.pso = pso_compute_->pso;
      cmd_setpso.threadgroup_size = pso_compute_->threadgroup_size;
      dirty_state_.clr(DirtyState::ComputePipelineState);
    }

    if (dirty_state_.test(DirtyState::ComputeRootArguments) && !SkipResourceBinding) {
      if (rootsig_compute_) {
        auto Offset = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_.words.data());
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        cmd_argbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_argbuf.offset = Offset;
        cmd_argbuf.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        if (light_rename::Armed()) {   // LIGHTRN: the block a candidate dispatch of this pass reads
          lightrn_croot_cmd_ = &cmd_argbuf; lightrn_croot_pass_ = allocator_->encoder_current;
          lightrn_croot_words_ = reinterpret_cast<const uint64_t *>(static_cast<const uint8_t *>(allocator_->gpu_heap_) + Offset);
        }
      }
      dirty_state_.clr(DirtyState::ComputeRootArguments);
    }

    if (dirty_state_.test(DirtyState::ComputeRootSignature) && !SkipResourceBinding) {
      if (rootsig_compute_ && (!binding_elision::SkipEmptyStatic() || rootsig_compute_->NumStaticSamplers)) {
        auto Offset = EncodeStaticSamplers(rootsig_compute_.ptr());
        auto &cmd_argbuf = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
        cmd_argbuf.type = WMTComputeCommandSetBuffer;
        cmd_argbuf.buffer = allocator_->gpu_heap_buffer_;
        cmd_argbuf.offset = Offset;
        cmd_argbuf.index = SM50_BINDING_INDEX_STATIC_SAMPLERS;
      }
      dirty_state_.clr(DirtyState::ComputeRootSignature);
    }

    return true;
  }

  void STDMETHODCALLTYPE
  Dispatch(UINT X, UINT Y, UINT Z) { DXMT_LT(7);
    if (!RequireOpenRecording("Dispatch", DXMT_OPCLASS("Dispatch"))) return;
    if (failed_async_pso_) { pso_work::Skipped("dispatch"); return; }
    try {
    if (!PreDispatch()) {
      TraceRecording("Dispatch", "rejected", "PreDispatch_false_missing_compute_pso", "x=%u y=%u z=%u zero_work=%u",
                     X, Y, Z, unsigned(!X || !Y || !Z));
      return;
    }

    auto &cmd_dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    cmd_dispatch.type = WMTComputeCommandDispatch;
    cmd_dispatch.size = {X, Y, Z};
    if (light_rename::Armed() && pso_compute_->lightrn_form) LightrnRecordDispatch(X, Y, Z);   // LIGHTRN
    AuditReuseDraw(true, {2, X, Y, Z});
    TraceRecording("Dispatch", "emitted", !X || !Y || !Z ? "zero_work" : "compute_command",
                   "x=%u y=%u z=%u zero_work=%u", X, Y, Z, unsigned(!X || !Y || !Z));
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "Dispatch", __LINE__);
      TraceRecording("Dispatch", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  bool
  PreBlit() {
    ENCODERS_REASON(ReasonSwitchBlit);
    FlushDeferredResolves();
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Blit) {
      EndPass();
      auto render = allocator_->AllocatePass<BlitEncoderData>();
      render->type = EncoderType::Blit;
      render->cmd_head.type = WMTBlitCommandNop;
      render->cmd_head.next.set(0);
      render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
    }
    return true;
  }

  void STDMETHODCALLTYPE
  CopyBufferRegion(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 ByteCount
  ) { DXMT_LT(8);
    if (!RequireOpenRecording("CopyBufferRegion", DXMT_OPCLASS("CopyBufferRegion"))) return; DropDiscards();
    try {
    if (!pDstBuffer || !pSrcBuffer)
      return;
    SideBlitScope side(this, {{ResourceObject(pSrcBuffer), SrcOffset, SrcOffset + ByteCount, false},
                              {ResourceObject(pDstBuffer), DstOffset, DstOffset + ByteCount, true}});
    if (!PreBlit())
      return;

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd_cp.src = static_cast<MTLD3D12Resource *>(pSrcBuffer)->buffer->current()->buffer();
    cmd_cp.dst = static_cast<MTLD3D12Resource *>(pDstBuffer)->buffer->current()->buffer();
    cmd_cp.src_offset = SrcOffset;
    cmd_cp.dst_offset = DstOffset;
    cmd_cp.copy_length = ByteCount;
    CensusCopy("buf", pSrcBuffer, SrcOffset, pDstBuffer, DstOffset, ByteCount);   // GPU-PASS TESS_CENSUS
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "CopyBufferRegion", __LINE__);
      TraceRecording("CopyBufferRegion", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  CopyTextureRegion(
      const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc,
      const D3D12_BOX *pSrcBox
  ) { DXMT_LT(9);
    if (!RequireOpenRecording("CopyTextureRegion", DXMT_OPCLASS("CopyTextureRegion"))) return; DropDiscards();
    try {
    if (!pDst || !pSrc)
      return;
    if (tess_census::Enabled()) {   // GPU-PASS TESS_CENSUS: footprint side gives the bytes and the CPU-visible source
      const bool sf = pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, df = pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      const auto &fp = sf ? pSrc->PlacedFootprint : pDst->PlacedFootprint;
      const uint64_t bytes = (sf || df) ? uint64_t(fp.Footprint.RowPitch) * fp.Footprint.Height * std::max(1u, fp.Footprint.Depth) : 0;
      CensusCopy(sf ? (df ? "b2b" : "b2t") : (df ? "t2b" : "t2t"), pSrc->pResource, sf ? pSrc->PlacedFootprint.Offset : 0, pDst->pResource,
                 df ? pDst->PlacedFootprint.Offset : uint64_t(pDst->SubresourceIndex), sf ? bytes : (df ? bytes : 0));
    }
    SideBlitScope side(this, {{ResourceObject(pSrc->pResource), 0, ~0ull, false},
                              {ResourceObject(pDst->pResource), 0, ~0ull, true}});
    if (!PreBlit())
      return;

    auto src_desc = pSrc->pResource->GetDesc();
    auto dst_desc = pDst->pResource->GetDesc();
    uint32_t src_level = 0, src_slice = 0, src_planar = 0;
    uint32_t dst_level = 0, dst_slice = 0, dst_planar = 0;
    D3D12_BOX src_box, full_src_box;

    if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      full_src_box = {
          0, 0, 0,
          pSrc->PlacedFootprint.Footprint.Width,
          pSrc->PlacedFootprint.Footprint.Height,
          pSrc->PlacedFootprint.Footprint.Depth
      };
    } else {
      DecomposeSubresource(src_desc, pSrc->SubresourceIndex, &src_level, &src_slice, &src_planar);
      full_src_box = GetResourceExtent(src_desc, src_level);
    }
    src_box = pSrcBox ? *pSrcBox : full_src_box;

    // discard invalid & empty box
    if (src_box.left >= src_box.right || src_box.front >= src_box.back || src_box.top >= src_box.bottom)
      return;

    if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
      auto &dst = static_cast<MTLD3D12Resource *>(pDst->pResource)->texture;
      if (!dst)
        return;

      DecomposeSubresource(dst_desc, pDst->SubresourceIndex, &dst_level, &dst_slice, &dst_planar);

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), dst_desc.Format, dst_format))) {
        WARN("CopyTextureRegion: unsupported format ", dst_desc.Format);
        return;
      }
      auto dst_planar_count = dst_format.PlanarCount;

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->buffer;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pSrc->PlacedFootprint.Footprint.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", pSrc->PlacedFootprint.Footprint.Format);
          return;
        }

        auto block_width = src_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
        auto src_depth_pitch = dst->textureType() == WMTTextureType3D
                                   ? (pSrc->PlacedFootprint.Footprint.Height / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch
                                   : 0;

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
        cmd_cp.src = src->current()->buffer();
        cmd_cp.src_offset = pSrc->PlacedFootprint.Offset + (src_box.left / block_width) * src_format.BytesPerTexel +
                            (src_box.top / block_width) * pSrc->PlacedFootprint.Footprint.RowPitch + src_box.front * src_depth_pitch;
        cmd_cp.bytes_per_row = pSrc->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = src_depth_pitch;
        cmd_cp.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->texture();
        cmd_cp.level = dst_level;
        cmd_cp.slice = dst_slice;
        cmd_cp.options = (dst_planar_count > 1)
                             ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
        cmd_cp.origin = {DstX, DstY, DstZ};
      } else {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        // copy between depth-stencil texture is tricky
        if (dst_planar_count > 1 || src_planar_count > 1) {
          // in this path, one/both of dst/src would be depth-stencil texture

          if (dst_planar_count > 1 && src_planar_count > 1 && dst_planar != src_planar) {
            WARN("CopyTextureRegion: unmatched planar"); // just in case
            return;
          }

          auto texel_size = (dst_planar == 1 || src_planar == 1) ? 1 : 4;
          auto width = src_box.right - src_box.left;
          auto height = src_box.bottom - src_box.top;
          auto bytes_per_row = align(width * texel_size, 256);
          auto bytes_per_image = bytes_per_row * height;

          auto [temp_buffer, temp_buffer_offset] = allocator_->AllocateTempBuffer(bytes_per_image, 256);

          auto &cmd_to_tmp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
          cmd_to_tmp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
          cmd_to_tmp.src = src->current()->texture();
          cmd_to_tmp.level = src_level;
          cmd_to_tmp.slice = src_slice;
          cmd_to_tmp.origin = {src_box.left, src_box.top, src_box.front};
          cmd_to_tmp.size = {width, height, src_box.back - src_box.front};
          cmd_to_tmp.dst = temp_buffer;
          cmd_to_tmp.offset = temp_buffer_offset;
          cmd_to_tmp.bytes_per_image = 0; // DSV cannot be 3D
          cmd_to_tmp.bytes_per_row = bytes_per_row;
          cmd_to_tmp.options = (src_planar_count > 1) ? (src_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;

          auto &cmd_to_tex = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture_withblitoption>();
          cmd_to_tex.type = WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
          cmd_to_tex.src = temp_buffer;
          cmd_to_tex.src_offset = temp_buffer_offset;
          cmd_to_tex.bytes_per_image = 0; // DSV cannot be 3D
          cmd_to_tex.bytes_per_row = bytes_per_row;
          cmd_to_tex.dst = dst->current()->texture();
          cmd_to_tex.level = dst_level;
          cmd_to_tex.slice = dst_slice;
          cmd_to_tex.origin = {DstX, DstY, DstZ};
          cmd_to_tex.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
          cmd_to_tex.options = (dst_planar_count > 1) ? (dst_planar ? WMTBlitOptionStencilFromDepthStencil
                                                                    : WMTBlitOptionDepthFromDepthStencil)
                                                      : WMTBlitOptionNone;
          return;
        }

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
        cmd_cp.type = WMTBlitCommandCopyFromTextureToTexture;
        cmd_cp.src = src->current()->texture();
        cmd_cp.src_level = src_level;
        cmd_cp.src_slice = src_slice;
        cmd_cp.src_origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.src_size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->texture();
        cmd_cp.dst_level = dst_level;
        cmd_cp.dst_slice = dst_slice;
        cmd_cp.dst_origin = {DstX, DstY, DstZ};
      }
    } else if (pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
      auto &dst = static_cast<MTLD3D12Resource *>(pDst->pResource)->buffer;
      if (!dst)
        return;

      MTL_DXGI_FORMAT_DESC dst_format;
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), pDst->PlacedFootprint.Footprint.Format, dst_format))) {
        WARN("CopyTextureRegion: unsupported format ", pDst->PlacedFootprint.Footprint.Format);
        return;
      }

      if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        auto &src = static_cast<MTLD3D12Resource *>(pSrc->pResource)->texture;
        if (!src)
          return;

        MTL_DXGI_FORMAT_DESC src_format;
        if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), src_desc.Format, src_format))) {
          WARN("CopyTextureRegion: unsupported format ", src_desc.Format);
          return;
        }
        auto src_planar_count = src_format.PlanarCount;

        auto block_width = dst_format.Flag & MTL_DXGI_FORMAT_BC ? 4 : 1;
        auto dst_depth_pitch = src->textureType() == WMTTextureType3D
                                   ? (pDst->PlacedFootprint.Footprint.Height / block_width) * pDst->PlacedFootprint.Footprint.RowPitch
                                   : 0;

        auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer_withblitoption>();
        cmd_cp.type = WMTBlitCommandCopyFromTextureToBufferWithBlitOption;
        cmd_cp.src = src->current()->texture();
        cmd_cp.level = src_level;
        cmd_cp.slice = src_slice;
        cmd_cp.origin = {src_box.left, src_box.top, src_box.front};
        cmd_cp.size = {src_box.right - src_box.left, src_box.bottom - src_box.top, src_box.back - src_box.front};
        cmd_cp.dst = dst->current()->buffer();
        cmd_cp.offset = pDst->PlacedFootprint.Offset + (DstX / block_width) * dst_format.BytesPerTexel +
                        (DstY / block_width) * pDst->PlacedFootprint.Footprint.RowPitch + DstZ * dst_depth_pitch;
        cmd_cp.bytes_per_row = pDst->PlacedFootprint.Footprint.RowPitch;
        cmd_cp.bytes_per_image = dst_depth_pitch;
        cmd_cp.options = (src_planar_count > 1)
                             ? (src_planar ? WMTBlitOptionStencilFromDepthStencil : WMTBlitOptionDepthFromDepthStencil)
                             : WMTBlitOptionNone;
      } else {
        // so it is buffer to buffer copy?
        IMPLEMENT_ME
      }
    }
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "CopyTextureRegion", __LINE__);
      TraceRecording("CopyTextureRegion", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource) { DXMT_LT(10);
    if (!RequireOpenRecording("CopyResource", DXMT_OPCLASS("CopyResource"))) return; DropDiscards();
    try {
    auto *pDst = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *pSrc = static_cast<MTLD3D12Resource *>(pSrcResource);
    if (!pDst || !pSrc || (pDst == pSrc))
      return;

    auto DstDesc = pDst->GetDesc();
    auto SrcDesc = pSrc->GetDesc();
    if (DstDesc.Dimension != SrcDesc.Dimension)
      return;

    SideBlitScope side(this, {{ResourceObject(pSrcResource), 0, ~0ull, false},
                              {ResourceObject(pDstResource), 0, ~0ull, true}});
    CensusCopy("res", pSrcResource, 0, pDstResource, 0, DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? SrcDesc.Width : 0);   // GPU-PASS
    if (!PreBlit())
      return;

    if (DstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd_cp.type = WMTBlitCommandCopyFromBufferToBuffer;
      cmd_cp.copy_length = SrcDesc.Width;
      cmd_cp.src = pSrc->buffer->current()->buffer();
      cmd_cp.src_offset = 0;
      cmd_cp.dst = pDst->buffer->current()->buffer();
      cmd_cp.dst_offset = 0;
      return;
    }

    // TODO: handle reinterpret copy
    if (pDst->texture->pixelFormat() != pSrc->texture->pixelFormat()) {
      static std::atomic<uint32_t> trace_reinterpret{0};
      TraceFrame(trace_reinterpret, "copy.resource.skip", this, "reason=reinterpret_format src=%llx dst=%llx src_format=%u dst_format=%u",
                 (unsigned long long)pSrc->texture->current()->texture().handle,
                 (unsigned long long)pDst->texture->current()->texture().handle,
                 unsigned(pSrc->texture->pixelFormat()), unsigned(pDst->texture->pixelFormat()));
      WARN("CopyResource: TODO: reinterpret copy");
      return;
    }

    auto &cmd_cp = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_texture>();
    cmd_cp.type = WMTBlitCommandCopyTexture;
    cmd_cp.src = pSrc->texture->current()->texture();
    cmd_cp.dst = pDst->texture->current()->texture();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "CopyResource", __LINE__);
      TraceRecording("CopyResource", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE CopyTiles(
      ID3D12Resource *tiled_resource, const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *tile_region_size, ID3D12Resource *buffer, UINT64 buffer_offset,
      D3D12_TILE_COPY_FLAGS flags
  ) { DXMT_LT(11);
    if (!RequireOpenRecording("CopyTiles", DXMT_OPCLASS("CopyTiles"))) return; DropDiscards(); ENCODERS_REASON(ReasonOther);
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "CopyTiles", __LINE__);
      TraceRecording("CopyTiles", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE ResolveSubresource(
      ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource,
      DXGI_FORMAT Format
  ) { DXMT_LT(12);
    if (!RequireOpenRecording("ResolveSubresource", DXMT_OPCLASS("ResolveSubresource"))) return; DropDiscards(); ENCODERS_REASON(ReasonResolveSubresource);
    try {
    auto *pDst = static_cast<MTLD3D12Resource *>(pDstResource);
    auto *pSrc = static_cast<MTLD3D12Resource *>(pSrcResource);

    if (!pDst->texture || !pSrc->texture)
      return;

    auto DstMips = pDst->texture->miplevelCount();
    auto DstLevel = DstSubresource % DstMips;
    auto DstSlice = DstSubresource / DstMips;

    EndPass();
    auto resolve = allocator_->AllocatePass<ResolveEncoderData>();
    resolve->type = EncoderType::Resolve;

    MTL_DXGI_FORMAT_DESC format_desc;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format_desc))) {
      ERR("ResolveSubresource: invalid format ", Format);
      return;
    }
    {
      auto format = format_desc.PixelFormat;
      TextureViewDescriptor src_desc;
      auto &src = pSrc->texture;
      auto &dst = pDst->texture;
      src_desc.format = format;
      src_desc.type = src->textureType();
      src_desc.arraySize = 1;
      src_desc.firstArraySlice = SrcSubresource; // src must be a MS(Array) texture which has exactly 1 mipmap level
      src_desc.miplevelCount = 1;
      src_desc.firstMiplevel = 0;

      TextureViewDescriptor dst_desc;
      dst_desc.format = format;
      dst_desc.type = WMTTextureType2D;
      dst_desc.arraySize = 1;
      dst_desc.firstArraySlice = DstSlice;
      dst_desc.miplevelCount = 1;
      dst_desc.firstMiplevel = DstLevel;

      auto src_view = src->createView(src_desc);
      auto dst_view = dst->createView(dst_desc);

      resolve->src = src->view(src_view);
      resolve->dst = dst->view(dst_view);
    }
    EndPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ResolveSubresource", __LINE__);
      TraceRecording("ResolveSubresource", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY Topology) { DXMT_LT(13);
    if (!RequireOpenRecording("IASetPrimitiveTopology", DXMT_OPCLASS("IASetPrimitiveTopology"))) return;
    try {
    topology_ = Topology;
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "IASetPrimitiveTopology", __LINE__);
      TraceRecording("IASetPrimitiveTopology", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports) { DXMT_LT(14);
    if ((dd_mask_ & (1u << setter_dedup::VP)) && DedupSkip(setter_dedup::VP, NumViewports == num_viewports &&
        NumViewports <= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE &&
        (!NumViewports || (pViewports && !std::memcmp(viewports, pViewports, sizeof(D3D12_VIEWPORT) * NumViewports))))) return;
    if (!RequireOpenRecording("RSSetViewports", DXMT_OPCLASS("RSSetViewports"))) return;
    try {
    if (NumViewports > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    num_viewports = NumViewports;
    for (auto i = 0u; i < NumViewports; i++) {
      viewports[i] = pViewports[i];
    }
    dirty_state_.set(DirtyState::Viewport);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "RSSetViewports", __LINE__);
      TraceRecording("RSSetViewports", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  RSSetScissorRects(UINT NumRects, const D3D12_RECT *rects) { DXMT_LT(15);
    if ((dd_mask_ & (1u << setter_dedup::SC)) && DedupSkip(setter_dedup::SC, NumRects == num_scissors &&
        NumRects <= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE &&
        (!NumRects || (rects && !std::memcmp(scissors, rects, sizeof(D3D12_RECT) * NumRects))))) return;
    if (!RequireOpenRecording("RSSetScissorRects", DXMT_OPCLASS("RSSetScissorRects"))) return;
    try {
    if (NumRects > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
      return;
    num_scissors = NumRects;
    for (auto i = 0u; i < NumRects; i++) {
      scissors[i] = rects[i];
    }
    dirty_state_.set(DirtyState::ScissorRect);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "RSSetScissorRects", __LINE__);
      TraceRecording("RSSetScissorRects", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  OMSetBlendFactor(const FLOAT BlendFactors[4]) { DXMT_LT(16);
    if (!RequireOpenRecording("OMSetBlendFactor", DXMT_OPCLASS("OMSetBlendFactor"))) return;
    try {
    if (BlendFactors) {
      memcpy(blend_factor_, BlendFactors, std::size(blend_factor_) * sizeof(blend_factor_[0]));
    } else {
      blend_factor_[0] = 1.0f;
      blend_factor_[1] = 1.0f;
      blend_factor_[2] = 1.0f;
      blend_factor_[3] = 1.0f;
    }
    dirty_state_.set(DirtyState::BlendFactor);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "OMSetBlendFactor", __LINE__);
      TraceRecording("OMSetBlendFactor", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  OMSetStencilRef(UINT StencilRef) { DXMT_LT(17);
    if ((dd_mask_ & (1u << setter_dedup::SR)) && DedupSkip(setter_dedup::SR, stencil_ref_ == (UINT8)StencilRef)) return;
    if (!RequireOpenRecording("OMSetStencilRef", DXMT_OPCLASS("OMSetStencilRef"))) return;
    try {
    if (stencil_ref_ == (UINT8)StencilRef)
      return;
    stencil_ref_ = (UINT8)StencilRef;
    dirty_state_.set(DirtyState::StencilRef);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "OMSetStencilRef", __LINE__);
      TraceRecording("OMSetStencilRef", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  SetPipelineState(ID3D12PipelineState *pPSO) { DXMT_LT(18);
    if ((dd_mask_ & (1u << setter_dedup::PSO)) && DedupSkip(setter_dedup::PSO, pPSO && pPSO == dd_pso_arg_ && dd_pso_resolved_ &&
        !failed_async_pso_ && (dd_pso_resolved_ == static_cast<MTLD3D12PipelineState *>(pso_graphics_.ptr()) ||
                               dd_pso_resolved_ == static_cast<MTLD3D12PipelineState *>(pso_compute_.ptr())))) return;
    if (!RequireOpenRecording("SetPipelineState", DXMT_OPCLASS("SetPipelineState"))) return;
    try {
    if (!pPSO) {
      if (dd_mode_) DedupForgetPSO();
      failed_async_pso_ = false;
      pso_graphics_ = nullptr;
      pso_compute_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      TraceRecording("SetPipelineState", "accepted", "cleared", "requested_pso=%p", static_cast<const void *>(pPSO));
      return;
    }

    auto pso = pso_work::Resolve(static_cast<MTLD3D12PipelineState *>(pPSO));
    failed_async_pso_ = !pso;
    if (dd_mode_) {   // GAPS 02.11 SETTER_DEDUP: every path below ends with `pso` bound (or nothing bound when it failed)
      if (!pso) DedupForgetPSO();
      else {
        dd_pso_arg_ = pPSO; dd_pso_resolved_ = pso;
        ID3D12PipelineState *hold = pso != static_cast<MTLD3D12PipelineState *>(pPSO) ? pPSO : nullptr;
        if (dd_pso_hold_.ptr() != hold) dd_pso_hold_ = hold;
      }
    }
    if (!pso) {
      pso_graphics_ = nullptr; pso_compute_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      TraceRecording("SetPipelineState", "rejected", "async_backend_failure", "requested_pso=%p", static_cast<const void *>(pPSO));
      return;
    }
    if (pso->IsComputePipelineState) {
      auto compute_pso = static_cast<MTLD3D12ComputePipelineState *>(pso);
      if (pso_compute_.ptr() == compute_pso) {
        TraceRecording("SetPipelineState", "accepted", "unchanged_compute", "requested_pso=%p", static_cast<const void *>(pPSO));
        return;
      }
      pso_compute_ = compute_pso;
      pso_graphics_ = nullptr;
      dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState);
      TraceRecording("SetPipelineState", "accepted", "bound_compute", "requested_pso=%p", static_cast<const void *>(pPSO));
      return;
    }

    auto graphics_pso = static_cast<MTLD3D12GraphicsPipelineState *>(pso);
    if (pso_graphics_.ptr() == graphics_pso) {
      TraceRecording("SetPipelineState", "accepted", "unchanged_graphics", "requested_pso=%p", static_cast<const void *>(pPSO));
      return;
    }
    // Ordinary graphics PSOs share the root-buffer ABI. Keep pending dirty bits:
    // root setters, new encoders and indirect work still force their normal bind.
    // Mesh and compute transitions remain on the conservative path.
    const bool reuse_root = binding_elision::ReuseRootOnPSO() && pso_graphics_ &&
        !pso_graphics_->tess_control_points && !pso_graphics_->geometry_pipeline &&
        !graphics_pso->tess_control_points && !graphics_pso->geometry_pipeline;
    pso_graphics_ = graphics_pso;
    pso_compute_ = nullptr;
    dirty_state_.set(DirtyState::GraphicsPipelineState, DirtyState::ComputePipelineState, DirtyState::VertexBuffer);
    if (!reuse_root)
      dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
    TraceRecording("SetPipelineState", "accepted", "bound_graphics", "requested_pso=%p", static_cast<const void *>(pPSO));
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetPipelineState", __LINE__);
      TraceRecording("SetPipelineState", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE ResourceBarrier(UINT Count, const D3D12_RESOURCE_BARRIER *barriers) { DXMT_LT(19);
    if (!RequireOpenRecording("ResourceBarrier", DXMT_OPCLASS("ResourceBarrier"))) return; DropDiscards(); ENCODERS_REASON(ReasonBarrier);
    try {
    if (!Count || !barriers || encoder_count != std::numeric_limits<size_t>::max())
      return;
    if (tess_census::Enabled())   // GPU-PASS TESS_CENSUS: a copy destination leaving COPY_DEST is about to be read
      for (UINT i = 0; i < Count; ++i)
        if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
          tess_census::Transition(barriers[i].Transition.pResource, uint32_t(barriers[i].Transition.StateBefore),
                                  uint32_t(barriers[i].Transition.StateAfter));
    // ENCODERS (DEFER_COPIES / BLIT_CHAIN): a barrier among copy states concerns only floating copy work; the open pass
    // goes on. The OUT join it carries is kept by the flush.
    if (CanDeferBlit() && CopyStateBarriers(Count, barriers)) {
      ++absorbed_barriers_;
      for (UINT i = 0; i < Count; ++i)   // GAPS COPY_GATHER: a COMMON 'before' may hide an implicit read promotion earlier
        if (barriers[i].Transition.StateBefore == D3D12_RESOURCE_STATE_COMMON) absorbed_common_ = true;
      encoder_stats::Note(encoder_stats::Get().absorbed_barriers);
      return;
    }
    // ENCODERS (COMPUTE_CHAIN): UAV and shader-state barriers between dispatches become a memory barrier inside the open
    // compute encoder (as the D3D11 path of DXMT does for its implicit UAV barriers).
    if (encoder_stats::ComputeChain() && deferred_.empty() && !absorbed_barriers_ && allocator_->encoder_current &&
        allocator_->encoder_current->type == EncoderType::Compute && ComputeChainBarriers(Count, barriers)) {
      auto &cmd = allocator_->EncodeComputeCommand<wmtcmd_compute_memory_barrier>();
      cmd.type = WMTComputeCommandMemoryBarrier;
      cmd.scope = WMTBarrierScope(WMTBarrierScopeBuffers | WMTBarrierScopeTextures);
      encoder_stats::Note(encoder_stats::Get().compute_chain_barriers);
      return;
    }
    // Join deferred OUT writes only at an explicit dependency, not every pass.
    // Record this even without a local OUT: it may be pending from a prior list.
    EndPass();
    PreBlit();
    auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
    pass->join_immediate_out = true;
    pass->barrier_only = true;
    RecordMergeBarriers(pass, Count, barriers);
    if (encoder_stats::BarrierRecords() || c17::MergeDependencies() || c17::Enabled()) {
      // ENCODERS (MERGE_SAFE / DEFER_CLEARS / stats): which objects this barrier is about, read at execution.
      pass->barrier_records_valid = true;
      for (UINT i = 0; i < Count; ++i) {
        const auto &b = barriers[i];
        BarrierRecord record{};
        record.type = uint8_t(b.Type);
        record.flags = uint8_t(b.Flags);
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
          record.object = ResourceObject(b.Transition.pResource);
          record.subresource = b.Transition.Subresource;
          record.before = uint32_t(b.Transition.StateBefore);
          record.after = uint32_t(b.Transition.StateAfter);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
          record.object = ResourceObject(b.UAV.pResource);
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
          // c17: both sides; the ENCODERS readers treat every aliasing record as touching anyway
          record.object = ResourceObject(b.Aliasing.pResourceAfter);
          record.object2 = ResourceObject(b.Aliasing.pResourceBefore);
        }
        pass->barrier_records.push_back(record);
      }
    }
    EndPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ResourceBarrier", __LINE__);
      TraceRecording("ResourceBarrier", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *CommandList) { DXMT_LT(20);
    if (!RequireOpenRecording("ExecuteBundle", DXMT_OPCLASS("ExecuteBundle"))) return;
    if (dd_mode_) { dd_seen_ = 0; DedupForgetPSO(); }   // GAPS 02.11 SETTER_DEDUP: a bundle may set state (the PSO shadow is the only non-state field)
    try { IMPLEMENT_ME   } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ExecuteBundle", __LINE__);
      TraceRecording("ExecuteBundle", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };


  HRESULT RecordBoundDescriptor(bool compute, const ray_binding::Binding &binding, const ray_library::Resource &required,
                                D3D12RecordedDescriptorBinding &out) override {
    out = {};
    const auto *root = compute ? rootsig_compute_.ptr() : rootsig_graphics_.ptr();
    const auto &arguments = compute ? rootarg_compute_staging_ : rootarg_graphics_staging_;
    if (!root || root->RayBindingStatus != ray_binding::Status::Ready || binding.local)
      return DXGI_ERROR_UNSUPPORTED;
    const ray_binding::Span *span = nullptr;
    uint64_t selected_offset = 0;
    auto status = ray_binding::SelectResource(root->RayBindings, binding, required, 0, span, selected_offset);
    if (binding.source != ray_binding::Source::Table) return E_INVALIDARG;
    if (status != ray_binding::Status::Ready || binding.descriptor_offset > UINT32_MAX) return E_INVALIDARG;
    D3D12RecordedDescriptorBinding next;
    next.recorded_device = device_;
    status = ray_binding::DecodePolicy(span->type, span->flags, true, next.policy);
    if (status != ray_binding::Status::Ready)
      return status == ray_binding::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    const UINT type = span->type == 3 ? 1 : 0;
    root_argument::TableLocation location;
    if (!root_argument::ReadTable(root->ArgumentLayout, arguments, binding.parameter,
                                  bound_heap_ranges_[type], UINT(binding.descriptor_offset), 1, location))
      return E_INVALIDARG;
    D3D12DescriptorProvider provider{bound_heaps_[type], location.index, span->type, required};
    const HRESULT hr = CaptureResult(next.replay.Record(std::move(provider), next.policy.descriptors_volatile));
    if (FAILED(hr)) return hr;
    out = std::move(next);
    return S_OK;
  }

  HRESULT RecordRayArgument(const ray_binding::Binding &binding, const ray_library::Resource &required, UINT element,
      MTLD3D12RootSignature *local, const void *arguments, size_t argument_bytes,
      uint64_t buffer_bytes, D3D12RecordedDescriptorBinding &out) override {
    out = {};
    const auto *root = binding.local ? local : rootsig_compute_.ptr();
    if (!root || root->IsLocal != binding.local || root->RayBindingStatus != ray_binding::Status::Ready)
      return DXGI_ERROR_UNSUPPORTED;
    if (binding.local) {
      Com<ID3D12Device> owner;
      if (!root->LocalRootLayout || FAILED(local->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) return E_INVALIDARG;
    }
    const ray_binding::Span *span = nullptr;
    uint64_t descriptor_offset = 0;
    const auto status = ray_binding::SelectResource(root->RayBindings, binding, required, element, span, descriptor_offset);
    if (status != ray_binding::Status::Ready)
      return status == ray_binding::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    D3D12RecordedDescriptorBinding next;
    next.recorded_device = device_;
    const bool table = binding.source == ray_binding::Source::Table;
    const auto policy = ray_binding::DecodePolicy(span->type, span->flags, table, next.policy);
    if (policy != ray_binding::Status::Ready)
      return policy == ray_binding::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    const auto kind = table ? root_argument::Table :
        span->type == 2 ? root_argument::CBV :
        span->type == 0 ? root_argument::SRV : root_argument::UAV;
    uint64_t value = 0;
    if (binding.local) {
      const auto &layout = *root->LocalRootLayout;
      // The caller supplies the captured local-argument region, excluding the
      // shader identifier. It must not point into a mutable shader-table upload.
      if (binding.parameter >= layout.parameters.size() || layout.parameters[binding.parameter].kind != kind ||
          !local_root::Read(layout, binding.parameter, arguments, argument_bytes, 0, value)) return E_INVALIDARG;
    } else if (!rootarg_compute_staging_.ReadValue(root->ArgumentLayout, binding.parameter, kind, value)) return E_INVALIDARG;
    if (table) {
      const UINT heap_type = span->type == 3 ? 1 : 0;
      root_argument::TableLocation location;
      if (descriptor_offset > UINT32_MAX ||
          !root_argument::Locate(bound_heap_ranges_[heap_type], value, UINT(descriptor_offset), 1, location)) return E_INVALIDARG;
      D3D12DescriptorProvider provider{bound_heaps_[heap_type], location.index, span->type, required};
      const HRESULT hr = CaptureResult(next.replay.Record(std::move(provider), next.policy.descriptors_volatile));
      if (FAILED(hr)) return hr;
    } else {
      const HRESULT valid = CaptureResult(resource_shape::Root(required, value, buffer_bytes));
      if (FAILED(valid)) return valid;
      uint64_t offset = 0;
      auto owner = device_->LookupCaptureByVA(value, buffer_bytes, offset);
      if (!owner) return DXGI_ERROR_UNSUPPORTED;
      next.root_buffer = true;
      auto &capture = next.fixed_buffer;
      capture.type = span->type == 2 ? ShaderVisibleDescriptorType::ConstantBuffer :
          span->type == 0 ? ShaderVisibleDescriptorType::SRVBuffer : ShaderVisibleDescriptorType::UAVBuffer;
      // Root descriptors have no view stride or bounds. The byte requirement
      // comes from executable shader ABI analysis, never from base RDAT alone.
      capture.shape = {required.kind, 0, 0, 0};
      capture.words = {value, buffer_bytes, 0, 0};
      capture.resource = std::move(owner);
      capture.buffer_offset = offset;
      capture.byte_length = buffer_bytes;
    }
    out = std::move(next);
    return S_OK;
  }

  HRESULT RecordCompilerRayArgument(const ray_binding::Binding &binding, const compiler_resource::Footprint &footprint,
      UINT element, MTLD3D12RootSignature *local, const void *arguments, size_t argument_bytes,
      uint64_t buffer_bytes, D3D12RecordedDescriptorBinding &out) override {
    out = {};
    if (encoder_count != std::numeric_limits<size_t>::max()) return E_INVALIDARG;
    const HRESULT valid = CaptureResult(compiler_resource::Validate(footprint));
    if (FAILED(valid)) return valid;
    // Declared DXIL size is a lower bound, not proof of the lowered backend's
    // physical loads. Direct roots still require a caller-proven byte range.
    try {
      D3D12RecordedDescriptorBinding next;
      const HRESULT hr = RecordRayArgument(binding, footprint.resource, element, local,
          arguments, argument_bytes, buffer_bytes, next);
      if (FAILED(hr)) return hr;
      next.compiler_footprint = footprint;
      // Static arguments can be checked now. Volatile descriptors may not yet
      // be initialized; check their exact compiler requirements at submission.
      if (next.root_buffer || !next.policy.descriptors_volatile) {
        D3D12NativeDescriptorCapture checked;
        const HRESULT match = next.ResolveForSubmission(checked);
        if (FAILED(match)) return match;
      }
      out = std::move(next);
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

  // Internal transport for a compiler-supplied ABI and current compute PSO.
  // It does not admit a StateObject or implement public DispatchRays.
  HRESULT RecordArgumentDispatch(const argument_upload::Plan &arguments,
      const std::vector<D3D12RecordedDescriptorBinding> &bindings, UINT x, UINT y, UINT z) override {
    if (!allocator_ || encoder_count != std::numeric_limits<size_t>::max() ||
        !pso_compute_ || !pso_compute_->pso) return E_INVALIDARG;
    if (x > 65535 || y > 65535 || z > 65535) return E_INVALIDARG;
    if (allocator_->type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && allocator_->type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return DXGI_ERROR_UNSUPPORTED;
    if (!x || !y || !z) return S_OK;
    try {
      const HRESULT valid = CaptureResult(argument_upload::Validate(arguments, bindings.size()));
      if (FAILED(valid)) return valid;
      for (const auto &binding : bindings)
        if (binding.recorded_device != static_cast<ID3D12Device *>(device_)) return E_INVALIDARG;
      auto record = std::make_shared<D3D12RecordedComputeDispatch>();
      record->device = device_;
      record->arguments = arguments;
      record->bindings = bindings;
      record->pipeline = pso_compute_->pso;
      record->threads = pso_compute_->threadgroup_size;
      record->groups = {x, y, z};
      ENCODERS_REASON(ReasonArgument);
      EndPass();
      auto pass = allocator_->AllocatePass<ArgumentComputeEncoderData>();
      pass->type = EncoderType::ArgumentCompute;
      pass->dispatch = std::move(record);
      EndPass();
      dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature, DirtyState::ComputePipelineState);
      return S_OK;
    } catch (const std::bad_alloc &) {
      SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);
      return E_OUTOFMEMORY;
    }
  }

  // Function identity is retained from the same PSO, not accepted from an unrelated caller.
  HRESULT RecordMetalArgumentDispatch(const metal_argument::Plan &,
      const std::vector<D3D12RecordedDescriptorBinding> &, UINT, UINT, UINT) override {
    // A current ordinary PSO has no complete entry contract; use a reflected compiled program.
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT RecordAccelerationStructureBuild(const D3D12AccelerationBuildInput &input,
      D3D12RecordedDescriptorBinding &out) override {
    out={};
    if (!allocator_ || encoder_count!=std::numeric_limits<size_t>::max()) return E_INVALIDARG;
    if (allocator_->type_!=D3D12_COMMAND_LIST_TYPE_DIRECT && allocator_->type_!=D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return DXGI_ERROR_UNSUPPORTED;
    try {
      std::shared_ptr<const D3D12AccelerationBuildPlan> plan;
      const HRESULT hr=PrepareAccelerationBuildPlan(device_,input,plan);
      if (FAILED(hr)) return hr;
      ENCODERS_REASON(ReasonArgument);
      EndPass();
      auto pass=allocator_->AllocatePass<AccelerationBuildEncoderData>();
      pass->type=EncoderType::AccelerationBuild;pass->build=plan;
      EndPass();
      out.recorded_device=static_cast<ID3D12Device *>(device_);
      out.acceleration_device=static_cast<ID3D12Device *>(device_);
      out.acceleration_producer=std::move(plan);
      return S_OK;
    } catch (const std::bad_alloc &) { SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);return E_OUTOFMEMORY; }
  }

  HRESULT RecordCompiledComputeDispatch(const std::shared_ptr<const D3D12CompiledComputeProgram> &program,
      const metal_argument::Plan &arguments,
      const std::vector<D3D12RecordedDescriptorBinding> &bindings, UINT x, UINT y, UINT z) override {
    if (!allocator_ || encoder_count != std::numeric_limits<size_t>::max() ||
        !program || program->Owner() != static_cast<ID3D12Device *>(device_) ||
        !program->Pipeline() || !program->Function()) return E_INVALIDARG;
    if (x > 65535 || y > 65535 || z > 65535) return E_INVALIDARG;
    if (allocator_->type_ != D3D12_COMMAND_LIST_TYPE_DIRECT && allocator_->type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE)
      return DXGI_ERROR_UNSUPPORTED;
    if (!x || !y || !z) return S_OK;
    try {
      const HRESULT valid = CaptureResult(metal_argument::Match(arguments, bindings.size(), program->Layout()));
      if (FAILED(valid)) return valid;
      for (const auto &binding : bindings)
        if (binding.recorded_device != static_cast<ID3D12Device *>(device_)) return E_INVALIDARG;
      auto record = std::make_shared<D3D12RecordedComputeDispatch>();
      record->device = device_;
      record->metal_arguments = arguments;
      record->function = program->Function();
      record->entry_layout = program->Layout();
      record->bindings = bindings;
      record->pipeline = program->Pipeline();
      record->threads = program->Threads();
      record->groups = {x, y, z};
      ENCODERS_REASON(ReasonArgument);
      EndPass();
      auto pass = allocator_->AllocatePass<ArgumentComputeEncoderData>();
      pass->type = EncoderType::ArgumentCompute;
      pass->dispatch = std::move(record);
      EndPass();
      dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature, DirtyState::ComputePipelineState);
      return S_OK;
    } catch (const std::bad_alloc &) {
      SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);
      return E_OUTOFMEMORY;
    }
  }

  HRESULT CaptureBoundDescriptor(bool compute, UINT parameter, UINT offset, D3D12DescriptorCapture &out) override {
    out = {};
    const auto *root = compute ? rootsig_compute_.ptr() : rootsig_graphics_.ptr();
    const auto &arguments = compute ? rootarg_compute_staging_ : rootarg_graphics_staging_;
    if (!root || parameter >= root->ArgumentLayout.count) return E_INVALIDARG;
    const auto &p = root->ArgumentLayout.parameters[parameter];
    if (p.kind != root_argument::Table || p.heap_type > 1 || !bound_heaps_[p.heap_type]) return E_INVALIDARG;
    root_argument::TableLocation location;
    if (!root_argument::ReadTable(root->ArgumentLayout, arguments, parameter,
                                  bound_heap_ranges_[p.heap_type], offset, 1, location)) return E_INVALIDARG;
    if (p.heap_type == 0) {
      Com<MTLD3D12DescriptorHeap> heap;
      if (FAILED(bound_heaps_[0]->QueryInterface(kD3D12ResourceHeapCaptureUUID, reinterpret_cast<void **>(&heap))))
        return DXGI_ERROR_UNSUPPORTED;
      return heap->CaptureDescriptor(location.index, out);
    }
    Com<MTLD3D12SamplerDescriptorHeap> heap;
    if (FAILED(bound_heaps_[1]->QueryInterface(kD3D12SamplerHeapCaptureUUID, reinterpret_cast<void **>(&heap))))
      return DXGI_ERROR_UNSUPPORTED;
    return heap->CaptureDescriptor(location.index, out);
  }

  HRESULT CaptureRootBuffer(bool compute, UINT parameter, uint64_t bytes, D3D12DescriptorCapture &out) override {
    out = {};
    const auto *root = compute ? rootsig_compute_.ptr() : rootsig_graphics_.ptr();
    const auto &arguments = compute ? rootarg_compute_staging_ : rootarg_graphics_staging_;
    if (!root || parameter >= root->ArgumentLayout.count || !bytes) return E_INVALIDARG;
    const auto kind = root->ArgumentLayout.parameters[parameter].kind;
    if (kind != root_argument::CBV && kind != root_argument::SRV && kind != root_argument::UAV)
      return DXGI_ERROR_UNSUPPORTED;
    uint64_t address = 0, offset = 0;
    if (!arguments.ReadValue(root->ArgumentLayout, parameter, kind, address)) return E_INVALIDARG;
    if (kind == root_argument::CBV && ((address & 255) || bytes > 65536)) return E_INVALIDARG;
    auto resource = device_->LookupCaptureByVA(address, bytes, offset);
    if (!resource) return DXGI_ERROR_UNSUPPORTED;
    D3D12DescriptorCapture next;
    next.type = kind == root_argument::CBV ? ShaderVisibleDescriptorType::ConstantBuffer :
                kind == root_argument::SRV ? ShaderVisibleDescriptorType::SRVBuffer : ShaderVisibleDescriptorType::UAVBuffer;
    next.resource = std::move(resource);
    next.buffer_offset = offset;
    next.byte_length = bytes;
    next.words = {address, bytes, 0, 0};
    out = std::move(next);
    return S_OK;
  }

  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT HeapCount, ID3D12DescriptorHeap *const *Heaps) { DXMT_LT(21);
    if (!RequireOpenRecording("SetDescriptorHeaps", DXMT_OPCLASS("SetDescriptorHeaps"))) return;
    try {
    if (HeapCount > 2 || (HeapCount && !Heaps)) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
    std::array<Com<ID3D12DescriptorHeap>, 2> next;
    std::array<root_argument::HeapRange, 2> ranges{};
    for (UINT i = 0; i < HeapCount; ++i) {
      if (!Heaps[i]) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
      const auto desc = Heaps[i]->GetDesc();
      if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
      }
      const UINT type = UINT(desc.Type);
      Com<ID3D12Device> owner;
      if (next[type] || !(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ||
          FAILED(Heaps[i]->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
      ranges[type] = {Heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr, desc.NumDescriptors,
                      device_->GetDescriptorHandleIncrementSize(desc.Type), type};
      if (!ranges[type].Valid()) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
      next[type] = Heaps[i];
    }
    if (next[0].ptr() == bound_heaps_[0].ptr() && next[1].ptr() == bound_heaps_[1].ptr()) return;
    bound_heaps_ = std::move(next);
    bound_heap_ranges_ = ranges;
    if (rootsig_compute_) rootarg_compute_staging_.InvalidateTables(rootsig_compute_->ArgumentLayout);
    if (rootsig_graphics_) rootarg_graphics_staging_.InvalidateTables(rootsig_graphics_->ArgumentLayout);
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetDescriptorHeaps", __LINE__);
      TraceRecording("SetDescriptorHeaps", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *root) { DXMT_LT(22);
    if (!RequireOpenRecording("SetComputeRootSignature", DXMT_OPCLASS("SetComputeRootSignature"))) return;
    try {
    if (rootsig_compute_.ptr() == root) return;
    Com<MTLD3D12RootSignature> impl;
    if (root) {
      Com<ID3D12Device> owner;
      if (FAILED(root->QueryInterface(kD3D12RootSignatureImplementationUUID, reinterpret_cast<void **>(&impl))) ||
          !impl || impl->IsLocal || impl->UploadQwords > 64 ||
          FAILED(impl->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
    }
    rootsig_compute_ = impl.ptr();
    rootarg_compute_staging_ = {};
    diagnostic::Add(diagnostic::Counter::RootStateZeroCalls);
    diagnostic::Add(diagnostic::Counter::RootStateZeroBytes, sizeof(rootarg_compute_staging_));
    dirty_state_.set(DirtyState::ComputeRootArguments, DirtyState::ComputeRootSignature);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootSignature", __LINE__);
      TraceRecording("SetComputeRootSignature", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) { DXMT_LT(23);
    if (!RequireOpenRecording("SetComputeRootDescriptorTable", DXMT_OPCLASS("SetComputeRootDescriptorTable"))) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteValue(rootsig_compute_->ArgumentLayout, Index, root_argument::Table, BaseDescriptor.ptr)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootDescriptorTable", __LINE__);
      TraceRecording("SetComputeRootDescriptorTable", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) { DXMT_LT(24);
    if (!RequireOpenRecording("SetComputeRootConstantBufferView", DXMT_OPCLASS("SetComputeRootConstantBufferView"))) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteValue(rootsig_compute_->ArgumentLayout, Index, root_argument::CBV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootConstantBufferView", __LINE__);
      TraceRecording("SetComputeRootConstantBufferView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) { DXMT_LT(25);
    if (!RequireOpenRecording("SetComputeRootShaderResourceView", DXMT_OPCLASS("SetComputeRootShaderResourceView"))) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteValue(rootsig_compute_->ArgumentLayout, Index, root_argument::SRV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootShaderResourceView", __LINE__);
      TraceRecording("SetComputeRootShaderResourceView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) { DXMT_LT(26);
    if (!RequireOpenRecording("SetComputeRootUnorderedAccessView", DXMT_OPCLASS("SetComputeRootUnorderedAccessView"))) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteValue(rootsig_compute_->ArgumentLayout, Index, root_argument::UAV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRootUnorderedAccessView", __LINE__);
      TraceRecording("SetComputeRootUnorderedAccessView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) { DXMT_LT(27);
    if (!RequireOpenRecording("SetComputeRoot32BitConstant", DXMT_OPCLASS("SetComputeRoot32BitConstant"))) return;
    try {
    SetComputeRoot32BitConstants(Index, 1, &Data, DstOffset);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRoot32BitConstant", __LINE__);
      TraceRecording("SetComputeRoot32BitConstant", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT Index, UINT Count, const void *Data, UINT DstOffset) { DXMT_LT(28);
    if (!RequireOpenRecording("SetComputeRoot32BitConstants", DXMT_OPCLASS("SetComputeRoot32BitConstants"))) return;
    try {
    if (!rootsig_compute_ ||
        !rootarg_compute_staging_.WriteConstants(rootsig_compute_->ArgumentLayout, Index, Count, Data, DstOffset)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    if (Count) dirty_state_.set(DirtyState::ComputeRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetComputeRoot32BitConstants", __LINE__);
      TraceRecording("SetComputeRoot32BitConstants", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }
  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *root) { DXMT_LT(29);
    if (!RequireOpenRecording("SetGraphicsRootSignature", DXMT_OPCLASS("SetGraphicsRootSignature"))) return;
    try {
    if (rootsig_graphics_.ptr() == root) return;
    Com<MTLD3D12RootSignature> impl;
    if (root) {
      Com<ID3D12Device> owner;
      if (FAILED(root->QueryInterface(kD3D12RootSignatureImplementationUUID, reinterpret_cast<void **>(&impl))) ||
          !impl || impl->IsLocal || impl->UploadQwords > 64 ||
          FAILED(impl->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) { SetRecordingError(E_INVALIDARG, __func__, __LINE__); return; }
    }
    rootsig_graphics_ = impl.ptr();
    rootarg_graphics_staging_ = {};
    diagnostic::Add(diagnostic::Counter::RootStateZeroCalls);
    diagnostic::Add(diagnostic::Counter::RootStateZeroBytes, sizeof(rootarg_graphics_staging_));
    dirty_state_.set(DirtyState::GraphicsRootArguments, DirtyState::GraphicsRootSignature);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootSignature", __LINE__);
      TraceRecording("SetGraphicsRootSignature", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT Index, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) { DXMT_LT(30);
    if (!RequireOpenRecording("SetGraphicsRootDescriptorTable", DXMT_OPCLASS("SetGraphicsRootDescriptorTable"))) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteValue(rootsig_graphics_->ArgumentLayout, Index, root_argument::Table, BaseDescriptor.ptr)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootDescriptorTable", __LINE__);
      TraceRecording("SetGraphicsRootDescriptorTable", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) { DXMT_LT(31);
    if (!RequireOpenRecording("SetGraphicsRootConstantBufferView", DXMT_OPCLASS("SetGraphicsRootConstantBufferView"))) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteValue(rootsig_graphics_->ArgumentLayout, Index, root_argument::CBV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootConstantBufferView", __LINE__);
      TraceRecording("SetGraphicsRootConstantBufferView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) { DXMT_LT(32);
    if (!RequireOpenRecording("SetGraphicsRootShaderResourceView", DXMT_OPCLASS("SetGraphicsRootShaderResourceView"))) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteValue(rootsig_graphics_->ArgumentLayout, Index, root_argument::SRV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootShaderResourceView", __LINE__);
      TraceRecording("SetGraphicsRootShaderResourceView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT Index, D3D12_GPU_VIRTUAL_ADDRESS VA) { DXMT_LT(33);
    if (!RequireOpenRecording("SetGraphicsRootUnorderedAccessView", DXMT_OPCLASS("SetGraphicsRootUnorderedAccessView"))) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteValue(rootsig_graphics_->ArgumentLayout, Index, root_argument::UAV, VA)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRootUnorderedAccessView", __LINE__);
      TraceRecording("SetGraphicsRootUnorderedAccessView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT Index, UINT Data, UINT DstOffset) { DXMT_LT(34);
    if (!RequireOpenRecording("SetGraphicsRoot32BitConstant", DXMT_OPCLASS("SetGraphicsRoot32BitConstant"))) return;
    try {
    SetGraphicsRoot32BitConstants(Index, 1, &Data, DstOffset);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRoot32BitConstant", __LINE__);
      TraceRecording("SetGraphicsRoot32BitConstant", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT Index, UINT Count, const void *Data, UINT DstOffset) { DXMT_LT(35);
    if (!RequireOpenRecording("SetGraphicsRoot32BitConstants", DXMT_OPCLASS("SetGraphicsRoot32BitConstants"))) return;
    try {
    if (!rootsig_graphics_ ||
        !rootarg_graphics_staging_.WriteConstants(rootsig_graphics_->ArgumentLayout, Index, Count, Data, DstOffset)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__); return;
    }
    if (Count) dirty_state_.set(DirtyState::GraphicsRootArguments);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetGraphicsRoot32BitConstants", __LINE__);
      TraceRecording("SetGraphicsRoot32BitConstants", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView) { DXMT_LT(36);
    if ((dd_mask_ & (1u << setter_dedup::IB)) && DedupSkip(setter_dedup::IB, pView && index_allocation_ &&
        index_buffer_address == pView->BufferLocation && index_buffer_size == pView->SizeInBytes &&
        (pView->Format == DXGI_FORMAT_R32_UINT ? index_type == WMTIndexTypeUInt32 :
         pView->Format == DXGI_FORMAT_R16_UINT && index_type == WMTIndexTypeUInt16) &&
        dd_ib_gen_ == device_->va_generation.load(std::memory_order_acquire))) return;
    if (!RequireOpenRecording("IASetIndexBuffer", DXMT_OPCLASS("IASetIndexBuffer"))) return;
    try {
    index_buffer_address = 0;
    index_buffer = {};
    index_allocation_ = nullptr;
    index_type = {};
    index_offset = 0;
    index_buffer_size = 0;
    if (!pView || (pView->Format != DXGI_FORMAT_R16_UINT && pView->Format != DXGI_FORMAT_R32_UINT))
      return;
    const uint32_t index_size = pView->Format == DXGI_FORMAT_R32_UINT ? 4 : 2;
    if (pView->BufferLocation % index_size)
      return;
    const uint64_t dd_gen = dd_mode_ ? device_->va_generation.load(std::memory_order_acquire) : 0;   // GAPS 02.11: before the lookup
    auto index_buffer_allocation = va_cache_on_ ? VACacheLookup(pView->BufferLocation, pView->SizeInBytes, index_offset)
                                                : device_->LookupBufferByVA(pView->BufferLocation, pView->SizeInBytes, &index_offset);
    dd_ib_gen_ = dd_gen;
    if (index_buffer_allocation) {
      index_buffer_address = pView->BufferLocation;
      index_buffer = index_buffer_allocation->buffer();
      index_allocation_ = std::move(index_buffer_allocation);
      index_type = pView->Format == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
      index_buffer_size = pView->SizeInBytes;
    } else {
      index_buffer_address = 0;
      index_buffer = {};
      index_type = {};
      index_offset = {};
    }
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "IASetIndexBuffer", __LINE__);
      TraceRecording("IASetIndexBuffer", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  IASetVertexBuffers(UINT StartSlot, UINT Count, const D3D12_VERTEX_BUFFER_VIEW *Views) { DXMT_LT(37);
    if ((dd_mask_ & (1u << setter_dedup::VB)) && DedupSkip(setter_dedup::VB, Views && Count && StartSlot < vertex_buffers_.size() &&
        Count <= vertex_buffers_.size() - StartSlot &&
        !std::memcmp(&vertex_buffers_[StartSlot], Views, sizeof(D3D12_VERTEX_BUFFER_VIEW) * Count))) return;
    if (!RequireOpenRecording("IASetVertexBuffers", DXMT_OPCLASS("IASetVertexBuffers"))) return;
    try {
    if (!Views)
      return;
    
    for (unsigned Slot = StartSlot; Slot < StartSlot + Count; Slot++) {
      vertex_buffers_[Slot] = Views[Slot - StartSlot];
    }
    dirty_state_.set(DirtyState::VertexBuffer);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "IASetVertexBuffers", __LINE__);
      TraceRecording("IASetVertexBuffers", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT Count, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *Views) { DXMT_LT(38);
    if (!RequireOpenRecording("SOSetTargets", DXMT_OPCLASS("SOSetTargets"))) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SOSetTargets", __LINE__);
      TraceRecording("SOSetTargets", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  OMSetRenderTargets(
      UINT NumRTV, const D3D12_CPU_DESCRIPTOR_HANDLE *RTVs, WINBOOL SingleDescriptor,
      const D3D12_CPU_DESCRIPTOR_HANDLE *DSV
  ) { DXMT_LT(39);
    if (!RequireOpenRecording("OMSetRenderTargets", DXMT_OPCLASS("OMSetRenderTargets"))) return; ENCODERS_REASON(ReasonRenderTargets);
    try {
    EndPass();

    num_rtvs = NumRTV;
    for (unsigned i = 0; i < NumRTV; i++) {
      auto RTV = SingleDescriptor ? D3D12_CPU_DESCRIPTOR_HANDLE{RTVs[0].ptr + i * 32 /* kRTVDSVHeapIncrementalSize */}
                                  : RTVs[i];
      rtvs[i] = RTV;
    }
    dsv = DSV ? *DSV : D3D12_CPU_DESCRIPTOR_HANDLE();
    if (recording_trace_enabled_)
      memset(recording_rtvs_, 0, sizeof(recording_rtvs_));
    TraceRecording("OMSetRenderTargets", "accepted", "bound_descriptors", "single_descriptor=%u dsv_descriptor=%llx",
                   unsigned(SingleDescriptor != 0), static_cast<unsigned long long>(dsv.ptr));
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "OMSetRenderTargets", __LINE__);
      TraceRecording("OMSetRenderTargets", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ClearDepthStencilView(
      D3D12_CPU_DESCRIPTOR_HANDLE DSV, D3D12_CLEAR_FLAGS Flags, FLOAT Depth, UINT8 Stencil, UINT RectCount,
      const D3D12_RECT *Rects
  ) { DXMT_LT(40);
    if (!RequireOpenRecording("ClearDepthStencilView", DXMT_OPCLASS("ClearDepthStencilView"))) return; DropDiscards(); ENCODERS_REASON(ReasonClear);
    try {
    auto [Heap, Index] = GetRenderTargetHeap(device_, DSV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    auto CheckedFlags = Flags & DepthStencilPlanarFlags(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View));
    if (!CheckedFlags)
      return;
    if (Rects) {
      FlushDeferredResolves();
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, 0, CheckedFlags);
      for (unsigned i = 0; i < RectCount; i++) {
        auto rect = Rects[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        allocator_->clear_rtv_.clear(rect_offset_x, rect_offset_y, rect_width, rect_height, Depth, Stencil);
      }
      allocator_->clear_rtv_.end();
      return;
    }
    EndPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = CheckedFlags;
    encoder_info->depth_stencil = {Depth, Stencil};
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = 0;

    EndPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearDepthStencilView", __LINE__);
      TraceRecording("ClearDepthStencilView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ClearRenderTargetView(
      D3D12_CPU_DESCRIPTOR_HANDLE RTV, const FLOAT Color[4], UINT RectCount, const D3D12_RECT *Rects
  ) { DXMT_LT(41);
    if (!RequireOpenRecording("ClearRenderTargetView", DXMT_OPCLASS("ClearRenderTargetView"))) return; DropDiscards(); ENCODERS_REASON(ReasonClear);
    try {
    auto [Heap, Index] = GetRenderTargetHeap(device_, RTV);
    auto AttachmentDesc = Heap->GetRenderTarget(Index);
    if (!AttachmentDesc.Texture)
      return;
    if (Rects) {
      FlushDeferredResolves();
      allocator_->clear_rtv_.begin(AttachmentDesc.Texture, AttachmentDesc.View, AttachmentDesc.DepthPlane);
      for (unsigned i = 0; i < RectCount; i++) {
        auto rect = Rects[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        allocator_->clear_rtv_.clear(
            rect_offset_x, rect_offset_y, rect_width, rect_height, AttachmentDesc.RenderTargetArrayLength,
            {Color[0], Color[1], Color[2], Color[3]}
        );
      }
      allocator_->clear_rtv_.end();
      return;
    }
    EndPass();
    auto encoder_info = allocator_->AllocatePass<ClearEncoderData>();
    encoder_info->type = EncoderType::Clear;
    encoder_info->clear_dsv = 0;
    encoder_info->color = {Color[0], Color[1], Color[2], Color[3]};
    SanitizeRTVClearColor(AttachmentDesc.Texture->pixelFormat(AttachmentDesc.View), encoder_info->color);
    encoder_info->attachment = AttachmentDesc.Texture->view(AttachmentDesc.View);
    encoder_info->array_length = AttachmentDesc.RenderTargetArrayLength;
    encoder_info->width = AttachmentDesc.Width;
    encoder_info->height = AttachmentDesc.Height;
    encoder_info->depth_plane = AttachmentDesc.DepthPlane;

    EndPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearRenderTargetView", __LINE__);
      TraceRecording("ClearRenderTargetView", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewUint(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const UINT Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) { DXMT_LT(42);
    if (!RequireOpenRecording("ClearUnorderedAccessViewUint", DXMT_OPCLASS("ClearUnorderedAccessViewUint"))) return; DropDiscards(); ENCODERS_REASON(ReasonClearHelper); FlushDeferredResolves();   // the clear helpers end passes inside the allocator
    try {
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<uint32_t, 4>({Values[0], Values[1], Values[2], Values[3]});
    D3D12_RECT full_rect;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      full_rect = {
          (LONG)Descriptor.UAVBuffer.slice.byteOffset >> 2, 0,
          (LONG)((Descriptor.UAVBuffer.slice.byteOffset + Descriptor.UAVBuffer.slice.byteLength) >> 2), 1
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexture.texture, Descriptor.UAVTexture.view);
      full_rect = {
          0, 0, (LONG)Descriptor.UAVTexture.texture->width(Descriptor.UAVTexture.view),
          (LONG)Descriptor.UAVTexture.texture->height(Descriptor.UAVTexture.view)
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexelBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      full_rect = {
          (LONG)Descriptor.UAVTexelBuffer.slice.firstElement, 0,
          (LONG)(Descriptor.UAVTexelBuffer.slice.firstElement + Descriptor.UAVTexelBuffer.slice.elementCount), 1
      };
      break;
    }
    default:
      allocator_->clear_uav_.end();
      return;
    }

    const D3D12_RECT *rects = RectCount > 0 ? pRects : &full_rect;
    UINT rect_count = RectCount > 0 ? RectCount : 1;

    for (unsigned i = 0; i < rect_count; i++) {
      auto &rect = rects[i];
      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0)
        continue;
      allocator_->clear_uav_.clear(rect.left, rect.top, width, height);
      AuditReuseWrite(pResource);
    }

    allocator_->clear_uav_.end();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearUnorderedAccessViewUint", __LINE__);
      TraceRecording("ClearUnorderedAccessViewUint", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ClearUnorderedAccessViewFloat(
      D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle, D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle, ID3D12Resource *pResource,
      const float Values[4], UINT RectCount, const D3D12_RECT *pRects
  ) { DXMT_LT(43);
    if (!RequireOpenRecording("ClearUnorderedAccessViewFloat", DXMT_OPCLASS("ClearUnorderedAccessViewFloat"))) return; DropDiscards(); ENCODERS_REASON(ReasonClearHelper); FlushDeferredResolves();   // the clear helpers end passes inside the allocator
    try {
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, CpuHandle);
    auto &Descriptor = Heap->GetDescriptor(Index);
    auto color = std::array<float, 4>({Values[0], Values[1], Values[2], Values[3]});
    D3D12_RECT full_rect;
    switch (Descriptor.type) {
    case ShaderVisibleDescriptorType::UAVBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVBuffer.buffer);
      full_rect = {
          (LONG)Descriptor.UAVBuffer.slice.byteOffset >> 2, 0,
          (LONG)((Descriptor.UAVBuffer.slice.byteOffset + Descriptor.UAVBuffer.slice.byteLength) >> 2), 1
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexture: {
      allocator_->clear_uav_.begin(color, Descriptor.SRVTexture.texture, Descriptor.SRVTexture.view);
      full_rect = {
          0, 0, (LONG)Descriptor.SRVTexture.texture->width(Descriptor.SRVTexture.view),
          (LONG)Descriptor.SRVTexture.texture->height(Descriptor.SRVTexture.view)
      };
      break;
    }
    case ShaderVisibleDescriptorType::UAVTexelBuffer: {
      allocator_->clear_uav_.begin(color, Descriptor.UAVTexelBuffer.buffer, Descriptor.UAVTexelBuffer.view);
      full_rect = {
          (LONG)Descriptor.UAVTexelBuffer.slice.firstElement, 0,
          (LONG)(Descriptor.UAVTexelBuffer.slice.firstElement + Descriptor.UAVTexelBuffer.slice.elementCount), 1
      };
      break;
    }
    default:
      allocator_->clear_uav_.end();
      return;
    }

    const D3D12_RECT *rects = RectCount > 0 ? pRects : &full_rect;
    UINT rect_count = RectCount > 0 ? RectCount : 1;

    for (unsigned i = 0; i < rect_count; i++) {
      auto &rect = rects[i];
      auto width = rect.right - rect.left;
      auto height = rect.bottom - rect.top;
      if (width <= 0 || height <= 0)
        continue;
      allocator_->clear_uav_.clear(rect.left, rect.top, width, height);
      AuditReuseWrite(pResource);
    }

    allocator_->clear_uav_.end();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ClearUnorderedAccessViewFloat", __LINE__);
      TraceRecording("ClearUnorderedAccessViewFloat", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) { DXMT_LT(44);
    if (!RequireOpenRecording("DiscardResource", DXMT_OPCLASS("DiscardResource"))) return;
    try {
    // do nothing for now (ENCODERS: counted — a DontCare load candidate)
    encoder_stats::Note(encoder_stats::Get().discard_calls);
    // ENCODERS 4.2: a whole-resource discard of a render target / depth texture lets the next pass skip its load.
    auto r = static_cast<MTLD3D12Resource *>(pResource);
    if (encoder_stats::TileMemory() && r && r->texture && !pRegion) {
      const auto desc = r->GetDesc();
      const auto object = r->texture->current()->texture().handle;
      bool in_open_pass = false;
      if (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render) {
        auto render = static_cast<RenderEncoderData *>(allocator_->encoder_current);
        const auto uses = [&](const TextureViewRef &v) { return v && v->allocation->texture().handle == object; };
        for (auto &c : render->colors) in_open_pass |= uses(c.attachment) || uses(c.resolve_attachment);
        in_open_pass |= uses(render->depth.attachment) || uses(render->stencil.attachment);
      }
      if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) &&
          std::find(pending_discards_.begin(), pending_discards_.end(), object) == pending_discards_.end()) {
        // A discard of an attachment of the open pass ends it: what it drew so far is never observed (its store
        // becomes dead at execution), and the rest starts in a pass that does not load.
        if (in_open_pass) {
          ENCODERS_REASON(ReasonOther);
          EndPass();
        }
        pending_discards_.push_back(object);
      }
    }
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "DiscardResource", __LINE__);
      TraceRecording("DiscardResource", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  BeginQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) { DXMT_LT(45);
    if (!RequireOpenRecording("BeginQuery", DXMT_OPCLASS("BeginQuery"))) return;
    try {
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (!ValidateQuery(heap, Type, Index, 1, "BeginQuery"))
      return;
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    for (const auto &query : active_queries_) {
      if (query.heap == heap && query.index == Index) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        return;
      }
    }
    ENCODERS_REASON(ReasonBeginQuery);
    const auto zero_slot = [&](MTLD3D12QueryHeap *slot_heap, UINT slot) {
      static_cast<BlitEncoderData *>(allocator_->encoder_current)->query_buffers.push_back(slot_heap->results);
      auto &clear = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
      clear.type = WMTBlitCommandFillBuffer;
      clear.buffer = slot_heap->results;
      clear.offset = uint64_t(slot) * sizeof(uint64_t);
      clear.length = sizeof(uint64_t);
      clear.value = 0;
    };
    if (encoder_stats::QueryInPass()) {
      // A deferred resolve still has to read the previous value of this slot.
      if (DeferredResolveReads(heap, Index))
        EndPass();
      if (active_queries_.empty()) {
        // ENCODERS: sole active query, counted into the heap by the render pass (no split, no zero fill:
        // Reset mode overwrites every offset the pass selects, also with 0).
        ActiveQuery query{heap, Type, Index, 1, nullptr};
        if (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render &&
            !SelectDirectQuery(query))
          EndPass();   // the open pass cannot count it (other buffer, legacy queries, slot reused): next draw opens one
        active_queries_.push_back(query);
        encoder_stats::Note(encoder_stats::Get().direct_queries);
        return;
      }
      // Overlapping queries: everything active continues on the legacy path. A direct query never selected by a
      // pass has stale results and is zeroed here, together with the new one.
      PreBlit();
      for (auto &query : active_queries_) {
        if (query.mode == 1)
          zero_slot(query.heap, query.index);
        query.mode = 0;
      }
      zero_slot(heap, Index);
      active_queries_.push_back({heap, Type, Index, 0, nullptr});
      encoder_stats::Note(encoder_stats::Get().legacy_queries);
      return;
    }
    // Query boundaries split the pass so each pass has a constant active set.
    PreBlit();
    zero_slot(heap, Index);
    active_queries_.push_back({heap, Type, Index});
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "BeginQuery", __LINE__);
      TraceRecording("BeginQuery", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  EndQuery(ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT Index) { DXMT_LT(46);
    if (!RequireOpenRecording("EndQuery", DXMT_OPCLASS("EndQuery"))) return;
    try {
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    if (!ValidateQuery(heap, Type, Index, 1, "EndQuery"))
      return;
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      ENCODERS_REASON(ReasonTimestamp);
      EndPass();
      PreBlit();
      auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
      pass->timestamp_samples = heap->timestamp_pages[Index / heap->TimestampPageSize];
      pass->timestamp_index = Index % heap->TimestampPageSize;
      pass->query_buffers.push_back(heap->results);
      // Metal does not sample an empty blit pass; the private write gives it work.
      auto &fill = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
      fill.type = WMTBlitCommandFillBuffer;
      fill.buffer = heap->results;
      fill.offset = uint64_t(Index) * sizeof(uint64_t);
      fill.length = sizeof(uint64_t);
      fill.value = 0;
      EndPass();
      return;
    }
    ENCODERS_REASON(ReasonEndQuery);
    for (auto query = active_queries_.begin(); query != active_queries_.end(); ++query) {
      if (query->heap == heap && query->index == Index && query->type == Type) {
        if (query->mode == 2) {
          // Direct: stop counting; the pass keeps going. Disabling at the query's own offset never rewrites
          // another slot (native probe 15.10: a disable at a different offset can store 0 there).
          if (allocator_->encoder_current == query->pass)
            EmitVisibilityMode(WMTVisibilityResultModeDisabled, query->index);
          active_queries_.erase(query);
          return;
        }
        if (query->mode == 1) {
          // Direct but no pass ever selected it: the result is 0 and has to be written.
          const auto slot_heap = query->heap;
          active_queries_.erase(query);
          PreBlit();
          static_cast<BlitEncoderData *>(allocator_->encoder_current)->query_buffers.push_back(slot_heap->results);
          auto &clear = allocator_->EncodeBlitCommand<wmtcmd_blit_fillbuffer>();
          clear.type = WMTBlitCommandFillBuffer;
          clear.buffer = slot_heap->results;
          clear.offset = uint64_t(Index) * sizeof(uint64_t);
          clear.length = sizeof(uint64_t);
          clear.value = 0;
          return;
        }
        EndPass();
        active_queries_.erase(query);
        return;
      }
    }
    SetRecordingError(E_INVALIDARG, __func__, __LINE__);
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "EndQuery", __LINE__);
      TraceRecording("EndQuery", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  ResolveQueryData(
      ID3D12QueryHeap *pHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT QueryCount, ID3D12Resource *pDstBuffer,
      UINT64 AlignedDstBufferOffset
  ) { DXMT_LT(47);
    if (!RequireOpenRecording("ResolveQueryData", DXMT_OPCLASS("ResolveQueryData"))) return;
    try {
    auto heap = static_cast<MTLD3D12QueryHeap *>(pHeap);
    auto dst = static_cast<MTLD3D12Resource *>(pDstBuffer);
    if (!ValidateQuery(heap, Type, StartIndex, QueryCount, "ResolveQueryData"))
      return;
    if (!dst || !dst->buffer || (AlignedDstBufferOffset & 7)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    Com<ID3D12Device> dst_device;
    if (FAILED(dst->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&dst_device))) ||
        dst_device.ptr() != device_) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    const auto desc = dst->GetDesc();
    const uint64_t bytes = uint64_t(QueryCount) * sizeof(uint64_t);
    if (AlignedDstBufferOffset > desc.Width || bytes > desc.Width - AlignedDstBufferOffset) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    for (const auto &query : active_queries_) {
      if (query.heap == heap && query.index >= StartIndex && query.index - StartIndex < QueryCount) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        return;
      }
    }
    if (!QueryCount)
      return;
    if (Type == D3D12_QUERY_TYPE_TIMESTAMP) {
      ENCODERS_REASON(ReasonTimestamp);
      EndPass();
      while (QueryCount) {
        const UINT local_index = StartIndex % heap->TimestampPageSize;
        const UINT length = std::min(QueryCount, heap->TimestampPageSize - local_index);
        PreBlit();
        auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
        pass->resolve_samples = heap->timestamp_pages[StartIndex / heap->TimestampPageSize];
        pass->resolve_destination = dst->buffer->current()->buffer();
        pass->resolve_start = local_index;
        pass->resolve_count = length;
        pass->resolve_offset = AlignedDstBufferOffset;
        EndPass();
        StartIndex += length;
        QueryCount -= length;
        AlignedDstBufferOffset += uint64_t(length) * sizeof(uint64_t);
      }
      return;
    }
    ENCODERS_REASON(ReasonResolveQuery);
    if (encoder_stats::QueryInPass() && (Type == D3D12_QUERY_TYPE_OCCLUSION || Type == D3D12_QUERY_TYPE_BINARY_OCCLUSION)) {
      // ENCODERS: the copy waits for the next pass boundary, so a render pass full of queries stays one pass.
      // Until then nothing may read dst (that needs a barrier, which ends the pass) or restart these slots
      // (BeginQuery flushes first); direct counts land when the pass ends, before the flushed copy runs.
      PushResolve({Type, heap->results, heap->binary_resolve, dst->buffer->current()->buffer(),
                   StartIndex, QueryCount, AlignedDstBufferOffset});
      encoder_stats::Note(encoder_stats::Get().deferred_resolves);
      return;
    }
    if (Type == D3D12_QUERY_TYPE_OCCLUSION) {
      PreBlit();
      auto &buffers = static_cast<BlitEncoderData *>(allocator_->encoder_current)->query_buffers;
      buffers.push_back(heap->results);
      buffers.emplace_back(dst->buffer->current()->buffer());
      auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = heap->results;
      copy.src_offset = uint64_t(StartIndex) * sizeof(uint64_t);
      copy.dst = dst->buffer->current()->buffer();
      copy.dst_offset = AlignedDstBufferOffset;
      copy.copy_length = bytes;
      return;
    }

    EndPass();
    auto compute = allocator_->AllocatePass<ComputeEncoderData>();
    compute->type = EncoderType::Compute;
    compute->cmd_head.type = WMTComputeCommandNop;
    compute->cmd_head.next.set(0);
    compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
    compute->query_buffers.push_back(heap->results);
    compute->query_buffers.emplace_back(dst->buffer->current()->buffer());
    compute->query_pipeline = heap->binary_resolve;
    auto &pipeline = allocator_->EncodeComputeCommand<wmtcmd_compute_setpso>();
    pipeline.type = WMTComputeCommandSetPSO;
    pipeline.pso = heap->binary_resolve;
    pipeline.threadgroup_size = {1, 1, 1};
    auto &src_buffer = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    src_buffer.type = WMTComputeCommandSetBuffer;
    src_buffer.buffer = heap->results;
    src_buffer.offset = uint64_t(StartIndex) * sizeof(uint64_t);
    src_buffer.index = 0;
    auto &dst_buffer = allocator_->EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    dst_buffer.type = WMTComputeCommandSetBuffer;
    dst_buffer.buffer = dst->buffer->current()->buffer();
    dst_buffer.offset = AlignedDstBufferOffset;
    dst_buffer.index = 1;
    auto &dispatch = allocator_->EncodeComputeCommand<wmtcmd_compute_dispatch>();
    dispatch.type = WMTComputeCommandDispatch;
    dispatch.size = {QueryCount, 1, 1};
    // Internal compute state must never leak into an application's next dispatch.
    EndPass();
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ResolveQueryData", __LINE__);
      TraceRecording("ResolveQueryData", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Op) { DXMT_LT(48);
    if (!RequireOpenRecording("SetPredication", DXMT_OPCLASS("SetPredication"))) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetPredication", __LINE__);
      TraceRecording("SetPredication", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *data, UINT size) { DXMT_LT(49);
    if (!RequireOpenRecording("SetMarker", DXMT_OPCLASS("SetMarker"))) return;
    try { IMPLEMENT_ME   } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetMarker", __LINE__);
      TraceRecording("SetMarker", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *data, UINT size) { DXMT_LT(50);
    if (!RequireOpenRecording("BeginEvent", DXMT_OPCLASS("BeginEvent"))) return;
    try { IMPLEMENT_ME   } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "BeginEvent", __LINE__);
      TraceRecording("BeginEvent", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE EndEvent() { DXMT_LT(51);
    if (!RequireOpenRecording("EndEvent", DXMT_OPCLASS("EndEvent"))) return;
    try { IMPLEMENT_ME   } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "EndEvent", __LINE__);
      TraceRecording("EndEvent", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

#include "encode-mesh-indirect.inc"

  void STDMETHODCALLTYPE ExecuteIndirect(
      ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgBuffer,
      UINT64 ArgBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset
  ) { DXMT_LT(52);
    if (!RequireOpenRecording("ExecuteIndirect", DXMT_OPCLASS("ExecuteIndirect"))) return; ENCODERS_REASON(ReasonIndirect);
    if (failed_async_pso_) { pso_work::Skipped("indirect"); return; }
    struct ForgetOnExit { MTLD3D12GraphicsCommandListImpl *list; ~ForgetOnExit() { list->ForgetEmittedState(); } } forget_on_exit{this};
    try {
    const auto trace_outcome = [&](const char *outcome, const char *reason) {
      TraceRecording("ExecuteIndirect", outcome, reason,
                     "max_count=%u zero_count=%u signature=%p arg_buffer=%p arg_offset=%llu count_buffer=%p count_offset=%llu recording_hr=%08x",
                     MaxCommandCount, unsigned(!MaxCommandCount), static_cast<const void *>(pCommandSignature),
                     static_cast<const void *>(pArgBuffer), static_cast<unsigned long long>(ArgBufferOffset),
                     static_cast<const void *>(pCountBuffer), static_cast<unsigned long long>(CountBufferOffset), unsigned(recording_error_));
    };
    auto sig = static_cast<MTLD3D12CommandSignature *>(pCommandSignature);
    if (!sig) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "missing_command_signature");
      return;
    }
    if (tess_prepass::Verify() && tess_census::Enabled() && MaxCommandCount && pso_graphics_ &&
        pso_graphics_->tess_control_points && (sig->CommandType==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ||
        sig->CommandType==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED)) {
      const DWORD saved=GetLastError(); DWORD written; char line[192];
      const int n=std::snprintf(line,sizeof(line),"dx12_tess_verify status=NOT_ENABLED reason=indirect_tess_draw max_count=%u\n",MaxCommandCount);
      if (n>0 && size_t(n)<sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,DWORD(n),&written,nullptr);
      SetLastError(saved);
    }
    auto arg_buffer = static_cast<MTLD3D12Resource *>(pArgBuffer);
    if (!arg_buffer || !arg_buffer->buffer) {
      trace_outcome("rejected", !arg_buffer ? "missing_argument_resource" : "argument_resource_not_buffer");
      return;
    }
    auto ArgBufferAddress = arg_buffer->buffer->current()->gpuAddress() + ArgBufferOffset;
    uint64_t CountBufferAddress = 0;
    if (auto count_buffer = static_cast<MTLD3D12Resource *>(pCountBuffer)) {
      if (!count_buffer->buffer) {
        trace_outcome("rejected", "count_resource_not_buffer");
        return;
      }
      CountBufferAddress = count_buffer->buffer->current()->gpuAddress() + CountBufferOffset;
    }
    if (sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
      if (!PreDispatch(sig->UpdateRootArguments)) {
        trace_outcome("rejected", "PreDispatch_false_missing_compute_pso");
        return;
      }

      auto cmd = allocator_->EncodeIndirectComputeCommand(sig, pso_compute_.ptr(), MaxCommandCount);
      cmd->max_count_buffer = CountBufferAddress;
      cmd->argument_buffer = ArgBufferAddress;

      AuditReuseDraw(true,{4,MaxCommandCount},false,sig,ArgBufferAddress,CountBufferAddress,MaxCommandCount);

      if (sig->UpdateRootArguments) {
        cmd->rootsig_qwords = EncodeRootArgument(rootsig_compute_.ptr(), rootarg_compute_staging_.words.data(), MaxCommandCount);
        cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
        cmd->rootsig_qwords_stride = rootsig_compute_->UploadQwords;
        cmd->static_samplers = EncodeStaticSamplers(rootsig_compute_.ptr());
        cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
        // The CPU snapshot cannot claim arguments modified by indirect execution are still initialized.
        rootarg_compute_staging_.initialized.fill(0);
      }

      trace_outcome("emitted", MaxCommandCount ? "indirect_compute_command" : "indirect_compute_zero_max_count");
      return;
    }
    WMTPrimitiveType primitive_type;
    uint32_t cp_count;
    if (!to_metal_primitive_type(topology_, primitive_type, cp_count)) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "invalid_topology");
      return;
    }
    if (!pso_graphics_ || cp_count != pso_graphics_->tess_control_points) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", !pso_graphics_ ? "missing_graphics_pso" : "patch_control_point_mismatch");
      return;
    }
    const auto gs_topology = pso_graphics_->geometry_input_vertices == 1 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST :
                             pso_graphics_->geometry_input_vertices == 2 ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (pso_graphics_->geometry_pipeline && topology_ != gs_topology) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      trace_outcome("rejected", "geometry_input_topology_mismatch");
      return;
    }
    if (pso_graphics_->geometry_pipeline || cp_count) {
      if (EncodeMeshIndirect(sig, MaxCommandCount, arg_buffer, ArgBufferOffset,
                             static_cast<MTLD3D12Resource *>(pCountBuffer), CountBufferOffset)) {
        if (MaxCommandCount) AuditVertexInputs(sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? "indirect_indexed" : "indirect_draw");
        AuditReuseDraw(false,{5,MaxCommandCount},sig->CommandType==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
                       sig,ArgBufferAddress,CountBufferAddress,MaxCommandCount);
        AuditHazardDraw(sig->UpdateRootArguments);
        RecordGpu4Reads(sig->UpdateRootArguments);
        CountGpu4Geometry(2, MaxCommandCount, 1);
        trace_outcome("emitted", MaxCommandCount ? "indirect_mesh_gpu_marshaled" : "indirect_mesh_zero_max_count");
      }
      return;
    }
    bool encode_binding = sig->UpdateRootArguments || sig->UpdateIndexBuffer || sig->UpdateVertexBuffers;
    DrawCallStatus status = PreDraw(encode_binding);
    if (encode_binding) LightrnRootsUnknown();   // LIGHTRN: root arguments copied into the indirect command buffer
    // c17 QUERY_NO_COLOR: indirect command buffers may carry their own pipelines: keep the colours.
    if (allocator_->encoder_current && allocator_->encoder_current->type == EncoderType::Render)
      static_cast<RenderEncoderData *>(allocator_->encoder_current)->no_color_ok = false;
    if (status == DrawCallStatus::Invalid) {
      trace_outcome("rejected", recording_predraw_reason_ ? recording_predraw_reason_ : "PreDraw_invalid");
      return;
    }
    if (status != DrawCallStatus::Ordinary) {
      SetRecordingError(E_NOTIMPL, __func__, __LINE__);
      trace_outcome("rejected", status == DrawCallStatus::Geometry ? "indirect_geometry_unsupported" :
                    "indirect_tessellation_unsupported");
      return;
    }

    auto cmd = allocator_->EncodeIndirectRenderCommand(sig, pso_graphics_.ptr(), MaxCommandCount);
    cmd->max_count_buffer = CountBufferAddress;
    cmd->argument_buffer = ArgBufferAddress;
    cmd->primitive_type = primitive_type;
    cmd->index_buffer = index_buffer_address;
    cmd->index_buffer_format = index_type == WMTIndexTypeUInt32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    AuditReuseDraw(false,{5,MaxCommandCount},sig->CommandType==D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED,
                   sig,ArgBufferAddress,CountBufferAddress,MaxCommandCount);
    AuditHazardDraw(sig->UpdateRootArguments);
    RecordGpu4Reads(sig->UpdateRootArguments);
    CountGpu4Geometry(2, MaxCommandCount, 1);
    if (!encode_binding) {
      if (MaxCommandCount) AuditVertexInputs(sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? "indirect_indexed" : "indirect_draw");
      trace_outcome("emitted", MaxCommandCount ? "indirect_render_command" : "indirect_render_zero_max_count");
      return;
    }
    cmd->rootsig_qwords = EncodeRootArgument(rootsig_graphics_.ptr(), rootarg_graphics_staging_.words.data(), MaxCommandCount);
    cmd->rootsig_qwords += allocator_->gpu_heap_buffer_address_;
    cmd->rootsig_qwords_stride = rootsig_graphics_->UploadQwords;
    cmd->static_samplers = EncodeStaticSamplers(rootsig_graphics_.ptr());
    cmd->static_samplers += allocator_->gpu_heap_buffer_address_;
    auto [VBOffset, VBStride] = PopulateVertexBufferTable(MaxCommandCount);
    cmd->vertex_buffer = allocator_->gpu_heap_buffer_address_ + VBOffset;
    cmd->vertex_argbuf_stride = VBStride;
    if (sig->UpdateRootArguments) rootarg_graphics_staging_.initialized.fill(0);
    if (MaxCommandCount) AuditVertexInputs(sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? "indirect_indexed" : "indirect_draw",
                                          unsigned(sig->UpdateVertexBuffers) | (unsigned(sig->UpdateIndexBuffer) << 1));
    trace_outcome("emitted", MaxCommandCount ? "indirect_render_with_bindings" : "indirect_render_zero_max_count");
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ExecuteIndirect", __LINE__);
      TraceRecording("ExecuteIndirect", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  };

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) { DXMT_LT(53);
    if (!RequireOpenRecording("AtomicCopyBufferUINT", DXMT_OPCLASS("AtomicCopyBufferUINT"))) return; ENCODERS_REASON(ReasonOther);
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "AtomicCopyBufferUINT", __LINE__);
      TraceRecording("AtomicCopyBufferUINT", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  AtomicCopyBufferUINT64(
      ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies,
      ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges
  ) { DXMT_LT(54);
    if (!RequireOpenRecording("AtomicCopyBufferUINT64", DXMT_OPCLASS("AtomicCopyBufferUINT64"))) return; ENCODERS_REASON(ReasonOther);
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "AtomicCopyBufferUINT64", __LINE__);
      TraceRecording("AtomicCopyBufferUINT64", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  OMSetDepthBounds(FLOAT Min, FLOAT Max) { DXMT_LT(55);
    if (!RequireOpenRecording("OMSetDepthBounds", DXMT_OPCLASS("OMSetDepthBounds"))) return;
    try {
    WARN("OMSetDepthBounds: ignoring (", Min, ", ", Max, ")");
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "OMSetDepthBounds", __LINE__);
      TraceRecording("OMSetDepthBounds", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *pSamplePositions) { DXMT_LT(56);
    if (!RequireOpenRecording("SetSamplePositions", DXMT_OPCLASS("SetSamplePositions"))) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetSamplePositions", __LINE__);
      TraceRecording("SetSamplePositions", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  ResolveSubresourceRegion(
      ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource *pSrcResource,
      UINT SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode
  ) { DXMT_LT(57);
    if (!RequireOpenRecording("ResolveSubresourceRegion", DXMT_OPCLASS("ResolveSubresourceRegion"))) return; DropDiscards(); ENCODERS_REASON(ReasonResolveSubresource);
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "ResolveSubresourceRegion", __LINE__);
      TraceRecording("ResolveSubresourceRegion", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  SetViewInstanceMask(UINT Mask) { DXMT_LT(58);
    if (!RequireOpenRecording("SetViewInstanceMask", DXMT_OPCLASS("SetViewInstanceMask"))) return;
    try {
    IMPLEMENT_ME
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "SetViewInstanceMask", __LINE__);
      TraceRecording("SetViewInstanceMask", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }

  void STDMETHODCALLTYPE
  WriteBufferImmediate(
      UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes
  ) { DXMT_LT(59);
    if (!RequireOpenRecording("WriteBufferImmediate", DXMT_OPCLASS("WriteBufferImmediate"))) return; ENCODERS_REASON(ReasonWriteImmediate);
    try {
    if (encoder_count != std::numeric_limits<size_t>::max() || !Count)
      return;
    if (!pParams) {
      SetRecordingError(E_INVALIDARG, __func__, __LINE__);
      return;
    }
    if (allocator_->type_ != D3D12_COMMAND_LIST_TYPE_DIRECT &&
        allocator_->type_ != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        allocator_->type_ != D3D12_COMMAND_LIST_TYPE_COPY) {
      SetRecordingError(E_NOTIMPL, __func__, __LINE__);
      return;
    }
    struct Destination {
      WMT::Reference<WMT::Buffer> buffer;
      uint64_t offset;
    };
    std::vector<Destination> destinations;
    destinations.reserve(Count);
    UINT modes_seen = 0;
    for (UINT i = 0; i < Count; ++i) {
      const auto mode = pModes ? pModes[i] : D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
      uint64_t offset = 0;
      auto allocation = (pParams[i].Dest & 3) ? nullptr :
          device_->LookupBufferByVA(pParams[i].Dest, sizeof(UINT), &offset);
      if (!allocation || (mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT &&
                          mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN &&
                          mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT)) {
        SetRecordingError(E_INVALIDARG, __func__, __LINE__);
        return;
      }
      modes_seen |= 1u << mode;
      destinations.push_back({WMT::Reference<WMT::Buffer>(allocation->buffer()), offset});
      copy_gather::NoteImmediateTarget(allocation->buffer().handle);   // GAPS COPY_GATHER: copies touching it never move
    }
    static std::atomic_uint trace_count{0};
    if (trace_count.fetch_add(1, std::memory_order_relaxed) < 16)
      fprintf(stderr, "dx12_immediate count=%u modes_mask=%u supported=1\n", Count, modes_seen);
    WMTBufferInfo info{};
    info.length = uint64_t(Count) * sizeof(UINT);
    info.options = WMTResourceStorageModeShared;
    auto values = device_->GetMTLDevice().newBuffer(info);
    if (!values || !info.memory.get()) {
      SetRecordingError(E_OUTOFMEMORY, __func__, __LINE__);
      return;
    }
    // Capture caller values at recording time; retain them across list replays.
    for (UINT i = 0; i < Count; ++i)
      static_cast<UINT *>(info.memory.get())[i] = pParams[i].Value;
    // c17 DEFER_MARKERS: MARKER_OUT (=1) / also MARKER_IN (=2). Both modes only bound the write from below ("after the
    // preceding commands have completed / started"), so a later write keeps the guarantee; recording order is kept by the
    // flush (same bytes = later blit pass behind a fence), the last value lands before Close() at the latest.
    const unsigned defer_markers = c17::DeferMarkers();
    const UINT marker_modes = modes_seen & ~(1u << D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT);
    const UINT floatable_markers = (1u << D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT) |
                                   (defer_markers >= 2 ? (1u << D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN) : 0u);
    if (defer_markers && marker_modes && !(marker_modes & ~floatable_markers)) {
      if (!CanDeferBlit()) {
        c17::Note(c17::Get().markers_inline, Count);
      } else {
        for (UINT i = 0; i < Count; ++i) {
          const AccessRange range{destinations[i].buffer.handle, destinations[i].offset,
                                  destinations[i].offset + sizeof(UINT), true};
          DropDeadMarker(range);
          const size_t before = deferred_.size();
          {
            SideBlitScope side(this, {range});
            if (!PreBlit())
              return;
            auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
            pass->join_immediate_out = true;
            pass->immediate_buffers.push_back(values);
            pass->immediate_buffers.push_back(destinations[i].buffer);
            auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
            copy.type = WMTBlitCommandCopyFromBufferToBuffer;
            copy.src = values;
            copy.src_offset = uint64_t(i) * sizeof(UINT);
            copy.dst = destinations[i].buffer;
            copy.dst_offset = destinations[i].offset;
            copy.copy_length = sizeof(UINT);
          }
          if (deferred_.size() == before + 1)
            deferred_.back().marker = true;
        }
        c17::Note(c17::Get().markers_deferred, Count);
        return;
      }
    }
    if (modes_seen == (1u << D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT) && CanDeferBlit()) {
      // ENCODERS: plain immediate writes (destination in COPY_DEST) float with the other copy work; markers do not.
      std::vector<AccessRange> access;
      for (const auto &d : destinations)
        access.push_back({d.buffer.handle, d.offset, d.offset + sizeof(UINT), true});
      SideBlitScope side(this, std::move(access));
      if (!PreBlit())
        return;
      auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
      pass->join_immediate_out = true;
      pass->immediate_buffers.push_back(values);
      for (UINT i = 0; i < Count; ++i) {
        pass->immediate_buffers.push_back(destinations[i].buffer);
        auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
        copy.type = WMTBlitCommandCopyFromBufferToBuffer;
        copy.src = values;
        copy.src_offset = uint64_t(i) * sizeof(UINT);
        copy.dst = destinations[i].buffer;
        copy.dst_offset = destinations[i].offset;
        copy.copy_length = sizeof(UINT);
      }
      return;
    }
    for (UINT i = 0; i < Count; ++i) {
      const auto mode = pModes ? pModes[i] : D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
      EndPass();
      if (!PreBlit())
        return;
      auto pass = static_cast<BlitEncoderData *>(allocator_->encoder_current);
      pass->wait_for_prior_work = mode == D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
      pass->immediate_out = mode == D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
      pass->join_immediate_out = true;
      pass->immediate_buffers.push_back(values);
      pass->immediate_buffers.push_back(destinations[i].buffer);
      auto &copy = allocator_->EncodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = values;
      copy.src_offset = uint64_t(i) * sizeof(UINT);
      copy.dst = destinations[i].buffer;
      copy.dst_offset = destinations[i].offset;
      copy.copy_length = sizeof(UINT);
      EndPass();
    }
    } catch (const GPUHeapExhausted &e) {
      SetRecordingError(E_OUTOFMEMORY, "WriteBufferImmediate", __LINE__);
      TraceRecording("WriteBufferImmediate", "rejected", "gpu_upload_heap_exhausted",
                     "used=%llu requested=%llu alignment=%llu capacity=%llu",
                     (unsigned long long)e.used, (unsigned long long)e.requested,
                     (unsigned long long)e.alignment, (unsigned long long)e.capacity);
    }
  }
};

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::CreateCommandList(
    UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12PipelineState *pInitialPipelineState, REFIID riid,
    void **ppCommandList
) { DXMT_LT(60);
  if (Type != type_)
    return E_INVALIDARG;

  auto cmd_list = Com(new MTLD3D12GraphicsCommandListImpl(device_));
  HRESULT hr = cmd_list->Initialize(this, pInitialPipelineState);
  if (FAILED(hr))
    return hr;
  return cmd_list->QueryInterface(riid, ppCommandList);
}

}; // namespace dxmt
