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
namespace reuse { struct Binding; }

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
  virtual bool ReuseDescriptor(UINT index, reuse::Binding &out) = 0;
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
  // LIGHTRN: the shader-visible 32-byte GPU records (record i at + 32 * i); null for a CPU-only heap
  virtual const void *GPURecords() = 0;

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

// CPU-LATE 23.10.2026 (DESC_FAST): descriptor writes and copies without per-call bookkeeping.
//   MACRUNNER_DX12_DESC_FAST=1  CBV/SRV/UAV/sampler copies and view writes take the heap straight from the handle (no
//   QueryInterface / AddRef / Release), take no slot lock (D3D12 makes the application keep a descriptor from being written
//   while it is read or copied), and copy the CPU record and the 32-byte GPU record with plain stores. The capture source of a
//   descriptor (ray tracing bindings, capture diagnostics) is not published at write time; CaptureDescriptor resolves it on
//   demand from the record (resource identity, or GPU address for CBV / UAV counter). =0 keeps the previous path. Unset: the
//   build default below. Read once per process.
inline constexpr bool kDescFastDefault = false;
inline bool ReadDescFastKey() {
  const DWORD error = GetLastError();
  char text[4]{};
  const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_DESC_FAST", text, sizeof(text));
  SetLastError(error);
  if (n == 0 || n >= sizeof(text)) return kDescFastDefault;
  return !(text[0] == '0' && text[1] == 0);
}
inline const bool g_desc_fast = ReadDescFastKey();
// MACRUNNER_DX12_DESC_RACE=1 (diagnostic, with DESC_FAST): every fast write marks its slot busy-for-write and every copy marks
// its source busy-for-read for the duration of the operation (atomics, so not free); an operation that finds the slot busy
// with a write (or a writer that finds readers) is counted. A non-zero count is a real overlap; zero bounds it, not proves.
inline bool ReadDescRaceKey() {
  const DWORD error = GetLastError();
  char text[4]{};
  const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_DESC_RACE", text, sizeof(text));
  SetLastError(error);
  return n == 1 && text[0] == '1';
}
inline const bool g_desc_race = ReadDescRaceKey();
inline bool DescFast() { return g_desc_fast; }
// Direct (non-virtual) entry points of the fast path: no indirect call, so no ARM64EC call check either.
void FastCopyResourceDescriptors(MTLD3D12DescriptorHeap *src, UINT from, MTLD3D12DescriptorHeap *dst, UINT to, UINT count);
void FastCopySamplerDescriptors(MTLD3D12SamplerDescriptorHeap *src, UINT from, MTLD3D12SamplerDescriptorHeap *dst, UINT to,
                                UINT count);
void FastAddConstantBufferView(MTLD3D12DescriptorHeap *heap, UINT index, UINT64 va, UINT32 size);

} // namespace dxmt
