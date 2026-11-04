// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// LIGHTRN 02.11.2026: the encode-time half of MACRUNNER_DX12_LIGHT_RENAME (rules: d3d12_light_rename.hpp). Included by
// d3d12_command_queue.cpp at namespace scope. Plan() finds the runs in the lists of one encoding, Apply() builds the redirected
// tables / root blocks into pooled memory and patches the recorded passes; Applied restores every patched word when the
// encoding is done (the recorded lists stay as recorded).

namespace light_rename {   // (included inside namespace dxmt)

// ---- pool: copies of a texture (its own allocations) and memory blocks for tables / root blocks ------------------------------
inline unsigned Bpp(Texture *t) {   // estimate by Metal format class
  const unsigned f = unsigned(t->pixelFormat());
  return f < 30 ? 1 : f < 50 ? 2 : f < 100 ? 4 : f < 120 ? 8 : f < 130 ? 16 : 8;
}
inline uint64_t TexBytes(Texture *t) {
  const unsigned bpp = Bpp(t);
  uint64_t sum = 0;
  for (unsigned m = 0; m < t->miplevelCount(); ++m)
    sum += uint64_t(std::max(t->width() >> m, 1u)) * std::max(t->height() >> m, 1u) * std::max(t->depth() >> m, 1u) *
           std::max(t->arrayLength(), 1u) * bpp;
  return sum;
}
struct TexCopy {
  Rc<Texture> tex;
  Rc<TextureAllocation> alloc;
  uint64_t bytes = 0;
  MTLD3D12Device *device = nullptr;
  uint64_t last = 0;
  ~TexCopy() {
    if (device && alloc) device->UnregisterResidency(alloc->texture());
    PoolBytes(-int64_t(bytes));
    Get().pool_entries.fetch_sub(1, std::memory_order_relaxed);
  }
};
struct Block {
  Rc<Buffer> buf;
  Rc<BufferAllocation> alloc;
  uint8_t *cpu = nullptr;
  uint64_t gpu = 0;
  obj_handle_t handle = 0;
  size_t size = 0;
  ~Block() { PoolBytes(-int64_t(size)); }
};
constexpr size_t kBlockSize = 256 * 1024;
class Pool {
  dxmt::mutex m_;
  std::vector<std::shared_ptr<TexCopy>> tex_;
  std::vector<std::shared_ptr<Block>> blocks_;
  uint64_t clock_ = 0;
  void Trim() {   // copies unused for many acquisitions (their texture may be gone) give their memory back
    if (++clock_ % 256) return;
    tex_.erase(std::remove_if(tex_.begin(), tex_.end(), [&](const std::shared_ptr<TexCopy> &e) {
      return e.use_count() == 1 && clock_ - e->last > 4096;
    }), tex_.end());
  }
public:
  // A free copy of `t` (same texture info), or a new one within the cap; null = cap reached / allocation failed.
  std::shared_ptr<TexCopy> Copy(MTLD3D12Device *dev, Texture *t) {
    std::lock_guard<dxmt::mutex> lock(m_);
    Trim();
    for (auto &e : tex_)
      if (e->tex.ptr() == t && e.use_count() == 1) { e->last = clock_; return e; }
    const uint64_t bytes = TexBytes(t);
    if (Get().pool_bytes.load(std::memory_order_relaxed) + bytes > uint64_t(E().pool_mb) * 1048576ull) return nullptr;
    auto e = std::make_shared<TexCopy>();
    Flags<TextureAllocationFlag> flags;
    flags.set(TextureAllocationFlag::CpuInvisible);
    e->alloc = t->allocate(flags);
    if (!e->alloc || !e->alloc->texture()) { e->alloc = nullptr; e->bytes = 0; Get().pool_entries.fetch_add(1); return nullptr; }
    e->tex = t; e->bytes = bytes; e->device = dev; e->last = clock_;
    dev->RegisterResidency(e->alloc->texture());
    PoolBytes(int64_t(bytes));
    Get().pool_entries.fetch_add(1, std::memory_order_relaxed);
    tex_.push_back(e);
    return e;
  }
  std::shared_ptr<Block> Memory(MTLD3D12Device *dev) {
    std::lock_guard<dxmt::mutex> lock(m_);
    for (auto &b : blocks_)
      if (b.use_count() == 1) return b;
    if (blocks_.size() >= 64) return nullptr;
    auto b = std::make_shared<Block>();
    b->buf = new Buffer(kBlockSize, dev->GetMTLDevice());
    b->alloc = b->buf->allocate(Flags<BufferAllocationFlag>{});
    if (!b->alloc || !b->alloc->buffer() || !b->alloc->hasMappedMemory()) return nullptr;
    b->cpu = static_cast<uint8_t *>(b->alloc->mappedMemory(0));
    b->gpu = b->alloc->gpuAddress();
    b->handle = b->alloc->buffer().handle;
    b->size = kBlockSize;
    PoolBytes(int64_t(kBlockSize));
    blocks_.push_back(b);
    return b;
  }
};
inline Pool &ThePool() { static Pool p; return p; }

// Bump allocation in pooled blocks for one encoding (the leases keep the blocks until the command buffer completes).
struct Writer {
  MTLD3D12Device *dev;
  std::vector<std::shared_ptr<void>> &leases;
  std::shared_ptr<Block> cur;
  size_t at = 0;
  uint64_t bytes = 0;
  bool Alloc(size_t n, uint8_t *&cpu, uint64_t &gpu, obj_handle_t &handle, uint64_t &offset) {
    n = (n + 63) & ~size_t(63);
    if (n > kBlockSize) return false;
    if (!cur || at + n > cur->size) {
      cur = ThePool().Memory(dev);
      if (!cur) return false;
      leases.push_back(cur);
      at = 0;
    }
    cpu = cur->cpu + at; gpu = cur->gpu + at; handle = cur->handle; offset = at;
    at += n; bytes += n;
    return true;
  }
};

// ---- what an encoding changed (restored when the encoding is done) ------------------------------------------------------------
struct Snap { Rc<BufferAllocation> keep; uint64_t offset = 0; uint32_t value = 0; };
// A descriptor range an early encoding read or copied: hash of its 32-byte GPU records (they change with every rewrite of a descriptor).
struct DescSnap { MTLD3D12DescriptorHeap *heap = nullptr; uint32_t first = 0, count = 0; uint64_t hash = 0; };
inline uint64_t HashRecords(MTLD3D12DescriptorHeap *heap, uint32_t first, uint32_t count) {
  const auto *p = static_cast<const uint64_t *>(heap->GPURecords());
  if (!p) return 0;
  p += size_t(first) * 4;
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < size_t(count) * 4; ++i) { h ^= p[i]; h *= 0x100000001b3ull; h ^= h >> 29; }
  return h;
}
// Collector of the current early encoding (encoding thread only); null: nothing to keep (an encoding at ExecuteCommandLists).
inline thread_local std::vector<DescSnap> *t_descs = nullptr;
inline void NoteDescs(ID3D12DescriptorHeap *heap_object, uint32_t first, uint32_t count) {
  if (!t_descs || !heap_object || !count) return;
  auto *heap = static_cast<MTLD3D12DescriptorHeap *>(heap_object);
  for (const auto &d : *t_descs) if (d.heap == heap && d.first == first && d.count == count) return;
  t_descs->push_back({heap, first, count, HashRecords(heap, first, count)});
}
struct Stash {   // an early (Close-time) encoding: kept with the list until its commit
  std::vector<std::shared_ptr<void>> leases;
  std::vector<Snap> snaps;
  std::vector<DescSnap> descs;
};
inline bool DescsHold(const std::vector<DescSnap> &descs) {
  for (const auto &d : descs) if (HashRecords(d.heap, d.first, d.count) != d.hash) return false;
  return true;
}
struct Applied {
  std::vector<std::pair<EncoderData *, EncoderType>> types;
  std::vector<std::pair<WMTMemoryPointer *, void *>> nexts;
  struct Buf { obj_handle_t *buffer; uint64_t *offset; obj_handle_t b; uint64_t o; };
  std::vector<Buf> bufs;
  std::vector<ComputeEncoderData *> concurrent;
  std::vector<std::shared_ptr<void>> leases;
  std::vector<Snap> snaps;
  std::vector<DescSnap> descs;   // early encodings only
  // copy-in runs: the blit commands (consumed when the pass is encoded) and the passes that carry them for this encoding
  std::vector<std::vector<wmtcmd_blit_copy_from_texture_to_texture>> blits;
  std::vector<ComputeEncoderData *> copyin;
  void Restore() {
    for (auto it = bufs.rbegin(); it != bufs.rend(); ++it) { *it->buffer = it->b; *it->offset = it->o; }
    for (auto it = nexts.rbegin(); it != nexts.rend(); ++it) it->first->set(it->second);
    for (auto it = types.rbegin(); it != types.rend(); ++it) it->first->type = it->second;
    for (auto *c : concurrent) c->lightrn_concurrent = 0;
    for (auto *c : copyin) c->lightrn_copyin = nullptr;
    bufs.clear(); nexts.clear(); types.clear(); concurrent.clear(); copyin.clear();
  }
  ~Applied() { Restore(); }
};
// true: every word an early encoding relied on still holds the value it read
inline bool SnapsHold(const std::vector<Snap> &snaps) {
  for (const auto &s : snaps) {
    if (!s.keep || !s.keep->hasMappedMemory()) return false;
    uint32_t v = 0;
    memcpy(&v, static_cast<const uint8_t *>(s.keep->mappedMemory(0)) + s.offset, 4);
    if (v != s.value) return false;
  }
  return true;
}

