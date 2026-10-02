#include "com/com_pointer.hpp"
#include "config/config.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "com/com_guid.hpp"
#include "log/log.hpp"
#include "instr.hpp"
#include "util_string.hpp"
#include "wsi_window.hpp"
#include "Metal.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace dxmt {

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config);

static bool dxmt_hk_dxgi_cap_trace_enabled() {
  static int enabled = -1;
  if (enabled < 0) {
    const char *env = std::getenv("MACRUNNER_DXMT_CAP_TRACE");
    enabled = (env && env[0] && env[0] != '0') ? 1 : 0;
  }
  return enabled != 0;
}

static bool dxmt_hk_dxgi_cap_trace_take_slot() {
  static unsigned count = 0;
  static unsigned max_count = 0;
  if (!dxmt_hk_dxgi_cap_trace_enabled())
    return false;
  if (!max_count) {
    const char *env = std::getenv("MACRUNNER_DXMT_CAP_TRACE_MAX");
    char *end = nullptr;
    unsigned long parsed = env && env[0] ? std::strtoul(env, &end, 0) : 0;
    max_count = (end && end != env && parsed > 0 && parsed <= 1000000) ? parsed : 4096;
  }
  return ++count <= max_count;
}

static void dxmt_hk_dxgi_cap_trace(UINT feature, UINT size, HRESULT hr, const void *data) {
  uint64_t q0 = 0;
  if (data && size) {
    UINT copy = size < sizeof(q0) ? size : sizeof(q0);
    std::memcpy(&q0, data, copy);
  }
  if (!dxmt_hk_dxgi_cap_trace_take_slot())
    return;
  std::fprintf(stderr,
               "dxmt-hk-cap: kind=DXGIFactoryCheckFeatureSupport feature=%u "
               "size=%u hr=0x%08x q0=0x%llx\n",
               feature, size, static_cast<unsigned>(hr),
               static_cast<unsigned long long>(q0));
}

