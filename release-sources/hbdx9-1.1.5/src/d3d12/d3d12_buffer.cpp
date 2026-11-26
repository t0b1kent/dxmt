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
#include "d3d12_pageable.hpp"
#include "com/com_pointer.hpp"
#include "dxmt_format.hpp"

namespace dxmt {

class MTLD3D12Buffer : public MTLD3D12Pageable<MTLD3D12Resource> {
  D3D12_RESOURCE_DESC desc_;
  D3D12_HEAP_PROPERTIES heap_props_;
  D3D12_HEAP_FLAGS heap_flags_;

public:
  MTLD3D12Buffer(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Resource>(pDevice) {}

  HRESULT
  Initialize(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, MTLD3D12Heap *pHeap, UINT64 Offset
  ) {
    if (OptimizedClearValue)
      return E_INVALIDARG;

    // TODO: validate and normalize
    desc_ = *pDesc;
    heap_props_ = *pHeapProps;
    heap_flags_ = HeapFlags;

    if (desc_.Alignment) {
      if (desc_.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
        return E_INVALIDARG;
    } else {
      desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }

    if (pHeap) {
      auto size_and_align = device_->GetMTLDevice().heapBufferSizeAndAlign(desc_.Width, {});
      if (size_and_align.size + Offset > pHeap->GetDesc().SizeInBytes)
        return E_INVALIDARG;
    }

    buffer = new Buffer(desc_.Width, device_->GetMTLDevice());

    Flags<BufferAllocationFlag> flags;
    if (pHeap && pHeap->placement_sparse_compatible)
      flags.set(BufferAllocationFlag::GpuPrivate);
    if (pHeap)
      buffer->rename(buffer->allocate(flags, pHeap->heap, Offset));
    else
      buffer->rename(buffer->allocate(flags));
    device_->RegisterResidencyAndVA(buffer->current(), desc_.Width);

    if (!pHeap) {
      try {
        capture_source = std::make_shared<D3D12ResourceCaptureSource>(static_cast<ID3D12Device *>(device_), buffer, desc_.Width, device_->GetResidencyOwner());
        device_->RegisterCaptureSource(capture_source);
      } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    }

    return S_OK;
  };

  HRESULT InitializeReserved(const D3D12_RESOURCE_DESC *desc) {
    desc_ = *desc;
    desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_props_ = {};
    heap_props_.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_flags_ = D3D12_HEAP_FLAG_NONE;
    const uint64_t length = (desc_.Width + 65535) & ~uint64_t(65535);
    buffer = new Buffer(length, device_->GetMTLDevice());
    Flags<BufferAllocationFlag> flags;
    flags.set(BufferAllocationFlag::GpuPrivate);
    flags.set(BufferAllocationFlag::PlacementSparse);
    buffer->rename(buffer->allocate(flags));
    if (!buffer->current()->buffer()) {
      buffer = {};
      return E_OUTOFMEMORY;
    }
    device_->RegisterResidencyAndVA(buffer->current(), desc_.Width);
    sparse = std::make_shared<D3D12SparseBufferState>();
    sparse->buffer = buffer->current()->buffer();
    sparse->tiles = length / 65536;
    sparse->residency = device_->GetResidencyOwner();
    sparse->residency->Retain(sparse->buffer);
    return S_OK;
  }

  ~MTLD3D12Buffer() {
    if (capture_source) {
      capture_source->live.store(false, std::memory_order_release);
      device_->UnregisterCaptureSource(*capture_source);
      capture_source.reset();
    }
    if (buffer)
      device_->UnregisterResidencyAndVA(buffer->current());
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource) ||
        riid == kD3D12ResourceCaptureImplementationUUID) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12Buffer: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  Map(UINT Subresource, const D3D12_RANGE *pReadRange, void **ppData) {
    if (Subresource)
      return E_INVALIDARG;
    if (heap_props_.Type == D3D12_HEAP_TYPE_DEFAULT)
      return E_INVALIDARG;
    if (ppData)
      *ppData = buffer->current()->mappedMemory(0);
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE Unmap(UINT Subresource, const D3D12_RANGE *pWrittenRange) {
    // no-op
  };

  virtual D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };

  virtual D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() {
    return buffer->current()->gpuAddress();
  };

  virtual HRESULT STDMETHODCALLTYPE
  WriteToSubresource(
      UINT DstSubresource, const D3D12_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcSlicePitch
  ) {
    return E_INVALIDARG;
  };

  virtual HRESULT STDMETHODCALLTYPE
  ReadFromSubresource(
      void *pDstData, UINT DstRowPitch, UINT DstSlicePitch, UINT SrcSubresource, const D3D12_BOX *pSrcBox
  ) {
    return E_INVALIDARG;
  };

  virtual HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS *pFlags) {
    if (sparse)
      return E_INVALIDARG;
    if (pHeapProps)
      *pHeapProps = heap_props_;
    if (pFlags)
      *pFlags = heap_flags_;
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    HRESULT hr;
    D3D12_SHADER_RESOURCE_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
    }

