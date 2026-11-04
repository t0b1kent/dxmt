/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d12_device.hpp"
#include "d3d12_layer_time.hpp"   // CPU-LATE 23.10: DXMT_LT scopes
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_fine_locks.hpp"
#include <utility>
#include "d3d12_pageable.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_sampler.hpp"
#include <mutex>
#include "log/log.hpp"
#include "d3d12_shader_source_stats.hpp"

namespace dxmt {

struct SRVTextureGPUStorage {
  uint64_t resource_id;
  uint64_t metadata;
  uint64_t padding[2];
};

using UAVTextureGPUStorage = SRVTextureGPUStorage;

struct UAVTexelBufferGPUStorage {
  uint64_t resource_id;
  uint64_t metadata;
  uint64_t padding[2];
};

using SRVTexelBufferGPUStorage = UAVTexelBufferGPUStorage;

struct UAVBufferGPUStorage {
  uint64_t pointer;
  uint64_t metadata;
  uint64_t counter_pointer;
  uint64_t padding;
};

using SRVBufferGPUStorage = UAVBufferGPUStorage;

struct ShaderVisibleDescriptorGPUStorage {
  union {
    SRVTextureGPUStorage SRVTexture;
    CBVCommonStorage ConstantBuffer;
    UAVTextureGPUStorage UAVTexture;
    UAVTexelBufferGPUStorage UAVTexelBuffer;
    UAVBufferGPUStorage UAVBuffer;
    SRVTexelBufferGPUStorage SRVTexelBuffer;
    SRVBufferGPUStorage SRVBuffer;
    std::array<uint64_t, 4> ZeroFilled;
  };

  ShaderVisibleDescriptorGPUStorage();
};

static_assert(sizeof(ShaderVisibleDescriptorGPUStorage) == 32);

inline uint64_t
TextureMetadata(uint32_t array_length, float min_lod) {
  return ((uint64_t)array_length << 32) | (uint64_t)std::bit_cast<uint32_t>(min_lod);
}

class MTLD3D12DescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12DescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<capture::Slot<D3D12ResourceCaptureSource, ShaderVisibleDescriptorCPUStorage>> descriptors_;
  DescriptorSlotLocks slot_locks_;
  Rc<Buffer> buffer_;
  ShaderVisibleDescriptorGPUStorage *mapped_argument_buffer_ = nullptr;
  uint64_t argument_buffer_gpu_address_ = 0;
  // CPU-LATE 23.10 (DESC_FAST): per slot 0 = never written, 1 = written without capture bookkeeping (the capture source is
  // resolved on demand by AcquireFast). Allocated only with DescFast(); then no slot ever calls Publish.
  std::unique_ptr<uint8_t[]> fast_state_;
  std::unique_ptr<std::atomic<uint32_t>[]> race_;   // DESC_RACE: bit 31 writer, low bits active readers
  void RaceWriteEnter(UINT i) {
    if (!race_) return;
    layer_time::g_race_ops.fetch_add(1, std::memory_order_relaxed);
    const uint32_t prev = race_[i].fetch_or(0x80000000u, std::memory_order_acq_rel);
    if (prev & 0x80000000u) layer_time::g_race_write_write.fetch_add(1, std::memory_order_relaxed);
    else if (prev) layer_time::g_race_write_read.fetch_add(1, std::memory_order_relaxed);
  }
  void RaceWriteExit(UINT i) { if (race_) race_[i].fetch_and(0x7fffffffu, std::memory_order_acq_rel); }
  void RaceReadEnter(UINT i) {
    if (!race_) return;
    layer_time::g_race_ops.fetch_add(1, std::memory_order_relaxed);
    if (race_[i].fetch_add(1, std::memory_order_acq_rel) & 0x80000000u)
      layer_time::g_race_write_read.fetch_add(1, std::memory_order_relaxed);
  }
  void RaceReadExit(UINT i) { if (race_) race_[i].fetch_sub(1, std::memory_order_acq_rel); }
  friend void FastCopyResourceDescriptors(MTLD3D12DescriptorHeap *, UINT, MTLD3D12DescriptorHeap *, UINT, UINT);
  friend void FastAddConstantBufferView(MTLD3D12DescriptorHeap *, UINT, UINT64, UINT32);

  // Slot lock of the previous path; DESC_FAST takes none (D3D12: a descriptor is not written while it is read or copied).
  struct SlotGuard {
    dxmt::mutex *m = nullptr;
    MTLD3D12DescriptorHeapImpl *race = nullptr;
    UINT slot = 0;
    SlotGuard(MTLD3D12DescriptorHeapImpl &heap, UINT index) {
      if (!DescFast()) { m = &heap.slot_locks_.For(index); m->lock(); }
      else if (heap.race_ && index < heap.descriptors_.size()) { race = &heap; slot = index; heap.RaceWriteEnter(index); }
    }
    ~SlotGuard() { if (m) m->unlock(); if (race) race->RaceWriteExit(slot); }
    SlotGuard(const SlotGuard &) = delete;
    SlotGuard &operator=(const SlotGuard &) = delete;
  };
  template <typename F> void PublishSlot(UINT index, F &&publish) {
    if (DescFast()) fast_state_[index] = 1;
    else publish();
  }

