#pragma once
// Read sets for moving the existing tess pre-pass. Resolve volatile descriptor entries at submission,
// just as the application heap will be consumed then. Never inspect resource contents or walk a whole heap.
#include "d3d12_device.hpp"
#include "d3d12_tess_prepass.hpp"

namespace dxmt::tess_prepass {
// Resource and sampler capture interfaces have different vtable layouts. Select the actual heap family before
// dispatching; treating a sampler as a resource heap calls AddSampler instead of CaptureDescriptor.
inline HRESULT CaptureInputDescriptor(ID3D12DescriptorHeap *heap, uint32_t type, UINT index,
                                     D3D12DescriptorCapture &out) {
  if (!heap || type > D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) return E_INVALIDARG;
  const bool sampler = type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  const auto expected = sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  if (heap->GetDesc().Type != expected) return E_INVALIDARG;
  if (sampler)
    return static_cast<MTLD3D12SamplerDescriptorHeap *>(heap)->CaptureDescriptor(index,out);
  return static_cast<MTLD3D12DescriptorHeap *>(heap)->CaptureDescriptor(index,out);
}

struct DrawInputs {
  struct Table { Com<ID3D12DescriptorHeap> heap; uint32_t index, type; };
  std::vector<Table> tables;
  std::vector<WMT::Reference<WMT::Buffer>> buffers;
  bool known = true;

  bool Buffer(MTLD3D12Device *device, uint64_t address, uint64_t bytes) {
    uint64_t offset = 0;
    auto allocation = device->LookupBufferByVA(address, std::max(uint64_t(1),bytes), &offset);
    if (!allocation || !allocation->buffer()) return known = false;
    buffers.emplace_back(allocation->buffer()); return true;
  }
  bool Root(const PsoPrepass &pso, MTLD3D12Device *device, MTLD3D12RootSignature *root,
            const root_argument::State &args, const std::array<Com<ID3D12DescriptorHeap>,2> &heaps,
            const std::array<root_argument::HeapRange,2> &ranges) {
    if (!pso.inputs_known || !root || root->reuse_parameters.size() != root->ArgumentLayout.count) return known = false;
    const auto &layout = root->ArgumentLayout;
    for (const auto &need : pso.inputs) {
      if (need.last < need.first || need.last - need.first > 1023) return known = false;
      for (uint64_t reg = need.first; reg <= need.last; ++reg) {
        bool found = false;
        for (unsigned i = 0; i < layout.count && !found; ++i) {
          const auto &p = root->reuse_parameters[i]; const auto &l = layout.parameters[i];
          if (p.visibility && p.visibility != need.visibility) continue;
          if (l.kind == root_argument::Table) {
            for (const auto &range : p.ranges) {
              if (range.type != need.type || range.space != need.space || reg < range.binding ||
                  reg - range.binding >= range.count) continue;
              const unsigned ht = need.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? 1 : 0;
              const uint64_t offset = uint64_t(range.offset) + reg - range.binding;
              root_argument::TableLocation location;
              if (offset > UINT32_MAX || !heaps[ht] || !root_argument::ReadTable(layout,args,i,ranges[ht],
                  unsigned(offset),1,location)) return known = false;
              tables.push_back({heaps[ht],location.index,need.type}); found = true; break;
            }
          } else if (p.space == need.space && p.binding == reg) {
            if (l.kind == root_argument::Constants && need.type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
              if (!args.ConstantsReady(layout,i) || uint64_t(l.count)*4 < need.bytes) return known = false;
              found = true; // Immutable argument upload, not an application resource.
            } else if ((l.kind == root_argument::CBV && need.type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) ||
                       (l.kind == root_argument::SRV && need.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV)) {
              uint64_t address = 0;
              if (!args.ReadValue(layout,i,l.kind,address) || !Buffer(device,address,1)) return known = false;
              found = true;
            }
          }
        }
        // A PSO's root-coverage validation admits a sampler absent from tables only as a static sampler.
        // Static samplers have no application-writable memory, so contribute no resource dependency.
        if (!found && need.type != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) return known = false;
      }
    }
    return true;
  }
  bool Resolve(std::vector<obj_handle_t> &reads) const {
    if (!known) return false;
    for (const auto &buffer : buffers) reads.push_back(buffer.handle);
    for (const auto &t : tables) {
      if (t.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) continue;
      D3D12DescriptorCapture c;
      if (FAILED(CaptureInputDescriptor(t.heap.ptr(),t.type,t.index,c))) return false;
      if (c.type == ShaderVisibleDescriptorType::Null) continue;
      const bool srv = c.type == ShaderVisibleDescriptorType::SRVTexture || c.type == ShaderVisibleDescriptorType::SRVBuffer ||
                       c.type == ShaderVisibleDescriptorType::SRVTexelBuffer;
      if (!(t.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? srv : c.type == ShaderVisibleDescriptorType::ConstantBuffer) ||
          !c.resource || !c.resource->Live()) return false;
      const auto &r = *c.resource->native;
      if (r.buffer_allocation) reads.push_back(r.buffer_allocation->buffer().handle);
      else if (r.texture_allocation) reads.push_back(r.texture_allocation->texture().handle);
      else return false;
    }
    std::sort(reads.begin(),reads.end()); reads.erase(std::unique(reads.begin(),reads.end()),reads.end());
    return true;
  }
};

// Exact, bounded mode-3 observation. CBV contents are copied on the GPU at the draw boundary;
// textures/SRV/VB/IB compare immutable view identity plus intervening write versions at submission.
// A different resource with coincidentally identical texels is deliberately not called equal.
struct TwinDraw {
  struct Range { WMT::Reference<WMT::Buffer> buffer; uint64_t offset, bytes; };
  std::shared_ptr<PsoPrepass> pso;
  Com<ID3D12RootSignature> root;
  root_argument::State args;
  std::array<Com<ID3D12DescriptorHeap>,2> heaps;
  std::array<root_argument::HeapRange,2> ranges;
  std::vector<Range> vertices;
  std::vector<uint8_t> draw_key;
  uint64_t frame = 0, id = 0, patches = 0;
  bool known = true;