    if (ViewDesc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
      return E_INVALIDARG;

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    const auto shape = resource_shape::Buffer(UINT(ViewDesc.Format), ViewDesc.Buffer.StructureByteStride, UINT(ViewDesc.Buffer.Flags), false);
    BufferSlice Slice;

    if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN || ViewDesc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
      UINT Stride = (ViewDesc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4 : ViewDesc.Buffer.StructureByteStride;
      const auto status = capture::BufferSlice(desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements, Stride, Slice);
      if (status != capture::Status::Ready)
        return status == capture::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
      return Heap->AddShaderResourceView(Index, buffer.ptr(), Slice, shape);
    }

    MTL_DXGI_FORMAT_DESC Format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, Format))) {
      ERR("D3D12Buffer::CreateShaderResourceView: not an ordinary or packed format: ", ViewDesc.Format);
      return E_FAIL;
    }
    BufferViewDescriptor view_descriptor{Format.PixelFormat};
    const auto status = capture::BufferSlice(desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements, Format.BytesPerTexel, Slice);
    if (status != capture::Status::Ready)
      return status == capture::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;

    auto view = buffer->createView(view_descriptor);
    return Heap->AddShaderResourceView(Index, buffer.ptr(), view, Slice, shape);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateUnorderedAccessView(
      ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    HRESULT hr;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ViewDesc;
    if (!pDesc) {
      hr = ExtractEntireResourceViewDescription(desc_, &ViewDesc);
      if (FAILED(hr))
        return hr;
    } else {
      ViewDesc = *pDesc;
    }

    if (ViewDesc.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
      return E_INVALIDARG;

    static_assert(D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT == 4096);
    Com<MTLD3D12Resource> counter;
    UINT counter_offset = 0;
    if (pCounter) {
      if (!pDesc || ViewDesc.Format != DXGI_FORMAT_UNKNOWN || !ViewDesc.Buffer.StructureByteStride ||
          (ViewDesc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW)) return E_INVALIDARG;
      Com<ID3D12Device> owner;
      if (FAILED(pCounter->QueryInterface(kD3D12ResourceCaptureImplementationUUID, reinterpret_cast<void **>(&counter))) ||
          !counter || !counter->buffer ||
          FAILED(counter->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner))) ||
          owner.ptr() != static_cast<ID3D12Device *>(device_)) return E_INVALIDARG;
      const auto desc = counter->GetDesc();
      if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return E_INVALIDARG;
      const auto status = capture::CounterOffset(desc.Width, ViewDesc.Buffer.CounterOffsetInBytes, true, counter_offset);
      if (status != capture::Status::Ready)
        return status == capture::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
    } else if (ViewDesc.Buffer.CounterOffsetInBytes) return E_INVALIDARG;

    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(device_, Descriptor);
    const auto shape = resource_shape::Buffer(UINT(ViewDesc.Format), ViewDesc.Buffer.StructureByteStride, UINT(ViewDesc.Buffer.Flags), pCounter != nullptr);
    BufferSlice Slice;

    if (ViewDesc.Format == DXGI_FORMAT_UNKNOWN || ViewDesc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
      UINT Stride = (ViewDesc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? 4 : ViewDesc.Buffer.StructureByteStride;
      const auto status = capture::BufferSlice(desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements, Stride, Slice);
      if (status != capture::Status::Ready)
        return status == capture::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;
      if (!pCounter)
        return Heap->AddUnorderedAccessView(Index, buffer.ptr(), Slice, nullptr, 0, shape);

      return Heap->AddUnorderedAccessView(Index, buffer.ptr(), Slice, counter->buffer.ptr(), counter_offset, shape);
    }

    MTL_DXGI_FORMAT_DESC Format;
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), ViewDesc.Format, Format))) {
      ERR("D3D12Buffer::CreateUnorderedAccessView: not an ordinary or packed format: ", ViewDesc.Format);
      return E_FAIL;
    }
    BufferViewDescriptor view_descriptor{Format.PixelFormat};
    const auto status = capture::BufferSlice(desc_.Width, ViewDesc.Buffer.FirstElement, ViewDesc.Buffer.NumElements, Format.BytesPerTexel, Slice);
    if (status != capture::Status::Ready)
      return status == capture::Status::Unsupported ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG;

    auto view = buffer->createView(view_descriptor);
    return Heap->AddUnorderedAccessView(Index, buffer.ptr(), view, Slice, shape);
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    IMPLEMENT_ME
    return S_OK;
  };

  virtual HRESULT STDMETHODCALLTYPE
  CreateDepthStencilView(const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    IMPLEMENT_ME
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE GetResourceTiling(
      UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo, D3D12_TILE_SHAPE *StandardTitleShape,
      UINT *SubresourceTilingCount, UINT FirstSubresourceTiling, D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    if (TotalTileCount) *TotalTileCount = sparse ? sparse->tiles : 0;
    if (PackedMipInfo) *PackedMipInfo = sparse ? D3D12_PACKED_MIP_INFO{1, 0, 0, 0} : D3D12_PACKED_MIP_INFO{};
    if (StandardTitleShape) *StandardTitleShape = sparse ? D3D12_TILE_SHAPE{65536, 1, 1} : D3D12_TILE_SHAPE{};
    if (SubresourceTilingCount) {
      const UINT written = sparse && FirstSubresourceTiling == 0 && *SubresourceTilingCount && SubresourceTilings ? 1 : 0;
      if (written) *SubresourceTilings = {sparse->tiles, 1, 1, 0};
      *SubresourceTilingCount = written;
    }
  };
};

