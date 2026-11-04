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

#include "d3d12_command_allocator.hpp"
#include "d3d12_icb_range.hpp"   // GPU-PASS 24.10
#include "d3d12_layer_time.hpp"   // CPU-LATE 23.10: DXMT_LT scopes
#include "com/com_pointer.hpp"
#include "d3d12_command_failure_trace.hpp"
#include "d3d12_pipeline_timing.hpp"

namespace dxmt {

MTLD3D12CommandAllocatorImpl::MTLD3D12CommandAllocatorImpl(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type) :
    MTLD3D12Pageable<MTLD3D12CommandAllocator>(pDevice),
    type_(Type),
    clear_uav_(device_->GetMTLDevice(), *this),
    clear_rtv_(device_->GetMTLDevice(), *this),
    copy_temp_allocator_(
        {device_->GetMTLDevice(), WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}
    ),
    copy_temp_version_(0),
    tess_temp_allocator_(
        {device_->GetMTLDevice(), WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}
    ) {}

HRESULT
CreateCommandAllocator(MTLD3D12Device *pDevice, D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator) {
  switch (Type) {
  case D3D12_COMMAND_LIST_TYPE_DIRECT:
  case D3D12_COMMAND_LIST_TYPE_BUNDLE:
  case D3D12_COMMAND_LIST_TYPE_COMPUTE:
  case D3D12_COMMAND_LIST_TYPE_COPY:
    break;
  default:
    return E_INVALIDARG;
  }
  auto command_allocator = Com(new MTLD3D12CommandAllocatorImpl(pDevice, Type));
  HRESULT hr = command_allocator->Initialize();
  if (FAILED(hr))
    return hr;
  return command_allocator->QueryInterface(riid, ppCommandAllocator);
}

HRESULT
MTLD3D12CommandAllocatorImpl::Initialize() {

  if (!cpu_heap_)
    cpu_heap_ = malloc(kCPUHeapSize);
  if (!cpu_heap_)
    return E_OUTOFMEMORY;
  cpu_heap_offset_ = 0;

  if (!gpu_heap_)
    gpu_heap_ = malloc(kGPUHeapSize);
  if (!gpu_heap_)
    return E_OUTOFMEMORY;
  gpu_heap_offset_ = 0;

  // GAPS CPU stage: ALLOC_RESET_SPLIT times the parts (observation only); ALLOC_RESET_FAST keeps the GPU heap buffer.
  const bool split = cpu_stage::AllocResetSplit() != 0;
  auto &st = cpu_stage::Split();
  uint64_t t = split ? cpu_stage::Ticks() : 0;
  const auto mark = [&](unsigned part) {
    if (!split) return;
    const uint64_t now = cpu_stage::Ticks();
    st.part[part].fetch_add(now - t, std::memory_order_relaxed);
    t = now;
  };
  const unsigned fast = cpu_stage::AllocResetFast();
  if (fast && gpu_heap_buffer_) {
    // The buffer wraps gpu_heap_wrapped_ (== gpu_heap_ unless the negative control moved the CPU side away).
    if (fast == 9) {
      if (!gpu_heap_alt_) gpu_heap_alt_ = malloc(kGPUHeapSize);
      if (gpu_heap_alt_) gpu_heap_ = gpu_heap_ == gpu_heap_wrapped_ ? gpu_heap_alt_ : gpu_heap_wrapped_;
    }
    if (split) st.kept_buffers.fetch_add(1, std::memory_order_relaxed);
    mark(cpu_stage::PartNewBuffer);
  } else {
    gpu_heap_buffer_ = {};
    mark(cpu_stage::PartRelease);
    WMTBufferInfo buffer_info;
    buffer_info.memory.set(gpu_heap_);
    buffer_info.length = kGPUHeapSize;
    buffer_info.options = WMTResourceHazardTrackingModeUntracked;
    gpu_heap_buffer_ = device_->GetMTLDevice().newBuffer(buffer_info);

    if (!gpu_heap_buffer_) {
      ERR("CommandAllocator: failed to allocate gpu buffer");
      return E_FAIL;
    }
    gpu_heap_buffer_address_ = buffer_info.gpu_address;
    gpu_heap_wrapped_ = gpu_heap_;
    mark(cpu_stage::PartNewBuffer);
  }

  encoder_current = nullptr;
  encoder_last = nullptr;
  encoder_count_ = 0;

  icb_.clear();
  mark(cpu_stage::PartIcb);

  tess_temp_allocator_.reset();   // GPU-PASS TESS_PREPASS; GPU completion is required by ID3D12CommandAllocator::Reset
  mark(cpu_stage::PartTess);
  copy_temp_allocator_.free_blocks(copy_temp_version_++);
  mark(cpu_stage::PartCopy);

  return S_OK;
}

HRESULT
STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::QueryInterface(REFIID riid, void **ppvObject) { DXMT_LT(232);
  if (ppvObject == nullptr)
    return E_POINTER;

  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
      riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandAllocator)) {
    *ppvObject = ref(this);
    return S_OK;
  }

  if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid)) {
    WARN("D3D12CommandAllocator: Unknown interface query ", str::format(riid));
  }

  return E_NOINTERFACE;
}

