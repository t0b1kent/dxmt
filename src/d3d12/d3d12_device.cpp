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
#include "d3d12_device_child.hpp"
#include "d3d12_lifetime.hpp"
#include "d3d12_frame_trace.hpp"
#include "d3d12_diagnostic_counters.hpp"
#include "d3d12_command_failure_trace.hpp"
#include "d3d12_state_object_libraries.hpp"
#include "d3d12_state_object_associations.hpp"
#include "Metal.hpp"
#include "com/com_pointer.hpp"
#include "com/com_object.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include <bit>
#include <map>
#include <memory>
#include <cstdio>
#include <cstring>
#include "d3d10_1.h"
#include "d3d11_4.h"

namespace dxmt {

const GUID kD3D12DeviceDownlevelUUID = {0x74eaee3f, 0x2f4b, 0x476d, {0x82, 0xba, 0x2b, 0x85, 0xcb, 0x49, 0xe3, 0x10}};

HRESULT PopulateWMTTextureInfo(WMT::Device Device, WMTTextureInfo &InfoOut, const D3D12_RESOURCE_DESC &Desc);

class MTLD3D12DeviceImpl;

namespace {
bool GameBoundaryTraceEnabled() {
  static const bool enabled = [] {
    char value[4] = {};
    return GetEnvironmentVariableA("MACRUNNER_DX12_GAME_BOUNDARY_TRACE", value, sizeof(value)) == 1 && value[0] == '1';
  }();
  return enabled;
}

void TraceGameBoundary(const char *api, const void *self, UINT key, HRESULT hr, const void *data, UINT size) {
  if (!GameBoundaryTraceEnabled())
    return;
  static std::atomic<UINT> count{0};
  static std::atomic<UINT> list_count{0};
  static std::atomic<UINT> error_count{0};
  const bool failed = FAILED(hr) && hr != E_PENDING;
  // Successful list pools must not hide a later initialization failure.
  if (!failed && !std::strncmp(api, "CreateCommandList", 17) &&
      list_count.fetch_add(1, std::memory_order_relaxed) >= 8)
    return;
  const UINT sequence = count.fetch_add(1, std::memory_order_relaxed);
  if (failed ? error_count.fetch_add(1, std::memory_order_relaxed) >= 64 : sequence >= 512)
    return;
  unsigned words[8] = {};
  if (SUCCEEDED(hr) && data)
    std::memcpy(words, data, std::min<size_t>(size, sizeof(words)));
  char line[512];
  const int length = std::snprintf(line, sizeof(line),
      "dx12_game_boundary n=%u api=%s self=%p key=%u size=%u hr=%08x words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
      sequence, api, self, key, size, unsigned(hr), words[0], words[1], words[2], words[3],
      words[4], words[5], words[6], words[7]);
  DWORD written;
  if (length > 0 && size_t(length) < sizeof(line))
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
}

struct D3D12DeviceCache {
  dxmt::mutex mutex;
  std::map<uint64_t, MTLD3D12DeviceImpl *> devices;
};

std::shared_ptr<D3D12DeviceCache>
GetD3D12DeviceCache() {
  static const auto cache = std::make_shared<D3D12DeviceCache>();
  return cache;
}
} // namespace

class MTLD3D12DeviceImpl : public MTLD3D12Object<ComObject<MTLD3D12Device>> {

  Com<IMTLDXGIAdapter> adapter_;
  std::shared_ptr<D3D12DeviceCache> device_cache_;
  const uint64_t adapter_luid_;

  bool advertise_numa_ = false;

  dxmt::mutex residency_lock_;
  WMT::Reference<WMT::ResidencySet> residency_set_;
  std::shared_ptr<D3D12ResidencyState> residency_owner_ = std::make_shared<D3D12ResidencyState>();
  struct BufferInterval {
    BufferAllocation *allocation;
    uint64_t logical_length;
    std::weak_ptr<D3D12ResourceCaptureSource> capture_source;
  };
  std::map<uint64_t, BufferInterval> interval_map_;
  std::unordered_map<const void *, std::weak_ptr<D3D12ResourceCaptureSource>> capture_sources_;

  std::atomic<HRESULT> removed_reason_{S_OK};
  dxmt::mutex fence_lock_;
  std::map<D3D12RemovableFence *, std::shared_ptr<D3D12RemovableFence>> fences_;

  InternalCommandLibrary command_library;
  FormatCapabilityInspector format_inspector_;

public:
  MTLD3D12DeviceImpl(IMTLDXGIAdapter *adapter, std::shared_ptr<D3D12DeviceCache> cache, uint64_t adapter_luid) :
      adapter_(adapter), device_cache_(std::move(cache)), adapter_luid_(adapter_luid),
      command_library(adapter_->GetMTLDevice()) {
    static std::atomic<uint32_t> trace_enabled{0};
    TraceFrame(trace_enabled, "enabled", this, "MACRUNNER_DX12_FRAME_TRACE=1 type=device");
  }

  ~MTLD3D12DeviceImpl() { RemoveDevice(); diagnostic::Dump(this); }

  ULONG STDMETHODCALLTYPE
  Release() override {
    uint32_t ref_count;
    {
      // Cache acquisition and the final public release must be mutually exclusive.
      std::lock_guard<dxmt::mutex> lock(device_cache_->mutex);
      ref_count = --m_refCount;
      if (!ref_count) {
        auto it = device_cache_->devices.find(adapter_luid_);
        if (it != device_cache_->devices.end() && it->second == this)
          device_cache_->devices.erase(it);
      }
    }
    // Destruction can release COM private data and re-enter the factory.
    if (!ref_count)
      ReleasePrivate();
    return ref_count;
  }

  HRESULT
  Initialize() {
    WMT::Reference<WMT::Error> err;
    residency_set_ = adapter_->GetMTLDevice().newResidencySet(0, err);
    if (!residency_set_) {
      ERR("Failed to create MTLResidencySet: ", err.description().getUTF8String());
      return E_FAIL;
    }
    residency_owner_->set = residency_set_;
    format_inspector_.Inspect(GetMTLDevice());
    
    WMTDepthStencilInfo info{};
    info.depth_compare_function = WMTCompareFunctionAlways;
    default_depth_stencil_state = GetMTLDevice().newDepthStencilState(info);

    return S_OK;
  };

  WMT::Device
  GetMTLDevice() {
    return adapter_->GetMTLDevice();
  };

  D3D_FEATURE_LEVEL
  GetFeatureLevel() {
    return kD3D12ExperimentalFeatureLevel;
  };

  HRESULT
  GetAdapter(REFIID riid, void **ppAdapter) {
    return adapter_->QueryInterface(riid, ppAdapter);
  };

  UINT STDMETHODCALLTYPE
  GetNodeCount() {
    return 1; // FIXME
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    const HRESULT hr = QueryInterfaceImpl(riid, ppvObject);
    TraceGameBoundary("QueryInterface", this, riid.Data1, hr, ppvObject, sizeof(void *));
    return hr;
  }

