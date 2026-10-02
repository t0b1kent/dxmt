/*
 * Copyright 2026 Feifan He for CodeWeavers
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

#include "dxmt_names.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "config/config.hpp"
#include "d3d11_device.hpp"
#include "d3d11_drawtrace_totals.hpp"
#include "d3d11_vscb_dump.hpp"
#include "log/log.hpp"
#include "instr.hpp"
#include "util_string.hpp"
#include "dxmt_capture.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace dxmt {
Logger Logger::s_instance("d3d11.log");

namespace {

constexpr uint64_t kDXMTHKDrawTraceTotalsPeriod = 1024;
std::atomic<uint64_t> g_dxmt_hk_drawtrace_counts[
    static_cast<size_t>(DXMTHKDrawTraceCounter::Count)] {};
std::atomic<uint64_t> g_dxmt_hk_drawtrace_events {0};

uint64_t dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter counter) {
  return g_dxmt_hk_drawtrace_counts[static_cast<size_t>(counter)].load(
      std::memory_order_relaxed);
}

} // namespace

bool dxmt_hk_drawtrace_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("MACRUNNER_DXMT_SWAPCHAIN_TRACE");
    return env && env[0] && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

void dxmt_hk_drawtrace_emit_totals(const char *reason) {
  if (!dxmt_hk_drawtrace_enabled())
    return;

  std::fprintf(
      stderr,
      "dxmt-drawtrace-totals: draw=%llu drawindexed=%llu drawinstanced=%llu "
      "drawindexedinstanced=%llu clear=%llu omset=%llu present=%llu present1=%llu reason=%s\\n",
      static_cast<unsigned long long>(dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter::Draw)),
      static_cast<unsigned long long>(dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter::DrawIndexed)),
      static_cast<unsigned long long>(dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter::DrawInstanced)),
      static_cast<unsigned long long>(dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter::DrawIndexedInstanced)),
      static_cast<unsigned long long>(dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter::ClearRenderTargetView)),
      static_cast<unsigned long long>(dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter::OMSetRenderTargets)),
      static_cast<unsigned long long>(dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter::Present)),
      static_cast<unsigned long long>(dxmt_hk_drawtrace_total(DXMTHKDrawTraceCounter::Present1)),
      reason ? reason : "unspecified");
  std::fflush(stderr);
}

void dxmt_hk_drawtrace_record(DXMTHKDrawTraceCounter counter) {
  if (!dxmt_hk_drawtrace_enabled())
    return;

  g_dxmt_hk_drawtrace_counts[static_cast<size_t>(counter)].fetch_add(
      1, std::memory_order_relaxed);
  const uint64_t event_count = g_dxmt_hk_drawtrace_events.fetch_add(
      1, std::memory_order_relaxed) + 1;
  if (event_count % kDXMTHKDrawTraceTotalsPeriod == 0)
    dxmt_hk_drawtrace_emit_totals("periodic");
}

/*
 * MACRUNNER_DXMT_VS_CB_DUMP — Hollow Knight degenerate-transform probe.
 * Counters live in this single TU (d3d11_context_impl.cpp is included by two
 * TUs, so statics there would double-count or split the totals).
 * Aggregate counts are never capped; only per-buffer detail lines are bounded
 * by MACRUNNER_DXMT_VS_CB_DUMP_MAX.
 */
namespace {

constexpr uint64_t kDXMTHKVSCBDumpTotalsPeriod = 4096;
std::atomic<uint64_t> g_dxmt_hk_vscb_draws{0};
std::atomic<uint64_t> g_dxmt_hk_vscb_buffers{0};
std::atomic<uint64_t> g_dxmt_hk_vscb_unreadable{0};
std::atomic<uint64_t> g_dxmt_hk_vscb_class_counts[DXMT_HK_VSCB_CLASS_COUNT]{};
std::atomic<uint64_t> g_dxmt_hk_vscb_detail_logged{0};

unsigned dxmt_hk_vscb_dump_detail_limit() {
  static const unsigned limit = [] {
    const char *env = std::getenv("MACRUNNER_DXMT_VS_CB_DUMP_MAX");
    char *end = nullptr;
    unsigned long parsed = env && env[0] ? std::strtoul(env, &end, 0) : 0;
    return (end && end != env && parsed > 0 && parsed <= 1000000) ? unsigned(parsed) : 256u;
  }();
  return limit;
}

} // namespace

