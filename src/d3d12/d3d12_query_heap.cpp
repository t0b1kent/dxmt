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

#include "com/com_pointer.hpp"
#include "d3d12_pageable.hpp"
#include "util_env.hpp"
#include <cstdio>

namespace dxmt {

class MTLD3D12QueryHeapImpl : public MTLD3D12Pageable<MTLD3D12QueryHeap> {
public:
  MTLD3D12QueryHeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12QueryHeap>(pDevice) {}

  HRESULT
  Initialize(const D3D12_QUERY_HEAP_DESC *pDesc) {
    if (!pDesc || !pDesc->Count || pDesc->NodeMask > 1)
      return E_INVALIDARG;
    const auto type = static_cast<unsigned>(pDesc->Type);
    // The bundled header stops at SO_STATISTICS; 4, 5 and 7 are the newer
    // video-decode, copy-queue timestamp and pipeline-statistics1 heap types.
    if (type > 7 || type == 6)
      return E_INVALIDARG;
    const bool statistics_storage_only =
        (pDesc->Type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS || pDesc->Type == D3D12_QUERY_HEAP_TYPE_SO_STATISTICS) &&
        env::getEnvVar("MACRUNNER_DX12_STATISTICS_STORAGE_ONLY") == "NOT_PIPELINE_STATISTICS_SUPPORT";
    if (pDesc->Type != D3D12_QUERY_HEAP_TYPE_OCCLUSION && pDesc->Type != D3D12_QUERY_HEAP_TYPE_TIMESTAMP &&
        !statistics_storage_only)
      return E_NOTIMPL;

    auto metal = device_->GetMTLDevice();
    WMTBufferInfo info{};
    const size_t result_size = statistics_storage_only ?
        (pDesc->Type == D3D12_QUERY_HEAP_TYPE_SO_STATISTICS ? sizeof(D3D12_QUERY_DATA_SO_STATISTICS) :
         sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)) : sizeof(uint64_t);
    info.length = uint64_t(pDesc->Count) * result_size;
    info.options = WMTResourceStorageModePrivate;
    results = metal.newBuffer(info);
    if (!results)
      return E_OUTOFMEMORY;

    this->type = pDesc->Type;
    count = pDesc->Count;
    owner = device_;
    if (statistics_storage_only) {
      // Diagnostic allocation only: recording any query against this heap fails.
      fprintf(stderr, "dx12_statistics_storage_only type=%u count=%u sampling_implemented=0\n", unsigned(pDesc->Type), count);
      return S_OK;
    }
    if (pDesc->Type == D3D12_QUERY_HEAP_TYPE_TIMESTAMP) {
      // Apple GPUs cap each native timestamp buffer at 32 KiB; D3D12 heaps
      // can be larger, so map their indices onto independently retained pages.
      for (uint64_t start = 0; start < pDesc->Count; start += TimestampPageSize) {
        auto samples = metal.newCounterSampleBuffer(std::min<uint64_t>(TimestampPageSize, pDesc->Count - start));
        if (!samples)
          return E_NOTIMPL;
        timestamp_pages.push_back(std::move(samples));
      }
      return S_OK;
    }

    // Accumulate between render encoders on the GPU, before any resolve or reuse.
    // One thread owns each destination: no 64-bit atomic capability is required.
    static constexpr char source[] = R"(
      #include <metal_stdlib>
      using namespace metal;
      kernel void accumulate_query(device const ulong *src [[buffer(0)]],
                                   device ulong *dst [[buffer(1)]]) {
        dst[0] += src[0];
      }
      kernel void resolve_binary(device const ulong *src [[buffer(0)]],
                                 device ulong *dst [[buffer(1)]],
                                 uint index [[thread_position_in_grid]]) {
        dst[index] = src[index] != 0 ? 1ul : 0ul;
      }
    )";
    WMT::Reference<WMT::Error> error;
    auto library = metal.newLibraryWithSource(source, error);
    if (!library) {
      ERR("QueryHeap: failed to compile visibility kernels: ", error.description().getUTF8String());
      return E_FAIL;
    }
    WMTComputePipelineInfo pipeline_info;
    WMT::InitializeComputePipelineInfo(pipeline_info);
    auto add_function = library.newFunction("accumulate_query");
    pipeline_info.compute_function = add_function;
    accumulate = metal.newComputePipelineState(pipeline_info, error);
    auto binary_function = library.newFunction("resolve_binary");
    pipeline_info.compute_function = binary_function;
    binary_resolve = metal.newComputePipelineState(pipeline_info, error);
    if (!accumulate || !binary_resolve) {
      ERR("QueryHeap: failed to create visibility pipelines: ", error.description().getUTF8String());
      return E_FAIL;
    }
    count = pDesc->Count;
    owner = device_;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12QueryHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12QueryHeap), riid)) {
      WARN("D3D12QueryHeap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }
};

HRESULT
CreateQueryHeap(MTLD3D12Device *pDevice, const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppQueryHeap) {
  if (ppQueryHeap)
    *ppQueryHeap = nullptr;
  auto heap = Com(new MTLD3D12QueryHeapImpl(pDevice));
  HRESULT hr = heap->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  if (!ppQueryHeap)
    return S_FALSE;
  return heap->QueryInterface(riid, ppQueryHeap);
}

} // namespace dxmt