// ---- planning ----------------------------------------------------------------------------------------------------------------
struct Covered {
  std::array<uint64_t, 16> slices{};   // per mip: slices fully written
  bool Has(const TextureViewKey &k) const {
    if (k.mip_end > 16 || k.array_end > 64 || k.mip_end <= k.mip_start || k.array_end <= k.array_start) return false;
    const uint64_t want = (k.array_end - k.array_start >= 64 ? ~uint64_t(0) : ((uint64_t(1) << (k.array_end - k.array_start)) - 1))
                          << k.array_start;
    for (unsigned m = k.mip_start; m < k.mip_end; ++m) if ((slices[m] & want) != want) return false;
    return true;
  }
  bool Contains(const Covered &o) const {
    for (unsigned m = 0; m < 16; ++m) if ((slices[m] & o.slices[m]) != o.slices[m]) return false;
    return true;
  }
};
struct Cand {
  ComputeEncoderData *pass = nullptr;
  const DispatchRec *d = nullptr;
  Texture *S = nullptr;
  std::vector<TextureViewKey> views;         // per form UAV
  std::vector<uint32_t> uav_params;          // root parameters (tables) holding the UAVs
  Covered covered;
  std::vector<const Texture *> inputs;
  std::vector<obj_handle_t> input_handles;
  std::vector<Snap> snaps;                   // CB words the coverage proof read (re-checked at commit of an early encoding)
  // copy-in (03.11): some view of S is not proven fully written, the form is pure; `sig` = what fixes the written texel set
  // (form, grid, views, the values of every CB word its store conditions read): a copy-in run joins only equal signatures
  bool copyin = false;
  std::vector<uint64_t> sig;
};
struct Entry { EncoderData *pass = nullptr; int cand = -1; };   // cand >= 0: the candidate it is; else a member pass

inline MTLD3D12DescriptorHeap *HeapOf(ID3D12DescriptorHeap *h) { return static_cast<MTLD3D12DescriptorHeap *>(h); }
inline const Texture *ViewTexture(const TextureViewRef &v) { return v && v->allocation ? v->allocation->descriptor : nullptr; }

