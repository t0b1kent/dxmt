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

#pragma once

#include "dxmt_texture.hpp"
#include "dxmt_buffer.hpp"
#include "d3d12_merge_trace.hpp"
#include <cstdint>
#include <vector>
#include <memory>

namespace dxmt {
namespace reuse { struct RecordedPass; }
namespace xl_hazard { struct PassBindings; }
namespace gpu4 { struct Reads; }   // GPU4 TYPED_PRECISE / CLEAR_SINK (d3d12_gpu4_overlap.hpp)
namespace tess_prepass { struct DrawInputs; }
namespace tess_prepass { struct TwinDraw; }
namespace light_rename { struct PassRoots; struct PassDispatch; }   // LIGHTRN (d3d12_light_rename.hpp)
namespace post_chain { struct Draw; }   // POSTCS 03.11 (d3d12_post_chain.hpp)

enum class EncoderType {
  Null,
  Clear,
  Render,
  Blit,
  Compute,
  Resolve,
  ArgumentCompute,
  AccelerationBuild,
};

struct EncoderData {
  EncoderType type;
  EncoderData *next = nullptr;
  uint64_t id;
  uint8_t close_reason = 0; // ENCODERS: encoder_stats::Reason the recorded pass ended (diagnostics only)
  // ENC-CENSUS (diagnostics only, written by every encoding, read only by MACRUNNER_DX12_ENC_CENSUS):
  // census_begin = enc_census::Begin (this pass opened a Metal encoder or not), census_why = enc_census::Why.
  uint8_t census_begin = 0xff;
  uint8_t census_why = 0;
  std::shared_ptr<reuse::RecordedPass> reuse_record;
};

struct ClearEncoderData : EncoderData {
  union {
    WMTClearColor color;
    std::pair<float, uint8_t> depth_stencil;
  };
  TextureViewRef attachment;
  unsigned clear_dsv;
  unsigned array_length;
  unsigned width;
  unsigned height;
  unsigned depth_plane;
  // ENC-CENSUS (diagnostics only): the barrier record that made this pending clear be encoded on its own
  // (ClearWhyBarrier): states and subresource; census_bar_type 0xff = none.
  mutable uint32_t census_bar_before = 0, census_bar_after = 0, census_bar_sub = 0;
  mutable uint8_t census_bar_type = 0xff, census_bar_flags = 0;

  ClearEncoderData() {}
};

struct RenderEncoderColorAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  struct WMTClearColor clear_color;
  TextureViewRef resolve_attachment;
  uint16_t resolve_level;
  uint16_t resolve_slice;
  uint32_t resolve_depth_plane;
};

struct RenderEncoderDepthAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  float clear_depth;
};

struct RenderEncoderStencilAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  uint8_t clear_stencil;
};

struct VisibilityQueryTarget {
  WMT::Reference<WMT::Buffer> results;
  uint64_t offset;
};