  capture::Status AcquireFast(UINT index, capture::Snapshot<D3D12ResourceCaptureSource, ShaderVisibleDescriptorCPUStorage> &out) {
    out = {};
    if (!fast_state_ || !fast_state_[index]) return capture::Status::Uninitialized;
    const ShaderVisibleDescriptorCPUStorage payload = descriptors_[index].payload;
    std::shared_ptr<D3D12ResourceCaptureSource> resource, counter;
    uint64_t offset = 0;
    switch (payload.type) {
    case ShaderVisibleDescriptorType::SRVTexture: resource = device_->LookupCaptureSource(payload.SRVTexture.texture); break;
    case ShaderVisibleDescriptorType::UAVTexture: resource = device_->LookupCaptureSource(payload.UAVTexture.texture); break;
    case ShaderVisibleDescriptorType::SRVTexelBuffer: resource = device_->LookupCaptureSource(payload.SRVTexelBuffer.buffer); break;
    case ShaderVisibleDescriptorType::UAVTexelBuffer: resource = device_->LookupCaptureSource(payload.UAVTexelBuffer.buffer); break;
    case ShaderVisibleDescriptorType::SRVBuffer: resource = device_->LookupCaptureSource(payload.SRVBuffer.buffer); break;
    case ShaderVisibleDescriptorType::UAVBuffer: {
      resource = device_->LookupCaptureSource(payload.UAVBuffer.buffer);
      const uint64_t counter_va = mapped_argument_buffer_ ? mapped_argument_buffer_[index].UAVBuffer.counter_pointer : 0;
      if (counter_va) {
        counter = device_->LookupCaptureByVA(counter_va, 4, offset);
        if (!counter) return capture::Status::Unsupported;
      }
      break;
    }
    case ShaderVisibleDescriptorType::ConstantBuffer:
      resource = device_->LookupCaptureByVA(payload.ConstantBuffer.address, payload.ConstantBuffer.size, offset);
      break;
    default: return capture::Status::Unsupported;
    }
    if (!resource) return capture::Status::Unsupported;
    out.payload = payload;
    out.resource = std::move(resource);
    out.counter = std::move(counter);
    return capture::Status::Ready;
  }

public:
  MTLD3D12DescriptorHeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12DescriptorHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
      break;
    }
    default:
      return E_INVALIDARG;
    }
    descriptors_.resize(pDesc->NumDescriptors);
    if (DescFast()) fast_state_.reset(new (std::nothrow) uint8_t[pDesc->NumDescriptors]());
    if (DescFast() && !fast_state_) return E_OUTOFMEMORY;
    if (DescFast() && g_desc_race) race_.reset(new (std::nothrow) std::atomic<uint32_t>[pDesc->NumDescriptors]());

    if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
      buffer_ = new Buffer(descriptors_.size() * sizeof(ShaderVisibleDescriptorGPUStorage), device_->GetMTLDevice());

      Flags<BufferAllocationFlag> flags;
#ifdef __i386__
      IMPLEMENT_ME