void
MTLD3D12CommandAllocatorImpl::DestroyRecordedPasses() {
  // Also retire an unfinished pass when the allocator itself is destroyed.
  InvalidateCurrentPass();
  const bool split = cpu_stage::AllocResetSplit() != 0;   // GAPS CPU stage (observation only)
  auto &st = cpu_stage::Split();
  uint64_t t = split ? cpu_stage::Ticks() : 0;
  uint64_t kinds[8] = {};
  for (auto &encoder_list : encoder_lists_) {
    EncoderData *next = encoder_list.next;
    while (next) {
      EncoderData *following = next->next;
      if (split) ++kinds[unsigned(next->type) & 7];
      switch (next->type) {
      case EncoderType::Null:
        break;
      case EncoderType::Clear:
        reinterpret_cast<ClearEncoderData *>(next)->~ClearEncoderData();
        break;
      case EncoderType::Render:
        reinterpret_cast<RenderEncoderData *>(next)->~RenderEncoderData();
        break;
      case EncoderType::Blit:
        reinterpret_cast<BlitEncoderData *>(next)->~BlitEncoderData();
        break;
      case EncoderType::Compute:
        reinterpret_cast<ComputeEncoderData *>(next)->~ComputeEncoderData();
        break;
      case EncoderType::ArgumentCompute:
        reinterpret_cast<ArgumentComputeEncoderData *>(next)->~ArgumentComputeEncoderData();
        break;
      case EncoderType::AccelerationBuild:
        reinterpret_cast<AccelerationBuildEncoderData *>(next)->~AccelerationBuildEncoderData();
        break;
      case EncoderType::Resolve:
        reinterpret_cast<ResolveEncoderData *>(next)->~ResolveEncoderData();
        break;
      }
      next = following;
    }
  }
  encoder_lists_.clear();
  if (split) {
    const uint64_t now = cpu_stage::Ticks();
    st.part[cpu_stage::PartDestroy].fetch_add(now - t, std::memory_order_relaxed);
    t = now;
    for (unsigned i = 0; i < 8; ++i)
      if (kinds[i]) st.passes[i].fetch_add(kinds[i], std::memory_order_relaxed);
    st.spilled_blocks.fetch_add(spilled_cpu_heap_.size(), std::memory_order_relaxed);
  }

  if (cpu_stage::AllocResetFast() && cpu_heap_) {
    // GAPS ALLOC_RESET_FAST: keep the spilled blocks for the next recordings (bounded by this allocator's high-water mark).
    if (split) st.reused_blocks.fetch_add(spilled_cpu_heap_.size(), std::memory_order_relaxed);
    for (auto ptr : spilled_cpu_heap_)
      spare_cpu_heap_.push_back(ptr);
  } else {
    for (auto ptr : spilled_cpu_heap_)
      free(ptr);
  }
  spilled_cpu_heap_.clear();
  if (split) st.part[cpu_stage::PartSpill].fetch_add(cpu_stage::Ticks() - t, std::memory_order_relaxed);
  encoder_last = nullptr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandAllocatorImpl::Reset() { DXMT_LT(233);
  pipeline_timing::Scope pipeline(pipeline_timing::AllocatorReset, this);
  if (encoder_last)
    return TraceCommandFailure(CommandFailureOperation::AllocatorReset, "allocator.reset.failure", this,
                               E_FAIL, "still_recording");
  const unsigned split = cpu_stage::AllocResetSplit();   // GAPS CPU stage (observation only)
  const uint64_t t0 = split ? cpu_stage::Ticks() : 0;
  DestroyRecordedPasses();

  const HRESULT hr = Initialize();
  if (split) {
    auto &st = cpu_stage::Split();
    st.total.fetch_add(cpu_stage::Ticks() - t0, std::memory_order_relaxed);
    const uint64_t n = st.resets.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % split == 0) cpu_stage::ReportSplit(n);
  }
  pipeline.Result(hr);
  return TraceCommandFailure(CommandFailureOperation::AllocatorReset, "allocator.reset.failure", this,
                             hr, "Initialize");
};

