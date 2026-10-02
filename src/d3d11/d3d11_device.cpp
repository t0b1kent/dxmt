#include "com/com_guid.hpp"
#include "d3d11_fence.hpp"
#include "d3d11_input_layout.hpp"
#include "d3d11_interfaces.hpp"
#include "d3d11_multithread.hpp"
#include "d3d11_pipeline.hpp"
#include "d3d11_class_linkage.hpp"
#include "d3d11_inspection.hpp"
#include "d3d11_context.hpp"
#include "d3d11_context_state.hpp"
#include "d3d11_device.hpp"
#include "d3d11_pipeline_cache.hpp"
#include "d3d11_private.h"
#include "d3d11_query.hpp"
#include "d3d11_swapchain.hpp"
#include "d3d11_state_object.hpp"
#include "dxgi_interfaces.h"
#include "../d3d10/d3d10_device.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_device.hpp"
#include "dxmt_format.hpp"
#include "ftl.hpp"
#include "d3d11_resource.hpp"
#include "dxgi_object.hpp"
#include <memory>
#include "d3d11_4.h"
#include "util_win32_compat.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace dxmt {

const GUID kRenderdocUUID = {0xa7aa6116,
                             0x9c8d,
                             0x4bba,
                             {0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78}};
const GUID kPixUUID = {0x9f251514,
                       0x9d4d,
                       0x4902,
                       {0x9d, 0x60, 0x18, 0x98, 0x8a, 0xb7, 0xd4, 0xb5}};
const GUID kGpaUUID = {0xccffef16,
                       0x7b69,
                       0x468f,
                       {0xbc, 0xe3, 0xcd, 0x95, 0x33, 0x69, 0xa3, 0x9a}};

static bool dxmt_hk_cap_trace_enabled() {
  static int enabled = -1;
  if (enabled < 0) {
    const char *env = std::getenv("MACRUNNER_DXMT_CAP_TRACE");
    enabled = (env && env[0] && env[0] != '0') ? 1 : 0;
  }
  return enabled != 0;
}

static bool dxmt_hk_cap_trace_take_slot() {
  static unsigned count = 0;
  static unsigned max_count = 0;
  if (!dxmt_hk_cap_trace_enabled())
    return false;
  if (!max_count) {
    const char *env = std::getenv("MACRUNNER_DXMT_CAP_TRACE_MAX");
    char *end = nullptr;
    unsigned long parsed = env && env[0] ? std::strtoul(env, &end, 0) : 0;
    max_count = (end && end != env && parsed > 0 && parsed <= 1000000) ? parsed : 4096;
  }
  return ++count <= max_count;
}

static uint64_t dxmt_hk_cap_trace_qword(const void *data, UINT size, UINT offset) {
  uint64_t value = 0;
  if (data && size > offset) {
    UINT copy = std::min<UINT>(sizeof(value), size - offset);
    std::memcpy(&value, static_cast<const char *>(data) + offset, copy);
  }
  return value;
}

static void dxmt_hk_cap_trace_feature(const char *kind, UINT feature, UINT size,
                                      HRESULT hr, const void *data) {
  if (!dxmt_hk_cap_trace_take_slot())
    return;
  std::fprintf(stderr,
               "dxmt-hk-cap: kind=%s feature=%u size=%u hr=0x%08x "
               "q0=0x%llx q8=0x%llx q10=0x%llx q18=0x%llx\n",
               kind, feature, size, static_cast<unsigned>(hr),
               static_cast<unsigned long long>(dxmt_hk_cap_trace_qword(data, size, 0)),
               static_cast<unsigned long long>(dxmt_hk_cap_trace_qword(data, size, 8)),
               static_cast<unsigned long long>(dxmt_hk_cap_trace_qword(data, size, 16)),
               static_cast<unsigned long long>(dxmt_hk_cap_trace_qword(data, size, 24)));
}

static void dxmt_hk_cap_trace_format(const char *kind, UINT format, UINT arg1,
                                     UINT arg2, HRESULT hr, UINT out0) {
  if (!dxmt_hk_cap_trace_take_slot())
    return;
  std::fprintf(stderr,
               "dxmt-hk-cap: kind=%s format=%u arg1=%u arg2=%u "
               "hr=0x%08x out0=0x%x\n",
               kind, format, arg1, arg2, static_cast<unsigned>(hr), out0);
}

static const char *dxmt_hk_cap_trace_device_iid(REFIID riid) {
  if (riid == __uuidof(ID3D11Device)) return "ID3D11Device";
  if (riid == __uuidof(ID3D11Device1)) return "ID3D11Device1";
  if (riid == __uuidof(ID3D11Device2)) return "ID3D11Device2";
  if (riid == __uuidof(ID3D11Device3)) return "ID3D11Device3";
  if (riid == __uuidof(ID3D11Device4)) return "ID3D11Device4";
  if (riid == __uuidof(ID3D11Device5)) return "ID3D11Device5";
  if (riid == __uuidof(ID3D11Multithread)) return "ID3D11Multithread";
  return nullptr;
}

static void dxmt_hk_cap_trace_qi(const char *iface, HRESULT hr) {
  if (!iface || !dxmt_hk_cap_trace_take_slot())
    return;
  std::fprintf(stderr, "dxmt-hk-cap: kind=QueryInterface iface=%s hr=0x%08x\n",
               iface, static_cast<unsigned>(hr));
}

static bool dxmt_hk_swap_trace_enabled() {
  static int enabled = -1;
  if (enabled < 0) {
    const char *env = std::getenv("MACRUNNER_DXMT_SWAPCHAIN_TRACE");
    enabled = (env && env[0] && std::strcmp(env, "0") != 0) ? 1 : 0;
  }
  return enabled != 0;
}

static bool dxmt_hk_swap_trace_take_slot() {
  static unsigned count = 0;
  static unsigned max_count = 0;
  if (!dxmt_hk_swap_trace_enabled())
    return false;
  if (!max_count) {
    const char *env = std::getenv("MACRUNNER_DXMT_SWAPCHAIN_TRACE_MAX");
    char *end = nullptr;
    unsigned long parsed = env && env[0] ? std::strtoul(env, &end, 0) : 0;
    max_count = (end && end != env && parsed > 0 && parsed <= 1000000) ? parsed : 4096;
  }
  return ++count <= max_count;
}

static void dxmt_hk_swap_trace_device_method(const char *method, const void *self,
                                             HRESULT hr, const void *resource,
                                             const void *desc, const void *out,
                                             const void *ret0, const void *ret1,
                                             const void *ret2, const void *ret3) {
  if (!dxmt_hk_swap_trace_take_slot())
    return;
  std::fprintf(stderr,
               "dxmt-hk-swaptrace: kind=Device method=%s tid=%lu this=%p hr=0x%08x "
               "resource=%p desc=%p out=%p ret0=%p ret1=%p ret2=%p ret3=%p\n",
               method, (unsigned long)GetCurrentThreadId(), self,
               static_cast<unsigned>(static_cast<uint32_t>(hr)), resource, desc,
               out, ret0, ret1, ret2, ret3);
}

