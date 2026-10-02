#include "d3d11_swapchain.hpp"
#include "com/com_guid.hpp"
#include "config/config.hpp"
#include "d3d11_private.h"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "dxgi_output.hpp"
#include "d3d11_context.hpp"
#include "d3d11_drawtrace_totals.hpp"
#include "dxmt_context.hpp"
#include "dxmt_hud_state.hpp"
#include "dxmt_statistics.hpp"
#include "dxmt_presenter.hpp"
#include "log/log.hpp"
#include "d3d11_resource.hpp"
#include "d3d11_device.hpp"
#include "util_cpu_fence.hpp"
#include "util_env.hpp"
#include "util_error.hpp"
#include "util_string.hpp"
#include "util_win32_compat.h"
#include "wsi_monitor.hpp"
#include "wsi_platform_win32.hpp"
#include "wsi_window.hpp"
#include "dxmt_info.hpp"
#include "dxmt_presenter.hpp"
#include <atomic>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

/**
Ref: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency
This value is 1 by default.
*/
constexpr size_t kSwapchainLatency = 1;

namespace dxmt {

static bool dxmt_hk_swap_trace_enabled() {
  static int enabled = []() {
    const char *env = std::getenv("MACRUNNER_DXMT_SWAPCHAIN_TRACE");
    return env && env[0] && std::strcmp(env, "0") != 0;
  }();
  return enabled != 0;
}

static bool dxmt_hk_swap_trace_take_slot() {
  if (!dxmt_hk_swap_trace_enabled())
    return false;

  static std::atomic<uint64_t> counter {0};
  static uint64_t max_lines = []() {
    const char *env = std::getenv("MACRUNNER_DXMT_SWAPCHAIN_TRACE_MAX");
    if (!env || !env[0])
      return UINT64_C(4096);
    char *end = nullptr;
    uint64_t parsed = std::strtoull(env, &end, 0);
    if (end == env || parsed == 0)
      return UINT64_C(4096);
    if (parsed > UINT64_C(1000000))
      return UINT64_C(1000000);
    return parsed;
  }();

  return counter.fetch_add(1, std::memory_order_relaxed) < max_lines;
}

struct DxmtFrameDumpRequest {
  bool selected = false;
  uint64_t frame = 0;
};

static DxmtFrameDumpRequest dxmt_frame_dump_take_slot() {
  static const bool enabled = []() {
    const char *env = std::getenv("MACRUNNER_DXMT_FRAME_DUMP");
    return env && env[0] && std::strcmp(env, "0") != 0;
  }();
  static std::atomic<uint64_t> present_count {0};

  if (!enabled)
    return {};

  const uint64_t frame = present_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool selected = frame <= 5 || (frame % 200) == 0;
  if (selected)
    std::fprintf(stderr, "dxmt-frame-dump: frame=%llu stage=requested\n",
                 static_cast<unsigned long long>(frame));
  return {selected, frame};
}

static const char *dxmt_hk_swap_trace_iid_name(REFIID riid) {
  if (riid == __uuidof(IUnknown))
    return "IUnknown";
  if (riid == __uuidof(IDXGIObject))
    return "IDXGIObject";
  if (riid == __uuidof(IDXGIDeviceSubObject))
    return "IDXGIDeviceSubObject";
  if (riid == __uuidof(IDXGISwapChain))
    return "IDXGISwapChain";
  if (riid == __uuidof(IDXGISwapChain1))
    return "IDXGISwapChain1";
  if (riid == __uuidof(IDXGISwapChain2))
    return "IDXGISwapChain2";
  if (riid == __uuidof(IDXGISwapChain3))
    return "IDXGISwapChain3";
  if (riid == __uuidof(IDXGISwapChain4))
    return "IDXGISwapChain4";
  if (riid == __uuidof(ID3D11Resource))
    return "ID3D11Resource";
  if (riid == __uuidof(ID3D11Texture2D))
    return "ID3D11Texture2D";
  if (riid == __uuidof(ID3D11Texture2D1))
    return "ID3D11Texture2D1";
  return "other";
}

static void dxmt_hk_swap_trace_method(const char *method, const void *self,
                                      HRESULT hr, uint64_t arg0, uint64_t arg1,
                                      const char *iid, const void *out,
                                      const void *ret0, const void *ret1,
                                      const void *ret2, const void *ret3) {
  if (!dxmt_hk_swap_trace_take_slot())
    return;

  std::fprintf(
      stderr,
      "dxmt-hk-swaptrace: kind=SwapChain method=%s tid=%lu this=%p hr=0x%08x "
      "arg0=0x%llx arg1=0x%llx iid=%s out=%p ret0=%p ret1=%p ret2=%p ret3=%p\n",
      method, (unsigned long)GetCurrentThreadId(), self,
      (unsigned)static_cast<uint32_t>(hr), (unsigned long long)arg0,
      (unsigned long long)arg1, iid ? iid : "-", out, ret0, ret1, ret2, ret3);
}

static void dxmt_hk_swap_trace_handle(const char *method, const void *self,
                                      HANDLE handle, uint64_t arg0,
                                      const void *ret0, const void *ret1,
                                      const void *ret2, const void *ret3) {
  if (!dxmt_hk_swap_trace_take_slot())
    return;

  std::fprintf(
      stderr,
      "dxmt-hk-swaptrace: kind=SwapChain method=%s tid=%lu this=%p handle=%p "
      "arg0=0x%llx ret0=%p ret1=%p ret2=%p ret3=%p\n",
      method, (unsigned long)GetCurrentThreadId(), self, handle,
      (unsigned long long)arg0, ret0, ret1, ret2, ret3);
}

WMTPixelFormat ConvertSwapChainFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return WMTPixelFormatBGRA8Unorm_sRGB;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return WMTPixelFormatBGRA8Unorm;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTPixelFormatRGB10A2Unorm;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return WMTPixelFormatRGBA16Float;
  default:
    return WMTPixelFormatInvalid;
  }
}

WMTColorSpace ConvertColorSpace(DXGI_COLOR_SPACE_TYPE color_space, bool hdr) {
  switch (color_space) {
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    return WMTColorSpaceSRGB;
  case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    return hdr ? WMTColorSpaceHDR_scRGB : WMTColorSpaceSRGBLinear;
  case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    return WMTColorSpaceHDR_PQ;
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
    return WMTColorSpaceBT2020;
  default:
    return WMTColorSpaceInvalid;
  }
}