// CB words of a candidate dispatch: root constants, root CBV or a CBV table entry; only UPLOAD-heap memory is read.
struct FetchCtx {
  MTLD3D12Device *dev; const DispatchRec *d; std::vector<Snap> *snaps;
  unsigned fail = RCount;
  struct Word { uint32_t space, reg, word, value; };
  std::vector<Word> cache;   // a guard reads the same words for every view
};
inline bool FetchCbRead(FetchCtx &ctx, uint32_t space, uint32_t reg, uint32_t word, uint32_t &out);
inline bool FetchCb(void *ctxp, uint32_t space, uint32_t reg, uint32_t word, uint32_t &out) {
  auto &ctx = *static_cast<FetchCtx *>(ctxp);
  for (const auto &x : ctx.cache) if (x.space == space && x.reg == reg && x.word == word) { out = x.value; return true; }
  const bool ok = FetchCbRead(ctx, space, reg, word, out);
  if (ok) ctx.cache.push_back({space, reg, word, out});
  return ok;
}
inline bool FetchCbRead(FetchCtx &ctx, uint32_t space, uint32_t reg, uint32_t word, uint32_t &out) {
  const auto *root = ctx.d->root;
  const auto &layout = root->ArgumentLayout;
  const auto &params = root->reuse_parameters;
  uint64_t va = 0, size = UINT64_MAX;
  bool found = false;
  for (uint32_t i = 0; i < layout.count && !found; ++i) {
    const auto &p = params[i]; const auto &l = layout.parameters[i];
    if (l.kind == root_argument::Constants && p.binding == reg && p.space == space) {
      if (word >= l.count) { ctx.fail = RCbUnresolved; return false; }
      memcpy(&out, reinterpret_cast<const uint8_t *>(ctx.d->words + l.qword) + word * 4, 4);
      return true;
    }
    if (l.kind == root_argument::CBV && p.binding == reg && p.space == space) { va = ctx.d->words[l.qword]; found = true; break; }
    if (l.kind != root_argument::Table || l.heap_type != 0) continue;
    for (const auto &r : p.ranges) {
      if (r.type != 2 || r.space != space || reg < r.binding || (r.count != UINT32_MAX && reg - r.binding >= r.count)) continue;
      root_argument::TableLocation loc;
      if (!ctx.d->heap_object || !root_argument::Locate(ctx.d->heap, ctx.d->words[l.qword], r.offset + (reg - r.binding), 1, loc)) {
        ctx.fail = RCbUnresolved; return false;
      }
      NoteDescs(ctx.d->heap_object, loc.index, 1);
      const auto &desc = HeapOf(ctx.d->heap_object)->GetDescriptor(loc.index);
      if (desc.type != ShaderVisibleDescriptorType::ConstantBuffer) { ctx.fail = RCbUnresolved; return false; }
      va = desc.ConstantBuffer.address; size = desc.ConstantBuffer.size; found = true; break;
    }
  }
  if (!found || !va || uint64_t(word) * 4 + 4 > size) { ctx.fail = RCbUnresolved; return false; }
  Snap s;
  if (!ctx.dev->LightrnReadUpload(va + uint64_t(word) * 4, 4, &s.value, &s.keep, &s.offset)) { ctx.fail = RCbHeap; return false; }
  out = s.value;
  ctx.snaps->push_back(std::move(s));
  return true;
}

// The first PARTIAL_LOG candidates whose views are not all proven fully written: what the proof saw (03.11, Elden Ring LRS2:
// partial 8.9/frame with no values). One `dx12_lightrn_partial` line each: S (size, mips, slices, Metal type / format), the
// dispatch (groups, numthreads, grid), every UAV view (mips, slices, 1 written / 0 not proven / s shape) and the CB words the
// store conditions read (s.b.w=value, ? = unreadable).
inline void LogPartial(const char *why, Texture *S, const DispatchRec &d, const form::Form &f, const std::vector<TextureViewKey> &views,
                       const std::vector<int8_t> &cov, const std::vector<uint32_t> &words, const std::vector<uint8_t> &have,
                       const uint64_t grid[3]) {
  static std::atomic<unsigned> printed{0};
  const unsigned limit = E().partial_log;
  if (!limit || printed.load(std::memory_order_relaxed) >= limit) return;
  const unsigned n = printed.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > limit) return;
  char b[2048];
  int at = snprintf(b, sizeof(b), "dx12_lightrn_partial n=%u why=%s pure=%d stores=%zu/%u s=%ux%u mips=%u slices=%u type=%u fmt=%u"
                    " groups=%u,%u,%u threads=%u,%u,%u grid=%llu,%llu,%llu views=", n, why, int(f.pure), f.stores.size(), f.store_total,
                    S->width(), S->height(), S->miplevelCount(), S->arrayLength(), unsigned(S->textureType()), unsigned(S->pixelFormat()),
                    d.groups[0], d.groups[1], d.groups[2], d.threads[0], d.threads[1], d.threads[2], (unsigned long long)grid[0],
                    (unsigned long long)grid[1], (unsigned long long)grid[2]);
  for (size_t u = 0; u < views.size() && at > 0 && size_t(at) < sizeof(b) - 64; ++u)
    at += snprintf(b + at, sizeof(b) - size_t(at), "%sm%u-%ua%u-%u:%c", u ? ";" : "", unsigned(views[u].mip_start), unsigned(views[u].mip_end),
                   unsigned(views[u].array_start), unsigned(views[u].array_end), cov[u] > 0 ? '1' : cov[u] < 0 ? 's' : '0');
  if (at > 0 && size_t(at) < sizeof(b) - 64) at += snprintf(b + at, sizeof(b) - size_t(at), " cb=");
  for (size_t i = 0; i < f.cb_words.size() && at > 0 && size_t(at) < sizeof(b) - 64; ++i) {
    if (have[i]) at += snprintf(b + at, sizeof(b) - size_t(at), "%s%u.%u.%u=%u", i ? ";" : "", f.cb_words[i].space, f.cb_words[i].reg,
                                f.cb_words[i].word, words[i]);
    else at += snprintf(b + at, sizeof(b) - size_t(at), "%s%u.%u.%u=?", i ? ";" : "", f.cb_words[i].space, f.cb_words[i].reg, f.cb_words[i].word);
  }
  if (at < 0) return;
  if (size_t(at) > sizeof(b) - 2) at = int(sizeof(b) - 2);
  b[at++] = '\n'; b[at] = 0;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  DWORD wr = 0;
  if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, b, DWORD(at), &wr, nullptr);
}