bool dxmt_hk_vscb_dump_enabled() {
  static const bool enabled = [] {
    const char *env = std::getenv("MACRUNNER_DXMT_VS_CB_DUMP");
    return env && env[0] && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

unsigned long long dxmt_hk_vscb_dump_next_draw_ordinal() {
  const uint64_t ordinal = g_dxmt_hk_vscb_draws.fetch_add(1, std::memory_order_relaxed) + 1;
  if (ordinal % kDXMTHKVSCBDumpTotalsPeriod == 0)
    dxmt_hk_vscb_dump_emit_totals("periodic");
  return ordinal;
}

bool dxmt_hk_vscb_dump_take_detail_slot() {
  return g_dxmt_hk_vscb_detail_logged.fetch_add(1, std::memory_order_relaxed) <
         dxmt_hk_vscb_dump_detail_limit();
}

void dxmt_hk_vscb_dump_record_buffer(unsigned n_matrices, const uint32_t *classes, bool readable) {
  g_dxmt_hk_vscb_buffers.fetch_add(1, std::memory_order_relaxed);
  if (!readable) {
    g_dxmt_hk_vscb_unreadable.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  for (unsigned i = 0; i < n_matrices && i < 4; i++) {
    if (classes[i] < DXMT_HK_VSCB_CLASS_COUNT)
      g_dxmt_hk_vscb_class_counts[classes[i]].fetch_add(1, std::memory_order_relaxed);
  }
}

void dxmt_hk_vscb_dump_emit_totals(const char *reason) {
  if (!dxmt_hk_vscb_dump_enabled())
    return;

  std::fprintf(
      stderr,
      "dxmt-vscb-totals: draws_sampled=%llu buffers=%llu unreadable=%llu "
      "identity=%llu zero=%llu nan_or_inf=%llu degenerate=%llu plausible=%llu "
      "detail_logged=%llu reason=%s\n",
      (unsigned long long)g_dxmt_hk_vscb_draws.load(std::memory_order_relaxed),
      (unsigned long long)g_dxmt_hk_vscb_buffers.load(std::memory_order_relaxed),
      (unsigned long long)g_dxmt_hk_vscb_unreadable.load(std::memory_order_relaxed),
      (unsigned long long)g_dxmt_hk_vscb_class_counts[DXMT_HK_VSCB_CLASS_IDENTITY].load(std::memory_order_relaxed),
      (unsigned long long)g_dxmt_hk_vscb_class_counts[DXMT_HK_VSCB_CLASS_ZERO].load(std::memory_order_relaxed),
      (unsigned long long)g_dxmt_hk_vscb_class_counts[DXMT_HK_VSCB_CLASS_NAN_OR_INF].load(std::memory_order_relaxed),
      (unsigned long long)g_dxmt_hk_vscb_class_counts[DXMT_HK_VSCB_CLASS_DEGENERATE].load(std::memory_order_relaxed),
      (unsigned long long)g_dxmt_hk_vscb_class_counts[DXMT_HK_VSCB_CLASS_PLAUSIBLE].load(std::memory_order_relaxed),
      (unsigned long long)g_dxmt_hk_vscb_detail_logged.load(std::memory_order_relaxed),
      reason ? reason : "unspecified");
  std::fflush(stderr);
}

extern "C" HRESULT WINAPI
D3D11CoreCreateDevice(IDXGIFactory *pFactory, IDXGIAdapter *pAdapter,
                      UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
                      UINT FeatureLevels, ID3D11Device **ppDevice) {
  InitReturnPtr(ppDevice);
  ::dxmt::instr::logf("CALL  D3D11CoreCreateDevice pFactory=%p pAdapter=%p Flags=0x%08x FeatureLevels=%u ppDevice=%p", (void*)pFactory, (void*)pAdapter, (unsigned)Flags, (unsigned)FeatureLevels, (void*)ppDevice);

  Com<IMTLDXGIAdapter> dxgi_adapter;

  // Try to find the corresponding Metal device for the DXGI adapter
  if (FAILED(pAdapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter)))) {
    ::dxmt::instr::logf("RET   D3D11CoreCreateDevice hr=0x%08x (Not a DXMT adapter)", (unsigned)E_INVALIDARG);
    ERR("Not a DXMT adapter");
    return E_INVALIDARG;
  }

  // Feature levels to probe if the
  // application does not specify any.
  std::array<D3D_FEATURE_LEVEL, 6> defaultFeatureLevels = {
      D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1,
  };

  if (!pFeatureLevels || !FeatureLevels) {
    pFeatureLevels = defaultFeatureLevels.data();
    FeatureLevels = defaultFeatureLevels.size();
  }

  D3D_FEATURE_LEVEL maxFeatureLevel =
      dxgi_adapter->GetMTLDevice().supportsFamily(WMTGPUFamilyApple7) ? D3D_FEATURE_LEVEL_11_1 : D3D_FEATURE_LEVEL_11_0;
  D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL();
  D3D_FEATURE_LEVEL devFeatureLevel = D3D_FEATURE_LEVEL();

  if (auto fl_override = Config::getInstance().getOption<std::string>("d3d11.maxFeatureLevel", "");
      fl_override != "") {
    static const std::unordered_map<std::string, D3D_FEATURE_LEVEL> s_feature_levels = {{
        {"12_1", D3D_FEATURE_LEVEL_12_1},
        {"12_0", D3D_FEATURE_LEVEL_12_0},
        {"11_1", D3D_FEATURE_LEVEL_11_1},
        {"11_0", D3D_FEATURE_LEVEL_11_0},
        {"10_1", D3D_FEATURE_LEVEL_10_1},
        {"10_0", D3D_FEATURE_LEVEL_10_0},
        {"9_3", D3D_FEATURE_LEVEL_9_3},
        {"9_2", D3D_FEATURE_LEVEL_9_2},
        {"9_1", D3D_FEATURE_LEVEL_9_1},
    }};
    if (auto iter = s_feature_levels.find(fl_override); iter != s_feature_levels.end()) {
      maxFeatureLevel = iter->second;
    }
  }

  Logger::info(
      str::format("Maximum supported feature level: ", maxFeatureLevel));

  for (uint32_t flId = 0; flId < FeatureLevels; flId++) {
    minFeatureLevel = pFeatureLevels[flId];

    if (minFeatureLevel <= maxFeatureLevel) {
      devFeatureLevel = minFeatureLevel;
      break;
    }
  }

  if (!devFeatureLevel) {
    Logger::err(str::format("Minimum required feature level ", minFeatureLevel,
                            " not supported"));
    return E_INVALIDARG;
  }

  try {
    Logger::info(str::format("Using feature level ", devFeatureLevel));
    ::dxmt::instr::logf("      D3D11CoreCreateDevice creating device, featureLevel=0x%08x", (unsigned)devFeatureLevel);

    auto device = CreateD3D11Device(
        CreateDXMTDevice({.device = dxgi_adapter->GetMTLDevice()}),
        dxgi_adapter.ptr(), devFeatureLevel, Flags);

    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(ppDevice));
    ::dxmt::instr::logf("RET   D3D11CoreCreateDevice hr=0x%08x *ppDevice=%p", (unsigned)hr, ppDevice ? (void*)*ppDevice : (void*)0);
    return hr;
  } catch (const MTLD3DError &e) {
    ::dxmt::instr::logf("RET   D3D11CoreCreateDevice hr=0x%08x (MTLD3DError exception)", (unsigned)E_FAIL);
    Logger::err("D3D11CoreCreateDevice: Failed to create D3D11 device");
    return E_FAIL;
  }
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {
  InitReturnPtr(ppDevice);
  InitReturnPtr(ppSwapChain);
  InitReturnPtr(ppImmediateContext);
  ::dxmt::instr::logf("CALL  D3D11CreateDeviceAndSwapChain pAdapter=%p DriverType=%u Flags=0x%08x FeatureLevels=%u pSwapChainDesc=%p ppSwapChain=%p ppDevice=%p", (void*)pAdapter, (unsigned)DriverType, (unsigned)Flags, (unsigned)FeatureLevels, (void*)pSwapChainDesc, (void*)ppSwapChain, (void*)ppDevice);

  if (pFeatureLevel)
    *pFeatureLevel = D3D_FEATURE_LEVEL(0);

  Com<IDXGIFactory> dxgiFactory = nullptr;
  Com<IDXGIAdapter> dxgiAdapter = pAdapter;
  Com<ID3D11Device> device = nullptr;

  HRESULT hr;

  if (ppSwapChain && !pSwapChainDesc) {
    ::dxmt::instr::logf("RET   D3D11CreateDeviceAndSwapChain hr=0x%08x (ppSwapChain&&!pSwapChainDesc)", (unsigned)E_INVALIDARG);
    return E_INVALIDARG;
  }

  if (!pAdapter) {
    // Ignore DriverType
    if (DriverType != D3D_DRIVER_TYPE_HARDWARE)
      WARN("D3D11CreateDevice: Unsupported driver type ", DriverType);
    ::dxmt::instr::logf("      D3D11CreateDevice: pAdapter==NULL, creating DXGI factory via CreateDXGIFactory1");
    // We'll use the first adapter returned by a DXGI factory
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));

    if (FAILED(hr)) {
      ::dxmt::instr::logf("RET   D3D11CreateDeviceAndSwapChain hr=0x%08x (CreateDXGIFactory1 FAILED)", (unsigned)hr);
      Logger::err("D3D11CreateDevice: Failed to create a DXGI factory");
      return hr;
    }

    hr = dxgiFactory->EnumAdapters(0, &dxgiAdapter);

    if (FAILED(hr)) {
      ::dxmt::instr::logf("RET   D3D11CreateDeviceAndSwapChain hr=0x%08x (EnumAdapters(0) FAILED - no adapter)", (unsigned)hr);
      Logger::err("D3D11CreateDevice: No default adapter available");
      return hr;
    }
    ::dxmt::instr::logf("      D3D11CreateDevice: EnumAdapters(0) OK dxgiAdapter=%p", (void*)dxgiAdapter.ptr());
  } else {
    // We should be able to query the DXGI factory from the adapter
    if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) {
      Logger::err(
          "D3D11CreateDevice: Failed to query DXGI factory from DXGI adapter");
      return E_INVALIDARG;
    }

    // In theory we could ignore these, but the Microsoft docs explicitly
    // state that we need to return E_INVALIDARG in case the arguments are
    // invalid. Both the driver type and software parameter can only be
    // set if the adapter itself is unspecified.
    // See:
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ff476082(v=vs.85).aspx
    if (DriverType != D3D_DRIVER_TYPE_UNKNOWN || Software)
      return E_INVALIDARG;
  }
  // Create the actual device
  hr = D3D11CoreCreateDevice(dxgiFactory.ptr(), dxgiAdapter.ptr(), Flags,
                             pFeatureLevels, FeatureLevels, &device);

  if (FAILED(hr))
    return hr;

  // Create the swap chain, if requested
  if (ppSwapChain) {
    DXGI_SWAP_CHAIN_DESC desc = *pSwapChainDesc;
    hr = dxgiFactory->CreateSwapChain(device.ptr(), &desc, ppSwapChain);

    if (FAILED(hr)) {
      Logger::err("D3D11CreateDevice: Failed to create swap chain");
      return hr;
    }
  }
  // Write back whatever info the application requested
  if (pFeatureLevel)
    *pFeatureLevel = device->GetFeatureLevel();

  if (ppDevice)
    *ppDevice = device.ref();

  if (ppImmediateContext)
    device->GetImmediateContext(ppImmediateContext);

  // If we were unable to write back the device and the
  // swap chain, the application has no way of working
  // with the device so we should report S_FALSE here.
  if (!ppDevice && !ppImmediateContext && !ppSwapChain) {
    ::dxmt::instr::logf("RET   D3D11CreateDeviceAndSwapChain hr=0x%08x (S_FALSE)", (unsigned)S_FALSE);
    return S_FALSE;
  }

  ::dxmt::instr::logf("RET   D3D11CreateDeviceAndSwapChain hr=0x%08x *ppDevice=%p", (unsigned)S_OK, ppDevice ? (void*)*ppDevice : (void*)0);
  return S_OK;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {
  ::dxmt::instr::logf("CALL  D3D11CreateDevice pAdapter=%p DriverType=%u Flags=0x%08x ppDevice=%p", (void*)pAdapter, (unsigned)DriverType, (unsigned)Flags, (void*)ppDevice);
  HRESULT hr = D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags,
                                       pFeatureLevels, FeatureLevels,
                                       SDKVersion, nullptr, nullptr, ppDevice,
                                       pFeatureLevel, ppImmediateContext);
  ::dxmt::instr::logf("RET   D3D11CreateDevice hr=0x%08x", (unsigned)hr);
  return hr;
}

