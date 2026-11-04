// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_device.hpp"
#include <atomic>

namespace dxmt::reuse {
struct RecordedDraw {
  Signature state;
  uint64_t pso = 0;
  std::shared_ptr<const ShaderInfo> shader;
  Com<MTLD3D12RootSignature, false> root;
  root_argument::State arguments;
  std::array<Com<ID3D12DescriptorHeap>, 2> heaps;
  std::array<root_argument::HeapRange, 2> heap_ranges;
  std::vector<Binding> inputs;
};
struct RecordedPass {
  static inline std::atomic<size_t> live_draws{0};
  std::vector<RecordedDraw> draws;
  std::vector<ResourcePtr> extra_writes;
  uint64_t unknown = 0, count = 0;
  ~RecordedPass() { live_draws.fetch_sub(draws.size(),std::memory_order_relaxed); }
  bool Append(RecordedDraw draw) {
    const auto old=live_draws.fetch_add(1,std::memory_order_relaxed);
    if (old>=MaxDraws) { live_draws.fetch_sub(1,std::memory_order_relaxed);unknown|=DrawLimit;return false; }
    try { draws.push_back(std::move(draw)); }
    catch (...) { live_draws.fetch_sub(1,std::memory_order_relaxed);throw; }
    return true;
  }
};

inline Binding AddressBinding(uint64_t address, uint64_t length, bool write = false, bool cbv = false) {
  Binding b; b.resource = FindAddress(address, b.offset); b.length = length; b.write = write; b.cbv = cbv;
  if (b.resource && !length) b.length = std::min<uint64_t>(b.resource->length - b.offset, cbv ? 65536 : UINT64_MAX);
  b.descriptor.Word(address); b.descriptor.Word(b.length); b.descriptor.Word(write); b.descriptor.Word(cbv);
  return b;
}

inline void Bindings(const RecordedDraw &d, Signature &sig, PSO *pso, std::vector<ResourcePtr> &writes,
                     std::vector<const Resource *> *bound = nullptr) {
  auto &w = Get().window;
  size_t observed = 0;
  auto observe = [&](Binding &b, RegisterKey key, uint32_t param = SlotUnknown, uint32_t slot = SlotUnknown) {
    if (++observed > MaxBindings) { sig.unknown |= BindingLimit; return; }
    if (!b.type && !b.resource) { sig.part[Descriptors].Join(b.descriptor); return; } // null table descriptor
    if (bound && b.resource) bound->push_back(b.resource.get());
    Read(sig, b, w.budget, pso, key, d.shader.get(), param, slot);
    if (b.write && b.resource) writes.push_back(b.resource);
    if (b.write && b.counter) writes.push_back(b.counter);
  };
  for (auto b : d.inputs) { b.type = 1; observe(b, {}); }
  if (!d.root) return;
  const auto &layout = d.root->ArgumentLayout;
  const auto &parameters = d.root->reuse_parameters;
  if (parameters.size() != layout.count) { sig.unknown |= MissingBinding; return; }
  for (uint32_t index = 0; index < layout.count; ++index) {
    const auto &p = parameters[index]; const auto &l = layout.parameters[index];
    RegisterKey key{p.visibility, p.space, p.binding, 0};
    if (l.kind == root_argument::Constants) {
      if (!d.arguments.ConstantsReady(layout, index)) sig.unknown |= MissingBinding;
      const void *data = d.arguments.words.data() + l.qword;
      sig.Bytes(Constants, data, size_t(l.count) * 4);
      for (unsigned i = 0; i + 4 <= l.count; i += 4) {
        key.index = i / 4;
        auto flags=d.shader?d.shader->Flags(key):0;
        w.ObserveRegister(pso, key, static_cast<const uint8_t *>(data) + i * 4, flags&2, flags&4, flags&1, index, SlotRootConstants);
      }
    } else if (l.kind == root_argument::Table) {
      for (const auto &range : p.ranges) {
        const unsigned ht = range.type == 3 ? 1 : 0;
        const auto &heap = d.heaps[ht];
        if (!heap) { sig.unknown |= MissingBinding; continue; }
        // Unbounded descriptor arrays are sampled to the same explicit bound; incomplete cannot become a hit.
        if (range.count > MaxBindings - std::min(observed, MaxBindings)) sig.unknown |= BindingLimit;
        const unsigned count = std::min<size_t>(range.count, MaxBindings - std::min(observed, MaxBindings));
        for (unsigned j = 0; j < count; ++j) {
          root_argument::TableLocation loc;
          if (range.offset > UINT32_MAX - j || !root_argument::ReadTable(layout, d.arguments, index,
              d.heap_ranges[ht], range.offset + j, 1, loc)) { sig.unknown |= MissingBinding; break; }
          key.binding = range.binding + j; key.space = range.space;
          if (ht == 1) {
            D3D12DescriptorCapture capture;
            auto h = static_cast<MTLD3D12SamplerDescriptorHeap *>(heap.ptr());
            if (FAILED(h->CaptureDescriptor(loc.index, capture))) sig.unknown |= MissingBinding;
            else {
              const auto &v=capture.sampler->info();
              sig.Word(Descriptors,v.min_filter);sig.Word(Descriptors,v.mag_filter);sig.Word(Descriptors,v.mip_filter);
              sig.Word(Descriptors,v.r_address_mode);sig.Word(Descriptors,v.s_address_mode);sig.Word(Descriptors,v.t_address_mode);
              sig.Word(Descriptors,v.border_color);sig.Word(Descriptors,v.compare_function);
              sig.Bytes(Descriptors,&v.lod_min_clamp,sizeof(v.lod_min_clamp));sig.Bytes(Descriptors,&v.lod_max_clamp,sizeof(v.lod_max_clamp));
              sig.Word(Descriptors,v.max_anisotroy);sig.Word(Descriptors,v.normalized_coords);sig.Word(Descriptors,v.lod_average);
              sig.Bytes(Descriptors,&capture.sampler->lod_bias,sizeof(capture.sampler->lod_bias));
            }
            ++observed;
          } else {
            Binding b;
            auto h = static_cast<MTLD3D12DescriptorHeap *>(heap.ptr());
            if (!h->ReuseDescriptor(loc.index, b)) sig.unknown |= MissingBinding;
            else observe(b, key, index, range.offset + j);
          }
        }
      }
    } else {
      uint64_t address = 0;
      if (!d.arguments.ReadValue(layout, index, l.kind, address)) { sig.unknown |= MissingBinding; continue; }
      uint64_t bytes=l.kind==root_argument::CBV && d.shader && !d.shader->unknown ? d.shader->Bound(key) : 0;
      auto b = AddressBinding(address, bytes, l.kind == root_argument::UAV, l.kind == root_argument::CBV);
      b.type = 1; observe(b, key, index, SlotRootDescriptor);
    }
  }
}

inline ResourcePtr ViewResource(const TextureViewRef &view) {
  return view && view->allocation ? FindIdentity(view->allocation->descriptor) : ResourcePtr{};
}
inline void TargetView(Signature &s, const TextureViewRef &view, unsigned level, unsigned slice,
                       unsigned plane, unsigned load, unsigned store, bool reads_old, const RenderEncoderData &pass,
                       unsigned dsv_flags = 0) {
  if (!view) { s.Word(Targets, 0); return; }
  auto r = ViewResource(view);
  if (!r || !r->live) { s.unknown |= MissingResource; return; }
  s.Word(Targets, r->id); s.Word(Targets, view->key); s.Word(Targets, level); s.Word(Targets, slice);
  s.Word(Targets, plane); s.Word(Targets, load); s.Word(Targets, store);
  const auto &c=r->clear;
  const bool folded=c.known && c.view==view.texture().handle && c.width==pass.render_target_width &&
    c.height==pass.render_target_height && c.array==pass.render_target_array_length &&
    (!dsv_flags ? !c.flags && !level && !slice && c.plane==plane : (c.flags&dsv_flags)==dsv_flags && !plane);
  if (reads_old) {
    s.Word(Targets, folded);
    if (folded) s.part[Targets].Join(c.value);
    else s.Word(History, r->version);
  }
}

inline void BlitWrites(const BlitEncoderData &pass) {
  for (auto c = reinterpret_cast<const wmtcmd_base *>(&pass.cmd_head); c; c = reinterpret_cast<const wmtcmd_base *>(c->next.ptr)) {
    uint64_t dst = 0;
    switch (c->type) {
    case WMTBlitCommandNop: case WMTBlitCommandWaitForFence: case WMTBlitCommandUpdateFence: break;
#define REUSE_BLIT(type, structure, member) case type: dst = reinterpret_cast<const structure *>(c)->member; break
    REUSE_BLIT(WMTBlitCommandCopyFromBufferToBuffer, wmtcmd_blit_copy_from_buffer_to_buffer, dst);
    REUSE_BLIT(WMTBlitCommandCopyFromBufferToTexture, wmtcmd_blit_copy_from_buffer_to_texture, dst);
    REUSE_BLIT(WMTBlitCommandCopyFromBufferToTextureWithBlitOption, wmtcmd_blit_copy_from_buffer_to_texture_withblitoption, dst);
    REUSE_BLIT(WMTBlitCommandCopyFromTextureToBuffer, wmtcmd_blit_copy_from_texture_to_buffer, dst);
    REUSE_BLIT(WMTBlitCommandCopyFromTextureToBufferWithBlitOption, wmtcmd_blit_copy_from_texture_to_buffer_withblitoption, dst);
    REUSE_BLIT(WMTBlitCommandCopyFromTextureToTexture, wmtcmd_blit_copy_from_texture_to_texture, dst);
    REUSE_BLIT(WMTBlitCommandCopyTexture, wmtcmd_blit_copy_texture, dst);
    REUSE_BLIT(WMTBlitCommandGenerateMipmaps, wmtcmd_blit_generate_mipmaps, texture);
    REUSE_BLIT(WMTBlitCommandFillBuffer, wmtcmd_blit_fillbuffer, buffer);
    REUSE_BLIT(WMTBlitCommandResolveCounters, wmtcmd_blit_resolvecounters, dst_buffer);
    REUSE_BLIT(WMTBlitCommandResetCommandsInBuffer, wmtcmd_blit_resetcommands, indirect_command_buffer);
#undef REUSE_BLIT
    default: UnknownWrite(); break;
    }
    if (dst) WriteObject(dst);
  }
  if (pass.resolve_destination) WriteObject(pass.resolve_destination.handle);
  // Aliasing barriers invalidate both identities, including null/global barriers.
  for (const auto &b : pass.barrier_records) if (b.type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
    if (b.object) WriteObject(b.object);
    if (b.object2) WriteObject(b.object2);
    if (!b.object && !b.object2) UnknownWrite();
  }
}

inline void Capture(const void *encoder, Pass pass) {
  auto &s=Get();
  if (s.pending_count>=MaxPasses) { s.frame_incomplete=true; ++s.window.skipped_passes; return; }
  s.pending[encoder].push_back(std::move(pass)); ++s.pending_count;
}
inline void Publish(const std::shared_ptr<EncoderPlan> &plan) noexcept {
  if (!Active()) return;
  auto &s=Get(); std::lock_guard lock(s.mutex);
  try {
    if (!plan || plan->incomplete) s.frame_incomplete=true;
    if (!plan) return;
    for (const auto &group:plan->groups) {
      Pass combined; bool first=true; uint64_t missing=0;
      for (const auto key:group) {
        if (!key) { combined.target=Compute; combined.signature.unknown|=UnsupportedCommand; continue; }
        auto found=s.pending.find(key);
        if (found==s.pending.end() || found->second.empty()) { missing|=MissingBinding; continue; }
        auto pass=std::move(found->second.front()); found->second.pop_front(); --s.pending_count;
        if (first) { combined=std::move(pass); first=false; continue; }
        // A merged encoder's later load observes its own earlier writes. It is
        // not an external input. SRV/UAV resource histories remain untouched.
        pass.signature.part[History]={};
        for (auto &draw:pass.draws) draw.signature.part[History]={};
        combined.signature.Join(pass.signature);
        combined.actual_draws+=pass.actual_draws;
        for (auto &draw:pass.draws) combined.draws.push_back(std::move(draw));
        for (const auto &out:pass.outputs) {
          auto old=std::find_if(combined.outputs.begin(),combined.outputs.end(),[&](auto &o){return o.id==out.id;});
          if (old==combined.outputs.end()) combined.outputs.push_back(out); else old->after=out.after;
        }
        if (pass.target!=combined.target) combined.target=Other;
      }
      if (s.frame_incomplete) combined.signature.unknown|=PassLimit;
      combined.signature.unknown|=missing;
      s.window.Add(std::move(combined));
    }
  } catch (...) { ++s.exceptions; s.frame_incomplete=true; }
}
struct PlanBuilder {
  std::shared_ptr<EncoderPlan> plan;
  size_t entries=0;
  PlanBuilder() noexcept { if (Active()) try { plan=std::make_shared<EncoderPlan>(); } catch (...) {} }
  void Add(const void *encoder,bool merge=false) noexcept {
    if (!plan) return;
    try {
      if (++entries>MaxPasses) { plan->incomplete=true; return; }
      if (!merge || plan->groups.empty()) plan->groups.emplace_back();
      if (plan->groups.back().size()>=MaxPasses) { plan->incomplete=true; return; }
      plan->groups.back().push_back(encoder);
    } catch (...) { plan->incomplete=true; }
  }
};

inline void Execute(const void *queue, unsigned count, ID3D12CommandList *const *lists) {
  if (!Active()) return;
  auto &s = Get(); std::lock_guard lock(s.mutex); LARGE_INTEGER start{}, end{}; QueryPerformanceCounter(&start);
  try {
    s.queues.insert(queue);
    for (unsigned n = 0; n < count; ++n) {
      auto list = static_cast<MTLD3D12GraphicsCommandList *>(lists[n]);
      for (auto e = list->entry; e; e = e->next) {
        if (e->type == EncoderType::Null) continue;
        if (e->type == EncoderType::Blit) { BlitWrites(*static_cast<BlitEncoderData *>(e)); continue; }
        if (e->type == EncoderType::Clear) {
          const auto c=static_cast<ClearEncoderData *>(e);auto r=ViewResource(c->attachment);
          if (r) {
            auto before=r->version;Write(r);
            r->clear={true,before,c->attachment.texture().handle,c->width,c->height,c->array_length,c->depth_plane,c->clear_dsv,{}};
            if (c->clear_dsv) {
              r->clear.value.Bytes(&c->depth_stencil.first,sizeof(c->depth_stencil.first));r->clear.value.Word(c->depth_stencil.second);
            } else r->clear.value.Bytes(&c->color,sizeof(c->color));
            Pass clear;
            clear.signature.Word(Targets,r->id);clear.signature.Word(Targets,c->attachment->key);
            clear.signature.Word(Targets,c->width);clear.signature.Word(Targets,c->height);
            clear.signature.Word(Targets,c->array_length);clear.signature.Word(Targets,c->depth_plane);
            clear.signature.Word(Targets,c->clear_dsv);clear.signature.part[Targets].Join(r->clear.value);
            clear.outputs.push_back({r->id,before,r->version});
            if (c->clear_dsv && c->width==4096 && c->height==2048 &&
                (r->format==DXGI_FORMAT_D32_FLOAT || r->format==DXGI_FORMAT_R32_TYPELESS)) clear.target=Shadow;
            Capture(e,std::move(clear));
          } else Write(r);
          continue;
        }
        if (e->type == EncoderType::Resolve) {
          Write(ViewResource(static_cast<ResolveEncoderData *>(e)->dst));
          Pass resolve;resolve.signature.unknown|=UnsupportedCommand;Capture(e,std::move(resolve));continue;
        }
        Pass pass;
        if (s.queues.size() > 1) pass.signature.unknown |= MultipleQueues;
        std::vector<ResourcePtr> targets;
        if (e->type == EncoderType::Render) {
          auto r = static_cast<RenderEncoderData *>(e);
          pass.target = r->render_target_count > 1 ? GBuffer : Other;
          if (!r->render_target_count) if (auto depth = ViewResource(r->depth.attachment))
            if ((depth->format == DXGI_FORMAT_D32_FLOAT || depth->format == DXGI_FORMAT_R32_TYPELESS) && r->render_target_width == 4096 && r->render_target_height == 2048) pass.target = Shadow;
          pass.signature.Word(Targets, r->render_target_width); pass.signature.Word(Targets, r->render_target_height);
          pass.signature.Word(Targets, r->default_raster_sample_count); pass.signature.Word(Targets, r->render_target_array_length);
          for (unsigned i = 0; i < r->render_target_count; ++i) {
            const auto &a = r->colors[i];
            TargetView(pass.signature, a.attachment, a.level, a.slice, a.depth_plane, a.load_action, a.store_action, a.load_action == WMTLoadActionLoad, *r);
            pass.signature.Bytes(Targets, &a.clear_color, sizeof(a.clear_color));
            TargetView(pass.signature, a.resolve_attachment, a.resolve_level, a.resolve_slice, a.resolve_depth_plane, 0, 0, false, *r);
            if (a.attachment) targets.push_back(ViewResource(a.attachment));
            if (a.resolve_attachment) targets.push_back(ViewResource(a.resolve_attachment));
          }
          TargetView(pass.signature, r->depth.attachment, r->depth.level, r->depth.slice, r->depth.depth_plane,
                     r->depth.load_action, r->depth.store_action, r->depth.load_action == WMTLoadActionLoad, *r, 1);
          TargetView(pass.signature, r->stencil.attachment, r->stencil.level, r->stencil.slice, r->stencil.depth_plane,
                     r->stencil.load_action, r->stencil.store_action, r->stencil.load_action == WMTLoadActionLoad, *r, 2);
          pass.signature.Bytes(Targets, &r->depth.clear_depth, sizeof(r->depth.clear_depth));
          pass.signature.Word(Targets, r->stencil.clear_stencil); pass.signature.Word(Targets, r->dsv_readonly_flags);
          if (r->depth.attachment && !(r->dsv_readonly_flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH)) targets.push_back(ViewResource(r->depth.attachment));
          if (r->stencil.attachment && !(r->dsv_readonly_flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL)) targets.push_back(ViewResource(r->stencil.attachment));
          if (!r->visibility_targets.empty() || r->visibility_direct) {
            pass.signature.unknown |= UnsupportedCommand;
            UnknownWrite(); // query buffers are internal; never leave a later read's version stale
          }
        } else pass.target = Compute;
        for (const auto &r:targets) if (r) pass.outputs.push_back({r->id,r->clear.known?r->clear.before:r->version,0});
        if (!e->reuse_record) {
          pass.signature.unknown |= UnsupportedCommand;
          if (pass.target==Compute) UnknownWrite();
          for (auto &r:targets) Write(r); // internal rectangular RT/DS clears still write
        }
        else {
          pass.actual_draws = e->reuse_record->count;
          pass.signature.unknown |= e->reuse_record->unknown;
          // XL-MERGE observation: a draw binding (SRV/UAV/CBV/VB/IB) the same resource identity as a
          // writable attachment of its own pass. Object level: other mips/slices of it also count.
          bool feedback_pass = false;
          std::vector<const Resource *> bound;
          for (const auto &d : e->reuse_record->draws) {
            if (++s.window.draw_count > MaxDraws) { pass.signature.unknown |= DrawLimit; UnknownWrite(); break; }
            Draw draw{d.state, d.pso};
            auto pso = s.window.ObservePSO(d.pso, d.shader && d.shader->watched);
            if (pso && pso->watched && pso->shaders.empty() && d.shader) pso->shaders = d.shader->hashes;
            if (pso && (!d.shader || d.shader->unknown)) ++pso->metadata_unknown;
            std::vector<ResourcePtr> writes;
            bound.clear();
            Bindings(d, draw.signature, pso, writes, e->type == EncoderType::Render ? &bound : nullptr);
            bool feedback = false;
            for (const auto *r : bound) for (const auto &t : targets) feedback |= t && t.get() == r;
            if (feedback) { ++s.window.counts[pass.target].feedback_draws; feedback_pass = true; }
            draw.signature.Join(pass.signature);
            pass.draws.push_back(std::move(draw));
            // Reads precede each occurrence's writes. A second dispatch in the same encoder sees the new version.
            for (auto &r : writes) Write(r);
            for (auto &r : targets) Write(r);
          }
          if (feedback_pass) ++s.window.counts[pass.target].feedback_passes;
          for (auto &r:e->reuse_record->extra_writes) Write(r);
          if (e->reuse_record->unknown & (DrawLimit|MissingResource)) UnknownWrite();
        }
        for (auto &o : pass.outputs) for (auto &r : targets) if (r && r->id==o.id) o.after=r->version;
        Capture(e,std::move(pass));
      }
    }
  } catch (...) { ++s.exceptions; s.frame_incomplete=true; }
  QueryPerformanceCounter(&end); s.window.observed_ticks += end.QuadPart - start.QuadPart;
}
} // namespace dxmt::reuse