// A candidate compute pass, or the reason it is not one.
inline unsigned Evaluate(MTLD3D12Device *dev, ComputeEncoderData &c, unsigned key, Cand &out) {
  auto &snaps = out.snaps;
  auto *pd = c.lightrn.get();
  if (!pd || pd->d.empty()) return RNoRecord;
  if (pd->unknown || pd->dispatches != 1 || pd->d.size() != 1 || c.query_pipeline) return RMulti;
  const auto &d = pd->d[0];
  if (!d.form || !d.form->ok) return RForm;
  auto *root = d.root;
  if (!root) return RTableUnresolved;   // volatile descriptors: read at encoding, re-checked at commit of an early encoding
  const auto &layout = root->ArgumentLayout;
  const auto &params = root->reuse_parameters;
  if (params.size() != layout.count || !d.heap_object || !d.words) return RTableUnresolved;
  auto *heap = HeapOf(d.heap_object);
  const auto &f = *d.form;
  Texture *S = nullptr;
  out.views.assign(f.uavs.size(), TextureViewKey{});
  for (uint32_t u = 0; u < f.uavs.size(); ++u) {
    const auto &res = f.uavs[u];
    if (res.count != 1) return RUavUnresolved;
    bool found = false;
    for (uint32_t i = 0; i < layout.count && !found; ++i) {
      const auto &l = layout.parameters[i];
      if (l.kind != root_argument::Table || l.heap_type != 0) continue;
      for (const auto &r : params[i].ranges) {
        if (r.type != 1 || r.space != res.space || res.lower < r.binding || (r.count != UINT32_MAX && res.lower - r.binding >= r.count)) continue;
        root_argument::TableLocation loc;
        if (!root_argument::Locate(d.heap, d.words[l.qword], r.offset + (res.lower - r.binding), 1, loc)) return RUavUnresolved;
        NoteDescs(d.heap_object, loc.index, 1);
        const auto &desc = heap->GetDescriptor(loc.index);
        if (desc.type != ShaderVisibleDescriptorType::UAVTexture || !desc.UAVTexture.texture) return RUavNotTexture;
        if (!S) S = desc.UAVTexture.texture;
        else if (desc.UAVTexture.texture != S) return RUavMixed;
        out.views[u] = desc.UAVTexture.view;
        if (std::find(out.uav_params.begin(), out.uav_params.end(), i) == out.uav_params.end()) out.uav_params.push_back(i);
        found = true; break;
      }
    }
    if (!found) return RUavUnresolved;
  }
  if (!S || S->miplevelCount() > 16 || S->arrayLength() > 64) return RUavNotTexture;
  const uint64_t grid[3] = {uint64_t(d.groups[0]) * d.threads[0], uint64_t(d.groups[1]) * d.threads[1], uint64_t(d.groups[2]) * d.threads[2]};
  FetchCtx ctx{dev, &d, &snaps};
  std::vector<int8_t> cov(f.uavs.size(), 0);   // 1 every texel written, 0 not proven, -1 view shape not modelled
  bool partial = false, shape = false;
  for (uint32_t u = 0; u < f.uavs.size(); ++u) {
    const auto &k = out.views[u];
    if (k.mip_end != k.mip_start + 1 || k.mip_start >= 16 || k.array_end <= k.array_start || k.array_end > 64) {
      cov[u] = -1; shape = true; continue;
    }
    const unsigned w = std::max(S->width() >> k.mip_start, 1u), h = std::max(S->height() >> k.mip_start, 1u);
    const unsigned layers = k.array_end - k.array_start;
    if (key != 9 && !form::Covers(f, u, w, h, layers, grid, FetchCb, &ctx)) {
      if (ctx.fail != RCount) return ctx.fail;
      partial = true; continue;
    }
    cov[u] = 1;
    const uint64_t mask = (layers >= 64 ? ~uint64_t(0) : ((uint64_t(1) << layers) - 1)) << k.array_start;
    out.covered.slices[k.mip_start] |= mask;
  }
  if (partial || shape) {
    // Copy-in (03.11): with a pure form, the texels this dispatch writes are fixed by (form, grid, views, the CB words its
    // conditions read). A run of candidates with EQUAL such signatures writes the same texel set W in every batch, so a texel
    // outside W keeps its pre-run value through the whole run: S_j := pre-run S on every subresource not fully written, then Cj,
    // gives S_j exactly the content S has after Cj without the key. Readers may then read any view (CheckRender).
    const auto type = S->textureType();
    const bool can = !shape && key == 1 && E().copyin && f.pure && S->sampleCount() <= 1 &&
                     (type == WMTTextureType2D || type == WMTTextureType2DArray || type == WMTTextureTypeCube ||
                      type == WMTTextureTypeCubeArray);
    std::vector<uint32_t> words(f.cb_words.size(), 0);
    std::vector<uint8_t> have(f.cb_words.size(), 0);
    bool all_words = true;
    for (size_t i = 0; i < f.cb_words.size(); ++i) {
      have[i] = FetchCb(&ctx, f.cb_words[i].space, f.cb_words[i].reg, f.cb_words[i].word, words[i]) ? 1 : 0;
      all_words &= have[i] != 0;
    }
    LogPartial(shape ? "view_shape" : !f.pure ? "impure" : key != 1 || !E().copyin ? "copyin_off" : !can ? "texture_kind"
                                    : !all_words ? "cb_words" : "copyin",
               S, d, f, out.views, cov, words, have, grid);
    if (!can) return shape ? RViewShape : RPartial;
    if (!all_words) return ctx.fail != RCount ? ctx.fail : RCbUnresolved;
    out.copyin = true;
    out.sig.push_back(reinterpret_cast<uintptr_t>(d.form.get()));
    for (unsigned a = 0; a < 3; ++a) out.sig.push_back(grid[a]);
    for (const auto &k : out.views)
      out.sig.push_back(uint64_t(k.mip_start) | uint64_t(k.mip_end) << 16 | uint64_t(k.array_start) << 32 | uint64_t(k.array_end) << 48);
    for (const auto w : words) out.sig.push_back(w);
    Note(Get().copyin_cands);
  }
  for (const auto &res : f.srvs) {
    if (res.count > 128) return RSrvUnresolved;
    for (uint32_t reg = res.lower; reg < res.lower + res.count; ++reg) {
      bool found = false;
      for (uint32_t i = 0; i < layout.count && !found; ++i) {
        const auto &l = layout.parameters[i]; const auto &p = params[i];
        if (l.kind == root_argument::SRV && p.binding == reg && p.space == res.space) { found = true; break; }   // a buffer
        if (l.kind != root_argument::Table || l.heap_type != 0) continue;
        for (const auto &r : p.ranges) {
          if (r.type != 0 || r.space != res.space || reg < r.binding || (r.count != UINT32_MAX && reg - r.binding >= r.count)) continue;
          root_argument::TableLocation loc;
          if (!root_argument::Locate(d.heap, d.words[l.qword], r.offset + (reg - r.binding), 1, loc)) return RSrvUnresolved;
          NoteDescs(d.heap_object, loc.index, 1);
          const auto &desc = heap->GetDescriptor(loc.index);
          if ((desc.type == ShaderVisibleDescriptorType::SRVTexture || desc.type == ShaderVisibleDescriptorType::UAVTexture) &&
              desc.SRVTexture.texture) {
            if (desc.SRVTexture.texture == S) return RSrvIsS;
            out.inputs.push_back(desc.SRVTexture.texture);
            auto *a = desc.SRVTexture.texture->current();
            if (a) out.input_handles.push_back(a->texture().handle);
          }
          found = true; break;
        }
      }
      if (!found) return RSrvUnresolved;
    }
  }
  out.pass = &c; out.d = &d; out.S = S;
  return RCount;
}