  static void Bytes(std::vector<uint8_t> &key, const void *p, size_t n) {
    const auto b = static_cast<const uint8_t *>(p); key.insert(key.end(),b,b+n);
  }
  static void Word(std::vector<uint8_t> &key, uint64_t value) { Bytes(key,&value,sizeof(value)); }
  bool Vertex(MTLD3D12Device *device, uint64_t address, uint64_t bytes) {
    uint64_t offset = 0;
    auto allocation = device->LookupBufferByVA(address,std::max(uint64_t(1),bytes),&offset);
    if (!allocation || !allocation->buffer()) return known = false;
    vertices.push_back({allocation->buffer(),offset,bytes}); return true;
  }
  bool Resolve(MTLD3D12Device *device, std::vector<uint8_t> &key, std::vector<Range> &cbvs,
               std::vector<obj_handle_t> &reads) const {
    if (!known || !pso || !pso->twin_known || pso->twin_shaders.empty() || !root) return false;
    auto rs = static_cast<MTLD3D12RootSignature *>(root.ptr());
    const auto &layout = rs->ArgumentLayout;
    if (rs->reuse_parameters.size() != layout.count) return false;
    key = pso->twin_shaders;
    Word(key,reinterpret_cast<uintptr_t>(root.ptr())); // Includes immutable static samplers and layout.
    Bytes(key,draw_key.data(),draw_key.size());
    for (const auto &v : vertices) {
      Word(key,v.buffer.handle); Word(key,v.offset); Word(key,v.bytes); reads.push_back(v.buffer.handle);
    }
    for (const auto &need : pso->twin_inputs) {
      if (need.last < need.first || need.last-need.first > 1023) return false;
      for (uint64_t reg=need.first; reg<=need.last; ++reg) {
        Word(key,need.visibility); Word(key,need.type); Word(key,need.space); Word(key,reg);
        bool found = false;
        for (unsigned i=0; i<layout.count && !found; ++i) {
          const auto &p=rs->reuse_parameters[i]; const auto &l=layout.parameters[i];
          if (p.visibility && p.visibility!=need.visibility) continue;
          if (l.kind==root_argument::Table) {
            for (const auto &range:p.ranges) {
              if (range.type!=need.type || range.space!=need.space || reg<range.binding || reg-range.binding>=range.count) continue;
              const unsigned ht=need.type==D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER?1:0;
              const uint64_t off=uint64_t(range.offset)+reg-range.binding;
              root_argument::TableLocation loc;
              if (off>UINT32_MAX || !heaps[ht] || !root_argument::ReadTable(layout,args,i,ranges[ht],unsigned(off),1,loc)) return false;
              D3D12DescriptorCapture c;
              if (FAILED(CaptureInputDescriptor(heaps[ht].ptr(),need.type,loc.index,c))) return false;
              Word(key,unsigned(c.type));
              if (need.type==D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
                if (!need.bytes || need.bytes>65536 || c.type!=ShaderVisibleDescriptorType::ConstantBuffer ||
                    !c.resource || !c.resource->Live() || !c.resource->native->buffer_allocation || c.byte_length<need.bytes) return false;
                auto b=c.resource->native->buffer_allocation->buffer();
                cbvs.push_back({b,c.buffer_offset,need.bytes}); Word(key,need.bytes);
              } else {
                Bytes(key,c.words.data(),sizeof(c.words));
                Word(key,c.buffer_offset); Word(key,c.byte_length);
                if (c.resource) {
                  if (!c.resource->Live()) return false;
                  const auto &n=*c.resource->native;
                  const obj_handle_t object=n.buffer_allocation?n.buffer_allocation->buffer().handle:
                      n.texture_allocation?n.texture_allocation->texture().handle:0;
                  if (!object) return false;
                  Word(key,object); reads.push_back(object);
                } else if (need.type!=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER && c.type!=ShaderVisibleDescriptorType::Null) return false;
              }
              found=true; break;
            }
          } else if (p.space==need.space && p.binding==reg) {
            if (l.kind==root_argument::Constants && need.type==D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
              if (!args.ConstantsReady(layout,i) || uint64_t(l.count)*4<need.bytes || !need.bytes) return false;
              Word(key,need.bytes); Bytes(key,args.words.data()+l.qword,need.bytes); found=true;
            } else if ((l.kind==root_argument::CBV && need.type==D3D12_DESCRIPTOR_RANGE_TYPE_CBV) ||
                       (l.kind==root_argument::SRV && need.type==D3D12_DESCRIPTOR_RANGE_TYPE_SRV)) {
              uint64_t address=0,offset=0;
              if (!args.ReadValue(layout,i,l.kind,address)) return false;
              const uint64_t bytes=l.kind==root_argument::CBV?need.bytes:1;
              if (!bytes || bytes>65536) return false;
              auto a=device->LookupBufferByVA(address,bytes,&offset);
              if (!a || !a->buffer()) return false;
              if (l.kind==root_argument::CBV) { cbvs.push_back({a->buffer(),offset,bytes}); Word(key,bytes); }
              else { Word(key,a->buffer().handle); Word(key,offset); reads.push_back(a->buffer().handle); }
              found=true;
            }
          }
        }
        if (!found && need.type!=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) return false;
      }
    }
    uint64_t bytes=0;
    for (const auto &c:cbvs) bytes+=c.bytes; // Ranges were admitted by CaptureDescriptor / LookupBufferByVA.
    if (cbvs.size()>64 || bytes>1024*1024) return false;
    std::sort(reads.begin(),reads.end()); reads.erase(std::unique(reads.begin(),reads.end()),reads.end());
    return true;
  }
};
}