IndirectComputeCommandData *
MTLD3D12CommandAllocatorImpl::EncodeIndirectComputeCommand(MTLD3D12CommandSignature *pCmdSig, MTLD3D12ComputePipelineState *pPSO, size_t MaxCount) {
  WMTIndirectCommandBufferInfo info;
  info.inherit_buffers = !pCmdSig->UpdateRootArguments;
  info.inherit_pso = 1;
  info.inherit_cull_mode = 0;
  info.inherit_fill_mode = 0;
  info.inherit_front_facing = 0;
  info.inherit_depth_bias = 0;
  info.inherit_depth_clip_mode = 0;
  info.inherit_depth_stencil_state = 0;
  info.support_color_attachment_mapping = 0;
  info.support_dynamic_attribute_stride = 0;
  info.support_ray_tracing = 0;
  info.type = WMTIndirectCommandTypeConcurrentDispatch;
  info.max_vertex_buffer_binding = 0;
  info.max_fragment_buffer_binding = 0;
  info.max_object_buffer_binding = 0;
  info.max_mesh_buffer_binding = 0;
  info.max_kernel_buffer_binding = 31;
  info.max_kernel_threadgroup_memory_binding = 0;
  info.max_object_threadgroup_memory_binding = 0;
  info.gpu_resource_id = 0;

  auto icb = device_->GetMTLDevice().newIndirectCommandBuffer(info, MaxCount, WMTResourceStorageModeShared);

  auto [Ptr, Offset] = AllocateGPUHeap(sizeof(IndirectComputeCommandData), 16);

  auto data = reinterpret_cast<IndirectComputeCommandData *>(Ptr);

  data->cmd_buf = info.gpu_resource_id;
  data->max_count = MaxCount;
  data->tgsize_x = pPSO->threadgroup_size.width;
  data->tgsize_y = pPSO->threadgroup_size.height;
  data->tgsize_z = pPSO->threadgroup_size.depth;
  // GPU-PASS ICB_RANGE: one resolver thread per slot (key 1) instead of one thread over all slots (key 0).
  const bool parallel_resolve = icb_range::Enabled() && MaxCount;
  data->resolve_mode = parallel_resolve ? 1 : 0;

  {
    // populated outside
    data->max_count_buffer = 0;
    data->argument_buffer = 0;
    data->rootsig_qwords = 0;
    data->rootsig_qwords_stride = 0;
    data->static_samplers = 0;
  }

  {
    /**
     * TODO: move these out?
     */

    auto &cmd_use_icb = EncodeComputeCommand<wmtcmd_compute_useresource>();
    cmd_use_icb.type = WMTComputeCommandUseResource;
    cmd_use_icb.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
    cmd_use_icb.resource = icb;

    auto &cmd_setpso_res = EncodeComputeCommand<wmtcmd_compute_setpso>();
    cmd_setpso_res.type = WMTComputeCommandSetPSO;
    cmd_setpso_res.pso = pCmdSig->compute_resolver;
    cmd_setpso_res.threadgroup_size = {parallel_resolve ? 64u : 1u, 1, 1};

    auto &cmd_argbuf_res = EncodeComputeCommand<wmtcmd_compute_setbuffer>();
    cmd_argbuf_res.type = WMTComputeCommandSetBuffer;
    cmd_argbuf_res.buffer = gpu_heap_buffer_;
    cmd_argbuf_res.offset = Offset;
    cmd_argbuf_res.index = 30;

    auto &cmd_dispatch_res = EncodeComputeCommand<wmtcmd_compute_dispatch>();
    cmd_dispatch_res.type = WMTComputeCommandDispatch;
    cmd_dispatch_res.size = {parallel_resolve ? uint32_t((MaxCount + 63) / 64) : 1u, 1, 1};

    auto &cmd_setpso = EncodeComputeCommand<wmtcmd_compute_setpso>();
    cmd_setpso.type = WMTComputeCommandSetPSO;
    cmd_setpso.pso = pPSO->pso;
    cmd_setpso.threadgroup_size = pPSO->threadgroup_size; // not really used
  }

  auto &cmd = EncodeComputeCommand<wmtcmd_compute_executecommands>();
  cmd.type = WMTComputeCommandExecuteCommandsInBuffer;
  cmd.indirect_command_buffer = icb;
  cmd.location = 0;
  cmd.length = MaxCount;
  cmd.indirect_range_buffer = 0;   // GPU-PASS: the GPU-side range path exists in winemetal; not used (probe icb1: the range
  cmd.indirect_range_offset = 0;   // written by the resolver in the same encoder was not seen by the execution)

  icb_.push_back(std::move(icb));

  return data;
}

