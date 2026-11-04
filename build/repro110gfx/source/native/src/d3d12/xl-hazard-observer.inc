// SPDX-License-Identifier: LGPL-2.1-or-later
// XL-GAPS (23.10.2026), included after empty-transition-merge.inc: MACRUNNER_DX12_XL_HAZARD observer (see
// d3d12_xl_hazard.hpp). Judges, with the mode-2 rules and without acting, every pair of passes mode 2 would consider
// and checks, for each draw of both passes, whether a descriptor it can read is the same subresource as a written
// attachment of the pair. Nothing is merged, skipped or reordered here.
namespace xl_hazard_observer {

enum HitClass : uint8_t { HitNone = 0, HitOtherMip, HitOtherSlice, HitOtherPlane, HitReadOnly, HitSame, HitClasses };
static const char *const kHitNames[HitClasses] = {"none", "other_mip", "other_slice", "other_plane", "readonly", "same"};

struct Attachment {
  const Texture *texture = nullptr;
  uint32_t mip = 0, a0 = 0, a1 = 1;
  uint8_t plane = 0, written = 1, slot = 0;
  bool volume = false;
};
struct DescHit {
  uint32_t index = 0;
  const Texture *texture = nullptr;
  uint32_t mip0 = 0, mip1 = 1, a0 = 0, a1 = 1;
  uint8_t plane = 0, uav = 0;
};
struct Counts {
  uint64_t pairs = 0, passes = 0, draws = 0, snapshots = 0, unknown_draws = 0, undeclared_draws = 0, wide = 0;
  uint64_t descriptors = 0, hits = 0;
  uint64_t cls[HitClasses] = {};      // worst class per draw over the DECLARED registers (the verdict)
  uint64_t any[HitClasses] = {};      // worst class per draw over the WHOLE bound table ranges (declared or not)
  uint64_t any_descriptors = 0, any_wide = 0, any_skipped = 0;
  uint64_t same_right = 0, same_uav = 0, same_undeclared = 0;
  void Add(const Counts &o) {
    pairs += o.pairs; passes += o.passes; draws += o.draws; snapshots += o.snapshots; unknown_draws += o.unknown_draws;
    undeclared_draws += o.undeclared_draws; wide += o.wide; descriptors += o.descriptors; hits += o.hits;
    for (unsigned i = 0; i < HitClasses; ++i) { cls[i] += o.cls[i]; any[i] += o.any[i]; }
    any_descriptors += o.any_descriptors; any_wide += o.any_wide; any_skipped += o.any_skipped;
    same_right += o.same_right; same_uav += o.same_uav; same_undeclared += o.same_undeclared;
  }
};
struct State {
  std::mutex mutex;
  std::atomic<uint64_t> presents{0};
  uint64_t frames_evaluated = 0, executes = 0, ticks = 0, last_frame = UINT64_MAX, ordinal = 0, detail_rows = 0;
  std::map<std::string, Counts> buckets;                          // verdict
  std::map<std::pair<std::string, std::string>, Counts> classes;  // verdict, class
  std::map<std::tuple<unsigned, std::string, std::string>, Counts> pairs;   // ordinal in frame, verdict, class
  Counts unique_approved;                                         // each approved-pair pass counted once per Execute
};
static State &Get() { static State s; return s; }

static void Line(const char *format, ...) {
  char line[2048];
  va_list args; va_start(args, format);
  const int n = std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  DWORD written;
  if (n > 0 && size_t(n) < sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
}

static const Texture *ViewTexture(const TextureViewRef &view) {
  return view && view->allocation ? view->allocation->descriptor : nullptr;
}
static void KeyRange(TextureViewKey key, uint32_t &mip0, uint32_t &mip1, uint32_t &a0, uint32_t &a1) {
  mip0 = uint32_t(key.mip_start); mip1 = uint32_t(key.mip_end);
  if (mip1 <= mip0) mip1 = 16;            // 4-bit end wrapped (a view up to the 16th level)
  a0 = uint32_t(key.array_start); a1 = uint32_t(key.array_end);
  if (a1 <= a0) a1 = 4096;
}
static std::vector<Attachment> Attachments(const RenderEncoderData &r) {
  std::vector<Attachment> out;
  const auto add = [&](const TextureViewRef &view, uint8_t plane, bool written, uint8_t slot) {
    const auto *t = ViewTexture(view);
    if (!t) return;
    Attachment a; a.texture = t; a.plane = plane; a.written = written; a.slot = slot;
    uint32_t mip1, a1; KeyRange(TextureViewKey(uint64_t(view->key)), a.mip, mip1, a.a0, a1); a.a1 = a1;
    a.volume = const_cast<Texture *>(t)->textureType() == WMTTextureType3D;
    out.push_back(a);
  };
  for (unsigned i = 0; i < r.render_target_count && i < r.colors.size(); ++i) {
    add(r.colors[i].attachment, 0, true, uint8_t(i));
    add(r.colors[i].resolve_attachment, 0, true, uint8_t(16 + i));
  }
  const unsigned ro = r.dsv_readonly_flags | r.xl_dsv_flags;   // the DSV's own flags (the layer does not apply them)
  add(r.depth.attachment, 0, !(ro & D3D12_DSV_FLAG_READ_ONLY_DEPTH), 8);
  add(r.stencil.attachment, 1, !(ro & D3D12_DSV_FLAG_READ_ONLY_STENCIL), 9);
  return out;
}
static std::string ClassOf(const RenderEncoderData &r) {
  char text[64];
  std::snprintf(text, sizeof(text), "rt%u%s%s%s_%ux%u", unsigned(r.render_target_count), r.depth.attachment ? "_d" : "",
                r.stencil.attachment ? "_s" : "", r.xl_dsv_flags ? "_ro" : "", r.render_target_width, r.render_target_height);
  return text;
}
static std::string BucketOf(const EmptyPairVerdict &v) {
  if (!v.reject) return "approved";
  if (v.reject == 1) return "reject_binding";
  if (v.reject == 2) {
    switch (v.effect) {
    case EmptyEffectWrite: return "reject_effect_write";
    case EmptyEffectMesh: return "reject_effect_mesh";
    case EmptyEffectQuery: return "reject_effect_query";
    case EmptyEffectNoDraw: return "reject_effect_nodraw";
    case EmptyEffectLoadStore: return "reject_effect_loadstore";
    default: return v.indirect ? "reject_effect_indirect" : "reject_effect_readonly";
    }
  }
  return "reject_state";
}
static uint8_t SrvPlane(uint32_t dxgi_format) {
  return (dxgi_format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT || dxgi_format == DXGI_FORMAT_X24_TYPELESS_G8_UINT) ? 1 : 0;
}
static HitClass Classify(const DescHit &h, const std::vector<Attachment> &att, const Attachment **which) {
  HitClass worst = HitNone;
  for (const auto &a : att) {
    if (a.texture != h.texture) continue;
    const bool mip = a.mip >= h.mip0 && a.mip < h.mip1;
    const bool slice = a.volume || (a.a0 < h.a1 && h.a0 < a.a1);
    const bool plane = a.plane == h.plane;
    HitClass c = mip && slice && plane ? (a.written ? HitSame : HitReadOnly) : !mip ? HitOtherMip : !slice ? HitOtherSlice : HitOtherPlane;
    if (c > worst) { worst = c; if (which) *which = &a; }
  }
  return worst;
}

struct SpanKey {
  const void *heap; uint32_t start, count;
  bool operator<(const SpanKey &o) const { return std::tie(heap, start, count) < std::tie(o.heap, o.start, o.count); }
};
constexpr uint32_t kDeclCap = 1u << 20;     // a declared interval is read in full up to 1M descriptors
constexpr uint32_t kAnyCap = 65536;          // the whole-range statistic reads at most 64K per range ...
constexpr uint64_t kAnyBudget = 1u << 20;    // ... and at most 1M descriptors per ExecuteCommandLists
struct Scan { std::vector<DescHit> hits; bool wide = false; };
using ScanCache = std::map<SpanKey, Scan>;

// Descriptors [start, start + count) whose texture is one of the union of attachment textures of this Execute.
static const Scan &ScanRange(const void *heap_ptr, uint32_t start, uint32_t count, uint32_t cap,
                             const std::vector<const Texture *> &textures, ScanCache &cache) {
  const SpanKey key{heap_ptr, start, count};
  if (auto it = cache.find(key); it != cache.end()) return it->second;
  Scan scan;
  auto *heap = static_cast<MTLD3D12DescriptorHeap *>(static_cast<ID3D12DescriptorHeap *>(const_cast<void *>(heap_ptr)));
  const uint32_t n = std::min(count, cap);
  scan.wide = count > cap;
  for (uint32_t i = 0; i < n; ++i) {
    const auto &d = heap->GetDescriptor(start + i);
    if (d.type != ShaderVisibleDescriptorType::SRVTexture && d.type != ShaderVisibleDescriptorType::UAVTexture) continue;
    const Texture *t = d.SRVTexture.texture;
    if (!t || std::find(textures.begin(), textures.end(), t) == textures.end()) continue;
    DescHit h; h.index = start + i; h.texture = t; h.uav = d.type == ShaderVisibleDescriptorType::UAVTexture;
    KeyRange(d.SRVTexture.view, h.mip0, h.mip1, h.a0, h.a1);
    h.plane = SrvPlane(d.shape.format);
    scan.hits.push_back(h);
  }
  return cache.emplace(key, std::move(scan)).first->second;
}

struct DetailContext { bool on = false; uint64_t frame = 0; unsigned ordinal = 0; const char *side = ""; const char *bucket = ""; };

static Counts EvaluatePass(const RenderEncoderData &pass, const std::vector<Attachment> &att,
                           const std::vector<const Texture *> &textures, ScanCache &decl_cache, ScanCache &any_cache,
                           uint64_t &any_budget, const DetailContext &detail) {
  Counts c; c.passes = 1;
  if (!pass.xl_hazard) return c;
  const auto &b = *pass.xl_hazard;
  c.draws = b.draws;
  for (unsigned s = 0; s < b.snapshots.size(); ++s) {
    const auto &snap = b.snapshots[s];
    ++c.snapshots;
    if (snap.flags) c.unknown_draws += snap.draws;
    HitClass worst = HitNone, worst_any = HitNone; bool undeclared = false, uav_same = false, undeclared_same = false;
    for (uint32_t k = 0; k < snap.span_count; ++k) {
      const auto &span = b.spans[snap.first_span + k];
      if (!span.declared) undeclared = true;
      // the verdict: every declared interval, in full
      for (uint32_t j = 0; j < span.decl_count; ++j) {
        const auto &iv = b.decls[span.first_decl + j];
        c.descriptors += std::min(iv.count, kDeclCap);
        const auto &scan = ScanRange(span.heap, iv.start, iv.count, kDeclCap, textures, decl_cache);
        if (scan.wide) ++c.wide;
        for (const auto &h : scan.hits) {
          const Attachment *which = nullptr;
          const auto cls = Classify(h, att, &which);
          if (cls == HitNone) continue;
          ++c.hits;
          if (cls == HitSame) { uav_same |= h.uav != 0; undeclared_same |= !span.declared; }
          worst = std::max(worst, cls);
          auto &st = Get();
          if (detail.on && st.detail_rows < 1024 && which) {
            ++st.detail_rows;
            Line("dx12_xl_hazard_hit frame=%llu k=%u verdict=%s side=%s snapshot=%u draws=%u class=%s heap_index=%u texture=%p "
                 "view_mips=%u-%u view_slices=%u-%u plane=%u uav=%u declared=%u att_slot=%u att_mip=%u att_slices=%u-%u "
                 "att_plane=%u att_written=%u\n",
                 (unsigned long long)detail.frame, detail.ordinal, detail.bucket, detail.side, s, snap.draws, kHitNames[cls],
                 h.index, (const void *)h.texture, h.mip0, h.mip1, h.a0, h.a1, unsigned(h.plane), unsigned(h.uav),
                 unsigned(span.declared), unsigned(which->slot), which->mip, which->a0, which->a1, unsigned(which->plane),
                 unsigned(which->written));
          }
        }
      }
      // the statistic: the whole bound range, declared or not (what an object-level check would see)
      const uint32_t n = std::min(span.count, kAnyCap);
      if (!any_cache.count({span.heap, span.start, span.count}) && any_budget < n) { ++c.any_skipped; continue; }
      if (!any_cache.count({span.heap, span.start, span.count})) any_budget -= n;
      c.any_descriptors += n;
      const auto &scan = ScanRange(span.heap, span.start, span.count, kAnyCap, textures, any_cache);
      if (scan.wide) ++c.any_wide;
      for (const auto &h : scan.hits) worst_any = std::max(worst_any, Classify(h, att, nullptr));
    }
    worst_any = std::max(worst_any, worst);
    if (undeclared) c.undeclared_draws += snap.draws;
    c.cls[worst] += snap.draws; c.any[worst_any] += snap.draws;
    if (worst == HitSame) { if (uav_same) c.same_uav += snap.draws; if (undeclared_same) c.same_undeclared += snap.draws; }
  }
  return c;
}

static void Report(const char *why) {
  auto &s = Get();
  const auto print = [](const char *head, const Counts &c, const char *tail) {
    Line("%s pairs=%llu passes=%llu draws=%llu snapshots=%llu unknown_draws=%llu undeclared_draws=%llu wide_spans=%llu "
         "descriptors=%llu hits=%llu draws_none=%llu draws_other_mip=%llu draws_other_slice=%llu draws_other_plane=%llu "
         "draws_readonly=%llu draws_same=%llu same_right=%llu same_uav=%llu same_undeclared=%llu "
         "any_descriptors=%llu any_wide=%llu any_skipped=%llu any_other_mip=%llu any_other_slice=%llu any_other_plane=%llu "
         "any_readonly=%llu any_same=%llu%s\n",
         head, (unsigned long long)c.pairs, (unsigned long long)c.passes, (unsigned long long)c.draws,
         (unsigned long long)c.snapshots, (unsigned long long)c.unknown_draws, (unsigned long long)c.undeclared_draws,
         (unsigned long long)c.wide, (unsigned long long)c.descriptors, (unsigned long long)c.hits,
         (unsigned long long)c.cls[HitNone], (unsigned long long)c.cls[HitOtherMip], (unsigned long long)c.cls[HitOtherSlice],
         (unsigned long long)c.cls[HitOtherPlane], (unsigned long long)c.cls[HitReadOnly], (unsigned long long)c.cls[HitSame],
         (unsigned long long)c.same_right, (unsigned long long)c.same_uav, (unsigned long long)c.same_undeclared,
         (unsigned long long)c.any_descriptors, (unsigned long long)c.any_wide, (unsigned long long)c.any_skipped,
         (unsigned long long)c.any[HitOtherMip], (unsigned long long)c.any[HitOtherSlice], (unsigned long long)c.any[HitOtherPlane],
         (unsigned long long)c.any[HitReadOnly], (unsigned long long)c.any[HitSame], tail);
  };
  char head[256];
  std::snprintf(head, sizeof(head), "dx12_xl_hazard why=%s presents=%llu frames_evaluated=%llu executes=%llu ticks=%llu "
                "pso_dxil=%llu pso_dxbc=%llu pso_unknown=%llu bucket=approved_unique", why,
                (unsigned long long)s.presents.load(std::memory_order_relaxed), (unsigned long long)s.frames_evaluated,
                (unsigned long long)s.executes, (unsigned long long)s.ticks,
                (unsigned long long)xl_hazard::g_pso_dxil.load(), (unsigned long long)xl_hazard::g_pso_dxbc.load(),
                (unsigned long long)xl_hazard::g_pso_unknown.load());
  print(head, s.unique_approved, "");
  for (const auto &[bucket, c] : s.buckets) {
    std::snprintf(head, sizeof(head), "dx12_xl_hazard why=%s bucket=%s", why, bucket.c_str());
    print(head, c, "");
  }
  for (const auto &[key, c] : s.classes) {
    std::snprintf(head, sizeof(head), "dx12_xl_hazard_class why=%s bucket=%s class=%s", why, key.first.c_str(), key.second.c_str());
    print(head, c, "");
  }
  unsigned rows = 0;
  for (const auto &[key, c] : s.pairs) {
    if (++rows > 256) break;
    std::snprintf(head, sizeof(head), "dx12_xl_hazard_pair why=%s k=%u bucket=%s class=%s", why, std::get<0>(key),
                  std::get<1>(key).c_str(), std::get<2>(key).c_str());
    print(head, c, "");
  }
}

static void Presented() {
  if (!xl_hazard::Enabled()) return;
  auto &s = Get();
  const auto n = s.presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % xl_hazard::ReportPeriod() == 0) {
    const DWORD error = GetLastError();
    std::lock_guard lock(s.mutex);
    Report("present");
    SetLastError(error);
  }
}

static void Execute(UINT count, ID3D12CommandList *const *lists) {
  if (!xl_hazard::Enabled() || !count) return;
  auto &s = Get();
  const uint64_t frame = s.presents.load(std::memory_order_relaxed);
  if (frame % xl_hazard::Every()) return;
  const DWORD error = GetLastError();
  LARGE_INTEGER t0{}, t1{}; QueryPerformanceCounter(&t0);
  try {
    struct Candidate { RenderEncoderData *left, *right; EmptyPairVerdict verdict; };
    std::vector<Candidate> candidates;
    {
      RenderEncoderData *left = nullptr; MTLD3D12GraphicsCommandList *left_list = nullptr;
      std::vector<const BlitEncoderData *> between;
      for (UINT l = 0; l < count; ++l) {
        auto list = static_cast<MTLD3D12GraphicsCommandList *>(lists[l]);
        for (auto p = list->entry; p; p = p->next) {
          if (p->type == EncoderType::Null) continue;
          if (p->type == EncoderType::Blit && static_cast<BlitEncoderData *>(p)->barrier_only) {
            if (left) between.push_back(static_cast<BlitEncoderData *>(p));
            continue;
          }
          if (p->type != EncoderType::Render) { left = nullptr; left_list = nullptr; between.clear(); continue; }
          auto right = static_cast<RenderEncoderData *>(p);
          if (left && !between.empty() && SameRenderTargets(*left, *right)) {
            bool binding = left_list == list ? left->empty_binding_last != right->empty_binding_first :
                left->empty_binding_last != left_list->empty_binding_epoch || right->empty_binding_first != 0;
            bool indirect = left_list->empty_indirect || list->empty_indirect;
            if (left_list != list) {
              bool inside = false;
              for (UINT j = 0; j < l; ++j) {
                auto mid = static_cast<MTLD3D12GraphicsCommandList *>(lists[j]);
                if (mid == left_list) { inside = true; continue; }
                if (inside && mid->empty_binding_epoch) binding = true;
                if (inside && mid->empty_indirect) indirect = true;
              }
            }
            candidates.push_back({left, right, EvaluateEmptyPair(*left, *right, binding, indirect, between, 2)});
          }
          left = right; left_list = list; between.clear();
        }
      }
    }
    std::lock_guard lock(s.mutex);
    ++s.executes;
    if (s.last_frame != frame) { s.last_frame = frame; ++s.frames_evaluated; s.ordinal = 0; }
    if (!candidates.empty()) {
      std::vector<const Texture *> textures;
      for (const auto &c : candidates)
        for (const auto &a : Attachments(*c.left))
          if (std::find(textures.begin(), textures.end(), a.texture) == textures.end()) textures.push_back(a.texture);
      ScanCache decl_cache, any_cache;
      uint64_t any_budget = kAnyBudget;
      std::set<const RenderEncoderData *> unique;
      const bool detail_frame = frame == xl_hazard::DetailFrame();
      for (const auto &c : candidates) {
        const unsigned k = unsigned(std::min<uint64_t>(s.ordinal++, 63));
        const auto att = Attachments(*c.left);
        const auto bucket = BucketOf(c.verdict);
        const auto klass = ClassOf(*c.left);
        DetailContext dl{detail_frame, frame, k, "left", bucket.c_str()}, dr{detail_frame, frame, k, "right", bucket.c_str()};
        auto left = EvaluatePass(*c.left, att, textures, decl_cache, any_cache, any_budget, dl);
        auto right = EvaluatePass(*c.right, att, textures, decl_cache, any_cache, any_budget, dr);
        right.same_right = right.cls[HitSame];
        Counts pair = left; pair.Add(right); pair.pairs = 1;
        s.buckets[bucket].Add(pair);
        s.classes[{bucket, klass}].Add(pair);
        s.pairs[{k, bucket, klass}].Add(pair);
        if (!c.verdict.reject) {
          if (unique.insert(c.left).second) s.unique_approved.Add(left);
          if (unique.insert(c.right).second) { right.same_right = right.cls[HitSame]; s.unique_approved.Add(right); }
          else s.unique_approved.same_right += right.cls[HitSame];
          s.unique_approved.pairs += 1;
        }
        if (detail_frame && s.detail_rows < 1024) {
          ++s.detail_rows;
          Line("dx12_xl_hazard_pairrow frame=%llu k=%u verdict=%s class=%s left_draws=%llu right_draws=%llu left_same=%llu "
               "right_same=%llu other_mip=%llu other_slice=%llu other_plane=%llu readonly=%llu undeclared_draws=%llu\n",
               (unsigned long long)frame, k, bucket.c_str(), klass.c_str(), (unsigned long long)left.draws,
               (unsigned long long)right.draws, (unsigned long long)left.cls[HitSame], (unsigned long long)right.cls[HitSame],
               (unsigned long long)pair.cls[HitOtherMip], (unsigned long long)pair.cls[HitOtherSlice],
               (unsigned long long)pair.cls[HitOtherPlane], (unsigned long long)pair.cls[HitReadOnly],
               (unsigned long long)pair.undeclared_draws);
        }
      }
    }
    QueryPerformanceCounter(&t1); s.ticks += uint64_t(t1.QuadPart - t0.QuadPart);
  } catch (...) {}
  SetLastError(error);
}

} // namespace xl_hazard_observer