/**
 FIXME: duplicated implementation in dxgi_output.cpp
*/
uint32_t
GetMonitorFormatBpp(DXGI_FORMAT Format) {
  switch (Format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    return 32;

  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return 64;

  default:
    Logger::warn(str::format("GetMonitorFormatBpp: Unknown format: ", Format));
    return 32;
  }
}

class ModeSetGuard {
  std::atomic_flag in_progress_;
public:
  class ModeSetInProgress {
    ModeSetGuard &guard_;
    bool protected_;

  public:
    ModeSetInProgress(ModeSetGuard &guard) : guard_(guard) {
      protected_ = guard_.in_progress_.test_and_set();
    }
    ~ModeSetInProgress() {
      if (!protected_) {
        guard_.in_progress_.clear();
      }
    }
    operator bool() {
      return protected_;
    }
  };
};

template <bool EnableMetalFX>
class MTLD3D11SwapChain final : public MTLDXGISubObject<IDXGISwapChain4, MTLD3D11Device> {
public:
  MTLD3D11SwapChain(
      IDXGIFactory1 *pFactory, MTLD3D11Device *pDevice, IMTLDXGIDevice *pDXGIDevice,
       HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc
  ) :
      MTLDXGISubObject(pDevice),
      factory_(pFactory),
      dxgi_device_(pDXGIDevice),
      presentation_count_(0),
      desc_(*pDesc),
      device_context_(pDevice->GetImmediateContextPrivate()),
      hWnd(hWnd),
      monitor_(wsi::getWindowMonitor(hWnd)),
      hud(WMT::DeveloperHUDProperties::instance()) {

    native_view_ = WMT::CreateMetalViewFromHWND((intptr_t)hWnd, pDevice->GetMTLDevice(), layer_weak_);

    if (!native_view_) {
      ERR("Failed to create metal view, it seems like your Wine has no exported symbols needed by DXMT.");
      abort();
    }

    if constexpr (EnableMetalFX) {
      scale_factor = std::max(Config::getInstance().getOption<float>("d3d11.metalSpatialUpscaleFactor", 2), 1.0f);
    }

    presenter = Rc(new Presenter(pDevice->GetMTLDevice(), layer_weak_,
                                 pDevice->GetDXMTDevice().queue().cmd_library,
                                 scale_factor, desc_.SampleDesc.Count));

    frame_latency = kSwapchainLatency;
    present_semaphore_ = CreateSemaphore(nullptr, frame_latency,
                                         DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);

    // without this flag, there is still a DXGIDevice level of frame latency control
    if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
      frame_latency_fence_ = std::make_unique<CpuFence>();
    }

    if (desc_.Width == 0 || desc_.Height == 0) {
      wsi::getWindowSize(hWnd, &desc_.Width, &desc_.Height);
    }
    source_width_ = desc_.Width;
    source_height_ = desc_.Height;

    if (pFullscreenDesc) {
      fullscreen_desc_ = *pFullscreenDesc;
    } else {
      fullscreen_desc_.Windowed = true;
    }

    preferred_max_frame_rate =
        Config::getInstance().getOption<int>("d3d11.preferredMaxFrameRate", 0);
    wsi::WsiMode current_mode;
    if (wsi::getCurrentDisplayMode(monitor_, &current_mode) &&
        current_mode.refreshRate.denominator != 0 &&
        current_mode.refreshRate.numerator != 0) {
      init_refresh_rate_ = (double)current_mode.refreshRate.numerator /
                           (double)current_mode.refreshRate.denominator;
    }

    handle_alt_tab_ = Config::getInstance().getOption<bool>("dxgi.handleAltTab", false);

    hud.initialize(GetVersionDescriptionText(device_->GetDirectXVersion(), device_->GetFeatureLevel()));

    backbuffer_desc_ = D3D11_TEXTURE2D_DESC1 {
      .Width = desc_.Width,
      .Height = desc_.Height,
      .MipLevels = 1,
      .ArraySize = 1,
      .Format = desc_.Format,
      .SampleDesc = desc_.SampleDesc,
      .Usage = D3D11_USAGE_DEFAULT,
      .BindFlags = D3D11_BIND_RENDER_TARGET,
      .CPUAccessFlags = {},
      .MiscFlags = {},
      .TextureLayout = {},
    };
    
