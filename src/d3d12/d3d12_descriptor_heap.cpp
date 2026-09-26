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
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pageable.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_sampler.hpp"
#include <mutex>
#include "log/log.hpp"

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
  std::mutex capture_mutex_;
  Rc<Buffer> buffer_;
  ShaderVisibleDescriptorGPUStorage *mapped_argument_buffer_ = nullptr;
  uint64_t argument_buffer_gpu_address_ = 0;

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
  QueryInterface(REFIID riid, void **ppvObject) override {
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
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) override {
    *__ret = GetShaderVisibleDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) override {
    __ret->ptr = argument_buffer_gpu_address_;
    return __ret;
  }

  virtual HRESULT
  AddShaderResourceView(UINT Index, Texture *Texture, TextureViewKey View, FLOAT ResourceMinLODClamp, resource_shape::View Shape) override {
    std::lock_guard lock(capture_mutex_);
    if (Index >= descriptors_.size())
      return E_INVALIDARG;
    auto &cpu_storage = descriptors_[Index].payload;
    if (mapped_argument_buffer_) capture::ClearDescriptor(mapped_argument_buffer_[Index]);
    cpu_storage.type = ShaderVisibleDescriptorType::SRVTexture;
    cpu_storage.shape = Shape;
    cpu_storage.SRVTexture.texture = Texture;
    cpu_storage.SRVTexture.view = View;
    if (mapped_argument_buffer_) {
      auto &texture_view = Texture->view(View);
      auto &gpu_storage = mapped_argument_buffer_[Index];
      gpu_storage.SRVTexture.resource_id = texture_view.gpuResourceID;
      gpu_storage.SRVTexture.metadata = TextureMetadata(Texture->arrayLength(View), ResourceMinLODClamp);
    }
    descriptors_[Index].Publish(device_->LookupCaptureSource(Texture));
    return S_OK;
  }
    virtual HRESULT
  AddConstantBufferView(UINT Index, UINT64 VA, UINT32 SizeInBytes) override {
    std::lock_guard lock(capture_mutex_);
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
    descriptors_[Index].Publish(device_->LookupCaptureByVA(VA, SizeInBytes, ignored_offset));
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Texture *Texture, TextureViewKey View, resource_shape::View Shape) override {
    std::lock_guard lock(capture_mutex_);
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
    descriptors_[Index].Publish(device_->LookupCaptureSource(Texture));
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Buffer *UAVBuffer, BufferViewKey View, BufferSlice Slice, resource_shape::View Shape) override {
    std::lock_guard lock(capture_mutex_);
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
    descriptors_[Index].Publish(device_->LookupCaptureSource(UAVBuffer));
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, Buffer *UAVBuffer, BufferSlice Slice, Buffer *Counter, UINT CounterOffsetInBytes, resource_shape::View Shape) override {
    std::lock_guard lock(capture_mutex_);
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
    descriptors_[Index].Publish(device_->LookupCaptureSource(UAVBuffer), device_->LookupCaptureSource(Counter), Counter != nullptr);
    return S_OK;
  }

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferViewKey View, BufferSlice Slice, resource_shape::View Shape) override {
    std::lock_guard lock(capture_mutex_);
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
    descriptors_[Index].Publish(device_->LookupCaptureSource(Buffer));
    return S_OK;
  }

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferSlice Slice, resource_shape::View Shape) override {
    std::lock_guard lock(capture_mutex_);
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
    descriptors_[Index].Publish(device_->LookupCaptureSource(Buffer));
    return S_OK;
  }

  virtual HRESULT
  AddShaderResourceView(UINT Index, D3D12_SHADER_RESOURCE_VIEW_DESC const *pDesc) override {
    std::lock_guard lock(capture_mutex_);
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
    descriptors_[Index].Publish({});
    return S_OK;
  }

  virtual HRESULT
  AddUnorderedAccessView(UINT Index, D3D12_UNORDERED_ACCESS_VIEW_DESC const *pDesc) override {
    std::lock_guard lock(capture_mutex_);
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
    descriptors_[Index].Publish({});
    return S_OK;
  }

  virtual ShaderVisibleDescriptorCPUStorage const &
  GetDescriptor(UINT Index) override {
    return descriptors_[Index].payload;
  }

  HRESULT CaptureDescriptor(UINT index, D3D12DescriptorCapture &out) override {
    out = {};
    std::lock_guard lock(capture_mutex_);
    if (index >= descriptors_.size() || !mapped_argument_buffer_) return E_INVALIDARG;
    capture::Snapshot<D3D12ResourceCaptureSource, ShaderVisibleDescriptorCPUStorage> snapshot;
    const auto status = descriptors_[index].Acquire(snapshot);
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
    if (to == this) { std::lock_guard lock(capture_mutex_); copy(); }
    else { std::scoped_lock lock(capture_mutex_, to->capture_mutex_); copy(); }
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
  QueryInterface(REFIID riid, void **ppvObject) override {
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
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) override {
    *__ret = GetRenderTargetDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) override {
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

  D3D12_DESCRIPTOR_HEAP_DESC desc_;

  std::vector<Rc<Sampler>> samplers_;
  std::mutex capture_mutex_;

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
  QueryInterface(REFIID riid, void **ppvObject) override {
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
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }

  virtual D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) override {
    *__ret = GetSamplerDescriptor(this, 0);
    return __ret;
  }

  virtual D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) override {
    __ret->ptr = argument_buffer_gpu_address_;
    return __ret;
  }


  virtual HRESULT
  AddSampler(UINT Index, const D3D12_SAMPLER_DESC *pDesc) override {
    std::lock_guard lock(capture_mutex_);
    if (!pDesc)
      return E_INVALIDARG;
    if (Index >= samplers_.size())
      return E_INVALIDARG;
  
    WMTSamplerInfo info;
    PopulateWMTSamplerInfo(device_->GetMTLDevice(), info, *pDesc);
    auto sampler = Sampler::createSampler(device_->GetMTLDevice(), info, pDesc->MipLODBias);

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
    std::lock_guard lock(capture_mutex_);
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
    if (to == this) { std::lock_guard lock(capture_mutex_); copy(); }
    else { std::scoped_lock lock(capture_mutex_, to->capture_mutex_); copy(); }
  }
};

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