inline bool PassHasDraws(const RenderEncoderData &r) {
  for (auto cmd = static_cast<const wmtcmd_base *>(r.cmd_head.next.ptr); cmd; cmd = static_cast<const wmtcmd_base *>(cmd->next.ptr)) {
    switch (cmd->type) {
    case WMTRenderCommandDraw: case WMTRenderCommandDrawIndexed: case WMTRenderCommandDrawIndirect:
    case WMTRenderCommandDrawIndexedIndirect: case WMTRenderCommandDrawMeshThreadgroups:
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: case WMTRenderCommandDXMTGeometryDraw:
    case WMTRenderCommandDXMTGeometryDrawIndexed: case WMTRenderCommandDXMTGeometryDrawIndirect:
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: case WMTRenderCommandDXMTTessellationMeshDraw:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: case WMTRenderCommandDispatchThreadsPerTile:
    case WMTRenderCommandExecuteCommandsInBuffer:
      return true;
    default: break;
    }
  }
  return false;
}

// Walk the CBV/SRV/UAV tables of a root block: calls hit(param, table_first_heap_index, extent, k, descriptor) for every entry
// naming S. Returns RCount or a reason the tables cannot be read.
template <typename Hit>
inline unsigned ScanTables(const MTLD3D12RootSignature *root, const uint64_t *words, const root_argument::HeapRange &heap_range,
                           ID3D12DescriptorHeap *heap_object, const Texture *S, Hit &&hit) {
  if (!root || !words) return RTableUnresolved;
  const auto &layout = root->ArgumentLayout;
  const auto &params = root->reuse_parameters;
  if (params.size() != layout.count) return RTableUnresolved;
  for (uint32_t i = 0; i < layout.count; ++i) {
    const auto &l = layout.parameters[i];
    if (l.kind != root_argument::Table || l.heap_type != 0) continue;
    uint32_t extent = 0;
    for (const auto &r : params[i].ranges) {
      if (r.count == UINT32_MAX) return RUnbounded;
      extent = std::max(extent, r.offset + r.count);
    }
    if (!extent) continue;
    const uint64_t va = words[l.qword];
    if (!va) continue;   // an unset table is never read by a valid draw
    root_argument::TableLocation loc;
    if (!heap_object || !root_argument::Locate(heap_range, va, 0, extent, loc)) return RTableUnresolved;
    NoteDescs(heap_object, loc.index, extent);
    auto *heap = HeapOf(heap_object);
    for (uint32_t k = 0; k < extent; ++k) {
      const auto &desc = heap->GetDescriptor(loc.index + k);
      if ((desc.type == ShaderVisibleDescriptorType::SRVTexture || desc.type == ShaderVisibleDescriptorType::UAVTexture) &&
          desc.SRVTexture.texture == S) {
        if (const unsigned why = hit(i, loc.index, extent, k, desc)) return why;
      }
    }
  }
  return RCount;
}