    if (desc_.BufferUsage & DXGI_USAGE_SHADER_INPUT)
      backbuffer_desc_.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      backbuffer_desc_.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = ResizeBuffers(0, desc_.Width, desc_.Height, DXGI_FORMAT_UNKNOWN, desc_.Flags);
    if (FAILED(hr)) {
      // ResizeBuffers failed to allocate the backbuffer, so backbuffer_ is null.
      // Letting the ctor return would hand out a half-built swapchain whose
      // GetBuffer()/Present1() dereference the null backbuffer. Fail construction
      // instead. The dtor does NOT run for a throwing ctor, so release the
      // manually-managed handles here (same order as ~MTLD3D11SwapChain) before
      // throwing; RAII members (presenter, fences, backbuffer_) clean themselves up.
      ERR("MTLD3D11SwapChain: failed to allocate backbuffer (hr 0x", std::hex, (unsigned)hr, ")");
      WMT::ReleaseMetalView(native_view_);
      native_view_ = {};
      CloseHandle(present_semaphore_);
      throw MTLD3DError("MTLD3D11SwapChain: failed to allocate backbuffer");
    }
    if (!fullscreen_desc_.Windowed)
      EnterFullscreenMode(nullptr);
  };

  ~MTLD3D11SwapChain() {
    device_context_->WaitUntilGPUIdle();
    WMT::ReleaseMetalView(native_view_);
    native_view_ = {};
    CloseHandle(present_semaphore_);
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    const char *iid = dxmt_hk_swap_trace_iid_name(riid);
    if (ppvObject == nullptr) {
      dxmt_hk_swap_trace_method("QueryInterface", this, E_POINTER, 0, 0, iid,
                                nullptr, ret0, ret1, ret2, ret3);
      return E_POINTER;
    }

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
        riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) ||
        riid == __uuidof(IDXGISwapChain4)) {
      *ppvObject = ref(this);
      dxmt_hk_swap_trace_method("QueryInterface", this, S_OK, 0, 0, iid,
                                *ppvObject, ret0, ret1, ret2, ret3);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGISwapChain1), riid)) {
      WARN("DXGISwapChain: Unknown interface query ", str::format(riid));
    }

    dxmt_hk_swap_trace_method("QueryInterface", this, E_NOINTERFACE, 0, 0, iid,
                              nullptr, ret0, ret1, ret2, ret3);
    return E_NOINTERFACE;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetParent(REFIID riid, void **parent) final {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    HRESULT hr = factory_->QueryInterface(riid, parent);
    dxmt_hk_swap_trace_method("GetParent", this, hr, 0, 0,
                              dxmt_hk_swap_trace_iid_name(riid),
                              parent ? *parent : nullptr, ret0, ret1, ret2,
                              ret3);
    return hr;
  };

  HRESULT
  STDMETHODCALLTYPE
  Present(UINT sync_interval, UINT flags) final {
    dxmt_hk_drawtrace_record(DXMTHKDrawTraceCounter::Present);
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    HRESULT hr = Present1(sync_interval, flags, nullptr);
    dxmt_hk_swap_trace_method("Present", this, hr, sync_interval, flags, "-",
                              nullptr, ret0, ret1, ret2, ret3);
    return hr;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetBuffer(UINT buffer_idx, REFIID riid, void **surface) final {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    const char *iid = dxmt_hk_swap_trace_iid_name(riid);
    InitReturnPtr(surface);
    UINT exposed_buffer_count = desc_.BufferCount ? desc_.BufferCount : 1;
    if (buffer_idx < exposed_buffer_count) {
      HRESULT hr = backbuffer_->QueryInterface(riid, surface);
      dxmt_hk_swap_trace_method("GetBuffer", this, hr, buffer_idx,
                                exposed_buffer_count, iid,
                                surface ? *surface : nullptr, ret0, ret1,
                                ret2, ret3);
      return hr;
    } else {
      dxmt_hk_swap_trace_method("GetBuffer", this, DXGI_ERROR_INVALID_CALL,
                                buffer_idx, exposed_buffer_count, iid, nullptr,
                                ret0, ret1, ret2, ret3);
      return DXGI_ERROR_INVALID_CALL;
    }
  };

  HRESULT
  STDMETHODCALLTYPE
  SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) final {
    Com<IDXGIOutput1> target;

    if (pTarget) {
      DXGI_OUTPUT_DESC desc;

      pTarget->QueryInterface(IID_PPV_ARGS(&target));
      target->GetDesc(&desc);

      if (!fullscreen_desc_.Windowed && Fullscreen && monitor_ != desc.Monitor) {
        HRESULT hr = LeaveFullscreenMode();
        if (FAILED(hr))
          return hr;
      }
    }

    if (fullscreen_desc_.Windowed && Fullscreen)
      return EnterFullscreenMode(target.ptr());
    else if (!fullscreen_desc_.Windowed && !Fullscreen)
      return LeaveFullscreenMode();

    return S_OK;
  };

  HRESULT EnterFullscreenMode(IDXGIOutput1* pTarget) {
    ModeSetGuard::ModeSetInProgress modeset_inprogress(modeset_guard_);
    if (modeset_inprogress)
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

    Com<IDXGIOutput1> output = pTarget;

    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    
    if (output == nullptr) {
      if (FAILED(GetOutputFromMonitor(wsi::getWindowMonitor(hWnd), &output))) {
       ERR("DXGI: EnterFullscreenMode: Cannot query containing output");
        return E_FAIL;
      }
    }

    std::unique_lock<dxmt::mutex> lock(mutex_);

    DXGI_MODE_DESC1 preferred_display_mode = {
        desc_.Width,
        desc_.Height,
        fullscreen_desc_.RefreshRate,
        desc_.Format,
        DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
        DXGI_MODE_SCALING_UNSPECIFIED
    };
    if (FAILED(ChangeDisplayMode(output.ptr(), &preferred_display_mode))) {
      ERR("DXGI: EnterFullscreenMode: Failed to change display mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    // Update swap chain description
    fullscreen_desc_.Windowed = FALSE;
    
    // Move the window so that it covers the entire output
    bool modeSwitch = (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0u;

    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);

    monitor_ = desc.Monitor;
    target_  = std::move(output);

    lock = {};

    if (!wsi::enterFullscreenMode(desc.Monitor, hWnd, &window_state_, modeSwitch)) {
      ERR("DXGI: EnterFullscreenMode: Failed to enter fullscreen mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    return S_OK;
  }
  
  
  HRESULT LeaveFullscreenMode() {
    ModeSetGuard::ModeSetInProgress modeset_inprogress(modeset_guard_);
    if (modeset_inprogress)
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

    std::lock_guard<dxmt::mutex> lock(mutex_);

    if (FAILED(RestoreDisplayMode(monitor_)))
      WARN("DXGI: LeaveFullscreenMode: Failed to restore display mode");

    // Restore internal state
    fullscreen_desc_.Windowed = TRUE;
    target_  = nullptr;
    monitor_ = wsi::getWindowMonitor(hWnd);
    
    if (!wsi::isWindow(hWnd))
      return S_OK;
    
    if (!wsi::leaveFullscreenMode(hWnd, &window_state_, true)) {
      ERR("DXGI: LeaveFullscreenMode: Failed to exit fullscreen mode");
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    
    presenter->changeGammaRamp(nullptr);

    return S_OK;
  }

  HRESULT
  ChangeDisplayMode(IDXGIOutput1 *pOutput, DXGI_MODE_DESC1 *pDisplayMode) {
    if (!pOutput)
      return DXGI_ERROR_INVALID_CALL;

    // Find a mode that the output supports

    DXGI_MODE_DESC1 preferred_mode = *pDisplayMode;
    DXGI_MODE_DESC1 selected_mode = {};

    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)) {
      preferred_mode.Width = 0;
      preferred_mode.Height = 0;
    }

    if (preferred_mode.Format == DXGI_FORMAT_UNKNOWN)
      preferred_mode.Format = desc_.Format;

    HRESULT hr = pOutput->FindClosestMatchingMode1(&preferred_mode, &selected_mode, nullptr);

    if (FAILED(hr)) {
      ERR("DXGI: Failed to query closest mode:"
          "\n"
          "  Format: ",
          preferred_mode.Format,
          "\n"
          "  Mode:   ",
          preferred_mode.Width, "x", preferred_mode.Height, "@",
          preferred_mode.RefreshRate.Numerator / std::max(preferred_mode.RefreshRate.Denominator, 1u));
      return hr;
    }

    if (!selected_mode.RefreshRate.Denominator)
      selected_mode.RefreshRate.Denominator = 1;

    DXGI_OUTPUT_DESC output_desc;
    pOutput->GetDesc(&output_desc);
    wsi::WsiMode wsi_mode{
        selected_mode.Width,
        selected_mode.Height,
        {selected_mode.RefreshRate.Numerator, selected_mode.RefreshRate.Denominator},
        GetMonitorFormatBpp(selected_mode.Format),
        selected_mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST ||
            selected_mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST
    };
    if (!wsi::setWindowMode(output_desc.Monitor, hWnd, wsi_mode))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    *pDisplayMode = selected_mode;
    init_refresh_rate_ = double(selected_mode.RefreshRate.Numerator) / double(selected_mode.RefreshRate.Denominator);
    return S_OK;
  }

  HRESULT
  RestoreDisplayMode(HMONITOR hMonitor) {
    if (!hMonitor)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::restoreDisplayMode(hMonitor))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    init_refresh_rate_ = DBL_MAX;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) final {
    HRESULT hr = S_OK;

    if (handle_alt_tab_ && !fullscreen_desc_.Windowed && !wsi::isForeground(hWnd))
      SetFullscreenState(FALSE, nullptr);

    if (pFullscreen != nullptr)
      *pFullscreen = !fullscreen_desc_.Windowed;
    
    if (ppTarget != nullptr)
      *ppTarget = target_.ref();

    return hr;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) final {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    if (!pDesc) {
      dxmt_hk_swap_trace_method("GetDesc", this, E_INVALIDARG, 0, 0,
                                "DXGI_SWAP_CHAIN_DESC", nullptr, ret0, ret1,
                                ret2, ret3);
      return E_INVALIDARG;
    }

    pDesc->BufferDesc.Width = desc_.Width;
    pDesc->BufferDesc.Height = desc_.Height;
    pDesc->BufferDesc.RefreshRate = fullscreen_desc_.RefreshRate;
    pDesc->BufferDesc.Format = desc_.Format;
    pDesc->BufferDesc.ScanlineOrdering = fullscreen_desc_.ScanlineOrdering;
    pDesc->BufferDesc.Scaling = fullscreen_desc_.Scaling;
    pDesc->SampleDesc = desc_.SampleDesc;
    pDesc->BufferUsage = desc_.BufferUsage;
    pDesc->BufferCount = desc_.BufferCount;
    pDesc->OutputWindow = hWnd;
    pDesc->Windowed = fullscreen_desc_.Windowed;
    pDesc->SwapEffect = desc_.SwapEffect;
    pDesc->Flags = desc_.Flags;
    dxmt_hk_swap_trace_method("GetDesc", this, S_OK, desc_.Width, desc_.Height,
                              "DXGI_SWAP_CHAIN_DESC", pDesc, ret0, ret1, ret2,
                              ret3);
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                UINT flags) final {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    /* BufferCount ignored */
    if (Width == 0 || Height == 0) {
      wsi::getWindowSize(hWnd, &desc_.Width, &desc_.Height);
    } else {
      desc_.Width = Width;
      desc_.Height = Height;
    }
    if (Format != DXGI_FORMAT_UNKNOWN) {
      if (ConvertSwapChainFormat(Format) != WMTPixelFormatInvalid)
        desc_.Format = Format;
    }

    backbuffer_ = nullptr;
    if (desc_.Width == 0 || desc_.Height == 0) {
      backbuffer_desc_.Width = 1;
      backbuffer_desc_.Height = 1;
    } else {
      backbuffer_desc_.Width = desc_.Width;
      backbuffer_desc_.Height = desc_.Height;
    }
    if (source_width_ == 0 || source_width_ > desc_.Width)
      source_width_ = desc_.Width;
    if (source_height_ == 0 || source_height_ > desc_.Height)
      source_height_ = desc_.Height;

    ApplyLayerProps();

    backbuffer_desc_.Format = desc_.Format;

    backbuffer_ = nullptr;
    if (FAILED(dxmt::CreateDeviceTexture2D(
            device_, &backbuffer_desc_, nullptr, reinterpret_cast<ID3D11Texture2D1 **>(&backbuffer_)
        ))) {
      dxmt_hk_swap_trace_method("ResizeBuffers", this, E_FAIL, desc_.Width,
                                desc_.Height, "CreateDeviceTexture2D", nullptr,
                                ret0, ret1, ret2, ret3);
      return E_FAIL;
    }
    // CreateDeviceTexture2D returns public reference, change to private one here
    backbuffer_->AddRefPrivate();
    backbuffer_->Release();

    if constexpr (EnableMetalFX) {
      D3D11_TEXTURE2D_DESC1 upscaled_desc_ = backbuffer_desc_;
      upscaled_desc_.Height *= scale_factor;
      upscaled_desc_.Width *= scale_factor;
      upscaled_backbuffer_ = nullptr;
      if (FAILED(dxmt::CreateDeviceTexture2D(
              device_, &upscaled_desc_, nullptr, reinterpret_cast<ID3D11Texture2D1 **>(&upscaled_backbuffer_)
          ))) {
        dxmt_hk_swap_trace_method("ResizeBuffers", this, E_FAIL, desc_.Width,
                                  desc_.Height, "CreateDeviceTexture2D.upscaled",
                                  nullptr, ret0, ret1, ret2, ret3);
        return E_FAIL;
      }

      WMTFXSpatialScalerInfo info;
      info.input_height = desc_.Height;
      info.input_width = desc_.Width;
      info.output_height = desc_.Height * scale_factor;
      info.output_width = desc_.Width * scale_factor;
      info.color_format = backbuffer_->texture()->pixelFormat();
      info.output_format =upscaled_backbuffer_->texture()->pixelFormat();
      metalfx_scaler = new SpatialScaler(device_->GetMTLDevice(), info);
      D3D11_ASSERT(metalfx_scaler && "otherwise metalfx failed to initialize");
    }

    dxmt_hk_swap_trace_method("ResizeBuffers", this, S_OK, desc_.Width,
                              desc_.Height, "backbuffer", backbuffer_.ptr(),
                              ret0, ret1, ret2, ret3);
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  ResizeTarget(const DXGI_MODE_DESC *pDesc) final {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    if (!pDesc) {
      dxmt_hk_swap_trace_method("ResizeTarget", this, DXGI_ERROR_INVALID_CALL,
                                0, 0, "DXGI_MODE_DESC", nullptr, ret0, ret1,
                                ret2, ret3);
      return DXGI_ERROR_INVALID_CALL;
    }

    if (!wsi::isWindow(hWnd)) {
      dxmt_hk_swap_trace_method("ResizeTarget", this, DXGI_ERROR_INVALID_CALL,
                                pDesc->Width, pDesc->Height, "DXGI_MODE_DESC",
                                nullptr, ret0, ret1, ret2, ret3);
      return DXGI_ERROR_INVALID_CALL;
    }

    std::unique_lock<dxmt::mutex> lock(mutex_);

    // Promote display mode
    DXGI_MODE_DESC1 newDisplayMode = {};
    newDisplayMode.Width = pDesc->Width;
    newDisplayMode.Height = pDesc->Height;
    newDisplayMode.RefreshRate = pDesc->RefreshRate;
    newDisplayMode.Format = pDesc->Format;
    newDisplayMode.ScanlineOrdering = pDesc->ScanlineOrdering;
    newDisplayMode.Scaling = pDesc->Scaling;

    // Update the swap chain description
    if (newDisplayMode.RefreshRate.Numerator != 0)
      fullscreen_desc_.RefreshRate = newDisplayMode.RefreshRate;

    fullscreen_desc_.ScanlineOrdering = newDisplayMode.ScanlineOrdering;
    fullscreen_desc_.Scaling = newDisplayMode.Scaling;

    if (fullscreen_desc_.Windowed) {
      wsi::resizeWindow(hWnd, &window_state_, newDisplayMode.Width,
                        newDisplayMode.Height);
    } else {
      ChangeDisplayMode(target_.ptr(), &newDisplayMode);
      lock = {};
      wsi::updateFullscreenWindow(monitor_, hWnd, false);
    }

    dxmt_hk_swap_trace_method("ResizeTarget", this, S_OK, pDesc->Width,
                              pDesc->Height, "DXGI_MODE_DESC", nullptr, ret0,
                              ret1, ret2, ret3);
    return S_OK;
  };

  void ApplyLayerProps() {
    auto target_color_space =
        ConvertColorSpace(desc_.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                              ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                              : colorspace_,
                          LayerSupportEDR());
    if (presenter->changeLayerProperties(
            ConvertSwapChainFormat(desc_.Format), target_color_space, desc_.Width * scale_factor,
            desc_.Height * scale_factor, desc_.SampleDesc.Count
        ))
      device_context_->WaitUntilGPUIdle();
  };

  HRESULT GetOutputFromMonitor(
          HMONITOR                  Monitor,
          IDXGIOutput1**            ppOutput) {
    if (!ppOutput)
      return DXGI_ERROR_INVALID_CALL;

    Com<IDXGIAdapter> adapter;
    Com<IDXGIOutput> output;

    if (FAILED(dxgi_device_->GetAdapter(&adapter)))
      return E_FAIL;

    for (uint32_t i = 0; SUCCEEDED(adapter->EnumOutputs(i, &output)); i++) {
      DXGI_OUTPUT_DESC outputDesc;
      output->GetDesc(&outputDesc);
      
      if (outputDesc.Monitor == Monitor)
        return output->QueryInterface(IID_PPV_ARGS(ppOutput));
      
      output = nullptr;
    }
    
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetContainingOutput(IDXGIOutput **ppOutput) final {
    InitReturnPtr(ppOutput);
    
    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;
    
    Com<IDXGIOutput1> output;

    if (target_ == nullptr) {
      HRESULT hr = GetOutputFromMonitor(wsi::getWindowMonitor(hWnd), &output);

      if (FAILED(hr))
        return hr;
    } else {
      output = target_;
    }

    *ppOutput = output.ref();
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) final {
    DEBUG("DXGISwapChain::GetFrameStatistics: stub");
    std::unique_lock<d3d11_device_mutex> lock(device_->mutex);
    stats->PresentCount = presentation_count_;
    stats->SyncRefreshCount = presentation_count_;
    stats->PresentRefreshCount = presentation_count_;
    stats->SyncGPUTime = {};
    stats->SyncQPCTime = {};
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetLastPresentCount(UINT *last_present_count) final {
    if (last_present_count == NULL) {
      return E_POINTER;
    }
    std::unique_lock<d3d11_device_mutex> lock(device_->mutex);
    *last_present_count = presentation_count_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) final {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    if (pDesc == NULL) {
      dxmt_hk_swap_trace_method("GetDesc1", this, E_POINTER, 0, 0,
                                "DXGI_SWAP_CHAIN_DESC1", nullptr, ret0, ret1,
                                ret2, ret3);
      return E_POINTER;
    }
    *pDesc = desc_;
    dxmt_hk_swap_trace_method("GetDesc1", this, S_OK, desc_.Width,
                              desc_.Height, "DXGI_SWAP_CHAIN_DESC1", pDesc,
                              ret0, ret1, ret2, ret3);
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) final {
    if (pDesc == NULL) {
      return E_POINTER;
    }
    *pDesc = fullscreen_desc_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetHwnd(HWND *pHwnd) final {
    if (pHwnd == NULL) {
      return E_POINTER;
    }
    *pHwnd = hWnd;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetCoreWindow(REFIID refiid, void **ppUnk) final {
    (void)refiid;
    InitReturnPtr(ppUnk);
    return DXGI_ERROR_INVALID_CALL;
  };

  class SyncFrameState {
    uint64_t frame_id_ = 0;
    CpuFence *frame_latency_fence_ = nullptr;

  public:
    SyncFrameState() {}
    SyncFrameState(uint64_t frame_id, CpuFence *frame_latency_fence)
        : frame_id_(frame_id), frame_latency_fence_(frame_latency_fence) {}

    SyncFrameState(const SyncFrameState &) = delete;
    SyncFrameState(SyncFrameState &&move) {
      frame_id_ = move.frame_id_;
      frame_latency_fence_ = move.frame_latency_fence_;
      move.frame_latency_fence_ = nullptr;
    };
    ~SyncFrameState() {
      if (frame_latency_fence_) {
        frame_latency_fence_->signal(frame_id_);
        frame_latency_fence_ = nullptr;
      }
    }
  };

  SyncFrameState SyncFrame(uint64_t current_frame_id) {
    if (frame_latency_fence_) {
      if (current_frame_id > frame_latency)
        frame_latency_fence_->wait(current_frame_id - frame_latency);
      return SyncFrameState(current_frame_id, frame_latency_fence_.get());
    }
    return SyncFrameState();
  };

  HRESULT
  STDMETHODCALLTYPE
  Present1(UINT SyncInterval, UINT PresentFlags,
           const DXGI_PRESENT_PARAMETERS *pPresentParameters) final {
    dxmt_hk_drawtrace_record(DXMTHKDrawTraceCounter::Present1);
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    if (SyncInterval > 4) {
      dxmt_hk_swap_trace_method("Present1", this, DXGI_ERROR_INVALID_CALL,
                                SyncInterval, PresentFlags, "-", nullptr, ret0,
                                ret1, ret2, ret3);
      return DXGI_ERROR_INVALID_CALL;
    }

    HRESULT hr = S_OK;
    bool window_minimized = wsi::isMinimized(hWnd);
    if ((window_minimized || desc_.Width == 0 || desc_.Height == 0)
        // MSDN: You will not receive DXGI_STATUS_OCCLUDED if you're using a flip model swap chain.
        && desc_.SwapEffect <= DXGI_SWAP_EFFECT_SEQUENTIAL)
      hr = DXGI_STATUS_OCCLUDED;
    bool should_exit_fs = handle_alt_tab_ // At the moment this is still broken for certain games
                          && !fullscreen_desc_.Windowed && !window_minimized && !wsi::isForeground(hWnd);
    if (hr == S_OK && should_exit_fs)
      hr = DXGI_STATUS_OCCLUDED;
    if (PresentFlags & DXGI_PRESENT_TEST) {
      dxmt_hk_swap_trace_method("Present1", this, hr, SyncInterval,
                                PresentFlags, "DXGI_PRESENT_TEST", nullptr,
                                ret0, ret1, ret2, ret3);
      return hr;
    }

    if (should_exit_fs)
      SetFullscreenState(FALSE, nullptr);

    std::unique_lock<d3d11_device_mutex> lock(device_->mutex);

    device_context_->PrepareFlush();
    if (hr == DXGI_STATUS_OCCLUDED) {
      // flush commands without presenting
      device_context_->Commit();
      dxmt_hk_swap_trace_method("Present1", this, hr, SyncInterval,
                                PresentFlags, "DXGI_STATUS_OCCLUDED", nullptr,
                                ret0, ret1, ret2, ret3);
      return hr;
    }

    double vsync_duration =
        std::max(SyncInterval * 1.0 /
                     (preferred_max_frame_rate ? preferred_max_frame_rate
                                               : init_refresh_rate_),
                 preferred_max_frame_rate ? 1.0 / preferred_max_frame_rate : 0);

    auto &cmd_queue = device_->GetDXMTDevice().queue();
    auto chunk = cmd_queue.CurrentChunk();
    chunk->signal_frame_latency_fence_ = cmd_queue.CurrentFrameSeq();
    if (target_) {
      auto output = static_cast<MTLDXGIOutput *>(target_.ptr());
      presenter->changeGammaRamp(output->GetGammaRamp());
    }
    const auto frame_dump = dxmt_frame_dump_take_slot();
    if constexpr (EnableMetalFX) {
      chunk->emitcc([
        this, vsync_duration, backbuffer = backbuffer_->texture(),
        sync_state = SyncFrame(++presentation_count_),
        upscaled = upscaled_backbuffer_->texture(),
        scaler = this->metalfx_scaler, state = presenter->synchronizeLayerProperties(),
        frame_dump
      ](ArgumentEncodingContext &ctx) mutable {
        auto &scaler_info = ctx.currentFrameStatistics().last_scaler_info;
        scaler_info.type = ScalerType::Spatial;
        scaler_info.input_width = backbuffer->width();
        scaler_info.input_height = backbuffer->height();
        scaler_info.output_width = upscaled->width();
        scaler_info.output_height = upscaled->height();
        ctx.upscale(backbuffer, upscaled, scaler);
        ctx.present(upscaled, presenter, vsync_duration, state.metadata,
                    frame_dump.selected, frame_dump.frame);
        ReleaseSemaphore(present_semaphore_, 1, nullptr);
        this->UpdateStatistics(ctx.queue().statistics, ctx.currentFrameId());
      });
    } else {
      chunk->emitcc([
        this, vsync_duration, state = presenter->synchronizeLayerProperties(),
        sync_state = SyncFrame(++presentation_count_),
        backbuffer = backbuffer_->texture(), frame_dump
      ](ArgumentEncodingContext &ctx) mutable {
        ctx.present(backbuffer, presenter, vsync_duration, state.metadata,
                    frame_dump.selected, frame_dump.frame);
        ReleaseSemaphore(present_semaphore_, 1, nullptr);
        this->UpdateStatistics(ctx.queue().statistics, ctx.currentFrameId());
      });
    }
    device_context_->Commit();

    lock.unlock(); // since PresentBoundary() will and should only stall current thread

    cmd_queue.PresentBoundary();

    dxmt_hk_swap_trace_method("Present1", this, hr, SyncInterval, PresentFlags,
                              "-", nullptr, ret0, ret1, ret2, ret3);
    return hr;
  };

  void UpdateStatistics(const FrameStatisticsContainer& statistics, uint64_t frame_id) {
    hud.begin();
    auto &frame = statistics.at(frame_id - 1); // show the previous one frame statistics
    auto &average = statistics.average();
    Flags<FeatureCompatibility> flags = frame.compatibility_flags;
    char text[] = "---------------------------";
    if (flags.test(FeatureCompatibility::UnsupportedGeometryDraw)) {
      text[3] = 'G';
    }
    if (flags.test(FeatureCompatibility::UnsupportedTessellationOutputPrimitive)) {
      text[5] = 'T';
      text[6] = 'O';
    }
    if (flags.test(FeatureCompatibility::UnsupportedIndirectTessellationDraw)) {
      text[8] = 'I';
      text[9] = 'T';
    }
    if (flags.test(FeatureCompatibility::UnsupportedGeometryTessellationDraw)) {
      text[11] = 'G';
      text[12] = 'T';
    }
    if (flags.test(FeatureCompatibility::UnsupportedDrawAuto)) {
      text[14] = 'A';
    }
    if (flags.test(FeatureCompatibility::UnsupportedPredication)) {
      text[16] = 'P';
    }
    if (flags.test(FeatureCompatibility::UnsupportedStreamOutputAppending)) {
      text[18] = 'S';
      text[19] = 'A';
    }
    if (flags.test(FeatureCompatibility::UnsupportedMultipleStreamOutput)) {
      text[21] = 'M';
      text[22] = 'S';
    }
    hud.printLine(text);
    hud.printLine(std::format(
        "Commit: {:2} -{:4.1f} -{:4.1f}", std::min(frame.command_buffer_count, 99u),
        std::min(average.commit_interval.count() / 1000000.0, 99.9),
        std::min(statistics.max().commit_interval.count() / 1000000.0, 99.9)
    ));
    hud.printLine(std::format(
        "Sync:   {:2} {:4.1f}  {:2} {:4.1f} {:2}", std::min(frame.sync_count, 99u),
        std::min(average.sync_interval.count() / 1000000.0, 99.9), std::min(statistics.max().event_stall, 99u),
        std::min(average.present_latency_interval.count() / 1000000.0, 99.9), frame.latency
    ));
    hud.printLine(std::format(
        "Encode: {:4.1f}+{:4.1f}+{:4.1f}={:4.1f}", std::min(average.encode_prepare_interval.count() / 1000000.0, 99.9),
        std::min((average.encode_flush_interval - average.drawable_blocking_interval).count() / 1000000.0, 99.9),
        std::min(average.drawable_blocking_interval.count() / 1000000.0, 99.9),
        std::min((average.encode_prepare_interval + average.encode_flush_interval).count() / 1000000.0, 99.9)
    ));
    hud.printLine(std::format(
        "Render:{:3}+{:<3} Clear:{:3}+{:<2}", std::min(frame.render_pass_count - frame.render_pass_optimized, 999u),
        std::min(frame.render_pass_optimized, 999u),
        std::min(frame.clear_pass_count - frame.clear_pass_optimized, 999u), std::min(frame.clear_pass_optimized, 99u)
    ));
    {
      /* scaler info */
      auto &info = frame.last_scaler_info;
      if (info.type == ScalerType::Temporal) {
        auto &info = frame.last_scaler_info;
        hud.printLine(std::format(
            "MetalFX: Temporal {} {}", info.auto_exposure ? "AEXP" : "", info.motion_vector_highres ? "HMV" : ""
        ));
        hud.printLine(std::format(
            "Scale: {:4}x{:4}->{:4}x{:4}", info.input_width, info.input_height, info.output_width, info.output_height
        ));
      }
      if (info.type == ScalerType::Spatial) {
        hud.printLine(std::format(
            "MetalFX: Spatial"
        ));
        hud.printLine(std::format(
            "Scale: {:4}x{:4}->{:4}x{:4}", info.input_width, info.input_height, info.output_width, info.output_height
        ));
      }
    }
    hud.end();
  }

  BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() final { return FALSE; };

  HRESULT
  STDMETHODCALLTYPE
  GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) final {
    InitReturnPtr(ppRestrictToOutput);
    if (!ppRestrictToOutput)
      return DXGI_ERROR_INVALID_CALL;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  SetBackgroundColor(const DXGI_RGBA *pColor) final {
    if (!pColor)
      return E_INVALIDARG;
    background_color_ = *pColor;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetBackgroundColor(DXGI_RGBA *pColor) final {
    if (!pColor)
      return E_INVALIDARG;
    *pColor = background_color_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  SetRotation(DXGI_MODE_ROTATION Rotation) final {
    switch (Rotation) {
    case DXGI_MODE_ROTATION_IDENTITY:
    case DXGI_MODE_ROTATION_ROTATE90:
    case DXGI_MODE_ROTATION_ROTATE180:
    case DXGI_MODE_ROTATION_ROTATE270:
      rotation_ = Rotation;
      return S_OK;
    default:
      return DXGI_ERROR_INVALID_CALL;
    }
  };

  HRESULT
  STDMETHODCALLTYPE
  GetRotation(DXGI_MODE_ROTATION *pRotation) final {
    if (!pRotation)
      return E_INVALIDARG;
    *pRotation = rotation_;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  SetSourceSize(UINT Width, UINT Height) override {
    if (Width == 0 || Height == 0 || Width > desc_.Width ||
        Height > desc_.Height)
      return DXGI_ERROR_INVALID_CALL;

    source_width_ = Width;
    source_height_ = Height;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetSourceSize(UINT *pWidth, UINT *pHeight) override {
    if (pWidth)
      *pWidth = source_width_;
    if (pHeight)
      *pHeight = source_height_;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    if (max_latency == 0 || max_latency > DXGI_MAX_SWAP_CHAIN_BUFFERS) {
      dxmt_hk_swap_trace_method("SetMaximumFrameLatency", this, E_INVALIDARG,
                                max_latency, frame_latency, "-", nullptr, ret0,
                                ret1, ret2, ret3);
      return E_INVALIDARG;
    }
    if (max_latency > frame_latency) {
      ReleaseSemaphore(present_semaphore_, max_latency - frame_latency,
                       nullptr);
    }
    frame_latency = max_latency;

    dxmt_hk_swap_trace_method("SetMaximumFrameLatency", this, S_OK,
                              max_latency, frame_latency, "-", nullptr, ret0,
                              ret1, ret2, ret3);
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    if (max_latency) {
      *max_latency = frame_latency;
    }
    dxmt_hk_swap_trace_method("GetMaximumFrameLatency", this, S_OK,
                              frame_latency, 0, "-", max_latency, ret0, ret1,
                              ret2, ret3);
    return S_OK;
  };

  HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
      dxmt_hk_swap_trace_handle("GetFrameLatencyWaitableObject", this, nullptr,
                                desc_.Flags, ret0, ret1, ret2, ret3);
      return nullptr;
    }

    HANDLE result = nullptr;
    HANDLE processHandle = GetCurrentProcess();

    if (!DuplicateHandle(processHandle, present_semaphore_, processHandle,
                         &result, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      dxmt_hk_swap_trace_handle("GetFrameLatencyWaitableObject", this, nullptr,
                                desc_.Flags, ret0, ret1, ret2, ret3);
      return nullptr;
    }

    dxmt_hk_swap_trace_handle("GetFrameLatencyWaitableObject", this, result,
                              desc_.Flags, ret0, ret1, ret2, ret3);
    return result;
  };

  HRESULT STDMETHODCALLTYPE
  SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override {
    if (!matrix)
      return DXGI_ERROR_INVALID_CALL;
    matrix_transform_ = *matrix;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override {
    if (!matrix)
      return DXGI_ERROR_INVALID_CALL;
    *matrix = matrix_transform_;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(
      DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override {
    if (!pColorSpaceSupport)
      return E_INVALIDARG;
    *pColorSpaceSupport = CGColorSpace_checkColorSpaceSupported(ConvertColorSpace(ColorSpace, false))
            ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
            : 0;
    return S_OK;
  };

  UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
    // TODO(swapchain): can be non-zero once sequential swapchain is implemented
    return 0;
  }

  HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                          DXGI_FORMAT Format, UINT SwapChainFlags,
                          const UINT *pCreationNodeMask,
                          IUnknown *const *ppPresentQueue) override {
    (void)pCreationNodeMask;
    (void)ppPresentQueue;
    const void *ret0 = __builtin_return_address(0);
    const void *ret1 = __builtin_return_address(1);
    const void *ret2 = __builtin_return_address(2);
    const void *ret3 = __builtin_return_address(3);
    HRESULT hr = ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
    dxmt_hk_swap_trace_method("ResizeBuffers1", this, hr, Width, Height, "-",
                              nullptr, ret0, ret1, ret2, ret3);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override {
    auto target_color_space = ConvertColorSpace(ColorSpace, LayerSupportEDR());
    if (presenter->changeLayerColorSpace(target_color_space))
      device_context_->WaitUntilGPUIdle();
    colorspace_ = ColorSpace;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type,
                                           UINT Size,
                                           void *pMetaData) override {
    if (Type == DXGI_HDR_METADATA_TYPE_NONE) {
      presenter->changeHDRMetadata(nullptr);
      return S_OK;
    }
    if (Type == DXGI_HDR_METADATA_TYPE_HDR10) {
      if (!pMetaData || Size != sizeof(DXGI_HDR_METADATA_HDR10))
        return E_INVALIDARG;
      const auto *dxgi_metadata =
          reinterpret_cast<const DXGI_HDR_METADATA_HDR10 *>(pMetaData);
      WMTHDRMetadata metadata = {};
      metadata.red_primary[0] = dxgi_metadata->RedPrimary[0];
      metadata.red_primary[1] = dxgi_metadata->RedPrimary[1];
      metadata.green_primary[0] = dxgi_metadata->GreenPrimary[0];
      metadata.green_primary[1] = dxgi_metadata->GreenPrimary[1];
      metadata.blue_primary[0] = dxgi_metadata->BluePrimary[0];
      metadata.blue_primary[1] = dxgi_metadata->BluePrimary[1];
      metadata.white_point[0] = dxgi_metadata->WhitePoint[0];
      metadata.white_point[1] = dxgi_metadata->WhitePoint[1];
      metadata.max_mastering_luminance = dxgi_metadata->MaxMasteringLuminance;
      metadata.min_mastering_luminance = dxgi_metadata->MinMasteringLuminance;
      metadata.max_content_light_level = dxgi_metadata->MaxContentLightLevel;
      metadata.max_frame_average_light_level =
          dxgi_metadata->MaxFrameAverageLightLevel;
      presenter->changeHDRMetadata(&metadata);
      return S_OK;
    }
    return DXGI_ERROR_UNSUPPORTED;
  }

private:
  bool LayerSupportEDR() {
    WMTEDRValue edr_value;
    MetalLayer_getEDRValue(layer_weak_, &edr_value);
    return edr_value.maximum_potential_edr_color_component_value > 1.0f;
  };

  Com<IDXGIFactory1> factory_;
  Com<IMTLDXGIDevice> dxgi_device_;
  WMT::Object native_view_;
  WMT::MetalLayer layer_weak_;
  ULONG presentation_count_;
  DXGI_SWAP_CHAIN_DESC1 desc_;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc_;
  D3D11_TEXTURE2D_DESC1 backbuffer_desc_;
  UINT source_width_ = 0;
  UINT source_height_ = 0;
  DXGI_MATRIX_3X2_F matrix_transform_ = {1.0f, 0.0f, 0.0f,
                                         1.0f, 0.0f, 0.0f};
  DXGI_RGBA background_color_ = {0.0f, 0.0f, 0.0f, 0.0f};
  DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
  IMTLD3D11DeviceContext* device_context_;
  Com<D3D11ResourceCommon, false> backbuffer_;
  HANDLE present_semaphore_;
  std::unique_ptr<CpuFence> frame_latency_fence_;
  HWND hWnd;
  HMONITOR monitor_;
  Com<IDXGIOutput1> target_;
  wsi::DXMTWindowState window_state_;
  uint32_t frame_latency;
  DXGI_COLOR_SPACE_TYPE colorspace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  double init_refresh_rate_ = DBL_MAX;
  int preferred_max_frame_rate = 0;
  HUDState hud;
  Rc<Presenter> presenter;
  ModeSetGuard modeset_guard_;
  dxmt::mutex mutex_;

  bool handle_alt_tab_;

  std::conditional<EnableMetalFX, Rc<SpatialScaler>, std::monostate>::type metalfx_scaler;
  std::conditional<EnableMetalFX, Com<D3D11ResourceCommon>, std::monostate>::type upscaled_backbuffer_;
  float scale_factor = 1.0;
};

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D11Device *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, IDXGISwapChain1 **ppSwapChain
) {
  if (pDesc == NULL)
    return DXGI_ERROR_INVALID_CALL;
  if (ppSwapChain == NULL)
    return DXGI_ERROR_INVALID_CALL;
  InitReturnPtr(ppSwapChain);

  DWORD window_process_id;
  GetWindowThreadProcessId(hWnd, &window_process_id);
  if (GetProcessId(GetCurrentProcess()) != window_process_id) {
    ERR("CreateSwapChain: cross-process swapchain not supported yet");
    return E_FAIL;
  }

  Com<IMTLDXGIDevice> layer_factory;
  if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&layer_factory)))) {
    ERR("CreateSwapChain: failed to get IMTLDXGIDevice");
    return E_FAIL;
  }
  if ((pDesc->SwapEffect != DXGI_SWAP_EFFECT_DISCARD &&
       pDesc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD) &&
      pDesc->BufferCount != 1) {
    WARN("CreateSwapChain: unsupported swap effect ", pDesc->SwapEffect, " with backbuffer size ", pDesc->BufferCount);
  }
  try {
    if (env::getEnvVar("DXMT_METALFX_SPATIAL_SWAPCHAIN") == "1") {
      if (pDevice->GetMTLDevice().supportsFXSpatialScaler()) {
        *ppSwapChain = new MTLD3D11SwapChain<true>(
            pFactory, pDevice, layer_factory.ptr(), hWnd, pDesc, pFullscreenDesc
        );
        return S_OK;
      } else {
        WARN("MetalFX spatial scaler is not supported on this device");
      }
    }
    *ppSwapChain = new MTLD3D11SwapChain<false>(
        pFactory, pDevice, layer_factory.ptr(), hWnd, pDesc, pFullscreenDesc
    );
    return S_OK;
  } catch (const MTLD3DError &err) {
    // A throwing ctor frees its own storage; *ppSwapChain stays the null set by
    // InitReturnPtr above, so the caller gets a clean error, not a half-built object.
    ERR(err.message());
    return E_FAIL;
  }
};

} // namespace dxmt
