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
#include "log/log.hpp"
#include "instr.hpp"
#include "util_string.hpp"
#include "dxmt_capture.hpp"
#include <exception>

namespace dxmt {
Logger Logger::s_instance("d3d11.log");

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