extern "C" HRESULT __stdcall D3D11On12CreateDevice(
    IUnknown *pDevice, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
    UINT FeatureLevels, IUnknown *const *ppCommandQueues, UINT NumQueues,
    UINT NodeMask, ID3D11Device **ppDevice,
    ID3D11DeviceContext **ppImmediateContext,
    D3D_FEATURE_LEVEL *pChosenFeatureLevel) {
  InitReturnPtr(ppDevice);
  InitReturnPtr(ppImmediateContext);
  if (pChosenFeatureLevel)
    *pChosenFeatureLevel = (D3D_FEATURE_LEVEL)0;

  if (!pDevice || !ppCommandQueues || !NumQueues ||
      (!ppDevice && !ppImmediateContext))
    return E_INVALIDARG;

  if (FeatureLevels && !pFeatureLevels)
    return E_INVALIDARG;

  for (UINT i = 0; i < NumQueues; i++) {
    if (!ppCommandQueues[i])
      return E_INVALIDARG;
  }

  return DXGI_ERROR_UNSUPPORTED;
}

} // namespace dxmt

#ifdef _WIN32

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {  
  ::dxmt::instr::logf("D3D11 DllMain reason=%u instance=%p", (unsigned)reason, (void*)instance);
  if (reason == DLL_PROCESS_DETACH) {
    ::dxmt::dxmt_hk_drawtrace_emit_totals("dll-process-detach");
    ::dxmt::dxmt_hk_vscb_dump_emit_totals("dll-process-detach");
  }
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  ::dxmt::instr::logf("D3D11 DllMain PROCESS_ATTACH done");
  return TRUE;
}

#endif

extern "C" void _massert(const char *_Message, const char *_File,
                         unsigned _Line) {
  dxmt::Logger::err(dxmt::str::format("Assertion failed: ", _Message,
                                      "\nfile: ", _File, ":", _Line));
  std::terminate();
}

extern "C" void __cxa_pure_virtual() {
  dxmt::Logger::err(dxmt::str::format("Pure virtual function called"));
  __builtin_trap();
}
