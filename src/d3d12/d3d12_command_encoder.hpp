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
};

struct BlitEncoderData : EncoderData {
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
