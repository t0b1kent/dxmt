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
  // c17 MERGE_DEPENDENCIES: a draw of this pass uses a pipeline that can write memory (a UAV in some stage).
  bool may_write_memory = false;
  // c17 QUERY_NO_COLOR: every draw so far used a pipeline with a colourless twin; its SetPSO commands and their twins.
  bool no_color_ok = true;
  std::vector<std::pair<wmtcmd_render_setpso *, obj_handle_t>> no_color_cmds;
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
  std::vector<WMT::Reference<WMT::Buffer>> query_buffers;
  WMT::Reference<WMT::ComputePipelineState> query_pipeline;
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