// The first PASS_LOG render passes refused inside a run (03.11, Elden Ring LRS3: runs cut by pass_writes 6.1 + pass_unknown 0.95 per
// frame): one `dx12_lightrn_pass` line each with what refused it and the pass's flags.
inline void LogPass(const RenderEncoderData &r, unsigned why, const char *what, uint32_t param, uint32_t index, int dtype) {
  static std::atomic<unsigned> printed{0};
  const unsigned limit = E().pass_log;
  if (!limit || printed.load(std::memory_order_relaxed) >= limit) return;
  const unsigned n = printed.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > limit) return;
  char b[768];
  int at = snprintf(b, sizeof(b), "dx12_lightrn_pass n=%u why=%s what=%s may_write=%d tess=%d geom=%d mesh_ind=%zu prepass_tail=%d verify=%d"
                    " twin=%d roots=%d roots_unknown=%d nroots=%zu draws=%d colors=%zu param=%d index=%d dtype=%d\n", n, ReasonName(why), what,
                    int(r.may_write_memory), int(r.use_tessellation), int(r.use_geometry), r.mesh_indirect_buffers.size(), int(r.prepass_tail != nullptr),
                    int(r.verify_mesh_draw != nullptr), int(bool(r.twin_draw)), int(bool(r.lightrn_roots)),
                    int(r.lightrn_roots && r.lightrn_roots->unknown), r.lightrn_roots ? r.lightrn_roots->roots.size() : size_t(0),
                    int(PassHasDraws(r)), r.colors.size(), int(param), int(index), dtype);
  if (at <= 0) return;
  if (size_t(at) >= sizeof(b)) { at = int(sizeof(b) - 1); b[at - 1] = '\n'; }
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  DWORD wr = 0;
  if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, b, DWORD(at), &wr, nullptr);
}

// A member render pass (between two candidates): may it stay in the run, reading only covered views of S?
inline unsigned CheckRender(const RenderEncoderData &r, const Texture *S, const Covered &cov, bool copyin) {
  const bool trust = E().uavtrust;   // NEGATIVE CONTROL (probes): UAV-declaring passes treated as non-writers
  if (r.may_write_memory && !trust) { LogPass(r, RPassWrites, "may_write", ~0u, ~0u, -1); return RPassWrites; }
  // Occlusion queries of the pass are allowed: renaming changes neither its pixels nor its depth test. Passes whose draws replay root
  // arguments elsewhere (tessellation pre-pass, geometry / mesh emulation, verify twins) are not.
  if (r.use_tessellation || r.use_geometry || !r.mesh_indirect_buffers.empty() || r.prepass_tail || r.verify_mesh_draw || r.twin_draw) {
    LogPass(r, RPassUnknown, r.use_tessellation ? "tess" : r.use_geometry ? "geom" : "replay", ~0u, ~0u, -1); return RPassUnknown;
  }
  for (unsigned k = 0; k < r.colors.size(); ++k)
    if (ViewTexture(r.colors[k].attachment) == S || ViewTexture(r.colors[k].resolve_attachment) == S) {
      LogPass(r, RPassAttachS, "color_s", k, ~0u, -1); return RPassAttachS;
    }
  if (ViewTexture(r.depth.attachment) == S || ViewTexture(r.stencil.attachment) == S) { LogPass(r, RPassAttachS, "depth_s", ~0u, ~0u, -1); return RPassAttachS; }
  if (!r.lightrn_roots) {
    if (!PassHasDraws(r)) return RCount;
    LogPass(r, RPassUnknown, "no_roots", ~0u, ~0u, -1); return RPassUnknown;
  }
  if (r.lightrn_roots->unknown) { LogPass(r, RPassUnknown, "roots_unknown", ~0u, ~0u, -1); return RPassUnknown; }
  for (const auto &rr : r.lightrn_roots->roots) {
    uint32_t hit_param = ~0u, hit_index = ~0u; int hit_type = -1;
    const unsigned why = ScanTables(rr.root, rr.words, rr.heap, rr.heap_object, S,
        [&](uint32_t param, uint32_t first, uint32_t, uint32_t k, const ShaderVisibleDescriptorCPUStorage &d) -> unsigned {
          hit_param = param; hit_index = first + k; hit_type = int(d.type);
          // (b) 03.11: a UAV descriptor of S in a table of a pass none of whose stages declares a UAV is never accessed (a shader
          // reaches only the UAVs it declares; this layer has no direct heap indexing): neither a write nor a read, not redirected.
          if (d.type == ShaderVisibleDescriptorType::UAVTexture) return r.may_write_memory && !trust ? unsigned(RPassWrites) : 0u;
          return copyin || cov.Has(d.SRVTexture.view) ? 0u : unsigned(RViewUncovered);   // copy-in: every texel is S's
        });
    if (why != RCount) {
      LogPass(r, why, why == RPassWrites ? "uav_s" : why == RViewUncovered ? "srv_uncovered" : "tables", hit_param, hit_index, hit_type);
      return why;
    }
  }
  return RCount;
}

// A copy of the root block `words` whose tables naming S point to copies of those tables naming the allocation `alloc` instead.
// Returns false (nothing allocated is used) when memory runs out. `changed` = some table named S.
inline bool Redirect(Writer &w, const MTLD3D12RootSignature *root, const uint64_t *words, const root_argument::HeapRange &heap_range,
                     ID3D12DescriptorHeap *heap_object, Texture *S, TextureAllocation *alloc, obj_handle_t &handle, uint64_t &offset,
                     bool &changed, uint32_t &tables, bool uav) {
  changed = false; tables = 0;
  std::vector<std::pair<uint32_t, uint64_t>> va_patch;   // qword -> new table address
  uint32_t cur_param = UINT32_MAX; uint8_t *cpu = nullptr; uint64_t gpu = 0;
  bool ok = true;
  const unsigned why = ScanTables(root, words, heap_range, heap_object, S,
      [&](uint32_t param, uint32_t first, uint32_t extent, uint32_t k, const ShaderVisibleDescriptorCPUStorage &d) -> unsigned {
        if (!uav && d.type == ShaderVisibleDescriptorType::UAVTexture) return 0;   // render pass: an undeclared UAV (CheckRender (b))
        if (param != cur_param) {
          cur_param = param;
          const uint8_t *src = static_cast<const uint8_t *>(HeapOf(heap_object)->GPURecords());
          obj_handle_t h; uint64_t o;
          if (!src || !w.Alloc(size_t(extent) * 32, cpu, gpu, h, o)) { ok = false; return RNoMemory; }
          memcpy(cpu, src + size_t(first) * 32, size_t(extent) * 32);
          va_patch.push_back({root->ArgumentLayout.parameters[param].qword, gpu});
          ++tables;
        }
        const uint64_t id = S->view(d.SRVTexture.view, alloc).gpuResourceID;
        memcpy(cpu + size_t(k) * 32, &id, 8);
        return 0;
      });
  if (!ok || (why != RCount)) return false;
  if (va_patch.empty()) return true;
  uint8_t *rcpu; uint64_t rgpu;
  if (!w.Alloc(size_t(root->UploadQwords) * 8, rcpu, rgpu, handle, offset)) return false;
  memcpy(rcpu, words, size_t(root->UploadQwords) * 8);
  for (const auto &[q, va] : va_patch) memcpy(rcpu + size_t(q) * 8, &va, 8);
  changed = true;
  return true;
}

