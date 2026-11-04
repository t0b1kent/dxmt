// SPDX-License-Identifier: LGPL-2.1-or-later
// Included after SameRenderTargets/AttachmentObject. A proof over the whole interval,
// never an exemption for a single attachment barrier in isolation.
struct EmptyTransitionPlan {
  std::unordered_set<const BlitEncoderData *> barriers;
  std::unordered_set<const RenderEncoderData *> heads;
};
// Why a pass is not plain (0 = plain). Order is the reporting priority.
enum EmptyEffect : uint8_t { EmptyEffectNone = 0, EmptyEffectWrite, EmptyEffectMesh, EmptyEffectQuery, EmptyEffectNoDraw,
                             EmptyEffectLoadStore };
static uint8_t EmptyPlainReason(const RenderEncoderData &r) {
  if (r.may_write_memory) return EmptyEffectWrite;
  if (r.use_tessellation || r.use_geometry) return EmptyEffectMesh;
  if (!r.visibility_targets.empty() || r.visibility_direct || !r.direct_slots.empty()) return EmptyEffectQuery;
  if (r.empty_binding_first == UINT64_MAX) return EmptyEffectNoDraw;
  // No in-pass clear/discard/resolve semantics may disappear with an encoder boundary.
  for (const auto &a:r.colors) if (a.attachment && (a.resolve_attachment || a.load_action != WMTLoadActionLoad ||
                                                 a.store_action != WMTStoreActionStore)) return EmptyEffectLoadStore;
  if (r.depth.attachment && (r.depth.load_action != WMTLoadActionLoad || r.depth.store_action != WMTStoreActionStore)) return EmptyEffectLoadStore;
  if (r.stencil.attachment && (r.stencil.load_action != WMTLoadActionLoad || r.stencil.store_action != WMTStoreActionStore)) return EmptyEffectLoadStore;
  return EmptyEffectNone;
}
static bool EmptyPlainRender(const RenderEncoderData &r) { return EmptyPlainReason(r) == EmptyEffectNone; }
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
// One boundary: left (last render pass before the barriers) and right (first after), same targets.
// reject: 0 approved, 1 binding veto (mode 1 only), 2 effect, 3 state. Pure: no counters, no plan.
struct EmptyPairVerdict { uint8_t reject = 0, effect = 0, state = 0; bool binding = false, indirect = false; };
static EmptyPairVerdict EvaluateEmptyPair(const RenderEncoderData &left, const RenderEncoderData &right, bool binding,
                                          bool indirect, const std::vector<const BlitEncoderData *> &between, unsigned mode) {
  EmptyPairVerdict v; v.binding = binding; v.indirect = indirect;
  v.effect = EmptyPlainReason(left);
  if (!v.effect) v.effect = EmptyPlainReason(right);
  const bool readonly = left.dsv_readonly_flags || right.dsv_readonly_flags;
  const bool effects = v.effect || indirect || readonly;
  // 1 records, 2 state value, 3 subresource mix, 4 not a closed write-state cycle
  struct Cycle {obj_handle_t object;uint32_t sub,first,last;};
  std::vector<Cycle> cycles;
  bool valid=true; unsigned records=0;
  constexpr uint32_t reads=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  for (const auto *b:between) {
    if (!EmptyBarrierOnly(*b)) {valid=false;v.state=1;break;}
    for (const auto &r:b->barrier_records) {
      if (++records>256 || r.type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || r.flags || !r.object ||
          !PassUsesObject(left,r.object)) {valid=false;v.state=1;break;}
      const auto write_state = [&](uint32_t s) {return s==D3D12_RESOURCE_STATE_RENDER_TARGET || s==D3D12_RESOURCE_STATE_DEPTH_WRITE;};
      const auto allowed = [&](uint32_t s) {return write_state(s) || (s && !(s&~reads));};
      if (!allowed(r.before) || !allowed(r.after)) {valid=false;v.state=2;break;}
      auto found=cycles.end();
      for (auto it=cycles.begin();it!=cycles.end();++it) if (it->object==r.object) {
        if (it->sub==r.subresource) found=it;
        else if (it->sub==UINT32_MAX || r.subresource==UINT32_MAX) {valid=false;v.state=3;}
      }
      if (!valid) break;
      if (found==cycles.end()) {
        if (!write_state(r.before)) {valid=false;v.state=4;break;}
        cycles.push_back({r.object,r.subresource,r.before,r.after});
      } else {
        if (found->last!=r.before) {valid=false;v.state=4;break;}
        found->last=r.after;
      }
    }
    if (!valid) break;
  }
  if (cycles.empty() && valid) {valid=false;v.state=1;}
  for (const auto &cycle:cycles) if (cycle.first!=cycle.last && valid) {valid=false;v.state=4;}
  // Mode 2: bindings never veto (see c17::EmptyTransitionsMode); the state proof is unchanged.
  if (binding && mode < 2) v.reject = 1;
  else if (effects) v.reject = 2;
  else if (!valid) v.reject = 3;
  return v;
}
// Counters for one verdict. live=true: the plan's own counters; false: the observer's xl_e2_* counters.
static void NoteEmptyVerdict(const EmptyPairVerdict &v, bool live) {
  auto &s=c17::Get();
  if (v.reject == 1) { c17::Note(live ? s.empty_reject_binding : s.xl_e2_binding); return; }
  if (v.reject == 2) {
    c17::Note(live ? s.empty_reject_effect : s.xl_e2_effect);
    switch (v.effect) {
    case EmptyEffectWrite: c17::Note(live ? s.empty_effect_write : s.xl_e2_effect_write); break;
    case EmptyEffectMesh: c17::Note(live ? s.empty_effect_mesh : s.xl_e2_effect_mesh); break;
    case EmptyEffectQuery: c17::Note(live ? s.empty_effect_query : s.xl_e2_effect_query); break;
    case EmptyEffectNoDraw: c17::Note(live ? s.empty_effect_nodraw : s.xl_e2_effect_nodraw); break;
    case EmptyEffectLoadStore: c17::Note(live ? s.empty_effect_loadstore : s.xl_e2_effect_loadstore); break;
    default:
      if (v.indirect) c17::Note(live ? s.empty_effect_indirect : s.xl_e2_effect_indirect);
      else c17::Note(live ? s.empty_effect_readonly : s.xl_e2_effect_readonly);
      break;
    }
    return;
  }
  if (v.reject == 3) {
    c17::Note(live ? s.empty_reject_state : s.xl_e2_state);
    switch (v.state) {
    case 2: c17::Note(live ? s.empty_state_value : s.xl_e2_state_value); break;
    case 3: c17::Note(live ? s.empty_state_subresource : s.xl_e2_state_subresource); break;
    case 4: c17::Note(live ? s.empty_state_open : s.xl_e2_state_open); break;
    default: c17::Note(live ? s.empty_state_records : s.xl_e2_state_records); break;
    }
    return;
  }
  c17::Note(live ? s.empty_approved : s.xl_e2_approved);
  if (v.binding) c17::Note(live ? s.empty_binding_tolerated : s.xl_e2_tolerated);
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
        bool indirect = left_list->empty_indirect || list->empty_indirect;
        // Empty intermediate lists are also part of the interval.
        if (left_list!=list) {
          bool inside=false;
          for (UINT j=0;j<l;++j) {
            auto mid=static_cast<MTLD3D12GraphicsCommandList *>(lists[j]);
            if (mid==left_list) {inside=true;continue;}
            if (inside && mid->empty_binding_epoch) binding=true;
            if (inside && mid->empty_indirect) indirect=true;
          }
        }
        const auto verdict = EvaluateEmptyPair(*left, *right, binding, indirect, between, c17::EmptyTransitionsMode());
        NoteEmptyVerdict(verdict, true);
        if (!verdict.reject) {
          plan.heads.insert(right);
          plan.barriers.insert(between.begin(),between.end());
        }
      }
      left=right;left_list=list;between.clear();
    }
  }
  return plan;
}