class MTLD3D11DeviceImpl final : public MTLD3D11Device, public IMTLD3D11DeviceExt {
friend class MTLD3D11DXGIDevice;
public:
  MTLD3D11DeviceImpl(
      MTLDXGIObject<IMTLDXGIDevice> *container, IMTLDXGIAdapter *pAdapter, D3D_FEATURE_LEVEL FeatureLevel,
      UINT FeatureFlags, Device &device
  ) :
      container_(container),
      feature_level_(FeatureLevel),
      feature_flags_(FeatureFlags),
      features_(container->GetMTLDevice()),
      sampler_states_(this),
      rasterizer_states_(this),
      depthstencil_states_(this),
      device_(device),
      d3dmt_(static_cast<ID3D11Device *>(this), mutex) {
    commandlist_pool_ = InitializeCommandListPool(this);
    pipeline_cache_ = InitializePipelineCache(this);
    context_ = InitializeImmediateContext(this, device_.queue());
    d3d10_ = std::make_unique<MTLD3D10Device>(this, context_.get());
    is_traced_ = !!::GetModuleHandle("dxgitrace.dll");
    format_inspector_.Inspect(GetMTLDevice());
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return container_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() override { return container_->Release(); }

  bool IsTraced() override { return is_traced_; }

  HRESULT STDMETHODCALLTYPE
  CreateBuffer(const D3D11_BUFFER_DESC *pDesc,
               const D3D11_SUBRESOURCE_DATA *pInitialData,
               ID3D11Buffer **ppBuffer) override {
    InitReturnPtr(ppBuffer);

    if (pDesc->ByteWidth == 0 && !(pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILE_POOL))
      return E_INVALIDARG; 

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
      case D3D11_USAGE_DYNAMIC:
        return dxmt::CreateBuffer(this, pDesc, pInitialData, ppBuffer);
      case D3D11_USAGE_STAGING:
        return CreateStagingBuffer(this, pDesc, pInitialData, ppBuffer);
      default:
        DXMT_UNREACHABLE
      }
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture1D **ppTexture1D) override {

    InitReturnPtr(ppTexture1D);

    if (!pDesc)
      return E_INVALIDARG;

    if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED)
      return E_INVALIDARG; // not supported yet

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
        return CreateDeviceTexture1D(this, pDesc, pInitialData, ppTexture1D);
      case D3D11_USAGE_DYNAMIC: {
        HRESULT hr = CreateDynamicLinearTexture1D(this, pDesc, pInitialData, ppTexture1D);
        if (SUCCEEDED(hr))
          return hr;
        return CreateDynamicTexture1D(this, pDesc, pInitialData, ppTexture1D);
      }
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture1D(this, pDesc, pInitialData, ppTexture1D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture2D **ppTexture2D) override {
    D3D11_TEXTURE2D_DESC1 desc1;
    UpgradeResourceDescription(pDesc, desc1);
    return CreateTexture2D1(&desc1, pInitialData,
                            (ID3D11Texture2D1 **)ppTexture2D);
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc,
                  const D3D11_SUBRESOURCE_DATA *pInitialData,
                  ID3D11Texture3D **ppTexture3D) override {
    D3D11_TEXTURE3D_DESC1 desc1;
    UpgradeResourceDescription(pDesc, desc1);
    return CreateTexture3D1(&desc1, pInitialData,
                            (ID3D11Texture3D1 **)ppTexture3D);
  }

  HRESULT STDMETHODCALLTYPE CreateShaderResourceView(
      ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc,
      ID3D11ShaderResourceView **ppSRView) override {
    if (pDesc) {
      D3D11_SHADER_RESOURCE_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      return CreateShaderResourceView1(pResource, &desc1,
                                       (ID3D11ShaderResourceView1 **)ppSRView);
    }
    return CreateShaderResourceView1(pResource, nullptr,
                                     (ID3D11ShaderResourceView1 **)ppSRView);
  }

  HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(
      ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc,
      ID3D11UnorderedAccessView **ppUAView) override {
    if (pDesc) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      return CreateUnorderedAccessView1(
          pResource, &desc1, (ID3D11UnorderedAccessView1 **)ppUAView);
    }
    return CreateUnorderedAccessView1(pResource, nullptr,
                                      (ID3D11UnorderedAccessView1 **)ppUAView);
  }

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(
      ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
      ID3D11RenderTargetView **ppRTView) override {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    HRESULT hr;
    if (pDesc) {
      D3D11_RENDER_TARGET_VIEW_DESC1 desc1;
      UpgradeViewDescription(pDesc, desc1);
      hr = CreateRenderTargetView1(pResource, &desc1,
                                   (ID3D11RenderTargetView1 **)ppRTView);
      dxmt_hk_swap_trace_device_method("CreateRenderTargetView", this, hr,
                                       pResource, pDesc,
                                       ppRTView ? *ppRTView : nullptr, ret0,
                                       ret1, ret2, ret3);
      return hr;
    }
    hr = CreateRenderTargetView1(pResource, nullptr,
                                 (ID3D11RenderTargetView1 **)ppRTView);
    dxmt_hk_swap_trace_device_method("CreateRenderTargetView", this, hr,
                                     pResource, nullptr,
                                     ppRTView ? *ppRTView : nullptr, ret0,
                                     ret1, ret2, ret3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(
      ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
      ID3D11DepthStencilView **ppDepthStencilView) override {
    InitReturnPtr(ppDepthStencilView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppDepthStencilView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateDepthStencilView(pDesc, ppDepthStencilView);
  }

  HRESULT STDMETHODCALLTYPE CreateInputLayout(
      const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements,
      const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
      ID3D11InputLayout **ppInputLayout) override {
    InitReturnPtr(ppInputLayout);

    if (!pInputElementDescs)
      return E_INVALIDARG;
    if (!pShaderBytecodeWithInputSignature)
      return E_INVALIDARG;

    // TODO: must get shader reflection info

    if (!ppInputLayout) {
      return S_FALSE;
    }

    return pipeline_cache_->AddInputLayout(
        pShaderBytecodeWithInputSignature, pInputElementDescs, NumElements,
        (IMTLD3D11InputLayout **)ppInputLayout);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                     ID3D11ClassLinkage *pClassLinkage,
                     ID3D11VertexShader **ppVertexShader) override {
    InitReturnPtr(ppVertexShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddVertexShader(pShaderBytecode, BytecodeLength,
                                            ppVertexShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                       ID3D11ClassLinkage *pClassLinkage,
                       ID3D11GeometryShader **ppGeometryShader) override {
    InitReturnPtr(ppGeometryShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppGeometryShader)
      return S_FALSE;

    return pipeline_cache_->AddGeometryShader(pShaderBytecode, BytecodeLength,
                                              ppGeometryShader);
  }

  HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(
      const void *pShaderBytecode, SIZE_T BytecodeLength,
      const D3D11_SO_DECLARATION_ENTRY *pSODeclaration, UINT NumEntries,
      const UINT *pBufferStrides, UINT NumStrides, UINT RasterizedStream,
      ID3D11ClassLinkage *pClassLinkage,
      ID3D11GeometryShader **ppGeometryShader) override {
    InitReturnPtr(ppGeometryShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (NumEntries > 0 && RasterizedStream == D3D11_SO_NO_RASTERIZED_STREAM &&
        ((char *)pShaderBytecode)[0] == 'D' &&
        ((char *)pShaderBytecode)[1] == 'X' &&
        ((char *)pShaderBytecode)[2] == 'B' &&
        ((char *)pShaderBytecode)[3] == 'C') {
      // FIXME: ensure the input shader is a vertex shader
      WARN("Emulate stream output");

      Com<IMTLD3D11StreamOutputLayout> so_layout;
      HRESULT hr = pipeline_cache_->AddStreamOutputLayout(
          pShaderBytecode, NumEntries, pSODeclaration, NumStrides,
          pBufferStrides, RasterizedStream, &so_layout);
      if (SUCCEEDED(hr))
        return so_layout->QueryInterface(IID_PPV_ARGS(ppGeometryShader));
    }
    ERR("CreateGeometryShaderWithStreamOutput: not supported, expect problem");
    return pipeline_cache_->AddGeometryShader(pShaderBytecode, BytecodeLength,
                                              ppGeometryShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                    ID3D11ClassLinkage *pClassLinkage,
                    ID3D11PixelShader **ppPixelShader) override {
    InitReturnPtr(ppPixelShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddPixelShader(pShaderBytecode, BytecodeLength,
                                           ppPixelShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                   ID3D11ClassLinkage *pClassLinkage,
                   ID3D11HullShader **ppHullShader) override {
    InitReturnPtr(ppHullShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppHullShader)
      return S_FALSE;

    return pipeline_cache_->AddHullShader(pShaderBytecode, BytecodeLength,
                                          ppHullShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                     ID3D11ClassLinkage *pClassLinkage,
                     ID3D11DomainShader **ppDomainShader) override {
    InitReturnPtr(ppDomainShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    if (!ppDomainShader)
      return S_FALSE;

    return pipeline_cache_->AddDomainShader(pShaderBytecode, BytecodeLength,
                                            ppDomainShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength,
                      ID3D11ClassLinkage *pClassLinkage,
                      ID3D11ComputeShader **ppComputeShader) override {
    InitReturnPtr(ppComputeShader);
    if (pClassLinkage != nullptr)
      WARN("Class linkage not supported");

    return pipeline_cache_->AddComputeShader(pShaderBytecode, BytecodeLength,
                                             ppComputeShader);
  }

  HRESULT STDMETHODCALLTYPE
  CreateClassLinkage(ID3D11ClassLinkage **ppLinkage) override {
    *ppLinkage = ref(new MTLD3D11ClassLinkage(this));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc,
                   ID3D11BlendState **ppBlendState) override {
    ID3D11BlendState1 *pBlendState1;
    D3D11_BLEND_DESC1 desc;
    desc.AlphaToCoverageEnable = pBlendStateDesc->AlphaToCoverageEnable;
    desc.IndependentBlendEnable = pBlendStateDesc->IndependentBlendEnable;

    for (uint32_t i = 0; i < 8; i++) {
      desc.RenderTarget[i].BlendEnable =
          pBlendStateDesc->RenderTarget[i].BlendEnable;
      desc.RenderTarget[i].LogicOpEnable = FALSE;
      desc.RenderTarget[i].LogicOp = D3D11_LOGIC_OP_NOOP;
      desc.RenderTarget[i].SrcBlend = pBlendStateDesc->RenderTarget[i].SrcBlend;
      desc.RenderTarget[i].DestBlend =
          pBlendStateDesc->RenderTarget[i].DestBlend;
      desc.RenderTarget[i].BlendOp = pBlendStateDesc->RenderTarget[i].BlendOp;
      desc.RenderTarget[i].SrcBlendAlpha =
          pBlendStateDesc->RenderTarget[i].SrcBlendAlpha;
      desc.RenderTarget[i].DestBlendAlpha =
          pBlendStateDesc->RenderTarget[i].DestBlendAlpha;
      desc.RenderTarget[i].BlendOpAlpha =
          pBlendStateDesc->RenderTarget[i].BlendOpAlpha;
      desc.RenderTarget[i].RenderTargetWriteMask =
          pBlendStateDesc->RenderTarget[i].RenderTargetWriteMask;
    }
    auto hr = CreateBlendState1(&desc, &pBlendState1);
    *ppBlendState = pBlendState1;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateDepthStencilState(
      const D3D11_DEPTH_STENCIL_DESC *pDesc,
      ID3D11DepthStencilState **ppDepthStencilState) override {
    return depthstencil_states_.CreateStateObject(
        pDesc, (IMTLD3D11DepthStencilState **)ppDepthStencilState);
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc,
                        ID3D11RasterizerState **ppRasterizerState) override {
    ID3D11RasterizerState2 *pRasterizerState;
    D3D11_RASTERIZER_DESC2 desc;
    desc.FillMode = pRasterizerDesc->FillMode;
    desc.CullMode = pRasterizerDesc->CullMode;
    desc.FrontCounterClockwise = pRasterizerDesc->FrontCounterClockwise;
    desc.DepthBias = pRasterizerDesc->DepthBias;
    desc.DepthBiasClamp = pRasterizerDesc->DepthBiasClamp;
    desc.SlopeScaledDepthBias = pRasterizerDesc->SlopeScaledDepthBias;
    desc.DepthClipEnable = pRasterizerDesc->DepthClipEnable;
    desc.ScissorEnable = pRasterizerDesc->ScissorEnable;
    desc.MultisampleEnable = pRasterizerDesc->MultisampleEnable;
    desc.AntialiasedLineEnable = pRasterizerDesc->AntialiasedLineEnable;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    auto hr = CreateRasterizerState2(&desc, &pRasterizerState);
    *ppRasterizerState = pRasterizerState;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc,
                     ID3D11SamplerState **ppSamplerState) override {
    return sampler_states_.CreateStateObject(
        pSamplerDesc, (D3D11SamplerState **)ppSamplerState);
  }

  HRESULT STDMETHODCALLTYPE
  CreateQuery(const D3D11_QUERY_DESC *pQueryDesc, ID3D11Query **ppQuery) override {
    D3D11_QUERY_DESC1 desc;
    desc.Query = pQueryDesc->Query;
    desc.MiscFlags = pQueryDesc->MiscFlags;
    desc.ContextType = D3D11_CONTEXT_TYPE_ALL;
    return CreateQuery1(&desc, reinterpret_cast<ID3D11Query1 **>(ppQuery));
  }

  HRESULT STDMETHODCALLTYPE
  CreatePredicate(const D3D11_QUERY_DESC *pPredicateDesc,
                  ID3D11Predicate **ppPredicate) override {
    return CreateQuery(pPredicateDesc,
                       reinterpret_cast<ID3D11Query **>(ppPredicate));
  }

  HRESULT STDMETHODCALLTYPE
  CreateCounter(const D3D11_COUNTER_DESC *pCounterDesc,
                ID3D11Counter **ppCounter) override {
    InitReturnPtr(ppCounter);
    if (!pCounterDesc)
      return E_INVALIDARG;
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext(
      UINT ContextFlags,
      ID3D11DeviceContext **ppDeferredContext) override{
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext *>(ppDeferredContext3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) override {
    return ImportSharedTexture(this, hResource, ReturnedInterface, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
      CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport) override {
    auto trace_return = [&](HRESULT hr) -> HRESULT {
      dxmt_hk_cap_trace_format("CheckFormatSupport", Format, 0, 0, hr,
                               pFormatSupport ? *pFormatSupport : 0);
      return hr;
    };

    if (pFormatSupport) {
      *pFormatSupport = 0;
    }

    if (Format == DXGI_FORMAT_UNKNOWN) {
      *pFormatSupport =
          D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;
      return trace_return(S_OK);
    }

    MTL_DXGI_FORMAT_DESC metal_format;
    if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), Format, metal_format))) {
      return trace_return(E_INVALIDARG);
    }

    UINT outFormatSupport = 0;

    if (metal_format.PixelFormat) {
      // All graphics and compute kernels can read or sample a texture with any pixel format.
      outFormatSupport |= D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                          D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD |
                          D3D11_FORMAT_SUPPORT_CPU_LOCKABLE;

      /* UNCHECKED */
      outFormatSupport |= D3D11_FORMAT_SUPPORT_TEXTURE1D | D3D11_FORMAT_SUPPORT_TEXTURE2D |
                          D3D11_FORMAT_SUPPORT_TEXTURE3D | D3D11_FORMAT_SUPPORT_TEXTURECUBE |
                          D3D11_FORMAT_SUPPORT_MIP |
                          D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT;

      if (!(metal_format.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER))) {
        outFormatSupport |= D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
      }

      if (metal_format.Flag & MTL_DXGI_FORMAT_BACKBUFFER) {
        outFormatSupport |= D3D11_FORMAT_SUPPORT_DISPLAY;
      }
    }

    if (metal_format.AttributeFormat) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER;
    }

    if (metal_format.PixelFormat == WMTPixelFormatR32Uint ||
        metal_format.PixelFormat == WMTPixelFormatR16Uint) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER;
    }

    auto Capability = GetMTLPixelFormatCapability(metal_format.PixelFormat);

    if (any_bit_set(Capability & FormatCapability::Color)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    }

    if (any_bit_set(Capability & FormatCapability::Blend)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_BLENDABLE;
    }

    if (any_bit_set(Capability & FormatCapability::DepthStencil)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL |
                          D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON |
                          D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON;
    }

    if (any_bit_set(Capability & FormatCapability::Resolve)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
    }

    if (any_bit_set(Capability & FormatCapability::MSAA)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    }

    if (any_bit_set(Capability & FormatCapability::Write)) {
      outFormatSupport |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    }

    if (Format == DXGI_FORMAT_R32_FLOAT || Format == DXGI_FORMAT_R32_UINT ||
        Format == DXGI_FORMAT_R32_SINT || Format == DXGI_FORMAT_R32G32_FLOAT ||
        Format == DXGI_FORMAT_R32G32_UINT ||
        Format == DXGI_FORMAT_R32G32_SINT ||
        Format == DXGI_FORMAT_R32G32B32_FLOAT ||
        Format == DXGI_FORMAT_R32G32B32_UINT ||
        Format == DXGI_FORMAT_R32G32B32_SINT ||
        Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
        Format == DXGI_FORMAT_R32G32B32A32_UINT ||
        Format == DXGI_FORMAT_R32G32B32A32_SINT)
      outFormatSupport |= D3D11_FORMAT_SUPPORT_SO_BUFFER;

    if (pFormatSupport) {
      *pFormatSupport = outFormatSupport;
    }

    return trace_return(S_OK);
  }

  HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(
      DXGI_FORMAT Format, UINT SampleCount, UINT *pNumQualityLevels) override {
    HRESULT hr = CheckMultisampleQualityLevels1(Format, SampleCount, 0,
                                                pNumQualityLevels);
    dxmt_hk_cap_trace_format("CheckMultisampleQualityLevels", Format, SampleCount, 0,
                             hr, pNumQualityLevels ? *pNumQualityLevels : 0);
    return hr;
  }

  void STDMETHODCALLTYPE
  CheckCounterInfo(D3D11_COUNTER_INFO *pCounterInfo) override {
    if (!pCounterInfo)
      return;
    pCounterInfo->LastDeviceDependentCounter = D3D11_COUNTER{D3D11_COUNTER_DEVICE_DEPENDENT_0};
    pCounterInfo->NumSimultaneousCounters = 0;
    pCounterInfo->NumDetectableParallelUnits = 0;
  }

  HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC *pDesc,
                                         D3D11_COUNTER_TYPE *pType,
                                         UINT *pActiveCounters, LPSTR szName,
                                         UINT *pNameLength, LPSTR szUnits,
                                         UINT *pUnitsLength,
                                         LPSTR szDescription,
                                         UINT *pDescriptionLength) override {
    if (!pDesc || !pType || !pActiveCounters)
      return E_INVALIDARG;

    *pType = D3D11_COUNTER_TYPE_UINT32;
    *pActiveCounters = 0;

    auto clear_string = [](LPSTR string, UINT *length) {
      if (!length)
        return;
      UINT capacity = *length;
      *length = 0;
      if (string && capacity)
        string[0] = '\0';
    };

    clear_string(szName, pNameLength);
    clear_string(szUnits, pUnitsLength);
    clear_string(szDescription, pDescriptionLength);

    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(D3D11_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    // Format support queries are special in that they use in-out
    // structs
    case D3D11_FEATURE_FORMAT_SUPPORT: {
      auto info =
          static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info)) {
        dxmt_hk_cap_trace_feature("CheckFeatureSupport", Feature, FeatureSupportDataSize,
                                  E_INVALIDARG, pFeatureSupportData);
        return E_INVALIDARG;
      }

      HRESULT hr = CheckFormatSupport(info->InFormat, &info->OutFormatSupport);
      dxmt_hk_cap_trace_feature("CheckFeatureSupport", Feature, FeatureSupportDataSize,
                                hr, pFeatureSupportData);
      return hr;
    }
    case D3D11_FEATURE_FORMAT_SUPPORT2: {
      auto info = static_cast<D3D11_FEATURE_DATA_FORMAT_SUPPORT2 *>(
          pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info)) {
        dxmt_hk_cap_trace_feature("CheckFeatureSupport", Feature, FeatureSupportDataSize,
                                  E_INVALIDARG, pFeatureSupportData);
        return E_INVALIDARG;
      }
      info->OutFormatSupport2 = 0;

      if (info->InFormat == DXGI_FORMAT_UNKNOWN) {
        info->OutFormatSupport2 |=
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
            D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE |
            D3D11_FORMAT_SUPPORT2_SHAREABLE;
        dxmt_hk_cap_trace_feature("CheckFeatureSupport", Feature, FeatureSupportDataSize,
                                  S_OK, pFeatureSupportData);
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC metal_format;
      if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), info->InFormat,
                                    metal_format))) {
        dxmt_hk_cap_trace_feature("CheckFeatureSupport", Feature, FeatureSupportDataSize,
                                  E_INVALIDARG, pFeatureSupportData);
        return E_INVALIDARG;
      }
      auto Capability = GetMTLPixelFormatCapability(metal_format.PixelFormat);

      if (any_bit_set(Capability & FormatCapability::TextureBufferRead)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
      }

      if (any_bit_set(Capability & FormatCapability::TextureBufferWrite)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
      }

      if (any_bit_set(Capability & FormatCapability::TextureBufferReadWrite)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
                                   D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
      }

      if (any_bit_set(Capability & FormatCapability::Atomic)) {
        info->OutFormatSupport2 |=
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX |
            D3D11_FORMAT_SUPPORT2_SHAREABLE;
      }

#ifndef DXMT_NO_PRIVATE_API
      if (any_bit_set(Capability & FormatCapability::Blend)) {
        /* UNCHECKED */
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP;
      }
#endif

      if (any_bit_set(Capability & FormatCapability::Sparse)) {
        info->OutFormatSupport2 |= D3D11_FORMAT_SUPPORT2_TILED;
      }

      dxmt_hk_cap_trace_feature("CheckFeatureSupport", Feature, FeatureSupportDataSize,
                                S_OK, pFeatureSupportData);
      return S_OK;
    }
    default:
      // For everything else, we can use the device feature struct
      // that we already initialized during device creation.
      HRESULT hr = features_.GetFeatureData(Feature, FeatureSupportDataSize,
                                            pFeatureSupportData);
      dxmt_hk_cap_trace_feature("CheckFeatureSupport", Feature, FeatureSupportDataSize,
                                hr, pFeatureSupportData);
      return hr;
    }
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize,
                                           void *pData) override {
    return container_->GetPrivateData(guid, pDataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize,
                                           const void *pData) override {
    return container_->SetPrivateData(guid, DataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override {
    return container_->SetPrivateDataInterface(guid, pData);
  }

  D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override {
    dxmt_hk_cap_trace_format("GetFeatureLevel", feature_level_, 0, 0, S_OK, feature_level_);
    return feature_level_;
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return feature_flags_ & 0x7fffffff; }

  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
    // unless we are deal with eGPU, this method should awalys return S_OK?
    return S_OK;
  }

  void STDMETHODCALLTYPE
  GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override {
    if (RaiseFlags & ~D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR)
      return E_INVALIDARG;
    exception_mode_ = RaiseFlags;
    return S_OK;
  }

  UINT STDMETHODCALLTYPE GetExceptionMode() override {
    return exception_mode_;
  }

  void STDMETHODCALLTYPE
  GetImmediateContext1(ID3D11DeviceContext1 **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext1(
      UINT ContextFlags,
      ID3D11DeviceContext1 **ppDeferredContext) override{
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext1 *>(ppDeferredContext3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
      CreateBlendState1(const D3D11_BLEND_DESC1 *pBlendStateDesc,
                        ID3D11BlendState1 **ppBlendState) override {
    return pipeline_cache_->AddBlendState(pBlendStateDesc,
                                          (IMTLD3D11BlendState **)ppBlendState);
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState1(const D3D11_RASTERIZER_DESC1 *pRasterizerDesc,
                         ID3D11RasterizerState1 **ppRasterizerState) override {
    ID3D11RasterizerState2 *pRasterizerState;
    D3D11_RASTERIZER_DESC2 desc;
    desc.FillMode = pRasterizerDesc->FillMode;
    desc.CullMode = pRasterizerDesc->CullMode;
    desc.FrontCounterClockwise = pRasterizerDesc->FrontCounterClockwise;
    desc.DepthBias = pRasterizerDesc->DepthBias;
    desc.DepthBiasClamp = pRasterizerDesc->DepthBiasClamp;
    desc.SlopeScaledDepthBias = pRasterizerDesc->SlopeScaledDepthBias;
    desc.DepthClipEnable = pRasterizerDesc->DepthClipEnable;
    desc.ScissorEnable = pRasterizerDesc->ScissorEnable;
    desc.MultisampleEnable = pRasterizerDesc->MultisampleEnable;
    desc.AntialiasedLineEnable = pRasterizerDesc->AntialiasedLineEnable;
    desc.ForcedSampleCount = pRasterizerDesc->ForcedSampleCount;
    desc.ConservativeRaster = D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    auto hr = CreateRasterizerState2(&desc, &pRasterizerState);
    *ppRasterizerState = pRasterizerState;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateDeviceContextState(
      UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
      UINT SDKVersion, REFIID EmulatedInterface,
      D3D_FEATURE_LEVEL *pChosenFeatureLevel,
      ID3DDeviceContextState **ppContextState) override {
    InitReturnPtr(ppContextState);
    if (pChosenFeatureLevel)
      *pChosenFeatureLevel = (D3D_FEATURE_LEVEL)0;

    // TODO: validation
    D3D_FEATURE_LEVEL chosen_feature_level = feature_level_;
    if (pFeatureLevels && FeatureLevels) {
      bool found_feature_level = false;
      for (UINT i = 0; i < FeatureLevels; i++) {
        if (pFeatureLevels[i] <= feature_level_) {
          chosen_feature_level = pFeatureLevels[i];
          found_feature_level = true;
          break;
        }
      }
      if (!found_feature_level)
        return E_INVALIDARG;
    }

    if (ppContextState == nullptr) {
      return S_FALSE;
    }
    if (pChosenFeatureLevel)
      *pChosenFeatureLevel = chosen_feature_level;
    *ppContextState = ref(new MTLD3D11DeviceContextState(this));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResource1(HANDLE hResource, REFIID ReturnedInterface, void **ppResource) override {
    return ImportSharedTextureFromNtHandle(this, hResource, ReturnedInterface, ppResource);
  }

  HRESULT STDMETHODCALLTYPE
  OpenSharedResourceByName(LPCWSTR lpName, DWORD dwDesiredAccess, REFIID ReturnedInterface, void **ppResource)
      override {
    return ImportSharedTextureByName(this, lpName, dwDesiredAccess, ReturnedInterface, ppResource);
  }

  void STDMETHODCALLTYPE
  GetImmediateContext2(ID3D11DeviceContext2 **ppImmediateContext) override {
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE
  CreateDeferredContext2(UINT ContextFlags, ID3D11DeviceContext2 **ppDeferredContext) override {
    ID3D11DeviceContext3 *ppDeferredContext3;
    HRESULT hr = CreateDeferredContext3(ContextFlags, &ppDeferredContext3);
    *ppDeferredContext = static_cast<ID3D11DeviceContext2 *>(ppDeferredContext3);
    return hr;
  }

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D11Resource *resource, UINT *tile_count,
      D3D11_PACKED_MIP_DESC *mip_desc, D3D11_TILE_SHAPE *tile_shape,
      UINT *subresource_tiling_count, UINT first_subresource_tiling,
      D3D11_SUBRESOURCE_TILING *subresource_tiling) override{
    (void)resource;
    (void)first_subresource_tiling;
    (void)subresource_tiling;

    if (tile_count)
      *tile_count = 0;
    if (mip_desc)
      *mip_desc = {};
    if (tile_shape)
      *tile_shape = {};
    if (subresource_tiling_count)
      *subresource_tiling_count = 0;
  }

  HRESULT STDMETHODCALLTYPE
  CheckMultisampleQualityLevels1(DXGI_FORMAT Format, UINT SampleCount, UINT Flags, UINT *pNumQualityLevels) override {
    if (!pNumQualityLevels) {
      dxmt_hk_cap_trace_format("CheckMultisampleQualityLevels1", Format, SampleCount, Flags,
                               E_INVALIDARG, 0);
      return E_INVALIDARG;
    }

    *pNumQualityLevels = 0;
    if (Flags) {
      dxmt_hk_cap_trace_format("CheckMultisampleQualityLevels1", Format, SampleCount, Flags,
                               E_INVALIDARG, *pNumQualityLevels);
      return E_INVALIDARG;
    }
    MTL_DXGI_FORMAT_DESC desc;
    if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), Format, desc)) ||
        desc.PixelFormat == WMTPixelFormatInvalid) {
      dxmt_hk_cap_trace_format("CheckMultisampleQualityLevels1", Format, SampleCount, Flags,
                               E_INVALIDARG, *pNumQualityLevels);
      return E_INVALIDARG;
    }

    // MSDN:
    // FEATURE_LEVEL_11_0 devices are required to support 4x MSAA for all render
    // target formats, and 8x MSAA for all render target formats except
    // R32G32B32A32 formats.

    // seems some pixel format doesn't support MSAA
    // https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf

    if (GetMTLDevice().supportsTextureSampleCount(SampleCount)) {
      *pNumQualityLevels = 1; // always 1: in metal there is no concept of
                              // Quality Level (so is it in vulkan iirc)
    }
    dxmt_hk_cap_trace_format("CheckMultisampleQualityLevels1", Format, SampleCount, Flags,
                             S_OK, *pNumQualityLevels);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture2D1(const D3D11_TEXTURE2D_DESC1 *pDesc,
                   const D3D11_SUBRESOURCE_DATA *pInitialData,
                   ID3D11Texture2D1 **ppTexture2D) override {
    InitReturnPtr(ppTexture2D);

    if (!pDesc)
      return E_INVALIDARG;

    if ((pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED))
      return E_INVALIDARG; // not supported yet

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
        return CreateDeviceTexture2D(this, pDesc, pInitialData, ppTexture2D);
      case D3D11_USAGE_DYNAMIC: {
        HRESULT hr = CreateDynamicLinearTexture2D(this, pDesc, pInitialData, ppTexture2D);
        if (SUCCEEDED(hr))
          return hr;
        return CreateDynamicTexture2D(this, pDesc, pInitialData, ppTexture2D);
      }
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture2D(this, pDesc, pInitialData, ppTexture2D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateTexture3D1(const D3D11_TEXTURE3D_DESC1 *pDesc,
                   const D3D11_SUBRESOURCE_DATA *pInitialData,
                   ID3D11Texture3D1 **ppTexture3D) override {
    InitReturnPtr(ppTexture3D);

    if (!pDesc)
      return E_INVALIDARG;

    if ((pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED))
      return E_INVALIDARG; // not supported yet

    try {
      switch (pDesc->Usage) {
      case D3D11_USAGE_DEFAULT:
      case D3D11_USAGE_IMMUTABLE:
        return CreateDeviceTexture3D(this, pDesc, pInitialData, ppTexture3D);
      case D3D11_USAGE_DYNAMIC:
        return CreateDynamicTexture3D(this, pDesc, pInitialData, ppTexture3D);
      case D3D11_USAGE_STAGING:
        if (pDesc->BindFlags != 0) {
          return E_INVALIDARG;
        }
        return CreateStagingTexture3D(this, pDesc, pInitialData, ppTexture3D);
      }
      return S_OK;
    } catch (const MTLD3DError &err) {
      ERR(err.message());
      return E_FAIL;
    }
  }

  HRESULT STDMETHODCALLTYPE
  CreateRasterizerState2(const D3D11_RASTERIZER_DESC2 *pRasterizerDesc,
                         ID3D11RasterizerState2 **ppRasterizerState) override {
    return rasterizer_states_.CreateStateObject(
        pRasterizerDesc, (IMTLD3D11RasterizerState **)ppRasterizerState);
  }

  HRESULT STDMETHODCALLTYPE CreateShaderResourceView1(
      ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
      ID3D11ShaderResourceView1 **ppSRView) override {
    InitReturnPtr(ppSRView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppSRView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateShaderResourceView(pDesc, ppSRView);
  }

  HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView1(
      ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
      ID3D11UnorderedAccessView1 **ppUAView) override {
    InitReturnPtr(ppUAView);

    if (!pResource)
      return E_INVALIDARG;

    if (!ppUAView)
      return S_FALSE;

    return static_cast<D3D11ResourceCommon *>(pResource)->CreateUnorderedAccessView(pDesc, ppUAView);
  }

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView1(
      ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
      ID3D11RenderTargetView1 **ppRTView) override {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    InitReturnPtr(ppRTView);

    if (!pResource) {
      dxmt_hk_swap_trace_device_method("CreateRenderTargetView1", this,
                                       E_INVALIDARG, pResource, pDesc, nullptr,
                                       ret0, ret1, ret2, ret3);
      return E_INVALIDARG;
    }

    if (!ppRTView) {
      dxmt_hk_swap_trace_device_method("CreateRenderTargetView1", this, S_FALSE,
                                       pResource, pDesc, nullptr, ret0, ret1,
                                       ret2, ret3);
      return S_FALSE;
    }

    HRESULT hr = static_cast<D3D11ResourceCommon *>(pResource)->CreateRenderTargetView(pDesc, ppRTView);
    dxmt_hk_swap_trace_device_method("CreateRenderTargetView1", this, hr,
                                     pResource, pDesc,
                                     ppRTView ? *ppRTView : nullptr, ret0,
                                     ret1, ret2, ret3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  CreateQuery1(const D3D11_QUERY_DESC1 *pQueryDesc, ID3D11Query1 **ppQuery) override {
    InitReturnPtr(ppQuery);

    if (!pQueryDesc)
      return E_INVALIDARG;

    if (!ppQuery)
      return S_FALSE;

    switch (pQueryDesc->Query) {
    case D3D11_QUERY_EVENT:
      *ppQuery = ref(new MTLD3D11EventQueryImpl<BOOL>(this, pQueryDesc));
      return S_OK;
    case D3D11_QUERY_OCCLUSION:
    case D3D11_QUERY_OCCLUSION_PREDICATE:
      return CreateOcclusionQuery(this, pQueryDesc, ppQuery);
    case D3D11_QUERY_TIMESTAMP:
      return CreateTimestampQuery(this, pQueryDesc, ppQuery);
    case D3D11_QUERY_TIMESTAMP_DISJOINT: {
      *ppQuery = ref(new MTLD3D11EventQueryImpl<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT>(this, pQueryDesc));
      return S_OK;
    }
    case D3D11_QUERY_PIPELINE_STATISTICS: {
      *ppQuery = ref(new MTLD3D11ImmediateQuery<D3D11_QUERY_DATA_PIPELINE_STATISTICS>(this, pQueryDesc));
      return S_OK;
    }
    case D3D11_QUERY_SO_STATISTICS:
    case D3D11_QUERY_SO_STATISTICS_STREAM0:
    case D3D11_QUERY_SO_STATISTICS_STREAM1:
    case D3D11_QUERY_SO_STATISTICS_STREAM2:
    case D3D11_QUERY_SO_STATISTICS_STREAM3: {
      *ppQuery = ref(new MTLD3D11ImmediateQuery<D3D11_QUERY_DATA_SO_STATISTICS>(this, pQueryDesc));
      return S_OK;
    }
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: {
      *ppQuery = ref(new MTLD3D11ImmediateQuery<BOOL>(this, pQueryDesc));
      return S_OK;
    }
    default:
      return E_INVALIDARG;
    }
  }

  void STDMETHODCALLTYPE
  GetImmediateContext3(ID3D11DeviceContext3 **ppImmediateContext) override{
    context_->QueryInterface(IID_PPV_ARGS(ppImmediateContext));
  }

  HRESULT STDMETHODCALLTYPE CreateDeferredContext3(
    UINT ContextFlags, ID3D11DeviceContext3 **ppDeferredContext) override {
    *ppDeferredContext = std::move(dxmt::CreateDeferredContext(this, ContextFlags));
    return S_OK;
  }

  void STDMETHODCALLTYPE
  WriteToSubresource(
      ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData,
      UINT SrcRowPitch, UINT SrcDepthPitch
  ) override {
    if (!pDstResource || !pSrcData)
      return;

    context_->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData,
                                SrcRowPitch, SrcDepthPitch);
  }

  void STDMETHODCALLTYPE
  ReadFromSubresource(
      void *pDstData, UINT DstRowPitch, UINT DstDepthPitch, ID3D11Resource *SrcResource, UINT SrcSubresource,
      const D3D11_BOX *pSrcBox
  ) override {
    if (!pDstData || !SrcResource)
      return;

    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    SrcResource->GetType(&dimension);

    auto copy_rows = [](void *dst_data, UINT dst_row_pitch,
                        UINT dst_depth_pitch, const void *src_data,
                        UINT src_row_pitch, UINT src_depth_pitch,
                        UINT row_bytes, UINT rows, UINT depth) {
      if (!dst_row_pitch)
        dst_row_pitch = row_bytes;
      if (!dst_depth_pitch)
        dst_depth_pitch = dst_row_pitch * rows;

      for (UINT z = 0; z < depth; z++) {
        auto dst_slice = static_cast<char *>(dst_data) + z * dst_depth_pitch;
        auto src_slice = static_cast<const char *>(src_data) + z * src_depth_pitch;
        for (UINT y = 0; y < rows; y++) {
          std::memcpy(dst_slice + y * dst_row_pitch,
                      src_slice + y * src_row_pitch, row_bytes);
        }
      }
    };

    auto texture_copy_shape = [this](DXGI_FORMAT format, UINT width, UINT height,
                                     UINT *row_bytes, UINT *rows) {
      MTL_DXGI_FORMAT_DESC metal_format;
      if (FAILED(MTLQueryDXGIFormat(GetMTLDevice(), format, metal_format)))
        return false;

      if (metal_format.Flag & MTL_DXGI_FORMAT_BC) {
        *row_bytes = ((width + 3) / 4) * metal_format.BytesPerTexel;
        *rows = (height + 3) / 4;
      } else {
        *row_bytes = width * metal_format.BytesPerTexel;
        *rows = height;
      }
      return true;
    };

    ID3D11Resource *staging = nullptr;
    UINT row_bytes = 0;
    UINT rows = 1;
    UINT depth = 1;

    switch (dimension) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC desc = {};
      static_cast<ID3D11Buffer *>(SrcResource)->GetDesc(&desc);
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      row_bytes = pSrcBox ? pSrcBox->right - pSrcBox->left : desc.ByteWidth;
      if (FAILED(CreateBuffer(&desc, nullptr,
                              reinterpret_cast<ID3D11Buffer **>(&staging))))
        return;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC desc = {};
      static_cast<ID3D11Texture1D *>(SrcResource)->GetDesc(&desc);
      UINT mip = SrcSubresource % desc.MipLevels;
      UINT width = pSrcBox ? pSrcBox->right - pSrcBox->left
                           : std::max(desc.Width >> mip, 1u);
      if (!texture_copy_shape(desc.Format, width, 1, &row_bytes, &rows))
        return;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      if (FAILED(CreateTexture1D(&desc, nullptr,
                                 reinterpret_cast<ID3D11Texture1D **>(&staging))))
        return;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC desc = {};
      static_cast<ID3D11Texture2D *>(SrcResource)->GetDesc(&desc);
      UINT mip = SrcSubresource % desc.MipLevels;
      UINT width = pSrcBox ? pSrcBox->right - pSrcBox->left
                           : std::max(desc.Width >> mip, 1u);
      UINT height = pSrcBox ? pSrcBox->bottom - pSrcBox->top
                            : std::max(desc.Height >> mip, 1u);
      if (!texture_copy_shape(desc.Format, width, height, &row_bytes, &rows))
        return;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      if (FAILED(CreateTexture2D(&desc, nullptr,
                                 reinterpret_cast<ID3D11Texture2D **>(&staging))))
        return;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC desc = {};
      static_cast<ID3D11Texture3D *>(SrcResource)->GetDesc(&desc);
      UINT mip = SrcSubresource % desc.MipLevels;
      UINT width = pSrcBox ? pSrcBox->right - pSrcBox->left
                           : std::max(desc.Width >> mip, 1u);
      UINT height = pSrcBox ? pSrcBox->bottom - pSrcBox->top
                            : std::max(desc.Height >> mip, 1u);
      depth = pSrcBox ? pSrcBox->back - pSrcBox->front
                      : std::max(desc.Depth >> mip, 1u);
      if (!texture_copy_shape(desc.Format, width, height, &row_bytes, &rows))
        return;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      if (FAILED(CreateTexture3D(&desc, nullptr,
                                 reinterpret_cast<ID3D11Texture3D **>(&staging))))
        return;
      break;
    }
    default:
      return;
    }

    if (pSrcBox) {
      context_->CopySubresourceRegion(staging, SrcSubresource, 0, 0, 0,
                                      SrcResource, SrcSubresource, pSrcBox);
    } else {
      context_->CopyResource(staging, SrcResource);
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context_->Map(staging, SrcSubresource, D3D11_MAP_READ, 0,
                                &mapped))) {
      copy_rows(pDstData, DstRowPitch, DstDepthPitch, mapped.pData,
                mapped.RowPitch, mapped.DepthPitch, row_bytes, rows, depth);
      context_->Unmap(staging, SrcSubresource);
    }

    staging->Release();
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return container_->GetMTLDevice();
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return container_->GetLocalD3DKMT();
  }

  HRESULT
  CreateGraphicsPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledGraphicsPipeline **ppPipeline) override {
    pipeline_cache_->GetGraphicsPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  HRESULT
  CreateComputePipeline(MTL_COMPUTE_PIPELINE_DESC *pDesc,
                        MTLCompiledComputePipeline **ppPipeline) override {
    pipeline_cache_->GetComputePipeline(pDesc, ppPipeline);
    return S_OK;
  };

  virtual HRESULT
  CreateGeometryPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledGeometryPipeline **ppPipeline) override {
    pipeline_cache_->GetGeometryPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  HRESULT
  CreateTessellationMeshPipeline(MTL_GRAPHICS_PIPELINE_DESC *pDesc,
                         MTLCompiledTessellationMeshPipeline **ppPipeline) override {
    pipeline_cache_->GetTessellationPipeline(pDesc, ppPipeline);
    return S_OK;
  };

  Device &GetDXMTDevice() override { return device_; };

  void CreateCommandList(ID3D11CommandList** pCommandList) final {
    commandlist_pool_->CreateCommandList(pCommandList);
  };

  virtual void STDMETHODCALLTYPE SetShaderExtensionSlot(UINT Slot) final {
    // TODO
  };

  virtual FormatCapability
  GetMTLPixelFormatCapability(WMTPixelFormat Format) final {
    Format = ORIGINAL_FORMAT(Format);
    if (!format_inspector_.textureCapabilities.contains(Format))
      return FormatCapability(0);
    return format_inspector_.textureCapabilities.at(Format);
  };

  virtual IMTLD3D11DeviceContext *GetImmediateContextPrivate() final {
    return context_.get();
  };

  virtual unsigned int GetDirectXVersion() final {
    return feature_flags_ & 0x80000000 ? 10 : 11;
  };

  virtual HRESULT STDMETHODCALLTYPE RegisterDeviceRemovedEvent(HANDLE Event,
                                                               DWORD *pCookie) final {
    // no device to remove
    return S_OK;
  };

  virtual void STDMETHODCALLTYPE UnregisterDeviceRemoved(DWORD Cookie) final {
    // just do nothing
  };

  virtual HRESULT STDMETHODCALLTYPE OpenSharedFence(HANDLE Handle, REFIID riid,
                                                    void **ppFence) final {
    return dxmt::OpenSharedFence(this, Handle, riid, ppFence);
  };

  virtual HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue,
                                                D3D11_FENCE_FLAG Flags,
                                                REFIID riid, void **ppFence) final {
    fprintf(stderr, "macrunner-dxmt-fence: device CreateFence ENTER InitialValue=%llu Flags=0x%x feature_flags=0x%x\n",
            (unsigned long long)InitialValue, (unsigned)Flags, (unsigned)feature_flags_);
    if (feature_flags_ & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) {
      fprintf(stderr, "macrunner-dxmt-fence: VIDEO_SUPPORT set, degrading to local fence path\n");
      // MacRunner: do not hard-fail here. The inner CreateFence already falls
      // back to a local MTLSharedEvent-backed fence when D3DKMT shared handles
      // are unavailable, which is sufficient for in-process present sync.
    }
    return dxmt::CreateFence(this, InitialValue, Flags, riid, ppFence);
  };

private:
  MTLDXGIObject<IMTLDXGIDevice> *container_;
  D3D_FEATURE_LEVEL feature_level_;
  UINT feature_flags_;
  UINT exception_mode_ = 0;
  MTLD3D11Inspection features_;
  FormatCapabilityInspector format_inspector_;

  bool is_traced_;

  StateObjectCache<D3D11_SAMPLER_DESC, D3D11SamplerState> sampler_states_;
  StateObjectCache<D3D11_RASTERIZER_DESC2, IMTLD3D11RasterizerState> rasterizer_states_;
  StateObjectCache<D3D11_DEPTH_STENCIL_DESC, IMTLD3D11DepthStencilState> depthstencil_states_;

  std::unique_ptr<MTLD3D11CommandListPoolBase> commandlist_pool_;
  std::unique_ptr<MTLD3D11PipelineCacheBase> pipeline_cache_;

  Device& device_;
  /** ensure destructor called first */
  std::unique_ptr<MTLD3D11DeviceContextBase> context_;
  std::unique_ptr<MTLD3D10Device> d3d10_;
  D3D11Multithread d3dmt_;
};

/**
 * \brief D3D11 device container
 *
 * Stores all the objects that contribute to the D3D11
 * device implementation, including the DXGI device.
 */
class MTLD3D11DXGIDevice final : public MTLDXGIObject<IMTLDXGIDevice> {
public:
  friend class MTLDXGIMetalLayerFactory;

  MTLD3D11DXGIDevice(std::unique_ptr<Device> &&device, IMTLDXGIAdapter *adapter,
                     D3D_FEATURE_LEVEL feature_level, UINT feature_flags)
      : adapter_(adapter), device(std::move(device)),
        cmd_queue_(this->device->queue()),
        d3d11_device_(this, adapter, feature_level, feature_flags,
                      *this->device.get()) {
    if (adapter_->GetLocalD3DKMT()) {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = adapter_->GetLocalD3DKMT();
      if (D3DKMTCreateDevice(&create))
        WARN("Failed to create D3DKMT device");
      else
        local_kmt_ = create.hDevice;
    }
  }

  ~MTLD3D11DXGIDevice() {
    if (local_kmt_) {
      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = local_kmt_;
      D3DKMTDestroyDevice(&destroy);
    }
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) ||
        riid == __uuidof(IDXGIDevice2) || riid == __uuidof(IDXGIDevice3) ||
        riid == __uuidof(IMTLDXGIDevice)) {
      *ppvObject = ref(this);
      dxmt_hk_cap_trace_qi("IDXGIDevice-family", S_OK);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1) ||
        riid == __uuidof(ID3D11Device2) || riid == __uuidof(ID3D11Device3) ||
        riid == __uuidof(ID3D11Device4) || riid == __uuidof(ID3D11Device5)) {
      *ppvObject = ref_and_cast<ID3D11Device>(&d3d11_device_);
      dxmt_hk_cap_trace_qi(dxmt_hk_cap_trace_device_iid(riid), S_OK);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10Device) || riid == __uuidof(ID3D10Device1)) {
      *ppvObject = ref_and_cast<ID3D10Device>(d3d11_device_.d3d10_.get());
      return S_OK;
    }

    if (riid == __uuidof(IMTLD3D11DeviceExt)) {
      *ppvObject = ref_and_cast<IMTLD3D11DeviceExt>(&d3d11_device_);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Multithread)) {
      *ppvObject = ref_and_cast<ID3D11Multithread>(&d3d11_device_.d3dmt_);
      dxmt_hk_cap_trace_qi("ID3D11Multithread", S_OK);
      return S_OK;
    }

    if (riid == __uuidof(ID3D11Debug))
      return E_NOINTERFACE;

    if (riid == kRenderdocUUID || riid == kPixUUID || riid == kGpaUUID)
      return E_NOINTERFACE;

    if (logQueryInterfaceError(__uuidof(IMTLDXGIDevice), riid)) {
      WARN("D3D11Device: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return adapter_->QueryInterface(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    if (pAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    *pAdapter = adapter_.ref();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count,
                DXGI_USAGE usage, const DXGI_SHARED_RESOURCE *shared_resource,
                IDXGISurface **surface) override {
    (void)usage;
    (void)shared_resource;

    if (!desc || !surface || !surface_count)
      return E_INVALIDARG;

    for (UINT i = 0; i < surface_count; i++)
      surface[i] = nullptr;

    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  QueryResourceResidency(IUnknown *const *ppResources, DXGI_RESIDENCY *pResidency, UINT ResourceCount) override {
    if (!ppResources || !pResidency)
      return E_INVALIDARG;

    // Basically in DXMT D3D11 implementation, all resources are assumed to be resident.
    for (uint32_t i = 0; i < ResourceCount; i++)
      pResidency[i] = DXGI_RESIDENCY_FULLY_RESIDENT;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override {
    if (Priority < -7 || Priority > 7)
      return E_INVALIDARG;

    WARN("SetGPUThreadPriority: Ignoring");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *pPriority) override {
    *pPriority = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
    cmd_queue_.SetMaxLatency(MaxLatency);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override {
    if (pMaxLatency) {
      *pMaxLatency = cmd_queue_.GetMaxLatency();
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
      OfferResources(UINT NumResources, IDXGIResource *const *ppResources,
                     DXGI_OFFER_RESOURCE_PRIORITY Priority) override {
    if (Priority < DXGI_OFFER_RESOURCE_PRIORITY_LOW ||
        Priority > DXGI_OFFER_RESOURCE_PRIORITY_HIGH)
      return E_INVALIDARG;
    if (NumResources && !ppResources)
      return E_INVALIDARG;

    for (UINT i = 0; i < NumResources; i++) {
      if (!ppResources[i])
        return E_INVALIDARG;
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT NumResources,
                                             IDXGIResource *const *ppResources,
                                             WINBOOL *pDiscarded) override {
    if (NumResources && !ppResources)
      return E_INVALIDARG;

    for (UINT i = 0; i < NumResources; i++) {
      if (!ppResources[i])
        return E_INVALIDARG;
      if (pDiscarded)
        pDiscarded[i] = FALSE;
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE hEvent) override {
    return E_FAIL;
  }

  void STDMETHODCALLTYPE Trim() override { WARN("DXGIDevice3::Trim: no-op"); };

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return adapter_->GetMTLDevice();
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGISwapChain1 **ppSwapChain) override {

    return dxmt::CreateSwapChain(pFactory, &d3d11_device_, hWnd, pDesc, pFullscreenDesc,
                                 ppSwapChain);
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final { return local_kmt_; }

private:
  Com<IMTLDXGIAdapter> adapter_;
  D3DKMT_HANDLE local_kmt_ = 0;
  std::unique_ptr<Device> device;
  CommandQueue &cmd_queue_;
  MTLD3D11DeviceImpl d3d11_device_;
};

Com<IMTLDXGIDevice> CreateD3D11Device(std::unique_ptr<Device> &&device,
                                      IMTLDXGIAdapter *adapter,
                                      D3D_FEATURE_LEVEL feature_level,
                                      UINT feature_flags) {
  return Com<IMTLDXGIDevice>::transfer(new MTLD3D11DXGIDevice(
      std::move(device), adapter, feature_level, feature_flags));
};
} // namespace dxmt