HRESULT CreateReservedBuffer(
    MTLD3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE *clear, REFIID riid, void **resource
) {
  InitReturnPtr(resource);
  if (!desc || !desc->Width || clear) return E_INVALIDARG;
  if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return E_NOTIMPL;
  if (desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR || desc->Format != DXGI_FORMAT_UNKNOWN ||
      desc->Height != 1 || desc->DepthOrArraySize != 1 || desc->MipLevels != 1 ||
      desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality ||
      (desc->Alignment && desc->Alignment != 65536)) return E_INVALIDARG;
  // Buffer views currently use 32-bit byte offsets. Do not truncate a wider reservation.
  if (desc->Width > UINT64_C(0xffff0000) || state != D3D12_RESOURCE_STATE_COMMON ||
      (desc->Flags != D3D12_RESOURCE_FLAG_NONE && desc->Flags != D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    return E_NOTIMPL;
  if (!device->GetMTLDevice().supportsPlacementSparse()) return E_NOTIMPL;
  auto buffer = Com(new MTLD3D12Buffer(device));
  HRESULT hr = buffer->InitializeReserved(desc);
  if (FAILED(hr)) return hr;
  if (!resource) return S_FALSE;
  return buffer->QueryInterface(riid, resource);
}

HRESULT
CreateCommittedBuffer(
    MTLD3D12Device *pDevice, const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags,
    const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
    REFIID riid, void **ppResource
) {
  auto buffer = Com(new MTLD3D12Buffer(pDevice));
  HRESULT hr = buffer->Initialize(pHeapProps, HeapFlags, pDesc, OptimizedClearValue, nullptr, 0);
  if (FAILED(hr))
    return hr;
  if (!ppResource)
    return S_FALSE;
  return buffer->QueryInterface(riid, ppResource);
}

HRESULT
CreatePlacedBuffer(
    MTLD3D12Device *pDevice, MTLD3D12Heap *pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
) {
  auto buffer = Com(new MTLD3D12Buffer(pDevice));
  D3D12_HEAP_DESC heap_desc = pHeap->GetDesc();

  if (heap_desc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)
    return E_INVALIDARG;

  HRESULT hr = buffer->Initialize(&heap_desc.Properties, heap_desc.Flags, pDesc, OptimizedClearValue, pHeap, Offset);
  if (FAILED(hr))
    return hr;
  if (!ppResource)
    return S_FALSE;
  return buffer->QueryInterface(riid, ppResource);
}

} // namespace dxmt