inline void Finish(MTLD3D12Device *dev, std::vector<Cand> &cands, std::vector<Entry> &entries, Applied &out) {
  struct Reset { std::vector<Cand> &c; std::vector<Entry> &e; ~Reset() { c.clear(); e.clear(); } } reset{cands, entries};
  if (cands.size() < 2) { if (cands.size() == 1) NoteReason(RSingle); return; }
  size_t n = cands.size();
  // final state: what earlier batches wrote is rewritten by the last one (S ends as without the key)
  while (n >= 2) {
    bool ok = true;
    for (size_t j = 0; j + 1 < n; ++j) if (!cands[n - 1].covered.Contains(cands[j].covered)) { ok = false; break; }
    if (ok) break;
    NoteReason(RFinalState);
    --n;
  }
  if (n < 2) return;
  Texture *S = cands[0].S;
  std::vector<std::shared_ptr<TexCopy>> copies;
  for (size_t j = 0; j + 1 < n; ++j) {
    auto c = ThePool().Copy(dev, S);
    if (!c) { NoteReason(RPoolFull); n = j + 1; break; }
    copies.push_back(std::move(c));
  }
  if (n < 2) return;
  // build every redirection first; nothing is patched unless all of it fits
  std::vector<std::shared_ptr<void>> leases;
  Writer w{dev, leases};
  struct Patch { obj_handle_t *buffer; uint64_t *offset; obj_handle_t b; uint64_t o; };
  std::vector<Patch> patches;
  uint64_t passes = 0, tables_total = 0, roots_total = 0;
  int cand = -1;
  for (const auto &e : entries) {
    if (e.cand >= 0) {
      cand = e.cand;
      if (size_t(cand) >= n) break;
      if (size_t(cand) + 1 == n) continue;   // the last batch writes S itself
      const auto &c = cands[size_t(cand)];
      obj_handle_t h = 0; uint64_t o = 0; bool changed = false; uint32_t tables = 0;
      if (!Redirect(w, c.d->root, c.d->words, c.d->heap, c.d->heap_object, S, copies[size_t(cand)]->alloc.ptr(), h, o, changed, tables, true) ||
          !changed) { NoteReason(RNoMemory); return; }
      patches.push_back({&c.d->root_cmd->buffer, &c.d->root_cmd->offset, h, o});
      tables_total += tables; ++roots_total;
      continue;
    }
    if (cand < 0 || size_t(cand) + 1 >= n || e.pass->type != EncoderType::Render) continue;
    auto *r = static_cast<RenderEncoderData *>(e.pass);
    if (!r->lightrn_roots) continue;
    bool any = false;
    for (const auto &rr : r->lightrn_roots->roots) {
      obj_handle_t h = 0; uint64_t o = 0; bool changed = false; uint32_t tables = 0;
      if (!Redirect(w, rr.root, rr.words, rr.heap, rr.heap_object, S, copies[size_t(cand)]->alloc.ptr(), h, o, changed, tables, false)) {
        NoteReason(RNoMemory); return;
      }
      if (!changed) continue;
      for (uint8_t k = 0; k < rr.ncmd; ++k) patches.push_back({&rr.cmd[k]->buffer, &rr.cmd[k]->offset, h, o});
      tables_total += tables; ++roots_total; any = true;
    }
    passes += any;
  }
  // copy-in run: S_j := S (its pre-run content) on every subresource the batches do not fully write; one blit encoder right before
  // the run's compute encoder (the queue encodes it; the copies are private allocations nothing else uses)
  std::vector<wmtcmd_blit_copy_from_texture_to_texture> blit;
  uint64_t copy_bytes = 0;
  if (cands[0].copyin && !E().nocopyin) {
    auto *src = S->current();
    if (!src || !src->texture()) { NoteReason(RNoMemory); return; }
    const obj_handle_t sh = src->texture().handle;
    const unsigned mips = std::min(S->miplevelCount(), 16u), slices = std::min(S->arrayLength(), 64u), bpp = Bpp(S);
    const auto &cov = cands[n - 1].covered;   // equal for every candidate of a copy-in run (equal signatures)
    for (size_t j = 0; j + 1 < n; ++j) {
      const obj_handle_t dh = copies[j]->alloc->texture().handle;
      for (unsigned m = 0; m < mips; ++m) {
        const unsigned w = std::max(S->width() >> m, 1u), h = std::max(S->height() >> m, 1u);
        for (unsigned a = 0; a < slices; ++a) {
          if ((cov.slices[m] >> a) & 1) continue;
          wmtcmd_blit_copy_from_texture_to_texture c{};
          c.type = WMTBlitCommandCopyFromTextureToTexture;
          c.src = sh; c.src_slice = a; c.src_level = m; c.src_origin = {0, 0, 0}; c.src_size = {w, h, 1};
          c.dst = dh; c.dst_slice = a; c.dst_level = m; c.dst_origin = {0, 0, 0};
          blit.push_back(c);
          copy_bytes += uint64_t(w) * h * bpp;
        }
      }
    }
  }
  // apply: all computes of the run in the first candidate's pass (one concurrent encoder), the others become empty
  if (!blit.empty()) {
    out.blits.push_back(std::move(blit));
    auto &v = out.blits.back();
    for (size_t i = 0; i < v.size(); ++i) v[i].next.set(i + 1 < v.size() ? static_cast<void *>(&v[i + 1]) : nullptr);
    cands[0].pass->lightrn_copyin = reinterpret_cast<const wmtcmd_base *>(v.data());
    out.copyin.push_back(cands[0].pass);
  }
  for (size_t j = 0; j + 1 < n; ++j) {
    auto *tail = cands[j].pass->cmd_tail;
    out.nexts.push_back({&tail->next, tail->next.ptr});
    tail->next.set(&cands[j + 1].pass->cmd_head);
  }
  for (size_t j = 1; j < n; ++j) { out.types.push_back({cands[j].pass, cands[j].pass->type}); cands[j].pass->type = EncoderType::Null; }
  cands[0].pass->lightrn_concurrent = 1; out.concurrent.push_back(cands[0].pass);
  for (const auto &p : patches) { out.bufs.push_back({p.buffer, p.offset, *p.buffer, *p.offset}); *p.buffer = p.b; *p.offset = p.o; }
  for (auto &c : copies) out.leases.push_back(c);
  for (auto &l : leases) out.leases.push_back(std::move(l));
  for (size_t j = 0; j < n; ++j) for (auto &sn : cands[j].snaps) out.snaps.push_back(std::move(sn));
  auto &s = Get();
  Note(s.runs); Note(s.renamed, n - 1); Note(s.kept); Note(s.passes_redirected, passes); Note(s.tables_redirected, tables_total);
  Note(s.roots_redirected, roots_total); Note(s.bytes_tables, w.bytes);
  if (cands[0].copyin) {
    Note(s.copyin_runs);
    if (!out.copyin.empty() && out.copyin.back() == cands[0].pass) { Note(s.copyin_cmds, out.blits.back().size()); Note(s.copyin_bytes, copy_bytes); }
  }
}