#endif
      buffer_->rename(buffer_->allocate(flags));
      mapped_argument_buffer_ =
          reinterpret_cast<ShaderVisibleDescriptorGPUStorage *>(buffer_->current()->mappedMemory(0));
      argument_buffer_gpu_address_ = buffer_->current()->gpuAddress();
      device_->RegisterResidencyAndVA(buffer_->current(), buffer_->length());
    } else {
      mapped_argument_buffer_ = reinterpret_cast<ShaderVisibleDescriptorGPUStorage *>(
          malloc(descriptors_.size() * sizeof(ShaderVisibleDescriptorGPUStorage))
      );
    }

    return S_OK;
  };

  ~MTLD3D12DescriptorHeapImpl() {
    if (buffer_) {
      device_->UnregisterResidencyAndVA(buffer_->current());
    } else {
      free(mapped_argument_buffer_);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override { DXMT_LT(105);
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap) ||
        riid == kD3D12ResourceHeapCaptureUUID) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) override { DXMT_LT(106);
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) override { DXMT_LT(107);
    *__ret = GetShaderVisibleDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) override { DXMT_LT(108);
    __ret->ptr = argument_buffer_gpu_address_;
    return __ret;
  }

  virtual HRESULT
  AddShaderResourceView(UINT Index, Texture *Texture, TextureViewKey View, FLOAT ResourceMinLODClamp, resource_shape::View Shape) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::SRVTexture;
    cpu_storage.shape = Shape;
    cpu_storage.SRVTexture.texture = Texture;
    cpu_storage.SRVTexture.view = View;
    shader_source::stats::CountTextureSRV(ResourceMinLODClamp);
    if (mapped_argument_buffer_) {
      auto &texture_view = Texture->view(View);
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.SRVTexture.resource_id = texture_view.gpuResourceID;
      gpu_storage.SRVTexture.metadata = TextureMetadata(Texture->arrayLength(View), ResourceMinLODClamp);
    }
    PublishSlot(Index, [&] { descriptors_[Index].Publish(device_->LookupCaptureSource(Texture)); });
    return S_OK;
  }
    virtual HRESULT
  AddConstantBufferView(UINT Index, UINT64 VA, UINT32 SizeInBytes) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::ConstantBuffer;
    cpu_storage.shape = {13, 0, 0, 0};
    cpu_storage.ConstantBuffer.address = VA;
    cpu_storage.ConstantBuffer.size = SizeInBytes;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.ConstantBuffer.address = VA;
      gpu_storage.ConstantBuffer.size = SizeInBytes;
    }
    uint64_t ignored_offset = 0;
    PublishSlot(Index, [&] { descriptors_[Index].Publish(device_->LookupCaptureByVA(VA, SizeInBytes, ignored_offset)); });
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Texture *Texture, TextureViewKey View, resource_shape::View Shape) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::UAVTexture;
    cpu_storage.shape = Shape;
    cpu_storage.UAVTexture.texture = Texture; // 
    cpu_storage.UAVTexture.view = View;
    if (mapped_argument_buffer_) {
      auto &texture_view = Texture->view(View);
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.UAVTexture.resource_id = texture_view.gpuResourceID;
      gpu_storage.UAVTexture.metadata = TextureMetadata(Texture->arrayLength(View), 0);
    }
    PublishSlot(Index, [&] { descriptors_[Index].Publish(device_->LookupCaptureSource(Texture)); });
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Buffer *UAVBuffer, BufferViewKey View, BufferSlice Slice, resource_shape::View Shape) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::UAVTexelBuffer;
    cpu_storage.shape = Shape;
    cpu_storage.UAVTexelBuffer.buffer = UAVBuffer;
    cpu_storage.UAVTexelBuffer.slice = Slice;
    cpu_storage.UAVTexelBuffer.view = View;
    if (mapped_argument_buffer_) { 
      auto &gpu_storage = mapped_argument_buffer_[Index];
      if (UAVBuffer) {
        auto &buffer_view = UAVBuffer->view_(View);
        gpu_storage.UAVTexelBuffer.resource_id = buffer_view.gpu_resource_id;
        gpu_storage.UAVTexelBuffer.metadata = ((uint64_t)Slice.elementCount << 32) | (uint64_t)(Slice.firstElement);
      } else {
        gpu_storage.UAVTexelBuffer.resource_id = 0;
        gpu_storage.UAVTexelBuffer.metadata = 0;
      }
    }
    PublishSlot(Index, [&] { descriptors_[Index].Publish(device_->LookupCaptureSource(UAVBuffer)); });
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Buffer *UAVBuffer, BufferSlice Slice, Buffer *Counter, UINT CounterOffsetInBytes, resource_shape::View Shape) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::UAVBuffer;
    cpu_storage.shape = Shape;
    cpu_storage.UAVBuffer.buffer = UAVBuffer;
    cpu_storage.UAVBuffer.slice = Slice;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      if (UAVBuffer) {
        gpu_storage.UAVBuffer.pointer = UAVBuffer->current()->gpuAddress() + Slice.byteOffset;
        gpu_storage.UAVBuffer.metadata = Slice.byteLength;
        gpu_storage.UAVBuffer.counter_pointer = Counter ? Counter->current()->gpuAddress() + CounterOffsetInBytes : 0;
      } else {
        gpu_storage.UAVBuffer.pointer = 0;
        gpu_storage.UAVBuffer.metadata = 0;
        gpu_storage.UAVBuffer.counter_pointer = 0;
      }
    }
    PublishSlot(Index, [&] { descriptors_[Index].Publish(device_->LookupCaptureSource(UAVBuffer), device_->LookupCaptureSource(Counter), Counter != nullptr); });
    return S_OK;
  }

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferViewKey View, BufferSlice Slice, resource_shape::View Shape) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::SRVTexelBuffer;
    cpu_storage.shape = Shape;
    cpu_storage.SRVTexelBuffer.buffer = Buffer;
    cpu_storage.SRVTexelBuffer.slice = Slice;
    cpu_storage.SRVTexelBuffer.view = View;
    if (mapped_argument_buffer_) { 
      auto &gpu_storage = mapped_argument_buffer_[Index];
      if (Buffer) {
        auto &buffer_view = Buffer->view_(View);
        gpu_storage.UAVTexelBuffer.resource_id = buffer_view.gpu_resource_id;
        gpu_storage.UAVTexelBuffer.metadata = ((uint64_t)Slice.elementCount << 32) | (uint64_t)(Slice.firstElement);
      } else {
        gpu_storage.UAVTexelBuffer.resource_id = 0;
        gpu_storage.UAVTexelBuffer.metadata = 0;
      }
    }
    PublishSlot(Index, [&] { descriptors_[Index].Publish(device_->LookupCaptureSource(Buffer)); });
    return S_OK;
  }

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferSlice Slice, resource_shape::View Shape) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::SRVBuffer;
    cpu_storage.shape = Shape;
    cpu_storage.SRVBuffer.buffer = Buffer;
    cpu_storage.SRVBuffer.slice = Slice;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      if (Buffer) {
        gpu_storage.SRVBuffer.pointer = Buffer->current()->gpuAddress() + Slice.byteOffset;
        gpu_storage.SRVBuffer.metadata = Slice.byteLength;
      } else {
        gpu_storage.SRVBuffer.pointer = 0;
        gpu_storage.SRVBuffer.metadata = 0;
      }
    }
    PublishSlot(Index, [&] { descriptors_[Index].Publish(device_->LookupCaptureSource(Buffer)); });
    return S_OK;
  }

  virtual HRESULT
  AddShaderResourceView(UINT Index, D3D12_SHADER_RESOURCE_VIEW_DESC const *pDesc) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    if (!pDesc)
      return E_INVALIDARG;
    /**
     * TODO: support null descriptor properly (respect different view dimensions)
     */
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::Null;
    cpu_storage.shape = {};
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.ZeroFilled = {{}};
    }
    PublishSlot(Index, [&] { descriptors_[Index].Publish({}); });
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, D3D12_UNORDERED_ACCESS_VIEW_DESC const *pDesc) override {
    SlotGuard lock(*this, Index);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    if (!pDesc)
      return E_INVALIDARG;
    /**
     * TODO: support null descriptor properly (respect different view dimensions)
     */
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::Null;
    cpu_storage.shape = {};
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.ZeroFilled = {{}};
    }
    PublishSlot(Index, [&] { descriptors_[Index].Publish({}); });
    return S_OK;
  }

  virtual ShaderVisibleDescriptorCPUStorage const &
  GetDescriptor(UINT Index) override {
    return descriptors_[Index].payload;
  }

  const void *GPURecords() override { return mapped_argument_buffer_; }   // LIGHTRN

  bool ReuseDescriptor(UINT index, reuse::Binding &out) override {
    out = {};
    std::lock_guard lock(slot_locks_.For(index));
    if (index >= descriptors_.size() || !mapped_argument_buffer_) return false;
    const auto &v = descriptors_[index].payload;
    out.type = unsigned(v.type);
    out.descriptor.Word(out.type);
    // These four qwords are canonical, initialized GPU storage, not a padded C++ union.
    uint64_t words[4]; std::memcpy(words, &mapped_argument_buffer_[index], sizeof(words));
    out.descriptor.Bytes(words, sizeof(words));
    switch (v.type) {
    case ShaderVisibleDescriptorType::Null: return true;
    case ShaderVisibleDescriptorType::ConstantBuffer:
      out.resource = reuse::FindAddress(v.ConstantBuffer.address, out.offset);
      out.length = v.ConstantBuffer.size; out.cbv = true; break;
    case ShaderVisibleDescriptorType::SRVTexture:
    case ShaderVisibleDescriptorType::UAVTexture:
      out.resource = reuse::FindIdentity(v.SRVTexture.texture);
      out.write = v.type == ShaderVisibleDescriptorType::UAVTexture; break;
    case ShaderVisibleDescriptorType::SRVBuffer:
    case ShaderVisibleDescriptorType::UAVBuffer:
      out.resource = reuse::FindIdentity(v.SRVBuffer.buffer);
      out.offset = v.SRVBuffer.slice.byteOffset; out.length = v.SRVBuffer.slice.byteLength;
      out.write = v.type == ShaderVisibleDescriptorType::UAVBuffer;
      if (out.write && words[2]) out.counter = reuse::FindAddress(words[2], out.counter_offset);
      break;
    case ShaderVisibleDescriptorType::SRVTexelBuffer:
    case ShaderVisibleDescriptorType::UAVTexelBuffer:
      out.resource = reuse::FindIdentity(v.SRVTexelBuffer.buffer);
      out.offset = v.SRVTexelBuffer.slice.byteOffset; out.length = v.SRVTexelBuffer.slice.byteLength;
      out.write = v.type == ShaderVisibleDescriptorType::UAVTexelBuffer; break;
    default: return false;
    }
    return bool(out.resource);
  }

  HRESULT CaptureDescriptor(UINT index, D3D12DescriptorCapture &out) override {
    out = {};
    std::lock_guard lock(slot_locks_.For(index));
    if (index >= descriptors_.size() || !mapped_argument_buffer_) return E_INVALIDARG;
    capture::Snapshot<D3D12ResourceCaptureSource, ShaderVisibleDescriptorCPUStorage> snapshot;
    const auto status = DescFast() ? AcquireFast(index, snapshot) : descriptors_[index].Acquire(snapshot);
    if (status != capture::Status::Ready)
      return status == capture::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    D3D12DescriptorCapture next;
    next.heap = this;
    next.type = snapshot.payload.type;
    next.shape = snapshot.payload.shape;
    next.resource = std::move(snapshot.resource);
    next.counter = std::move(snapshot.counter);
    std::memcpy(next.words.data(), &mapped_argument_buffer_[index], sizeof(next.words));
    const auto &cpu = snapshot.payload;
    auto &source = *next.resource->native;
    try {
      switch (cpu.type) {
      case ShaderVisibleDescriptorType::SRVTexture:
      case ShaderVisibleDescriptorType::UAVTexture: {
        const auto &desc = cpu.type == ShaderVisibleDescriptorType::SRVTexture ? cpu.SRVTexture : cpu.UAVTexture;
        if (!source.texture || source.texture.ptr() != desc.texture ||
            source.texture->current() != source.texture_allocation.ptr() || !source.texture_allocation->texture()) return DXGI_ERROR_UNSUPPORTED;
        auto &view = source.texture->view(desc.view);
        if (!view.texture || !view.gpuResourceID || view.gpuResourceID != next.words[0]) return E_INVALIDARG;
        next.texture = TextureViewRef(&view);
        break;
      }
      case ShaderVisibleDescriptorType::ConstantBuffer:
        if ((next.words[0] & 255) || !next.words[1] || (next.words[1] & 255) || next.words[1] > 65536 ||
            !source.BufferRange(next.words[0], next.words[1], next.buffer_offset)) return E_INVALIDARG;
        next.byte_length = next.words[1];
        break;
      case ShaderVisibleDescriptorType::SRVBuffer:
      case ShaderVisibleDescriptorType::UAVBuffer: {
        const auto &desc = cpu.type == ShaderVisibleDescriptorType::SRVBuffer ? cpu.SRVBuffer : cpu.UAVBuffer;
        if (!source.buffer || source.buffer.ptr() != desc.buffer ||
            !capture::Span(source.logical_length, desc.slice.byteOffset, desc.slice.byteLength) ||
            !source.BufferRange(next.words[0], next.words[1], next.buffer_offset) ||
            next.buffer_offset != desc.slice.byteOffset || next.words[1] != desc.slice.byteLength) return E_INVALIDARG;
        next.byte_length = next.words[1];
        if (next.counter && ((next.words[2] & 3) ||
            !next.counter->BufferRange(next.words[2], 4, next.counter_offset))) return E_INVALIDARG;
        if (!next.counter && next.words[2]) return E_INVALIDARG;
        break;
      }
      case ShaderVisibleDescriptorType::SRVTexelBuffer:
      case ShaderVisibleDescriptorType::UAVTexelBuffer: {
        const auto &desc = cpu.type == ShaderVisibleDescriptorType::SRVTexelBuffer ? cpu.SRVTexelBuffer : cpu.UAVTexelBuffer;
        if (!source.buffer || source.buffer.ptr() != desc.buffer || source.buffer->current() != source.buffer_allocation.ptr() || !source.buffer_allocation->buffer() ||
            !capture::Span(source.logical_length, desc.slice.byteOffset, desc.slice.byteLength)) return E_INVALIDARG;
        auto &view = source.buffer->view_(desc.view);
        if (!view.texture || !view.gpu_resource_id || view.gpu_resource_id != next.words[0]) return E_INVALIDARG;
        next.texel = view.texture;
        next.buffer_offset = desc.slice.byteOffset;
        next.byte_length = desc.slice.byteLength;
        break;
      }
      default: return DXGI_ERROR_UNSUPPORTED;
      }
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    out = std::move(next);
    return S_OK;
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12DescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) override {
    if (DescFast()) { FastCopyResourceDescriptors(this, From, pHeapTo, DescriptorTo, CopyCount); return; }
    if (!pHeapTo) return;
    Com<MTLD3D12DescriptorHeap> queried;
    if (FAILED(pHeapTo->QueryInterface(kD3D12ResourceHeapCaptureUUID, reinterpret_cast<void **>(&queried)))) return;
    auto *to = static_cast<MTLD3D12DescriptorHeapImpl *>(queried.ptr());
    if (to->device_ != device_) return;
    const auto copy = [&] {
      if (!mapped_argument_buffer_ || !to->mapped_argument_buffer_) return;
      capture::CopyRange(descriptors_.size(), From, to->descriptors_.size(), DescriptorTo, CopyCount, to == this,
          [&](uint64_t from, uint64_t dest) {
            to->descriptors_[dest] = descriptors_[from];
            std::memcpy(&to->mapped_argument_buffer_[dest], &mapped_argument_buffer_[from], sizeof(ShaderVisibleDescriptorGPUStorage));
          });
    };
    if (FineLocksEnabled()) {
      if (!mapped_argument_buffer_ || !to->mapped_argument_buffer_) return;
      // One slot at a time and never two slot locks together: no lock order to respect, no heap-wide stall.
      capture::CopyRange(descriptors_.size(), From, to->descriptors_.size(), DescriptorTo, CopyCount, to == this,
          [&](uint64_t from, uint64_t dest) {
            auto slot = [&] {
              std::lock_guard lock(slot_locks_.For(from));
              return std::pair(descriptors_[from], mapped_argument_buffer_[from]);
            }();
            std::lock_guard lock(to->slot_locks_.For(dest));
            to->descriptors_[dest] = slot.first;
            std::memcpy(&to->mapped_argument_buffer_[dest], &slot.second, sizeof(ShaderVisibleDescriptorGPUStorage));
          });
      return;
    }
    if (to == this) { std::lock_guard lock(slot_locks_.For(0)); copy(); }
    else { std::scoped_lock lock(slot_locks_.For(0), to->slot_locks_.For(0)); copy(); }
  }
};