static bool dxmt_hk_swap_trace_enabled() {
  static int enabled = -1;
  if (enabled < 0) {
    const char *env = std::getenv("MACRUNNER_DXMT_SWAPCHAIN_TRACE");
    enabled = (env && env[0] && env[0] != '0') ? 1 : 0;
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

static void dxmt_hk_swap_trace_factory(const char *method, HRESULT hr, HWND hwnd,
                                       const DXGI_SWAP_CHAIN_DESC1 *desc,
                                       void *swapchain, void *ret0, void *ret1,
                                       void *ret2, void *ret3) {
  if (!dxmt_hk_swap_trace_take_slot())
    return;
  std::fprintf(stderr,
               "dxmt-hk-swaptrace: kind=Factory method=%s tid=%lu hr=0x%08x "
               "hwnd=%p swapchain=%p w=%u h=%u fmt=%u buffers=%u usage=0x%x "
               "swap_effect=%u flags=0x%x sample_count=%u sample_quality=%u "
               "ret0=%p ret1=%p ret2=%p ret3=%p\n",
               method ? method : "?", static_cast<unsigned long>(GetCurrentThreadId()),
               static_cast<unsigned>(hr), hwnd, swapchain,
               desc ? desc->Width : 0, desc ? desc->Height : 0,
               desc ? desc->Format : 0, desc ? desc->BufferCount : 0,
               desc ? desc->BufferUsage : 0, desc ? desc->SwapEffect : 0,
               desc ? desc->Flags : 0, desc ? desc->SampleDesc.Count : 0,
               desc ? desc->SampleDesc.Quality : 0, ret0, ret1, ret2, ret3);
  std::fflush(stderr);
}

class MTLDXGIFactory : public MTLDXGIObject<IDXGIFactory6> {

public:
  MTLDXGIFactory(UINT Flags) : flags_(Flags) {};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
        riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory2) ||
        riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) ||
        riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIFactory2), riid)) {
      WARN("DXGIFactory: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    InitReturnPtr(ppParent);

    WARN("DXGIFactory::GetParent: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() final {
    // We don't support Stereo 3D at the moment
    return FALSE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    ERR("Software adapters not supported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
                  IDXGISwapChain **ppSwapChain) final {
    void *ret0 = __builtin_return_address(0);
    void *ret1 = __builtin_return_address(1);
    void *ret2 = __builtin_return_address(2);
    void *ret3 = __builtin_return_address(3);
    if (ppSwapChain == nullptr || pDesc == nullptr || pDevice == nullptr) {
      dxmt_hk_swap_trace_factory("CreateSwapChain", DXGI_ERROR_INVALID_CALL, nullptr,
                                 nullptr, nullptr, ret0, ret1, ret2, ret3);
      return DXGI_ERROR_INVALID_CALL;
    }

    DXGI_SWAP_CHAIN_DESC1 desc;
    desc.Width = pDesc->BufferDesc.Width;
    desc.Height = pDesc->BufferDesc.Height;
    desc.Format = pDesc->BufferDesc.Format;
    desc.Stereo = FALSE;
    desc.SampleDesc = pDesc->SampleDesc;
    desc.BufferUsage = pDesc->BufferUsage;
    desc.BufferCount = pDesc->BufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = pDesc->SwapEffect;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = pDesc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC descFs;
    descFs.RefreshRate = pDesc->BufferDesc.RefreshRate;
    descFs.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    descFs.Scaling = pDesc->BufferDesc.Scaling;
    descFs.Windowed = pDesc->Windowed;

    IDXGISwapChain1 *swapChain = nullptr;
    HRESULT hr = CreateSwapChainForHwnd(pDevice, pDesc->OutputWindow, &desc,
                                        &descFs, nullptr, &swapChain);

    *ppSwapChain = swapChain;
    dxmt_hk_swap_trace_factory("CreateSwapChain", hr, pDesc->OutputWindow, &desc,
                               swapChain, ret0, ret1, ret2, ret3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
      IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    void *ret0 = __builtin_return_address(0);
    void *ret1 = __builtin_return_address(1);
    void *ret2 = __builtin_return_address(2);
    void *ret3 = __builtin_return_address(3);
    InitReturnPtr(ppSwapChain);

    if (!ppSwapChain || !pDesc || !hWnd || !pDevice) {
      dxmt_hk_swap_trace_factory("CreateSwapChainForHwnd", DXGI_ERROR_INVALID_CALL,
                                 hWnd, pDesc, nullptr, ret0, ret1, ret2, ret3);
      return DXGI_ERROR_INVALID_CALL;
    }

    Com<IMTLDXGIDevice> metal_dxgi_device;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&metal_dxgi_device)))) {
      ERR("Unsupported device type");
      dxmt_hk_swap_trace_factory("CreateSwapChainForHwnd", DXGI_ERROR_UNSUPPORTED,
                                 hWnd, pDesc, nullptr, ret0, ret1, ret2, ret3);
      return DXGI_ERROR_UNSUPPORTED;
    }

    // Make sure the back buffer size is not zero
    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;

    wsi::getWindowSize(hWnd, desc.Width ? nullptr : &desc.Width,
                       desc.Height ? nullptr : &desc.Height);

    // If necessary, set up a default set of
    // fullscreen parameters for the swap chain
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc;

    if (pFullscreenDesc) {
      fsDesc = *pFullscreenDesc;
    } else {
      fsDesc.RefreshRate = {0, 0};
      fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
      fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
      fsDesc.Windowed = TRUE;
    }

    HRESULT hr = metal_dxgi_device->CreateSwapChain(this, hWnd, &desc, &fsDesc,
                                                    ppSwapChain);
    dxmt_hk_swap_trace_factory("CreateSwapChainForHwnd", hr, hWnd, &desc,
                               ppSwapChain ? *ppSwapChain : nullptr, ret0, ret1,
                               ret2, ret3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
      IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    (void)pWindow;
    (void)pRestrictToOutput;
    if (!ppSwapChain || !pDevice || !pWindow || !pDesc)
      return DXGI_ERROR_INVALID_CALL;

    Com<IMTLDXGIDevice> metal_dxgi_device;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&metal_dxgi_device))))
      return DXGI_ERROR_UNSUPPORTED;

    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
      IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    (void)pRestrictToOutput;
    if (!ppSwapChain || !pDevice || !pDesc)
      return DXGI_ERROR_INVALID_CALL;

    Com<IMTLDXGIDevice> metal_dxgi_device;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&metal_dxgi_device))))
      return DXGI_ERROR_UNSUPPORTED;

    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter,
                                         IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);
    ::dxmt::instr::logf("CALL  IDXGIFactory::EnumAdapters Adapter=%u ppAdapter=%p", (unsigned)Adapter, (void*)ppAdapter);

    if (ppAdapter == nullptr) {
      ::dxmt::instr::logf("RET   IDXGIFactory::EnumAdapters hr=0x%08x (null ppAdapter)", (unsigned)DXGI_ERROR_INVALID_CALL);
      return DXGI_ERROR_INVALID_CALL;
    }

    IDXGIAdapter1 *handle = nullptr;
    HRESULT hr = this->EnumAdapters1(Adapter, &handle);
    *ppAdapter = handle;
    ::dxmt::instr::logf("RET   IDXGIFactory::EnumAdapters hr=0x%08x *ppAdapter=%p", (unsigned)hr, (void*)*ppAdapter);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter,
                                          IDXGIAdapter1 **ppAdapter) final {
    InitReturnPtr(ppAdapter);
    ::dxmt::instr::logf("CALL  IDXGIFactory::EnumAdapters1 Adapter=%u ppAdapter=%p", (unsigned)Adapter, (void*)ppAdapter);

    auto devices = WMT::CopyAllDevices();
    UINT adapter_count = devices.count();
    ::dxmt::instr::logf("      EnumAdapters1 WMT::CopyAllDevices count=%u", (unsigned)adapter_count);

    if (Adapter >= adapter_count) {
      ::dxmt::instr::logf("RET   IDXGIFactory::EnumAdapters1 hr=0x%08x (Adapter>=count, NO ADAPTER)", (unsigned)DXGI_ERROR_NOT_FOUND);
      return DXGI_ERROR_NOT_FOUND;
    }

    UINT adjusted_adapter = Adapter;
    if (adapter_count > 1) {
      UINT preferred_adapter = 0;
      for (unsigned i = 0; i < adapter_count; i++) {
        if (!devices.object(i).hasUnifiedMemory())
          preferred_adapter = i;
      }
      if (Adapter == 0)
        adjusted_adapter = preferred_adapter;
      else
        adjusted_adapter = Adapter <= preferred_adapter ? Adapter - 1 : Adapter;
    }

    auto device = devices.object(adjusted_adapter);

    *ppAdapter = CreateAdapter(device, this, Config::getInstance());
    // devices->release(); // no you should not release it...
    ::dxmt::instr::logf("RET   IDXGIFactory::EnumAdapters1 hr=0x%08x adjusted=%u *ppAdapter=%p", (unsigned)S_OK, (unsigned)adjusted_adapter, (void*)*ppAdapter);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) final {
    if (pWindowHandle == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    *pWindowHandle = associated_window_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource,
                                                         LUID *pLuid) final {
    if (pLuid)
      *pLuid = {};
    if (!hResource || !pLuid)
      return DXGI_ERROR_INVALID_CALL;

    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle,
                                                  UINT Flags) final {
    void *ret0 = __builtin_return_address(0);
    void *ret1 = __builtin_return_address(1);
    void *ret2 = __builtin_return_address(2);
    void *ret3 = __builtin_return_address(3);
    if (Flags) {
      WARN("MakeWindowAssociation: Ignoring flags ", Flags);
    }
    associated_window_ = WindowHandle;
    dxmt_hk_swap_trace_factory("MakeWindowAssociation", S_OK, WindowHandle,
                               nullptr, reinterpret_cast<void *>((uintptr_t)Flags),
                               ret0, ret1, ret2, ret3);
    return S_OK;
  }

  BOOL STDMETHODCALLTYPE IsCurrent() final { return TRUE; }

  HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(
      HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) final {
    (void)wMsg;
    if (pdwCookie)
      *pdwCookie = 0;
    if (!WindowHandle || !pdwCookie)
      return DXGI_ERROR_INVALID_CALL;
    return AllocateStatusCookie(pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent,
                                                      DWORD *pdwCookie) final {
    if (pdwCookie)
      *pdwCookie = 0;
    if (!hEvent || !pdwCookie)
      return DXGI_ERROR_INVALID_CALL;
    return AllocateStatusCookie(pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle,
                                                       UINT wMsg,
                                                       DWORD *pdwCookie) final {
    (void)wMsg;
    if (pdwCookie)
      *pdwCookie = 0;
    if (!WindowHandle || !pdwCookie)
      return DXGI_ERROR_INVALID_CALL;
    return AllocateStatusCookie(pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE
  RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) final {
    if (pdwCookie)
      *pdwCookie = 0;
    if (!hEvent || !pdwCookie)
      return DXGI_ERROR_INVALID_CALL;
    return AllocateStatusCookie(pdwCookie);
  }

  void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) final {
    (void)dwCookie;
  }

  void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) final {
    (void)dwCookie;
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

  HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID iid,
                                              void **adapter) override {
    InitReturnPtr(adapter);

    if (!adapter)
      return DXGI_ERROR_INVALID_CALL;

    for (UINT index = 0;; index++) {
      IDXGIAdapter1 *candidate = nullptr;
      HRESULT hr = EnumAdapters1(index, &candidate);
      if (FAILED(hr))
        return hr;

      DXGI_ADAPTER_DESC1 desc = {};
      hr = candidate->GetDesc1(&desc);
      if (SUCCEEDED(hr) && desc.AdapterLuid.LowPart == luid.LowPart &&
          desc.AdapterLuid.HighPart == luid.HighPart) {
        hr = candidate->QueryInterface(iid, adapter);
        candidate->Release();
        return hr;
      }

      candidate->Release();
    }

    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid,
                                            void **adapter) override {
    InitReturnPtr(adapter);

    if (!adapter)
      return DXGI_ERROR_INVALID_CALL;

    IDXGIAdapter1 *default_adapter = nullptr;
    HRESULT hr = EnumAdapters1(0, &default_adapter);
    if (FAILED(hr))
      return hr;

    hr = default_adapter->QueryInterface(iid, adapter);
    default_adapter->Release();
    return hr;
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    case DXGI_FEATURE_PRESENT_ALLOW_TEARING: {
      auto info = static_cast<BOOL *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info)) {
        dxmt_hk_dxgi_cap_trace(Feature, FeatureSupportDataSize, E_INVALIDARG,
                               pFeatureSupportData);
        return E_INVALIDARG;
      }

      *info = TRUE;
      dxmt_hk_dxgi_cap_trace(Feature, FeatureSupportDataSize, S_OK,
                             pFeatureSupportData);
      return S_OK;
    }
    default: {
      ERR("DXGIFactory::CheckFeatureSupport: unknown feature ", Feature);
      dxmt_hk_dxgi_cap_trace(Feature, FeatureSupportDataSize, E_INVALIDARG,
                             pFeatureSupportData);
      return E_INVALIDARG;
    }
    }
  };

  HRESULT STDMETHODCALLTYPE
  EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                             REFIID riid, void **ppvAdapter) override {
    // GpuPreference ignored, since Apple Silicon has only 1 GPU anyway
    // FIXME: support Intel Mac with dedicated GPU
    Com<IDXGIAdapter1> adapter;
    HRESULT hr = this->EnumAdapters1(Adapter, &adapter);

    if (FAILED(hr))
      return hr;
    return adapter->QueryInterface(riid, ppvAdapter);
  };

