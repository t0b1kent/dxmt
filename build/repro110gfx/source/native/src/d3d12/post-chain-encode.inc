    // POSTCS 03.11.2026 (d3d12_post_chain.hpp): included in EncodeIntoScope after the encoder helpers. A run of >= MIN_RUN
    // candidate post-process passes (consecutive, across the lists of this call, only plain transition barriers between them)
    // is encoded as ONE compute encoder at the place of its first pass; its other passes (and the barrier / null passes between
    // them) are skipped by the main loop (pc_consumed). Shorter runs stay render passes (their passes are remembered in
    // pc_rejected so a run is judged once). The key is read once per encoding; early (Close-time) encodings never convert.
    const unsigned pc_key = [&]() -> unsigned {
      if (!post_chain::Armed()) return 0u;
      post_chain::Note(post_chain::Get().encodings);
      if (early) { post_chain::Note(post_chain::Get().early); return 0u; }
      if (post_chain::Disabled() || scope.inflight.fault_capture || FrameReadbackEnabled() || FullFrameCaptureEnabled() ||
          reuse::Enabled() || merge_trace::Target() || tess_prepass::Verify() || enc_census::Enabled() || xl_hazard::Enabled())
        return 0u;
      return post_chain::Key();
    }();
    std::unordered_set<const EncoderData *> pc_consumed, pc_rejected;
    // A pass that may stand in a run (its own properties only).
    const auto pc_candidate = [&](const RenderEncoderData &r) -> bool {
      const auto *d = r.post_chain;
      if (!d || r.post_chain_draws != 1 || !d->variant || !d->variant->kernel) return false;
      if (!r.visibility_targets.empty() || r.visibility_direct || r.ldc_probe || r.prepass_tail || r.use_tessellation || r.use_geometry ||
          !r.mesh_indirect_buffers.empty() || r.verify_mesh_draw || r.twin_draw || r.may_write_memory)
        return false;
      if (r.render_target_count != 1 || !r.colors[0].attachment || r.colors[0].resolve_attachment || r.depth.attachment ||
          r.stencil.attachment)
        return false;
      for (unsigned k = 1; k < r.colors.size(); ++k) if (r.colors[k].attachment) return false;
      return true;
    };
    // A blit pass a run may contain: a barrier-only pass, which the barrier path skips (it encodes nothing there: no OUT write is
    // pending - checked at the run's start - and nothing in a run writes one). Its records matter only to the bookkeeping the run
    // repeats when it consumes it (alias_join, typed_exposed, the pending clears of every object it names - all of them when it
    // names none or has no records). Memory barriers between the dispatches order everything inside the run.
    // 03.11 PCS1: the first rule (only RT / UAV / shader-state transitions of named resources, no flags) stopped every ER post
    // chain (stop_barrier 35 per frame, runs 0); it stays as MACRUNNER_DX12_POST_CHAIN_STRICT_BARRIERS=1, and what a barrier has
    // beyond it is counted (bx_*) and printed (dx12_post_chain_barrier, first 32).
    // Returns the stop counter, or nullptr when the pass may stand in a run.
    const auto pc_blit_stop = [&](const BlitEncoderData &b) -> std::atomic<uint64_t> * {
      auto &st = post_chain::Get();
      if (b.timestamp_samples || b.resolve_samples) return &st.stop_query;
      if (b.wait_for_prior_work || b.immediate_out) return &st.stop_marker;
      if (!b.barrier_only || b.cmd_head.next.ptr) return &st.stop_copy;
      constexpr uint32_t allowed = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      unsigned extra = b.barrier_records_valid ? 0u : 1u;
      for (const auto &rec : b.barrier_records) {
        if (rec.flags) extra |= 2u;
        if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_UAV) continue;
        if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) { extra |= 4u; continue; }
        if (rec.type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) { extra |= 8u; continue; }
        if (!rec.object) extra |= 16u;
        if (!rec.before || !rec.after) extra |= 32u;
        else if ((rec.before & ~allowed) || (rec.after & ~allowed)) extra |= 64u;
      }
      post_chain::Note(st.bx_passes);
      if (!extra) { post_chain::Note(st.bx_strict); return nullptr; }
      if (extra & 1u) post_chain::Note(st.bx_invalid);
      if (extra & 2u) post_chain::Note(st.bx_flags);
      if (extra & 4u) post_chain::Note(st.bx_alias);
      if (extra & 8u) post_chain::Note(st.bx_type);
      if (extra & 16u) post_chain::Note(st.bx_noobj);
      if (extra & 32u) post_chain::Note(st.bx_common);
      if (extra & 64u) post_chain::Note(st.bx_states);
      if ((post_chain::E().stats || post_chain::E().trace) && post_chain::Get().barriers_printed.load(std::memory_order_relaxed) < 32) {
        char rows[512]; int at = 0; size_t n = 0;
        for (const auto &rec : b.barrier_records) {
          if (++n > 8 || at < 0 || size_t(at) >= sizeof(rows)) break;
          at += std::snprintf(rows + at, sizeof(rows) - at, "%s%u/%u/%x/%x%s", n > 1 ? "," : "", unsigned(rec.type),
                              unsigned(rec.flags), rec.before, rec.after, rec.object ? "" : "/o0");
        }
        if (at <= 0) rows[0] = 0;
        post_chain::BarrierSeen(extra, b.barrier_records_valid, b.barrier_records.size(), rows);
      }
      return post_chain::E().strict_barriers ? &st.stop_barrier : nullptr;
    };
    // Has list `li` a pass whose tessellation pre-pass the scheduler may move to another pass (its schedule is made when the loop
    // reaches the list: a run must not swallow the pass such a pre-pass would be encoded at).
    // 03.11 06:xx: one walk per list per encoding (the scans of a frame cross the same lists again and again)
    std::vector<int8_t> pc_prepass_memo(pc_key ? Count : 0, int8_t(-1));
    const auto pc_list_has_prepass = [&](UINT li) {
      if (li < pc_prepass_memo.size() && pc_prepass_memo[li] >= 0) return pc_prepass_memo[li] != 0;
      bool has = false;
      for (auto p = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[li])->entry; p && !has; p = p->next)
        if (p->type == EncoderType::Render && static_cast<RenderEncoderData *>(p)->prepass_tail) has = true;
      if (li < pc_prepass_memo.size()) pc_prepass_memo[li] = has ? 1 : 0;
      return has;
    };
    const auto pc_pixels = [](const post_chain::Draw &d) { return uint64_t(d.x1 - d.x0) * uint64_t(d.y1 - d.y0); };
    struct PcMember {
      RenderEncoderData *pass;
      std::shared_ptr<post_chain::MapEntry> map;
      bool render_map = false;
      SM50_POST_CS_PARAMS params{};
      obj_handle_t sources[SM50_POST_CS_MAX_SOURCES] = {};
      uint64_t source_offsets[SM50_POST_CS_MAX_SOURCES] = {};
      std::vector<EncoderData *> before;   // null / barrier passes between the previous member and this one (consumed with it)
    };
    // The content key, the uv-map entry, the witness words and (when the maps carry no coverage) the coverage proof of a member;
    // false = the pass cannot be converted now (reason counted).
    const auto pc_prepare = [&](PcMember &m) -> bool {
      auto &st = post_chain::Get();
      // PCX 03.11: where the preparation goes (ns per frame): buffer lookups by VA, the key (hash, witness, coverage), the map cache
      static const double pp_ns = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart ? 1e9 / double(f.QuadPart) : 0.0; }();
      LARGE_INTEGER p0{}; QueryPerformanceCounter(&p0);
      int64_t lookup_ticks = 0; uint64_t lookups = 0;
      struct PcPrepTimes {
        LARGE_INTEGER &p0; int64_t &lookup_ticks; uint64_t &lookups; std::atomic<uint64_t> &n_lookup, &n_key, &n_count, &n_qpc; double ns;
        LARGE_INTEGER f0{}; bool found = false;
        ~PcPrepTimes() {
          LARGE_INTEGER e{}; QueryPerformanceCounter(&e);
          const int64_t total = (found ? f0.QuadPart : e.QuadPart) - p0.QuadPart;
          n_lookup.fetch_add(uint64_t(double(lookup_ticks) * ns), std::memory_order_relaxed);
          n_key.fetch_add(uint64_t(double(std::max<int64_t>(0, total - lookup_ticks)) * ns), std::memory_order_relaxed);
          n_count.fetch_add(lookups, std::memory_order_relaxed);
          n_qpc.fetch_add(2 * lookups + 2 + (found ? 2 : 0), std::memory_order_relaxed);
        }
      } pp{p0, lookup_ticks, lookups, st.prep_lookup_ns, st.prep_key_ns, st.prep_lookups, st.qpc_calls, pp_ns};
      const auto &d = *m.pass->post_chain;
      auto &v = *d.variant;
      post_chain::MapKey key;
      key.value(v.id);
      key.value(d.width); key.value(d.height);
      key.value(d.viewport); key.value(d.scissor);
      key.value(d.topology); key.value(d.vertex_count); key.value(d.start_vertex);
      uint32_t source_count = 0, words = 0;
      std::memset(&m.params, 0, sizeof(m.params));
      const auto witness = [&](const Rc<BufferAllocation> &allocation, uint64_t offset, const uint8_t *bytes, uint64_t length) {
        if ((offset & 3) || (length & 3) || source_count == SM50_POST_CS_MAX_SOURCES ||
            words + length / 4 > SM50_POST_CS_MAX_WITNESS_WORDS) return false;
        std::memcpy(m.params.expect + words, bytes, length);
        m.params.source_words[source_count] = uint32_t(length / 4);
        m.sources[source_count] = allocation->buffer().handle;
        m.source_offsets[source_count] = offset;
        ++source_count;
        words += uint32_t(length / 4);
        return true;
      };
      const uint8_t *slot_bytes[32] = {};
      for (uint32_t slot = 0; slot < 32; ++slot) {
        if (!(d.slot_mask & (1u << slot))) continue;
        uint64_t offset = 0;
        LARGE_INTEGER l0{}, l1{}; QueryPerformanceCounter(&l0);
        auto allocation = device_->LookupBufferByVA(d.vb_va[slot], d.vb_size[slot], &offset);
        QueryPerformanceCounter(&l1); lookup_ticks += l1.QuadPart - l0.QuadPart; ++lookups;
        if (!allocation || !allocation->hasMappedMemory()) { post_chain::Note(st.r_vb); return false; }
        const auto *bytes = static_cast<const uint8_t *>(allocation->mappedMemory(0)) + offset;
        key.value(slot); key.value(d.vb_stride[slot]); key.put(bytes, d.vb_size[slot]);
        slot_bytes[slot] = bytes;
        if (!witness(allocation, offset, bytes, d.vb_size[slot])) { post_chain::Note(st.r_witness); return false; }
      }
      uint8_t rowbytes[SM50_POST_CS_VS_MAX_ROWS][16] = {};
      for (uint32_t r = 0; r < v.row_count; ++r) {
        key.value(v.rows[r]);
        if (d.row_const_mask & (1u << r)) {   // root constants: baked into the recording, nothing to witness
          key.put(d.row_bytes[r], 16);
          std::memcpy(rowbytes[r], d.row_bytes[r], 16);
          continue;
        }
        uint64_t offset = 0;
        LARGE_INTEGER l0{}, l1{}; QueryPerformanceCounter(&l0);
        auto allocation = device_->LookupBufferByVA(d.row_va[r], 16, &offset);
        QueryPerformanceCounter(&l1); lookup_ticks += l1.QuadPart - l0.QuadPart; ++lookups;
        if (!allocation || !allocation->hasMappedMemory()) { post_chain::Note(st.r_cb); return false; }
        const auto *bytes = static_cast<const uint8_t *>(allocation->mappedMemory(0)) + offset;
        key.put(bytes, 16);
        std::memcpy(rowbytes[r], bytes, 16);
        if (!witness(allocation, offset, bytes, 16)) { post_chain::Note(st.r_witness); return false; }
      }
      if (!v.coverage) {   // the maps carry no coverage: this draw must cover every pixel of the rectangle
        const uint8_t *vertex_bytes[3] = {};
        bool ok = d.vertex_count == 3 && slot_bytes[v.pos_slot];
        for (unsigned k = 0; ok && k < 3; ++k) vertex_bytes[k] = slot_bytes[v.pos_slot] + uint64_t(k) * d.vb_stride[v.pos_slot];
        if (!ok || !post_chain::Covered(v, vertex_bytes, rowbytes, d.viewport, d.x0, d.y0, d.x1, d.y1)) {
          post_chain::Note(st.r_coverage); return false;
        }
      }
      if (v.maps_created.load(std::memory_order_relaxed) >= post_chain::Variant::kMaxMaps) { post_chain::Note(st.r_churn); return false; }
      bool ready = false, created = false;
      QueryPerformanceCounter(&pp.f0); pp.found = true;
      m.map = post_chain::Cache::Instance().Find(key, d.width, d.height, v.map_count, v.map_format, device_, queue_id_, ready, created);
      { LARGE_INTEGER f1{}; QueryPerformanceCounter(&f1); post_chain::Note(st.prep_find_ns, uint64_t(double(f1.QuadPart - pp.f0.QuadPart) * pp_ns)); }
      if (!m.map) { post_chain::Note(st.r_cache); return false; }
      if (created) { v.maps_created.fetch_add(1, std::memory_order_relaxed); post_chain::Note(st.maps_created); }
      m.render_map = !ready;
      m.params.origin_x = uint32_t(d.x0);
      m.params.origin_y = uint32_t(d.y0);
      m.params.source_count = source_count;
      m.params.witness_id = uint32_t(v.id);
      return true;
    };
    // The uv-map render pass of a member (the vertex stage of the pass with the uv-map fragment function, maps cleared to 0).
    const auto pc_render_map = [&](const PcMember &m) {
      const auto &d = *m.pass->post_chain;
      const auto &v = *d.variant;
      WMTRenderPassInfo info;
      WMT::InitializeRenderPassInfo(info);
      for (uint32_t i = 0; i < v.map_count; ++i) {
        info.colors[i].texture = m.map->maps[i]->view(m.map->maps[i]->fullView).texture.handle;
        info.colors[i].load_action = WMTLoadActionClear;
        info.colors[i].store_action = WMTStoreActionStore;
        info.colors[i].clear_color = {0.0, 0.0, 0.0, 0.0};
      }
      info.render_target_width = d.width;
      info.render_target_height = d.height;
      wmtcmd_render_nop head{}; head.type = WMTRenderCommandNop;
      wmtcmd_render_setpso pso{}; pso.type = WMTRenderCommandSetPSO; pso.pso = v.uvmap.handle;
      wmtcmd_render_setdepthstencilstate dss{}; dss.type = WMTRenderCommandSetDepthStencilState; dss.depth_stencil_state = v.dss;
      wmtcmd_render_setrasterizerstate rs{}; rs.type = WMTRenderCommandSetRasterizerState;
      rs.cull_mode = v.r_cull; rs.depth_clip_mode = v.r_depth_clip; rs.fill_mode = v.r_fill; rs.depth_bias = v.r_bias;
      rs.depth_bias_clamp = v.r_bias_clamp; rs.scole_scale = v.r_slope; rs.winding = v.r_winding;
      wmtcmd_render_setbuffer vb{}, root{}, samplers{};
      vb.type = root.type = samplers.type = WMTRenderCommandSetVertexBuffer;
      if (d.vs_vb) { vb.buffer = d.vs_vb->buffer; vb.offset = d.vs_vb->offset; vb.index = d.vs_vb->index; }
      root.buffer = d.vs_root->buffer; root.offset = d.vs_root->offset; root.index = d.vs_root->index;
      if (d.vs_samplers) { samplers.buffer = d.vs_samplers->buffer; samplers.offset = d.vs_samplers->offset; samplers.index = d.vs_samplers->index; }
      WMTViewport viewport = {d.viewport.TopLeftX, d.viewport.TopLeftY, d.viewport.Width, d.viewport.Height, d.viewport.MinDepth,
                              d.viewport.MaxDepth};
      WMTScissorRect scissor = {uint64_t(d.scissor.left), uint64_t(d.scissor.top), uint64_t(d.scissor.right - d.scissor.left),
                                uint64_t(d.scissor.bottom - d.scissor.top)};
      wmtcmd_render_setviewports vps{}; vps.type = WMTRenderCommandSetViewports; vps.viewports.set(&viewport); vps.viewport_count = 1;
      wmtcmd_render_setscissorrects scs{}; scs.type = WMTRenderCommandSetScissorRects; scs.scissor_rects.set(&scissor); scs.rect_count = 1;
      wmtcmd_render_draw draw{}; draw.type = WMTRenderCommandDraw; draw.primitive_type = d.primitive;
      draw.vertex_start = d.start_vertex; draw.vertex_count = d.vertex_count; draw.instance_count = 1; draw.base_instance = 0;
      wmtcmd_base *chain[] = {(wmtcmd_base *)&head, (wmtcmd_base *)&pso, (wmtcmd_base *)&dss, (wmtcmd_base *)&rs,
                              d.vs_vb ? (wmtcmd_base *)&vb : nullptr, (wmtcmd_base *)&root,
                              d.vs_samplers ? (wmtcmd_base *)&samplers : nullptr, (wmtcmd_base *)&vps, (wmtcmd_base *)&scs,
                              (wmtcmd_base *)&draw};
      wmtcmd_base *last = nullptr;
      for (auto *c : chain) { if (!c) continue; if (last) last->next.set(c); last = c; }
      last->next.set(nullptr);
      auto encoder = cmdbuf.renderCommandEncoder(info);
      encoder.waitForFence(state_->fence_, WMTRenderStageVertex);
      encoder.encodeCommands(&head);
      encoder.updateFence(state_->fence_, WMTRenderStageFragment);
      encoder.endEncoding();
      prev_render = true; scope_first = false;
    };
    // true: `first` (list li) started a run that is now encoded; the main loop goes on past it (its other passes are consumed).
    const auto post_chain_try = [&](UINT li, RenderEncoderData *first,
                                    const std::unordered_map<const EncoderData *, std::vector<RenderEncoderData *>> &tess_starts) -> bool {
      if (!pc_key || pc_rejected.count(first) || !pc_candidate(*first)) return false;
      if (out_pending || !gpu4_late.empty() || gpu4_late_flushing) return false;
      LARGE_INTEGER t0{}; QueryPerformanceCounter(&t0);
      auto &st = post_chain::Get();
      const auto &env = post_chain::E();
      const unsigned min_run = post_chain::MinRun(pc_key);
      // 1. the run, structurally (own properties of the passes and the barriers between them)
      std::vector<PcMember> run;
      std::vector<EncoderData *> between;
      std::unordered_map<const EncoderData *, const void *> clear_list;   // PCS2: hoisted clears and their lists
      std::unordered_set<const EncoderData *> clear_inrun;                 // PCS2: clears encoded inside the run (clear kernel)
      std::atomic<uint64_t> *stop = &st.stop_end;
      if (pc_pixels(*first->post_chain) > env.max_px) { pc_rejected.insert(first); post_chain::Note(st.stop_size); return false; }
      run.push_back({first});
      UINT l = li;
      EncoderData *p = first->next;
      for (;;) {
        if (!p) {
          if (++l >= Count) { stop = &st.stop_end; break; }
          if (pc_list_has_prepass(l)) { stop = &st.stop_tess; break; }
          p = static_cast<MTLD3D12GraphicsCommandList *>(ppCommandLists[l])->entry;
          continue;
        }
        if (l == li && tess_starts.find(p) != tess_starts.end()) { stop = &st.stop_tess; break; }
        if (p->type == EncoderType::Null) { between.push_back(p); p = p->next; continue; }
        if (p->type == EncoderType::Blit) {
          if (auto *why = pc_blit_stop(*static_cast<BlitEncoderData *>(p))) {
            stop = why;
            if (why != &st.stop_barrier) post_chain::Note(st.stop_barrier);   // stop_barrier: every blit stop (as in PCS1); split above
            break;
          }
          between.push_back(p); p = p->next; continue;
        }
        if (p->type == EncoderType::Clear) {
          // 03.11 PCS2 (HOIST_CLEARS): a clear inside the run is encoded before the run when the run cannot have seen its
          // texture: no member so far renders to it, and no barrier since the run began names it (D3D12: a shader reads a
          // texture only in a shader-resource state, entered through a barrier that names it; a clear needs RENDER_TARGET /
          // DEPTH_WRITE). Aliasing or unrecorded barriers count as naming everything (BarrierTouchesObject). It joins the
          // pending clears at the run's start, so it folds or is encoded as the barrier path would have.
          auto *c = static_cast<ClearEncoderData *>(p);
          const obj_handle_t object = AttachmentObject(c->attachment);
          bool ok = env.hoist_clears && fold_clears && object;
          for (const auto &m : run) {
            if (!ok) break;
            if (AttachmentObject(m.pass->colors[0].attachment) == object) ok = false;
            for (auto *b : m.before)
              if (b->type == EncoderType::Blit && BarrierTouchesObject(*static_cast<const BlitEncoderData *>(b), object)) ok = false;
          }
          for (auto *b : between)
            if (ok && b->type == EncoderType::Blit && BarrierTouchesObject(*static_cast<const BlitEncoderData *>(b), object)) ok = false;
          if (!ok) {
            // INRUN_CLEARS: the clear stays at its place, as the layer's clear kernel inside the run's encoder (a shader-writable,
            // single-sample 2D colour view of one slice, a kernel-writable float format, a whole-view clear)
            const auto *view = c->attachment.ptr();
            auto *tex = view && view->allocation ? view->allocation->descriptor : nullptr;
            const bool inrun = env.inrun_clears && fold_clears && object && !c->clear_dsv && tex && c->array_length <= 1 &&
                               !c->depth_plane && (tex->usage() & WMTTextureUsageShaderWrite) && tex->sampleCount() == 1 &&
                               tex->textureType(view->key) == WMTTextureType2D &&
                               post_chain::KernelFormat(ORIGINAL_FORMAT(tex->pixelFormat(view->key))) && c->width && c->height &&
                               post_chain::ClearKernel(device_);
            if (!inrun) { stop = &st.stop_clear; break; }
            clear_inrun.insert(p);
            between.push_back(p); p = p->next; continue;
          }
          clear_list[p] = ppCommandLists[l];
          between.push_back(p); p = p->next; continue;
        }
        if (p->type != EncoderType::Render) { stop = &st.stop_other; break; }
        auto *r = static_cast<RenderEncoderData *>(p);
        if (!pc_candidate(*r)) {
          stop = r->post_chain && r->post_chain_draws != 1 ? &st.stop_draws : &st.stop_render;
          if (stop == &st.stop_render) {   // PCS2 03.11: why this render pass is not a member
            const unsigned why = r->post_chain ? unsigned(post_chain::WhyNone) : unsigned(r->post_chain_why);
            if (r->post_chain) post_chain::Note(st.sr_record);
            else if (why == post_chain::WhyBlend) post_chain::Note(st.sr_pso_blend);
            else if (why == post_chain::WhyKernel || why == post_chain::WhyUvmap) post_chain::Note(st.sr_pso_kernel);
            else if (why == post_chain::WhyTargetFormat) post_chain::Note(st.sr_pso_format);
            else if (why && why < 32) post_chain::Note(st.sr_pso_other);
            else if (!why) post_chain::Note(st.sr_no_record);
            else post_chain::Note(st.sr_record);
            post_chain::StopSeen(r->post_chain ? 99u : why, r->render_target_width, r->render_target_height, 0, r->post_chain_ps,
                                 r->post_chain_draws, unsigned(run.size()));
          }
          break;
        }
        if (pc_pixels(*r->post_chain) > env.max_px) { stop = &st.stop_size; break; }
        PcMember m{r};
        m.before = std::move(between); between.clear();
        run.push_back(std::move(m));
        p = p->next;
      }
      post_chain::Note(*stop);
      post_chain::Note(st.cand_passes, run.size());
      static const double pc_ticks_us = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return 1e6 / double(f.QuadPart ? f.QuadPart : 1); }();
      LARGE_INTEGER t_scan{}; QueryPerformanceCounter(&t_scan);
      post_chain::Note(st.cpu_scan_us, uint64_t(double(t_scan.QuadPart - t0.QuadPart) * pc_ticks_us));
      post_chain::Note(st.qpc_calls, 4);
      const auto reject = [&] {
        for (auto &m : run) pc_rejected.insert(m.pass);
        post_chain::Note(st.runs_short);
        LARGE_INTEGER t1{}; QueryPerformanceCounter(&t1);
        static const double ticks_us = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return 1e6 / double(f.QuadPart ? f.QuadPart : 1); }();
        post_chain::Note(st.cpu_us, uint64_t(double(t1.QuadPart - t0.QuadPart) * ticks_us));
        return false;
      };
      if (run.size() < min_run) return reject();
      // 2. contents: key, map, witness, coverage; the run ends before the first member that cannot be converted now
      for (size_t k = 0; k < run.size(); ++k)
        if (!pc_prepare(run[k])) { run.resize(k); break; }
      LARGE_INTEGER t_prep{}; QueryPerformanceCounter(&t_prep);
      post_chain::Note(st.cpu_prep_us, uint64_t(double(t_prep.QuadPart - t_scan.QuadPart) * pc_ticks_us));
      if (run.size() < min_run) return reject();
      // 3. encode: pending clears the run could observe, the uv-maps not yet rendered on this queue, then the compute encoder
      std::vector<obj_handle_t> objects;   // run targets
      std::vector<const BlitEncoderData *> run_barriers;   // the barrier passes the run consumes
      for (auto &m : run) {
        objects.push_back(AttachmentObject(m.pass->colors[0].attachment));
        for (auto *b : m.before)
          if (b->type == EncoderType::Blit) run_barriers.push_back(static_cast<const BlitEncoderData *>(b));
      }
      close_open_render();
      // PCS2 03.11: the clears inside the (final) run join the pending clears here, before the run
      for (auto &m : run)
        for (auto *b : m.before)
          if (b->type == EncoderType::Clear && clear_inrun.count(b)) {
            objects.push_back(AttachmentObject(static_cast<ClearEncoderData *>(b)->attachment));   // its earlier pending clears go first
          } else if (b->type == EncoderType::Clear) {
            auto *c = static_cast<ClearEncoderData *>(b);
            g_pass_stats.clear_recorded.fetch_add(1, std::memory_order_relaxed);
            c->census_begin = enc_census::BeginNo;
            c->census_why = enc_census::WhyFolded;
            pending_clears.push_back({c, clear_list[b]});
            post_chain::Note(st.clears_hoisted);
          }
      if (!pending_clears.empty()) {
        const size_t before = pending_clears.size();
        // as the barrier path: a clear of a run target, or of anything a run barrier touches (BarrierTouchesObject: every
        // object for a barrier without records, an aliasing one or one naming no object)
        bool all = false;
        encode_pending_if([&](obj_handle_t object, const ClearEncoderData &) {
          if (std::find(objects.begin(), objects.end(), object) != objects.end()) return true;
          for (const auto *b : run_barriers)
            if (BarrierTouchesObject(*b, object)) {
              if (!b->barrier_records_valid || std::any_of(b->barrier_records.begin(), b->barrier_records.end(), [](const BarrierRecord &rec) {
                    return rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !rec.object; }))
                all = true;
              return true;
            }
          return false;
        }, ClearWhyBreak);
        if (all) post_chain::Note(st.bx_clears_all);
        // CLEAR_SINK: a sunk clear goes on only past render passes proven not to read its texture; this is not one
        std::vector<PendingClear> now, keep;
        for (auto &pc : pending_clears) (pc.sunk ? now : keep).push_back(pc);
        if (!now.empty()) { pending_clears.swap(keep); encode_clears(std::move(now), ClearWhyBreak); }
        post_chain::Note(st.clears_flushed, before - pending_clears.size());
      }
      virtual_clears.clear();
      last_render = nullptr;
      barrier_since_render = false;
      relaxed_since_render = false;
      barrier_reason = ChainBarrierNone;
      std::vector<const post_chain::MapEntry *> rendered;   // a content two members share is rendered once
      for (auto &m : run)
        if (m.render_map && std::find(rendered.begin(), rendered.end(), m.map.get()) == rendered.end()) {
          pc_render_map(m);
          post_chain::Cache::Instance().Rendered(m.map, queue_id_);
          rendered.push_back(m.map.get());
          post_chain::Note(st.maps_rendered);
        } else {
          post_chain::Note(st.maps_reused);
        }
      const auto witness = post_chain::WitnessBuffer(device_);
      bool pc_alias = false;   // a consumed barrier is aliasing or has no records
      auto encoder = cmdbuf.computeCommandEncoder(false);
      encoder.waitForFence(state_->fence_);
      for (size_t k = 0; k < run.size(); ++k) {
        auto &m = run[k];
        const auto &d = *m.pass->post_chain;
        const auto &v = *d.variant;
        for (auto *b : m.before) {   // PCS2: a clear inside the run, at its place (after a memory barrier)
          if (b->type != EncoderType::Clear || !clear_inrun.count(b)) continue;
          auto *c = static_cast<ClearEncoderData *>(b);
          wmtcmd_compute_memory_barrier cb{};
          cb.type = WMTComputeCommandMemoryBarrier;
          cb.scope = WMTBarrierScope(WMTBarrierScopeBuffers | WMTBarrierScopeTextures);
          struct { float value[4]; uint32_t offset[2]; uint32_t size[2]; } meta = {
            {float(c->color.r), float(c->color.g), float(c->color.b), float(c->color.a)}, {0, 0}, {c->width, c->height}};
          wmtcmd_compute_setpso cpso{}; cpso.type = WMTComputeCommandSetPSO;
          cpso.pso = post_chain::ClearKernel(device_).handle; cpso.threadgroup_size = {8, 8, 1};
          wmtcmd_compute_settexture ctex{}; ctex.type = WMTComputeCommandSetTexture; ctex.texture = c->attachment.texture().handle; ctex.index = 0;
          wmtcmd_compute_setbytes cmeta{}; cmeta.type = WMTComputeCommandSetBytes; cmeta.bytes.set(&meta); cmeta.length = sizeof(meta); cmeta.index = 1;
          wmtcmd_compute_dispatch cd{}; cd.type = WMTComputeCommandDispatchThreads; cd.size = {uint64_t(c->width), uint64_t(c->height), 1};
          cb.next.set(&cpso); cpso.next.set(&ctex); ctex.next.set(&cmeta); cmeta.next.set(&cd); cd.next.set(nullptr);
          encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&cb));
          gpu4_add(c->attachment);
          g_pass_stats.clear_recorded.fetch_add(1, std::memory_order_relaxed);
          c->census_begin = enc_census::BeginNo;
          c->census_why = enc_census::WhyFolded;
          post_chain::Note(st.clears_inrun);
        }
        if (k) {
          wmtcmd_compute_memory_barrier barrier{};
          barrier.type = WMTComputeCommandMemoryBarrier;
          barrier.scope = WMTBarrierScope(WMTBarrierScopeBuffers | WMTBarrierScopeTextures);
          barrier.next.set(nullptr);
          encoder.encodeCommands(reinterpret_cast<wmtcmd_compute_nop *>(&barrier));
          post_chain::Note(st.barriers);
        }
        wmtcmd_compute_nop head{}; head.type = WMTComputeCommandNop;
        wmtcmd_compute_setpso pso{}; pso.type = WMTComputeCommandSetPSO; pso.pso = v.kernel.handle; pso.threadgroup_size = {8, 8, 1};
        wmtcmd_compute_setbuffer root{}, samplers{}, wit{}, src[SM50_POST_CS_MAX_SOURCES]{};
        root.type = samplers.type = wit.type = WMTComputeCommandSetBuffer;
        root.buffer = d.fs_root->buffer; root.offset = d.fs_root->offset; root.index = SM50_BINDING_INDEX_ROOT_ARGUMENTS;
        if (d.fs_samplers) { samplers.buffer = d.fs_samplers->buffer; samplers.offset = d.fs_samplers->offset; samplers.index = SM50_BINDING_INDEX_STATIC_SAMPLERS; }
        wmtcmd_compute_setbytes params{}; params.type = WMTComputeCommandSetBytes; params.bytes.set(&m.params);
        params.length = sizeof(m.params); params.index = SM50_POST_CS_PARAMS_SLOT;
        wmtcmd_compute_settexture target{}, maps[SM50_POST_CS_MAX_MAPS]{};
        target.type = WMTComputeCommandSetTexture; target.texture = m.pass->colors[0].attachment.texture().handle;
        target.index = SM50_POST_CS_TARGET_SLOT;
        for (uint32_t i = 0; i < v.map_count; ++i) {
          maps[i].type = WMTComputeCommandSetTexture;
          maps[i].texture = m.map->maps[i]->view(m.map->maps[i]->fullView).texture.handle;
          maps[i].index = uint8_t(SM50_POST_CS_MAP_SLOT + i);
        }
        wit.buffer = witness.handle; wit.offset = 0; wit.index = SM50_POST_CS_WITNESS_SLOT;
        for (uint32_t r = 0; r < SM50_POST_CS_MAX_SOURCES; ++r) {
          src[r].type = WMTComputeCommandSetBuffer;
          src[r].buffer = r < m.params.source_count ? m.sources[r] : witness.handle;
          src[r].offset = r < m.params.source_count ? m.source_offsets[r] : 0;
          src[r].index = uint8_t(SM50_POST_CS_SOURCE_SLOT + r);
        }
        wmtcmd_compute_dispatch dispatch{}; dispatch.type = WMTComputeCommandDispatchThreads;
        uint64_t w = uint64_t(d.x1 - d.x0);
        if (pc_key == 9 && w > 1) --w;   // NEGATIVE CONTROL (probes): the last pixel column stays unwritten
        dispatch.size = {w, uint64_t(d.y1 - d.y0), 1};
        std::vector<wmtcmd_base *> chain = {(wmtcmd_base *)&head, (wmtcmd_base *)&pso, (wmtcmd_base *)&root};
        if (d.fs_samplers) chain.push_back((wmtcmd_base *)&samplers);
        chain.push_back((wmtcmd_base *)&params);
        chain.push_back((wmtcmd_base *)&target);
        for (uint32_t i = 0; i < v.map_count; ++i) chain.push_back((wmtcmd_base *)&maps[i]);
        chain.push_back((wmtcmd_base *)&wit);
        for (uint32_t r = 0; r < SM50_POST_CS_MAX_SOURCES; ++r) chain.push_back((wmtcmd_base *)&src[r]);
        chain.push_back((wmtcmd_base *)&dispatch);
        for (size_t c = 0; c + 1 < chain.size(); ++c) chain[c]->next.set(chain[c + 1]);
        chain.back()->next.set(nullptr);
        encoder.encodeCommands(&head);
        gpu4_add(m.pass->colors[0].attachment);   // GPU4 TYPED_PRECISE bookkeeping (written by this scope)
        m.pass->census_begin = k ? enc_census::BeginNo : enc_census::BeginYes;
        // consumed: this member (the first is the current pass of the main loop) and the passes before it
        if (k) pc_consumed.insert(m.pass);
        for (auto *b : m.before) {
          pc_consumed.insert(b);
          if (b->type == EncoderType::Blit && (split_fence || fence_stats)) {   // as the barrier path would have noted them
            const auto *bb = static_cast<const BlitEncoderData *>(b);
            bool aliasing = !bb->barrier_records_valid;
            for (const auto &rec : bb->barrier_records) aliasing |= rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            pc_alias |= aliasing;
            if (!bb->barrier_records_valid) typed_exposed = true;
            for (const auto &rec : bb->barrier_records)
              if (typed_safe::Exposes(rec.type, rec.object, rec.before, rec.after)) typed_exposed = true;
          }
        }
        post_chain::Note(st.converted);
      }
      encoder.updateFence(state_->fence_);
      // kernels write only textures (and the witness word nobody's pre-raster stage reads): the texture-only compute rule.
      // An aliasing (or unrecorded) barrier consumed by the run: as the barrier path, alias_join holds until an encoder that
      // updates both fences (here: this one without PRERASTER_PRECISE; with it, the next render pass joins).
      if (!prerast_precise_on) { pre_update(encoder); alias_join = false; }
      else {
        gpu4_skip = true; prerast_precise::Note(true, true);
        if (pc_alias) { alias_join = true; post_chain::Note(st.bx_alias_join); }
      }
      prev_render = false; scope_first = false;
      encoder.endEncoding();
      g_pass_stats.other_encoders.fetch_add(1, std::memory_order_relaxed);
      encoder_stats::Count(encoder_stats::KindCompute, first->close_reason);
      census_prev = 3;
      post_chain::Note(st.runs);
      post_chain::Note(run.size() <= 8 ? st.len_8 : run.size() <= 16 ? st.len_16 : run.size() <= 32 ? st.len_32 : st.len_more);
      LARGE_INTEGER t1{}; QueryPerformanceCounter(&t1);
      static const double ticks_us = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return 1e6 / double(f.QuadPart ? f.QuadPart : 1); }();
      post_chain::Note(st.cpu_us, uint64_t(double(t1.QuadPart - t0.QuadPart) * ticks_us));
      post_chain::Note(st.cpu_enc_us, uint64_t(double(t1.QuadPart - t_prep.QuadPart) * ticks_us));
      return true;
    };