class MTLD3D12RenderTargetDescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12RenderTargetDescriptorHeap> {

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<MTL_RENDER_TARGET_DESC> render_targets_;

public:
  MTLD3D12RenderTargetDescriptorHeapImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12RenderTargetDescriptorHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: {
      if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        return E_INVALIDARG;
      render_targets_.resize(pDesc->NumDescriptors);
      break;
    }
    default:
      return E_INVALIDARG;
    }
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override { DXMT_LT(109);
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) override { DXMT_LT(110);
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) override { DXMT_LT(111);
    *__ret = GetRenderTargetDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) override { DXMT_LT(112);
    __ret->ptr = 0;
    return __ret;
  }

  virtual HRESULT
  AddRenderTarget(UINT Index, MTL_RENDER_TARGET_DESC const *pDesc) override {
    if (Index >= render_targets_.size())
      return E_INVALIDARG;
    if (pDesc)
      render_targets_[Index] = *pDesc;
    else
      render_targets_[Index] = {};
    return S_OK;
  }

  virtual MTL_RENDER_TARGET_DESC
  GetRenderTarget(UINT Index) override {
    return render_targets_[Index];
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12RenderTargetDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) override {
    for (unsigned i = 0; i < CopyCount; i++) {
      static_cast<MTLD3D12RenderTargetDescriptorHeapImpl *>(pHeapTo)->render_targets_[DescriptorTo + i] =
          render_targets_[From + i];
    }
  }
};

