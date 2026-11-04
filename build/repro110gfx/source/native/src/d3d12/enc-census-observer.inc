// SPDX-License-Identifier: LGPL-2.1-or-later
// ENC-CENSUS (23.10.2026), included after xl-hazard-observer.inc: MACRUNNER_DX12_ENC_CENSUS observer (see
// d3d12_enc_census.hpp). Reads the encode-time annotations of the recorded passes after they were encoded and judges
// every boundary between two consecutive Metal encoders of a queue. Nothing is merged, skipped or reordered here.
namespace enc_census_observer {
using namespace enc_census;

// ---------------------------------------------------------------------------------------------------------------
// Caller thread (like xl_hazard_observer::Execute): what each recorded render pass can read.
struct ReadSub {
  const Texture *texture = nullptr;   // nullptr for a buffer
  obj_handle_t object = 0;            // current allocation (texture) or buffer handle, as barrier records name it
  uint16_t mip0 = 0, mip1 = 16, a0 = 0, a1 = 4096;
  uint8_t plane = 0, uav = 0;
  bool operator<(const ReadSub &o) const {
    return std::tie(texture, object, mip0, mip1, a0, a1, plane, uav) <
           std::tie(o.texture, o.object, o.mip0, o.mip1, o.a0, o.a1, o.plane, o.uav);
  }
  bool operator==(const ReadSub &o) const {
    return texture == o.texture && object == o.object && mip0 == o.mip0 && mip1 == o.mip1 && a0 == o.a0 && a1 == o.a1 &&
           plane == o.plane && uav == o.uav;
  }
};
// flags: 1 draws whose bindings could not be read (unknown), 2 undeclared ranges counted whole, 4 an interval was capped,
// 8 no binding record at all (READS off or MACRUNNER_DX12_XL_HAZARD=0)
struct PassReads { const EncoderData *pass = nullptr; uint32_t first = 0, count = 0; uint8_t flags = 0; };
struct EclReads {
  std::vector<PassReads> passes;
  std::vector<ReadSub> subs;
  uint64_t frame = 0, ticks = 0;
  bool on = false;
  const PassReads *Find(const EncoderData *p, size_t &cursor) const {
    for (size_t k = cursor; k < passes.size(); ++k)
      if (passes[k].pass == p) { cursor = k + 1; return &passes[k]; }
    for (size_t k = 0; k < cursor && k < passes.size(); ++k)
      if (passes[k].pass == p) return &passes[k];
    return nullptr;
  }
};
// Hand-over from the caller thread to the encoding call (the async worker sets it from the submission it runs).
static thread_local std::shared_ptr<EclReads> t_handover;
static std::atomic<uint64_t> g_public_frames{0};
static bool FrameOn(uint64_t frame) { return frame % Every() == 0; }

static void PublicPresented() { if (Enabled()) g_public_frames.fetch_add(1, std::memory_order_relaxed); }

constexpr uint32_t kReadCap = 1u << 16;   // descriptors read per declared interval at most
static void ScanInterval(const void *heap_ptr, uint32_t start, uint32_t count, uint8_t uav,
                         std::map<std::tuple<const void *, uint32_t, uint32_t>, std::vector<ReadSub>> &cache,
                         std::vector<ReadSub> &out, uint8_t &flags) {
  const auto key = std::make_tuple(heap_ptr, start, count);
  auto it = cache.find(key);
  if (it == cache.end()) {
    std::vector<ReadSub> found;
    auto *heap = static_cast<MTLD3D12DescriptorHeap *>(static_cast<ID3D12DescriptorHeap *>(const_cast<void *>(heap_ptr)));
    const uint32_t n = std::min(count, kReadCap);
    if (count > kReadCap) flags |= 4;
    for (uint32_t i = 0; i < n; ++i) {
      const auto &d = heap->GetDescriptor(start + i);
      ReadSub s;
      switch (d.type) {
      case ShaderVisibleDescriptorType::SRVTexture:
      case ShaderVisibleDescriptorType::UAVTexture: {
        Texture *t = d.SRVTexture.texture;
        if (!t) continue;
        s.texture = t;
        s.object = t->current() ? t->current()->texture().handle : 0;
        uint32_t m0, m1, a0, a1;
        xl_hazard_observer::KeyRange(d.SRVTexture.view, m0, m1, a0, a1);
        s.mip0 = uint16_t(m0); s.mip1 = uint16_t(std::min(m1, 16u)); s.a0 = uint16_t(std::min(a0, 4096u));
        s.a1 = uint16_t(std::min(a1, 4096u));
        s.plane = xl_hazard_observer::SrvPlane(d.shape.format);
        s.uav = d.type == ShaderVisibleDescriptorType::UAVTexture;
        break;
      }
      case ShaderVisibleDescriptorType::SRVBuffer:
      case ShaderVisibleDescriptorType::UAVBuffer:
      case ShaderVisibleDescriptorType::SRVTexelBuffer:
      case ShaderVisibleDescriptorType::UAVTexelBuffer: {
        Buffer *b = d.type == ShaderVisibleDescriptorType::SRVBuffer || d.type == ShaderVisibleDescriptorType::UAVBuffer
                        ? d.SRVBuffer.buffer : d.SRVTexelBuffer.buffer;
        if (!b || !b->current()) continue;
        s.object = b->current()->buffer().handle;
        s.uav = d.type == ShaderVisibleDescriptorType::UAVBuffer || d.type == ShaderVisibleDescriptorType::UAVTexelBuffer;
        break;
      }
      default:
        continue;
      }
      found.push_back(s);
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    it = cache.emplace(key, std::move(found)).first;
  }
  out.insert(out.end(), it->second.begin(), it->second.end());
  (void)uav;
}

static std::shared_ptr<EclReads> PublicExecute(UINT count, ID3D12CommandList *const *lists) {
  if (!Enabled() || !count) return nullptr;
  const DWORD error = GetLastError();
  LARGE_INTEGER t0{}, t1{}; QueryPerformanceCounter(&t0);
  auto r = std::make_shared<EclReads>();
  r->frame = g_public_frames.load(std::memory_order_relaxed);
  r->on = FrameOn(r->frame);
  if (r->on && Reads()) {
    try {
      std::map<std::tuple<const void *, uint32_t, uint32_t>, std::vector<ReadSub>> cache;
      std::vector<ReadSub> mine;
      for (UINT l = 0; l < count; ++l)
        for (auto p = static_cast<MTLD3D12GraphicsCommandList *>(lists[l])->entry; p; p = p->next) {
          if (p->type != EncoderType::Render) continue;
          auto &render = *static_cast<RenderEncoderData *>(p);
          PassReads pr; pr.pass = p;
          if (!render.xl_hazard) { pr.flags = 8; r->passes.push_back(pr); continue; }
          const auto &b = *render.xl_hazard;
          mine.clear();
          for (const auto &snap : b.snapshots) {
            if (snap.flags) pr.flags |= 1;
            for (uint32_t k = 0; k < snap.span_count; ++k) {
              const auto &span = b.spans[snap.first_span + k];
              if (!span.declared) pr.flags |= 2;
              for (uint32_t j = 0; j < span.decl_count; ++j) {
                const auto &iv = b.decls[span.first_decl + j];
                ScanInterval(span.heap, iv.start, iv.count, span.uav, cache, mine, pr.flags);
              }
            }
          }
          std::sort(mine.begin(), mine.end());
          mine.erase(std::unique(mine.begin(), mine.end()), mine.end());
          pr.first = uint32_t(r->subs.size()); pr.count = uint32_t(mine.size());
          r->subs.insert(r->subs.end(), mine.begin(), mine.end());
          r->passes.push_back(pr);
        }
    } catch (...) { r->passes.clear(); r->subs.clear(); }
  }
  QueryPerformanceCounter(&t1);
  r->ticks = uint64_t(t1.QuadPart - t0.QuadPart);
  SetLastError(error);
  return r;
}

// ---------------------------------------------------------------------------------------------------------------
// Encoding side (the thread that encodes / commits, in submission order).
enum Kind : uint8_t { KRender, KClear, KBlit, KCompute, KResolve, KArgument, KAccel, KPresent, KCount };
static const char *const kKindName[KCount] = {"R", "CL", "B", "C", "RS", "A", "AS", "P"};
enum Dep : uint8_t { DepSrv, DepCopy, DepBarrier, DepUnknown, DepWar, DepEcl, DepLater, DepAttach, DepNone, DepFrame,
                     DepCount };
static const char *const kDepName[DepCount] = {"srv", "copy", "barrier", "unknown", "war", "ecl", "later", "attach", "none",
                                                "frame"};
enum Cause : uint8_t { CauseFrame, CauseSignal, CauseEcl, CauseList, CauseClear, CauseQuery, CauseResolve, CauseType,
                       CauseTargets, CauseMerge, CausePresent, CauseCount };
static const char *const kCauseName[CauseCount] = {"frame", "signal", "ecl", "list", "clear", "query", "resolve", "type",
                                                    "targets", "merge", "present"};

struct Att {
  const Texture *texture = nullptr;
  obj_handle_t object = 0;
  uint32_t level = 0, slice = 0, plane = 0, w = 0, h = 0, layers = 1;
  uint32_t format = 0;
  uint64_t bytes = 0;
  uint8_t aspect = 0, slot = 0, load = 0, store = 0, written = 1, samples = 1;
};
struct Enc {
  uint8_t kind = KRender, why = 0, svc = 0;
  bool mem_write = false, w_unknown = false, r_unknown = false, timestamp = false, valid = false;
  uint32_t ecl = 0, list = 0, scope = 0, passes = 0, draws = 0, signal_epoch = 0;
  uint64_t index = 0;
  std::vector<Att> att;
  std::vector<obj_handle_t> wobj, robj;            // copy / clear / resolve objects (writes, reads)
  std::vector<ReadSub> subs;                       // render: what its draws can read (READS)
  bool reads_known = false;
  uint64_t sig = 0;                                // render: targets signature (equal = SameRenderTargets)
  std::string clear_key;                           // clear: why it is its own encoder, the barrier that forced it
  uint8_t close_reason = 0;                        // recorded close reason of its last pass (encoder_stats::Reason)
  bool queries = false;                            // render: counts occlusion queries in the pass (direct slots)
};
struct Agg {
  uint64_t n = 0, rt = 0, store = 0, load = 0, dep[DepCount] = {}, cause[CauseCount] = {}, why[WhyCount] = {};
};
struct Window {
  uint64_t frames = 0, encoders[KCount] = {}, boundaries = 0, worker_ticks = 0, public_ticks = 0, executes = 0;
  uint64_t store_bytes = 0, load_bytes = 0, rt_bytes = 0, inside_barriers = 0, merged_passes = 0, folded_clears = 0;
  uint64_t clears = 0, clear_bytes = 0, clear_next_load = 0, clear_next_load_bytes = 0, clear_next_read = 0;
  uint64_t clear_overwritten = 0, clear_unused = 0, clear_next_other = 0;
  uint64_t sandwich = 0, sandwich_rt = 0, sandwich_between = 0, sandwich_blocked = 0, sandwich_blocked_rt = 0;
  uint64_t svc_occlusion = 0, svc_out_join = 0, reads_unknown_passes = 0, reads_passes = 0, reads_subs = 0;
  std::map<std::string, Agg> classes;
  std::map<std::string, std::pair<uint64_t, uint64_t>> clear_keys;   // key + next use -> (clears, bytes)
  // near-empty passes (VS-STAGE 23.10: a pass with 0-1 draws still costs a vertex phase and its load/store):
  // draws per render-type encoder (0, 1, 2-4, 5-16, 17+) and, for 0-1 draws, origin key -> (encoders, load+store bytes)
  uint64_t draw_hist[5] = {};
  std::map<std::string, std::pair<uint64_t, uint64_t>> empties;
  void Clear() { *this = Window{}; }
};
struct PendingBarriers { std::vector<BarrierRecord> recs; bool invalid = false; uint32_t passes = 0; };
struct ClearTrack { uint64_t enc = 0, bytes = 0; uint8_t state = 0; std::string key; };
struct QueueStream {
  bool have_prev = false, have_cur = false;
  Enc prev, cur;
  PendingBarriers between, pending;      // between(prev, cur); after cur's last pass
  // sandwich: last closed render encoder and what followed it
  bool have_last_render = false;
  Enc last_render;
  std::vector<Enc> since;
  std::vector<BarrierRecord> span_barriers;
  bool span_invalid = false;
  std::unordered_map<obj_handle_t, ClearTrack> clears;
  uint64_t frame = UINT64_MAX;
  uint64_t index = 0;
  uint32_t ecl = 0, signal_epoch = 0;
};
struct State {
  std::mutex mutex;
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> out_join{0};
  std::unordered_map<const void *, QueueStream> queues;
  Window window, total;
  uint64_t detail_rows = 0;
};
static State &Get() { static State s; return s; }

static void Line(const char *format, ...) {
  char line[4096];
  va_list args; va_start(args, format);
  const int n = std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  DWORD written;
  if (n > 0 && size_t(n) < sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
}

static uint32_t AspectBytes(uint32_t format, uint8_t aspect) {
  const uint32_t f = uint32_t(ORIGINAL_FORMAT(WMTPixelFormat(format)));
  switch (f) {
  case WMTPixelFormatDepth16Unorm: return 2;
  case WMTPixelFormatDepth32Float: return 4;
  case WMTPixelFormatStencil8: return 1;
  case WMTPixelFormatDepth24Unorm_Stencil8: return aspect == 2 ? 1 : 3;
  case WMTPixelFormatDepth32Float_Stencil8: return aspect == 2 ? 1 : 4;
  case WMTPixelFormatX32_Stencil8: case WMTPixelFormatX24_Stencil8: return 1;
  default: return MTLGetTexelSize(WMTPixelFormat(format));
  }
}
static bool IsStore(uint8_t s) { return s == WMTStoreActionStore || s == WMTStoreActionStoreAndMultisampleResolve ||
                                        s == WMTStoreActionUnknown || s == WMTStoreActionCustomSampleDepthStore; }
template <typename A>
static void AddAtt(Enc &e, const A &a, uint8_t aspect, uint8_t slot, uint8_t load, uint8_t store, bool written,
                   uint32_t w, uint32_t h, uint32_t layers) {
  if (!a.attachment) return;
  Att x;
  x.texture = xl_hazard_observer::ViewTexture(a.attachment);
  x.object = AttachmentObject(a.attachment);
  x.level = uint32_t(TextureViewKey(uint64_t(a.attachment->key)).mip_start) + a.level;
  x.slice = uint32_t(TextureViewKey(uint64_t(a.attachment->key)).array_start) + a.slice;
  x.plane = a.depth_plane;
  x.format = x.texture ? uint32_t(const_cast<Texture *>(x.texture)->pixelFormat()) : 0;
  x.aspect = aspect; x.slot = slot; x.load = load; x.store = store; x.written = written;
  const unsigned samples = x.texture ? const_cast<Texture *>(x.texture)->sampleCount() : 1u;
  x.w = w; x.h = h; x.layers = std::max(1u, layers); x.samples = uint8_t(std::max(1u, samples));
  x.bytes = uint64_t(w) * h * x.layers * x.samples * AspectBytes(x.format, aspect);
  e.att.push_back(x);
}
static void BlitObjects(const BlitEncoderData &b, Enc &e) {
  for (auto cmd = reinterpret_cast<const wmtcmd_base *>(b.cmd_head.next.ptr); cmd;
       cmd = static_cast<const wmtcmd_base *>(cmd->next.ptr)) {
    switch (WMTBlitCommandType(cmd->type)) {
    case WMTBlitCommandCopyFromBufferToBuffer: {
      auto c = reinterpret_cast<const wmtcmd_blit_copy_from_buffer_to_buffer *>(cmd); e.robj.push_back(c->src); e.wobj.push_back(c->dst); break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      auto c = reinterpret_cast<const wmtcmd_blit_copy_from_buffer_to_texture *>(cmd); e.robj.push_back(c->src); e.wobj.push_back(c->dst); break;
    }
    case WMTBlitCommandCopyFromBufferToTextureWithBlitOption: {
      auto c = reinterpret_cast<const wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *>(cmd); e.robj.push_back(c->src); e.wobj.push_back(c->dst); break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      auto c = reinterpret_cast<const wmtcmd_blit_copy_from_texture_to_texture *>(cmd); e.robj.push_back(c->src); e.wobj.push_back(c->dst); break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      auto c = reinterpret_cast<const wmtcmd_blit_copy_from_texture_to_buffer *>(cmd); e.robj.push_back(c->src); e.wobj.push_back(c->dst); break;
    }
    case WMTBlitCommandCopyFromTextureToBufferWithBlitOption: {
      auto c = reinterpret_cast<const wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *>(cmd); e.robj.push_back(c->src); e.wobj.push_back(c->dst); break;
    }
    case WMTBlitCommandCopyTexture: {
      auto c = reinterpret_cast<const wmtcmd_blit_copy_texture *>(cmd); e.robj.push_back(c->src); e.wobj.push_back(c->dst); break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      auto c = reinterpret_cast<const wmtcmd_blit_generate_mipmaps *>(cmd); e.robj.push_back(c->texture); e.wobj.push_back(c->texture); break;
    }
    case WMTBlitCommandFillBuffer: {
      auto c = reinterpret_cast<const wmtcmd_blit_fillbuffer *>(cmd); e.wobj.push_back(c->buffer); break;
    }
    case WMTBlitCommandResolveCounters: {
      auto c = reinterpret_cast<const wmtcmd_blit_resolvecounters *>(cmd); e.wobj.push_back(c->dst_buffer); break;
    }
    case WMTBlitCommandNop: case WMTBlitCommandWaitForFence: case WMTBlitCommandUpdateFence:
      break;
    default:
      e.w_unknown = true; e.r_unknown = true; break;
    }
  }
  for (const auto &q : b.query_buffers) e.wobj.push_back(q.handle);
  for (const auto &q : b.immediate_buffers) e.wobj.push_back(q.handle);
  if (b.resolve_destination) e.wobj.push_back(b.resolve_destination.handle);
}
// The fields SameRenderTargets compares, hashed: equal signatures = the same Metal render pass descriptor.
static uint64_t TargetsSig(const RenderEncoderData &r) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&](uint64_t v) { for (unsigned i = 0; i < 8; ++i) { h ^= (v >> (8 * i)) & 0xff; h *= 1099511628211ull; } };
  mix(r.render_target_width); mix(r.render_target_height); mix(r.render_target_array_length);
  mix(r.default_raster_sample_count); mix(r.dsv_planar_flags);
  for (unsigned i = 0; i < r.colors.size(); ++i) {
    const auto &c = r.colors[i];
    mix(i); mix(c.attachment ? c.attachment.texture().handle : 0);
    if (!c.attachment) continue;
    mix(c.level); mix(c.slice); mix(c.depth_plane);
    if (c.resolve_attachment) mix(reinterpret_cast<uintptr_t>(&r));   // a resolving pass never continues another
  }
  mix(r.depth.attachment ? r.depth.attachment.texture().handle : 0); mix(r.depth.level); mix(r.depth.slice); mix(r.depth.depth_plane);
  mix(r.stencil.attachment ? r.stencil.attachment.texture().handle : 0); mix(r.stencil.level); mix(r.stencil.slice);
  mix(r.stencil.depth_plane);
  return h;
}
static Enc MakeEnc(EncoderData *p, const PassReads *reads, const EclReads *rd) {
  Enc e; e.valid = true; e.why = p->census_why; e.passes = 1; e.close_reason = p->close_reason;
  switch (p->type) {
  case EncoderType::Render: {
    auto &r = *static_cast<RenderEncoderData *>(p);
    e.kind = KRender; e.sig = TargetsSig(r); e.svc = r.census_svc; e.mem_write = r.may_write_memory;
    e.queries = !r.direct_slots.empty() || !r.visibility_targets.empty();
    if (r.xl_hazard) e.draws = r.xl_hazard->draws;
    const unsigned ro = r.dsv_readonly_flags | r.xl_dsv_flags;
    const uint32_t w = r.render_target_width, h = r.render_target_height, layers = r.render_target_array_length;
    for (unsigned k = 0; k < r.colors.size(); ++k)
      AddAtt(e, r.colors[k], 0, uint8_t(k), r.census_load[k], r.census_store[k], true, w, h, layers);
    AddAtt(e, r.depth, 1, 8, r.census_load[8], r.census_store[8], !(ro & D3D12_DSV_FLAG_READ_ONLY_DEPTH), w, h, layers);
    AddAtt(e, r.stencil, 2, 9, r.census_load[9], r.census_store[9], !(ro & D3D12_DSV_FLAG_READ_ONLY_STENCIL), w, h, layers);
    if (reads && rd && !(reads->flags & (1 | 8))) {
      e.reads_known = true;
      e.subs.assign(rd->subs.begin() + reads->first, rd->subs.begin() + reads->first + reads->count);
    }
    e.r_unknown = !e.reads_known;
    break;
  }
  case EncoderType::Clear: {
    auto &c = *static_cast<ClearEncoderData *>(p);
    e.kind = KClear;
    struct { TextureViewRef attachment; uint16_t level = 0, slice = 0; uint32_t depth_plane = 0; } a;
    a.attachment = c.attachment; a.depth_plane = c.depth_plane;
    if (c.clear_dsv) {
      if (c.clear_dsv & 1) AddAtt(e, a, 1, 8, WMTLoadActionClear, WMTStoreActionStore, true, c.width, c.height, c.array_length);
      if (c.clear_dsv & 2) AddAtt(e, a, 2, 9, WMTLoadActionClear, WMTStoreActionStore, true, c.width, c.height, c.array_length);
    } else {
      AddAtt(e, a, 0, 0, WMTLoadActionClear, WMTStoreActionStore, true, c.width, c.height, c.array_length);
    }
    if (const auto o = AttachmentObject(c.attachment)) e.wobj.push_back(o);
    {
      const TextureViewKey key(uint64_t(c.attachment ? uint64_t(c.attachment->key) : 0));
      const Texture *t = xl_hazard_observer::ViewTexture(c.attachment);
      char text[320];
      std::snprintf(text, sizeof(text), "why=%s asp=%s fmt=%u size=%ux%ux%u view=m%u-%u/a%u-%u tex=%ux%u bar=%s:%x->%x sub=%d bflags=%u",
                    kWhyName[e.why < WhyCount ? e.why : 0], c.clear_dsv == 3 ? "ds" : c.clear_dsv == 1 ? "d" : c.clear_dsv == 2 ? "s" : "c",
                    t ? unsigned(const_cast<Texture *>(t)->pixelFormat()) : 0u, c.width, c.height, c.array_length,
                    unsigned(key.mip_start), unsigned(key.mip_end), unsigned(key.array_start), unsigned(key.array_end),
                    t ? const_cast<Texture *>(t)->miplevelCount() : 0u, t ? const_cast<Texture *>(t)->arrayLength() : 0u,
                    c.census_bar_type == 0xff ? "none" : c.census_bar_type == 0xfe ? "unnamed" :
                    c.census_bar_type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ? "T" :
                    c.census_bar_type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? "U" : "A",
                    c.census_bar_before, c.census_bar_after,
                    c.census_bar_sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? -1 : int(c.census_bar_sub), unsigned(c.census_bar_flags));
      e.clear_key = text;
    }
    break;
  }
  case EncoderType::Blit: {
    auto &b = *static_cast<BlitEncoderData *>(p);
    e.kind = KBlit; e.timestamp = bool(b.timestamp_samples) || bool(b.resolve_samples);
    BlitObjects(b, e);
    break;
  }
  case EncoderType::Compute:
    e.kind = KCompute; e.w_unknown = true; e.r_unknown = true; e.mem_write = true;
    break;
  case EncoderType::Resolve: {
    auto &r = *static_cast<ResolveEncoderData *>(p);
    e.kind = KResolve;
    if (const auto o = AttachmentObject(r.src)) e.robj.push_back(o);
    if (const auto o = AttachmentObject(r.dst)) e.wobj.push_back(o);
    break;
  }
  case EncoderType::ArgumentCompute:
    e.kind = KArgument; e.w_unknown = true; e.r_unknown = true; e.mem_write = true;
    break;
  case EncoderType::AccelerationBuild:
    e.kind = KAccel; e.w_unknown = true; e.r_unknown = true; e.mem_write = true;
    break;
  default:
    e.valid = false;
    break;
  }
  return e;
}

static bool Has(const std::vector<obj_handle_t> &v, obj_handle_t o) { return o && std::find(v.begin(), v.end(), o) != v.end(); }
static bool WritesObject(const Enc &e, obj_handle_t o) {
  if (!o) return false;
  for (const auto &a : e.att) if (a.written && a.object == o) return true;
  return Has(e.wobj, o);
}
static bool UsesObject(const Enc &e, obj_handle_t o) {
  if (!o) return false;
  for (const auto &a : e.att) if (a.object == o) return true;
  return Has(e.wobj, o) || Has(e.robj, o);
}
// May the left side read this object (for write-after-read)? Unknown reads count as yes.
static bool MayRead(const Enc &e, obj_handle_t o) {
  if (e.r_unknown) return true;
  if (Has(e.robj, o)) return true;
  for (const auto &s : e.subs) if (s.object == o) return true;
  for (const auto &a : e.att) if (a.object == o && a.load == WMTLoadActionLoad) return true;
  return false;
}
static bool SubHitsAtt(const ReadSub &s, const Att &a) {
  if (!a.written) return false;
  if (s.texture && a.texture) {
    if (s.texture != a.texture) return false;
    const bool mip = a.level >= s.mip0 && a.level < s.mip1;
    const bool slice = a.slice >= s.a0 && a.slice < s.a1;
    const uint8_t plane = a.aspect == 2 ? 1 : 0;
    return mip && slice && plane == s.plane;
  }
  return s.object && s.object == a.object;
}
constexpr uint32_t kWriteStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                                  D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
                                  D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;

struct Verdict { uint8_t dep = DepNone, cause = CauseType; uint64_t rt = 0, store = 0, load = 0; bool same = false, share = false; };

static Verdict Judge(const Enc &l, const Enc &r, const PendingBarriers &between) {
  Verdict v;
  for (const auto &a : l.att) if (IsStore(a.store)) v.store += a.bytes;
  for (const auto &a : r.att) if (a.load == WMTLoadActionLoad) v.load += a.bytes;
  bool attach = false;
  for (const auto &b : r.att)
    for (const auto &a : l.att)
      if (a.object == b.object && a.level == b.level && a.slice == b.slice && a.plane == b.plane && a.aspect == b.aspect) {
        v.share = true;
        if (IsStore(a.store) && b.load == WMTLoadActionLoad) { v.rt += b.bytes; attach = true; }
      }
  if (l.kind == KRender && r.kind == KRender) v.same = l.sig == r.sig;
  // barriers between the two encoders
  bool on_write = false, uav = false, alias = false, unknown = between.invalid, war = false;
  std::vector<obj_handle_t> named_writes;   // objects the barriers say the left side wrote (incl. UAV-before when unknown)
  for (const auto &rec : between.recs) {
    switch (rec.type) {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
      if (!rec.object) { unknown = true; break; }
      if (WritesObject(l, rec.object)) { on_write = true; named_writes.push_back(rec.object); }
      else if ((l.w_unknown || l.mem_write) && (rec.before & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        uav = true; named_writes.push_back(rec.object);
      }
      if ((rec.after & kWriteStates) && !(rec.before & kWriteStates) && MayRead(l, rec.object)) war = true;
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
      if (l.w_unknown || l.mem_write || (rec.object && WritesObject(l, rec.object))) {
        uav = true; if (rec.object) named_writes.push_back(rec.object);
      }
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
      alias = true;
      break;
    default:
      unknown = true;
      break;
    }
  }
  // does the right side read what the left side wrote?
  bool srv = false, copy = false;
  if (r.kind == KRender && r.reads_known)
    for (const auto &s : r.subs) {
      for (const auto &a : l.att) if (SubHitsAtt(s, a)) { srv = true; break; }
      if (!srv && (Has(l.wobj, s.object) || Has(named_writes, s.object))) srv = true;
      if (srv) break;
    }
  for (const auto o : r.robj) if (WritesObject(l, o) || Has(named_writes, o)) copy = true;
  bool later = false;
  if (srv) v.dep = DepSrv;
  else if (copy) v.dep = DepCopy;
  else if (on_write && r.r_unknown) v.dep = DepBarrier;
  else if ((uav || alias || unknown) && r.r_unknown) v.dep = DepUnknown;
  else if (war) v.dep = DepWar;
  else if (r.ecl != l.ecl) v.dep = DepEcl;
  else if ((later = on_write || uav || alias || unknown)) v.dep = DepLater;
  else if (attach) v.dep = DepAttach;
  else v.dep = DepNone;
  (void)later;
  // why the boundary exists (primary cause)
  if (r.signal_epoch != l.signal_epoch) v.cause = CauseSignal;
  else if (r.ecl != l.ecl) v.cause = CauseEcl;
  else if (r.scope != l.scope) v.cause = CauseList;
  else if (l.kind == KClear || r.kind == KClear) v.cause = CauseClear;
  else if (l.timestamp || r.timestamp || l.svc || r.svc) v.cause = CauseQuery;
  else if (l.kind == KResolve || r.kind == KResolve) v.cause = CauseResolve;
  else if (r.kind == KPresent) v.cause = CausePresent;
  else if (l.kind != r.kind) v.cause = CauseType;
  else if (l.kind == KRender && !v.same) v.cause = CauseTargets;
  else if (l.kind == KRender) v.cause = CauseMerge;
  else v.cause = CauseType;   // two blit / compute encoders in a row
  return v;
}
static std::string ClassName(const Enc &l, const Enc &r, const Verdict &v) {
  std::string s = kKindName[l.kind];
  s += "_"; s += kKindName[r.kind];
  if (l.kind == KRender && r.kind == KRender) s += v.same ? "_same" : v.share ? "_share" : "_diff";
  return s;
}
static void AddAgg(Agg &a, const Verdict &v, uint8_t why) {
  ++a.n; a.rt += v.rt; a.store += v.store; a.load += v.load; ++a.dep[v.dep]; ++a.cause[v.cause];
  ++a.why[why < WhyCount ? why : 0];
}

static void Detail(uint64_t frame, const Enc &e, const Enc *l, const Verdict *v) {
  auto &st = Get();
  if (st.detail_rows >= 4096) return;
  ++st.detail_rows;
  std::string atts;
  for (const auto &a : e.att) {
    char t[160];
    std::snprintf(t, sizeof(t), "%s%u:%llx/f%u/%ux%ux%u/m%u/s%u/p%u/L%uS%u/w%u/%lluB", atts.empty() ? "" : ",", unsigned(a.slot),
                  (unsigned long long)a.object, a.format, a.w, a.h, a.layers, a.level, a.slice, a.plane, unsigned(a.load),
                  unsigned(a.store), unsigned(a.written), (unsigned long long)a.bytes);
    atts += t;
  }
  std::string objs;
  for (auto o : e.wobj) { if (objs.size() > 900) break; char t[32]; std::snprintf(t, sizeof(t), "%sw%llx", objs.empty() ? "" : ",", (unsigned long long)o); objs += t; }
  for (auto o : e.robj) { if (objs.size() > 1800) break; char t[32]; std::snprintf(t, sizeof(t), "%sr%llx", objs.empty() ? "" : ",", (unsigned long long)o); objs += t; }
  if (e.kind == KClear) { objs += " key="; objs += e.clear_key; }
  Line("dx12_enc_census_enc frame=%llu idx=%llu ecl=%u list=%u scope=%u kind=%s why=%s passes=%u draws=%u svc=%u mem_write=%u "
       "reads=%s/%zu cls=%s dep=%s cause=%s rt=%llu store=%llu load=%llu att=[%s] obj=[%s]\n",
       (unsigned long long)frame, (unsigned long long)e.index, e.ecl, e.list, e.scope, kKindName[e.kind],
       kWhyName[e.why < WhyCount ? e.why : 0], e.passes, e.draws, unsigned(e.svc), unsigned(e.mem_write),
       e.kind != KRender ? "-" : e.reads_known ? "known" : "unknown", e.subs.size(),
       l && v ? ClassName(*l, e, *v).c_str() : "start", l && v ? kDepName[v->dep] : "frame", l && v ? kCauseName[v->cause] : "frame",
       (unsigned long long)(v ? v->rt : 0), (unsigned long long)(v ? v->store : 0), (unsigned long long)(v ? v->load : 0),
       atts.c_str(), objs.c_str());
}

// Next use of a texture a clear encoder wrote: counted once per clear.
static void ClearOutcome(Window &w, const ClearTrack &t, const char *next) {
  std::string k = t.key + " next=" + next + (t.state & 1 ? "+after_unknown_reader" : "");
  auto &c = w.clear_keys[k]; ++c.first; c.second += t.bytes;
}
static void TrackClears(QueueStream &q, Window &w, const Enc &e) {
  if (q.clears.empty()) return;
  static const char *const kNext[5] = {"none", "load", "read", "overwritten", "other"};
  for (auto it = q.clears.begin(); it != q.clears.end();) {
    const auto o = it->first;
    uint8_t outcome = 0;   // 1 load, 2 read, 3 overwritten, 4 other use
    if (e.kind == KClear && Has(e.wobj, o)) outcome = 3;
    else if (e.kind == KRender) {
      for (const auto &a : e.att) if (a.object == o) { outcome = a.load == WMTLoadActionLoad ? 1 : 3; break; }
      if (!outcome) for (const auto &s : e.subs) if (s.object == o) { outcome = 2; break; }
    } else if (UsesObject(e, o)) outcome = Has(e.robj, o) ? 2 : 4;
    if (!outcome) {
      if (e.r_unknown && e.kind != KClear) it->second.state |= 1;   // a reader of unknown reads passed by
      ++it; continue;
    }
    ClearOutcome(w, it->second, kNext[outcome]);
    if (outcome == 1) { ++w.clear_next_load; w.clear_next_load_bytes += it->second.bytes; for (const auto &a : e.att) if (a.object == o && a.load == WMTLoadActionLoad) w.clear_next_load_bytes += a.bytes; }
    else if (outcome == 2) ++w.clear_next_read;
    else if (outcome == 3) ++w.clear_overwritten;
    else ++w.clear_next_other;
    it = q.clears.erase(it);
  }
}

// Sandwich: render encoder R1, other encoders E, render encoder R2 with the same targets; could E go after R2 (then R1
// and R2 would be adjacent)? Conservative: no barrier of the span names an attachment of R1, E writes none of them,
// R2 cannot read (or, with unknown reads, no barrier announces) what E wrote, no query/timestamp/clear in E, no
// command-buffer split by Signal / ECL between R1 and R2, and R2 does not write memory while E writes.
static void Sandwich(QueueStream &q, Window &w, const Enc &r2) {
  if (!q.have_last_render || q.since.empty() || q.last_render.sig != r2.sig) return;
  const auto &r1 = q.last_render;
  uint64_t rt = 0;
  for (const auto &b : r2.att)
    for (const auto &a : r1.att)
      if (a.object == b.object && a.level == b.level && a.slice == b.slice && a.plane == b.plane && a.aspect == b.aspect &&
          IsStore(a.store) && b.load == WMTLoadActionLoad)
        rt += b.bytes;
  bool ok = !q.span_invalid && r2.ecl == r1.ecl && r2.signal_epoch == r1.signal_epoch;
  bool e_writes = false;
  std::vector<obj_handle_t> e_written;
  for (const auto &e : q.since) {
    if (e.kind == KClear || e.kind == KRender || e.kind == KPresent || e.timestamp || e.svc) ok = false;
    for (const auto &a : r1.att) if (WritesObject(e, a.object) || Has(e.robj, a.object)) ok = false;
    e_writes |= e.w_unknown || !e.wobj.empty();
    e_written.insert(e_written.end(), e.wobj.begin(), e.wobj.end());
  }
  bool e_unknown = false;
  for (const auto &e : q.since) e_unknown |= e.w_unknown;
  for (const auto &rec : q.span_barriers) {
    if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !rec.object) { ok = false; continue; }
    for (const auto &a : r1.att) if (a.object == rec.object) ok = false;
    // something E wrote is announced to readers: R2 may read it (unless its reads are known and miss it)
    const bool e_wrote = Has(e_written, rec.object) || (e_unknown && (rec.before & D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    if (e_wrote) {
      if (!r2.reads_known) ok = false;
      else for (const auto &s : r2.subs) if (s.object == rec.object) ok = false;
    }
    if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_UAV && e_unknown && !r2.reads_known) ok = false;
  }
  if (r2.reads_known) for (const auto &s : r2.subs) if (Has(e_written, s.object)) ok = false;
  if (r2.mem_write && e_writes) ok = false;
  if (ok) { ++w.sandwich; w.sandwich_rt += rt; w.sandwich_between += q.since.size(); }
  else { ++w.sandwich_blocked; w.sandwich_blocked_rt += rt; }
}

// The encoder `q.cur` is complete: judge its boundary with `q.prev`.
static void CloseCur(QueueStream &q, Window &w, uint64_t frame, bool detail) {
  if (!q.have_cur) return;
  auto &e = q.cur;
  ++w.encoders[e.kind];
  for (const auto &a : e.att) { if (IsStore(a.store)) w.store_bytes += a.bytes; if (a.load == WMTLoadActionLoad) w.load_bytes += a.bytes; }
  if (e.kind == KRender && e.svc) w.svc_occlusion += __builtin_popcount(e.svc);
  Verdict v; bool judged = false;
  if (q.have_prev) {
    v = Judge(q.prev, e, q.between);
    judged = true;
    ++w.boundaries; w.rt_bytes += v.rt;
    AddAgg(w.classes[ClassName(q.prev, e, v)], v, e.why);
    AddAgg(w.classes[std::string("cause_") + kCauseName[v.cause]], v, e.why);
  } else {
    Verdict f; f.dep = DepFrame; f.cause = CauseFrame;
    AddAgg(w.classes["frame_start"], f, e.why);
  }
  if (detail) Detail(frame, e, q.have_prev ? &q.prev : nullptr, judged ? &v : nullptr);
  if (e.kind == KRender || e.kind == KClear || e.kind == KResolve) {
    const uint32_t d = e.kind == KRender ? e.draws : 0;
    ++w.draw_hist[d == 0 ? 0 : d == 1 ? 1 : d <= 4 ? 2 : d <= 16 ? 3 : 4];
    if (d <= 1) {
      unsigned colors = 0; bool depth = false, stencil = false; uint64_t bytes = 0; uint32_t aw = 0, ah = 0;
      for (const auto &a : e.att) {
        if (a.aspect == 0) ++colors; else if (a.aspect == 1) depth = true; else stencil = true;
        if (IsStore(a.store)) bytes += a.bytes;
        if (a.load == WMTLoadActionLoad) bytes += a.bytes;
        aw = a.w; ah = a.h;
      }
      char key[256];
      std::snprintf(key, sizeof(key), "draws=%u kind=%s why=%s close=%c queries=%u att=%uc%s%s/%ux%u prev_same=%u cls=%s",
                    d, kKindName[e.kind], kWhyName[e.why < WhyCount ? e.why : 0],
                    encoder_stats::kReasonLetter[e.close_reason < encoder_stats::ReasonCount ? e.close_reason : 0],
                    unsigned(e.queries), colors, depth ? "d" : "", stencil ? "s" : "", aw, ah,
                    unsigned(q.have_prev && q.prev.kind == KRender && e.kind == KRender && q.prev.sig == e.sig),
                    judged ? ClassName(q.prev, e, v).c_str() : "start");
      auto &x = w.empties[key]; ++x.first; x.second += bytes;
    }
  }
  TrackClears(q, w, e);
  if (e.kind == KClear) {
    ++w.clears;
    uint64_t bytes = 0; for (const auto &a : e.att) bytes += a.bytes;
    w.clear_bytes += bytes;
    for (auto o : e.wobj) {
      auto old = q.clears.find(o);
      if (old != q.clears.end()) { ClearOutcome(w, old->second, "overwritten"); ++w.clear_overwritten; }
      q.clears[o] = ClearTrack{e.index, bytes, 0, e.clear_key};
    }
  }
  // sandwich bookkeeping
  if (e.kind == KRender) {
    Sandwich(q, w, e);
    q.have_last_render = true; q.last_render = e; q.since.clear(); q.span_barriers.clear(); q.span_invalid = false;
  } else if (q.have_last_render) {
    q.since.push_back(e);
  }
  q.prev = std::move(q.cur); q.have_prev = true; q.have_cur = false;
  q.between = std::move(q.pending); q.pending = PendingBarriers{};
}
static void OpenCur(QueueStream &q, Enc &&e) {
  q.cur = std::move(e); q.have_cur = true;
}
static void BeginFrame(QueueStream &q, uint64_t frame) {
  if (q.frame == frame) return;
  q = QueueStream{};
  q.frame = frame;
}

// After the lists of one ExecuteCommandLists were encoded (early or now) on the encoding thread, in order.
// scopes[i]: Metal command buffer ordinal of list i in this call.
static void Executed(const void *queue, UINT count, ID3D12CommandList *const *lists, const std::vector<uint32_t> &scopes) {
  if (!Enabled() || !count) return;
  auto handover = std::move(t_handover);
  t_handover.reset();
  auto &st = Get();
  const uint64_t frame = st.frames.load(std::memory_order_relaxed);
  if (!FrameOn(frame)) return;
  const DWORD error = GetLastError();
  LARGE_INTEGER t0{}, t1{}; QueryPerformanceCounter(&t0);
  std::lock_guard lock(st.mutex);
  try {
    auto &q = st.queues[queue];
    BeginFrame(q, frame);
    ++st.window.executes;
    const bool detail = frame == DetailFrame();
    const uint32_t ecl = ++q.ecl;
    size_t cursor = 0;
    const EclReads *rd = handover && handover->frame == frame ? handover.get() : nullptr;
    if (handover) st.window.public_ticks += handover->ticks;
    for (UINT l = 0; l < count; ++l) {
      const uint32_t scope = l < scopes.size() ? scopes[l] : 0;
      for (auto p = static_cast<MTLD3D12GraphicsCommandList *>(lists[l])->entry; p; p = p->next) {
        if (p->type == EncoderType::Null) continue;
        if (p->census_begin == BeginYes) {
          const PassReads *reads = rd && p->type == EncoderType::Render ? rd->Find(p, cursor) : nullptr;
          if (p->type == EncoderType::Render) {
            ++st.window.reads_passes;
            if (!reads || (reads->flags & (1 | 8))) ++st.window.reads_unknown_passes;
            else st.window.reads_subs += reads->count;
          }
          Enc e = MakeEnc(p, reads, rd);
          if (!e.valid) continue;
          e.ecl = ecl; e.list = l; e.scope = scope; e.index = q.index++; e.signal_epoch = q.signal_epoch;
          CloseCur(q, st.window, frame, detail);
          OpenCur(q, std::move(e));
          if (p->type == EncoderType::Blit && static_cast<BlitEncoderData *>(p)->barrier_only) {
            // an encoded barrier pass (OUT join): its barriers order this encoder and the next one
            auto &b = *static_cast<BlitEncoderData *>(p);
            if (!b.barrier_records_valid) { q.pending.invalid = true; q.span_invalid = true; }
            q.pending.recs.insert(q.pending.recs.end(), b.barrier_records.begin(), b.barrier_records.end());
            q.span_barriers.insert(q.span_barriers.end(), b.barrier_records.begin(), b.barrier_records.end());
          }
          continue;
        }
        // not an encoder of its own
        if (p->type == EncoderType::Blit && static_cast<BlitEncoderData *>(p)->barrier_only) {
          auto &b = *static_cast<BlitEncoderData *>(p);
          if (!b.barrier_records_valid) { q.pending.invalid = true; q.span_invalid = true; }
          q.pending.recs.insert(q.pending.recs.end(), b.barrier_records.begin(), b.barrier_records.end());
          q.span_barriers.insert(q.span_barriers.end(), b.barrier_records.begin(), b.barrier_records.end());
          ++q.pending.passes;
          continue;
        }
        if (p->census_why == WhyMerged && q.have_cur && q.cur.kind == KRender) {
          // continuation of the open encoder: barriers since its last pass are inside it
          st.window.inside_barriers += q.pending.passes;
          q.pending = PendingBarriers{};
          ++st.window.merged_passes;
          auto &r = *static_cast<RenderEncoderData *>(p);
          ++q.cur.passes;
          q.cur.mem_write |= r.may_write_memory;
          q.cur.close_reason = r.close_reason;
          q.cur.queries |= !r.direct_slots.empty() || !r.visibility_targets.empty();
          if (r.xl_hazard) q.cur.draws += r.xl_hazard->draws;
          const PassReads *reads = rd ? rd->Find(p, cursor) : nullptr;
          ++st.window.reads_passes;
          if (!reads || (reads->flags & (1 | 8))) { ++st.window.reads_unknown_passes; q.cur.reads_known = false; q.cur.r_unknown = true; }
          else {
            st.window.reads_subs += reads->count;
            q.cur.subs.insert(q.cur.subs.end(), rd->subs.begin() + reads->first, rd->subs.begin() + reads->first + reads->count);
          }
          continue;
        }
        if (p->census_why == WhyFolded) { ++st.window.folded_clears; continue; }
        if (p->census_why == WhyBlitMerged && q.have_cur && q.cur.kind == KBlit) {
          // ENC-FIX BLIT_RUN: this copy pass shares the open blit encoder
          Enc c = MakeEnc(p, nullptr, nullptr);
          q.cur.wobj.insert(q.cur.wobj.end(), c.wobj.begin(), c.wobj.end());
          q.cur.robj.insert(q.cur.robj.end(), c.robj.begin(), c.robj.end());
          ++q.cur.passes; st.window.inside_barriers += q.pending.passes; q.pending = PendingBarriers{};
          continue;
        }
        if (p->census_why == WhyClearBatched && q.have_cur && q.cur.kind == KClear) {
          // ENC-FIX CLEAR_BATCH: this clear shares the open clear encoder
          Enc c = MakeEnc(p, nullptr, nullptr);
          q.cur.att.insert(q.cur.att.end(), c.att.begin(), c.att.end());
          q.cur.wobj.insert(q.cur.wobj.end(), c.wobj.begin(), c.wobj.end());
          ++q.cur.passes;
          continue;
        }
      }
    }
  } catch (...) {}
  QueryPerformanceCounter(&t1);
  st.window.worker_ticks += uint64_t(t1.QuadPart - t0.QuadPart);
  SetLastError(error);
}
static void Signalled(const void *queue) {
  if (!Enabled()) return;
  auto &st = Get();
  if (!FrameOn(st.frames.load(std::memory_order_relaxed))) return;
  std::lock_guard lock(st.mutex);
  ++st.queues[queue].signal_epoch;
}
static void NoteOutJoin() { if (Enabled()) Get().out_join.fetch_add(1, std::memory_order_relaxed); }

static void AddWindow(Window &t, const Window &w) {
  t.frames += w.frames; for (unsigned k = 0; k < KCount; ++k) t.encoders[k] += w.encoders[k];
  t.boundaries += w.boundaries; t.worker_ticks += w.worker_ticks; t.public_ticks += w.public_ticks; t.executes += w.executes;
  t.store_bytes += w.store_bytes; t.load_bytes += w.load_bytes; t.rt_bytes += w.rt_bytes; t.inside_barriers += w.inside_barriers;
  t.merged_passes += w.merged_passes; t.folded_clears += w.folded_clears; t.clears += w.clears; t.clear_bytes += w.clear_bytes;
  t.clear_next_load += w.clear_next_load; t.clear_next_load_bytes += w.clear_next_load_bytes; t.clear_next_read += w.clear_next_read;
  t.clear_overwritten += w.clear_overwritten; t.clear_unused += w.clear_unused; t.clear_next_other += w.clear_next_other;
  t.sandwich += w.sandwich; t.sandwich_rt += w.sandwich_rt; t.sandwich_between += w.sandwich_between;
  t.sandwich_blocked += w.sandwich_blocked; t.sandwich_blocked_rt += w.sandwich_blocked_rt;
  t.svc_occlusion += w.svc_occlusion; t.svc_out_join += w.svc_out_join; t.reads_unknown_passes += w.reads_unknown_passes;
  t.reads_passes += w.reads_passes; t.reads_subs += w.reads_subs;
  for (const auto &[k, c] : w.clear_keys) { auto &d = t.clear_keys[k]; d.first += c.first; d.second += c.second; }
  for (const auto &[k, c] : w.empties) { auto &d = t.empties[k]; d.first += c.first; d.second += c.second; }
  for (unsigned i = 0; i < 5; ++i) t.draw_hist[i] += w.draw_hist[i];
  for (const auto &[k, a] : w.classes) {
    auto &b = t.classes[k];
    b.n += a.n; b.rt += a.rt; b.store += a.store; b.load += a.load;
    for (unsigned i = 0; i < DepCount; ++i) b.dep[i] += a.dep[i];
    for (unsigned i = 0; i < CauseCount; ++i) b.cause[i] += a.cause[i];
    for (unsigned i = 0; i < WhyCount; ++i) b.why[i] += a.why[i];
  }
}
static void Report(const char *scope, const Window &w, uint64_t presents) {
  LARGE_INTEGER hz{}; QueryPerformanceFrequency(&hz);
  Line("dx12_enc_census scope=%s presents=%llu frames=%llu every=%u reads=%u executes=%llu boundaries=%llu enc_R=%llu enc_CL=%llu "
       "enc_B=%llu enc_C=%llu enc_RS=%llu enc_A=%llu enc_AS=%llu enc_P=%llu svc_occlusion=%llu svc_out_join=%llu "
       "store_bytes=%llu load_bytes=%llu rt_bytes=%llu merged_passes=%llu inside_barriers=%llu folded_clears=%llu "
       "worker_us=%.1f public_us=%.1f reads_passes=%llu reads_unknown_passes=%llu reads_subs=%llu\n",
       scope, (unsigned long long)presents, (unsigned long long)w.frames, Every(), unsigned(Reads()),
       (unsigned long long)w.executes, (unsigned long long)w.boundaries,
       (unsigned long long)w.encoders[KRender], (unsigned long long)w.encoders[KClear], (unsigned long long)w.encoders[KBlit],
       (unsigned long long)w.encoders[KCompute], (unsigned long long)w.encoders[KResolve], (unsigned long long)w.encoders[KArgument],
       (unsigned long long)w.encoders[KAccel], (unsigned long long)w.encoders[KPresent], (unsigned long long)w.svc_occlusion,
       (unsigned long long)w.svc_out_join, (unsigned long long)w.store_bytes, (unsigned long long)w.load_bytes,
       (unsigned long long)w.rt_bytes, (unsigned long long)w.merged_passes, (unsigned long long)w.inside_barriers,
       (unsigned long long)w.folded_clears, hz.QuadPart ? 1e6 * double(w.worker_ticks) / double(hz.QuadPart) : 0.0,
       hz.QuadPart ? 1e6 * double(w.public_ticks) / double(hz.QuadPart) : 0.0, (unsigned long long)w.reads_passes,
       (unsigned long long)w.reads_unknown_passes, (unsigned long long)w.reads_subs);
  Line("dx12_enc_census_extra scope=%s frames=%llu clears=%llu clear_bytes=%llu clear_next_load=%llu clear_next_load_bytes=%llu "
       "clear_next_read=%llu clear_overwritten=%llu clear_next_other=%llu clear_unused=%llu sandwich=%llu sandwich_rt=%llu "
       "sandwich_between=%llu sandwich_blocked=%llu sandwich_blocked_rt=%llu\n",
       scope, (unsigned long long)w.frames, (unsigned long long)w.clears, (unsigned long long)w.clear_bytes,
       (unsigned long long)w.clear_next_load, (unsigned long long)w.clear_next_load_bytes, (unsigned long long)w.clear_next_read,
       (unsigned long long)w.clear_overwritten, (unsigned long long)w.clear_next_other, (unsigned long long)w.clear_unused,
       (unsigned long long)w.sandwich, (unsigned long long)w.sandwich_rt, (unsigned long long)w.sandwich_between,
       (unsigned long long)w.sandwich_blocked, (unsigned long long)w.sandwich_blocked_rt);
  Line("dx12_enc_census_draws scope=%s frames=%llu d0=%llu d1=%llu d2_4=%llu d5_16=%llu d17p=%llu\n", scope,
       (unsigned long long)w.frames, (unsigned long long)w.draw_hist[0], (unsigned long long)w.draw_hist[1],
       (unsigned long long)w.draw_hist[2], (unsigned long long)w.draw_hist[3], (unsigned long long)w.draw_hist[4]);
  for (const auto &[k, c] : w.empties)
    Line("dx12_enc_census_empty scope=%s frames=%llu n=%llu bytes=%llu %s\n", scope, (unsigned long long)w.frames,
         (unsigned long long)c.first, (unsigned long long)c.second, k.c_str());
  for (const auto &[k, c] : w.clear_keys)
    Line("dx12_enc_census_clear scope=%s frames=%llu n=%llu bytes=%llu %s\n", scope, (unsigned long long)w.frames,
         (unsigned long long)c.first, (unsigned long long)c.second, k.c_str());
  for (const auto &[k, a] : w.classes) {
    std::string tail;
    for (unsigned i = 0; i < DepCount; ++i)
      if (a.dep[i]) { char t[48]; std::snprintf(t, sizeof(t), " dep_%s=%llu", kDepName[i], (unsigned long long)a.dep[i]); tail += t; }
    for (unsigned i = 0; i < CauseCount; ++i)
      if (a.cause[i]) { char t[48]; std::snprintf(t, sizeof(t), " cause_%s=%llu", kCauseName[i], (unsigned long long)a.cause[i]); tail += t; }
    for (unsigned i = 0; i < WhyCount; ++i)
      if (a.why[i]) { char t[64]; std::snprintf(t, sizeof(t), " why_%s=%llu", kWhyName[i], (unsigned long long)a.why[i]); tail += t; }
    Line("dx12_enc_census_class scope=%s frames=%llu cls=%s n=%llu rt=%llu store=%llu load=%llu%s\n", scope,
         (unsigned long long)w.frames, k.c_str(), (unsigned long long)a.n, (unsigned long long)a.rt,
         (unsigned long long)a.store, (unsigned long long)a.load, tail.c_str());
  }
}
// One Present encoded on `queue` (the encoding thread): the present pass ends the frame of that queue.
static void Presented(const void *queue, obj_handle_t backbuffer) {
  if (!Enabled()) return;
  auto &st = Get();
  const DWORD error = GetLastError();
  {
    std::lock_guard lock(st.mutex);
    const uint64_t frame = st.frames.load(std::memory_order_relaxed);
    if (FrameOn(frame)) {
      try {
        auto &q = st.queues[queue];
        BeginFrame(q, frame);
        Enc p; p.valid = true; p.kind = KPresent; p.ecl = ++q.ecl; p.index = q.index++; p.signal_epoch = q.signal_epoch;
        if (backbuffer) p.robj.push_back(backbuffer);
        const bool detail = frame == DetailFrame();
        CloseCur(q, st.window, frame, detail);
        OpenCur(q, std::move(p));
        CloseCur(q, st.window, frame, detail);
        // clears of every queue that were never used again in this frame
        for (auto &[k, other] : st.queues) {
          if (other.frame != frame) continue;
          if (&other != &q) CloseCur(other, st.window, frame, detail);
          st.window.clear_unused += other.clears.size();
          for (const auto &[o, t] : other.clears) ClearOutcome(st.window, t, "unused_in_frame");
          other.clears.clear();
        }
        ++st.window.frames;
        st.window.svc_out_join += st.out_join.exchange(0, std::memory_order_relaxed);
      } catch (...) {}
    }
    const uint64_t n = st.frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % Period() == 0) {
      Report("window", st.window, n);
      AddWindow(st.total, st.window);
      Report("total", st.total, n);
      st.window.Clear();
    }
  }
  SetLastError(error);
}

} // namespace enc_census_observer