  HRESULT QueryInterfaceImpl(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12Device) ||
        riid == __uuidof(ID3D12Device1) || riid == __uuidof(ID3D12Device2) || riid == __uuidof(ID3D12Device3) ||
        riid == __uuidof(ID3D12Device4) || riid == __uuidof(ID3D12Device5)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IDXGIDevice4))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1))
      return E_NOINTERFACE;

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) || riid == __uuidof(ID3D11Device2) ||
        riid == __uuidof(ID3D11Device3) || riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5))
      return E_NOINTERFACE;

    if (riid == kD3D12DeviceDownlevelUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(ID3D12Device1), riid)) {
      WARN("D3D12Device: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
    TraceGameBoundary("CreateCommandQueue.enter", this, pDesc ? pDesc->Type : -1, E_PENDING, nullptr, 0);
    const HRESULT hr = CreateCommandQueueImpl(pDesc, riid, ppCommandQueue);
    TraceGameBoundary("CreateCommandQueue.leave", this, pDesc ? pDesc->Type : -1, hr, ppCommandQueue, sizeof(void *));
    return hr;
  }

  HRESULT CreateCommandQueueImpl(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue) {
    if (ppCommandQueue)
      *ppCommandQueue = nullptr;
    if (FAILED(GetDeviceRemovedReason()))
      return GetDeviceRemovedReason();
    if (!pDesc)
      return E_INVALIDARG;
    if (pDesc->Flags)
      WARN("CreateCommandQueue: flags ignored: ", pDesc->Flags);
    return dxmt::CreateCommandQueue(this, pDesc, riid, ppCommandQueue);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE Type, REFIID riid, void **ppCommandAllocator) {
    TraceGameBoundary("CreateCommandAllocator.enter", this, Type, E_PENDING, nullptr, 0);
    const HRESULT hr = dxmt::CreateCommandAllocator(this, Type, riid, ppCommandAllocator);
    TraceGameBoundary("CreateCommandAllocator.leave", this, Type, hr, ppCommandAllocator, sizeof(void *));
    return TraceCommandFailure(CommandFailureOperation::AllocatorCreate, "allocator.create.failure", this,
                               hr, "creation_result", nullptr, Type);
  };

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    static FrameTraceResultCounters trace;
    TraceFrame(trace.enter, "pso.graphics.enter", this, "desc=%p", static_cast<const void *>(pDesc));
    const HRESULT hr = dxmt::CreateGraphicsPipelineState(this, pDesc, riid, ppPipelineState);
    return TraceFrameResult(trace, "pso.graphics.return", "pso.graphics.failure", this, hr);
  };

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    static FrameTraceResultCounters trace;
    TraceFrame(trace.enter, "pso.compute.enter", this, "desc=%p", static_cast<const void *>(pDesc));
    const HRESULT hr = dxmt::CreateComputePipelineState(this, pDesc, riid, ppPipelineState);
    return TraceFrameResult(trace, "pso.compute.return", "pso.compute.failure", this, hr);
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandList(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, ID3D12CommandAllocator *pCommandAllocator,
      ID3D12PipelineState *pInitialPipelineState, REFIID riid, void **ppCommandList
  ) {
    TraceGameBoundary("CreateCommandList.enter", this, Type, E_PENDING, nullptr, 0);
    if (!pCommandAllocator)
      return TraceCommandFailure(CommandFailureOperation::ListCreate, "list.create.failure", this,
                                 E_INVALIDARG, "null_allocator", nullptr, Type);
    auto allocator = static_cast<MTLD3D12CommandAllocator *>(pCommandAllocator);
    const HRESULT hr = allocator->CreateCommandList(NodeMask, Type, pInitialPipelineState, riid, ppCommandList);
    TraceGameBoundary("CreateCommandList.leave", this, Type, hr, ppCommandList, sizeof(void *));
    return TraceCommandFailure(CommandFailureOperation::ListCreate, "list.create.failure", this,
                               hr, "creation_result", pCommandAllocator, Type);
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(D3D12_FEATURE Feature, void *pFeatureData, UINT DataSize) {
    TraceGameBoundary("CheckFeatureSupport.enter", this, Feature, E_PENDING, nullptr, DataSize);
    const HRESULT hr = CheckFeatureSupportImpl(Feature, pFeatureData, DataSize);
    TraceGameBoundary("CheckFeatureSupport.leave", this, Feature, hr, pFeatureData, DataSize);
    return hr;
  }

  HRESULT CheckFeatureSupportImpl(D3D12_FEATURE Feature, void *pFeatureData, UINT DataSize) {
    if (!pFeatureData)
      return E_INVALIDARG;
    auto metal = GetMTLDevice();
    switch (Feature) {
    case D3D12_FEATURE_ARCHITECTURE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(pFeatureData);
      if (out->NodeIndex > 0)
        return E_INVALIDARG;
      out->CacheCoherentUMA = FALSE;
      out->TileBasedRenderer = TRUE;
      out->UMA = !advertise_numa_;
      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ARCHITECTURE1 *>(pFeatureData);
      if (out->NodeIndex > 0)
        return E_INVALIDARG;
      out->CacheCoherentUMA = FALSE;
      out->TileBasedRenderer = TRUE;
      out->UMA = !advertise_numa_;
      out->IsolatedMMU = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(pFeatureData);

      if (out->SampleCount == 0) {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = 0;
        return E_FAIL;
      }

      if (out->Format == DXGI_FORMAT_UNKNOWN) {
        out->NumQualityLevels = out->SampleCount == 0 ? 1 : 0;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format_desc;
      HRESULT hr = MTLQueryDXGIFormat(metal, out->Format, format_desc);
      if (SUCCEEDED(hr) && out->SampleCount) {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = metal.supportsTextureSampleCount(out->SampleCount) ? 1 : 0;
      } else {
        out->Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        out->NumQualityLevels = 0;
        return E_FAIL;
      }
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(pFeatureData);
      switch (out->HighestVersion) {
      default:
        return E_INVALIDARG;
      case D3D_ROOT_SIGNATURE_VERSION_1:
        out->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1;
        break;
      case D3D_ROOT_SIGNATURE_VERSION_1_1:
        out->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        break;
      }
      return S_OK;
    }
    case D3D12_FEATURE_FEATURE_LEVELS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(pFeatureData);
      if (!out->NumFeatureLevels || !out->pFeatureLevelsRequested)
        return E_INVALIDARG;
      D3D_FEATURE_LEVEL max_level = {};
      for (unsigned i = 0; i < out->NumFeatureLevels; i++) {
        const auto requested = out->pFeatureLevelsRequested[i];
        if (requested <= GetFeatureLevel())
          max_level = std::max(requested, max_level);
      }
      out->MaxSupportedFeatureLevel = max_level;
      return max_level ? S_OK : E_FAIL;
    }
    case D3D12_FEATURE_FORMAT_INFO:  {
       if (DataSize != sizeof(D3D12_FEATURE_DATA_FORMAT_INFO))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_INFO *>(pFeatureData);
      if (out->Format == DXGI_FORMAT_UNKNOWN) {
        out->PlaneCount = 1;
        return S_OK;
      }
      MTL_DXGI_FORMAT_DESC format_desc;
      HRESULT hr = MTLQueryDXGIFormat(metal, out->Format, format_desc);
      if (FAILED(hr))
        return E_FAIL;

      out->PlaneCount = format_desc.PlanarCount;
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *>(pFeatureData);
      out->MaxGPUVirtualAddressBitsPerProcess = 48;
      out->MaxGPUVirtualAddressBitsPerResource = 48;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        return E_INVALIDARG;
      reinterpret_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(pFeatureData)->HighestShaderModel = D3D_SHADER_MODEL_5_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(pFeatureData);
      out->DoublePrecisionFloatShaderOps = FALSE;
      out->OutputMergerLogicOp = FALSE;
      out->MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
      out->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
      out->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_2;
      out->PSSpecifiedStencilRefSupported = TRUE;
      out->TypedUAVLoadAdditionalFormats = TRUE;
      out->ROVsSupported = TRUE;
      out->ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;
      out->MaxGPUVirtualAddressBitsPerResource = 48;
      out->StandardSwizzle64KBSupported = TRUE;
      out->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      out->CrossAdapterRowMajorTextureSupported = FALSE;
      out->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
      out->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS16: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS16 *>(pFeatureData);
      out->GPUUploadHeapSupported = FALSE;    // TODO(d3d12): gpu upload heap
      out->DynamicDepthBiasSupported = FALSE; // TODO(d3d12): ID3D12GraphicsCommandList9::RSSetDepthBias
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
        return E_INVALIDARG;
      auto *out = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(pFeatureData);
      out->SRVOnlyTiledResourceTier3 = FALSE;
      out->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      out->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2 *>(pFeatureData);
      out->DepthBoundsTestSupported = FALSE;
      out->ProgrammableSamplePositionsTier = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3 *>(pFeatureData);
      out->CastingFullyTypedFormatSupported = TRUE;
      out->BarycentricsSupported = FALSE;
      out->CopyQueueTimestampQueriesSupported = FALSE;
      out->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
      out->WriteBufferImmediateSupportFlags = static_cast<D3D12_COMMAND_LIST_SUPPORT_FLAGS>(
          D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT | D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
          D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1 *>(pFeatureData);
      out->WaveOps = 0;
      out->WaveLaneCountMin = 0;
      out->WaveLaneCountMax = 0;
      out->TotalLaneCount = 0;
      // If CheckFeatureSupport succeeds this value will always be true.
      out->ExpandedComputeResourceStates = TRUE;
      out->Int64ShaderOps = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS12: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12 *>(pFeatureData);
      out->RelaxedFormatCastingSupported = FALSE;
      out->EnhancedBarriersSupported = FALSE;
      out->MSPrimitivesPipelineStatisticIncludesCulledPrimitives = D3D12_TRI_STATE_FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(pFeatureData);
      out->MSAA64KBAlignedTextureSupported = TRUE;
      out->Native16BitShaderOpsSupported = FALSE; // TODO(d3d12): should be true
      // TODO(d3d12): revise when d3d12 shared resource is implemented
      out->SharedResourceCompatibilityTier = D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7 *>(pFeatureData);
      out->MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
      out->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_CACHE))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(pFeatureData);
      out->SupportFlags =
          D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_INPROC_CACHE | D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_DISK_CACHE;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      if (DataSize != sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
        return E_INVALIDARG;
      auto *out = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(pFeatureData);
      out->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
      out->Support2 = D3D12_FORMAT_SUPPORT2_NONE;

      if (out->Format == DXGI_FORMAT_UNKNOWN) {
        out->Support1 = D3D12_FORMAT_SUPPORT1_BUFFER;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(metal, out->Format, format)))
        return E_INVALIDARG;

      // Reuse D3D11's DXGI mapping and device-specific Metal capability inspector,
      // not its unchecked texture-dimension and multisample assumptions.
      const auto capability = GetMTLPixelFormatCapability(format.PixelFormat);
      if (!format.PixelFormat || !any_bit_set(capability))
        return E_INVALIDARG;
      const auto has = [capability](FormatCapability bits) { return any_bit_set(capability & bits); };

      switch (out->Format) {
      case DXGI_FORMAT_R32G32B32_TYPELESS:
        return E_INVALIDARG;
      case DXGI_FORMAT_R32G32B32_FLOAT:
      case DXGI_FORMAT_R32G32B32_UINT:
      case DXGI_FORMAT_R32G32B32_SINT:
        // The shared map uses a scalar pixel format for these 12-byte attributes.
        // D3D12's typed buffer/texture paths do not reconstruct three components.
        if (!format.AttributeFormat)
          return E_INVALIDARG;
        out->Support1 = D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
        return S_OK;
      case DXGI_FORMAT_R8G8_B8G8_UNORM:
      case DXGI_FORMAT_G8R8_G8B8_UNORM:
        // Metal's packed 4:2:2 mapping alone does not establish D3D12 view support.
        return E_NOTIMPL;
      default:
        break;
      }

      const bool compressed = format.Flag & MTL_DXGI_FORMAT_BC;
      const auto planes = format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER);
      const bool depth_stencil = out->Format == DXGI_FORMAT_D16_UNORM || out->Format == DXGI_FORMAT_D32_FLOAT ||
                                 out->Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                                 out->Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
      // R32_FLOAT_X8X24 and X32_G8X24 carry TYPELESS in the shared map but are
      // typed SRV planes, unlike their fully typeless parent resource format.
      const bool plane_view = !depth_stencil &&
                              (planes == MTL_DXGI_FORMAT_DEPTH_PLANER || planes == MTL_DXGI_FORMAT_STENCIL_PLANER);
      const bool typeless = (format.Flag & MTL_DXGI_FORMAT_TYPELESS) && !plane_view;
      const bool depth_format = has(FormatCapability::DepthStencil);
      const bool shared_exponent = out->Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
      const bool castable = !shared_exponent && out->Format != DXGI_FORMAT_A8_UNORM &&
                            out->Format != DXGI_FORMAT_B5G6R5_UNORM &&
                            out->Format != DXGI_FORMAT_B5G5R5A1_UNORM && out->Format != DXGI_FORMAT_B4G4R4A4_UNORM;

      out->Support1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE | D3D12_FORMAT_SUPPORT1_MIP;
      // PopulateWMTTextureInfo rejects BC and depth/stencil planes as Texture1D.
      // Do not infer compressed or depth Texture3D support from a 2D pixel format.
      if (!compressed && !planes)
        out->Support1 |= D3D12_FORMAT_SUPPORT1_TEXTURE1D;
      if (!compressed && !depth_format)
        out->Support1 |= D3D12_FORMAT_SUPPORT1_TEXTURE3D;
      if (castable)
        out->Support1 |= D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
      if (typeless)
        return S_OK;

      if (depth_stencil) {
        if (!depth_format)
          return E_INVALIDARG;
        out->Support1 |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
        if (has(FormatCapability::MSAA))
          out->Support1 |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
        return S_OK;
      }

      out->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
      if (has(FormatCapability::Filter))
        out->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_SHADER_GATHER;
      if (!shared_exponent && has(FormatCapability::MSAA))
        out->Support1 |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;

      // Single-component depth SRVs reuse the depth texture at view creation.
      auto comparison = depth_format && planes == MTL_DXGI_FORMAT_DEPTH_PLANER;
      if (out->Format == DXGI_FORMAT_R32_FLOAT)
        comparison = any_bit_set(GetMTLPixelFormatCapability(WMTPixelFormatDepth32Float) & FormatCapability::DepthStencil);
      if (out->Format == DXGI_FORMAT_R16_UNORM)
        comparison = any_bit_set(GetMTLPixelFormatCapability(WMTPixelFormatDepth16Unorm) & FormatCapability::DepthStencil);
      if (comparison)
        out->Support1 |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON | D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON;
      if (plane_view)
        return S_OK;

      const bool buffer_read = has(FormatCapability::TextureBufferRead | FormatCapability::TextureBufferReadWrite);
      const bool buffer_write = has(FormatCapability::TextureBufferWrite | FormatCapability::TextureBufferReadWrite);
      const bool srgb = Is_sRGBVariant(format.PixelFormat);
      if (buffer_read && !srgb && !shared_exponent && out->Format != DXGI_FORMAT_A8_UNORM)
        out->Support1 |= D3D12_FORMAT_SUPPORT1_BUFFER;
      if (format.AttributeFormat)
        out->Support1 |= D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER;
      if (out->Format == DXGI_FORMAT_R16_UINT || out->Format == DXGI_FORMAT_R32_UINT)
        out->Support1 |= D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER;
      if (format.Flag & MTL_DXGI_FORMAT_BACKBUFFER)
        out->Support1 |= D3D12_FORMAT_SUPPORT1_DISPLAY;

      if (!shared_exponent && has(FormatCapability::Color)) {
        out->Support1 |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        if (has(FormatCapability::Blend))
          out->Support1 |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
        if (has(FormatCapability::MSAA)) {
          out->Support1 |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
          if (has(FormatCapability::Resolve))
            out->Support1 |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
        }
      }

      // D3D11's typed UAV load/store tests use these texture-buffer capabilities.
      // Also require texture writes; sRGB and custom-swizzled views are not UAVs.
      if (!srgb && !shared_exponent && !(format.PixelFormat & WMTPixelFormatCustomSwizzle) &&
          has(FormatCapability::Write) && buffer_write) {
        out->Support1 |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
        out->Support2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
        if (buffer_read)
          out->Support2 |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
        if ((out->Format == DXGI_FORMAT_R32_UINT || out->Format == DXGI_FORMAT_R32_SINT) && has(FormatCapability::Atomic))
          out->Support2 |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
                           D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
                           D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
                           D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
      }
      // No stream output (SOSetTargets is unimplemented), tiled resources, logic
      // ops, video, or D3D11-only SHAREABLE bits are implied by this format table.
      return S_OK;
    }
    default:
      break;
    }
    ERR("CheckFeatureSupport: unhandled feature ", Feature);
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *pDesc, REFIID riid, void **ppDescriptorHeap) {
    const HRESULT hr = dxmt::CreateDescriptorHeap(this, pDesc, riid, ppDescriptorHeap);
    TraceGameBoundary("CreateDescriptorHeap", this, pDesc ? pDesc->Type : -1, hr, ppDescriptorHeap, sizeof(void *));
    return hr;
  };

  UINT STDMETHODCALLTYPE
  GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) {
    TraceGameBoundary("GetDescriptorHandleIncrementSize", this, DescriptorHeapType, S_OK, nullptr, 0);
    switch (DescriptorHeapType) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      return 32;
    default:
      break;
    }
    return 0;
  };

  HRESULT STDMETHODCALLTYPE
  CreateRootSignature(
      UINT NodeMask, const void *pBytecode, SIZE_T BytecodeLength, REFIID riid, void **ppRootSignature
  ) {
    const HRESULT hr = dxmt::CreateRootSignature(this, NodeMask, pBytecode, BytecodeLength, riid, ppRootSignature);
    TraceGameBoundary("CreateRootSignature", this, NodeMask, hr, ppRootSignature, sizeof(void *));
    return hr;
  };

  void STDMETHODCALLTYPE
  CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
    if (pDesc)
      Heap->AddConstantBufferView(Index, pDesc->BufferLocation, pDesc->SizeInBytes);
    else
      Heap->AddConstantBufferView(Index, 0, 0);
  };

  void STDMETHODCALLTYPE
  CreateShaderResourceView(
      ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
      Heap->AddShaderResourceView(Index, pDesc);
      return;
    }
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateShaderResourceView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateUnorderedAccessView(
      ID3D12Resource *pResource, ID3D12Resource *pCounter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc,
      D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetShaderVisibleDescriptorHeap(this, Descriptor);
      Heap->AddUnorderedAccessView(Index, pDesc);
      return;
    }
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateUnorderedAccessView(pCounter, pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateRenderTargetView(
      ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetRenderTargetHeap(this, Descriptor);
      Heap->AddRenderTarget(Index, nullptr);
      return;
    }
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateRenderTargetView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateDepthStencilView(
      ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor
  ) {
    if (!pResource) {
      auto [Heap, Index] = GetRenderTargetHeap(this, Descriptor);
      Heap->AddRenderTarget(Index, nullptr);
      return;
    }
    auto d3d12res = static_cast<MTLD3D12Resource *>(pResource);
    d3d12res->CreateDepthStencilView(pDesc, Descriptor);
  };

  void STDMETHODCALLTYPE
  CreateSampler(const D3D12_SAMPLER_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE Descriptor) {
    auto [Heap, Index] = GetSamplerDescriptorHeap(this, Descriptor);
    Heap->AddSampler(Index, pDesc);
  };

  void STDMETHODCALLTYPE
  CopyDescriptors(
      UINT DstDescriptorRangeCount, const D3D12_CPU_DESCRIPTOR_HANDLE *DstDescriptorRangeOffsets,
      const UINT *DstDescriptorRangeSizes, UINT SrcDescriptorRangeCount,
      const D3D12_CPU_DESCRIPTOR_HANDLE *SrcDescriptorRangeOffsets, const UINT *SrcDescriptorRangeSizes,
      D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType
  ) {
    unsigned int dst_range_idx, dst_idx, src_range_idx, src_idx;
    unsigned int dst_range_size, src_range_size, copy_count;

    dst_range_idx = dst_idx = 0;
    src_range_idx = src_idx = 0;
    while (dst_range_idx < DstDescriptorRangeCount && src_range_idx < SrcDescriptorRangeCount) {
      dst_range_size = DstDescriptorRangeSizes ? DstDescriptorRangeSizes[dst_range_idx] : 1;
      src_range_size = SrcDescriptorRangeSizes ? SrcDescriptorRangeSizes[src_range_idx] : 1;

      copy_count = std::min(dst_range_size - dst_idx, src_range_size - src_idx);

      switch (DescriptorHeapType) {
      case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: {
        auto [DstRangeHeap, DstRangeIndex] =
            GetShaderVisibleDescriptorHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] =
            GetShaderVisibleDescriptorHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }

      case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: {
        auto [DstRangeHeap, DstRangeIndex] = GetSamplerDescriptorHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] = GetSamplerDescriptorHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }
      case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
      case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: {
        auto [DstRangeHeap, DstRangeIndex] = GetRenderTargetHeap(this, DstDescriptorRangeOffsets[dst_range_idx]);
        auto [SrcRangeHeap, SrcRangeIndex] = GetRenderTargetHeap(this, SrcDescriptorRangeOffsets[src_range_idx]);
        SrcRangeHeap->CopyDescriptors(SrcRangeIndex + src_idx, DstRangeHeap, DstRangeIndex + dst_idx, copy_count);
        break;
      }
      default:
        return;
      }

      dst_idx += copy_count;
      src_idx += copy_count;

      if (dst_idx >= dst_range_size) {
        ++dst_range_idx;
        dst_idx = 0;
      }
      if (src_idx >= src_range_size) {
        ++src_range_idx;
        src_idx = 0;
      }
    }
  };

  void STDMETHODCALLTYPE
  CopyDescriptorsSimple(
      UINT DescriptorCount, const D3D12_CPU_DESCRIPTOR_HANDLE DstDescriptorRangeOffset,
      const D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeOffset, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType
  ) {
    CopyDescriptors(
        1, &DstDescriptorRangeOffset, &DescriptorCount, 1, &SrcDescriptorRangeOffset, &DescriptorCount,
        DescriptorHeapType
    );
  };

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT ResourceDestCount, const D3D12_RESOURCE_DESC *pDescs
  ) {
    return GetResourceAllocationInfo1(__ret, VisibleMask, ResourceDestCount, pDescs, nullptr);
  };

  D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE
  GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *__ret, UINT NodeMask, D3D12_HEAP_TYPE HeapType) {
    __ret->Type = D3D12_HEAP_TYPE_CUSTOM;
    __ret->CreationNodeMask = 1;
    __ret->VisibleNodeMask = 1;
    switch (HeapType) {
    case D3D12_HEAP_TYPE_DEFAULT:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
      __ret->MemoryPoolPreference = advertise_numa_ ? D3D12_MEMORY_POOL_L1 : D3D12_MEMORY_POOL_L0;
      break;
    case D3D12_HEAP_TYPE_UPLOAD:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
      __ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    case D3D12_HEAP_TYPE_READBACK:
      __ret->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
      __ret->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
      break;
    default:
      E_INVALIDARG;
    }

    return __ret;
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    const HRESULT hr = CreateCommittedResourceImpl(pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
    TraceGameBoundary("CreateCommittedResource", this, pDesc ? pDesc->Dimension : 0, hr, ppResource, sizeof(void *));
    return hr;
  }

  HRESULT CreateCommittedResourceImpl(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(pHeapProps, HeapFlags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(pDesc, pHeapProps);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceStates(InitialState, pHeapProps);
    if (FAILED(hr))
      return hr;
    switch (pDesc->Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
      return CreateCommittedTexture(
          this, pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource
      );
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return CreateCommittedBuffer(
          this, pHeapProps, HeapFlags, pDesc, InitialState, OptimizedClearValue, riid, ppResource
      );
    default:
      break;
    }
    return E_INVALIDARG;
  };

  HRESULT STDMETHODCALLTYPE
  CreateHeap(const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(&pDesc->Properties, pDesc->Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    return dxmt::CreateHeap(this, pDesc, riid, ppHeap);
  };

  HRESULT STDMETHODCALLTYPE
  CreatePlacedResource(
      ID3D12Heap *pHeap, UINT64 Offset, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **ppResource
  ) {
    InitReturnPtr(ppResource);
    if (!pHeap)
      return E_INVALIDARG;
    auto d3d12heap = static_cast<MTLD3D12Heap *>(pHeap);
    auto heap_desc = d3d12heap->GetDesc();
    HRESULT hr = S_OK;
    hr = ValidateHeapProperties(&heap_desc.Properties, heap_desc.Flags, advertise_numa_);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceDescs(pDesc, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
    hr = ValidateResourceStates(InitialState, &heap_desc.Properties);
    if (FAILED(hr))
      return hr;
    switch (pDesc->Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
      return CreatePlacedTexture(this, d3d12heap, Offset, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
    case D3D12_RESOURCE_DIMENSION_BUFFER:
      return CreatePlacedBuffer(this, d3d12heap, Offset, pDesc, InitialState, OptimizedClearValue, riid, ppResource);
    default:
      break;
    }
    return E_INVALIDARG;
  };

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource(
      const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, REFIID riid, void **resource
  ) {
    return CreateReservedBuffer(this, pDesc, InitialState, OptimizedClearValue, riid, resource);
  };

  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(
      ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle
  ) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandle(HANDLE handle, REFIID riid, void **object) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  OpenSharedHandleByName(const WCHAR *name, DWORD access, HANDLE *handle) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  MakeResident(UINT ObjectCount, ID3D12Pageable *const *objects) {
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  Evict(UINT ObjectCount, ID3D12Pageable *const *objects) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) {
    if (ppFence)
      *ppFence = nullptr;
    if (FAILED(GetDeviceRemovedReason()))
      return GetDeviceRemovedReason();
    const HRESULT hr = dxmt::CreateFence(this, InitialValue, Flags, riid, ppFence);
    TraceGameBoundary("CreateFence", this, Flags, hr, ppFence, sizeof(void *));
    return hr;
  };

  HRESULT STDMETHODCALLTYPE
  GetDeviceRemovedReason() {
    return removed_reason_.load(std::memory_order_acquire);
  };

  HRESULT
  RegisterFence(std::shared_ptr<D3D12RemovableFence> fence) override {
    std::lock_guard<dxmt::mutex> lock(fence_lock_);
    const HRESULT hr = GetDeviceRemovedReason();
    if (FAILED(hr))
      return hr;
    // Reclaim native states after both their owners and asynchronous events finish.
    for (auto it = fences_.begin(); it != fences_.end();) {
      if (it->second.use_count() == 1 && !it->second->HasPendingEvents())
        it = fences_.erase(it);
      else
        ++it;
    }
    fences_.emplace(fence.get(), std::move(fence));
    return S_OK;
  }

  void
  UnregisterFence(D3D12RemovableFence *fence) override {
    std::lock_guard<dxmt::mutex> lock(fence_lock_);
    auto it = fences_.find(fence);
    // The map and dying COM wrapper own two references. Queued native work or
    // unsatisfied CPU events must remain visible to device removal.
    if (it != fences_.end() && it->second.use_count() == 2 && !fence->HasPendingEvents())
      fences_.erase(it);
  }

  void STDMETHODCALLTYPE
  RemoveDevice() override {
    decltype(fences_) pending;
    {
      std::lock_guard<dxmt::mutex> lock(fence_lock_);
      if (FAILED(GetDeviceRemovedReason()))
        return;
      removed_reason_.store(DXGI_ERROR_DEVICE_REMOVED, std::memory_order_release);
      pending.swap(fences_);
    }
    // Shared native state survives concurrent COM fence destruction; no callbacks under the device lock.
    for (auto &[key, fence] : pending)
      fence->Remove();
  }

  HRESULT STDMETHODCALLTYPE
  CreateLifetimeTracker(ID3D12LifetimeOwner *owner, REFIID riid, void **tracker) override {
    if (tracker)
      *tracker = nullptr;
    if (FAILED(GetDeviceRemovedReason()))
      return GetDeviceRemovedReason();
    return dxmt::CreateLifetimeTracker(this, owner, riid, tracker);
  }

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommands(UINT *count, D3D12_META_COMMAND_DESC *descs) override {
    if (!count)
      return E_INVALIDARG;
    *count = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  EnumerateMetaCommandParameters(REFGUID id, D3D12_META_COMMAND_PARAMETER_STAGE stage,
      UINT *size, UINT *count, D3D12_META_COMMAND_PARAMETER_DESC *descs) override {
    return E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE
  CreateMetaCommand(REFGUID id, UINT mask, const void *data, SIZE_T size, REFIID riid, void **command) override {
    if (!command)
      return E_POINTER;
    *command = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  CreateStateObject(const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (!desc || (desc->NumSubobjects && !desc->pSubobjects))
      return E_INVALIDARG;
    std::vector<StateObjectLibrary> libraries;
    const HRESULT preparation = PrepareStateObjectLibraries(*desc, libraries);
    if (FAILED(preparation))
      return preparation;
    std::vector<Com<MTLD3D12RootSignature, false>> retained_roots;
    std::map<std::string, uint32_t> canonical_roots;
    state_plan::Plan plan;
    const HRESULT associations = PrepareStateObjectAssociations(*desc, libraries,
        [&](ID3D12RootSignature *root, bool local, uint32_t &key) -> HRESULT {
          if (!root) return DXGI_ERROR_UNSUPPORTED;
          Com<MTLD3D12RootSignature> implementation;
          HRESULT hr = root->QueryInterface(kD3D12RootSignatureImplementationUUID,
                                            reinterpret_cast<void **>(&implementation));
          if (FAILED(hr) || !implementation) return DXGI_ERROR_UNSUPPORTED;
          Com<ID3D12Device> owner;
          hr = implementation->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&owner));
          if (FAILED(hr)) return hr;
          if (owner.ptr() != static_cast<ID3D12Device *>(this)) return E_INVALIDARG;
          auto *impl = implementation.ptr();
          if (impl->IsLocal != local) return E_INVALIDARG;
          const void *bytes = nullptr;
          const UINT size = impl->GetBlob(&bytes);
          if (!size || !bytes) return DXGI_ERROR_UNSUPPORTED;
          std::string identity(1, local ? '\1' : '\0');
          identity.append(static_cast<const char *>(bytes), size);
          auto [it, inserted] = canonical_roots.emplace(std::move(identity), uint32_t(retained_roots.size()));
          if (inserted) retained_roots.emplace_back(impl);
          key = it->second;
          return S_OK;
        }, plan);
    if (FAILED(associations))
      return associations;
    for (const auto &root : retained_roots) {
      if (root->RayBindingStatus == ray_binding::Status::Invalid) return E_INVALIDARG;
      if (root->RayBindingStatus != ray_binding::Status::Ready) return DXGI_ERROR_UNSUPPORTED;
    }
    std::vector<std::vector<ray_binding::Binding>> resource_bindings;
    try {
      const auto resources = ray_binding::Resolve(plan, libraries,
          [&](uint32_t key) -> const ray_binding::RootLayout * {
            return key < retained_roots.size() ? &retained_roots[key]->RayBindings : nullptr;
          }, resource_bindings);
      if (resources == ray_binding::Status::Invalid) return E_INVALIDARG;
      if (resources != ray_binding::Status::Ready) return DXGI_ERROR_UNSUPPORTED;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    // Metadata preparation is not executable DXR support. Do not publish a
    // state object until full binding validation, resource lifetimes and dispatch exist.
    return DXGI_ERROR_UNSUPPORTED;
  }

  void STDMETHODCALLTYPE
  GetRaytracingAccelerationStructurePrebuildInfo(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) override {
    if (info)
      *info = {};
    WARN("Raytracing is not supported; query OPTIONS5 before requesting a prebuild.");
  }

  D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE
  CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE type,
      const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier) override {
    return D3D12_DRIVER_MATCHING_IDENTIFIER_UNSUPPORTED_TYPE;
  }

  void STDMETHODCALLTYPE GetCopyableFootprints(
      const D3D12_RESOURCE_DESC *pDesc, UINT FirstSubresource, UINT SubresourceCount, UINT64 BaseOffset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes
  ) {
    UINT64 TotalBytes = 0;
    UINT64 Offset = 0;
    UINT BlockWidth = 1;
    do {
      if (!pDesc)
        break;

      UINT PlaneCount = 1;
      UINT PerPlaneSubresources = DecomposeSubresource(*pDesc, 0);
      DXGI_FORMAT PlaneFormats[2] = {};
      UINT PlaneBytesPerTexel[2] = {};

      MTL_DXGI_FORMAT_DESC FormatDesc;

      if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (pDesc->Format != DXGI_FORMAT_UNKNOWN)
          break;
        PlaneFormats[0] = DXGI_FORMAT_UNKNOWN;
        PlaneBytesPerTexel[0] = 1;
      } else {
        if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), pDesc->Format, FormatDesc)))
          break;

        if (pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && pDesc->Height != 1)
          break;

        if (FormatDesc.Flag & MTL_DXGI_FORMAT_BC)
          BlockWidth = 4;

        if (FormatDesc.PlanarCount > 1) {
          assert(FormatDesc.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER));
          PlaneCount = FormatDesc.PlanarCount;
          PlaneFormats[0] = DXGI_FORMAT_R32_TYPELESS;
          PlaneBytesPerTexel[0] = 4;
          PlaneFormats[1] = DXGI_FORMAT_R8_TYPELESS;
          PlaneBytesPerTexel[1] = 1;
        } else {
          PlaneFormats[0] = pDesc->Format;
          PlaneBytesPerTexel[0] = FormatDesc.BytesPerTexel;
          if (PlaneBytesPerTexel[0] == 0)
            IMPLEMENT_ME
        }
      }

      if (pDesc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        PerPlaneSubresources *= pDesc->DepthOrArraySize;

      if (FirstSubresource >= PerPlaneSubresources * PlaneCount ||
          SubresourceCount > PerPlaneSubresources * PlaneCount - FirstSubresource) {
        WARN("GetCopyableFootprints: subresource is out of range");
        break;
      }

      for (unsigned i = 0; i < SubresourceCount; i++) {
        auto Subresource = FirstSubresource + i;
        auto Plane = 0u, MipLevel = 0u;
        DecomposeSubresource(*pDesc, Subresource, &MipLevel, NULL, &Plane);
        auto Extent = GetResourceExtent(*pDesc, MipLevel);
        auto Width = align(Extent.right, BlockWidth);
        auto Height = align(Extent.bottom, BlockWidth);
        auto RowCount = Height / BlockWidth;
        auto Depth = Extent.back;
        auto RowSize = (Width / BlockWidth) * PlaneBytesPerTexel[Plane];
        auto RowPitch = align(RowSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * PlaneCount);
        if (pLayouts) {
          pLayouts[i].Offset = BaseOffset + Offset;
          pLayouts[i].Footprint.Format = PlaneFormats[Plane];
          pLayouts[i].Footprint.Width = Width;
          pLayouts[i].Footprint.Height = Height;
          pLayouts[i].Footprint.Depth = Depth;
          pLayouts[i].Footprint.RowPitch = RowPitch;
        }
        if (pNumRows)
          pNumRows[i] = RowCount;
        if (pRowSizeInBytes)
          pRowSizeInBytes[i] = RowSize;

        auto SubresourceSize = RowPitch * (RowCount - 1) + RowSize;
        SubresourceSize =
            align(SubresourceSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * PlaneCount) * (Depth - 1) + SubresourceSize;

        TotalBytes = Offset + SubresourceSize;
        Offset = align(TotalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      }
      if (pTotalBytes)
        *pTotalBytes = TotalBytes;
      return;
    } while (0);
    for (unsigned i = 0; i < SubresourceCount; i++) {
      if (pLayouts) {
        pLayouts[i].Offset = ~0ull;
        pLayouts[i].Footprint.Format = ~(DXGI_FORMAT)0u;
        pLayouts[i].Footprint.Width = ~0u;
        pLayouts[i].Footprint.Height = ~0u;
        pLayouts[i].Footprint.Depth = ~0u;
        pLayouts[i].Footprint.RowPitch = ~0u;
      }
      if (pNumRows)
        pNumRows[i] = ~0u;
      if (pRowSizeInBytes)
        pRowSizeInBytes[i] = ~0ull;
    }
    if (pTotalBytes)
      *pTotalBytes = UINT64_MAX;
  };

  HRESULT STDMETHODCALLTYPE
  CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
    TraceGameBoundary("CreateQueryHeap.desc", this, pDesc ? pDesc->Type : -1, S_OK, pDesc, pDesc ? sizeof(*pDesc) : 0);
    const HRESULT hr = dxmt::CreateQueryHeap(this, pDesc, riid, ppHeap);
    TraceGameBoundary("CreateQueryHeap", this, pDesc ? pDesc->Type : -1, hr, ppHeap, sizeof(void *));
    return hr;
  };

  HRESULT STDMETHODCALLTYPE
  SetStablePowerState(WINBOOL Enable) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreateCommandSignature(
      const D3D12_COMMAND_SIGNATURE_DESC *pDesc, ID3D12RootSignature *pRootSignature, REFIID riid,
      void **ppCommandSignature
  ) {
    const HRESULT hr = dxmt::CreateCommandSignature(this, pDesc, pRootSignature, riid, ppCommandSignature);
    TraceGameBoundary("CreateCommandSignature", this, pDesc ? pDesc->NumArgumentDescs : 0, hr, ppCommandSignature, sizeof(void *));
    return hr;
  };

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D12Resource *pResource, UINT *TotalTileCount, D3D12_PACKED_MIP_INFO *PackedMipInfo,
      D3D12_TILE_SHAPE *StandardTileShape, UINT *SubresourceTilingCount, UINT FirstSubresourceTiling,
      D3D12_SUBRESOURCE_TILING *SubresourceTilings
  ) {
    if (pResource) {
      auto resource = static_cast<MTLD3D12Resource *>(pResource);
      if (resource->buffer) {
        resource->GetResourceTiling(TotalTileCount, PackedMipInfo, StandardTileShape, SubresourceTilingCount,
                                    FirstSubresourceTiling, SubresourceTilings);
        return;
      }
    }
    if (TotalTileCount) *TotalTileCount = 0;
    if (PackedMipInfo) *PackedMipInfo = {};
    if (StandardTileShape) *StandardTileShape = {};
    if (SubresourceTilingCount) *SubresourceTilingCount = 0;
    ERR("GetResourceTiling: only reserved buffer layouts are implemented");
  };

  LUID *STDMETHODCALLTYPE
  GetAdapterLuid(LUID *ret) {
    *ret = std::bit_cast<LUID>(__builtin_bswap64(adapter_->GetMTLDevice().registryID()));
    return ret;
  }

  HRESULT STDMETHODCALLTYPE
  CreatePipelineLibrary(const void *blob, SIZE_T blob_size, REFIID iid, void **lib) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *pFences, const UINT64 *pValues, UINT FenceCount, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags,
      HANDLE hEvent
  ) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  SetResidencyPriority(UINT ObjectCount, ID3D12Pageable *const *pObjects, const D3D12_RESIDENCY_PRIORITY *pPriorities) {
    return E_NOTIMPL;
  };

  HRESULT STDMETHODCALLTYPE
  CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc, REFIID riid, void **ppPipelineState) {
    static FrameTraceResultCounters trace;
    TraceFrame(trace.enter, "pso.stream.enter", this, "desc=%p caller=%p", static_cast<const void *>(pDesc),
               __builtin_return_address(0));
    const auto trace_result = [&](HRESULT hr) {
      return TraceFrameResult(trace, "pso.stream.return", "pso.stream.failure", this, hr);
    };
    const char *stream_start = reinterpret_cast<const char *>(pDesc->pPipelineStateSubobjectStream);
    const char *stream_end = stream_start + pDesc->SizeInBytes;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc_cs{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc_graphics{};
    {
      desc_graphics.DepthStencilState.DepthEnable = TRUE;
      desc_graphics.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      desc_graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      desc_graphics.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      desc_graphics.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
      desc_graphics.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
      desc_graphics.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
      desc_graphics.DepthStencilState.BackFace = desc_graphics.DepthStencilState.FrontFace;
      desc_graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      desc_graphics.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      desc_graphics.RasterizerState.DepthClipEnable = TRUE;
      desc_graphics.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
      desc_graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc_graphics.SampleDesc.Count = 1;
      desc_graphics.SampleDesc.Quality = 0;
      desc_graphics.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    }

    uint32_t defined_type = 0;

    while (stream_start < stream_end) {
      if (stream_start + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) > stream_end) {
        ERR("CreatePipelineState: invalid stream");
        return trace_result(E_INVALIDARG);
      }
      auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(stream_start);

      if (defined_type & (1 << type)) {
        ERR("CreatePipelineState: duplicated subobejct type ", type);
        return trace_result(E_INVALIDARG);
      }
      defined_type |= (1 << type);

#define GET_STREAM_DATA(data_type)                                                                                     \
  using subobject_t = struct {                                                                                         \
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;                                                                          \
    data_type data;                                                                                                    \
  };                                                                                                                   \
  auto subobject = reinterpret_cast<subobject_t const *>(stream_start);                                                \
  if (stream_start + sizeof(*subobject) > stream_end) {                                                                \
    ERR("CreatePipelineState: invalid stream");                                                                        \
    return trace_result(E_INVALIDARG);                                                                                 \
  }                                                                                                                    \
  stream_start += align(sizeof(*subobject), sizeof(void *));

      switch (type) {
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
        GET_STREAM_DATA(ID3D12RootSignature *);
        desc_cs.pRootSignature = subobject->data;
        desc_graphics.pRootSignature = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.VS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.PS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.DS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.HS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_graphics.GS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        desc_cs.CS = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
        GET_STREAM_DATA(D3D12_STREAM_OUTPUT_DESC);
        desc_graphics.StreamOutput = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: {
        GET_STREAM_DATA(D3D12_BLEND_DESC);
        desc_graphics.BlendState = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: {
        GET_STREAM_DATA(UINT);
        desc_graphics.SampleMask = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: {
        GET_STREAM_DATA(D3D12_RASTERIZER_DESC);
        desc_graphics.RasterizerState = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
        GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC);
        desc_graphics.DepthStencilState = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
        GET_STREAM_DATA(D3D12_INPUT_LAYOUT_DESC);
        desc_graphics.InputLayout = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: {
        GET_STREAM_DATA(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
        desc_graphics.IBStripCutValue = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: {
        GET_STREAM_DATA(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
        desc_graphics.PrimitiveTopologyType = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
        GET_STREAM_DATA(D3D12_RT_FORMAT_ARRAY);
        memcpy(desc_graphics.RTVFormats, subobject->data.RTFormats, sizeof(desc_graphics.RTVFormats));
        desc_graphics.NumRenderTargets = subobject->data.NumRenderTargets;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: {
        GET_STREAM_DATA(DXGI_FORMAT);
        desc_graphics.DSVFormat = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: {
        GET_STREAM_DATA(DXGI_SAMPLE_DESC);
        desc_graphics.SampleDesc = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: {
        GET_STREAM_DATA(UINT);
        desc_graphics.NodeMask = subobject->data;
        desc_cs.NodeMask = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
        GET_STREAM_DATA(D3D12_CACHED_PIPELINE_STATE);
        desc_graphics.CachedPSO = subobject->data;
        desc_cs.CachedPSO = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: {
        GET_STREAM_DATA(D3D12_PIPELINE_STATE_FLAGS);
        desc_graphics.Flags = subobject->data;
        desc_cs.Flags = subobject->data;
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
        GET_STREAM_DATA(D3D12_DEPTH_STENCIL_DESC1);
        desc_graphics.DepthStencilState.StencilEnable = subobject->data.StencilEnable;
        desc_graphics.DepthStencilState.DepthEnable = subobject->data.DepthEnable;
        desc_graphics.DepthStencilState.DepthFunc = subobject->data.DepthFunc;
        desc_graphics.DepthStencilState.DepthWriteMask = subobject->data.DepthWriteMask;
        desc_graphics.DepthStencilState.StencilWriteMask = subobject->data.StencilWriteMask;
        desc_graphics.DepthStencilState.StencilReadMask = subobject->data.StencilReadMask;
        desc_graphics.DepthStencilState.BackFace = subobject->data.BackFace;
        desc_graphics.DepthStencilState.FrontFace = subobject->data.FrontFace;
        if (subobject->data.DepthBoundsTestEnable) {
          WARN("CreatePipelineState: ignore DepthBoundsTestEnable");
        }
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
        GET_STREAM_DATA(D3D12_VIEW_INSTANCING_DESC);
        if (subobject->data.Flags) {
          WARN("CreatePipelineState: ignore ViewInstancing");
        }
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        if (subobject->data.pShaderBytecode) {
          ERR("CreatePipelineState: unsupported AS");
          return trace_result(E_NOTIMPL);
        }
        break;
      }
      case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
        GET_STREAM_DATA(D3D12_SHADER_BYTECODE);
        if (subobject->data.pShaderBytecode) {
          ERR("CreatePipelineState: unsupported MS");
          return trace_result(E_NOTIMPL);
        }
        break;
      }
      default:
        ERR("CreatePipelineState: unhandled subobject type ", type);
        return trace_result(E_INVALIDARG);
      }
    }

    if (desc_cs.CS.pShaderBytecode) {
      uint32_t incompatible_type =
          (1 << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS | 1 << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS |
           1 << D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS);
      if (defined_type & incompatible_type) {
        ERR("CreatePipelineState: invalid compute pipeline state stream");
        return trace_result(E_INVALIDARG);
      }
      return trace_result(CreateComputePipelineState(&desc_cs, riid, ppPipelineState));
    }

    return trace_result(CreateGraphicsPipelineState(&desc_graphics, riid, ppPipelineState));
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromAddress(const void *pAddress, REFIID riid, void **ppHeap) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid, void **ppHeap) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  EnqueueMakeResident(
      D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable *const *ppObjects, ID3D12Fence *pFence,
      UINT64 FenceValue
  ) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommandList1(
      UINT NodeMask, D3D12_COMMAND_LIST_TYPE Type, D3D12_COMMAND_LIST_FLAGS Flags, REFIID riid, void **ppCommandList
  ) {
    return TraceCommandFailure(CommandFailureOperation::ListCreate1, "list.create1.failure", this,
                               E_NOTIMPL, "unsupported", nullptr, Type);
  }

  HRESULT STDMETHODCALLTYPE
  CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC *pDesc, REFIID riid, void **ppSession) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource1(
      const D3D12_HEAP_PROPERTIES *pHeapProps, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
      D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *OptimizedClearValue,
      ID3D12ProtectedResourceSession *pSession, REFIID riid, void **ppResource
  ) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateHeap1(const D3D12_HEAP_DESC *pDesc, ID3D12ProtectedResourceSession *pSession, REFIID riid, void **ppHeap) {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateReservedResource1(
      const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState,
      const D3D12_CLEAR_VALUE *OptimizedClearValue, ID3D12ProtectedResourceSession *pSession, REFIID riid,
      void **ppResource
  ) {
    return E_NOTIMPL;
  }

  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo1(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT VisibleMask, UINT ResourceDestCount,
      const D3D12_RESOURCE_DESC *pDescs, D3D12_RESOURCE_ALLOCATION_INFO1 *pAllocationInfos
  ) {
    D3D12_RESOURCE_ALLOCATION_INFO1 resource_info;
    bool has_msaa_resource = false;

    __ret->SizeInBytes = 0;
    __ret->Alignment = 1;

    for (unsigned i = 0; i < ResourceDestCount; i++) {
      const D3D12_RESOURCE_DESC *desc = &pDescs[i];
      has_msaa_resource |= desc->SampleDesc.Count > 1;

      if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (desc->Alignment && desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) {
          DEBUG("GetResourceAllocationInfo: invalid alignment ", desc->Alignment, " for buffer resource.\n");
          goto invalid;
        }
        resource_info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        auto size_and_align = GetMTLDevice().heapBufferSizeAndAlign(desc->Width, {});
        resource_info.SizeInBytes = size_and_align.size;
      } else {
        WMTTextureInfo texture_info;
        if (FAILED(PopulateWMTTextureInfo(GetMTLDevice(), texture_info, *desc))) {
          DEBUG("GetResourceAllocationInfo: invalid texture descriptor\n");
          goto invalid;
        }
        auto size_and_align = GetMTLDevice().heapTextureSizeAndAlign(texture_info);
        resource_info.SizeInBytes = size_and_align.size;
        resource_info.Alignment = size_and_align.align;
        auto requested_alignment = desc->Alignment              ? desc->Alignment
                                   : desc->SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                                : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        resource_info.Alignment = std::max(resource_info.Alignment, requested_alignment);
      }

      resource_info.SizeInBytes = align(resource_info.SizeInBytes, resource_info.Alignment);
      resource_info.Offset = align(__ret->SizeInBytes, resource_info.Alignment);

      if (pAllocationInfos)
        pAllocationInfos[i] = resource_info;

      __ret->SizeInBytes = resource_info.Offset + resource_info.SizeInBytes;
      __ret->Alignment = std::max(__ret->Alignment, resource_info.Alignment);
    }

    __ret->SizeInBytes = align(__ret->SizeInBytes, __ret->Alignment);
    return __ret;

  invalid:

    __ret->SizeInBytes = ~(uint64_t)0;
    __ret->Alignment = has_msaa_resource ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                         : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    return __ret;
  }

  WMT::ResidencySet
  GetGlobalResidencySet() {
    return residency_set_;
  };

  std::shared_ptr<D3D12ResidencyState> GetResidencyOwner() override {
    return residency_owner_;
  }

  HRESULT
  RegisterResidency(WMT::Allocation allocation) {
    residency_owner_->Retain(allocation);
    return S_OK;
  }

  HRESULT
  UnregisterResidency(WMT::Allocation allocation) {
    residency_owner_->Release(allocation);
    return S_OK;
  }

  HRESULT
  RegisterResidencyAndVA(BufferAllocation *allocation, uint64_t logical_length) {
    diagnostic::ResidencyLock lock(residency_lock_, diagnostic::LockPath::RegisterVA);
    interval_map_.emplace(allocation->gpuAddress(), BufferInterval{allocation, logical_length, {}});
    if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap))
      return S_OK;
    auto buffer = allocation->buffer();
    residency_owner_->Retain(buffer);
    return S_OK;
  }

  HRESULT
  UnregisterResidencyAndVA(BufferAllocation *allocation) {
    diagnostic::ResidencyLock lock(residency_lock_, diagnostic::LockPath::UnregisterVA);
    interval_map_.erase(allocation->gpuAddress());
    if (allocation->flags().test(BufferAllocationFlag::AllocatedOnHeap))
      return S_OK;
    auto buffer = allocation->buffer();
    residency_owner_->Release(buffer);
    return S_OK;
  }

  void RegisterCaptureSource(const std::shared_ptr<D3D12ResourceCaptureSource> &source) override {
    diagnostic::ResidencyLock lock(residency_lock_, diagnostic::LockPath::RegisterSource);
    capture_sources_[source->Identity()] = source;
    if (source->native->buffer_allocation) {
      auto entry = interval_map_.find(source->native->buffer_allocation->gpuAddress());
      if (entry != interval_map_.end() && entry->second.allocation == source->native->buffer_allocation.ptr())
        entry->second.capture_source = source;
    }
  }

  void UnregisterCaptureSource(const D3D12ResourceCaptureSource &source) override {
    diagnostic::ResidencyLock lock(residency_lock_, diagnostic::LockPath::UnregisterSource);
    auto entry = capture_sources_.find(source.Identity());
    if (entry != capture_sources_.end() && entry->second.lock().get() == &source) capture_sources_.erase(entry);
    if (source.native->buffer_allocation) {
      auto buffer = interval_map_.find(source.native->buffer_allocation->gpuAddress());
      if (buffer != interval_map_.end() && buffer->second.capture_source.lock().get() == &source)
        buffer->second.capture_source.reset();
    }
  }

  std::shared_ptr<D3D12ResourceCaptureSource> LookupCaptureSource(const void *identity) override {
    diagnostic::ResidencyLock lock(residency_lock_, diagnostic::LockPath::Identity);
    auto entry = capture_sources_.find(identity);
    auto source = entry == capture_sources_.end() ? nullptr : entry->second.lock();
    return source && source->Live() ? source : nullptr;
  }

  std::shared_ptr<D3D12ResourceCaptureSource> LookupCaptureByVA(uint64_t address, uint64_t bytes, uint64_t &offset) override {
    offset = 0;
    diagnostic::ResidencyLock lock(residency_lock_, diagnostic::LockPath::CaptureVA);
    auto entry = interval_map_.upper_bound(address);
    if (entry == interval_map_.begin()) return {};
    --entry;
    if (!capture::Span(entry->second.logical_length, address - entry->first, bytes)) return {};
    auto source = entry->second.capture_source.lock();
    if (!source || !source->Live() || source->native->buffer_allocation.ptr() != entry->second.allocation ||
        !source->BufferRange(address, bytes, offset)) return {};
    return source;
  }

  Rc<BufferAllocation> LookupBufferByVA(D3D12_GPU_VIRTUAL_ADDRESS VA, uint64_t length, uint64_t *pOffset) {
    if (!pOffset) return {};
    diagnostic::ResidencyLock lock(residency_lock_, diagnostic::LockPath::BufferVA);
    return root_argument::AcquireByVA(interval_map_, VA, length, *pOffset,
                                      [](BufferAllocation *allocation) { return Rc<BufferAllocation>(allocation); });
  }

  InternalCommandLibrary &
  GetLib() {
    return command_library;
  }

  virtual FormatCapability
  GetMTLPixelFormatCapability(WMTPixelFormat Format) final {
    Format = ORIGINAL_FORMAT(Format);
    if (!format_inspector_.textureCapabilities.contains(Format))
      return FormatCapability(0);
    return format_inspector_.textureCapabilities.at(Format);
  };
};

HRESULT
CreateD3D12Device(IMTLDXGIAdapter *adapter, const IID &riid, void **ppDevice) {
  if (!ppDevice)
    return E_POINTER;
  *ppDevice = nullptr;
  if (!adapter)
    return E_INVALIDARG;

  DXGI_ADAPTER_DESC desc{};
  HRESULT hr = adapter->GetDesc(&desc);
  if (FAILED(hr))
    return hr;
  const auto luid = std::bit_cast<uint64_t>(desc.AdapterLuid);
  auto cache = GetD3D12DeviceCache();
  Com<MTLD3D12DeviceImpl> device;
  {
    std::lock_guard<dxmt::mutex> lock(cache->mutex);
    auto it = cache->devices.find(luid);
    if (it != cache->devices.end())
      device = it->second;
  }
  if (!device) {
    // Initialize outside the cache lock; only the winning candidate is exposed.
    auto candidate = Com(new MTLD3D12DeviceImpl(adapter, cache, luid));
    hr = candidate->Initialize();
    if (FAILED(hr))
      return hr;
    {
      std::lock_guard<dxmt::mutex> lock(cache->mutex);
      auto [it, inserted] = cache->devices.emplace(luid, candidate.ptr());
      device = it->second;
    }
  }
  hr = device->GetDeviceRemovedReason();
  if (FAILED(hr))
    return hr;
  return device->QueryInterface(riid, ppDevice);
};

} // namespace dxmt
