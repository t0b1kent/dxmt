    // POSTCS 03.11 (d3d12_post_chain.hpp; from POST-CS v3 post-cs-pso.inc 25.10): the compute variant of a qualifying post-process pipeline - the
    // kernel (pixel shader body as compute) and the uv-map pipeline (same vertex function + a fragment function with the
    // pixel shader's input declarations). Included in MTLD3D12GraphicsPipelineStateImpl::Initialize after the render
    // pipeline, depth/stencil and rasterizer state exist. A pipeline that does not qualify keeps post_cs empty.
    if (post_chain::Armed() && !mesh_pipeline && !translated.source && pso && ps_func && pDesc->PS.pShaderBytecode &&
        pDesc->PS.BytecodeLength) {
      post_chain::Note(post_chain::Get().pso_candidates);
      char ps_id[24];
      {
        const auto sha = pso_cache::PipelineCache::ShaderId(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);
        std::snprintf(ps_id, sizeof(ps_id), "%s", sha.string().substr(0, 16).c_str());
      }
      post_chain_ps = std::strtoull(ps_id, nullptr, 16);   // PCS2 03.11 diagnostics
      const char *why = nullptr;
      const auto &d = *pDesc;
      const auto &b = d.BlendState.RenderTarget[0];
      const auto target = ORIGINAL_FORMAT(info.colors[0].pixel_format);
      bool per_instance = false;
      for (UINT i = 0; i < d.InputLayout.NumElements && d.InputLayout.pInputElementDescs; ++i)
        per_instance |= d.InputLayout.pInputElementDescs[i].InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      if (d.NumRenderTargets != 1) why = "render_targets";
      else if (!post_chain::KernelFormat(target)) why = "target_format";
      else if (d.DSVFormat != DXGI_FORMAT_UNKNOWN) why = "depth_target";
      else if (d.SampleDesc.Count != 1 || (d.SampleMask & 1) == 0) why = "samples";
      else if (d.BlendState.AlphaToCoverageEnable) why = "alpha_to_coverage";
      else if (b.BlendEnable || b.LogicOpEnable) why = "blend";
      else if ((b.RenderTargetWriteMask & 15) != 15) why = "write_mask";
      else if (d.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) why = "topology";
      else if (d.RasterizerState.FillMode != D3D12_FILL_MODE_SOLID ||
               d.RasterizerState.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF ||
               d.RasterizerState.ForcedSampleCount)
        why = "rasterizer";
      else if (d.GS.pShaderBytecode || d.HS.pShaderBytecode || d.DS.pShaderBytecode) why = "stages";
      else if (d.StreamOutput.NumEntries || d.StreamOutput.NumStrides) why = "stream_output";
      else if (per_instance) why = "per_instance_input";
      else if (!d.pRootSignature) why = "root_signature";
      else if (FAILED(EnsureShader(d.VS, &shader_vs)) || FAILED(EnsureShader(d.PS, &shader_ps))) why = "shader_init";
      auto variant = std::make_shared<post_chain::Variant>();
      // The vertex shader's content key: what its outputs depend on.
      SM50_SHADER_POST_CS_VS_KEY_DATA vs_key{};
      if (!why) {
        vs_key.type = SM50_SHADER_POST_CS_VS_KEY_QUERY;
        SM50ShaderBitcode none;
        SM50Error query_error;
        if (PSO_XLAT(SM50Compile(shader_vs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&vs_key, "pcs_vs_key", &none, &query_error)) ||
            vs_key.applied != SM50_POST_CS_APPLIED)
          why = "vs_key_query";
        else if (!vs_key.ok)
          why = vs_key.reason[0] ? vs_key.reason : "vs_key";
      }
      // Can a draw prove its coverage on the CPU? SV_Position x, y, w from the vertex buffer or literals, z also from a keyed
      // constant row; the position input element read as float32 components at an explicit offset.
      uint32_t coverage = 1, pos_slot = 0, pos_offset = 0, pos_components = 0;
      if (!why) {
        bool provable = true;
        for (unsigned c = 0; c < 4; ++c) {
          const uint32_t k = vs_key.pos_kind[c];
          provable &= k == SM50_POST_CS_POS_INPUT || k == SM50_POST_CS_POS_LITERAL || (c == 2 && k == SM50_POST_CS_POS_CB);
        }
        bool needs_input = false;
        for (unsigned c = 0; c < 4; ++c) needs_input |= vs_key.pos_kind[c] == SM50_POST_CS_POS_INPUT;
        bool found = !needs_input;
        for (UINT i = 0; provable && needs_input && i < d.InputLayout.NumElements; ++i) {
          const auto &e = d.InputLayout.pInputElementDescs[i];
          if (!e.SemanticName || _stricmp(e.SemanticName, vs_key.pos_semantic) || e.SemanticIndex != vs_key.pos_semantic_index) continue;
          const uint32_t n = e.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ? 4 : e.Format == DXGI_FORMAT_R32G32B32_FLOAT ? 3 :
                             e.Format == DXGI_FORMAT_R32G32_FLOAT ? 2 : e.Format == DXGI_FORMAT_R32_FLOAT ? 1 : 0;
          if (n && e.AlignedByteOffset != D3D12_APPEND_ALIGNED_ELEMENT && e.InputSlot < 32) {
            found = true; pos_slot = e.InputSlot; pos_offset = e.AlignedByteOffset; pos_components = n;
          }
          break;
        }
        if (provable && found) coverage = 0;
      }
      SM50_SHADER_ROOT_SIGNATURE_DATA pcs_root{};
      if (!why) {
        pcs_root.type = SM50_SHADER_ROOT_SIGNATURE;
        pcs_root.bytecode_length = static_cast<MTLD3D12RootSignature *>(d.pRootSignature)->GetBlob(&pcs_root.bytecode);
        pcs_root.next = &common;
      }
      // One compile of the pixel shader per mode; the map layout must agree between the two.
      const auto compile = [&](uint32_t mode, const std::string &name, SM50_SHADER_PS_AS_COMPUTE_DATA &node,
                               WMT::Reference<WMT::Function> &function) -> const char * {
        node = {};
        node.type = SM50_SHADER_PS_AS_COMPUTE;
        node.mode = mode;
        node.target_format = uint32_t(target);
        node.coverage = coverage;
        node.next = &pcs_root;
        SM50ShaderBitcode bitcode;
        SM50Error compile_error;
        if (PSO_XLAT(SM50Compile(shader_ps, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&node, name.c_str(), &bitcode, &compile_error)) ||
            !sm50_bitcode_t(bitcode) || node.applied != SM50_POST_CS_APPLIED) {
          static thread_local char reason[160];
          std::snprintf(reason, sizeof(reason), "%s:%s", mode == SM50_POST_CS_KERNEL ? "kernel" : "uvmap",
                        SM50GetErrorMessageString(compile_error).c_str());
          for (char *c = reason; *c; ++c) if (*c == ' ' || *c == '\n') *c = '_';
          return reason;
        }
        SM50_COMPILED_BITCODE compiled = {};
        SM50GetCompiledBitcode(bitcode, &compiled);
        if (!compiled.Data || !compiled.Size) return "empty_bitcode";
        WMT::Reference<WMT::Error> lib_err;
        auto library = PSO_BACKEND(metal.newLibrary(WMT::MakeDispatchData(compiled.Data, compiled.Size), lib_err));
        if (!library) return mode == SM50_POST_CS_KERNEL ? "kernel_library" : "uvmap_library";
        function = PSO_BACKEND(library.newFunction(name.c_str()));
        return function ? nullptr : "function";
      };
      SM50_SHADER_PS_AS_COMPUTE_DATA kernel_node, uvmap_node;
      WMT::Reference<WMT::Function> kernel_func, uvmap_func;
      const std::string suffix = std::string(ps_id).substr(0, 8) + "_" + std::to_string(unsigned(target));
      if (!why) why = compile(SM50_POST_CS_KERNEL, "pcsk_" + suffix, kernel_node, kernel_func);
      if (!why) why = compile(SM50_POST_CS_UVMAP, "pcsu_" + suffix, uvmap_node, uvmap_func);
      if (!why && (kernel_node.map_count != uvmap_node.map_count || kernel_node.cov_comp != uvmap_node.cov_comp ||
                   kernel_node.map_format != uvmap_node.map_format ||
                   std::memcmp(kernel_node.map_reg, uvmap_node.map_reg, sizeof(kernel_node.map_reg))))
        why = "map_layout_mismatch";
      if (!why) {
        WMTComputePipelineInfo cinfo;
        WMT::InitializeComputePipelineInfo(cinfo);
        cinfo.compute_function = kernel_func;
        WMT::Reference<WMT::Error> cerr;
        variant->kernel = PSO_BACKEND(metal.newComputePipelineState(cinfo, cerr));
        if (!variant->kernel) why = "kernel_pipeline";
      }
      if (!why) {
        auto uinfo = info;
        for (unsigned i = 0; i < 8; ++i) {
          uinfo.colors[i] = {};
          if (i < uvmap_node.map_count) {
            uinfo.colors[i].pixel_format = uvmap_node.map_format == 2 ? WMTPixelFormatRG32Float : WMTPixelFormatRGBA32Float;
            uinfo.colors[i].write_mask = 15;
          }
        }
        uinfo.alpha_to_coverage_enabled = false;
        uinfo.logic_operation_enabled = false;
        uinfo.raster_sample_count = 1;
        uinfo.depth_pixel_format = WMTPixelFormatInvalid;
        uinfo.stencil_pixel_format = WMTPixelFormatInvalid;
        uinfo.fragment_function = uvmap_func.handle;
        uinfo.binary_archive_for_serialization = 0;
        uinfo.binary_archives_for_lookup.set(nullptr);
        uinfo.num_binary_archives_for_lookup = 0;
        uinfo.fail_on_binary_archive_miss = false;
        WMT::Reference<WMT::Error> uerr;
        variant->uvmap = PSO_BACKEND(metal.newRenderPipelineState(uinfo, uerr));
        if (!variant->uvmap) why = "uvmap_pipeline";
      }
      std::snprintf(variant->ps, sizeof(variant->ps), "%s", ps_id);
      variant->target_dxgi = uint32_t(d.RTVFormats[0]);
      if (!why) {
        variant->id = post_chain::NextVariantId();
        variant->dss = GetDepthStencilState(0, 0).handle;
        variant->target_format = target;
        variant->map_count = kernel_node.map_count;
        std::memcpy(variant->map_reg, kernel_node.map_reg, sizeof(variant->map_reg));
        std::memcpy(variant->map_mask, kernel_node.map_mask, sizeof(variant->map_mask));
        variant->cov_comp = kernel_node.cov_comp;
        variant->row_count = vs_key.row_count;
        std::memcpy(variant->rows, vs_key.rows, sizeof(variant->rows));
        variant->coverage = coverage;
        variant->map_format = kernel_node.map_format;
        std::memcpy(variant->pos_kind, vs_key.pos_kind, sizeof(variant->pos_kind));
        std::memcpy(variant->pos_a, vs_key.pos_a, sizeof(variant->pos_a));
        std::memcpy(variant->pos_b, vs_key.pos_b, sizeof(variant->pos_b));
        std::memcpy(variant->pos_value, vs_key.pos_value, sizeof(variant->pos_value));
        variant->pos_slot = pos_slot;
        variant->pos_offset = pos_offset;
        variant->pos_components = pos_components;
        variant->cull = uint32_t(d.RasterizerState.CullMode);
        variant->front_ccw = d.RasterizerState.FrontCounterClockwise ? 1 : 0;
        variant->depth_clip = d.RasterizerState.DepthClipEnable ? 1 : 0;
        variant->slot_mask = slot_mask;
        variant->r_cull = cull_mode; variant->r_depth_clip = depth_clip_mode; variant->r_fill = fill_mode;
        variant->r_bias = depth_bias; variant->r_bias_clamp = depth_bias_clamp; variant->r_slope = scole_scale; variant->r_winding = winding;
        post_chain_variant = std::move(variant);
        post_chain::Note(post_chain::Get().pso_ready);
      } else {
        post_chain::Refused(ps_id, why);   // a pipeline of the post-process shape that does not qualify: no variant
        post_chain_why = post_chain::WhyOfPipeline(why);
      }
    }