private:
  HRESULT AllocateStatusCookie(DWORD *cookie) {
    *cookie = next_status_cookie_++;
    if (!*cookie)
      *cookie = next_status_cookie_++;
    return S_OK;
  }

  UINT flags_;

  HWND associated_window_ = nullptr;
  DWORD next_status_cookie_ = 1;
};

extern "C" HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid,
                                                void **ppFactory) {
  ::dxmt::instr::logf("CALL  CreateDXGIFactory2 Flags=%u riid(data1=0x%08x) ppFactory=%p", (unsigned)Flags, (unsigned)riid.Data1, (void*)ppFactory);
  try {
    MTLDXGIFactory* factory = new MTLDXGIFactory(Flags);
    HRESULT hr = factory->QueryInterface(riid, ppFactory);
    factory->Release();

    if (FAILED(hr)) {
      ::dxmt::instr::logf("RET   CreateDXGIFactory2 hr=0x%08x (QueryInterface FAILED)", (unsigned)hr);
      return hr;
    }

    ::dxmt::instr::logf("RET   CreateDXGIFactory2 hr=0x%08x *ppFactory=%p", (unsigned)S_OK, ppFactory ? (void*)*ppFactory : (void*)0);
    return S_OK;
  } catch (const MTLD3DError &e) {
    ::dxmt::instr::logf("RET   CreateDXGIFactory2 hr=0x%08x (MTLD3DError exception)", (unsigned)E_FAIL);
    Logger::err(e.message());
    return E_FAIL;
  }
}

extern "C" HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  ::dxmt::instr::logf("CALL  CreateDXGIFactory1 (delegates to CreateDXGIFactory2)");
  HRESULT hr = CreateDXGIFactory2(0, riid, ppFactory);
  ::dxmt::instr::logf("RET   CreateDXGIFactory1 hr=0x%08x *ppFactory=%p", (unsigned)hr, ppFactory ? (void*)*ppFactory : (void*)0);
  return hr;
}

extern "C" HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **factory) {
  ::dxmt::instr::logf("CALL  CreateDXGIFactory (delegates to CreateDXGIFactory2)");
  HRESULT hr = CreateDXGIFactory2(0, riid, factory);
  ::dxmt::instr::logf("RET   CreateDXGIFactory hr=0x%08x *factory=%p", (unsigned)hr, factory ? (void*)*factory : (void*)0);
  return hr;
}

} // namespace dxmt