IndirectRenderCommandData *
MTLD3D12CommandAllocatorImpl::EncodeIndirectRenderCommand(
    MTLD3D12CommandSignature *pCmdSig, MTLD3D12GraphicsPipelineState *pPSO, size_t MaxCount
) {
  WMTIndirectCommandBufferInfo info;
  info.inherit_buffers = !(pCmdSig->UpdateVertexBuffers || pCmdSig->UpdateIndexBuffer || pCmdSig->UpdateRootArguments);
  info.inherit_pso = 1;
  info.inherit_cull_mode = 1;
  info.inherit_fill_mode = 1;
  info.inherit_front_facing = 1;
  info.inherit_depth_bias = 1;
  info.inherit_depth_clip_mode = 1;
  info.inherit_depth_stencil_state = 1;
  info.support_color_attachment_mapping = 0;
  info.support_dynamic_attribute_stride = 0;
  info.support_ray_tracing = 0;
  info.type = pCmdSig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ? WMTIndirectCommandTypeDrawIndexed
                                                                                : WMTIndirectCommandTypeDraw;
  info.max_vertex_buffer_binding = 31;
  info.max_fragment_buffer_binding = 31;
  info.max_object_buffer_binding = 0;
  info.max_mesh_buffer_binding = 0;
  info.max_kernel_buffer_binding = 0;
  info.max_kernel_threadgroup_memory_binding = 0;
  info.max_object_threadgroup_memory_binding = 0;
  info.gpu_resource_id = 0;

  auto icb = device_->GetMTLDevice().newIndirectCommandBuffer(info, MaxCount, WMTResourceStorageModePrivate);

  auto [Ptr, Offset] = AllocateGPUHeap(sizeof(IndirectRenderCommandData), 16);

  auto data = reinterpret_cast<IndirectRenderCommandData *>(Ptr);

  data->cmd_buf = info.gpu_resource_id;
  data->max_count = MaxCount;

  {
    // populated outside
    data->max_count_buffer = 0;
    data->argument_buffer = 0;
    data->rootsig_qwords = 0;
    data->rootsig_qwords_stride = 0;
    data->static_samplers = 0;
    data->vertex_buffer = 0;
    data->vertex_argbuf_stride = 0;
    data->primitive_type = 0;
    data->index_buffer = 0;
    data->index_buffer_format = {};
  }
  // GPU-PASS ICB_RANGE: one resolver thread per slot (key 1) instead of one thread over all slots (key 0).
  const bool parallel_resolve = icb_range::Enabled() && MaxCount;
  data->resolve_mode = parallel_resolve ? 1 : 0;

  {
    /**
     * TODO: move these out?
     */

    auto &cmd_use_icb = EncodeRenderCommand<wmtcmd_render_useresource>();
    cmd_use_icb.type = WMTRenderCommandUseResource;
    cmd_use_icb.stages = WMTRenderStageVertex;
    cmd_use_icb.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
    cmd_use_icb.resource = icb;

    auto &cmd_setpso_res = EncodeRenderCommand<wmtcmd_render_setpso>();
    cmd_setpso_res.type = WMTRenderCommandSetPSO;
    cmd_setpso_res.pso = pCmdSig->render_resolver;

    auto &cmd_argbuf_res = EncodeRenderCommand<wmtcmd_render_setbuffer>();
    cmd_argbuf_res.type = WMTRenderCommandSetVertexBuffer;
    cmd_argbuf_res.buffer = gpu_heap_buffer_;
    cmd_argbuf_res.offset = Offset;
    cmd_argbuf_res.index = 30;

    auto &cmd_draw_res = EncodeRenderCommand<wmtcmd_render_draw>();
    cmd_draw_res.type = WMTRenderCommandDraw;
    cmd_draw_res.primitive_type = WMTPrimitiveTypePoint;
    cmd_draw_res.vertex_start = 0;
    cmd_draw_res.vertex_count = parallel_resolve ? MaxCount : 1;   // GPU-PASS ICB_RANGE: a vertex per slot
    cmd_draw_res.base_instance = 0;
    cmd_draw_res.instance_count = 1;

    auto &cmd_setpso = EncodeRenderCommand<wmtcmd_render_setpso>();
    cmd_setpso.type = WMTRenderCommandSetPSO;
    cmd_setpso.pso = pPSO->pso;
  }

  auto &cmd = EncodeRenderCommand<wmtcmd_render_executecommands>();
  cmd.type = WMTRenderCommandExecuteCommandsInBuffer;
  cmd.indirect_command_buffer = icb;
  cmd.location = 0;
  cmd.length = MaxCount;
  cmd.indirect_range_buffer = 0;   // GPU-PASS: the GPU-side range path exists in winemetal; not used (probe icb1: the range
  cmd.indirect_range_offset = 0;   // written by the resolver in the same encoder was not seen by the execution)

  icb_.push_back(std::move(icb));

  return data;
}