struct SamplerGPUStorage {
  uint64_t sampler;
  uint64_t cube_sampler;
  uint64_t metadata;
  uint64_t padding;
};

class MTLD3D12SamplerDescriptorHeapImpl : public MTLD3D12Pageable<MTLD3D12SamplerDescriptorHeap> {
  friend void FastCopySamplerDescriptors(MTLD3D12SamplerDescriptorHeap *, UINT, MTLD3D12SamplerDescriptorHeap *, UINT, UINT);

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<Rc<Sampler>> samplers_;
  DescriptorSlotLocks slot_locks_;

  Rc<Buffer> buffer_;
  SamplerGPUStorage *mapped_argument_buffer_ = nullptr;
  uint64_t argument_buffer_gpu_address_ = 0;

public:
  MTLD3D12SamplerDescriptorHeapImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12SamplerDescriptorHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc) {
    if (!pDesc)
      return E_INVALIDARG;
    desc_ = *pDesc;
    switch (pDesc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
      break;
    }
    default:
      return E_INVALIDARG;
    }
    samplers_.resize(pDesc->NumDescriptors);

    if (pDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
      buffer_ = new Buffer(samplers_.size() * sizeof(SamplerGPUStorage), device_->GetMTLDevice());

      Flags<BufferAllocationFlag> flags;
#ifdef __i386__
      IMPLEMENT_ME
#endif
      buffer_->rename(buffer_->allocate(flags));
      mapped_argument_buffer_ = reinterpret_cast<SamplerGPUStorage *>(buffer_->current()->mappedMemory(0));
      argument_buffer_gpu_address_ = buffer_->current()->gpuAddress();
      // FIXME: is residency required for descriptor heap? Should be the case for Metal 4
      device_->RegisterResidencyAndVA(buffer_->current(), buffer_->length());
    } else {
      mapped_argument_buffer_ =
          reinterpret_cast<SamplerGPUStorage *>(malloc(samplers_.size() * sizeof(SamplerGPUStorage)));
    }

    return S_OK;
  };

  ~MTLD3D12SamplerDescriptorHeapImpl() {
     if (buffer_) {
      device_->UnregisterResidencyAndVA(buffer_->current());
    } else {
      free(mapped_argument_buffer_);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override { DXMT_LT(113);
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap) ||
        riid == kD3D12SamplerHeapCaptureUUID) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid)) {
      WARN("D3D12DescriptorHeap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) override { DXMT_LT(114);
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) override { DXMT_LT(115);
    *__ret = GetSamplerDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) override { DXMT_LT(116);
    __ret->ptr = argument_buffer_gpu_address_;
    return __ret;
  }


  virtual HRESULT
  AddSampler(UINT Index, const D3D12_SAMPLER_DESC *pDesc) override {
    std::lock_guard lock(slot_locks_.For(Index));
    if (!pDesc)
      return E_INVALIDARG;
    if (Index >= samplers_.size())
      return E_INVALIDARG;
  
    WMTSamplerInfo info;
    PopulateWMTSamplerInfo(device_->GetMTLDevice(), info, *pDesc);
    auto sampler = Sampler::createSampler(device_->GetMTLDevice(), info, pDesc->MipLODBias);
    shader_source::stats::CountSampler(pDesc->MipLODBias, pDesc->MinLOD, pDesc->MaxLOD);

    if (!sampler) return E_OUTOFMEMORY;
    samplers_[Index] = sampler;
    if (mapped_argument_buffer_) {
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.sampler = sampler->sampler_state_handle;
      gpu_storage.cube_sampler = sampler->sampler_state_cube_handle;
      gpu_storage.metadata = (uint64_t)std::bit_cast<uint32_t>(sampler->lod_bias);
      gpu_storage.padding = 0;
    }

    return S_OK;
  }

  HRESULT CaptureDescriptor(UINT index, D3D12DescriptorCapture &out) override {
    out = {};
    std::lock_guard lock(slot_locks_.For(index));
    if (index >= samplers_.size() || !samplers_[index] || !mapped_argument_buffer_) return E_INVALIDARG;
    D3D12DescriptorCapture next;
    next.heap = this;
    next.type = ShaderVisibleDescriptorType::Sampler;
    next.shape = {14, 0, 0, 0};
    next.sampler = samplers_[index];
    std::memcpy(next.words.data(), &mapped_argument_buffer_[index], sizeof(next.words));
    if (!next.words[0] || !next.words[1] || next.words[0] != next.sampler->sampler_state_handle || next.words[1] != next.sampler->sampler_state_cube_handle)
      return E_INVALIDARG;
    out = std::move(next);
    return S_OK;
  }

  virtual void
  CopyDescriptors(UINT From, MTLD3D12SamplerDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) override {
    if (DescFast()) { FastCopySamplerDescriptors(this, From, pHeapTo, DescriptorTo, CopyCount); return; }
    if (!pHeapTo) return;
    Com<MTLD3D12SamplerDescriptorHeap> queried;
    if (FAILED(pHeapTo->QueryInterface(kD3D12SamplerHeapCaptureUUID, reinterpret_cast<void **>(&queried)))) return;
    auto *to = static_cast<MTLD3D12SamplerDescriptorHeapImpl *>(queried.ptr());
    if (to->device_ != device_) return;
    const auto copy = [&] {
      if (!mapped_argument_buffer_ || !to->mapped_argument_buffer_) return;
      capture::CopyRange(samplers_.size(), From, to->samplers_.size(), DescriptorTo, CopyCount, to == this,
          [&](uint64_t from, uint64_t dest) {
            to->samplers_[dest] = samplers_[from];
            std::memcpy(&to->mapped_argument_buffer_[dest], &mapped_argument_buffer_[from], sizeof(SamplerGPUStorage));
          });
    };
    if (FineLocksEnabled()) {
      if (!mapped_argument_buffer_ || !to->mapped_argument_buffer_) return;
      capture::CopyRange(samplers_.size(), From, to->samplers_.size(), DescriptorTo, CopyCount, to == this,
          [&](uint64_t from, uint64_t dest) {
            auto slot = [&] {
              std::lock_guard lock(slot_locks_.For(from));
              return std::pair(samplers_[from], mapped_argument_buffer_[from]);
            }();
            std::lock_guard lock(to->slot_locks_.For(dest));
            to->samplers_[dest] = slot.first;
            std::memcpy(&to->mapped_argument_buffer_[dest], &slot.second, sizeof(SamplerGPUStorage));
          });
      return;
    }
    if (to == this) { std::lock_guard lock(slot_locks_.For(0)); copy(); }
    else { std::scoped_lock lock(slot_locks_.For(0), to->slot_locks_.For(0)); copy(); }
  }
};