struct RenderEncoderData : EncoderData {
  uint64_t empty_binding_first = UINT64_MAX, empty_binding_last = 0;
  // XL-GAPS (MACRUNNER_DX12_XL_HAZARD=1): descriptor spans each draw of this pass can read (observation only).
  std::shared_ptr<xl_hazard::PassBindings> xl_hazard;
  // GPU4 (d3d12_gpu4_overlap.hpp): descriptor spans the pass's draws can read (pre-raster typed SRVs; all-stage SRVs).
  std::shared_ptr<gpu4::Reads> gpu4_reads;
  bool gpu4_pre_missed = false, gpu4_all_missed = false;   // a draw recorded while the key was off (hot switching)
  // LIGHTRN (MACRUNNER_DX12_LIGHT_RENAME armed): root-argument blocks of this pass when recorded after a candidate dispatch.
  std::shared_ptr<light_rename::PassRoots> lightrn_roots;
  // POSTCS 03.11 (MACRUNNER_DX12_POST_CHAIN armed): the side record of this pass's draw when it is a candidate (command arena of the
  // list, POD), and how many draws the pass recorded (a candidate pass has exactly one).
  const post_chain::Draw *post_chain = nullptr;
  uint32_t post_chain_draws = 0;
  uint8_t post_chain_why = 0;      // PCS2 03.11: why this pass has no post_chain record (post_chain::Why*; 0 = no DrawInstanced judged)
  uint64_t post_chain_ps = 0;      // PCS2 03.11: its pipeline's pixel shader id (diagnostics)
  merge_trace::Render merge_trace;
  std::array<RenderEncoderColorAttachmentData, 8> colors;
  RenderEncoderDepthAttachmentData depth;
  RenderEncoderStencilAttachmentData stencil;
  uint8_t default_raster_sample_count;
  uint16_t render_target_array_length;
  uint32_t render_target_height;
  uint32_t render_target_width;
  wmtcmd_render_nop cmd_head;
  wmtcmd_base *cmd_tail;
  uint8_t dsv_planar_flags;
  uint8_t dsv_readonly_flags;
  // XL-GAPS (observation only): D3D12_DSV_FLAGS of the bound DSV as created. Since XL-DEFAULT (c18) the layer applies
  // them through dsv_readonly_flags (masked by the planes of the format); the hazard observer reads both.
  uint8_t xl_dsv_flags = 0;
  uint8_t render_target_count;
  std::vector<VisibilityQueryTarget> visibility_targets;
  std::vector<Rc<BufferAllocation>> index_allocations;
  WMT::Reference<WMT::ComputePipelineState> visibility_accumulate;
  bool use_tessellation = 0;
  bool use_geometry = 0;
  std::vector<WMT::Reference<WMT::Buffer>> mesh_indirect_buffers;
  std::vector<WMT::Reference<WMT::RenderPipelineState>> mesh_indirect_pipelines;
  // ENCODERS (MACRUNNER_DX12_QUERY_INPASS=1): occlusion queries counted straight into this query heap buffer
  // (visibility result buffer of the pass, MTLVisibilityResultTypeReset); direct_slots = query indices counted here.
  WMT::Reference<WMT::Buffer> visibility_direct;
  std::vector<uint32_t> direct_slots;
  // ENC-FIX LOAD_DONTCARE (c18 merge fix): attachments this pass may start with DontCare — applied only when the pass OPENS a
  // Metal encoder (a pass merged into an open encoder loads nothing at all); bits 0-7 colours, 8 depth. The load actions above
  // stay as recorded, so the merge decisions (empty-transition plan, encoder continuation) are taken first and unchanged.
  uint16_t ldc_dontcare_mask = 0;
  // The coverage probe of this pass (service query, not an application query: no merge veto): its two visibility commands
  // count into ldc_probe at ldc_probe_slot only in an encoder whose visibility buffer is ldc_probe; elsewhere they are nops.
  WMT::Reference<WMT::Buffer> ldc_probe;
  uint32_t ldc_probe_slot = 0;
  wmtcmd_render_setvisibilitymode *ldc_probe_on = nullptr, *ldc_probe_off = nullptr;
  // c17 MERGE_DEPENDENCIES: a draw of this pass uses a pipeline that can write memory (a UAV in some stage).
  bool may_write_memory = false;
  // VS-STAGE RENDER_FENCE=3: a draw of this pass reads a typed SRV or a UAV in a pre-raster stage (it waits before vertex).
  bool pre_raster_typed = false;
  // GPU-PASS TYPED_SAFE: a draw of this pass reads typed in a pre-raster stage through something other than an SRV (UAV,
  // heap indexing, unreadable bytecode, stream output): never eligible for the TYPED_SAFE split wait.
  bool pre_raster_hard = false;
  // VS-STAGE FENCE_STATS: vertex (or index) count x instances of the ordinary draws recorded in this pass.
  uint64_t vertex_work = 0;
  // c17 QUERY_NO_COLOR: every draw so far used a pipeline with a colourless twin; its SetPSO commands and their twins.
  bool no_color_ok = true;
  std::vector<std::pair<wmtcmd_render_setpso *, obj_handle_t>> no_color_cmds;
  // ENC-CENSUS (diagnostics only): FINAL load / store actions of the encoder this pass opened (slots 0-7 colours,
  // 8 depth, 9 stencil; after clear folding and dead-store dropping), and service encoders around it (bit 0 occlusion
  // scratch blit before, bit 1 occlusion reduction compute after).
  uint8_t census_load[10] = {};
  uint8_t census_store[10] = {};
  uint8_t census_svc = 0;
  // GPU-PASS TESS_PREPASS: compute commands of the tessellation pre-pass of this pass's draws, encoded in a compute encoder
  // right before the pass (the pass then never continues an open encoder); empty = none.
  wmtcmd_compute_nop prepass_head{};
  wmtcmd_base *prepass_tail = nullptr;
  wmtcmd_compute_nop prepass_scan_head{}, prepass_materialize_head{};
  wmtcmd_base *prepass_scan_tail = nullptr, *prepass_materialize_tail = nullptr;
  uint32_t prepass_draws = 0;
  // Independent hull dispatches may overlap only when all application stages before rasterization are read-only.
  bool prepass_read_only = true;
  std::vector<std::shared_ptr<tess_prepass::DrawInputs>> prepass_inputs;
  std::vector<obj_handle_t> prepass_scratch;
  // Mode 3: exactly one tess draw in this pass. Replay the original state prefix and mesh draw against a shadow target.
  const wmtcmd_base *verify_prefix_end = nullptr, *verify_mesh_draw = nullptr;
  uint64_t verify_pso = 0;
  WMT::Reference<WMT::Buffer> tess_record_buffer; // mode-3 only; retained through GPU completion
  std::shared_ptr<tess_prepass::TwinDraw> twin_draw;
};