// Plan and apply the runs of one encoding. `key` = Key() read once for the encoding.
inline void Plan(MTLD3D12Device *dev, UINT Count, ID3D12CommandList *const *lists, unsigned key, bool early, Applied &out) {
  Note(Get().encodings);
  struct Collect { Collect(std::vector<DescSnap> *v) { t_descs = v; } ~Collect() { t_descs = nullptr; } } collect(early ? &out.descs : nullptr);
  bool any = false;
  for (UINT l = 0; l < Count && !any; ++l)
    for (auto p = static_cast<MTLD3D12GraphicsCommandList *>(lists[l])->entry; p && !any; p = p->next)
      if (p->type == EncoderType::Compute && static_cast<ComputeEncoderData *>(p)->lightrn &&
          !static_cast<ComputeEncoderData *>(p)->lightrn->d.empty()) any = true;
  if (!any) return;
  std::vector<Cand> cands;
  std::vector<Entry> entries;
  std::vector<const Texture *> written;
  std::vector<obj_handle_t> barrier_objs;
  const auto finish = [&](unsigned why) {
    if (!cands.empty() && why != RCount) NoteReason(why);
    // members after the last candidate are not part of the run
    while (!entries.empty() && entries.back().cand < 0) entries.pop_back();
    Finish(dev, cands, entries, out);
    written.clear(); barrier_objs.clear();
  };
  for (UINT l = 0; l < Count; ++l) {
    for (auto p = static_cast<MTLD3D12GraphicsCommandList *>(lists[l])->entry; p; p = p->next) {
      if (p->type == EncoderType::Null) continue;
      if (p->type == EncoderType::Compute) {
        auto &c = *static_cast<ComputeEncoderData *>(p);
        Cand cd;
        const unsigned why = c.lightrn && !c.lightrn->d.empty() ? Evaluate(dev, c, key, cd) : unsigned(RNoRecord);
        if (why != RCount) {
          if (why != RNoRecord) NoteReason(why);
          finish(ROtherPass);
          continue;
        }
        Note(Get().candidates);
        bool join = !cands.empty() && cands.back().S == cd.S;
        if (join && (cands.back().copyin || cd.copyin) &&
            !(cands.back().copyin && cd.copyin && (E().nosig || cands.back().sig == cd.sig))) { NoteReason(RSigMismatch); join = false; }
        if (join) {
          for (auto *t : cd.inputs) if (std::find(written.begin(), written.end(), t) != written.end()) { NoteReason(RInputWritten); join = false; break; }
        }
        if (join) {
          for (auto h : cd.input_handles) if (std::find(barrier_objs.begin(), barrier_objs.end(), h) != barrier_objs.end()) { NoteReason(RInputBarrier); join = false; break; }
        }
        if (!join) finish(RCount);
        entries.push_back({p, int(cands.size())});
        cands.push_back(std::move(cd));
        continue;
      }
      if (cands.empty()) continue;
      if (p->type == EncoderType::Render) {
        auto &r = *static_cast<RenderEncoderData *>(p);
        const unsigned why = CheckRender(r, cands.back().S, cands.back().covered, cands.back().copyin);
        if (why != RCount) { finish(why); continue; }
        for (unsigned k = 0; k < r.colors.size(); ++k) {
          if (auto t = ViewTexture(r.colors[k].attachment)) written.push_back(t);
          if (auto t = ViewTexture(r.colors[k].resolve_attachment)) written.push_back(t);
        }
        if (auto t = ViewTexture(r.depth.attachment)) written.push_back(t);
        if (auto t = ViewTexture(r.stencil.attachment)) written.push_back(t);
        entries.push_back({p, -1});
        continue;
      }
      if (p->type == EncoderType::Blit && static_cast<BlitEncoderData *>(p)->barrier_only) {
        auto &b = *static_cast<BlitEncoderData *>(p);
        bool ok = b.barrier_records_valid;
        for (const auto &rec : b.barrier_records) {
          if (rec.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !rec.object) { ok = false; break; }
          barrier_objs.push_back(rec.object);
          if (rec.object2) barrier_objs.push_back(rec.object2);
        }
        if (!ok) { finish(RAlias); continue; }
        entries.push_back({p, -1});
        continue;
      }
      finish(ROtherPass);
    }
  }
  finish(RCount);
}

} // namespace light_rename
