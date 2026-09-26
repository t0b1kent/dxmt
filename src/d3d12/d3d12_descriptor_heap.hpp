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

#pragma once
#include "d3d12_resource_shape.hpp"
#include "d3d12.h"
#include "dxmt_buffer.hpp"
#include "dxmt_texture.hpp"
#include <cstdint>

#if UINTPTR_MAX == 0xffffffffffffffffULL
#define DXMT_USE_EMBEDDED_HEAP_POINTER
#endif

namespace dxmt {

struct EMBEDDED_DESCRIPTOR_HANDLE {
#ifndef DXMT_USE_EMBEDDED_HEAP_POINTER
  SIZE_T Tag        : 5;
  SIZE_T Descriptor : 20;
  SIZE_T Heap       : 7;
#else
  SIZE_T Tag        : 5;
  SIZE_T Descriptor : 20;
  SIZE_T Heap       : 39;

  // assume pointer is 8-byte aligned, providing 3 free bits
  template <typename T>
  T *
  extract() {
    return reinterpret_cast<T *>((Heap << 8) | (Tag << 3));
  }

  EMBEDDED_DESCRIPTOR_HANDLE(const void *heap, SIZE_T index) {
    Heap = (SIZE_T)heap >> 8;
    Tag = (SIZE_T)heap >> 3;
    Descriptor = index;
  }
#endif

  EMBEDDED_DESCRIPTOR_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE Handle) {
    union {
      D3D12_CPU_DESCRIPTOR_HANDLE d3d12;
      EMBEDDED_DESCRIPTOR_HANDLE impl;
    } _;
    _.d3d12 = Handle;
    *this = _.impl;
  }

  operator D3D12_CPU_DESCRIPTOR_HANDLE() const {
    union {
      D3D12_CPU_DESCRIPTOR_HANDLE d3d12;
      EMBEDDED_DESCRIPTOR_HANDLE impl;
    } _;
    _.impl = *this;
    return _.d3d12;
  };

  EMBEDDED_DESCRIPTOR_HANDLE() = default;
};

static_assert(sizeof(EMBEDDED_DESCRIPTOR_HANDLE) == sizeof(D3D12_CPU_DESCRIPTOR_HANDLE));

enum class ShaderVisibleDescriptorType {
  Null,
  SRVTexture,
  ConstantBuffer,
  UAVTexture,
  UAVTexelBuffer,
  UAVBuffer,
  SRVTexelBuffer,
  SRVBuffer,
  Sampler,
};

struct SRVTextureCPUStorage {
  Texture *texture = nullptr;
  TextureViewKey view{};
};

using UAVTextureCPUStorage = SRVTextureCPUStorage;

struct UAVTexelBufferCPUStorage {
  Buffer *buffer = nullptr;
  BufferViewKey view{};
  BufferSlice slice{};
};

using SRVTexelBufferCPUStorage = UAVTexelBufferCPUStorage;

struct CBVCommonStorage {
  uint64_t address = 0;
  uint64_t size = 0;
};

struct UAVBufferCPUStorage {
  Buffer *buffer = nullptr;
  BufferSlice slice{};
};

using SRVBufferCPUStorage = UAVBufferCPUStorage;

struct ShaderVisibleDescriptorCPUStorage {
  ShaderVisibleDescriptorType type;
  resource_shape::View shape;
  union {
    SRVTextureCPUStorage SRVTexture;
    CBVCommonStorage ConstantBuffer;
    UAVTextureCPUStorage UAVTexture;
    UAVTexelBufferCPUStorage UAVTexelBuffer;
    UAVBufferCPUStorage UAVBuffer;
    SRVTexelBufferCPUStorage SRVTexelBuffer;
    SRVBufferCPUStorage SRVBuffer;
  };

  ShaderVisibleDescriptorCPUStorage() : type(ShaderVisibleDescriptorType::Null) {}
};

struct D3D12DescriptorCapture;
inline constexpr GUID kD3D12ResourceHeapCaptureUUID = {0x6f6f5a8c, 0x47eb, 0x4d8e, {0x87, 0xe0, 0x92, 0x4a, 0x2d, 0x01, 0x21, 0x46}};
inline constexpr GUID kD3D12SamplerHeapCaptureUUID = {0x08c6bf12, 0xea50, 0x416e, {0xa6, 0x6b, 0x6a, 0xd8, 0x1f, 0xa3, 0xe1, 0x46}};

class MTLD3D12DescriptorHeap : public ID3D12DescriptorHeap {
public:
  virtual HRESULT CaptureDescriptor(UINT index, D3D12DescriptorCapture &out) = 0;
  virtual HRESULT
  AddShaderResourceView(UINT Index, Texture *Texture, TextureViewKey View, FLOAT ResourceMinLODClamp, resource_shape::View Shape) = 0;

  virtual HRESULT AddConstantBufferView(UINT Index, UINT64 VA, UINT32 SizeInBytes) = 0;

  virtual HRESULT AddUnorderedAccessView(UINT Index, Texture *Texture, TextureViewKey View, resource_shape::View Shape) = 0;

  virtual HRESULT AddUnorderedAccessView(UINT Index, Buffer *Buffer, BufferViewKey View, BufferSlice Slice, resource_shape::View Shape) = 0;

  virtual HRESULT AddUnorderedAccessView(
      UINT Index, Buffer *UAVBuffer, BufferSlice Slice, Buffer *Counter, UINT CounterOffsetInBytes, resource_shape::View Shape
  ) = 0;

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferViewKey View, BufferSlice Slice, resource_shape::View Shape) = 0;

  virtual HRESULT AddShaderResourceView(UINT Index, Buffer *Buffer, BufferSlice Slice, resource_shape::View Shape) = 0;

  virtual HRESULT AddShaderResourceView(UINT Index, D3D12_SHADER_RESOURCE_VIEW_DESC const *pDesc) = 0;

  virtual HRESULT AddUnorderedAccessView(UINT Index, D3D12_UNORDERED_ACCESS_VIEW_DESC const *pDesc) = 0;

  virtual ShaderVisibleDescriptorCPUStorage const &GetDescriptor(UINT Index) = 0;

  virtual void CopyDescriptors(UINT From, MTLD3D12DescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) = 0;
};

class MTLD3D12SamplerDescriptorHeap : public ID3D12DescriptorHeap {
public:
  virtual HRESULT CaptureDescriptor(UINT index, D3D12DescriptorCapture &out) = 0;
  virtual HRESULT AddSampler(UINT Index, const D3D12_SAMPLER_DESC *Desc) = 0;

  virtual void CopyDescriptors(UINT From, MTLD3D12SamplerDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) = 0;
};

struct MTL_RENDER_TARGET_DESC {
  Texture *Texture;
  TextureViewKey View;
  UINT Width;
  UINT Height;
  UINT DepthPlane;
  UINT RenderTargetArrayLength;
  UINT Flags;
};

class MTLD3D12RenderTargetDescriptorHeap : public ID3D12DescriptorHeap {
public:
  virtual HRESULT AddRenderTarget(UINT Index, MTL_RENDER_TARGET_DESC const *pDesc) = 0;
  virtual MTL_RENDER_TARGET_DESC GetRenderTarget(UINT Index) = 0;

  virtual void CopyDescriptors(UINT From, MTLD3D12RenderTargetDescriptorHeap *pHeapTo, UINT DescriptorTo, UINT CopyCount) = 0;
};

} // namespace dxmt
