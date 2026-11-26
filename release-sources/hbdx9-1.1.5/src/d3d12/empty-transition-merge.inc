// SPDX-License-Identifier: LGPL-2.1-or-later
// Included after SameRenderTargets/AttachmentObject. A proof over the whole interval,
// never an exemption for a single attachment barrier in isolation.
struct EmptyTransitionPlan {
  std::unordered_set<const BlitEncoderData *> barriers;
  std::unordered_set<const RenderEncoderData *> heads;
};
static bool EmptyPlainRender(const RenderEncoderData &r) {
  if (r.may_write_memory || r.use_tessellation || r.use_geometry || !r.visibility_targets.empty() ||
      r.visibility_direct || !r.direct_slots.empty() || r.empty_binding_first == UINT64_MAX) return false;
  // No in-pass clear/discard/resolve semantics may disappear with an encoder boundary.
  for (const auto &a:r.colors) if (a.attachment && (a.resolve_attachment || a.load_action != WMTLoadActionLoad ||
                                                 a.store_action != WMTStoreActionStore)) return false;
  if (r.depth.attachment && (r.depth.load_action != WMTLoadActionLoad || r.depth.store_action != WMTStoreActionStore)) return false;
  if (r.stencil.attachment && (r.stencil.load_action != WMTLoadActionLoad || r.stencil.store_action != WMTStoreActionStore)) return false;
  return true;
}
static bool EmptyBarrierOnly(const BlitEncoderData &b) {
  return b.barrier_only && b.barrier_records_valid && !b.timestamp_samples && !b.resolve_samples &&
         !b.wait_for_prior_work && !b.immediate_out && !b.cmd_head.next.ptr;
}
// Defer lists with a mergeable head or tail until submission, where neighbors are known.
// This includes multi-render lists; each list is still encoded once. Work in the middle
// does not forbid merging a safe boundary, but is never skipped by the interval proof.
static bool EmptyDeferBoundary(MTLD3D12GraphicsCommandList *list) {
  if (!c17::EmptyTransitions() || list->empty_indirect) return false;
  const EncoderData *first=nullptr, *last=nullptr;
  for (auto p=list->entry;p;p=p->next) {
    if (p->type==EncoderType::Null) continue;
    if (p->type==EncoderType::Blit && EmptyBarrierOnly(*static_cast<BlitEncoderData *>(p))) continue;
    if (!first) first=p;
    last=p;
  }
  const auto plain = [](const EncoderData *p) {
    return p && p->type==EncoderType::Render && EmptyPlainRender(*static_cast<const RenderEncoderData *>(p));
  };
  return plain(first) || plain(last);
}
static EmptyTransitionPlan PlanEmptyTransitions(UINT count, ID3D12CommandList *const *lists) {
  EmptyTransitionPlan plan;
  if (!c17::EmptyTransitions()) return plan;
  RenderEncoderData *left=nullptr;
  MTLD3D12GraphicsCommandList *left_list=nullptr;
  std::vector<const BlitEncoderData *> between;
  auto &stats=c17::Get();
  for (UINT l=0;l<count;++l) {
    auto list=static_cast<MTLD3D12GraphicsCommandList *>(lists[l]);
    for (auto p=list->entry;p;p=p->next) {
      if (p->type==EncoderType::Null) continue;
      if (p->type==EncoderType::Blit && static_cast<BlitEncoderData *>(p)->barrier_only) {
        if (left) between.push_back(static_cast<BlitEncoderData *>(p));
        continue;
      }
      if (p->type!=EncoderType::Render) {
        if (left && !between.empty()) c17::Note(stats.empty_reject_work);
        left=nullptr;left_list=nullptr;between.clear();continue;
      }
      auto right=static_cast<RenderEncoderData *>(p);
      if (left && !between.empty() && !SameRenderTargets(*left,*right))
        c17::Note(stats.empty_reject_work);
      if (left && !between.empty() && SameRenderTargets(*left,*right)) {
        c17::Note(stats.empty_candidates);
        bool binding = left_list==list ? left->empty_binding_last!=right->empty_binding_first :
            left->empty_binding_last!=left_list->empty_binding_epoch || right->empty_binding_first!=0;
        // Empty intermediate lists are also part of the interval.
        if (left_list!=list) {
          bool inside=false;
          for (UINT j=0;j<l;++j) {
            auto mid=static_cast<MTLD3D12GraphicsCommandList *>(lists[j]);
            if (mid==left_list) {inside=true;continue;}
            if (inside && (mid->empty_binding_epoch || mid->empty_indirect)) binding=true;
          }
        }
        const bool effects=!EmptyPlainRender(*left) || !EmptyPlainRender(*right) || left_list->empty_indirect ||
            list->empty_indirect || left->dsv_readonly_flags || right->dsv_readonly_flags;
        struct Cycle {obj_handle_t object;uint32_t sub,first,last;};
        std::vector<Cycle> cycles;
        bool valid=true; unsigned records=0;
        constexpr uint32_t reads=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        for (const auto *b:between) {
          if (!EmptyBarrierOnly(*b)) {valid=false;break;}
          for (const auto &r:b->barrier_records) {
            if (++records>256 || r.type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || r.flags || !r.object ||
                !PassUsesObject(*left,r.object)) {valid=false;break;}
            const auto write_state = [&](uint32_t s) {return s==D3D12_RESOURCE_STATE_RENDER_TARGET || s==D3D12_RESOURCE_STATE_DEPTH_WRITE;};
            const auto allowed = [&](uint32_t s) {return write_state(s) || (s && !(s&~reads));};
            if (!allowed(r.before) || !allowed(r.after)) {valid=false;break;}
            auto found=cycles.end();
            for (auto it=cycles.begin();it!=cycles.end();++it) if (it->object==r.object) {
              if (it->sub==r.subresource) found=it;
              else if (it->sub==UINT32_MAX || r.subresource==UINT32_MAX) valid=false;
            }
            if (!valid) break;
            if (found==cycles.end()) {
              if (!write_state(r.before)) {valid=false;break;}
              cycles.push_back({r.object,r.subresource,r.before,r.after});
            } else {
              if (found->last!=r.before) {valid=false;break;}
              found->last=r.after;
            }
          }
          if (!valid) break;
        }
        if (cycles.empty()) valid=false;
        for (const auto &cycle:cycles) if (cycle.first!=cycle.last) valid=false;
        if (binding) c17::Note(stats.empty_reject_binding);
        else if (effects) c17::Note(stats.empty_reject_effect);
        else if (!valid) c17::Note(stats.empty_reject_state);
        else {
          c17::Note(stats.empty_approved);
          plan.heads.insert(right);
          plan.barriers.insert(between.begin(),between.end());
        }
      }
      left=right;left_list=list;between.clear();
    }
  }
  return plan;
}