// ENCODERS: one barrier of a barrier_only pass, as an object handle (texture allocation or buffer) and states.
struct BarrierRecord {
  obj_handle_t object = 0;    // transition / UAV: the resource; aliasing: ResourceAfter
  obj_handle_t object2 = 0;   // c17: aliasing ResourceBefore (0 elsewhere)
  uint32_t subresource = 0xffffffffu;   // c17: transition subresource (D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES elsewhere)
  uint32_t before = 0, after = 0;
  uint8_t type = 0;
  uint8_t flags = 0; // diagnostic split-barrier flags; does not change the merge verdict
};

struct BlitEncoderData : EncoderData {
  merge_trace::Barriers merge_trace;
  bool barrier_records_valid = false;
  std::vector<BarrierRecord> barrier_records;
  wmtcmd_blit_nop cmd_head;
  wmtcmd_base *cmd_tail;
  std::vector<WMT::Reference<WMT::Buffer>> query_buffers;
  std::vector<WMT::Reference<WMT::Buffer>> immediate_buffers;
  bool wait_for_prior_work = false;
  bool immediate_out = false;
  bool join_immediate_out = false;
  bool barrier_only = false;
  // GAPS COPY_GATHER (d3d12_copy_gather.hpp): a flush pass of deferred copy work only (no resolve / immediate / marker), with
  // the objects of its items (ResourceObject handles, as in the barrier records); and the join pass a flush puts before them.
  bool gaps_movable = false, gaps_join = false;
  bool gaps_common = false;   // the join covers an absorbed barrier from COMMON (the resource may have been read through promotion)
  std::vector<obj_handle_t> gaps_objects;   // all objects (reads and writes)
  std::vector<obj_handle_t> gaps_writes;    // the written ones
  WMT::Reference<WMT::CounterSampleBuffer> timestamp_samples;
  UINT timestamp_index = 0;
  WMT::Reference<WMT::CounterSampleBuffer> resolve_samples;
  WMT::Reference<WMT::Buffer> resolve_destination;
  UINT resolve_start = 0;
  UINT resolve_count = 0;
  uint64_t resolve_offset = 0;
};

struct ComputeEncoderData : EncoderData {
  wmtcmd_compute_nop cmd_head;
  wmtcmd_base *cmd_tail;
  // GAPS PRERASTER_PRECISE: a dispatch of this pass may write buffer memory. true for every pass not opened by
  // PreDispatch (resolves, clear helpers) and for every pass while the key is off.
  bool may_write_buffers = true;
  std::vector<WMT::Reference<WMT::Buffer>> query_buffers;
  WMT::Reference<WMT::ComputePipelineState> query_pipeline;
  // LIGHTRN (MACRUNNER_DX12_LIGHT_RENAME armed): the dispatches of this pass (a candidate has one, with a form).
  std::shared_ptr<light_rename::PassDispatch> lightrn;
  uint8_t lightrn_concurrent = 0;   // set only for one encoding: the pass carries a run's independent dispatches
  // set only for one encoding (copy-in run): blit commands copying S's subresources no batch fully writes into S_1..S_n-1,
  // encoded in a blit encoder right before this pass's compute encoder
  const wmtcmd_base *lightrn_copyin = nullptr;
};
struct D3D12RecordedComputeDispatch;
struct ArgumentComputeEncoderData : EncoderData {
  std::shared_ptr<const D3D12RecordedComputeDispatch> dispatch;
};

struct D3D12AccelerationBuildPlan;
struct AccelerationBuildEncoderData : EncoderData {
  std::shared_ptr<const D3D12AccelerationBuildPlan> build;
};
struct ResolveEncoderData : EncoderData {
  TextureViewRef src;
  TextureViewRef dst;
};

}; // namespace dxmt
