  // POSTCS 03.11.2026 (d3d12_post_chain.hpp): included in MTLD3D12GraphicsCommandListImpl. Recording side of
  // MACRUNNER_DX12_POST_CHAIN: a candidate draw leaves a POD side record in its pass; nothing else changes in the recording.

  // The setbuffer commands PreDraw emitted for the CURRENT render pass (reset whenever PreDraw opens a pass).
  const wmtcmd_render_setbuffer *pc_fs_root_ = nullptr, *pc_vs_root_ = nullptr, *pc_fs_samplers_ = nullptr,
                                *pc_vs_samplers_ = nullptr, *pc_vs_vb_ = nullptr;
  const EncoderData *pc_root_pass_ = nullptr, *pc_sampler_pass_ = nullptr, *pc_vb_pass_ = nullptr;
  void PostChainForget() {
    pc_fs_root_ = pc_vs_root_ = pc_fs_samplers_ = pc_vs_samplers_ = pc_vs_vb_ = nullptr;
    pc_root_pass_ = pc_sampler_pass_ = pc_vb_pass_ = nullptr;
  }

  // After an ordinary DrawInstanced was recorded (its PreDraw emitted the pass's state): the side record, if the draw qualifies.
  void PostChainRecord(UINT vertex_count, UINT instance_count, UINT start_vertex, UINT start_instance, WMTPrimitiveType primitive) {
    if (!allocator_->encoder_current || allocator_->encoder_current->type != EncoderType::Render) return;
    auto *pass = static_cast<RenderEncoderData *>(allocator_->encoder_current);
    auto *v = pso_graphics_ ? pso_graphics_->post_chain_variant.get() : nullptr;
    // PCS2 03.11: why this draw gives the pass no record (the stop lines name it)
    pass->post_chain_ps = pso_graphics_ ? pso_graphics_->post_chain_ps : 0;
#define PC_WHY(code) do { pass->post_chain_why = uint8_t(post_chain::code); return; } while (0)
    if (!v || !v->kernel) {
      if (pso_graphics_ && pso_graphics_->post_chain_why) { pass->post_chain_why = pso_graphics_->post_chain_why; return; }
      PC_WHY(WhyNoVariant);
    }
    auto &st = post_chain::Get();
    if (pass->post_chain_draws != 1) { post_chain::Note(st.draw_multi); PC_WHY(WhyDraws); }
    if (instance_count != 1 || start_instance || !vertex_count) PC_WHY(WhyInstances);
    if (topology_ != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topology_ != D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) PC_WHY(WhyTopology);
    if (num_rtvs != 1 || !rtvs[0].ptr || dsv.ptr || num_viewports != 1 || !active_queries_.empty()) PC_WHY(WhyBinding);
    if (!pass->visibility_targets.empty() || pass->visibility_direct || pass->ldc_probe) PC_WHY(WhyVisibility);
    if (!rootsig_graphics_ || rootsig_graphics_->reuse_parameters.empty() || rootsig_graphics_->UploadQwords > 64) PC_WHY(WhyRootSize);
    if (pc_root_pass_ != pass || !pc_fs_root_ || !pc_vs_root_) PC_WHY(WhyRootCmds);
    if (rootsig_graphics_->NumStaticSamplers && (pc_sampler_pass_ != pass || !pc_fs_samplers_ || !pc_vs_samplers_)) PC_WHY(WhySamplerCmds);
    if (v->slot_mask && (pc_vb_pass_ != pass || !pc_vs_vb_)) PC_WHY(WhyVbCmd);
    auto [rt_heap, rt_index] = GetRenderTargetHeap(device_, rtvs[0]);
    const auto rt = rt_heap->GetRenderTarget(rt_index);
    if (!rt.Texture || rt.DepthPlane || rt.RenderTargetArrayLength > 1 || rt.Texture->sampleCount() != 1 ||
        rt.Texture->textureType(rt.View) != WMTTextureType2D || !rt.Width || !rt.Height ||
        !(rt.Texture->usage() & WMTTextureUsageShaderWrite) || ORIGINAL_FORMAT(rt.Texture->pixelFormat(rt.View)) != v->target_format)
      PC_WHY(WhyTarget);
    // Pixels the pass can touch: target, scissor (as the render path clamps it), viewport bounds.
    const D3D12_VIEWPORT vp = viewports[0];
    int64_t x0 = 0, y0 = 0, x1 = rt.Width, y1 = rt.Height;
    D3D12_RECT sc = {0, 0, 16384, 16384};
    if (num_scissors) {
      sc.left = std::clamp(scissors[0].left, (LONG)0, (LONG)16384);
      sc.top = std::clamp(scissors[0].top, (LONG)0, (LONG)16384);
      sc.right = std::clamp(scissors[0].right, sc.left, (LONG)16384);
      sc.bottom = std::clamp(scissors[0].bottom, sc.top, (LONG)16384);
    }
    x0 = std::max<int64_t>(x0, sc.left); y0 = std::max<int64_t>(y0, sc.top);
    x1 = std::min<int64_t>(x1, sc.right); y1 = std::min<int64_t>(y1, sc.bottom);
    x0 = std::max<int64_t>(x0, int64_t(std::floor(vp.TopLeftX))); y0 = std::max<int64_t>(y0, int64_t(std::floor(vp.TopLeftY)));
    x1 = std::min<int64_t>(x1, int64_t(std::ceil(vp.TopLeftX + vp.Width)));
    y1 = std::min<int64_t>(y1, int64_t(std::ceil(vp.TopLeftY + vp.Height)));
    if (x1 <= x0 || y1 <= y0) PC_WHY(WhyRect);
    auto *d = allocator_->AllocateCommandData<post_chain::Draw>(1);
    new (d) post_chain::Draw();
    d->variant = v;
    d->vertex_count = vertex_count; d->start_vertex = start_vertex; d->primitive = primitive; d->topology = uint32_t(topology_);
    d->fs_root = pc_fs_root_; d->vs_root = pc_vs_root_; d->fs_samplers = pc_fs_samplers_; d->vs_samplers = pc_vs_samplers_;
    d->vs_vb = pc_vs_vb_;
    d->viewport = vp; d->scissor = sc;
    d->x0 = int32_t(x0); d->y0 = int32_t(y0); d->x1 = int32_t(x1); d->y1 = int32_t(y1);
    d->width = rt.Width; d->height = rt.Height;
    d->target = rt.Texture->view(rt.View).texture.handle;
    // the vertex stage's inputs, as VAs (their bytes are read at encoding): vertex buffers of the keyed slots, constant rows
    d->slot_mask = v->slot_mask;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!(v->slot_mask & (1u << slot))) continue;
      const auto &view = vertex_buffers_[slot];
      const uint64_t stride = view.StrideInBytes, first = uint64_t(start_vertex) * stride, length = uint64_t(vertex_count) * stride;
      if (!view.BufferLocation || !stride || first + length > view.SizeInBytes) PC_WHY(WhyVbCmd);
      d->vb_va[slot] = view.BufferLocation + first; d->vb_stride[slot] = uint32_t(stride); d->vb_size[slot] = uint32_t(length);
    }
    const auto &layout = rootsig_graphics_->ArgumentLayout;
    for (uint32_t r = 0; r < v->row_count; ++r) {
      const auto row = v->rows[r];
      bool found = false;
      for (uint32_t p = 0; p < rootsig_graphics_->reuse_parameters.size() && !found; ++p) {
        const auto &param = rootsig_graphics_->reuse_parameters[p];
        if (param.visibility != D3D12_SHADER_VISIBILITY_ALL && param.visibility != D3D12_SHADER_VISIBILITY_VERTEX) continue;
        if (param.kind == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
          for (const auto &range : param.ranges) {
            if (range.type != D3D12_DESCRIPTOR_RANGE_TYPE_CBV || range.space != row.space || row.reg < range.binding ||
                (range.count != UINT_MAX && row.reg - range.binding >= range.count))
              continue;
            root_argument::TableLocation location;
            if (bound_heap_ranges_[0].type != 0 || !bound_heaps_[0] ||
                !root_argument::ReadTable(layout, rootarg_graphics_staging_, p, bound_heap_ranges_[0],
                                          range.offset + (row.reg - range.binding), 1, location))
              PC_WHY(WhyCb);
            const auto &descriptor = static_cast<MTLD3D12DescriptorHeap *>(bound_heaps_[0].ptr())->GetDescriptor(location.index);
            if (descriptor.type != ShaderVisibleDescriptorType::ConstantBuffer || uint64_t(row.row) * 16 + 16 > descriptor.ConstantBuffer.size)
              PC_WHY(WhyCb);
            d->row_va[r] = descriptor.ConstantBuffer.address + uint64_t(row.row) * 16;
            found = true;
            break;
          }
        } else if (param.kind == D3D12_ROOT_PARAMETER_TYPE_CBV && param.binding == row.reg && param.space == row.space) {
          uint64_t va = 0;
          if (!rootarg_graphics_staging_.ReadValue(layout, p, root_argument::CBV, va) || !va) PC_WHY(WhyCb);
          d->row_va[r] = va + uint64_t(row.row) * 16;
          found = true;
        } else if (param.kind == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && param.binding == row.reg && param.space == row.space) {
          const auto &lp = layout.parameters[p];
          if (!rootarg_graphics_staging_.ConstantsReady(layout, p) || row.row * 4 >= lp.count) PC_WHY(WhyCb);
          const uint32_t n = std::min<uint32_t>(4, lp.count - row.row * 4);
          std::memcpy(d->row_bytes[r], reinterpret_cast<const uint8_t *>(rootarg_graphics_staging_.words.data() + lp.qword) + row.row * 16, n * 4);
          d->row_const_mask |= 1u << r;
          found = true;
        }
      }
      if (!found) PC_WHY(WhyCb);
    }
    pass->post_chain = d;
    pass->post_chain_why = 0;
    post_chain::Note(st.draws);
#undef PC_WHY
  }