// CPU-LATE 23.10 (DESC_FAST): the handle embeds the heap pointer; every MTLD3D12DescriptorHeap /
// MTLD3D12SamplerDescriptorHeap of this device is the Impl above. Bounds as capture::CopyRange; memmove order on overlap.
void FastCopyResourceDescriptors(MTLD3D12DescriptorHeap *src_heap, UINT from, MTLD3D12DescriptorHeap *dst_heap, UINT to, UINT count) {
  if (!src_heap || !dst_heap || !count) return;
  auto *src = static_cast<MTLD3D12DescriptorHeapImpl *>(src_heap);
  auto *dst = static_cast<MTLD3D12DescriptorHeapImpl *>(dst_heap);
  if (dst->device_ != src->device_ || !src->mapped_argument_buffer_ || !dst->mapped_argument_buffer_ || !src->fast_state_ ||
      !dst->fast_state_) return;
  if (!capture::Span(src->descriptors_.size(), from, count) || !capture::Span(dst->descriptors_.size(), to, count)) return;
  if (src == dst && from == to) return;
  // Element copies written out (no CRT memmove: an imported call is an indirect call in ARM64EC, i.e. one more call check).
  const auto one = [&](UINT f, UINT d) {
    src->RaceReadEnter(f);
    dst->RaceWriteEnter(d);
    const auto *g = reinterpret_cast<const uint64_t *>(&src->mapped_argument_buffer_[f]);
    auto *h = reinterpret_cast<uint64_t *>(&dst->mapped_argument_buffer_[d]);
    const uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3];
    h[0] = g0; h[1] = g1; h[2] = g2; h[3] = g3;
    dst->descriptors_[d].payload = src->descriptors_[f].payload;
    dst->fast_state_[d] = src->fast_state_[f];
    dst->RaceWriteExit(d);
    src->RaceReadExit(f);
  };
  if (src == dst && to > from && to - from < count) {
    for (UINT n = count; n; --n) one(from + n - 1, to + n - 1);
  } else {
    for (UINT n = 0; n < count; ++n) one(from + n, to + n);
  }
}

