// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
  bool EncodeMeshIndirect(MTLD3D12CommandSignature *sig, UINT max_count,
                          MTLD3D12Resource *arguments, UINT64 argument_offset,
                          MTLD3D12Resource *count, UINT64 count_offset) {
    using namespace indirect_mesh;
    const auto reject = [&](HRESULT hr, const char *reason) {
      SetRecordingError(hr, "ExecuteIndirect", __LINE__);
      TraceRecording("ExecuteIndirect", "rejected", reason,
                     "max_count=%u stride=%u signature_type=%u",
                     max_count, sig->MeshDrawStride, unsigned(sig->CommandType));
      return false;
    };
    if (!sig->mesh_resolver || !sig->MeshDrawStride || sig->UpdateRootArguments ||
        sig->UpdateIndexBuffer || sig->UpdateVertexBuffers)
      return reject(E_NOTIMPL, "mesh_indirect_binding_updates_unsupported");
    // The fallback has no way to report a GPU-produced over-limit grid to D3D12.
    // Keep devices with the old outer-grid cap explicitly unsupported.
    if (max_object_threadgroups_ != UINT64_MAX)
      return reject(E_NOTIMPL, "mesh_indirect_device_grid_limit");
    const bool indexed = sig->CommandType == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    if (!RangeFits(arguments->GetDesc().Width, argument_offset, max_count,
                   sig->MeshDrawStride, indexed ? 20 : 16))
      return reject(E_INVALIDARG, "mesh_indirect_argument_range");
    if (count && (!count->buffer || !CountRangeFits(count->GetDesc().Width, count_offset)))
      return reject(E_INVALIDARG, "mesh_indirect_count_range");
    if (!max_count) return true;
    if (indexed && (!index_buffer || !index_allocation_))
      return reject(E_INVALIDARG, "mesh_indirect_missing_index_buffer");
    const bool geometry = pso_graphics_->geometry_pipeline;
    const uint32_t primitive = geometry ? pso_graphics_->geometry_input_vertices :
                                         pso_graphics_->tess_control_points;
    const uint32_t threads = geometry ? pso_graphics_->geometry_vertices_per_group :
                                       pso_graphics_->tess_threads_per_patch;
    if (!primitive || !threads || threads > 32 ||
        (geometry ? threads % primitive : 32 % threads))
      return reject(E_INVALIDARG, "mesh_indirect_invalid_group_layout");
    const uint32_t primitives_per_group = geometry ? threads / primitive : 32 / threads;
    const uint32_t variant = indexed ? (index_type == WMTIndexTypeUInt32 ? 2 : 1) : 0;
    auto arg_allocation = arguments->buffer->current();
    Rc<BufferAllocation> count_allocation = count ? count->buffer->current() : Rc<BufferAllocation>{};
    if (!arg_allocation || (count && !count_allocation))
      return reject(E_INVALIDARG, "mesh_indirect_missing_allocation");

    WMTBufferInfo info{};
    info.length = StorageBytes(max_count);
    info.options = WMTResourceStorageModeShared;
    auto dispatch = device_->GetMTLDevice().newBuffer(info);
    if (!dispatch || !info.memory.get() || !info.gpu_address)
      return reject(E_OUTOFMEMORY, "mesh_indirect_dispatch_allocation");
    const Parameters params{arg_allocation->gpuAddress() + argument_offset,
                            count ? count_allocation->gpuAddress() + count_offset : 0,
                            info.gpu_address + kRecordsOffset, max_count,
                            sig->MeshDrawStride, primitive, primitives_per_group,
                            uint32_t(indexed), 0};
    memcpy(info.memory.get(), &params, sizeof(params));
    const auto status = PreDraw(false, variant);
    if (status != (geometry ? DrawCallStatus::Geometry : DrawCallStatus::Tessellation)) return false;
    auto *pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    try {
      pass->mesh_indirect_buffers.push_back(dispatch);
      pass->mesh_indirect_pipelines.push_back(sig->mesh_resolver);
      pass->index_allocations.push_back(arg_allocation);
      if (count) pass->index_allocations.push_back(count_allocation);
    } catch (const std::bad_alloc &) {
      return reject(E_OUTOFMEMORY, "mesh_indirect_retention_allocation");
    }
    if (indexed && !RetainIndexAllocation()) return false;
    const auto use = [&](WMT::Buffer buffer, WMTResourceUsage usage, WMTRenderStages stages) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_useresource>();
      cmd.type = WMTRenderCommandUseResource;
      cmd.resource = buffer;
      cmd.usage = usage;
      cmd.stages = stages;
    };
    use(arg_allocation->buffer(), WMTResourceUsageRead, WMTRenderStageVertex);
    if (count) use(count_allocation->buffer(), WMTResourceUsageRead, WMTRenderStageVertex);
    use(dispatch, WMTResourceUsageRead | WMTResourceUsageWrite,
        WMTRenderStageVertex | WMTRenderStageObject | WMTRenderStageMesh);
    const auto barrier = [&](WMTRenderStages producers, WMTRenderStages consumers) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_memory_barrier>();
      cmd.type = WMTRenderCommandMemoryBarrier;
      cmd.scope = WMTBarrierScopeBuffers;
      SetProducerConsumerDependency(cmd, producers, consumers);
    };
    const auto bind = [&](WMTRenderCommandType type, WMT::Buffer buffer, uint64_t offset, uint32_t index) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setbuffer>();
      cmd.type = type;
      cmd.buffer = buffer;
      cmd.offset = offset;
      cmd.index = index;
    };
    const auto visibility = [&](WMTVisibilityResultMode mode) {
      auto &cmd = allocator_->EncodeRenderCommand<wmtcmd_render_setvisibilitymode>();
      cmd.type = WMTRenderCommandSetVisibilityMode;
      cmd.mode = mode;
      cmd.offset = 0;
    };
    barrier(WMTRenderStageVertex | WMTRenderStageFragment | WMTRenderStageObject | WMTRenderStageMesh,
            WMTRenderStageVertex);
    if (!active_queries_.empty()) visibility(WMTVisibilityResultModeDisabled);
    auto &resolver = allocator_->EncodeRenderCommand<wmtcmd_render_setpso>();
    resolver.type = WMTRenderCommandSetPSO;
    resolver.pso = sig->mesh_resolver;
    bind(WMTRenderCommandSetVertexBuffer, dispatch, 0, 30);
    auto &prepare = allocator_->EncodeRenderCommand<wmtcmd_render_draw>();
    prepare.type = WMTRenderCommandDraw;
    prepare.primitive_type = WMTPrimitiveTypePoint;
    prepare.vertex_start = 0;
    prepare.vertex_count = max_count;
    prepare.base_instance = 0;
    prepare.instance_count = 1;
    barrier(WMTRenderStageVertex, WMTRenderStageObject | WMTRenderStageMesh);
    bind(WMTRenderCommandSetVertexBuffer, {}, 0, 30);
    dirty_state_.set(DirtyState::GraphicsRootSignature);
    dirty_state_.set(DirtyState::GraphicsRootArguments);
    dirty_state_.set(DirtyState::VertexBuffer);
    // Flush the dirtied stage bindings before the first dependent mesh draw.
    dirty_state_.set(DirtyState::GraphicsPipelineState);
    if (PreDraw(false, variant) != status) return false;
    if (!active_queries_.empty()) visibility(WMTVisibilityResultModeCounting);
    if (indexed) bind(WMTRenderCommandSetObjectBuffer, index_buffer, index_offset, SM50_BINDING_INDEX_INDEX_BUFFER);
    for (uint32_t i = 0; i < max_count; ++i) {
      const uint64_t record = kRecordsOffset + uint64_t(i) * kRecordBytes;
      bind(WMTRenderCommandSetObjectBuffer, dispatch, record + kDrawOffset, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      auto &draw = allocator_->EncodeRenderCommand<wmtcmd_render_draw_meshthreadgroups_indirect>();
      draw.type = WMTRenderCommandDrawMeshThreadgroupsIndirect;
      draw.indirect_args_buffer = dispatch;
      draw.indirect_args_offset = record;
      draw.object_threadgroup_size = geometry ? WMTSize{threads, 1, 1} : WMTSize{threads, primitives_per_group, 1};
      draw.mesh_threadgroup_size = WMTSize{geometry ? 1u : 32u, 1, 1};
    }
    bind(WMTRenderCommandSetObjectBuffer, allocator_->gpu_heap_buffer_, 0, SM50_BINDING_INDEX_DRAW_ARGUMENTS);
    return true;
  }