template <>
WMT::Reference<WMT::ComputePipelineState>
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::getComputePipeline(std::string name) {
  auto lib = ctx.device_->GetLib().getLibrary();
  auto func = lib.newFunction(name.c_str());
  if (!func)
    return {};
  WMT::Reference<WMT::Error> err;
  auto pso = ctx.device_->GetMTLDevice().newComputePipelineState(func, err);
  if (err) {
    ERR("Failed to create compute PSO: ", err.description().getUTF8String());
  }
  return pso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::startComputePass() {
  ctx.InvalidateCurrentPass();
  auto compute = ctx.AllocatePass<ComputeEncoderData>();
  compute->type = EncoderType::Compute;
  compute->cmd_head.type = WMTComputeCommandNop;
  compute->cmd_head.next.set(0);
  compute->cmd_tail = (wmtcmd_base *)&compute->cmd_head;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::endPass() {
  ctx.InvalidateCurrentPass();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputePSO(WMT::ComputePipelineState pso, WMTSize tgsize) {
  auto &setpso = ctx.EncodeComputeCommand<wmtcmd_compute_setpso>();
  setpso.type = WMTComputeCommandSetPSO;
  setpso.pso = pso;
  setpso.threadgroup_size = tgsize;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::dispatch(WMTSize size) {
  auto &dispatch = ctx.EncodeComputeCommand<wmtcmd_compute_dispatch>();
  dispatch.type = WMTComputeCommandDispatchThreads;
  dispatch.size = size;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeTexture(
    uint32_t index, const Rc<Texture> &texture, uint64_t viewId, int flags
) {
  auto &dst_ = texture->view(viewId);
  auto &settex = ctx.EncodeComputeCommand<wmtcmd_compute_settexture>();
  settex.type = WMTComputeCommandSetTexture;
  settex.texture = dst_.texture;
  settex.index = index;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeTexelBuffer(
    uint32_t index, const Rc<Buffer> &buffer, uint64_t viewId, int flags
) {
  auto &dst_ = buffer->view_(viewId);
  auto &settexbuf = ctx.EncodeComputeCommand<wmtcmd_compute_settexture>();
  settexbuf.type = WMTComputeCommandSetTexture;
  settexbuf.texture = dst_.texture;
  settexbuf.index = index;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeBuffer(
    uint32_t index, const Rc<Buffer> &buffer, uint32_t offset, uint32_t length, int flags
) {
  auto dst_ = buffer->current();
  auto &setbuf = ctx.EncodeComputeCommand<wmtcmd_compute_setbuffer>();
  setbuf.type = WMTComputeCommandSetBuffer;
  setbuf.buffer = dst_->buffer();
  setbuf.index = index;
  setbuf.offset = 0; // the `offset` and `length` parameter of this function are just for (potential) hazard tracking,
                     // which we don't need here
}

template <>
void *
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setComputeBytes(uint32_t index, uint32_t length) {
  auto &setmeta = ctx.EncodeComputeCommand<wmtcmd_compute_setbytes>();
  setmeta.type = WMTComputeCommandSetBytes;
  void *temp = ctx.AllocateCPUHeap(length, 16);
  setmeta.bytes.set(temp);
  setmeta.length = length;
  setmeta.index = index;
  return temp;
}

template <>
WMT::Library
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::getDefaultLibrary() {
  return ctx.device_->GetLib().getLibrary();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::startRenderPass() {
  ctx.InvalidateCurrentPass();
  auto render = ctx.AllocatePass<RenderEncoderData>();
  render->type = EncoderType::Render;
  render->cmd_head.type = WMTRenderCommandNop;
  render->cmd_head.next.set(0);
  render->cmd_tail = (wmtcmd_base *)&render->cmd_head;
  render->dsv_planar_flags = 0;
  render->dsv_readonly_flags = 0;
  render->xl_dsv_flags = 0;
  render->render_target_count = 0;
  render->render_target_width = 16384;
  render->render_target_height = 16384;
  render->render_target_array_length = 0;
  render->default_raster_sample_count = 1;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setColorAttachment(
    uint32_t index, const Rc<Texture> &texture, uint64_t viewId, uint32_t depth_plane
) {
  auto encoder = static_cast<RenderEncoderData *>(ctx.encoder_current);
  encoder->colors[index].attachment = texture->view(viewId);
  encoder->colors[index].depth_plane = depth_plane;
  encoder->colors[index].load_action = WMTLoadActionLoad;
  encoder->colors[index].store_action = WMTStoreActionStore;
  encoder->render_target_count = std::max<uint32_t>(encoder->render_target_count, index + 1);
  encoder->render_target_width = std::min<uint32_t>(encoder->render_target_width, texture->width(viewId));
  encoder->render_target_height = std::min<uint32_t>(encoder->render_target_height, texture->height(viewId));
  encoder->render_target_array_length = texture->textureType() == WMTTextureType3D
                                            ? (texture->depth(viewId) - depth_plane)
                                            : texture->arrayLength(viewId);
  encoder->default_raster_sample_count = texture->sampleCount();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setDepthStencilAttachment(
    const Rc<Texture> &texture, uint64_t viewId, uint32_t dsv_flag
) {
  auto encoder = static_cast<RenderEncoderData *>(ctx.encoder_current);
  if (dsv_flag & 1) {
    encoder->depth.attachment = texture->view(viewId);
    encoder->depth.load_action = WMTLoadActionLoad;
    encoder->depth.store_action = WMTStoreActionStore;
  }
  if (dsv_flag & 2) {
    encoder->stencil.attachment = texture->view(viewId);
    encoder->stencil.load_action = WMTLoadActionLoad;
    encoder->stencil.store_action = WMTStoreActionStore;
  }
  encoder->dsv_planar_flags = dsv_flag;
  encoder->render_target_width = std::min<uint32_t>(encoder->render_target_width, texture->width(viewId));
  encoder->render_target_height = std::min<uint32_t>(encoder->render_target_height, texture->height(viewId));
  encoder->render_target_array_length = texture->arrayLength(viewId);
  encoder->default_raster_sample_count = texture->sampleCount();
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setRenderPSO(WMT::RenderPipelineState pso) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setpso>();
  cmd.type = WMTRenderCommandSetPSO;
  cmd.pso = pso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setViewport(WMTViewport viewport) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setviewport>();
  cmd.type = WMTRenderCommandSetViewport;
  cmd.viewport = viewport;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setDepthStencilState(WMT::DepthStencilState dsso) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setdepthstencilstate>();
  cmd.type = WMTRenderCommandSetDepthStencilState;
  cmd.depth_stencil_state = dsso;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setScissorRect(WMTScissorRect rect) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setscissorrect>();
  cmd.type = WMTRenderCommandSetScissorRect;
  cmd.scissor_rect = rect;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setStencilReference(uint8_t stencil_ref) {
  auto &cmd = ctx.EncodeRenderCommand<wmtcmd_render_setstencilref>();
  cmd.type = WMTRenderCommandSetStencilRef;
  cmd.stencil_ref = stencil_ref;
}

template <>
void *
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::setFragmentBytes(uint32_t index, uint32_t length) {
  auto &setmeta = ctx.EncodeRenderCommand<wmtcmd_render_setbytes>();
  setmeta.type = WMTRenderCommandSetFragmentBytes;
  void *temp = ctx.AllocateCPUHeap(length, 16);
  setmeta.bytes.set(temp);
  setmeta.length = length;
  setmeta.index = index;
  return temp;
}

template <>
void
SimpleCommandContext<MTLD3D12CommandAllocatorImpl>::draw(
    WMTPrimitiveType primitive, uint32_t vertex_start, uint32_t vertex_count, int32_t base_instance,
    uint32_t instance_count
) {
  auto &draw = ctx.EncodeRenderCommand<wmtcmd_render_draw>();
  draw.type = WMTRenderCommandDraw;
  draw.primitive_type = primitive;
  draw.vertex_start = vertex_start;
  draw.vertex_count = vertex_count;
  draw.base_instance = base_instance;
  draw.instance_count = instance_count;
}

}; // namespace dxmt