void FastAddConstantBufferView(MTLD3D12DescriptorHeap *heap, UINT index, UINT64 va, UINT32 size) {
  auto *h = static_cast<MTLD3D12DescriptorHeapImpl *>(heap);
  if (!h || index >= h->descriptors_.size() || !h->fast_state_) return;
  h->RaceWriteEnter(index);
  auto &cpu_storage = h->descriptors_[index].payload;
  cpu_storage.type = ShaderVisibleDescriptorType::ConstantBuffer;
  cpu_storage.shape = {13, 0, 0, 0};
  cpu_storage.ConstantBuffer.address = va;
  cpu_storage.ConstantBuffer.size = size;
  if (h->mapped_argument_buffer_) {
    auto &gpu_storage = h->mapped_argument_buffer_[index];
    capture::ClearDescriptor(gpu_storage);
    gpu_storage.ConstantBuffer.address = va;
    gpu_storage.ConstantBuffer.size = size;
  }
  h->fast_state_[index] = 1;
  h->RaceWriteExit(index);
}

void FastCopySamplerDescriptors(MTLD3D12SamplerDescriptorHeap *src_heap, UINT from, MTLD3D12SamplerDescriptorHeap *dst_heap, UINT to,
                                UINT count) {
  if (!src_heap || !dst_heap || !count) return;
  auto *src = static_cast<MTLD3D12SamplerDescriptorHeapImpl *>(src_heap);
  auto *dst = static_cast<MTLD3D12SamplerDescriptorHeapImpl *>(dst_heap);
  if (dst->device_ != src->device_ || !src->mapped_argument_buffer_ || !dst->mapped_argument_buffer_) return;
  capture::CopyRange(src->samplers_.size(), from, dst->samplers_.size(), to, count, src == dst,
      [&](uint64_t f, uint64_t d) {
        dst->samplers_[d] = src->samplers_[f];
        std::memcpy(&dst->mapped_argument_buffer_[d], &src->mapped_argument_buffer_[f], sizeof(SamplerGPUStorage));
      });
}

HRESULT
CreateDescriptorHeap(
    MTLD3D12Device *pDevice, const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap
) {
  InitReturnPtr(ppDescriptorHeap);
  if (!pDesc)
    return E_INVALIDARG;
  if (pDesc->NumDescriptors > 0xFFFFF) {
    ERR("CreateDescriptorHeap: NumDescriptors is too large");
    return E_INVALIDARG;
  }
  switch (pDesc->Type) {
  case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
    auto descriptor_heap = Com(new MTLD3D12DescriptorHeapImpl(pDevice));
    HRESULT hr = descriptor_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return descriptor_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
    auto sampler_heap = Com(new MTLD3D12SamplerDescriptorHeapImpl(pDevice));
    HRESULT hr = sampler_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return sampler_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
  case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: {
    auto descriptor_heap = Com(new MTLD3D12RenderTargetDescriptorHeapImpl(pDevice));
    HRESULT hr = descriptor_heap->Initialize(pDesc);
    if (FAILED(hr))
      return hr;
    if (!ppDescriptorHeap)
      return S_FALSE;
    return descriptor_heap->QueryInterface(riid, ppDescriptorHeap);
  }
  default:
    break;
  }
  return E_INVALIDARG;
}

} // namespace dxmt
