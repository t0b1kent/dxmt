#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <psapi.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const UINT kWidth = 64;
static const UINT kHeight = 64;

typedef HRESULT(WINAPI *D3D11CreateDeviceProc)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);
typedef HRESULT(WINAPI *D3D11On12CreateDeviceProc)(
    IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT,
    UINT, ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);
typedef HRESULT(WINAPI *CreateDXGIFactory1Proc)(REFIID, void **);
typedef HRESULT(WINAPI *D3DCompileProc)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
    LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT(WINAPI *D3DReflectProc)(LPCVOID, SIZE_T, REFIID, void **);
typedef BOOL(WINAPI *GetProcessMemoryInfoProc)(HANDLE,
                                               PPROCESS_MEMORY_COUNTERS,
                                               DWORD);

static const GUID kIIDID3D11ShaderReflection = {
    0x8d536ca1,
    0x0cca,
    0x4956,
    {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};

static void print_hr(const char *label, HRESULT hr) {
  printf("%s hr=0x%08lx\n", label, (unsigned long)(uint32_t)hr);
}

static bool sample_process_memory_kb(const char *label, SIZE_T *private_kb,
                                     SIZE_T *working_set_kb) {
  *private_kb = 0;
  *working_set_kb = 0;

  HMODULE psapi = LoadLibraryA("psapi.dll");
  if (!psapi) {
    printf("%s memory_sample=SKIP load_psapi_gle=%lu\n", label,
           GetLastError());
    return false;
  }

  auto get_process_memory_info =
      (GetProcessMemoryInfoProc)GetProcAddress(psapi, "GetProcessMemoryInfo");
  if (!get_process_memory_info) {
    printf("%s memory_sample=SKIP getproc_gle=%lu\n", label, GetLastError());
    FreeLibrary(psapi);
    return false;
  }

  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  BOOL ok = get_process_memory_info(
      GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&counters,
      sizeof(counters));
  if (!ok) {
    printf("%s memory_sample=SKIP call_gle=%lu\n", label, GetLastError());
    FreeLibrary(psapi);
    return false;
  }

  *private_kb = counters.PrivateUsage / 1024;
  *working_set_kb = counters.WorkingSetSize / 1024;
  printf("%s private_kb=%llu working_set_kb=%llu pagefile_kb=%llu\n", label,
         (unsigned long long)*private_kb,
         (unsigned long long)*working_set_kb,
         (unsigned long long)(counters.PagefileUsage / 1024));
  FreeLibrary(psapi);
  return true;
}

static bool check_texture_bytes(ID3D11DeviceContext *ctx, ID3D11Resource *resource,
                                const char *label, const uint8_t *expected,
                                size_t size) {
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  HRESULT hr = ctx->Map(resource, 0, D3D11_MAP_READ, 0, &mapped);
  char hr_label[160] = {};
  snprintf(hr_label, sizeof(hr_label), "%s Map", label);
  print_hr(hr_label, hr);
  if (FAILED(hr))
    return false;

  bool ok = memcmp(mapped.pData, expected, size) == 0;
  printf("%s bytes=%02x%02x%02x%02x%02x%02x%02x%02x result=%s\n", label,
         ((const uint8_t *)mapped.pData)[0], ((const uint8_t *)mapped.pData)[1],
         ((const uint8_t *)mapped.pData)[2], ((const uint8_t *)mapped.pData)[3],
         ((const uint8_t *)mapped.pData)[4], ((const uint8_t *)mapped.pData)[5],
         ((const uint8_t *)mapped.pData)[6], ((const uint8_t *)mapped.pData)[7],
         ok ? "PASS" : "FAIL");
  ctx->Unmap(resource, 0);
  return ok;
}

static UINT read_env_uint(const char *name, UINT fallback, UINT min_value,
                          UINT max_value) {
  char value[32] = {};
  DWORD len = GetEnvironmentVariableA(name, value, ARRAYSIZE(value));
  if (!len || len >= ARRAYSIZE(value))
    return fallback;

  char *end = nullptr;
  unsigned long parsed = strtoul(value, &end, 10);
  if (!end || *end || parsed < min_value || parsed > max_value) {
    printf("%s invalid=%s fallback=%u\n", name, value, fallback);
    return fallback;
  }

  return (UINT)parsed;
}

static void print_resource_hr(const char *resource_label, const char *op,
                              HRESULT hr) {
  char label[160] = {};
  snprintf(label, sizeof(label), "UnityResourceProbe %s %s", resource_label, op);
  print_hr(label, hr);
}

static bool probe_dxgi_resource_metadata(ID3D11Resource *resource,
                                         DXGI_USAGE expected_usage,
                                         const char *resource_label) {
  bool pass = true;
  IDXGIResource1 *dxgi_resource = nullptr;
  HRESULT hr = resource->QueryInterface(__uuidof(IDXGIResource1),
                                        (void **)&dxgi_resource);
  print_resource_hr(resource_label, "QueryInterface(IDXGIResource1)", hr);
  if (FAILED(hr) || !dxgi_resource)
    return false;

  DXGI_USAGE usage = 0;
  hr = dxgi_resource->GetUsage(&usage);
  print_resource_hr(resource_label, "GetUsage", hr);
  printf("UnityResourceProbe %s dxgi_usage=0x%08x expected=0x%08x\n",
         resource_label, usage, expected_usage);
  if (FAILED(hr) || usage != expected_usage)
    pass = false;

  hr = dxgi_resource->GetUsage(nullptr);
  print_resource_hr(resource_label, "GetUsage(null)", hr);
  if (hr != E_INVALIDARG)
    pass = false;

  UINT eviction_priority = 0;
  hr = dxgi_resource->GetEvictionPriority(&eviction_priority);
  print_resource_hr(resource_label, "GetEvictionPriority(initial)", hr);
  printf("UnityResourceProbe %s eviction_priority initial=0x%08x\n",
         resource_label, eviction_priority);
  if (FAILED(hr) || eviction_priority != DXGI_RESOURCE_PRIORITY_NORMAL)
    pass = false;

  hr = dxgi_resource->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);
  print_resource_hr(resource_label, "SetEvictionPriority(max)", hr);
  if (FAILED(hr))
    pass = false;

  eviction_priority = 0;
  hr = dxgi_resource->GetEvictionPriority(&eviction_priority);
  print_resource_hr(resource_label, "GetEvictionPriority(max)", hr);
  printf("UnityResourceProbe %s eviction_priority max=0x%08x\n",
         resource_label, eviction_priority);
  if (FAILED(hr) || eviction_priority != DXGI_RESOURCE_PRIORITY_MAXIMUM)
    pass = false;

  hr = dxgi_resource->GetEvictionPriority(nullptr);
  print_resource_hr(resource_label, "GetEvictionPriority(null)", hr);
  if (hr != E_INVALIDARG)
    pass = false;

  hr = dxgi_resource->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_NORMAL);
  print_resource_hr(resource_label, "SetEvictionPriority(normal)", hr);
  if (FAILED(hr))
    pass = false;

  IDXGISurface2 *surface = (IDXGISurface2 *)0x1;
  hr = dxgi_resource->CreateSubresourceSurface(0, &surface);
  print_resource_hr(resource_label, "CreateSubresourceSurface", hr);
  printf("UnityResourceProbe %s subresource_surface=%p\n", resource_label,
         surface);
  if (hr != DXGI_ERROR_UNSUPPORTED || surface)
    pass = false;

  hr = dxgi_resource->CreateSubresourceSurface(0, nullptr);
  print_resource_hr(resource_label, "CreateSubresourceSurface(null)", hr);
  if (hr != E_INVALIDARG)
    pass = false;

  dxgi_resource->Release();
  return pass;
}

static void print_module_path(const char *label, HMODULE module) {
  WCHAR wide_path[MAX_PATH] = {};
  DWORD len = GetModuleFileNameW(module, wide_path, ARRAYSIZE(wide_path));
  if (!len) {
    printf("loaded_%s_path=<GetModuleFileNameW failed> gle=%lu\n", label,
           GetLastError());
    return;
  }

  char path[1024] = {};
  int converted = WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, path,
                                      ARRAYSIZE(path), nullptr, nullptr);
  if (!converted) {
    printf("loaded_%s_path=<WideCharToMultiByte failed> gle=%lu\n", label,
           GetLastError());
    return;
  }

  printf("loaded_%s_path=%s\n", label, path);
}

static HMODULE load_local_dll(const wchar_t *path, const char *label) {
  SetLastError(0);
  HMODULE module =
      LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  DWORD gle = GetLastError();
  printf("LoadLibraryExW(%s) module=%p gle=%lu\n", label, module, gle);
  if (module)
    print_module_path(label, module);
  return module;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HWND prepare_smoke_window(HWND hwnd) {
  if (!hwnd)
    return nullptr;

  UINT visible = read_env_uint("DXMT_SMOKE_VISIBLE_WINDOW", 0, 0, 1);
  if (visible) {
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
  }
  printf("window_visible=%u hwnd=%p\n", visible, hwnd);
  return hwnd;
}

static HWND create_hidden_window(HINSTANCE instance) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"DXMTHeadlessSmokeWindow";

  SetLastError(0);
  ATOM atom = RegisterClassExW(&wc);
  DWORD register_error = GetLastError();
  printf("RegisterClassExW atom=%u gle=%lu\n", (unsigned)atom, register_error);
  if (!atom && register_error != ERROR_CLASS_ALREADY_EXISTS)
    return nullptr;

  HWND hwnd = nullptr;
  UINT force_message = read_env_uint("DXMT_SMOKE_FORCE_MESSAGE_WINDOW", 0, 0, 1);
  if (!force_message) {
    SetLastError(0);
    hwnd =
        CreateWindowExW(0, wc.lpszClassName, L"DXMT headless D3D11 smoke",
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        kWidth, kHeight, nullptr, nullptr, instance, nullptr);
    printf("CreateWindowExW(custom) hwnd=%p gle=%lu\n", hwnd, GetLastError());
    if (hwnd)
      return prepare_smoke_window(hwnd);

    SetLastError(0);
    hwnd = CreateWindowExW(0, L"STATIC", L"DXMT headless D3D11 smoke",
                           WS_OVERLAPPEDWINDOW, 0, 0, kWidth, kHeight, nullptr,
                           nullptr, instance, nullptr);
    printf("CreateWindowExW(static) hwnd=%p gle=%lu\n", hwnd, GetLastError());
    if (hwnd)
      return prepare_smoke_window(hwnd);

    SetLastError(0);
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
                           L"DXMT headless D3D11 smoke", WS_POPUP, 0, 0,
                           kWidth, kHeight, nullptr, nullptr, instance,
                           nullptr);
    printf("CreateWindowExW(popup) hwnd=%p gle=%lu\n", hwnd, GetLastError());
    if (hwnd)
      return prepare_smoke_window(hwnd);
  }

  SetLastError(0);
  hwnd = CreateWindowExW(0, wc.lpszClassName, L"DXMT headless D3D11 smoke", 0,
                         0, 0, 1, 1, HWND_MESSAGE, nullptr, instance, nullptr);
  printf("CreateWindowExW(message) hwnd=%p gle=%lu\n", hwnd, GetLastError());
  return prepare_smoke_window(hwnd);
}

static HRESULT create_swapchain_with_effect(ID3D11Device *device, HWND hwnd,
                                            DXGI_SWAP_EFFECT effect,
                                            IDXGISwapChain1 **swapchain) {
  *swapchain = nullptr;

  IDXGIDevice *dxgi_device = nullptr;
  HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                      (void **)&dxgi_device);
  if (FAILED(hr))
    return hr;

  IDXGIAdapter *adapter = nullptr;
  hr = dxgi_device->GetAdapter(&adapter);
  dxgi_device->Release();
  if (FAILED(hr))
    return hr;

  IDXGIFactory2 *factory2 = nullptr;
  hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory2);
  adapter->Release();
  if (FAILED(hr))
    return hr;

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = effect;
  desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
  desc.Flags = 0;

  hr = factory2->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr,
                                        swapchain);
  factory2->Release();
  return hr;
}

static HRESULT create_swapchain(ID3D11Device *device, HWND hwnd,
                                IDXGISwapChain1 **swapchain) {
  return create_swapchain_with_effect(device, hwnd, DXGI_SWAP_EFFECT_DISCARD,
                                      swapchain);
}

static bool probe_factory_unsupported_swapchains(ID3D11Device *device) {
  bool pass = true;

  IDXGIDevice *dxgi_device = nullptr;
  HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                      (void **)&dxgi_device);
  print_hr("UnityFactoryProbe QueryInterface(IDXGIDevice)", hr);
  if (FAILED(hr) || !dxgi_device)
    return false;

  DXGI_SURFACE_DESC surface_desc = {};
  surface_desc.Width = 64;
  surface_desc.Height = 64;
  surface_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  surface_desc.SampleDesc.Count = 1;
  IDXGISurface *surface = (IDXGISurface *)0x1;
  hr = dxgi_device->CreateSurface(&surface_desc, 1,
                                  DXGI_USAGE_RENDER_TARGET_OUTPUT, nullptr,
                                  &surface);
  print_hr("UnityFactoryProbe CreateSurface", hr);
  printf("UnityFactoryProbe surface=%p\n", surface);
  if (hr != DXGI_ERROR_UNSUPPORTED || surface)
    pass = false;
  surface = (IDXGISurface *)0x1;
  hr = dxgi_device->CreateSurface(nullptr, 1,
                                  DXGI_USAGE_RENDER_TARGET_OUTPUT, nullptr,
                                  &surface);
  print_hr("UnityFactoryProbe CreateSurface(null_desc)", hr);
  printf("UnityFactoryProbe surface_after_null_desc=%p\n", surface);
  if (hr != E_INVALIDARG || surface != (IDXGISurface *)0x1)
    pass = false;
  hr = dxgi_device->CreateSurface(&surface_desc, 1,
                                  DXGI_USAGE_RENDER_TARGET_OUTPUT, nullptr,
                                  nullptr);
  print_hr("UnityFactoryProbe CreateSurface(null)", hr);
  if (hr != E_INVALIDARG)
    pass = false;
  hr = dxgi_device->CreateSurface(&surface_desc, 0,
                                  DXGI_USAGE_RENDER_TARGET_OUTPUT, nullptr,
                                  &surface);
  print_hr("UnityFactoryProbe CreateSurface(zero_count)", hr);
  if (hr != E_INVALIDARG)
    pass = false;

  IDXGIAdapter *adapter = nullptr;
  hr = dxgi_device->GetAdapter(&adapter);
  dxgi_device->Release();
  print_hr("UnityFactoryProbe GetAdapter", hr);
  if (FAILED(hr) || !adapter)
    return false;

  IDXGIFactory2 *factory2 = nullptr;
  hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory2);
  adapter->Release();
  print_hr("UnityFactoryProbe GetParent(IDXGIFactory2)", hr);
  if (FAILED(hr) || !factory2)
    return false;

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

  IDXGISwapChain1 *swapchain = (IDXGISwapChain1 *)0x1;
  hr = factory2->CreateSwapChainForComposition(device, &desc, nullptr,
                                               &swapchain);
  print_hr("UnityFactoryProbe CreateSwapChainForComposition", hr);
  printf("UnityFactoryProbe composition_swapchain=%p\n", swapchain);
  if (hr != DXGI_ERROR_UNSUPPORTED || swapchain)
    pass = false;

  swapchain = (IDXGISwapChain1 *)0x1;
  hr = factory2->CreateSwapChainForCoreWindow(device, device, &desc, nullptr,
                                              &swapchain);
  print_hr("UnityFactoryProbe CreateSwapChainForCoreWindow", hr);
  printf("UnityFactoryProbe corewindow_swapchain=%p\n", swapchain);
  if (hr != DXGI_ERROR_UNSUPPORTED || swapchain)
    pass = false;

  swapchain = (IDXGISwapChain1 *)0x1;
  hr = factory2->CreateSwapChainForComposition(nullptr, &desc, nullptr,
                                               &swapchain);
  print_hr("UnityFactoryProbe CreateSwapChainForComposition(null_device)", hr);
  printf("UnityFactoryProbe composition_null_device_swapchain=%p\n",
         swapchain);
  if (hr != DXGI_ERROR_INVALID_CALL || swapchain)
    pass = false;

  factory2->Release();
  printf("UnityFactoryProbe unsupported_swapchains=%s\n",
         pass ? "PASS" : "FAIL");
  return pass;
}

static HRESULT read_first_pixel(ID3D11Device *device, ID3D11DeviceContext *ctx,
                                ID3D11Texture2D *backbuffer, uint8_t bgra[4]);

static bool probe_unity_output_gamma(IDXGIOutput *output, const char *label) {
  bool pass = true;

  DXGI_GAMMA_CONTROL_CAPABILITIES caps = {};
  HRESULT hr = output->GetGammaControlCapabilities(&caps);
  printf("UnityGammaProbe %s GetGammaControlCapabilities hr=0x%08lx points=%u scale_offset=%u\n",
         label, (unsigned long)(uint32_t)hr, caps.NumGammaControlPoints,
         (unsigned)caps.ScaleAndOffsetSupported);
  if (FAILED(hr) || !caps.NumGammaControlPoints)
    pass = false;

  DXGI_FRAME_STATISTICS output_stats = {};
  hr = output->GetFrameStatistics(&output_stats);
  print_hr("UnityGammaProbe GetFrameStatistics", hr);
  if (SUCCEEDED(hr)) {
    printf("UnityGammaProbe output_frame_stats present=%u sync=%u qpc=%llu\n",
           output_stats.PresentCount, output_stats.PresentRefreshCount,
           (unsigned long long)output_stats.SyncQPCTime.QuadPart);
  } else {
    pass = false;
  }

  hr = output->GetFrameStatistics(nullptr);
  print_hr("UnityGammaProbe GetFrameStatistics(null)", hr);
  if (hr != DXGI_ERROR_INVALID_CALL)
    pass = false;

  hr = output->SetDisplaySurface((IDXGISurface *)0x1);
  print_hr("UnityGammaProbe SetDisplaySurface(fake)", hr);
  if (hr != DXGI_ERROR_UNSUPPORTED)
    pass = false;

  hr = output->SetDisplaySurface(nullptr);
  print_hr("UnityGammaProbe SetDisplaySurface(null)", hr);
  if (hr != DXGI_ERROR_INVALID_CALL)
    pass = false;

  hr = output->GetDisplaySurfaceData((IDXGISurface *)0x1);
  print_hr("UnityGammaProbe GetDisplaySurfaceData(fake)", hr);
  if (hr != DXGI_ERROR_UNSUPPORTED)
    pass = false;

  hr = output->GetDisplaySurfaceData(nullptr);
  print_hr("UnityGammaProbe GetDisplaySurfaceData(null)", hr);
  if (hr != DXGI_ERROR_INVALID_CALL)
    pass = false;

  IDXGIOutput1 *output1 = nullptr;
  hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
  print_hr("UnityGammaProbe QueryInterface(IDXGIOutput1)", hr);
  if (FAILED(hr) || !output1) {
    pass = false;
  } else {
    hr = output1->GetDisplaySurfaceData1((IDXGIResource *)0x1);
    print_hr("UnityGammaProbe GetDisplaySurfaceData1(fake)", hr);
    if (hr != DXGI_ERROR_UNSUPPORTED)
      pass = false;

    hr = output1->GetDisplaySurfaceData1(nullptr);
    print_hr("UnityGammaProbe GetDisplaySurfaceData1(null)", hr);
    if (hr != DXGI_ERROR_INVALID_CALL)
      pass = false;

    output1->Release();
  }

  DXGI_GAMMA_CONTROL original = {};
  hr = output->GetGammaControl(&original);
  print_hr("UnityGammaProbe GetGammaControl(initial)", hr);
  bool have_original = SUCCEEDED(hr);
  if (!have_original) {
    pass = false;
  } else {
    DXGI_GAMMA_CONTROL updated = original;
    updated.GammaCurve[0].Red = 0.125f;
    updated.GammaCurve[0].Green = 0.25f;
    updated.GammaCurve[0].Blue = 0.5f;
    hr = output->SetGammaControl(&updated);
    print_hr("UnityGammaProbe SetGammaControl(updated)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      DXGI_GAMMA_CONTROL readback = {};
      hr = output->GetGammaControl(&readback);
      print_hr("UnityGammaProbe GetGammaControl(updated)", hr);
      printf("UnityGammaProbe sample0_rgb=%.3f,%.3f,%.3f\n",
             readback.GammaCurve[0].Red, readback.GammaCurve[0].Green,
             readback.GammaCurve[0].Blue);
      if (FAILED(hr) || readback.GammaCurve[0].Red != 0.125f ||
          readback.GammaCurve[0].Green != 0.25f ||
          readback.GammaCurve[0].Blue != 0.5f)
        pass = false;
    }

    hr = output->SetGammaControl(&original);
    print_hr("UnityGammaProbe SetGammaControl(restore)", hr);
    if (FAILED(hr))
      pass = false;
  }

  printf("UnityGammaProbe %s result=%s\n", label, pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_swapchain_variant(ID3D11Device *device,
                                    ID3D11DeviceContext *ctx,
                                    HINSTANCE instance,
                                    DXGI_SWAP_EFFECT effect,
                                    const char *label) {
  HWND hwnd = create_hidden_window(instance);
  if (!hwnd) {
    printf("UnitySwapchainProbe %s window=FAIL gle=%lu\n", label,
           GetLastError());
    return false;
  }

  IDXGISwapChain1 *swapchain = nullptr;
  HRESULT hr = create_swapchain_with_effect(device, hwnd, effect, &swapchain);
  printf("UnitySwapchainProbe %s effect=%u\n", label, effect);
  print_hr("UnitySwapchainProbe CreateSwapChainForHwnd", hr);
  bool pass = SUCCEEDED(hr) && swapchain;

  IDXGIOutput *containing_output = nullptr;
  if (pass) {
    hr = swapchain->GetContainingOutput(&containing_output);
    print_hr("UnitySwapchainProbe GetContainingOutput", hr);
    if (FAILED(hr))
      pass = false;
    else if (!probe_unity_output_gamma(containing_output, label))
      pass = false;
  }
  if (pass) {
    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    hr = swapchain->GetDesc1(&desc1);
    print_hr("UnitySwapchainProbe GetDesc1", hr);
    printf("UnitySwapchainProbe desc1 width=%u height=%u buffers=%u effect=%u\n",
           desc1.Width, desc1.Height, desc1.BufferCount, desc1.SwapEffect);
    if (FAILED(hr) || desc1.Width != kWidth || desc1.Height != kHeight ||
        desc1.SwapEffect != effect)
      pass = false;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc = {};
    hr = swapchain->GetFullscreenDesc(&fullscreen_desc);
    print_hr("UnitySwapchainProbe GetFullscreenDesc", hr);
    printf("UnitySwapchainProbe fullscreen_desc windowed=%u refresh=%u/%u\n",
           fullscreen_desc.Windowed, fullscreen_desc.RefreshRate.Numerator,
           fullscreen_desc.RefreshRate.Denominator);
    if (FAILED(hr))
      pass = false;

    HWND swapchain_hwnd = nullptr;
    hr = swapchain->GetHwnd(&swapchain_hwnd);
    print_hr("UnitySwapchainProbe GetHwnd", hr);
    printf("UnitySwapchainProbe hwnd=%p expected=%p\n", swapchain_hwnd, hwnd);
    if (FAILED(hr) || swapchain_hwnd != hwnd)
      pass = false;

    void *core_window = (void *)0x1;
    hr = swapchain->GetCoreWindow(__uuidof(IUnknown), &core_window);
    print_hr("UnitySwapchainProbe GetCoreWindow(hwnd)", hr);
    printf("UnitySwapchainProbe core_window=%p\n", core_window);
    if (hr != DXGI_ERROR_INVALID_CALL || core_window)
      pass = false;

    IDXGIOutput *restrict_output = nullptr;
    hr = swapchain->GetRestrictToOutput(&restrict_output);
    print_hr("UnitySwapchainProbe GetRestrictToOutput", hr);
    printf("UnitySwapchainProbe restrict_output=%p\n", restrict_output);
    if (FAILED(hr) || restrict_output)
      pass = false;
    if (restrict_output)
      restrict_output->Release();

    DXGI_RGBA original_background = {};
    hr = swapchain->GetBackgroundColor(&original_background);
    print_hr("UnitySwapchainProbe GetBackgroundColor(initial)", hr);
    if (FAILED(hr))
      pass = false;
    DXGI_RGBA background = {0.125f, 0.25f, 0.5f, 0.75f};
    hr = swapchain->SetBackgroundColor(&background);
    print_hr("UnitySwapchainProbe SetBackgroundColor(updated)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      DXGI_RGBA background_readback = {};
      hr = swapchain->GetBackgroundColor(&background_readback);
      print_hr("UnitySwapchainProbe GetBackgroundColor(updated)", hr);
      printf("UnitySwapchainProbe background_rgba=%.3f,%.3f,%.3f,%.3f\n",
             background_readback.r, background_readback.g,
             background_readback.b, background_readback.a);
      if (FAILED(hr) || background_readback.r != background.r ||
          background_readback.g != background.g ||
          background_readback.b != background.b ||
          background_readback.a != background.a)
        pass = false;
    }
    hr = swapchain->SetBackgroundColor(&original_background);
    print_hr("UnitySwapchainProbe SetBackgroundColor(restore)", hr);
    if (FAILED(hr))
      pass = false;

    DXGI_MODE_ROTATION original_rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    hr = swapchain->GetRotation(&original_rotation);
    print_hr("UnitySwapchainProbe GetRotation(initial)", hr);
    if (FAILED(hr))
      pass = false;
    hr = swapchain->SetRotation(DXGI_MODE_ROTATION_ROTATE180);
    print_hr("UnitySwapchainProbe SetRotation(180)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      DXGI_MODE_ROTATION rotation_readback = DXGI_MODE_ROTATION_UNSPECIFIED;
      hr = swapchain->GetRotation(&rotation_readback);
      print_hr("UnitySwapchainProbe GetRotation(180)", hr);
      printf("UnitySwapchainProbe rotation=%u\n", rotation_readback);
      if (FAILED(hr) || rotation_readback != DXGI_MODE_ROTATION_ROTATE180)
        pass = false;
    }
    hr = swapchain->SetRotation(original_rotation);
    print_hr("UnitySwapchainProbe SetRotation(restore)", hr);
    if (FAILED(hr))
      pass = false;

    IDXGISwapChain2 *swapchain2 = nullptr;
    hr = swapchain->QueryInterface(__uuidof(IDXGISwapChain2),
                                   (void **)&swapchain2);
    print_hr("UnitySwapchainProbe QueryInterface(IDXGISwapChain2)", hr);
    if (FAILED(hr) || !swapchain2) {
      pass = false;
    } else {
      UINT source_width = 0;
      UINT source_height = 0;
      hr = swapchain2->GetSourceSize(&source_width, &source_height);
      print_hr("UnitySwapchainProbe GetSourceSize(initial)", hr);
      printf("UnitySwapchainProbe source_size initial=%ux%u\n", source_width,
             source_height);
      if (FAILED(hr) || source_width != kWidth || source_height != kHeight)
        pass = false;

      hr = swapchain2->SetSourceSize(kWidth / 2, kHeight / 2);
      print_hr("UnitySwapchainProbe SetSourceSize(half)", hr);
      if (FAILED(hr)) {
        pass = false;
      } else {
        source_width = 0;
        source_height = 0;
        hr = swapchain2->GetSourceSize(&source_width, &source_height);
        print_hr("UnitySwapchainProbe GetSourceSize(half)", hr);
        printf("UnitySwapchainProbe source_size half=%ux%u\n", source_width,
               source_height);
        if (FAILED(hr) || source_width != kWidth / 2 ||
            source_height != kHeight / 2)
          pass = false;
      }
      hr = swapchain2->SetSourceSize(kWidth, kHeight);
      print_hr("UnitySwapchainProbe SetSourceSize(restore)", hr);
      if (FAILED(hr))
        pass = false;

      UINT original_latency = 0;
      hr = swapchain2->GetMaximumFrameLatency(&original_latency);
      print_hr("UnitySwapchainProbe GetMaximumFrameLatency(initial)", hr);
      printf("UnitySwapchainProbe max_frame_latency initial=%u\n",
             original_latency);
      if (FAILED(hr))
        pass = false;
      hr = swapchain2->SetMaximumFrameLatency(2);
      print_hr("UnitySwapchainProbe SetMaximumFrameLatency(2)", hr);
      if (FAILED(hr)) {
        pass = false;
      } else {
        UINT latency = 0;
        hr = swapchain2->GetMaximumFrameLatency(&latency);
        print_hr("UnitySwapchainProbe GetMaximumFrameLatency(2)", hr);
        printf("UnitySwapchainProbe max_frame_latency updated=%u\n", latency);
        if (FAILED(hr) || latency != 2)
          pass = false;
      }
      if (original_latency) {
        hr = swapchain2->SetMaximumFrameLatency(original_latency);
        print_hr("UnitySwapchainProbe SetMaximumFrameLatency(restore)", hr);
        if (FAILED(hr))
          pass = false;
      }

      DXGI_MATRIX_3X2_F original_matrix = {};
      hr = swapchain2->GetMatrixTransform(&original_matrix);
      print_hr("UnitySwapchainProbe GetMatrixTransform(initial)", hr);
      if (FAILED(hr))
        pass = false;
      DXGI_MATRIX_3X2_F matrix = {1.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.5f};
      hr = swapchain2->SetMatrixTransform(&matrix);
      print_hr("UnitySwapchainProbe SetMatrixTransform(offset)", hr);
      if (FAILED(hr)) {
        pass = false;
      } else {
        DXGI_MATRIX_3X2_F readback_matrix = {};
        hr = swapchain2->GetMatrixTransform(&readback_matrix);
        print_hr("UnitySwapchainProbe GetMatrixTransform(offset)", hr);
        printf("UnitySwapchainProbe matrix_offset=%.2f,%.2f\n",
               readback_matrix._31, readback_matrix._32);
        if (FAILED(hr) || readback_matrix._31 != matrix._31 ||
            readback_matrix._32 != matrix._32)
          pass = false;
      }
      hr = swapchain2->SetMatrixTransform(&original_matrix);
      print_hr("UnitySwapchainProbe SetMatrixTransform(restore)", hr);
      if (FAILED(hr))
        pass = false;

      swapchain2->Release();
    }

    IDXGISwapChain3 *swapchain3 = nullptr;
    hr = swapchain->QueryInterface(__uuidof(IDXGISwapChain3),
                                   (void **)&swapchain3);
    print_hr("UnitySwapchainProbe QueryInterface(IDXGISwapChain3)", hr);
    if (FAILED(hr) || !swapchain3) {
      pass = false;
    } else {
      UINT color_space_support = 0;
      hr = swapchain3->CheckColorSpaceSupport(
          DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &color_space_support);
      print_hr("UnitySwapchainProbe CheckColorSpaceSupport(P709)", hr);
      printf("UnitySwapchainProbe color_space_support=0x%08x\n",
             color_space_support);
      if (FAILED(hr) ||
          !(color_space_support &
            DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        pass = false;

      hr = swapchain3->CheckColorSpaceSupport(
          DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, nullptr);
      print_hr("UnitySwapchainProbe CheckColorSpaceSupport(null)", hr);
      if (hr != E_INVALIDARG)
        pass = false;

      hr = swapchain3->SetColorSpace1(
          DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      print_hr("UnitySwapchainProbe SetColorSpace1(P709)", hr);
      if (FAILED(hr))
        pass = false;

      swapchain3->Release();
    }

    IDXGISwapChain4 *swapchain4 = nullptr;
    hr = swapchain->QueryInterface(__uuidof(IDXGISwapChain4),
                                   (void **)&swapchain4);
    print_hr("UnitySwapchainProbe QueryInterface(IDXGISwapChain4)", hr);
    if (FAILED(hr) || !swapchain4) {
      pass = false;
    } else {
      DXGI_HDR_METADATA_HDR10 hdr10 = {};
      hdr10.RedPrimary[0] = 35466;
      hdr10.RedPrimary[1] = 7099;
      hdr10.GreenPrimary[0] = 24962;
      hdr10.GreenPrimary[1] = 53197;
      hdr10.BluePrimary[0] = 6553;
      hdr10.BluePrimary[1] = 3276;
      hdr10.WhitePoint[0] = 32178;
      hdr10.WhitePoint[1] = 32178;
      hdr10.MaxMasteringLuminance = 1000;
      hdr10.MinMasteringLuminance = 1;
      hdr10.MaxContentLightLevel = 800;
      hdr10.MaxFrameAverageLightLevel = 400;

      hr = swapchain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,
                                      sizeof(hdr10), &hdr10);
      print_hr("UnitySwapchainProbe SetHDRMetaData(HDR10)", hr);
      if (FAILED(hr))
        pass = false;

      hr = swapchain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,
                                      sizeof(hdr10) - 1, &hdr10);
      print_hr("UnitySwapchainProbe SetHDRMetaData(HDR10 invalid_size)", hr);
      if (hr != E_INVALIDARG)
        pass = false;

      hr = swapchain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,
                                      sizeof(hdr10), nullptr);
      print_hr("UnitySwapchainProbe SetHDRMetaData(HDR10 null)", hr);
      if (hr != E_INVALIDARG)
        pass = false;

      hr = swapchain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
      print_hr("UnitySwapchainProbe SetHDRMetaData(NONE)", hr);
      if (FAILED(hr))
        pass = false;

      hr = swapchain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10PLUS, 0,
                                      nullptr);
      print_hr("UnitySwapchainProbe SetHDRMetaData(HDR10PLUS)", hr);
      if (hr != DXGI_ERROR_UNSUPPORTED)
        pass = false;

      swapchain4->Release();
    }

    DXGI_MODE_DESC target_mode = {};
    target_mode.Width = kWidth;
    target_mode.Height = kHeight;
    target_mode.RefreshRate.Numerator = 60;
    target_mode.RefreshRate.Denominator = 1;
    target_mode.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    target_mode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    target_mode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    hr = swapchain->ResizeTarget(&target_mode);
    print_hr("UnitySwapchainProbe ResizeTarget(60hz)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      DXGI_SWAP_CHAIN_DESC legacy_desc = {};
      hr = swapchain->GetDesc(&legacy_desc);
      print_hr("UnitySwapchainProbe GetDesc(after ResizeTarget)", hr);
      printf("UnitySwapchainProbe resize_target_refresh=%u/%u\n",
             legacy_desc.BufferDesc.RefreshRate.Numerator,
             legacy_desc.BufferDesc.RefreshRate.Denominator);
      if (FAILED(hr) ||
          legacy_desc.BufferDesc.RefreshRate.Numerator != 60 ||
          legacy_desc.BufferDesc.RefreshRate.Denominator != 1)
        pass = false;
    }

    hr = swapchain->ResizeTarget(nullptr);
    print_hr("UnitySwapchainProbe ResizeTarget(null)", hr);
    if (hr != DXGI_ERROR_INVALID_CALL)
      pass = false;

    BOOL fullscreen = FALSE;
    IDXGIOutput *fullscreen_target = nullptr;
    hr = swapchain->GetFullscreenState(&fullscreen, &fullscreen_target);
    print_hr("UnitySwapchainProbe GetFullscreenState", hr);
    printf("UnitySwapchainProbe fullscreen=%u target=%p\n", fullscreen,
           fullscreen_target);
    if (FAILED(hr))
      pass = false;
    if (fullscreen_target)
      fullscreen_target->Release();

    hr = swapchain->SetFullscreenState(FALSE, nullptr);
    print_hr("UnitySwapchainProbe SetFullscreenState(FALSE)", hr);
    if (FAILED(hr))
      pass = false;

    if (read_env_uint("DXMT_SMOKE_FULLSCREEN_ENTER", 0, 0, 1)) {
      hr = swapchain->SetFullscreenState(TRUE, nullptr);
      print_hr("UnitySwapchainProbe SetFullscreenState(TRUE)", hr);
      HRESULT enter_hr = hr;
      if (FAILED(hr) && hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE &&
          hr != DXGI_STATUS_MODE_CHANGE_IN_PROGRESS && hr != E_FAIL)
        pass = false;

      fullscreen = FALSE;
      fullscreen_target = nullptr;
      hr = swapchain->GetFullscreenState(&fullscreen, &fullscreen_target);
      print_hr("UnitySwapchainProbe GetFullscreenState(after TRUE)", hr);
      printf("UnitySwapchainProbe fullscreen_after_true=%u target=%p\n",
             fullscreen, fullscreen_target);
      if (FAILED(hr))
        pass = false;
      if (fullscreen_target)
        fullscreen_target->Release();

      if (SUCCEEDED(enter_hr)) {
        if (!fullscreen)
          pass = false;
        hr = swapchain->SetFullscreenState(FALSE, nullptr);
        print_hr("UnitySwapchainProbe SetFullscreenState(FALSE restore)", hr);
        if (FAILED(hr))
          pass = false;
      } else if (fullscreen) {
        pass = false;
        hr = swapchain->SetFullscreenState(FALSE, nullptr);
        print_hr("UnitySwapchainProbe SetFullscreenState(FALSE cleanup)", hr);
      }
    }
  }

  ID3D11Texture2D *backbuffer = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  if (pass) {
    ID3D11Texture2D *extra_buffer = nullptr;
    hr = swapchain->GetBuffer(1, __uuidof(ID3D11Texture2D),
                              (void **)&extra_buffer);
    print_hr("UnitySwapchainProbe GetBuffer(1)", hr);
    printf("UnitySwapchainProbe extra_buffer=%p\n", extra_buffer);
    if (FAILED(hr) || !extra_buffer)
      pass = false;
    if (extra_buffer)
      extra_buffer->Release();

    void *invalid_buffer = (void *)0x1;
    hr = swapchain->GetBuffer(2, __uuidof(ID3D11Texture2D), &invalid_buffer);
    print_hr("UnitySwapchainProbe GetBuffer(2)", hr);
    printf("UnitySwapchainProbe invalid_buffer=%p\n", invalid_buffer);
    if (hr != DXGI_ERROR_INVALID_CALL || invalid_buffer)
      pass = false;

    hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                              (void **)&backbuffer);
    print_hr("UnitySwapchainProbe GetBuffer", hr);
    if (FAILED(hr))
      pass = false;
  }
  if (backbuffer) {
    hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    print_hr("UnitySwapchainProbe CreateRenderTargetView", hr);
    if (FAILED(hr))
      pass = false;
  }
  if (rtv) {
    const FLOAT color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, color);
    printf("UnitySwapchainProbe %s ClearRenderTargetView=PASS\n", label);
    hr = swapchain->Present(0, DXGI_PRESENT_TEST);
    printf("UnitySwapchainProbe %s Present(test) hr=0x%08lx\n", label,
           (unsigned long)(uint32_t)hr);
    if (FAILED(hr))
      pass = false;

    hr = swapchain->Present(0, 0);
    print_hr("UnitySwapchainProbe Present", hr);
    if (FAILED(hr))
      pass = false;

    ctx->ClearRenderTargetView(rtv, color);
    hr = swapchain->Present(1, 0);
    printf("UnitySwapchainProbe %s Present(vsync) hr=0x%08lx\n", label,
           (unsigned long)(uint32_t)hr);
    if (FAILED(hr))
      pass = false;

    RECT dirty = {0, 0, (LONG)kWidth, (LONG)kHeight};
    DXGI_PRESENT_PARAMETERS present_params = {};
    present_params.DirtyRectsCount = 1;
    present_params.pDirtyRects = &dirty;
    hr = swapchain->Present1(0, DXGI_PRESENT_TEST, &present_params);
    printf("UnitySwapchainProbe %s Present1(dirty_test) hr=0x%08lx\n", label,
           (unsigned long)(uint32_t)hr);
    if (FAILED(hr))
      pass = false;

    hr = swapchain->Present1(0, 0, &present_params);
    printf("UnitySwapchainProbe %s Present1(dirty) hr=0x%08lx\n", label,
           (unsigned long)(uint32_t)hr);
    if (FAILED(hr))
      pass = false;

    DXGI_FRAME_STATISTICS frame_stats = {};
    hr = swapchain->GetFrameStatistics(&frame_stats);
    print_hr("UnitySwapchainProbe GetFrameStatistics", hr);
    if (SUCCEEDED(hr)) {
      printf("UnitySwapchainProbe frame_stats present=%u sync=%u qpc=%llu\n",
             frame_stats.PresentCount, frame_stats.PresentRefreshCount,
             (unsigned long long)frame_stats.SyncQPCTime.QuadPart);
    }

    uint8_t pixel[4] = {};
    hr = read_first_pixel(device, ctx, backbuffer, pixel);
    print_hr("UnitySwapchainProbe Readback", hr);
    printf("UnitySwapchainProbe %s pixel0_bgra=%u,%u,%u,%u\n", label,
           pixel[0], pixel[1], pixel[2], pixel[3]);
    if (FAILED(hr) || pixel[0] > 2 || pixel[1] < 235 || pixel[2] > 2 ||
        pixel[3] != 255)
      pass = false;
  }

  if (pass) {
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->Flush();
    if (rtv) {
      rtv->Release();
      rtv = nullptr;
    }
    if (backbuffer) {
      backbuffer->Release();
      backbuffer = nullptr;
    }

    hr = swapchain->ResizeBuffers(0, kWidth / 2, kHeight / 2,
                                  DXGI_FORMAT_UNKNOWN, 0);
    printf("UnitySwapchainProbe %s ResizeBuffers(half) hr=0x%08lx\n", label,
           (unsigned long)(uint32_t)hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      DXGI_SWAP_CHAIN_DESC1 resized_desc = {};
      hr = swapchain->GetDesc1(&resized_desc);
      print_hr("UnitySwapchainProbe GetDesc1(resized)", hr);
      printf("UnitySwapchainProbe %s resized_desc width=%u height=%u\n",
             label, resized_desc.Width, resized_desc.Height);
      if (FAILED(hr) || resized_desc.Width != kWidth / 2 ||
          resized_desc.Height != kHeight / 2)
        pass = false;
    }
    if (pass) {
      hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                (void **)&backbuffer);
      print_hr("UnitySwapchainProbe GetBuffer(resized)", hr);
      if (FAILED(hr) || !backbuffer) {
        pass = false;
      } else {
        hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
        print_hr("UnitySwapchainProbe CreateRenderTargetView(resized)", hr);
        if (FAILED(hr))
          pass = false;
      }
    }
    if (pass && rtv) {
      const FLOAT resize_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
      ctx->ClearRenderTargetView(rtv, resize_color);
      printf("UnitySwapchainProbe %s ClearRenderTargetView(resized)=PASS\n",
             label);
      hr = swapchain->Present(0, 0);
      printf("UnitySwapchainProbe %s Present(resized) hr=0x%08lx\n", label,
             (unsigned long)(uint32_t)hr);
      if (FAILED(hr))
        pass = false;

      uint8_t resized_pixel[4] = {};
      hr = read_first_pixel(device, ctx, backbuffer, resized_pixel);
      print_hr("UnitySwapchainProbe Readback(resized)", hr);
      printf("UnitySwapchainProbe %s resized_pixel0_bgra=%u,%u,%u,%u\n",
             label, resized_pixel[0], resized_pixel[1], resized_pixel[2],
             resized_pixel[3]);
      if (FAILED(hr) || resized_pixel[0] < 235 || resized_pixel[1] > 2 ||
          resized_pixel[2] > 2 || resized_pixel[3] != 255)
        pass = false;
    }
  }

  if (rtv)
    rtv->Release();
  if (backbuffer)
    backbuffer->Release();
  if (containing_output)
    containing_output->Release();
  if (swapchain)
    swapchain->Release();
  DestroyWindow(hwnd);

  printf("UnitySwapchainProbe %s result=%s\n", label, pass ? "PASS" : "FAIL");
  return pass;
}

static HRESULT read_first_pixel(ID3D11Device *device, ID3D11DeviceContext *ctx,
                                ID3D11Texture2D *backbuffer, uint8_t bgra[4]) {
  D3D11_TEXTURE2D_DESC desc = {};
  backbuffer->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;

  ID3D11Texture2D *staging = nullptr;
  HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
  if (FAILED(hr))
    return hr;

  ctx->CopyResource(staging, backbuffer);
  ctx->Flush();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (SUCCEEDED(hr)) {
    const uint8_t *src = (const uint8_t *)mapped.pData;
    bgra[0] = src[0];
    bgra[1] = src[1];
    bgra[2] = src[2];
    bgra[3] = src[3];
    ctx->Unmap(staging, 0);
  }

  staging->Release();
  return hr;
}

static HRESULT read_first_pixel_subresource(ID3D11Device *device,
                                            ID3D11DeviceContext *ctx,
                                            ID3D11Texture2D *texture,
                                            UINT subresource,
                                            uint8_t rgba[4]) {
  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  if (subresource >= desc.MipLevels * desc.ArraySize)
    return E_INVALIDARG;

  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;

  ID3D11Texture2D *staging = nullptr;
  HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
  if (FAILED(hr))
    return hr;

  ctx->CopyResource(staging, texture);
  ctx->Flush();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = ctx->Map(staging, subresource, D3D11_MAP_READ, 0, &mapped);
  if (SUCCEEDED(hr)) {
    const uint8_t *src = (const uint8_t *)mapped.pData;
    rgba[0] = src[0];
    rgba[1] = src[1];
    rgba[2] = src[2];
    rgba[3] = src[3];
    ctx->Unmap(staging, subresource);
  }

  staging->Release();
  return hr;
}

static bool probe_unity_long_run_stability(ID3D11Device *device,
                                           ID3D11DeviceContext *ctx,
                                           IDXGISwapChain1 *swapchain,
                                           ID3D11RenderTargetView *rtv,
                                           ID3D11Texture2D *backbuffer) {
  const UINT frames =
      read_env_uint("DXMT_SMOKE_STABILITY_FRAMES", 180, 1, 1200);
  const UINT min_ms =
      read_env_uint("DXMT_SMOKE_STABILITY_MIN_MS", 0, 0, 600000);
  const UINT leak_budget_kb =
      read_env_uint("DXMT_SMOKE_LEAK_BUDGET_KB", 0, 0, 1024 * 1024);
  printf("UnityStabilityProbe frames=%u min_ms=%u leak_budget_kb=%u\n",
         frames, min_ms, leak_budget_kb);

  bool pass = true;
  SIZE_T private_before_kb = 0;
  SIZE_T working_set_before_kb = 0;
  bool leak_sampled_before = sample_process_memory_kb(
      "UnityLeakProbe before", &private_before_kb, &working_set_before_kb);

  UINT presented = 0;
  ULONGLONG start_ms = GetTickCount64();
  ULONGLONG elapsed_ms = 0;
  while (presented < frames || (min_ms && elapsed_ms < min_ms)) {
    const FLOAT color[4] = {
        (presented & 1) ? 0.0f : 0.125f,
        1.0f,
        (presented & 2) ? 0.0f : 0.25f,
        1.0f,
    };
    ctx->ClearRenderTargetView(rtv, color);
    HRESULT hr = swapchain->Present(0, 0);
    if (FAILED(hr)) {
      printf("UnityStabilityProbe Present frame=%u hr=0x%08lx\n", presented,
             (unsigned long)(uint32_t)hr);
      pass = false;
      break;
    }
    presented++;
    if (min_ms)
      Sleep(1);
    elapsed_ms = GetTickCount64() - start_ms;
  }

  const FLOAT final_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ctx->ClearRenderTargetView(rtv, final_color);
  ctx->Flush();

  uint8_t pixel[4] = {};
  HRESULT hr = read_first_pixel(device, ctx, backbuffer, pixel);
  print_hr("UnityStabilityProbe Readback(final)", hr);
  printf("UnityStabilityProbe final_bgra=%u,%u,%u,%u\n", pixel[0], pixel[1],
         pixel[2], pixel[3]);
  if (FAILED(hr) || pixel[0] > 2 || pixel[1] < 235 || pixel[2] > 2 ||
      pixel[3] != 255)
    pass = false;

  hr = swapchain->Present(0, 0);
  print_hr("UnityStabilityProbe Present(final)", hr);
  if (FAILED(hr))
    pass = false;

  DXGI_FRAME_STATISTICS frame_stats = {};
  hr = swapchain->GetFrameStatistics(&frame_stats);
  print_hr("UnityStabilityProbe GetFrameStatistics", hr);
  if (SUCCEEDED(hr)) {
    printf("UnityStabilityProbe frame_stats present=%u sync=%u qpc=%llu\n",
           frame_stats.PresentCount, frame_stats.PresentRefreshCount,
           (unsigned long long)frame_stats.SyncQPCTime.QuadPart);
  }

  elapsed_ms = GetTickCount64() - start_ms;

  SIZE_T private_after_kb = 0;
  SIZE_T working_set_after_kb = 0;
  bool leak_sampled_after = sample_process_memory_kb(
      "UnityLeakProbe after", &private_after_kb, &working_set_after_kb);
  if (leak_sampled_before && leak_sampled_after) {
    long long private_delta_kb =
        (long long)private_after_kb - (long long)private_before_kb;
    long long working_set_delta_kb =
        (long long)working_set_after_kb - (long long)working_set_before_kb;
    bool leak_ok = !leak_budget_kb ||
                   private_delta_kb <= (long long)leak_budget_kb;
    printf("UnityLeakProbe private_delta_kb=%lld working_set_delta_kb=%lld "
           "budget_kb=%u result=%s\n",
           private_delta_kb, working_set_delta_kb, leak_budget_kb,
           leak_ok ? "PASS" : "FAIL");
    if (!leak_ok)
      pass = false;
  } else {
    printf("UnityLeakProbe result=SKIP\n");
  }

  printf("UnityStabilityProbe presented_frames=%u elapsed_ms=%llu\n", presented,
         elapsed_ms);
  printf("UnityStabilityProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static HRESULT create_offscreen_target(ID3D11Device *device,
                                       ID3D11Texture2D **target) {
  *target = nullptr;

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  return device->CreateTexture2D(&desc, nullptr, target);
}

static bool probe_unity_feature_level_creation(
    D3D11CreateDeviceProc create_device) {
  struct FeatureLevelCase {
    D3D_FEATURE_LEVEL level;
    const char *label;
  };
  const FeatureLevelCase cases[] = {
      {D3D_FEATURE_LEVEL_11_1, "11_1"},
      {D3D_FEATURE_LEVEL_11_0, "11_0"},
      {D3D_FEATURE_LEVEL_10_1, "10_1"},
      {D3D_FEATURE_LEVEL_10_0, "10_0"},
  };

  bool pass = true;
  for (UINT i = 0; i < ARRAYSIZE(cases); i++) {
    D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               &cases[i].level, 1, D3D11_SDK_VERSION, &device,
                               &chosen, &ctx);
    printf("UnityFeatureLevelProbe request=%s chosen=0x%04x\n",
           cases[i].label, (unsigned)chosen);
    print_hr("UnityFeatureLevelProbe D3D11CreateDevice", hr);
    if (FAILED(hr) || chosen != cases[i].level)
      pass = false;
    if (ctx)
      ctx->Release();
    if (device)
      device->Release();
  }

  printf("UnityFeatureLevelProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_warp_device_creation(D3D11CreateDeviceProc create_device) {
  const D3D_FEATURE_LEVEL requested[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0;
  ID3D11Device *warp_device = nullptr;
  ID3D11DeviceContext *warp_ctx = nullptr;
  HRESULT hr = create_device(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested,
                             ARRAYSIZE(requested), D3D11_SDK_VERSION,
                             &warp_device, &chosen, &warp_ctx);
  print_hr("UnityWarpProbe D3D11CreateDevice(WARP)", hr);
  printf("UnityWarpProbe feature_level=0x%04x device=%p context=%p\n",
         (unsigned)chosen, warp_device, warp_ctx);

  bool pass = SUCCEEDED(hr) && warp_device && warp_ctx &&
              chosen >= D3D_FEATURE_LEVEL_10_0;
  if (warp_ctx)
    warp_ctx->Release();
  if (warp_device)
    warp_device->Release();

  printf("UnityWarpProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_d3d11on12_unsupported(D3D11On12CreateDeviceProc create_on12,
                                         IUnknown *device) {
  bool pass = true;

  ID3D11Device *wrapped_device = (ID3D11Device *)0x1;
  ID3D11DeviceContext *wrapped_ctx = (ID3D11DeviceContext *)0x1;
  D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0xdead;
  HRESULT hr = create_on12(nullptr, 0, nullptr, 0, nullptr, 0, 0,
                           &wrapped_device, &wrapped_ctx, &chosen);
  print_hr("D3D11On12Probe null_device", hr);
  printf("D3D11On12Probe null_outputs device=%p ctx=%p chosen=0x%04x\n",
         wrapped_device, wrapped_ctx, (unsigned)chosen);
  if (hr != E_INVALIDARG || wrapped_device || wrapped_ctx || chosen)
    pass = false;

  IUnknown *queues[] = {device};
  wrapped_device = (ID3D11Device *)0x1;
  wrapped_ctx = (ID3D11DeviceContext *)0x1;
  chosen = (D3D_FEATURE_LEVEL)0xdead;
  hr = create_on12(device, 0, nullptr, 0, queues, ARRAYSIZE(queues), 0,
                   &wrapped_device, &wrapped_ctx, &chosen);
  print_hr("D3D11On12Probe unsupported", hr);
  printf("D3D11On12Probe unsupported_outputs device=%p ctx=%p chosen=0x%04x\n",
         wrapped_device, wrapped_ctx, (unsigned)chosen);
  if (hr != DXGI_ERROR_UNSUPPORTED || wrapped_device || wrapped_ctx || chosen)
    pass = false;

  wrapped_device = (ID3D11Device *)0x1;
  chosen = (D3D_FEATURE_LEVEL)0xdead;
  hr = create_on12(device, 0, nullptr, 0, nullptr, 1, 0, &wrapped_device,
                   nullptr, &chosen);
  print_hr("D3D11On12Probe null_queue_array", hr);
  printf("D3D11On12Probe null_queue_outputs device=%p chosen=0x%04x\n",
         wrapped_device, (unsigned)chosen);
  if (hr != E_INVALIDARG || wrapped_device || chosen)
    pass = false;

  printf("D3D11On12Probe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_device_interfaces(ID3D11Device *device) {
  bool pass = true;
  ID3D11Device1 *device1 = nullptr;
  HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device1),
                                      (void **)&device1);
  print_hr("UnityDeviceInterfaceProbe QueryInterface(ID3D11Device1)", hr);
  if (FAILED(hr)) {
    pass = false;
  } else {
    ID3D11DeviceContext1 *ctx1 = nullptr;
    device1->GetImmediateContext1(&ctx1);
    printf("UnityDeviceInterfaceProbe GetImmediateContext1 ctx=%p\n", ctx1);
    if (!ctx1)
      pass = false;
    if (ctx1)
      ctx1->Release();

    ID3D11Device2 *device2 = nullptr;
    hr = device->QueryInterface(__uuidof(ID3D11Device2), (void **)&device2);
    print_hr("UnityDeviceInterfaceProbe QueryInterface(ID3D11Device2)", hr);
    if (FAILED(hr) || !device2) {
      pass = false;
    } else {
      UINT quality_levels = 0;
      hr = device2->CheckMultisampleQualityLevels1(
          DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0, &quality_levels);
      print_hr("UnityDeviceInterfaceProbe CheckMultisampleQualityLevels1(4x)",
               hr);
      printf("UnityDeviceInterfaceProbe msaa1_quality=%u\n", quality_levels);
      if (FAILED(hr) || !quality_levels)
        pass = false;

      quality_levels = 0xdeadbeef;
      hr = device2->CheckMultisampleQualityLevels1(
          DXGI_FORMAT_R8G8B8A8_UNORM, 4, 1, &quality_levels);
      print_hr(
          "UnityDeviceInterfaceProbe CheckMultisampleQualityLevels1(flags)", hr);
      printf("UnityDeviceInterfaceProbe msaa1_flags_quality=0x%08x\n",
             quality_levels);
      if (hr != E_INVALIDARG || quality_levels)
        pass = false;

      hr = device2->CheckMultisampleQualityLevels1(
          DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0, nullptr);
      print_hr("UnityDeviceInterfaceProbe CheckMultisampleQualityLevels1(null)",
               hr);
      if (hr != E_INVALIDARG)
        pass = false;
    }
    if (device2)
      device2->Release();
  }
  if (device1)
    device1->Release();

  UINT exception_mode = device->GetExceptionMode();
  printf("UnityDeviceInterfaceProbe GetExceptionMode(initial)=0x%08x\n",
         exception_mode);
  if (exception_mode != 0)
    pass = false;

  hr = device->SetExceptionMode(D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR);
  print_hr("UnityDeviceInterfaceProbe SetExceptionMode(driver_internal_error)",
           hr);
  exception_mode = device->GetExceptionMode();
  printf("UnityDeviceInterfaceProbe GetExceptionMode(updated)=0x%08x\n",
         exception_mode);
  if (FAILED(hr) ||
      exception_mode != D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR)
    pass = false;

  hr = device->SetExceptionMode(0x80000000u);
  print_hr("UnityDeviceInterfaceProbe SetExceptionMode(invalid)", hr);
  exception_mode = device->GetExceptionMode();
  printf("UnityDeviceInterfaceProbe GetExceptionMode(after_invalid)=0x%08x\n",
         exception_mode);
  if (hr != E_INVALIDARG ||
      exception_mode != D3D11_RAISE_FLAG_DRIVER_INTERNAL_ERROR)
    pass = false;

  hr = device->SetExceptionMode(0);
  print_hr("UnityDeviceInterfaceProbe SetExceptionMode(restore)", hr);
  exception_mode = device->GetExceptionMode();
  printf("UnityDeviceInterfaceProbe GetExceptionMode(restored)=0x%08x\n",
         exception_mode);
  if (FAILED(hr) || exception_mode != 0)
    pass = false;

  if (!probe_factory_unsupported_swapchains(device))
    pass = false;

  printf("UnityDeviceInterfaceProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_fence_and_context_state(ID3D11Device *device,
                                                ID3D11DeviceContext *ctx) {
  bool pass = true;

  ID3D11Device1 *device1 = nullptr;
  HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device1),
                                      (void **)&device1);
  print_hr("UnityFenceProbe QueryInterface(ID3D11Device1)", hr);
  if (FAILED(hr) || !device1)
    pass = false;

  if (device1) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                       D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0;
    ID3DDeviceContextState *state = nullptr;
    hr = device1->CreateDeviceContextState(
        0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        __uuidof(ID3D11DeviceContext), &chosen, &state);
    print_hr("UnityFenceProbe CreateDeviceContextState", hr);
    printf("UnityFenceProbe context_state=%p chosen=0x%04x\n", state,
           (unsigned)chosen);
    if (FAILED(hr) || !state || !chosen)
      pass = false;
    if (state) {
      ID3D11DeviceContext1 *ctx1 = nullptr;
      hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
      print_hr("UnityFenceProbe QueryInterface(ID3D11DeviceContext1)", hr);
      if (FAILED(hr) || !ctx1) {
        pass = false;
      } else {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3DDeviceContextState *previous_state = nullptr;
        ctx1->SwapDeviceContextState(state, &previous_state);
        printf("UnityFenceProbe SwapDeviceContextState(default) previous=%p\n",
               previous_state);
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ctx->IAGetPrimitiveTopology(&topology);
        printf("UnityFenceProbe topology_after_default=%u\n",
               (unsigned)topology);
        if (!previous_state ||
            topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
          pass = false;

        ctx1->SwapDeviceContextState(previous_state, nullptr);
        printf("UnityFenceProbe SwapDeviceContextState(restore)=PASS\n");
        ctx->IAGetPrimitiveTopology(&topology);
        printf("UnityFenceProbe topology_after_restore=%u\n",
               (unsigned)topology);
        if (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
          pass = false;
        if (previous_state)
          previous_state->Release();
        ctx1->Release();
      }
      state->Release();
    }
    device1->Release();
  }

  ID3D11Device5 *device5 = nullptr;
  hr = device->QueryInterface(__uuidof(ID3D11Device5), (void **)&device5);
  print_hr("UnityFenceProbe QueryInterface(ID3D11Device5)", hr);
  if (FAILED(hr) || !device5)
    pass = false;

  ID3D11DeviceContext4 *ctx4 = nullptr;
  hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4);
  print_hr("UnityFenceProbe QueryInterface(ID3D11DeviceContext4)", hr);
  if (FAILED(hr) || !ctx4)
    pass = false;

  if (ctx4) {
    BOOL protected_state = TRUE;
    ctx4->GetHardwareProtectionState(&protected_state);
    printf("UnityFenceProbe hardware_protection_initial=%u\n",
           protected_state);
    if (protected_state)
      pass = false;

    ctx4->SetHardwareProtectionState(TRUE);
    protected_state = FALSE;
    ctx4->GetHardwareProtectionState(&protected_state);
    printf("UnityFenceProbe hardware_protection_enabled=%u\n",
           protected_state);
    if (!protected_state)
      pass = false;

    ctx4->SetHardwareProtectionState(FALSE);
    protected_state = TRUE;
    ctx4->GetHardwareProtectionState(&protected_state);
    printf("UnityFenceProbe hardware_protection_restored=%u\n",
           protected_state);
    if (protected_state)
      pass = false;

    ctx4->GetHardwareProtectionState(nullptr);
    printf("UnityFenceProbe hardware_protection_null_get=PASS\n");
  }

  ID3D11Fence *fence = nullptr;
  if (device5) {
    hr = device5->CreateFence(3, D3D11_FENCE_FLAG_NONE,
                              __uuidof(ID3D11Fence), (void **)&fence);
    print_hr("UnityFenceProbe CreateFence", hr);
    printf("UnityFenceProbe fence=%p\n", fence);
    if (FAILED(hr) || !fence)
      pass = false;
  }

  HANDLE event = nullptr;
  if (fence && ctx4) {
    UINT64 completed = fence->GetCompletedValue();
    printf("UnityFenceProbe initial_completed=%llu\n",
           (unsigned long long)completed);
    if (completed != 3)
      pass = false;

    event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    printf("UnityFenceProbe event=%p gle=%lu\n", event, GetLastError());
    if (!event) {
      pass = false;
    } else {
      hr = fence->SetEventOnCompletion(7, event);
      print_hr("UnityFenceProbe SetEventOnCompletion(7)", hr);
      if (FAILED(hr))
        pass = false;
    }

    hr = ctx4->Signal(fence, 7);
    print_hr("UnityFenceProbe Signal(7)", hr);
    if (FAILED(hr))
      pass = false;

    if (event) {
      DWORD wait_result = WaitForSingleObject(event, 2000);
      printf("UnityFenceProbe wait_result=0x%08lx\n",
             (unsigned long)wait_result);
      if (wait_result != WAIT_OBJECT_0)
        pass = false;
    }

    completed = fence->GetCompletedValue();
    printf("UnityFenceProbe completed_after_signal=%llu\n",
           (unsigned long long)completed);
    if (completed < 7)
      pass = false;

    hr = ctx4->Wait(fence, 7);
    print_hr("UnityFenceProbe Wait(7)", hr);
    if (FAILED(hr))
      pass = false;
  }

  if (event)
    CloseHandle(event);
  if (fence)
    fence->Release();
  if (ctx4)
    ctx4->Release();
  if (device5)
    device5->Release();

  printf("UnityFenceProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_dxgi_output_capabilities(IDXGIOutput *output,
                                           ID3D11Device *device) {
  bool pass = true;

  IDXGIOutput6 *output6 = nullptr;
  HRESULT hr = output->QueryInterface(__uuidof(IDXGIOutput6),
                                      (void **)&output6);
  print_hr("UnityOutputCapabilityProbe QueryInterface(IDXGIOutput6)", hr);
  printf("UnityOutputCapabilityProbe output6=%p\n", output6);
  if (FAILED(hr) || !output6)
    return false;

  DXGI_OUTPUT_DESC1 desc1 = {};
  hr = output6->GetDesc1(&desc1);
  print_hr("UnityOutputCapabilityProbe GetDesc1", hr);
  printf("UnityOutputCapabilityProbe desc1 bpc=%u colorspace=%u luminance=%.1f\n",
         desc1.BitsPerColor, desc1.ColorSpace, desc1.MaxLuminance);
  if (FAILED(hr))
    pass = false;

  UINT flags = 0xffffffffu;
  hr = output6->CheckOverlaySupport(DXGI_FORMAT_R8G8B8A8_UNORM, device, &flags);
  print_hr("UnityOutputCapabilityProbe CheckOverlaySupport", hr);
  printf("UnityOutputCapabilityProbe overlay_flags=0x%08x\n", flags);
  if (FAILED(hr) || flags != 0)
    pass = false;

  BOOL overlays = output6->SupportsOverlays();
  printf("UnityOutputCapabilityProbe SupportsOverlays=%u\n", overlays);
  if (overlays)
    pass = false;

  flags = 0xffffffffu;
  hr = output6->CheckOverlayColorSpaceSupport(
      DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
      device, &flags);
  print_hr("UnityOutputCapabilityProbe CheckOverlayColorSpaceSupport", hr);
  printf("UnityOutputCapabilityProbe overlay_color_flags=0x%08x\n", flags);
  if (FAILED(hr) || flags != 0)
    pass = false;

  flags = 0xffffffffu;
  hr = output6->CheckHardwareCompositionSupport(&flags);
  print_hr("UnityOutputCapabilityProbe CheckHardwareCompositionSupport", hr);
  printf("UnityOutputCapabilityProbe hardware_composition_flags=0x%08x\n",
         flags);
  if (FAILED(hr) || flags != 0)
    pass = false;

  IDXGIOutputDuplication *duplication = (IDXGIOutputDuplication *)0x1;
  hr = output6->DuplicateOutput(nullptr, &duplication);
  print_hr("UnityOutputCapabilityProbe DuplicateOutput(null_device)", hr);
  printf("UnityOutputCapabilityProbe duplication_null=%p\n", duplication);
  if (hr != E_INVALIDARG || duplication)
    pass = false;

  duplication = (IDXGIOutputDuplication *)0x1;
  hr = output6->DuplicateOutput(device, &duplication);
  print_hr("UnityOutputCapabilityProbe DuplicateOutput", hr);
  printf("UnityOutputCapabilityProbe duplication=%p\n", duplication);
  if (hr != DXGI_ERROR_UNSUPPORTED || duplication)
    pass = false;

  DXGI_FORMAT duplicate_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  duplication = (IDXGIOutputDuplication *)0x1;
  hr = output6->DuplicateOutput1(device, 0, 1, &duplicate_format,
                                 &duplication);
  print_hr("UnityOutputCapabilityProbe DuplicateOutput1", hr);
  printf("UnityOutputCapabilityProbe duplication1=%p\n", duplication);
  if (hr != DXGI_ERROR_UNSUPPORTED || duplication)
    pass = false;

  output6->Release();
  printf("UnityOutputCapabilityProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_dxgi_adapter_notifications(IDXGIAdapter1 *adapter) {
  bool pass = true;

  IDXGIAdapter3 *adapter3 = nullptr;
  HRESULT hr = adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                       (void **)&adapter3);
  print_hr("UnityAdapterNotificationProbe QueryInterface(IDXGIAdapter3)", hr);
  printf("UnityAdapterNotificationProbe adapter3=%p\n", adapter3);
  if (FAILED(hr) || !adapter3)
    return false;

  DWORD cookie = 0xdeadbeefu;
  hr = adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(nullptr,
                                                                  &cookie);
  print_hr("UnityAdapterNotificationProbe RegisterVideoMemoryBudget(null_event)",
           hr);
  printf("UnityAdapterNotificationProbe budget_null_cookie=0x%08lx\n",
         (unsigned long)cookie);
  if (hr != DXGI_ERROR_INVALID_CALL || cookie)
    pass = false;

  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  printf("UnityAdapterNotificationProbe event=%p\n", event);
  if (!event) {
    pass = false;
  } else {
    hr = adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(event,
                                                                    nullptr);
    print_hr(
        "UnityAdapterNotificationProbe RegisterVideoMemoryBudget(null_cookie)",
        hr);
    if (hr != DXGI_ERROR_INVALID_CALL)
      pass = false;

    cookie = 0;
    hr = adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(event,
                                                                    &cookie);
    print_hr("UnityAdapterNotificationProbe RegisterVideoMemoryBudget", hr);
    printf("UnityAdapterNotificationProbe budget_cookie=0x%08lx\n",
           (unsigned long)cookie);
    if (FAILED(hr) || !cookie)
      pass = false;
    if (cookie)
      adapter3->UnregisterVideoMemoryBudgetChangeNotification(cookie);

    cookie = 0xdeadbeefu;
    hr = adapter3->RegisterHardwareContentProtectionTeardownStatusEvent(
        nullptr, &cookie);
    print_hr("UnityAdapterNotificationProbe RegisterHardwareProtection(null)",
             hr);
    printf("UnityAdapterNotificationProbe hardware_null_cookie=0x%08lx\n",
           (unsigned long)cookie);
    if (hr != DXGI_ERROR_INVALID_CALL || cookie)
      pass = false;

    cookie = 0;
    hr = adapter3->RegisterHardwareContentProtectionTeardownStatusEvent(
        event, &cookie);
    print_hr("UnityAdapterNotificationProbe RegisterHardwareProtection", hr);
    printf("UnityAdapterNotificationProbe hardware_cookie=0x%08lx\n",
           (unsigned long)cookie);
    if (FAILED(hr) || !cookie)
      pass = false;
    if (cookie)
      adapter3->UnregisterHardwareContentProtectionTeardownStatus(cookie);

    CloseHandle(event);
  }

  adapter3->Release();
  printf("UnityAdapterNotificationProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_dxgi_adapter_outputs(IDXGIFactory1 *factory) {
  bool pass = true;

  IDXGIAdapter1 *adapter = nullptr;
  HRESULT hr = factory->EnumAdapters1(0, &adapter);
  print_hr("UnityDeviceProbe IDXGIFactory1::EnumAdapters1(0)", hr);
  printf("UnityDeviceProbe adapter1=%p\n", adapter);
  if (FAILED(hr) || !adapter) {
    printf("UnityDeviceProbe result=FAIL\n");
    return false;
  }

  DXGI_ADAPTER_DESC1 adapter_desc = {};
  hr = adapter->GetDesc1(&adapter_desc);
  print_hr("UnityDeviceProbe IDXGIAdapter1::GetDesc1", hr);
  printf("UnityDeviceProbe adapter vendor=0x%04x device=0x%04x flags=0x%08x vram=%llu\n",
         adapter_desc.VendorId, adapter_desc.DeviceId, adapter_desc.Flags,
         (unsigned long long)adapter_desc.DedicatedVideoMemory);
  if (FAILED(hr))
    pass = false;

  if (!probe_dxgi_adapter_notifications(adapter))
    pass = false;

  IDXGIOutput *output = nullptr;
  hr = adapter->EnumOutputs(0, &output);
  print_hr("UnityDeviceProbe IDXGIAdapter::EnumOutputs(0)", hr);
  printf("UnityDeviceProbe output=%p\n", output);
  if (hr == DXGI_ERROR_NOT_FOUND) {
    printf("UnityDeviceProbe outputs=SKIP headless_no_monitor\n");
  } else if (FAILED(hr)) {
    pass = false;
  }

  if (output) {
    DXGI_OUTPUT_DESC output_desc = {};
    hr = output->GetDesc(&output_desc);
    print_hr("UnityDeviceProbe IDXGIOutput::GetDesc", hr);
    printf("UnityDeviceProbe output monitor=%p attached=%u desktop=%ld,%ld,%ld,%ld\n",
           output_desc.Monitor, output_desc.AttachedToDesktop,
           output_desc.DesktopCoordinates.left, output_desc.DesktopCoordinates.top,
           output_desc.DesktopCoordinates.right,
           output_desc.DesktopCoordinates.bottom);
    if (FAILED(hr))
      pass = false;

    UINT mode_count = 0;
    hr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                                    &mode_count, nullptr);
    print_hr("UnityDeviceProbe IDXGIOutput::GetDisplayModeList(count)", hr);
    printf("UnityDeviceProbe display_mode_count=%u\n", mode_count);
    if (FAILED(hr)) {
      pass = false;
    } else if (mode_count) {
      DXGI_MODE_DESC *modes =
          (DXGI_MODE_DESC *)calloc(mode_count, sizeof(*modes));
      if (!modes) {
        printf("UnityDeviceProbe display_mode_alloc=FAIL count=%u\n",
               mode_count);
        pass = false;
      } else {
        UINT fetch_count = mode_count;
        hr = output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                                        &fetch_count, modes);
        print_hr("UnityDeviceProbe IDXGIOutput::GetDisplayModeList(fetch)",
                 hr);
        printf("UnityDeviceProbe first_mode=%ux%u refresh=%u/%u fetched=%u\n",
               modes[0].Width, modes[0].Height,
               modes[0].RefreshRate.Numerator,
               modes[0].RefreshRate.Denominator, fetch_count);
        if (FAILED(hr))
          pass = false;
        free(modes);
      }
    }

    output->Release();
  }

  adapter->Release();
  printf("UnityDeviceProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_output_capabilities(
    CreateDXGIFactory1Proc create_factory, ID3D11Device *device) {
  if (!create_factory || !device)
    return false;

  bool pass = true;
  IDXGIFactory1 *factory = nullptr;
  HRESULT hr = create_factory(__uuidof(IDXGIFactory1), (void **)&factory);
  print_hr("UnityOutputCapabilityProbe CreateDXGIFactory1", hr);
  printf("UnityOutputCapabilityProbe factory=%p\n", factory);
  if (FAILED(hr) || !factory)
    return false;

  IDXGIAdapter1 *adapter = nullptr;
  hr = factory->EnumAdapters1(0, &adapter);
  print_hr("UnityOutputCapabilityProbe EnumAdapters1", hr);
  printf("UnityOutputCapabilityProbe adapter=%p\n", adapter);
  if (FAILED(hr) || !adapter) {
    pass = false;
  } else {
    IDXGIOutput *output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    print_hr("UnityOutputCapabilityProbe EnumOutputs", hr);
    printf("UnityOutputCapabilityProbe output=%p\n", output);
    if (hr == DXGI_ERROR_NOT_FOUND) {
      printf("UnityOutputCapabilityProbe result=SKIP headless_no_monitor\n");
    } else if (FAILED(hr) || !output) {
      pass = false;
    } else {
      pass = probe_dxgi_output_capabilities(output, device) && pass;
      output->Release();
    }
    adapter->Release();
  }

  factory->Release();
  printf("UnityOutputCapabilityProbe final=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

struct UnityFormatProbe {
  DXGI_FORMAT format;
  const char *name;
  UINT required;
  bool optional;
};

static bool probe_unity_formats(ID3D11Device *device) {
  static const UnityFormatProbe probes[] = {
      {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_R8G8B8A8_TYPELESS, "R8G8B8A8_TYPELESS",
       D3D11_FORMAT_SUPPORT_TEXTURE2D,
       false},
      {DXGI_FORMAT_B8G8R8A8_TYPELESS, "B8G8R8A8_TYPELESS",
       D3D11_FORMAT_SUPPORT_TEXTURE2D,
       false},
      {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_R16G16_FLOAT, "R16G16_FLOAT",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
           D3D11_FORMAT_SUPPORT_RENDER_TARGET,
       false},
      {DXGI_FORMAT_R32_FLOAT, "R32_FLOAT",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       false},
      {DXGI_FORMAT_R16_FLOAT, "R16_FLOAT",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       false},
      {DXGI_FORMAT_R8_UNORM, "R8_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       false},
      {DXGI_FORMAT_R8G8_UNORM, "R8G8_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       false},
      {DXGI_FORMAT_R8G8_B8G8_UNORM, "R8G8_B8G8_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_G8R8_G8B8_UNORM, "G8R8_G8B8_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL,
       false},
      {DXGI_FORMAT_R24G8_TYPELESS, "R24G8_TYPELESS",
       D3D11_FORMAT_SUPPORT_TEXTURE2D,
       false},
      {DXGI_FORMAT_D32_FLOAT, "D32_FLOAT",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL,
       false},
      {DXGI_FORMAT_BC1_UNORM, "BC1_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC1_UNORM_SRGB, "BC1_UNORM_SRGB",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC2_UNORM, "BC2_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC2_UNORM_SRGB, "BC2_UNORM_SRGB",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC3_UNORM, "BC3_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC3_UNORM_SRGB, "BC3_UNORM_SRGB",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC4_UNORM, "BC4_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC4_SNORM, "BC4_SNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC5_UNORM, "BC5_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC5_SNORM, "BC5_SNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC6H_UF16, "BC6H_UF16",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC6H_SF16, "BC6H_SF16",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC7_UNORM, "BC7_UNORM",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
      {DXGI_FORMAT_BC7_UNORM_SRGB, "BC7_UNORM_SRGB",
       D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
       true},
  };

  bool pass = true;
  for (UINT i = 0; i < ARRAYSIZE(probes); i++) {
    UINT support = 0;
    HRESULT hr = device->CheckFormatSupport(probes[i].format, &support);
    bool ok = SUCCEEDED(hr) && ((support & probes[i].required) == probes[i].required);
    printf("UnityFormatProbe %s hr=0x%08lx support=0x%08x required=0x%08x result=%s%s\n",
           probes[i].name, (unsigned long)(uint32_t)hr, support,
           probes[i].required, ok ? "PASS" : "FAIL",
           probes[i].optional ? " optional" : "");
    if (!probes[i].optional && !ok)
      pass = false;
  }

  return pass;
}

static bool probe_unity_features(ID3D11Device *device) {
  bool pass = true;

  D3D11_FEATURE_DATA_THREADING threading = {};
  HRESULT hr = device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading,
                                           sizeof(threading));
  printf("UnityFeatureProbe THREADING hr=0x%08lx concurrent=%u command_lists=%u\n",
         (unsigned long)(uint32_t)hr, threading.DriverConcurrentCreates,
         threading.DriverCommandLists);
  if (FAILED(hr) || !threading.DriverConcurrentCreates ||
      !threading.DriverCommandLists)
    pass = false;

  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
  hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                   sizeof(options));
  printf("UnityFeatureProbe D3D11_OPTIONS hr=0x%08lx clear_view=%u cb_offset=%u cb_partial=%u cb_no_overwrite=%u copy_overlap=%u forced_sample=%u srv_no_overwrite=%u uav_forced_sample=%u discard_seen=%u extended_share=%u sad4=%u flags_seen=%u logic_op=%u\n",
         (unsigned long)(uint32_t)hr, options.ClearView,
         options.ConstantBufferOffsetting, options.ConstantBufferPartialUpdate,
         options.MapNoOverwriteOnDynamicConstantBuffer, options.CopyWithOverlap,
         options.MultisampleRTVWithForcedSampleCountOne,
         options.MapNoOverwriteOnDynamicBufferSRV,
         options.UAVOnlyRenderingForcedSampleCount,
         options.DiscardAPIsSeenByDriver, options.ExtendedResourceSharing,
         options.SAD4ShaderInstructions,
         options.FlagsForUpdateAndCopySeenByDriver,
         options.OutputMergerLogicOp);
  if (FAILED(hr) || !options.ClearView || !options.ConstantBufferOffsetting ||
      !options.ConstantBufferPartialUpdate ||
      !options.MapNoOverwriteOnDynamicConstantBuffer ||
      !options.CopyWithOverlap ||
      !options.MultisampleRTVWithForcedSampleCountOne ||
      !options.MapNoOverwriteOnDynamicBufferSRV ||
      !options.UAVOnlyRenderingForcedSampleCount ||
      !options.DiscardAPIsSeenByDriver || !options.ExtendedResourceSharing ||
      !options.SAD4ShaderInstructions ||
      !options.FlagsForUpdateAndCopySeenByDriver)
    pass = false;

  D3D11_FEATURE_DATA_ARCHITECTURE_INFO arch = {};
  hr = device->CheckFeatureSupport(D3D11_FEATURE_ARCHITECTURE_INFO, &arch,
                                   sizeof(arch));
  printf("UnityFeatureProbe ARCHITECTURE_INFO hr=0x%08lx tile_based=%u\n",
         (unsigned long)(uint32_t)hr, arch.TileBasedDeferredRenderer);
  if (FAILED(hr))
    pass = false;

  D3D11_FEATURE_DATA_DOUBLES doubles = {};
  hr = device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles,
                                   sizeof(doubles));
  printf("UnityFeatureProbe DOUBLES hr=0x%08lx fp64_shader_ops=%u\n",
         (unsigned long)(uint32_t)hr, doubles.DoublePrecisionFloatShaderOps);
  if (FAILED(hr) || doubles.DoublePrecisionFloatShaderOps)
    pass = false;

  D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS d3d10x = {};
  hr = device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS,
                                   &d3d10x, sizeof(d3d10x));
  printf("UnityFeatureProbe D3D10_X_HARDWARE_OPTIONS hr=0x%08lx compute_raw_structured=%u\n",
         (unsigned long)(uint32_t)hr,
         d3d10x.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x);
  if (FAILED(hr) || !d3d10x.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
    pass = false;

  UINT msaa_quality = 0;
  hr = device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                                             &msaa_quality);
  print_hr("UnityFeatureProbe CheckMultisampleQualityLevels(R8G8B8A8,4)", hr);
  printf("UnityFeatureProbe msaa4_quality=%u\n", msaa_quality);
  if (FAILED(hr) || !msaa_quality)
    pass = false;

  const DXGI_FORMAT msaa_formats[] = {
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R16G16B16A16_FLOAT,
  };
  const UINT msaa_counts[] = {1, 2, 4, 8};
  for (UINT f = 0; f < ARRAYSIZE(msaa_formats); f++) {
    for (UINT c = 0; c < ARRAYSIZE(msaa_counts); c++) {
      UINT quality = 0;
      hr = device->CheckMultisampleQualityLevels(msaa_formats[f],
                                                 msaa_counts[c], &quality);
      printf("UnityFeatureProbe MSAA format=%u samples=%u hr=0x%08lx quality=%u\n",
             msaa_formats[f], msaa_counts[c], (unsigned long)(uint32_t)hr,
             quality);
      if (FAILED(hr))
        pass = false;
      if (msaa_counts[c] <= 4 && !quality)
        pass = false;
      if (msaa_counts[c] == 8 && quality)
        pass = false;
    }
  }

  printf("UnityFeatureProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_counters(ID3D11Device *device) {
  bool pass = true;

  D3D11_COUNTER_INFO info = {};
  device->CheckCounterInfo(&info);
  printf("UnityCounterProbe CheckCounterInfo last=0x%08x simultaneous=%u "
         "parallel=%u\n",
         info.LastDeviceDependentCounter, info.NumSimultaneousCounters,
         info.NumDetectableParallelUnits);
  if (info.LastDeviceDependentCounter != D3D11_COUNTER_DEVICE_DEPENDENT_0 ||
      info.NumSimultaneousCounters != 0 ||
      info.NumDetectableParallelUnits != 0)
    pass = false;

  D3D11_COUNTER_DESC desc = {};
  desc.Counter = D3D11_COUNTER_DEVICE_DEPENDENT_0;

  D3D11_COUNTER_TYPE type = D3D11_COUNTER_TYPE_FLOAT32;
  UINT active_counters = 0xffffffffu;
  char name[16] = {'x'};
  char units[16] = {'x'};
  char description[16] = {'x'};
  UINT name_length = ARRAYSIZE(name);
  UINT units_length = ARRAYSIZE(units);
  UINT description_length = ARRAYSIZE(description);
  HRESULT hr = device->CheckCounter(&desc, &type, &active_counters, name,
                                    &name_length, units, &units_length,
                                    description, &description_length);
  print_hr("UnityCounterProbe CheckCounter(device_dependent_0)", hr);
  printf("UnityCounterProbe counter_type=%u active=%u name_len=%u "
         "units_len=%u desc_len=%u\n",
         type, active_counters, name_length, units_length,
         description_length);
  if (hr != DXGI_ERROR_UNSUPPORTED || type != D3D11_COUNTER_TYPE_UINT32 ||
      active_counters != 0 || name_length != 0 || units_length != 0 ||
      description_length != 0 || name[0] != '\0' || units[0] != '\0' ||
      description[0] != '\0')
    pass = false;

  ID3D11Counter *counter = nullptr;
  hr = device->CreateCounter(&desc, &counter);
  print_hr("UnityCounterProbe CreateCounter(device_dependent_0)", hr);
  printf("UnityCounterProbe counter_ptr=%p\n", counter);
  if (hr != DXGI_ERROR_UNSUPPORTED || counter)
    pass = false;
  if (counter)
    counter->Release();

  printf("UnityCounterProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_tiled_resources_unsupported(
    ID3D11Device *device, ID3D11DeviceContext *ctx, ID3D11Resource *resource) {
  bool pass = true;

  ID3D11Device2 *device2 = nullptr;
  HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device2),
                                      (void **)&device2);
  print_hr("UnityTiledProbe QueryInterface(ID3D11Device2)", hr);
  if (FAILED(hr) || !device2)
    return false;

  ID3D11DeviceContext2 *ctx2 = nullptr;
  hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext2), (void **)&ctx2);
  print_hr("UnityTiledProbe QueryInterface(ID3D11DeviceContext2)", hr);
  if (FAILED(hr) || !ctx2) {
    device2->Release();
    return false;
  }

  UINT tile_count = 0xdeadbeefu;
  D3D11_PACKED_MIP_DESC mip_desc = {0xffu, 0xffu, 0xffffffffu,
                                    0xffffffffu};
  D3D11_TILE_SHAPE tile_shape = {0xffffffffu, 0xffffffffu, 0xffffffffu};
  UINT subresource_count = 1;
  D3D11_SUBRESOURCE_TILING subresource = {0xffffffffu, 0xffffu, 0xffffu,
                                          0xffffffffu};
  device2->GetResourceTiling(resource, &tile_count, &mip_desc, &tile_shape,
                             &subresource_count, 0, &subresource);
  printf("UnityTiledProbe tiling tiles=%u subresources=%u mip_tiles=%u shape=%ux%ux%u\n",
         tile_count, subresource_count, mip_desc.NumStandardMips,
         tile_shape.WidthInTexels, tile_shape.HeightInTexels,
         tile_shape.DepthInTexels);
  if (tile_count || subresource_count || mip_desc.NumStandardMips ||
      mip_desc.NumPackedMips || mip_desc.NumTilesForPackedMips ||
      mip_desc.StartTileIndexInOverallResource ||
      tile_shape.WidthInTexels || tile_shape.HeightInTexels ||
      tile_shape.DepthInTexels)
    pass = false;

  hr = ctx2->UpdateTileMappings(resource, 0, nullptr, nullptr, nullptr, 0,
                                nullptr, nullptr, nullptr, 0);
  print_hr("UnityTiledProbe UpdateTileMappings", hr);
  if (hr != DXGI_ERROR_UNSUPPORTED)
    pass = false;

  hr = ctx2->CopyTileMappings(resource, nullptr, resource, nullptr, nullptr, 0);
  print_hr("UnityTiledProbe CopyTileMappings", hr);
  if (hr != DXGI_ERROR_UNSUPPORTED)
    pass = false;

  hr = ctx2->ResizeTilePool(nullptr, 0);
  print_hr("UnityTiledProbe ResizeTilePool", hr);
  if (hr != DXGI_ERROR_UNSUPPORTED)
    pass = false;

  ctx2->CopyTiles(resource, nullptr, nullptr, nullptr, 0, 0);
  printf("UnityTiledProbe CopyTiles=PASS\n");
  ctx2->UpdateTiles(resource, nullptr, nullptr, nullptr, 0);
  printf("UnityTiledProbe UpdateTiles=PASS\n");
  ctx2->TiledResourceBarrier(nullptr, nullptr);
  printf("UnityTiledProbe TiledResourceBarrier=PASS\n");

  ctx2->Release();
  device2->Release();
  printf("UnityTiledProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_resources(ID3D11Device *device,
                                  ID3D11DeviceContext *ctx) {
  bool pass = true;

  uint32_t texels[16] = {};
  for (UINT i = 0; i < ARRAYSIZE(texels); i++)
    texels[i] = 0xffdf8020u;

  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = 4;
  tex_desc.Height = 4;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_DEFAULT;
  tex_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

  D3D11_SUBRESOURCE_DATA tex_data = {};
  tex_data.pSysMem = texels;
  tex_data.SysMemPitch = 4 * sizeof(uint32_t);

  ID3D11Texture2D *color_tex = nullptr;
  HRESULT hr = device->CreateTexture2D(&tex_desc, &tex_data, &color_tex);
  print_hr("UnityResourceProbe CreateTexture2D(color)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ShaderResourceView *srv = nullptr;
  if (color_tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = tex_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(color_tex, &srv_desc, &srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(color)", hr);
    if (FAILED(hr))
      pass = false;
  }

  ID3D11RenderTargetView *rtv = nullptr;
  if (color_tex) {
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = tex_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = device->CreateRenderTargetView(color_tex, &rtv_desc, &rtv);
    print_hr("UnityResourceProbe CreateRenderTargetView(color)", hr);
    if (FAILED(hr))
      pass = false;
  }

  if (color_tex &&
      !probe_dxgi_resource_metadata(
          color_tex,
          DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT, "color"))
    pass = false;
  if (color_tex && !probe_unity_tiled_resources_unsupported(device, ctx, color_tex))
    pass = false;

  ID3D11Device3 *device3 = nullptr;
  hr = device->QueryInterface(__uuidof(ID3D11Device3), (void **)&device3);
  print_hr("UnityResourceProbe QueryInterface(ID3D11Device3)", hr);
  if (FAILED(hr) || !device3)
    pass = false;

  ID3D11DeviceContext1 *ctx1 = nullptr;
  hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
  print_hr("UnityResourceProbe QueryInterface(ID3D11DeviceContext1)", hr);
  if (FAILED(hr)) {
    pass = false;
  } else {
    if (rtv) {
      const FLOAT clear_view_color[4] = {0.5f, 0.25f, 0.125f, 1.0f};
      D3D11_RECT clear_rect = {0, 0, 2, 2};
      ctx1->ClearView(rtv, clear_view_color, &clear_rect, 1);
      printf("UnityResourceProbe ClearView(color_rect)=PASS\n");
      ctx1->DiscardView(rtv);
      printf("UnityResourceProbe DiscardView(color)=PASS\n");
      ctx1->DiscardView1(rtv, &clear_rect, 1);
      printf("UnityResourceProbe DiscardView1(color_rect)=PASS\n");
    }
    if (color_tex) {
      ctx1->DiscardResource(color_tex);
      printf("UnityResourceProbe DiscardResource(color)=PASS\n");
    }
  }

  if (color_tex) {
    texels[0] = 0xff2040ffu;
    ctx->UpdateSubresource(color_tex, 0, nullptr, texels, 4 * sizeof(uint32_t),
                           0);
    printf("UnityResourceProbe UpdateSubresource(color)=PASS\n");

    D3D11_TEXTURE2D_DESC staging_desc = tex_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    print_hr("UnityResourceProbe CreateTexture2D(staging)", hr);
    if (FAILED(hr))
      pass = false;

    if (staging) {
      ctx->CopyResource(staging, color_tex);
      printf("UnityResourceProbe CopyResource(color_to_staging)=PASS\n");
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
      print_hr("UnityResourceProbe Map(staging)", hr);
      if (SUCCEEDED(hr)) {
        const uint32_t first = *(const uint32_t *)mapped.pData;
        printf("UnityResourceProbe staging_pixel0=0x%08x\n", first);
        ctx->Unmap(staging, 0);
      } else {
        pass = false;
      }
      staging->Release();
    }

    if (device3) {
      uint32_t device3_texels[4] = {
          0xff1020a0u,
          0xff1020b0u,
          0xff1020c0u,
          0xff1020d0u,
      };
      D3D11_BOX write_box = {1, 1, 0, 3, 3, 1};
      hr = ctx->Map(color_tex, 0, D3D11_MAP_WRITE, 0, nullptr);
      print_hr("UnityResourceProbe Map(default_texture_null_write)", hr);
      if (SUCCEEDED(hr)) {
        device3->WriteToSubresource(color_tex, 0, &write_box, device3_texels,
                                    2 * sizeof(uint32_t), 0);
        printf("UnityResourceProbe WriteToSubresource(texture_box)=PASS\n");
        ctx->Unmap(color_tex, 0);
      } else {
        pass = false;
      }

      uint32_t readback_texels[16] = {};
      hr = ctx->Map(color_tex, 0, D3D11_MAP_READ, 0, nullptr);
      print_hr("UnityResourceProbe Map(default_texture_null_read)", hr);
      if (SUCCEEDED(hr)) {
        device3->ReadFromSubresource(readback_texels, 4 * sizeof(uint32_t), 0,
                                     color_tex, 0, nullptr);
        printf("UnityResourceProbe ReadFromSubresource(texture) p11=0x%08x p12=0x%08x p21=0x%08x p22=0x%08x\n",
               readback_texels[5], readback_texels[6], readback_texels[9],
               readback_texels[10]);
        ctx->Unmap(color_tex, 0);
      } else {
        pass = false;
      }
      if (readback_texels[5] != device3_texels[0] ||
          readback_texels[6] != device3_texels[1] ||
          readback_texels[9] != device3_texels[2] ||
          readback_texels[10] != device3_texels[3])
        pass = false;
    }
  }

  uint32_t texels_1d[2][4] = {};
  D3D11_SUBRESOURCE_DATA tex_data_1d[2] = {};
  for (UINT slice = 0; slice < ARRAYSIZE(texels_1d); slice++) {
    for (UINT i = 0; i < ARRAYSIZE(texels_1d[slice]); i++)
      texels_1d[slice][i] = 0xff100020u | (slice << 12) | i;
    tex_data_1d[slice].pSysMem = texels_1d[slice];
    tex_data_1d[slice].SysMemPitch = 4 * sizeof(uint32_t);
  }
  D3D11_TEXTURE1D_DESC tex1d_desc = {};
  tex1d_desc.Width = 4;
  tex1d_desc.MipLevels = 1;
  tex1d_desc.ArraySize = 2;
  tex1d_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex1d_desc.Usage = D3D11_USAGE_IMMUTABLE;
  tex1d_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture1D *tex1d = nullptr;
  hr = device->CreateTexture1D(&tex1d_desc, tex_data_1d, &tex1d);
  print_hr("UnityResourceProbe CreateTexture1D(array)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *tex1d_srv = nullptr;
  if (tex1d) {
    D3D11_SHADER_RESOURCE_VIEW_DESC tex1d_srv_desc = {};
    tex1d_srv_desc.Format = tex1d_desc.Format;
    tex1d_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY;
    tex1d_srv_desc.Texture1DArray.ArraySize = tex1d_desc.ArraySize;
    tex1d_srv_desc.Texture1DArray.MipLevels = 1;
    hr = device->CreateShaderResourceView(tex1d, &tex1d_srv_desc, &tex1d_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(texture1d_array)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  uint32_t texels_1d_mip[4] = {};
  for (UINT i = 0; i < ARRAYSIZE(texels_1d_mip); i++)
    texels_1d_mip[i] = 0xff804020u;
  D3D11_TEXTURE1D_DESC tex1d_mip_desc = {};
  tex1d_mip_desc.Width = 4;
  tex1d_mip_desc.MipLevels = 2;
  tex1d_mip_desc.ArraySize = 1;
  tex1d_mip_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex1d_mip_desc.Usage = D3D11_USAGE_DEFAULT;
  tex1d_mip_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  tex1d_mip_desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ID3D11Texture1D *tex1d_mip = nullptr;
  hr = device->CreateTexture1D(&tex1d_mip_desc, nullptr, &tex1d_mip);
  print_hr("UnityResourceProbe CreateTexture1D(mip_chain)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *tex1d_mip_srv = nullptr;
  if (tex1d_mip) {
    D3D11_SHADER_RESOURCE_VIEW_DESC tex1d_mip_srv_desc = {};
    tex1d_mip_srv_desc.Format = tex1d_mip_desc.Format;
    tex1d_mip_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
    tex1d_mip_srv_desc.Texture1D.MipLevels = tex1d_mip_desc.MipLevels;
    hr = device->CreateShaderResourceView(tex1d_mip, &tex1d_mip_srv_desc,
                                          &tex1d_mip_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(texture1d_mip_chain)",
             hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      ctx->UpdateSubresource(tex1d_mip, 0, nullptr, texels_1d_mip,
                             tex1d_mip_desc.Width * sizeof(uint32_t), 0);
      ctx->GenerateMips(tex1d_mip_srv);
      printf("UnityResourceProbe GenerateMips(texture1d_mip_chain)=PASS\n");
    }
  }
  ID3D11Texture1D *tex1d_mip_staging = nullptr;
  if (tex1d_mip) {
    D3D11_TEXTURE1D_DESC staging_desc = tex1d_mip_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    hr = device->CreateTexture1D(&staging_desc, nullptr, &tex1d_mip_staging);
    print_hr("UnityResourceProbe CreateTexture1D(texture1d_mip_staging)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      ctx->CopyResource(tex1d_mip_staging, tex1d_mip);
      ctx->Flush();
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      const UINT mip1 = D3D11CalcSubresource(1, 0, tex1d_mip_desc.MipLevels);
      hr = ctx->Map(tex1d_mip_staging, mip1, D3D11_MAP_READ, 0, &mapped);
      print_hr("UnityResourceProbe Map(texture1d_mip1)", hr);
      if (SUCCEEDED(hr)) {
        const uint8_t *src = (const uint8_t *)mapped.pData;
        printf("UnityResourceProbe texture1d_mip1_rgba=%u,%u,%u,%u\n",
               src[0], src[1], src[2], src[3]);
        if (src[0] != 0x20 || src[1] != 0x40 || src[2] != 0x80 ||
            src[3] != 0xff)
          pass = false;
        ctx->Unmap(tex1d_mip_staging, mip1);
      } else {
        pass = false;
      }
    }
  }

  ID3D11Texture2D *copy_tex = nullptr;
  if (color_tex) {
    hr = device->CreateTexture2D(&tex_desc, nullptr, &copy_tex);
    print_hr("UnityResourceProbe CreateTexture2D(copy_region_dst)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      D3D11_BOX box = {0, 0, 0, 2, 2, 1};
      ctx->CopySubresourceRegion(copy_tex, 0, 1, 1, 0, color_tex, 0, &box);
      printf("UnityResourceProbe CopySubresourceRegion(color_box)=PASS\n");
    }
  }

  D3D11_TEXTURE2D_DESC mip_desc = tex_desc;
  mip_desc.MipLevels = 2;
  mip_desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ID3D11Texture2D *mip_tex = nullptr;
  hr = device->CreateTexture2D(&mip_desc, nullptr, &mip_tex);
  print_hr("UnityResourceProbe CreateTexture2D(mip_chain)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *mip_srv = nullptr;
  if (mip_tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC mip_srv_desc = {};
    mip_srv_desc.Format = mip_desc.Format;
    mip_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    mip_srv_desc.Texture2D.MipLevels = mip_desc.MipLevels;
    hr = device->CreateShaderResourceView(mip_tex, &mip_srv_desc, &mip_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(mip_chain)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      ctx->UpdateSubresource(mip_tex, 0, nullptr, texels,
                             4 * sizeof(uint32_t), 0);
      ctx->GenerateMips(mip_srv);
      printf("UnityResourceProbe GenerateMips(mip_chain)=PASS\n");
      uint8_t mip0_pixel[4] = {};
      hr = read_first_pixel_subresource(device, ctx, mip_tex, 0, mip0_pixel);
      print_hr("UnityResourceProbe Readback(mip0)", hr);
      printf("UnityResourceProbe mip0_rgba=%u,%u,%u,%u\n", mip0_pixel[0],
             mip0_pixel[1], mip0_pixel[2], mip0_pixel[3]);
      if (FAILED(hr) || mip0_pixel[0] != 255 || mip0_pixel[1] != 64 ||
          mip0_pixel[2] != 32 || mip0_pixel[3] != 255)
        pass = false;

      uint8_t mip_pixel[4] = {};
      hr = read_first_pixel_subresource(device, ctx, mip_tex, 1, mip_pixel);
      print_hr("UnityResourceProbe Readback(mip1)", hr);
      printf("UnityResourceProbe mip1_rgba=%u,%u,%u,%u\n", mip_pixel[0],
             mip_pixel[1], mip_pixel[2], mip_pixel[3]);
      if (FAILED(hr) || mip_pixel[0] < 87 || mip_pixel[0] > 89 ||
          mip_pixel[1] < 111 || mip_pixel[1] > 113 ||
          mip_pixel[2] < 174 || mip_pixel[2] > 176 || mip_pixel[3] != 255)
        pass = false;
    }
  }

  D3D11_BUFFER_DESC dynamic_desc = {};
  dynamic_desc.ByteWidth = 256;
  dynamic_desc.Usage = D3D11_USAGE_DYNAMIC;
  dynamic_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  dynamic_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ID3D11Buffer *dynamic_buffer = nullptr;
  hr = device->CreateBuffer(&dynamic_desc, nullptr, &dynamic_buffer);
  print_hr("UnityResourceProbe CreateBuffer(dynamic_vertex)", hr);
  if (FAILED(hr))
    pass = false;
  if (dynamic_buffer) {
    if (!probe_dxgi_resource_metadata(dynamic_buffer, 0, "dynamic_vertex"))
      pass = false;

    hr = ctx->Map(dynamic_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, nullptr);
    print_hr("UnityResourceProbe Map(dynamic_vertex_null)", hr);
    if (hr != E_INVALIDARG)
      pass = false;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(dynamic_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    print_hr("UnityResourceProbe Map(dynamic_vertex)", hr);
    if (SUCCEEDED(hr)) {
      memset(mapped.pData, 0x5a, dynamic_desc.ByteWidth);
      ctx->Unmap(dynamic_buffer, 0);
      D3D11_MAPPED_SUBRESOURCE no_overwrite = {};
      hr = ctx->Map(dynamic_buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                    &no_overwrite);
      print_hr("UnityResourceProbe Map(dynamic_vertex_no_overwrite)", hr);
      if (SUCCEEDED(hr)) {
        memset(no_overwrite.pData, 0xa5, 64);
        ctx->Unmap(dynamic_buffer, 0);
      } else {
        pass = false;
      }
    } else {
      pass = false;
    }
  }

  D3D11_BUFFER_DESC dynamic_constant_desc = {};
  dynamic_constant_desc.ByteWidth = 256;
  dynamic_constant_desc.Usage = D3D11_USAGE_DYNAMIC;
  dynamic_constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  dynamic_constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ID3D11Buffer *dynamic_constant_buffer = nullptr;
  hr = device->CreateBuffer(&dynamic_constant_desc, nullptr,
                            &dynamic_constant_buffer);
  print_hr("UnityResourceProbe CreateBuffer(dynamic_constant)", hr);
  if (FAILED(hr))
    pass = false;
  if (dynamic_constant_buffer) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(dynamic_constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
                  &mapped);
    print_hr("UnityResourceProbe Map(dynamic_constant)", hr);
    if (SUCCEEDED(hr)) {
      memset(mapped.pData, 0x6c, dynamic_constant_desc.ByteWidth);
      ctx->Unmap(dynamic_constant_buffer, 0);

      D3D11_MAPPED_SUBRESOURCE no_overwrite = {};
      hr = ctx->Map(dynamic_constant_buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                    &no_overwrite);
      print_hr("UnityResourceProbe Map(dynamic_constant_no_overwrite)", hr);
      if (SUCCEEDED(hr)) {
        memset(no_overwrite.pData, 0xc6, 64);
        ctx->Unmap(dynamic_constant_buffer, 0);
      } else {
        pass = false;
      }
    } else {
      pass = false;
    }
  }

  uint32_t partial_constant_initial[16] = {};
  uint32_t partial_constant_update[4] = {
      0xc0ffee10u,
      0xc0ffee11u,
      0xc0ffee12u,
      0xc0ffee13u,
  };
  for (UINT i = 0; i < ARRAYSIZE(partial_constant_initial); i++)
    partial_constant_initial[i] = 0xcb000000u | i;
  D3D11_SUBRESOURCE_DATA partial_constant_data = {};
  partial_constant_data.pSysMem = partial_constant_initial;
  D3D11_BUFFER_DESC partial_constant_desc = {};
  partial_constant_desc.ByteWidth = sizeof(partial_constant_initial);
  partial_constant_desc.Usage = D3D11_USAGE_DEFAULT;
  partial_constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer *partial_constant_buffer = nullptr;
  hr = device->CreateBuffer(&partial_constant_desc, &partial_constant_data,
                            &partial_constant_buffer);
  print_hr("UnityResourceProbe CreateBuffer(partial_constant)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC partial_constant_staging_desc = partial_constant_desc;
  partial_constant_staging_desc.Usage = D3D11_USAGE_STAGING;
  partial_constant_staging_desc.BindFlags = 0;
  partial_constant_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *partial_constant_staging = nullptr;
  hr = device->CreateBuffer(&partial_constant_staging_desc, nullptr,
                            &partial_constant_staging);
  print_hr("UnityResourceProbe CreateBuffer(partial_constant_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (partial_constant_buffer && partial_constant_staging) {
    D3D11_BOX update_box = {};
    update_box.left = 4 * sizeof(uint32_t);
    update_box.right = update_box.left + sizeof(partial_constant_update);
    update_box.bottom = 1;
    update_box.back = 1;
    ctx->UpdateSubresource(partial_constant_buffer, 0, &update_box,
                           partial_constant_update, 0, 0);
    printf("UnityResourceProbe UpdateSubresource(partial_constant_box)=PASS\n");
    ctx->CopyResource(partial_constant_staging, partial_constant_buffer);
    printf("UnityResourceProbe CopyResource(partial_constant_to_staging)=PASS\n");

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(partial_constant_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityResourceProbe Map(partial_constant_staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t *words = (const uint32_t *)mapped.pData;
      printf("UnityResourceProbe constant_partial_words=%08x,%08x,%08x,%08x,%08x\n",
             words[0], words[3], words[4], words[7], words[8]);
      if (words[0] != partial_constant_initial[0] ||
          words[3] != partial_constant_initial[3] ||
          words[4] != partial_constant_update[0] ||
          words[7] != partial_constant_update[3] ||
          words[8] != partial_constant_initial[8])
        pass = false;
      ctx->Unmap(partial_constant_staging, 0);
    } else {
      pass = false;
    }
  }

  uint32_t overlap_initial[8] = {};
  for (UINT i = 0; i < ARRAYSIZE(overlap_initial); i++)
    overlap_initial[i] = 0x0c000000u | i;
  D3D11_SUBRESOURCE_DATA overlap_data = {};
  overlap_data.pSysMem = overlap_initial;
  D3D11_BUFFER_DESC overlap_desc = {};
  overlap_desc.ByteWidth = sizeof(overlap_initial);
  overlap_desc.Usage = D3D11_USAGE_DEFAULT;
  overlap_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  ID3D11Buffer *overlap_buffer = nullptr;
  hr = device->CreateBuffer(&overlap_desc, &overlap_data, &overlap_buffer);
  print_hr("UnityResourceProbe CreateBuffer(overlap_copy)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC overlap_staging_desc = overlap_desc;
  overlap_staging_desc.Usage = D3D11_USAGE_STAGING;
  overlap_staging_desc.BindFlags = 0;
  overlap_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *overlap_staging = nullptr;
  hr = device->CreateBuffer(&overlap_staging_desc, nullptr, &overlap_staging);
  print_hr("UnityResourceProbe CreateBuffer(overlap_copy_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (overlap_buffer && overlap_staging) {
    D3D11_BOX overlap_src = {};
    overlap_src.left = 0;
    overlap_src.right = 4 * sizeof(uint32_t);
    overlap_src.bottom = 1;
    overlap_src.back = 1;
    ctx->CopySubresourceRegion(overlap_buffer, 0, 2 * sizeof(uint32_t), 0, 0,
                               overlap_buffer, 0, &overlap_src);
    printf("UnityResourceProbe CopySubresourceRegion(overlap_copy)=PASS\n");
    ctx->CopyResource(overlap_staging, overlap_buffer);
    printf("UnityResourceProbe CopyResource(overlap_copy_to_staging)=PASS\n");

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(overlap_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityResourceProbe Map(overlap_copy_staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t *words = (const uint32_t *)mapped.pData;
      printf("UnityResourceProbe overlap_copy_words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
             words[0], words[1], words[2], words[3], words[4], words[5],
             words[6], words[7]);
      if (words[0] != overlap_initial[0] || words[1] != overlap_initial[1] ||
          words[2] != overlap_initial[0] || words[3] != overlap_initial[1] ||
          words[4] != overlap_initial[2] || words[5] != overlap_initial[3] ||
          words[6] != overlap_initial[6] || words[7] != overlap_initial[7])
        pass = false;
      ctx->Unmap(overlap_staging, 0);
    } else {
      pass = false;
    }
  }

  uint32_t device3_buffer_initial[8] = {};
  uint32_t device3_buffer_update[3] = {
      0x3d3d1000u,
      0x3d3d1001u,
      0x3d3d1002u,
  };
  for (UINT i = 0; i < ARRAYSIZE(device3_buffer_initial); i++)
    device3_buffer_initial[i] = 0x3d3d0000u | i;
  D3D11_SUBRESOURCE_DATA device3_buffer_data = {};
  device3_buffer_data.pSysMem = device3_buffer_initial;
  D3D11_BUFFER_DESC device3_buffer_desc = {};
  device3_buffer_desc.ByteWidth = sizeof(device3_buffer_initial);
  device3_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  device3_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  ID3D11Buffer *device3_buffer = nullptr;
  hr = device->CreateBuffer(&device3_buffer_desc, &device3_buffer_data,
                            &device3_buffer);
  print_hr("UnityResourceProbe CreateBuffer(device3_default)", hr);
  if (FAILED(hr)) {
    pass = false;
  } else if (device3) {
    D3D11_BOX write_box = {2 * sizeof(uint32_t), 0, 0,
                           5 * sizeof(uint32_t), 1, 1};
    hr = ctx->Map(device3_buffer, 0, D3D11_MAP_WRITE, 0, nullptr);
    print_hr("UnityResourceProbe Map(default_buffer_null_write)", hr);
    if (hr != E_INVALIDARG)
      pass = false;
    device3->WriteToSubresource(device3_buffer, 0, &write_box,
                                device3_buffer_update, 0, 0);
    printf("UnityResourceProbe WriteToSubresource(buffer_box)=PASS\n");

    D3D11_MAPPED_SUBRESOURCE default_mapped = {};
    hr = ctx->Map(device3_buffer, 0, D3D11_MAP_READ, 0, &default_mapped);
    print_hr("UnityResourceProbe Map(default_buffer_nonnull)", hr);
    if (hr != E_INVALIDARG)
      pass = false;

    uint32_t readback_words[8] = {};
    hr = ctx->Map(device3_buffer, 0, D3D11_MAP_READ, 0, nullptr);
    print_hr("UnityResourceProbe Map(default_buffer_null_read)", hr);
    if (hr != E_INVALIDARG)
      pass = false;
    device3->ReadFromSubresource(readback_words, 0, 0, device3_buffer, 0,
                                 nullptr);
    printf("UnityResourceProbe ReadFromSubresource(buffer) w0=0x%08x w2=0x%08x w4=0x%08x w5=0x%08x\n",
           readback_words[0], readback_words[2], readback_words[4],
           readback_words[5]);
    if (readback_words[0] != device3_buffer_initial[0] ||
        readback_words[2] != device3_buffer_update[0] ||
        readback_words[4] != device3_buffer_update[2] ||
        readback_words[5] != device3_buffer_initial[5])
      pass = false;
  }

  uint32_t staging_initial_words[16] = {};
  uint32_t staging_update_words[4] = {};
  for (UINT i = 0; i < ARRAYSIZE(staging_initial_words); i++)
    staging_initial_words[i] = 0xcace0000u | i;
  for (UINT i = 0; i < ARRAYSIZE(staging_update_words); i++)
    staging_update_words[i] = 0x5a9e0000u | i;
  D3D11_SUBRESOURCE_DATA staging_buffer_data = {};
  staging_buffer_data.pSysMem = staging_initial_words;
  D3D11_BUFFER_DESC update_staging_desc = {};
  update_staging_desc.ByteWidth = sizeof(staging_initial_words);
  update_staging_desc.Usage = D3D11_USAGE_STAGING;
  update_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *update_staging_buffer = nullptr;
  hr = device->CreateBuffer(&update_staging_desc, &staging_buffer_data,
                            &update_staging_buffer);
  print_hr("UnityResourceProbe CreateBuffer(update_staging)", hr);
  if (FAILED(hr)) {
    pass = false;
  } else {
    D3D11_BOX update_box = {2 * sizeof(uint32_t), 0, 0,
                            6 * sizeof(uint32_t), 1, 1};
    ctx->UpdateSubresource(update_staging_buffer, 0, &update_box,
                           staging_update_words, 0, 0);
    printf("UnityResourceProbe UpdateSubresource(staging_buffer_box)=PASS\n");

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(update_staging_buffer, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityResourceProbe Map(update_staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t *words = (const uint32_t *)mapped.pData;
      printf("UnityResourceProbe update_staging_word0=0x%08x word2=0x%08x word5=0x%08x word6=0x%08x\n",
             words[0], words[2], words[5], words[6]);
      if (words[0] != staging_initial_words[0] ||
          words[2] != staging_update_words[0] ||
          words[5] != staging_update_words[3] ||
          words[6] != staging_initial_words[6])
        pass = false;
      ctx->Unmap(update_staging_buffer, 0);
    } else {
      pass = false;
    }
  }

  uint32_t staging_tex_initial[16] = {};
  uint32_t staging_tex_update[4] = {
      0x77ac0000u,
      0x77ac0001u,
      0x77ac0002u,
      0x77ac0003u,
  };
  for (UINT i = 0; i < ARRAYSIZE(staging_tex_initial); i++)
    staging_tex_initial[i] = 0x57a60000u | i;
  D3D11_SUBRESOURCE_DATA staging_tex_data = {};
  staging_tex_data.pSysMem = staging_tex_initial;
  staging_tex_data.SysMemPitch = 4 * sizeof(uint32_t);
  D3D11_TEXTURE2D_DESC update_staging_tex_desc = tex_desc;
  update_staging_tex_desc.Usage = D3D11_USAGE_STAGING;
  update_staging_tex_desc.BindFlags = 0;
  update_staging_tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  update_staging_tex_desc.MiscFlags = 0;
  ID3D11Texture2D *update_staging_tex = nullptr;
  hr = device->CreateTexture2D(&update_staging_tex_desc, &staging_tex_data,
                               &update_staging_tex);
  print_hr("UnityResourceProbe CreateTexture2D(update_staging)", hr);
  if (FAILED(hr)) {
    pass = false;
  } else {
    D3D11_BOX update_box = {1, 1, 0, 3, 3, 1};
    ctx->UpdateSubresource(update_staging_tex, 0, &update_box,
                           staging_tex_update, 2 * sizeof(uint32_t), 0);
    printf("UnityResourceProbe UpdateSubresource(staging_texture_box)=PASS\n");

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(update_staging_tex, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityResourceProbe Map(update_staging_texture)", hr);
    if (SUCCEEDED(hr)) {
      const uint8_t *base = (const uint8_t *)mapped.pData;
      const uint32_t texel00 = *(const uint32_t *)base;
      const uint32_t texel11 =
          *(const uint32_t *)(base + mapped.RowPitch + sizeof(uint32_t));
      const uint32_t texel22 =
          *(const uint32_t *)(base + 2 * mapped.RowPitch +
                              2 * sizeof(uint32_t));
      const uint32_t texel33 =
          *(const uint32_t *)(base + 3 * mapped.RowPitch +
                              3 * sizeof(uint32_t));
      printf("UnityResourceProbe update_staging_texel00=0x%08x texel11=0x%08x texel22=0x%08x texel33=0x%08x\n",
             texel00, texel11, texel22, texel33);
      if (texel00 != staging_tex_initial[0] ||
          texel11 != staging_tex_update[0] ||
          texel22 != staging_tex_update[3] ||
          texel33 != staging_tex_initial[15])
        pass = false;
      ctx->Unmap(update_staging_tex, 0);
    } else {
      pass = false;
    }
  }

  {
    const uint8_t bc_seed_a[8] = {0x11, 0x22, 0x33, 0x44,
                                  0x55, 0x66, 0x77, 0x88};
    const uint8_t bc_seed_b[8] = {0x21, 0x32, 0x43, 0x54,
                                  0x65, 0x76, 0x87, 0x98};
    const uint8_t bc_seed_c[8] = {0x31, 0x42, 0x53, 0x64,
                                  0x75, 0x86, 0x97, 0xa8};
    const uint8_t bc_seed_d[8] = {0x41, 0x52, 0x63, 0x74,
                                  0x85, 0x96, 0xa7, 0xb8};

    D3D11_TEXTURE2D_DESC bc_default_desc = {};
    bc_default_desc.Width = 4;
    bc_default_desc.Height = 4;
    bc_default_desc.MipLevels = 1;
    bc_default_desc.ArraySize = 1;
    bc_default_desc.Format = DXGI_FORMAT_BC1_UNORM;
    bc_default_desc.SampleDesc.Count = 1;
    bc_default_desc.Usage = D3D11_USAGE_DEFAULT;
    bc_default_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_TEXTURE2D_DESC bc_staging_desc = bc_default_desc;
    bc_staging_desc.Usage = D3D11_USAGE_STAGING;
    bc_staging_desc.BindFlags = 0;
    bc_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    D3D11_TEXTURE2D_DESC raw_default_desc = {};
    raw_default_desc.Width = 1;
    raw_default_desc.Height = 1;
    raw_default_desc.MipLevels = 1;
    raw_default_desc.ArraySize = 1;
    raw_default_desc.Format = DXGI_FORMAT_R32G32_UINT;
    raw_default_desc.SampleDesc.Count = 1;
    raw_default_desc.Usage = D3D11_USAGE_DEFAULT;
    raw_default_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_TEXTURE2D_DESC raw_staging_desc = raw_default_desc;
    raw_staging_desc.Usage = D3D11_USAGE_STAGING;
    raw_staging_desc.BindFlags = 0;
    raw_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    D3D11_SUBRESOURCE_DATA bc_data_a = {bc_seed_a, 8, 8};
    D3D11_SUBRESOURCE_DATA bc_data_b = {bc_seed_b, 8, 8};
    D3D11_SUBRESOURCE_DATA raw_data_c = {bc_seed_c, 8, 8};
    D3D11_SUBRESOURCE_DATA raw_data_d = {bc_seed_d, 8, 8};

    ID3D11Texture2D *bc_default_a = nullptr;
    ID3D11Texture2D *bc_default_b = nullptr;
    ID3D11Texture2D *bc_default_d = nullptr;
    ID3D11Texture2D *bc_staging = nullptr;
    ID3D11Texture2D *bc_staging_seed = nullptr;
    ID3D11Texture2D *raw_default_seed = nullptr;
    ID3D11Texture2D *raw_default_dst = nullptr;
    ID3D11Texture2D *raw_staging = nullptr;
    ID3D11Texture2D *raw_staging_seed = nullptr;

    hr = device->CreateTexture2D(&bc_default_desc, &bc_data_a, &bc_default_a);
    print_hr("UnityResourceProbe CreateTexture2D(bc1_default_a)", hr);
    if (FAILED(hr))
      pass = false;
    hr = device->CreateTexture2D(&bc_default_desc, nullptr, &bc_default_b);
    print_hr("UnityResourceProbe CreateTexture2D(bc1_default_b)", hr);
    if (FAILED(hr))
      pass = false;
    hr = device->CreateTexture2D(&bc_default_desc, nullptr, &bc_default_d);
    print_hr("UnityResourceProbe CreateTexture2D(bc1_default_d)", hr);
    if (FAILED(hr))
      pass = false;
    hr = device->CreateTexture2D(&bc_staging_desc, nullptr, &bc_staging);
    print_hr("UnityResourceProbe CreateTexture2D(bc1_staging)", hr);
    if (FAILED(hr))
      pass = false;
    hr = device->CreateTexture2D(&bc_staging_desc, &bc_data_b,
                                 &bc_staging_seed);
    print_hr("UnityResourceProbe CreateTexture2D(bc1_staging_seed)", hr);
    if (FAILED(hr))
      pass = false;
    hr = device->CreateTexture2D(&raw_default_desc, &raw_data_c,
                                 &raw_default_seed);
    print_hr("UnityResourceProbe CreateTexture2D(raw_default_seed)", hr);
    if (FAILED(hr))
      pass = false;
    hr = device->CreateTexture2D(&raw_default_desc, nullptr, &raw_default_dst);
    print_hr("UnityResourceProbe CreateTexture2D(raw_default_dst)", hr);
    if (FAILED(hr))
      pass = false;
    hr = device->CreateTexture2D(&raw_staging_desc, nullptr, &raw_staging);
    print_hr("UnityResourceProbe CreateTexture2D(raw_staging)", hr);
    if (FAILED(hr))
      pass = false;
    hr = device->CreateTexture2D(&raw_staging_desc, &raw_data_d,
                                 &raw_staging_seed);
    print_hr("UnityResourceProbe CreateTexture2D(raw_staging_seed)", hr);
    if (FAILED(hr))
      pass = false;

    if (bc_default_a && bc_staging) {
      ctx->CopyResource(bc_staging, bc_default_a);
      printf("UnityResourceProbe BC1 CopyResource(default_to_staging)=PASS\n");
      if (!check_texture_bytes(ctx, bc_staging,
                               "UnityResourceProbe BC1 default_to_staging",
                               bc_seed_a, sizeof(bc_seed_a)))
        pass = false;
    }
    if (bc_staging_seed && bc_default_b && bc_staging) {
      ctx->CopyResource(bc_default_b, bc_staging_seed);
      ctx->CopyResource(bc_staging, bc_default_b);
      printf("UnityResourceProbe BC1 CopyResource(staging_to_default)=PASS\n");
      if (!check_texture_bytes(ctx, bc_staging,
                               "UnityResourceProbe BC1 staging_to_default",
                               bc_seed_b, sizeof(bc_seed_b)))
        pass = false;
    }
    if (bc_default_a && raw_staging) {
      ctx->CopySubresourceRegion(raw_staging, 0, 0, 0, 0, bc_default_a, 0,
                                 nullptr);
      printf("UnityResourceProbe BC1 CopySubresourceRegion(bc_default_to_raw_staging)=PASS\n");
      if (!check_texture_bytes(ctx, raw_staging,
                               "UnityResourceProbe BC1 bc_default_to_raw_staging",
                               bc_seed_a, sizeof(bc_seed_a)))
        pass = false;
    }
    if (bc_staging_seed && raw_staging) {
      ctx->CopySubresourceRegion(raw_staging, 0, 0, 0, 0, bc_staging_seed, 0,
                                 nullptr);
      printf("UnityResourceProbe BC1 CopySubresourceRegion(bc_staging_to_raw_staging)=PASS\n");
      if (!check_texture_bytes(ctx, raw_staging,
                               "UnityResourceProbe BC1 bc_staging_to_raw_staging",
                               bc_seed_b, sizeof(bc_seed_b)))
        pass = false;
    }
    if (bc_staging_seed && raw_default_dst && raw_staging) {
      ctx->CopySubresourceRegion(raw_default_dst, 0, 0, 0, 0, bc_staging_seed,
                                 0, nullptr);
      ctx->CopyResource(raw_staging, raw_default_dst);
      printf("UnityResourceProbe BC1 CopySubresourceRegion(bc_staging_to_raw_default)=PASS\n");
      if (!check_texture_bytes(ctx, raw_staging,
                               "UnityResourceProbe BC1 bc_staging_to_raw_default",
                               bc_seed_b, sizeof(bc_seed_b)))
        pass = false;
    }
    if (raw_default_seed && bc_staging) {
      ctx->CopySubresourceRegion(bc_staging, 0, 0, 0, 0, raw_default_seed, 0,
                                 nullptr);
      printf("UnityResourceProbe BC1 CopySubresourceRegion(raw_default_to_bc_staging)=PASS\n");
      if (!check_texture_bytes(ctx, bc_staging,
                               "UnityResourceProbe BC1 raw_default_to_bc_staging",
                               bc_seed_c, sizeof(bc_seed_c)))
        pass = false;
    }
    if (raw_staging_seed && bc_staging) {
      ctx->CopySubresourceRegion(bc_staging, 0, 0, 0, 0, raw_staging_seed, 0,
                                 nullptr);
      printf("UnityResourceProbe BC1 CopySubresourceRegion(raw_staging_to_bc_staging)=PASS\n");
      if (!check_texture_bytes(ctx, bc_staging,
                               "UnityResourceProbe BC1 raw_staging_to_bc_staging",
                               bc_seed_d, sizeof(bc_seed_d)))
        pass = false;
    }
    if (raw_staging_seed && bc_default_d && bc_staging) {
      ctx->CopySubresourceRegion(bc_default_d, 0, 0, 0, 0, raw_staging_seed, 0,
                                 nullptr);
      ctx->CopyResource(bc_staging, bc_default_d);
      printf("UnityResourceProbe BC1 CopySubresourceRegion(raw_staging_to_bc_default)=PASS\n");
      if (!check_texture_bytes(ctx, bc_staging,
                               "UnityResourceProbe BC1 raw_staging_to_bc_default",
                               bc_seed_d, sizeof(bc_seed_d)))
        pass = false;
    }

    if (raw_staging_seed)
      raw_staging_seed->Release();
    if (raw_staging)
      raw_staging->Release();
    if (raw_default_dst)
      raw_default_dst->Release();
    if (raw_default_seed)
      raw_default_seed->Release();
    if (bc_staging_seed)
      bc_staging_seed->Release();
    if (bc_staging)
      bc_staging->Release();
    if (bc_default_d)
      bc_default_d->Release();
    if (bc_default_b)
      bc_default_b->Release();
    if (bc_default_a)
      bc_default_a->Release();
  }

  {
    struct BcFamilyCase {
      DXGI_FORMAT format;
      const char *name;
      UINT block_bytes;
      uint8_t seed[16];
    };
    static const BcFamilyCase bc_cases[] = {
        {DXGI_FORMAT_BC1_UNORM_SRGB,
         "BC1_UNORM_SRGB",
         8,
         {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}},
        {DXGI_FORMAT_BC2_UNORM,
         "BC2_UNORM",
         16,
         {0x02, 0x13, 0x24, 0x35, 0x46, 0x57, 0x68, 0x79, 0x8a, 0x9b,
          0xac, 0xbd, 0xce, 0xdf, 0xe0, 0xf1}},
        {DXGI_FORMAT_BC2_UNORM_SRGB,
         "BC2_UNORM_SRGB",
         16,
         {0xf1, 0xe0, 0xdf, 0xce, 0xbd, 0xac, 0x9b, 0x8a, 0x79, 0x68,
          0x57, 0x46, 0x35, 0x24, 0x13, 0x02}},
        {DXGI_FORMAT_BC3_UNORM,
         "BC3_UNORM",
         16,
         {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xa9,
          0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f}},
        {DXGI_FORMAT_BC3_UNORM_SRGB,
         "BC3_UNORM_SRGB",
         16,
         {0x1f, 0x2e, 0x3d, 0x4c, 0x5b, 0x6a, 0x79, 0x88, 0x97, 0xa6,
          0xb5, 0xc4, 0xd3, 0xe2, 0xf1, 0x00}},
        {DXGI_FORMAT_BC4_UNORM,
         "BC4_UNORM",
         8,
         {0x04, 0x15, 0x26, 0x37, 0x48, 0x59, 0x6a, 0x7b}},
        {DXGI_FORMAT_BC4_SNORM,
         "BC4_SNORM",
         8,
         {0x84, 0x95, 0xa6, 0xb7, 0xc8, 0xd9, 0xea, 0xfb}},
        {DXGI_FORMAT_BC5_UNORM,
         "BC5_UNORM",
         16,
         {0x05, 0x16, 0x27, 0x38, 0x49, 0x5a, 0x6b, 0x7c, 0x8d, 0x9e,
          0xaf, 0xb0, 0xc1, 0xd2, 0xe3, 0xf4}},
        {DXGI_FORMAT_BC5_SNORM,
         "BC5_SNORM",
         16,
         {0xf4, 0xe3, 0xd2, 0xc1, 0xb0, 0xaf, 0x9e, 0x8d, 0x7c, 0x6b,
          0x5a, 0x49, 0x38, 0x27, 0x16, 0x05}},
        {DXGI_FORMAT_BC6H_UF16,
         "BC6H_UF16",
         16,
         {0x06, 0x17, 0x28, 0x39, 0x4a, 0x5b, 0x6c, 0x7d, 0x8e, 0x9f,
          0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5}},
        {DXGI_FORMAT_BC6H_SF16,
         "BC6H_SF16",
         16,
         {0xf5, 0xe4, 0xd3, 0xc2, 0xb1, 0xa0, 0x9f, 0x8e, 0x7d, 0x6c,
          0x5b, 0x4a, 0x39, 0x28, 0x17, 0x06}},
        {DXGI_FORMAT_BC7_UNORM,
         "BC7_UNORM",
         16,
         {0x0f, 0xfe, 0xed, 0xdc, 0xcb, 0xba, 0xa9, 0x98, 0x87, 0x76,
          0x65, 0x54, 0x43, 0x32, 0x21, 0x10}},
        {DXGI_FORMAT_BC7_UNORM_SRGB,
         "BC7_UNORM_SRGB",
         16,
         {0x70, 0x61, 0x52, 0x43, 0x34, 0x25, 0x16, 0x07, 0xf8, 0xe9,
          0xda, 0xcb, 0xbc, 0xad, 0x9e, 0x8f}},
    };

    for (UINT i = 0; i < ARRAYSIZE(bc_cases); i++) {
      UINT format_support = 0;
      hr = device->CheckFormatSupport(bc_cases[i].format, &format_support);
      char label[192] = {};
      snprintf(label, sizeof(label),
               "UnityResourceProbe CheckFormatSupport(%s)", bc_cases[i].name);
      print_hr(label, hr);
      printf("UnityResourceProbe %s format_support=0x%08x\n",
             bc_cases[i].name, format_support);
      if (FAILED(hr) ||
          !(format_support & D3D11_FORMAT_SUPPORT_TEXTURE2D)) {
        printf("UnityResourceProbe %s skipped=unsupported\n",
               bc_cases[i].name);
        continue;
      }

      D3D11_TEXTURE2D_DESC bc_desc = {};
      bc_desc.Width = 4;
      bc_desc.Height = 4;
      bc_desc.MipLevels = 1;
      bc_desc.ArraySize = 1;
      bc_desc.Format = bc_cases[i].format;
      bc_desc.SampleDesc.Count = 1;
      bc_desc.Usage = D3D11_USAGE_DEFAULT;
      bc_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

      D3D11_SUBRESOURCE_DATA bc_data = {};
      bc_data.pSysMem = bc_cases[i].seed;
      bc_data.SysMemPitch = bc_cases[i].block_bytes;
      bc_data.SysMemSlicePitch = bc_cases[i].block_bytes;
      ID3D11Texture2D *bc_tex = nullptr;
      hr = device->CreateTexture2D(&bc_desc, &bc_data, &bc_tex);
      snprintf(label, sizeof(label), "UnityResourceProbe CreateTexture2D(%s)",
               bc_cases[i].name);
      print_hr(label, hr);
      if (FAILED(hr)) {
        pass = false;
        continue;
      }

      ID3D11ShaderResourceView *bc_srv = nullptr;
      hr = device->CreateShaderResourceView(bc_tex, nullptr, &bc_srv);
      snprintf(label, sizeof(label),
               "UnityResourceProbe CreateShaderResourceView(%s)",
               bc_cases[i].name);
      print_hr(label, hr);
      if (FAILED(hr))
        pass = false;

      D3D11_TEXTURE2D_DESC bc_staging_desc = bc_desc;
      bc_staging_desc.Usage = D3D11_USAGE_STAGING;
      bc_staging_desc.BindFlags = 0;
      bc_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Texture2D *bc_staging = nullptr;
      hr = device->CreateTexture2D(&bc_staging_desc, nullptr, &bc_staging);
      snprintf(label, sizeof(label),
               "UnityResourceProbe CreateTexture2D(%s_staging)",
               bc_cases[i].name);
      print_hr(label, hr);
      if (FAILED(hr)) {
        pass = false;
      } else {
        ctx->CopyResource(bc_staging, bc_tex);
        printf("UnityResourceProbe %s CopyResource(default_to_staging)=PASS\n",
               bc_cases[i].name);
        snprintf(label, sizeof(label),
                 "UnityResourceProbe %s default_to_staging",
                 bc_cases[i].name);
        if (!check_texture_bytes(ctx, bc_staging, label, bc_cases[i].seed,
                                 bc_cases[i].block_bytes))
          pass = false;
      }

      if (bc_staging)
        bc_staging->Release();
      if (bc_srv)
        bc_srv->Release();
      bc_tex->Release();
    }
  }

  uint32_t constants[64] = {};
  D3D11_SUBRESOURCE_DATA constant_data = {};
  constant_data.pSysMem = constants;
  D3D11_BUFFER_DESC constant_desc = {};
  constant_desc.ByteWidth = sizeof(constants);
  constant_desc.Usage = D3D11_USAGE_IMMUTABLE;
  constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer *constant_buffer = nullptr;
  hr = device->CreateBuffer(&constant_desc, &constant_data, &constant_buffer);
  print_hr("UnityResourceProbe CreateBuffer(immutable_constant)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_TEXTURE2D_DESC depth_desc = {};
  depth_desc.Width = 4;
  depth_desc.Height = 4;
  depth_desc.MipLevels = 1;
  depth_desc.ArraySize = 1;
  depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  depth_desc.SampleDesc.Count = 1;
  depth_desc.Usage = D3D11_USAGE_DEFAULT;
  depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D *depth_tex = nullptr;
  hr = device->CreateTexture2D(&depth_desc, nullptr, &depth_tex);
  print_hr("UnityResourceProbe CreateTexture2D(depth)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11DepthStencilView *dsv = nullptr;
  if (depth_tex) {
    if (!probe_dxgi_resource_metadata(depth_tex,
                                      DXGI_USAGE_RENDER_TARGET_OUTPUT, "depth"))
      pass = false;

    hr = device->CreateDepthStencilView(depth_tex, nullptr, &dsv);
    print_hr("UnityResourceProbe CreateDepthStencilView(depth)", hr);
    if (FAILED(hr))
      pass = false;
  }
  if (dsv) {
    ctx->ClearDepthStencilView(dsv,
                               D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f,
                               0);
    printf("UnityResourceProbe ClearDepthStencilView(depth)=PASS\n");
    if (ctx1) {
      const FLOAT clear_depth_full[4] = {0.75f, 0.0f, 0.0f, 0.0f};
      D3D11_RECT clear_depth_full_rect = {
          0, 0, (LONG)depth_desc.Width, (LONG)depth_desc.Height};
      ctx1->ClearView(dsv, clear_depth_full, &clear_depth_full_rect, 1);
      printf("UnityResourceProbe ClearView(depth_full)=PASS\n");

      const FLOAT clear_depth_rect_value[4] = {0.25f, 0.0f, 0.0f, 0.0f};
      D3D11_RECT clear_depth_rect = {1, 1, 3, 3};
      ctx1->ClearView(dsv, clear_depth_rect_value, &clear_depth_rect, 1);
      printf("UnityResourceProbe ClearView(depth_rect)=PASS\n");
    }
  }

  struct DepthFormatCase {
    DXGI_FORMAT format;
    const char *name;
    UINT clear_flags;
  };
  static const DepthFormatCase depth_cases[] = {
      {DXGI_FORMAT_D16_UNORM, "D16_UNORM", D3D11_CLEAR_DEPTH},
      {DXGI_FORMAT_D32_FLOAT, "D32_FLOAT", D3D11_CLEAR_DEPTH},
      {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "D32_FLOAT_S8X24_UINT",
       D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL},
  };
  for (UINT i = 0; i < ARRAYSIZE(depth_cases); i++) {
    D3D11_TEXTURE2D_DESC depth_format_desc = depth_desc;
    depth_format_desc.Format = depth_cases[i].format;

    ID3D11Texture2D *depth_format_tex = nullptr;
    hr = device->CreateTexture2D(&depth_format_desc, nullptr, &depth_format_tex);
    char label[160] = {};
    snprintf(label, sizeof(label), "UnityResourceProbe CreateTexture2D(%s)",
             depth_cases[i].name);
    print_hr(label, hr);
    if (FAILED(hr)) {
      pass = false;
      continue;
    }

    ID3D11DepthStencilView *depth_format_dsv = nullptr;
    hr = device->CreateDepthStencilView(depth_format_tex, nullptr,
                                        &depth_format_dsv);
    snprintf(label, sizeof(label),
             "UnityResourceProbe CreateDepthStencilView(%s)",
             depth_cases[i].name);
    print_hr(label, hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      ctx->ClearDepthStencilView(depth_format_dsv, depth_cases[i].clear_flags,
                                 0.25f, 7);
      printf("UnityResourceProbe ClearDepthStencilView(%s)=PASS\n",
             depth_cases[i].name);
    }

    if (depth_format_dsv)
      depth_format_dsv->Release();
    depth_format_tex->Release();
  }

  D3D11_TEXTURE2D_DESC typeless_depth_desc = depth_desc;
  typeless_depth_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
  typeless_depth_desc.BindFlags =
      D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D *typeless_depth_tex = nullptr;
  hr = device->CreateTexture2D(&typeless_depth_desc, nullptr,
                               &typeless_depth_tex);
  print_hr("UnityResourceProbe CreateTexture2D(depth_typeless)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11DepthStencilView *typeless_dsv = nullptr;
  ID3D11ShaderResourceView *depth_srv = nullptr;
  if (typeless_depth_tex) {
    D3D11_DEPTH_STENCIL_VIEW_DESC typeless_dsv_desc = {};
    typeless_dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    typeless_dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    hr = device->CreateDepthStencilView(typeless_depth_tex,
                                        &typeless_dsv_desc, &typeless_dsv);
    print_hr("UnityResourceProbe CreateDepthStencilView(depth_typeless)", hr);
    if (FAILED(hr))
      pass = false;

    D3D11_SHADER_RESOURCE_VIEW_DESC depth_srv_desc = {};
    depth_srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depth_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depth_srv_desc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(typeless_depth_tex, &depth_srv_desc,
                                          &depth_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(depth_typeless)", hr);
    if (FAILED(hr))
      pass = false;
  }
  if (typeless_dsv) {
    ctx->ClearDepthStencilView(typeless_dsv, D3D11_CLEAR_DEPTH, 0.5f, 0);
    printf("UnityResourceProbe ClearDepthStencilView(depth_typeless)=PASS\n");
  }

  D3D11_TEXTURE2D_DESC msaa_desc = tex_desc;
  msaa_desc.SampleDesc.Count = 4;
  msaa_desc.SampleDesc.Quality = 0;
  msaa_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D *msaa_tex = nullptr;
  hr = device->CreateTexture2D(&msaa_desc, nullptr, &msaa_tex);
  print_hr("UnityResourceProbe CreateTexture2D(msaa4)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11RenderTargetView *msaa_rtv = nullptr;
  ID3D11ShaderResourceView *msaa_srv = nullptr;
  if (msaa_tex) {
    hr = device->CreateRenderTargetView(msaa_tex, nullptr, &msaa_rtv);
    print_hr("UnityResourceProbe CreateRenderTargetView(msaa4)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      const FLOAT msaa_color[4] = {0.0f, 0.25f, 0.5f, 1.0f};
      ctx->ClearRenderTargetView(msaa_rtv, msaa_color);
      printf("UnityResourceProbe ClearRenderTargetView(msaa4)=PASS\n");
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC msaa_srv_desc = {};
    msaa_srv_desc.Format = msaa_desc.Format;
    msaa_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    hr = device->CreateShaderResourceView(msaa_tex, &msaa_srv_desc, &msaa_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(msaa4)", hr);
    if (FAILED(hr))
      pass = false;
  }
  ID3D11Texture2D *resolve_tex = nullptr;
  if (msaa_tex) {
    hr = device->CreateTexture2D(&tex_desc, nullptr, &resolve_tex);
    print_hr("UnityResourceProbe CreateTexture2D(resolve_dst)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      ctx->ResolveSubresource(resolve_tex, 0, msaa_tex, 0, tex_desc.Format);
      printf("UnityResourceProbe ResolveSubresource(msaa4)=PASS\n");
      uint8_t resolve_pixel[4] = {};
      hr = read_first_pixel_subresource(device, ctx, resolve_tex, 0,
                                        resolve_pixel);
      print_hr("UnityResourceProbe Readback(msaa4_resolve)", hr);
      printf("UnityResourceProbe msaa4_resolve_rgba=%u,%u,%u,%u\n",
             resolve_pixel[0], resolve_pixel[1], resolve_pixel[2],
             resolve_pixel[3]);
      if (FAILED(hr) || resolve_pixel[0] > 2 || resolve_pixel[1] < 63 ||
          resolve_pixel[1] > 65 || resolve_pixel[2] < 127 ||
          resolve_pixel[2] > 129 || resolve_pixel[3] != 255)
        pass = false;
    }
  }

  D3D11_TEXTURE2D_DESC uav_desc = {};
  uav_desc.Width = 4;
  uav_desc.Height = 4;
  uav_desc.MipLevels = 1;
  uav_desc.ArraySize = 1;
  uav_desc.Format = DXGI_FORMAT_R32_UINT;
  uav_desc.SampleDesc.Count = 1;
  uav_desc.Usage = D3D11_USAGE_DEFAULT;
  uav_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Texture2D *uav_tex = nullptr;
  hr = device->CreateTexture2D(&uav_desc, nullptr, &uav_tex);
  print_hr("UnityResourceProbe CreateTexture2D(uav)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11UnorderedAccessView *uav = nullptr;
  ID3D11Texture2D *uav_staging = nullptr;
  if (uav_tex) {
    if (!probe_dxgi_resource_metadata(
            uav_tex, DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_UNORDERED_ACCESS,
            "uav"))
      pass = false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_view_desc = {};
    uav_view_desc.Format = uav_desc.Format;
    uav_view_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    hr = device->CreateUnorderedAccessView(uav_tex, &uav_view_desc, &uav);
    print_hr("UnityResourceProbe CreateUnorderedAccessView(uav)", hr);
    if (FAILED(hr))
      pass = false;
  }
  if (uav_tex && uav) {
    D3D11_TEXTURE2D_DESC uav_staging_desc = uav_desc;
    uav_staging_desc.Usage = D3D11_USAGE_STAGING;
    uav_staging_desc.BindFlags = 0;
    uav_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&uav_staging_desc, nullptr, &uav_staging);
    print_hr("UnityResourceProbe CreateTexture2D(uav_clear_staging)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      const UINT clear_values[4] = {0x1234abcdu, 0, 0, 0};
      ctx->ClearUnorderedAccessViewUint(uav, clear_values);
      printf("UnityResourceProbe ClearUnorderedAccessViewUint(uav)=PASS\n");
      ctx->CopyResource(uav_staging, uav_tex);
      printf("UnityResourceProbe CopyResource(uav_clear_to_staging)=PASS\n");
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      hr = ctx->Map(uav_staging, 0, D3D11_MAP_READ, 0, &mapped);
      print_hr("UnityResourceProbe Map(uav_clear_staging)", hr);
      if (SUCCEEDED(hr)) {
        const uint32_t first = *(const uint32_t *)mapped.pData;
        printf("UnityResourceProbe uav_clear_uint_word=0x%08x\n", first);
        if (first != clear_values[0])
          pass = false;
        ctx->Unmap(uav_staging, 0);
      } else {
        pass = false;
      }
    }
  }

  D3D11_TEXTURE2D_DESC uav_float_desc = uav_desc;
  uav_float_desc.Format = DXGI_FORMAT_R32_FLOAT;
  ID3D11Texture2D *uav_float_tex = nullptr;
  ID3D11Texture2D *uav_float_staging = nullptr;
  ID3D11UnorderedAccessView *uav_float = nullptr;
  hr = device->CreateTexture2D(&uav_float_desc, nullptr, &uav_float_tex);
  print_hr("UnityResourceProbe CreateTexture2D(uav_float)", hr);
  if (FAILED(hr)) {
    pass = false;
  } else {
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_float_desc_view = {};
    uav_float_desc_view.Format = uav_float_desc.Format;
    uav_float_desc_view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    hr = device->CreateUnorderedAccessView(uav_float_tex, &uav_float_desc_view,
                                           &uav_float);
    print_hr("UnityResourceProbe CreateUnorderedAccessView(uav_float)", hr);
    if (FAILED(hr))
      pass = false;

    D3D11_TEXTURE2D_DESC uav_float_staging_desc = uav_float_desc;
    uav_float_staging_desc.Usage = D3D11_USAGE_STAGING;
    uav_float_staging_desc.BindFlags = 0;
    uav_float_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device->CreateTexture2D(&uav_float_staging_desc, nullptr,
                                 &uav_float_staging);
    print_hr("UnityResourceProbe CreateTexture2D(uav_float_staging)", hr);
    if (FAILED(hr))
      pass = false;

    if (uav_float && uav_float_staging) {
      const FLOAT clear_float[4] = {2.5f, 0.0f, 0.0f, 0.0f};
      ctx->ClearUnorderedAccessViewFloat(uav_float, clear_float);
      printf("UnityResourceProbe ClearUnorderedAccessViewFloat(uav_float)=PASS\n");
      ctx->CopyResource(uav_float_staging, uav_float_tex);
      printf("UnityResourceProbe CopyResource(uav_float_to_staging)=PASS\n");
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      hr = ctx->Map(uav_float_staging, 0, D3D11_MAP_READ, 0, &mapped);
      print_hr("UnityResourceProbe Map(uav_float_staging)", hr);
      if (SUCCEEDED(hr)) {
        const float first = *(const float *)mapped.pData;
        printf("UnityResourceProbe uav_clear_float_value=%.3f\n", first);
        if (first != clear_float[0])
          pass = false;
        ctx->Unmap(uav_float_staging, 0);
      } else {
        pass = false;
      }
    }
  }
  if (rtv && uav) {
    ID3D11RenderTargetView *rtvs[] = {rtv};
    ID3D11UnorderedAccessView *uavs[] = {uav};
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(ARRAYSIZE(rtvs), rtvs,
                                                   nullptr, ARRAYSIZE(rtvs),
                                                   ARRAYSIZE(uavs), uavs,
                                                   nullptr);
    printf("UnityResourceProbe OMSetRenderTargetsAndUnorderedAccessViews=PASS\n");

    ID3D11UnorderedAccessView *null_uavs[] = {nullptr};
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
                                                   ARRAYSIZE(rtvs),
                                                   ARRAYSIZE(null_uavs),
                                                   null_uavs, nullptr);
  }

  uint32_t volume_texels[8] = {};
  for (UINT i = 0; i < ARRAYSIZE(volume_texels); i++)
    volume_texels[i] = 0xff103050u + i;
  D3D11_TEXTURE3D_DESC volume_desc = {};
  volume_desc.Width = 2;
  volume_desc.Height = 2;
  volume_desc.Depth = 2;
  volume_desc.MipLevels = 1;
  volume_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  volume_desc.Usage = D3D11_USAGE_IMMUTABLE;
  volume_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA volume_data = {};
  volume_data.pSysMem = volume_texels;
  volume_data.SysMemPitch = 2 * sizeof(uint32_t);
  volume_data.SysMemSlicePitch = 2 * 2 * sizeof(uint32_t);
  ID3D11Texture3D *volume_tex = nullptr;
  hr = device->CreateTexture3D(&volume_desc, &volume_data, &volume_tex);
  print_hr("UnityResourceProbe CreateTexture3D(volume)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *volume_srv = nullptr;
  if (volume_tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC volume_srv_desc = {};
    volume_srv_desc.Format = volume_desc.Format;
    volume_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    volume_srv_desc.Texture3D.MipLevels = 1;
    hr = device->CreateShaderResourceView(volume_tex, &volume_srv_desc,
                                          &volume_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(volume)", hr);
    if (FAILED(hr))
      pass = false;
  }

  uint32_t cube_texels[6][4] = {};
  D3D11_SUBRESOURCE_DATA cube_data[6] = {};
  for (UINT face = 0; face < ARRAYSIZE(cube_texels); face++) {
    for (UINT i = 0; i < ARRAYSIZE(cube_texels[face]); i++)
      cube_texels[face][i] = 0xff000040u | (face << 12) | i;
    cube_data[face].pSysMem = cube_texels[face];
    cube_data[face].SysMemPitch = 2 * sizeof(uint32_t);
  }
  D3D11_TEXTURE2D_DESC cube_desc = {};
  cube_desc.Width = 2;
  cube_desc.Height = 2;
  cube_desc.MipLevels = 1;
  cube_desc.ArraySize = 6;
  cube_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  cube_desc.SampleDesc.Count = 1;
  cube_desc.Usage = D3D11_USAGE_IMMUTABLE;
  cube_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  cube_desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
  ID3D11Texture2D *cube_tex = nullptr;
  hr = device->CreateTexture2D(&cube_desc, cube_data, &cube_tex);
  print_hr("UnityResourceProbe CreateTexture2D(cube)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *cube_srv = nullptr;
  if (cube_tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC cube_srv_desc = {};
    cube_srv_desc.Format = cube_desc.Format;
    cube_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cube_srv_desc.TextureCube.MipLevels = 1;
    hr = device->CreateShaderResourceView(cube_tex, &cube_srv_desc, &cube_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(cube)", hr);
    if (FAILED(hr))
      pass = false;
  }

  uint32_t cube_mip_texels[6][4] = {};
  for (UINT face = 0; face < ARRAYSIZE(cube_mip_texels); face++) {
    const uint8_t r = (uint8_t)(32 + face * 20);
    const uint8_t g = (uint8_t)(96 + face * 8);
    const uint8_t b = (uint8_t)(160 - face * 10);
    const uint32_t texel = 0xff000000u | (b << 16) | (g << 8) | r;
    for (UINT i = 0; i < ARRAYSIZE(cube_mip_texels[face]); i++)
      cube_mip_texels[face][i] = texel;
  }
  D3D11_TEXTURE2D_DESC cube_mip_desc = cube_desc;
  cube_mip_desc.MipLevels = 2;
  cube_mip_desc.Usage = D3D11_USAGE_DEFAULT;
  cube_mip_desc.MiscFlags =
      D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ID3D11Texture2D *cube_mip_tex = nullptr;
  hr = device->CreateTexture2D(&cube_mip_desc, nullptr, &cube_mip_tex);
  print_hr("UnityResourceProbe CreateTexture2D(cube_mip_chain)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *cube_mip_srv = nullptr;
  if (cube_mip_tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC cube_mip_srv_desc = {};
    cube_mip_srv_desc.Format = cube_mip_desc.Format;
    cube_mip_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cube_mip_srv_desc.TextureCube.MipLevels = cube_mip_desc.MipLevels;
    hr = device->CreateShaderResourceView(cube_mip_tex, &cube_mip_srv_desc,
                                          &cube_mip_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(cube_mip_chain)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      for (UINT face = 0; face < ARRAYSIZE(cube_mip_texels); face++) {
        const UINT face_mip0 =
            D3D11CalcSubresource(0, face, cube_mip_desc.MipLevels);
        ctx->UpdateSubresource(cube_mip_tex, face_mip0, nullptr,
                               cube_mip_texels[face], 2 * sizeof(uint32_t), 0);
      }
      printf("UnityResourceProbe UpdateSubresource(cube_mip_faces)=PASS\n");
      ctx->GenerateMips(cube_mip_srv);
      printf("UnityResourceProbe GenerateMips(cube_mip_chain)=PASS\n");
      const UINT face = 5;
      const UINT mip1_subresource =
          D3D11CalcSubresource(1, face, cube_mip_desc.MipLevels);
      uint8_t cube_mip_pixel[4] = {};
      hr = read_first_pixel_subresource(device, ctx, cube_mip_tex,
                                        mip1_subresource, cube_mip_pixel);
      print_hr("UnityResourceProbe Readback(cube_face5_mip1)", hr);
      printf("UnityResourceProbe cube_face5_mip1_rgba=%u,%u,%u,%u\n",
             cube_mip_pixel[0], cube_mip_pixel[1], cube_mip_pixel[2],
             cube_mip_pixel[3]);
      if (FAILED(hr) || cube_mip_pixel[0] != 132 ||
          cube_mip_pixel[1] != 136 || cube_mip_pixel[2] != 110 ||
          cube_mip_pixel[3] != 255)
        pass = false;
    }
  }

  uint32_t cube_array_texels[12][4] = {};
  D3D11_SUBRESOURCE_DATA cube_array_data[12] = {};
  for (UINT face = 0; face < ARRAYSIZE(cube_array_texels); face++) {
    const uint8_t r = (uint8_t)(20 + face * 10);
    const uint8_t g = (uint8_t)(40 + face * 7);
    const uint8_t b = (uint8_t)(210 - face * 5);
    const uint32_t texel = 0xff000000u | (b << 16) | (g << 8) | r;
    for (UINT i = 0; i < ARRAYSIZE(cube_array_texels[face]); i++)
      cube_array_texels[face][i] = texel;
    cube_array_data[face].pSysMem = cube_array_texels[face];
    cube_array_data[face].SysMemPitch = 2 * sizeof(uint32_t);
  }
  D3D11_TEXTURE2D_DESC cube_array_desc = cube_desc;
  cube_array_desc.ArraySize = ARRAYSIZE(cube_array_texels);
  ID3D11Texture2D *cube_array_tex = nullptr;
  hr = device->CreateTexture2D(&cube_array_desc, cube_array_data,
                               &cube_array_tex);
  print_hr("UnityResourceProbe CreateTexture2D(cube_array)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *cube_array_srv = nullptr;
  if (cube_array_tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC cube_array_srv_desc = {};
    cube_array_srv_desc.Format = cube_array_desc.Format;
    cube_array_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    cube_array_srv_desc.TextureCubeArray.MipLevels = 1;
    cube_array_srv_desc.TextureCubeArray.NumCubes =
        cube_array_desc.ArraySize / 6;
    hr = device->CreateShaderResourceView(cube_array_tex,
                                          &cube_array_srv_desc,
                                          &cube_array_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(cube_array)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      const UINT face = 11;
      const UINT face_subresource =
          D3D11CalcSubresource(0, face, cube_array_desc.MipLevels);
      uint8_t cube_array_pixel[4] = {};
      hr = read_first_pixel_subresource(device, ctx, cube_array_tex,
                                        face_subresource, cube_array_pixel);
      print_hr("UnityResourceProbe Readback(cube_array_face11)", hr);
      printf("UnityResourceProbe cube_array_face11_rgba=%u,%u,%u,%u\n",
             cube_array_pixel[0], cube_array_pixel[1], cube_array_pixel[2],
             cube_array_pixel[3]);
      if (FAILED(hr) || cube_array_pixel[0] != 130 ||
          cube_array_pixel[1] != 117 || cube_array_pixel[2] != 155 ||
          cube_array_pixel[3] != 255)
        pass = false;
    }
  }

  uint32_t array_texels[2][4] = {};
  D3D11_SUBRESOURCE_DATA array_data[2] = {};
  for (UINT slice = 0; slice < ARRAYSIZE(array_texels); slice++) {
    for (UINT i = 0; i < ARRAYSIZE(array_texels[slice]); i++)
      array_texels[slice][i] = 0xff002010u | (slice << 16) | i;
    array_data[slice].pSysMem = array_texels[slice];
    array_data[slice].SysMemPitch = 2 * sizeof(uint32_t);
  }
  D3D11_TEXTURE2D_DESC array_desc = {};
  array_desc.Width = 2;
  array_desc.Height = 2;
  array_desc.MipLevels = 1;
  array_desc.ArraySize = 2;
  array_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  array_desc.SampleDesc.Count = 1;
  array_desc.Usage = D3D11_USAGE_DEFAULT;
  array_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *array_tex = nullptr;
  hr = device->CreateTexture2D(&array_desc, array_data, &array_tex);
  print_hr("UnityResourceProbe CreateTexture2D(array)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *array_srv = nullptr;
  ID3D11RenderTargetView *array_rtv = nullptr;
  ID3D11Texture2D *array_mip_tex = nullptr;
  ID3D11ShaderResourceView *array_mip_srv = nullptr;
  ID3D11RenderTargetView *array_mip_rtv = nullptr;
  if (array_tex) {
    D3D11_SHADER_RESOURCE_VIEW_DESC array_srv_desc = {};
    array_srv_desc.Format = array_desc.Format;
    array_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    array_srv_desc.Texture2DArray.ArraySize = array_desc.ArraySize;
    array_srv_desc.Texture2DArray.MipLevels = 1;
    hr = device->CreateShaderResourceView(array_tex, &array_srv_desc,
                                          &array_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(texture2d_array)",
             hr);
    if (FAILED(hr))
      pass = false;

    D3D11_RENDER_TARGET_VIEW_DESC array_rtv_desc = {};
    array_rtv_desc.Format = array_desc.Format;
    array_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    array_rtv_desc.Texture2DArray.ArraySize = array_desc.ArraySize;
    hr = device->CreateRenderTargetView(array_tex, &array_rtv_desc,
                                        &array_rtv);
    print_hr("UnityResourceProbe CreateRenderTargetView(texture2d_array)",
             hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      const FLOAT array_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
      ctx->ClearRenderTargetView(array_rtv, array_color);
      printf("UnityResourceProbe ClearRenderTargetView(texture2d_array)=PASS\n");
    }
  }

  {
    static const UINT kArrayMipLevels = 2;
    uint32_t array_mip_texels[4][16] = {};
    D3D11_SUBRESOURCE_DATA array_mip_data[4] = {};
    for (UINT slice = 0; slice < 2; slice++) {
      for (UINT mip = 0; mip < kArrayMipLevels; mip++) {
        const UINT subresource = D3D11CalcSubresource(mip, slice,
                                                      kArrayMipLevels);
        const UINT width = mip ? 2 : 4;
        const UINT height = mip ? 2 : 4;
        for (UINT i = 0; i < width * height; i++)
          array_mip_texels[subresource][i] =
              0xff405060u | (slice << 20) | (mip << 16) | i;
        array_mip_data[subresource].pSysMem = array_mip_texels[subresource];
        array_mip_data[subresource].SysMemPitch = width * sizeof(uint32_t);
      }
    }

    D3D11_TEXTURE2D_DESC array_mip_desc = {};
    array_mip_desc.Width = 4;
    array_mip_desc.Height = 4;
    array_mip_desc.MipLevels = kArrayMipLevels;
    array_mip_desc.ArraySize = 2;
    array_mip_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    array_mip_desc.SampleDesc.Count = 1;
    array_mip_desc.Usage = D3D11_USAGE_DEFAULT;
    array_mip_desc.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    hr = device->CreateTexture2D(&array_mip_desc, array_mip_data,
                                 &array_mip_tex);
    print_hr("UnityResourceProbe CreateTexture2D(array_mips)", hr);
    if (FAILED(hr))
      pass = false;

    if (array_mip_tex) {
      D3D11_SHADER_RESOURCE_VIEW_DESC array_mip_srv_desc = {};
      array_mip_srv_desc.Format = array_mip_desc.Format;
      array_mip_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      array_mip_srv_desc.Texture2DArray.MipLevels =
          array_mip_desc.MipLevels;
      array_mip_srv_desc.Texture2DArray.ArraySize =
          array_mip_desc.ArraySize;
      hr = device->CreateShaderResourceView(array_mip_tex,
                                            &array_mip_srv_desc,
                                            &array_mip_srv);
      print_hr("UnityResourceProbe CreateShaderResourceView(array_mips)", hr);
      if (FAILED(hr))
        pass = false;

      D3D11_RENDER_TARGET_VIEW_DESC array_mip_rtv_desc = {};
      array_mip_rtv_desc.Format = array_mip_desc.Format;
      array_mip_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
      array_mip_rtv_desc.Texture2DArray.MipSlice = 1;
      array_mip_rtv_desc.Texture2DArray.FirstArraySlice = 1;
      array_mip_rtv_desc.Texture2DArray.ArraySize = 1;
      hr = device->CreateRenderTargetView(array_mip_tex,
                                          &array_mip_rtv_desc,
                                          &array_mip_rtv);
      print_hr("UnityResourceProbe CreateRenderTargetView(array_mip_slice)",
               hr);
      if (FAILED(hr)) {
        pass = false;
      } else {
        const FLOAT clear_color[4] = {1.0f, 0.5f, 0.25f, 1.0f};
        ctx->ClearRenderTargetView(array_mip_rtv, clear_color);
        printf("UnityResourceProbe ClearRenderTargetView(array_mip_slice)=PASS\n");
      }

      uint8_t slice0_mip1[4] = {};
      const UINT slice0_mip1_subresource =
          D3D11CalcSubresource(1, 0, kArrayMipLevels);
      hr = read_first_pixel_subresource(device, ctx, array_mip_tex,
                                        slice0_mip1_subresource,
                                        slice0_mip1);
      print_hr("UnityResourceProbe Readback(array_slice0_mip1)", hr);
      printf("UnityResourceProbe array_slice0_mip1_rgba=%u,%u,%u,%u\n",
             slice0_mip1[0], slice0_mip1[1], slice0_mip1[2],
             slice0_mip1[3]);
      if (FAILED(hr) || slice0_mip1[0] != 0x60 ||
          slice0_mip1[1] != 0x50 || slice0_mip1[2] != 0x41 ||
          slice0_mip1[3] != 0xff)
        pass = false;

      uint8_t slice1_mip1[4] = {};
      const UINT slice1_mip1_subresource =
          D3D11CalcSubresource(1, 1, kArrayMipLevels);
      hr = read_first_pixel_subresource(device, ctx, array_mip_tex,
                                        slice1_mip1_subresource,
                                        slice1_mip1);
      print_hr("UnityResourceProbe Readback(array_slice1_mip1)", hr);
      printf("UnityResourceProbe array_slice1_mip1_rgba=%u,%u,%u,%u\n",
             slice1_mip1[0], slice1_mip1[1], slice1_mip1[2],
             slice1_mip1[3]);
      if (FAILED(hr) || slice1_mip1[0] != 255 ||
          slice1_mip1[1] < 127 || slice1_mip1[1] > 129 ||
          slice1_mip1[2] < 63 || slice1_mip1[2] > 65 ||
          slice1_mip1[3] != 255)
        pass = false;

      uint32_t array_mip_update[4] = {
          0xffcc8844u,
          0xffcc8844u,
          0xffcc8844u,
          0xffcc8844u,
      };
      ctx->UpdateSubresource(array_mip_tex, slice0_mip1_subresource,
                             nullptr, array_mip_update,
                             2 * sizeof(uint32_t), 0);
      printf("UnityResourceProbe UpdateSubresource(array_slice0_mip1)=PASS\n");
      ctx->CopySubresourceRegion(array_mip_tex, slice1_mip1_subresource, 0, 0,
                                 0, array_mip_tex, slice0_mip1_subresource,
                                 nullptr);
      printf("UnityResourceProbe CopySubresourceRegion(array_slice0_mip1_to_slice1)=PASS\n");

      uint8_t slice0_mip1_copy[4] = {};
      hr = read_first_pixel_subresource(device, ctx, array_mip_tex,
                                        slice0_mip1_subresource,
                                        slice0_mip1_copy);
      print_hr("UnityResourceProbe Readback(array_slice0_mip1_copy)", hr);
      printf("UnityResourceProbe array_slice0_mip1_copy_rgba=%u,%u,%u,%u\n",
             slice0_mip1_copy[0], slice0_mip1_copy[1],
             slice0_mip1_copy[2], slice0_mip1_copy[3]);
      if (FAILED(hr) || slice0_mip1_copy[0] != 0x44 ||
          slice0_mip1_copy[1] != 0x88 || slice0_mip1_copy[2] != 0xcc ||
          slice0_mip1_copy[3] != 0xff)
        pass = false;

      uint8_t slice1_mip1_copy[4] = {};
      hr = read_first_pixel_subresource(device, ctx, array_mip_tex,
                                        slice1_mip1_subresource,
                                        slice1_mip1_copy);
      print_hr("UnityResourceProbe Readback(array_slice1_mip1_copy)", hr);
      printf("UnityResourceProbe array_slice1_mip1_copy_rgba=%u,%u,%u,%u\n",
             slice1_mip1_copy[0], slice1_mip1_copy[1],
             slice1_mip1_copy[2], slice1_mip1_copy[3]);
      if (FAILED(hr) || slice1_mip1_copy[0] != 0x44 ||
          slice1_mip1_copy[1] != 0x88 || slice1_mip1_copy[2] != 0xcc ||
          slice1_mip1_copy[3] != 0xff)
        pass = false;
    }
  }

  uint32_t structured_elements[16] = {};
  for (UINT i = 0; i < ARRAYSIZE(structured_elements); i++)
    structured_elements[i] = i;
  D3D11_SUBRESOURCE_DATA structured_data = {};
  structured_data.pSysMem = structured_elements;
  D3D11_BUFFER_DESC structured_desc = {};
  structured_desc.ByteWidth = sizeof(structured_elements);
  structured_desc.Usage = D3D11_USAGE_DEFAULT;
  structured_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  structured_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  structured_desc.StructureByteStride = 4 * sizeof(uint32_t);
  ID3D11Buffer *structured_buffer = nullptr;
  hr = device->CreateBuffer(&structured_desc, &structured_data,
                            &structured_buffer);
  print_hr("UnityResourceProbe CreateBuffer(structured)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11ShaderResourceView *structured_srv = nullptr;
  ID3D11UnorderedAccessView *structured_uav = nullptr;
  ID3D11UnorderedAccessView *structured_counter_uav = nullptr;
  ID3D11UnorderedAccessView *structured_append_uav = nullptr;
  if (structured_buffer) {
    D3D11_SHADER_RESOURCE_VIEW_DESC structured_srv_desc = {};
    structured_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    structured_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    structured_srv_desc.Buffer.NumElements = 4;
    hr = device->CreateShaderResourceView(structured_buffer,
                                          &structured_srv_desc,
                                          &structured_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(structured)", hr);
    if (FAILED(hr))
      pass = false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC structured_uav_desc = {};
    structured_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    structured_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    structured_uav_desc.Buffer.NumElements = 4;
    hr = device->CreateUnorderedAccessView(structured_buffer,
                                           &structured_uav_desc,
                                           &structured_uav);
    print_hr("UnityResourceProbe CreateUnorderedAccessView(structured)", hr);
    if (FAILED(hr))
      pass = false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC structured_counter_desc =
        structured_uav_desc;
    structured_counter_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
    hr = device->CreateUnorderedAccessView(structured_buffer,
                                           &structured_counter_desc,
                                           &structured_counter_uav);
    print_hr("UnityResourceProbe CreateUnorderedAccessView(structured_counter)",
             hr);
    if (FAILED(hr))
      pass = false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC structured_append_desc =
        structured_uav_desc;
    structured_append_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    hr = device->CreateUnorderedAccessView(structured_buffer,
                                           &structured_append_desc,
                                           &structured_append_uav);
    print_hr("UnityResourceProbe CreateUnorderedAccessView(structured_append)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC raw_desc = {};
  raw_desc.ByteWidth = 64;
  raw_desc.Usage = D3D11_USAGE_DEFAULT;
  raw_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  raw_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ID3D11Buffer *raw_buffer = nullptr;
  hr = device->CreateBuffer(&raw_desc, nullptr, &raw_buffer);
  print_hr("UnityResourceProbe CreateBuffer(raw)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ShaderResourceView *raw_srv = nullptr;
  ID3D11UnorderedAccessView *raw_uav = nullptr;
  if (raw_buffer) {
    D3D11_SHADER_RESOURCE_VIEW_DESC raw_srv_desc = {};
    raw_srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    raw_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    raw_srv_desc.BufferEx.NumElements = raw_desc.ByteWidth / sizeof(uint32_t);
    raw_srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    hr = device->CreateShaderResourceView(raw_buffer, &raw_srv_desc, &raw_srv);
    print_hr("UnityResourceProbe CreateShaderResourceView(raw)", hr);
    if (FAILED(hr))
      pass = false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC raw_uav_desc = {};
    raw_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    raw_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    raw_uav_desc.Buffer.NumElements = raw_desc.ByteWidth / sizeof(uint32_t);
    raw_uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    hr = device->CreateUnorderedAccessView(raw_buffer, &raw_uav_desc, &raw_uav);
    print_hr("UnityResourceProbe CreateUnorderedAccessView(raw)", hr);
    if (FAILED(hr))
      pass = false;
  }

  if (raw_uav)
    raw_uav->Release();
  if (raw_srv)
    raw_srv->Release();
  if (raw_buffer)
    raw_buffer->Release();
  if (structured_append_uav)
    structured_append_uav->Release();
  if (structured_counter_uav)
    structured_counter_uav->Release();
  if (structured_uav)
    structured_uav->Release();
  if (structured_srv)
    structured_srv->Release();
  if (structured_buffer)
    structured_buffer->Release();
  if (mip_srv)
    mip_srv->Release();
  if (mip_tex)
    mip_tex->Release();
  if (copy_tex)
    copy_tex->Release();
  if (array_mip_rtv)
    array_mip_rtv->Release();
  if (array_mip_srv)
    array_mip_srv->Release();
  if (array_mip_tex)
    array_mip_tex->Release();
  if (array_rtv)
    array_rtv->Release();
  if (array_srv)
    array_srv->Release();
  if (array_tex)
    array_tex->Release();
  if (tex1d_mip_staging)
    tex1d_mip_staging->Release();
  if (tex1d_mip_srv)
    tex1d_mip_srv->Release();
  if (tex1d_mip)
    tex1d_mip->Release();
  if (tex1d_srv)
    tex1d_srv->Release();
  if (tex1d)
    tex1d->Release();
  if (cube_array_srv)
    cube_array_srv->Release();
  if (cube_array_tex)
    cube_array_tex->Release();
  if (cube_mip_srv)
    cube_mip_srv->Release();
  if (cube_mip_tex)
    cube_mip_tex->Release();
  if (cube_srv)
    cube_srv->Release();
  if (cube_tex)
    cube_tex->Release();
  if (volume_srv)
    volume_srv->Release();
  if (volume_tex)
    volume_tex->Release();
  if (uav_float)
    uav_float->Release();
  if (uav_float_staging)
    uav_float_staging->Release();
  if (uav_float_tex)
    uav_float_tex->Release();
  if (uav_staging)
    uav_staging->Release();
  if (uav)
    uav->Release();
  if (uav_tex)
    uav_tex->Release();
  if (resolve_tex)
    resolve_tex->Release();
  if (msaa_srv)
    msaa_srv->Release();
  if (msaa_rtv)
    msaa_rtv->Release();
  if (msaa_tex)
    msaa_tex->Release();
  if (depth_srv)
    depth_srv->Release();
  if (typeless_dsv)
    typeless_dsv->Release();
  if (typeless_depth_tex)
    typeless_depth_tex->Release();
  if (dsv)
    dsv->Release();
  if (depth_tex)
    depth_tex->Release();
  if (constant_buffer)
    constant_buffer->Release();
  if (partial_constant_staging)
    partial_constant_staging->Release();
  if (partial_constant_buffer)
    partial_constant_buffer->Release();
  if (overlap_staging)
    overlap_staging->Release();
  if (overlap_buffer)
    overlap_buffer->Release();
  if (device3_buffer)
    device3_buffer->Release();
  if (update_staging_tex)
    update_staging_tex->Release();
  if (update_staging_buffer)
    update_staging_buffer->Release();
  if (dynamic_buffer)
    dynamic_buffer->Release();
  if (dynamic_constant_buffer)
    dynamic_constant_buffer->Release();
  if (device3)
    device3->Release();
  if (ctx1)
    ctx1->Release();
  if (rtv)
    rtv->Release();
  if (srv)
    srv->Release();
  if (color_tex)
    color_tex->Release();

  printf("UnityResourceProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_srgb_render_target(ID3D11Device *device,
                                           ID3D11DeviceContext *ctx) {
  bool pass = true;

  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = 4;
  tex_desc.Height = 4;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_DEFAULT;
  tex_desc.BindFlags =
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  ID3D11Texture2D *srgb_tex = nullptr;
  HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, &srgb_tex);
  print_hr("UnitySRGBProbe CreateTexture2D(srgb_rtv)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11RenderTargetView *srgb_rtv = nullptr;
  if (srgb_tex) {
    hr = device->CreateRenderTargetView(srgb_tex, nullptr, &srgb_rtv);
    print_hr("UnitySRGBProbe CreateRenderTargetView(srgb)", hr);
    if (FAILED(hr))
      pass = false;
  }

  if (srgb_rtv) {
    const FLOAT clear_color[4] = {0.25f, 0.0f, 1.0f, 1.0f};
    ctx->ClearRenderTargetView(srgb_rtv, clear_color);
    printf("UnitySRGBProbe ClearRenderTargetView linear_rgba=0.25,0,1,1\n");
  }

  D3D11_TEXTURE2D_DESC staging_desc = tex_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging = nullptr;
  hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  print_hr("UnitySRGBProbe CreateTexture2D(staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (srgb_tex && staging) {
    ctx->CopyResource(staging, srgb_tex);
    printf("UnitySRGBProbe CopyResource(srgb_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnitySRGBProbe Map(staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint8_t *rgba = (const uint8_t *)mapped.pData;
      printf("UnitySRGBProbe staging_rgba=%u,%u,%u,%u\n", rgba[0], rgba[1],
             rgba[2], rgba[3]);
      if (rgba[0] < 134 || rgba[0] > 138 || rgba[1] != 0 ||
          rgba[2] != 255 || rgba[3] != 255)
        pass = false;
      ctx->Unmap(staging, 0);
    } else {
      pass = false;
    }
  }

  if (staging)
    staging->Release();
  if (srgb_rtv)
    srgb_rtv->Release();
  if (srgb_tex)
    srgb_tex->Release();

  printf("UnitySRGBProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_srgb_sampling(ID3D11Device *device,
                                      ID3D11DeviceContext *ctx) {
  static const char shader_source[] =
      "Texture2D g_tex : register(t0);\n"
      "SamplerState g_sampler : register(s0);\n"
      "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
      "VSOut vs_main(uint vid : SV_VertexID) {\n"
      "  float2 pos[3] = {float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0)};\n"
      "  VSOut output;\n"
      "  output.pos = float4(pos[vid], 0.0, 1.0);\n"
      "  output.uv = pos[vid] * 0.5 + 0.5;\n"
      "  return output;\n"
      "}\n"
      "float4 ps_main(VSOut input) : SV_TARGET { return g_tex.Sample(g_sampler, input.uv); }\n";

  HMODULE compiler = load_local_dll(L"d3dcompiler_47.dll", "d3dcompiler_47");
  if (!compiler)
    return false;

  auto compile = (D3DCompileProc)GetProcAddress(compiler, "D3DCompile");
  printf("UnitySRGBSampleProbe GetProcAddress(D3DCompile) proc=%p gle=%lu\n",
         compile, GetLastError());
  if (!compile)
    return false;

  bool pass = true;
  ID3DBlob *vs_blob = nullptr;
  ID3DBlob *ps_blob = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT hr = compile(shader_source, sizeof(shader_source) - 1,
                       "dxmt_headless_unity_srgb_sample", nullptr, nullptr,
                       "vs_main", "vs_5_0", 0, 0, &vs_blob, &errors);
  print_hr("UnitySRGBSampleProbe D3DCompile(vs_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnitySRGBSampleProbe vs_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    return false;
  }
  if (errors) {
    errors->Release();
    errors = nullptr;
  }

  hr = compile(shader_source, sizeof(shader_source) - 1,
               "dxmt_headless_unity_srgb_sample", nullptr, nullptr,
               "ps_main", "ps_5_0", 0, 0, &ps_blob, &errors);
  print_hr("UnitySRGBSampleProbe D3DCompile(ps_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnitySRGBSampleProbe ps_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (vs_blob)
      vs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  ID3D11VertexShader *vs = nullptr;
  hr = device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                  vs_blob->GetBufferSize(), nullptr, &vs);
  print_hr("UnitySRGBSampleProbe CreateVertexShader", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11PixelShader *ps = nullptr;
  hr = device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                 ps_blob->GetBufferSize(), nullptr, &ps);
  print_hr("UnitySRGBSampleProbe CreatePixelShader", hr);
  if (FAILED(hr))
    pass = false;

  const uint32_t encoded_srgb_texel = 0xff888888u;
  D3D11_TEXTURE2D_DESC sample_desc = {};
  sample_desc.Width = 1;
  sample_desc.Height = 1;
  sample_desc.MipLevels = 1;
  sample_desc.ArraySize = 1;
  sample_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  sample_desc.SampleDesc.Count = 1;
  sample_desc.Usage = D3D11_USAGE_IMMUTABLE;
  sample_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sample_data = {};
  sample_data.pSysMem = &encoded_srgb_texel;
  sample_data.SysMemPitch = sizeof(encoded_srgb_texel);
  ID3D11Texture2D *sample_tex = nullptr;
  hr = device->CreateTexture2D(&sample_desc, &sample_data, &sample_tex);
  print_hr("UnitySRGBSampleProbe CreateTexture2D(srgb_sample)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ShaderResourceView *sample_srv = nullptr;
  if (sample_tex) {
    hr = device->CreateShaderResourceView(sample_tex, nullptr, &sample_srv);
    print_hr("UnitySRGBSampleProbe CreateShaderResourceView(srgb_sample)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_TEXTURE2D_DESC target_desc = {};
  target_desc.Width = 4;
  target_desc.Height = 4;
  target_desc.MipLevels = 1;
  target_desc.ArraySize = 1;
  target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target_desc.SampleDesc.Count = 1;
  target_desc.Usage = D3D11_USAGE_DEFAULT;
  target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *target = nullptr;
  hr = device->CreateTexture2D(&target_desc, nullptr, &target);
  print_hr("UnitySRGBSampleProbe CreateTexture2D(linear_target)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11RenderTargetView *target_rtv = nullptr;
  if (target) {
    hr = device->CreateRenderTargetView(target, nullptr, &target_rtv);
    print_hr("UnitySRGBSampleProbe CreateRenderTargetView(linear_target)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *sampler = nullptr;
  hr = device->CreateSamplerState(&sampler_desc, &sampler);
  print_hr("UnitySRGBSampleProbe CreateSamplerState(point_clamp)", hr);
  if (FAILED(hr))
    pass = false;

  if (vs && ps && sample_srv && sampler && target_rtv) {
    D3D11_VIEWPORT viewport = {0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &viewport);
    ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &sample_srv);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->Draw(3, 0);
    printf("UnitySRGBSampleProbe Draw(srgb_sample)=PASS\n");

    ID3D11ShaderResourceView *null_srv[] = {nullptr};
    ID3D11SamplerState *null_sampler[] = {nullptr};
    ctx->PSSetShaderResources(0, 1, null_srv);
    ctx->PSSetSamplers(0, 1, null_sampler);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->VSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);

    uint8_t pixel[4] = {};
    hr = read_first_pixel(device, ctx, target, pixel);
    print_hr("UnitySRGBSampleProbe Readback(linear_target)", hr);
    printf("UnitySRGBSampleProbe sampled_rgba=%u,%u,%u,%u\n", pixel[0],
           pixel[1], pixel[2], pixel[3]);
    if (FAILED(hr) || pixel[0] < 61 || pixel[0] > 65 || pixel[1] < 61 ||
        pixel[1] > 65 || pixel[2] < 61 || pixel[2] > 65 || pixel[3] != 255)
      pass = false;
  } else {
    pass = false;
  }

  if (sampler)
    sampler->Release();
  if (target_rtv)
    target_rtv->Release();
  if (target)
    target->Release();
  if (sample_srv)
    sample_srv->Release();
  if (sample_tex)
    sample_tex->Release();
  if (ps)
    ps->Release();
  if (vs)
    vs->Release();
  if (ps_blob)
    ps_blob->Release();
  if (vs_blob)
    vs_blob->Release();

  printf("UnitySRGBSampleProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_msaa_resolve(ID3D11Device *device,
                                     ID3D11DeviceContext *ctx) {
  bool pass = true;
  const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  const DXGI_FORMAT depth_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  UINT quality_levels = 0;
  HRESULT hr = device->CheckMultisampleQualityLevels(format, 4, &quality_levels);
  print_hr("UnityMSAAProbe CheckMultisampleQualityLevels(4x)", hr);
  printf("UnityMSAAProbe quality_levels=%u\n", quality_levels);
  if (FAILED(hr) || !quality_levels)
    pass = false;

  UINT depth_quality_levels = 0;
  hr = device->CheckMultisampleQualityLevels(depth_format, 4,
                                             &depth_quality_levels);
  print_hr("UnityMSAAProbe CheckMultisampleQualityLevels(D24S8,4x)", hr);
  printf("UnityMSAAProbe depth_quality_levels=%u\n", depth_quality_levels);
  if (FAILED(hr) || !depth_quality_levels)
    pass = false;

  D3D11_TEXTURE2D_DESC msaa_desc = {};
  msaa_desc.Width = 8;
  msaa_desc.Height = 8;
  msaa_desc.MipLevels = 1;
  msaa_desc.ArraySize = 1;
  msaa_desc.Format = format;
  msaa_desc.SampleDesc.Count = 4;
  msaa_desc.SampleDesc.Quality = quality_levels ? quality_levels - 1 : 0;
  msaa_desc.Usage = D3D11_USAGE_DEFAULT;
  msaa_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  ID3D11Texture2D *msaa_tex = nullptr;
  ID3D11RenderTargetView *msaa_rtv = nullptr;
  ID3D11Texture2D *msaa_depth_tex = nullptr;
  ID3D11DepthStencilView *msaa_dsv = nullptr;
  ID3D11Texture2D *resolved_tex = nullptr;
  if (pass) {
    hr = device->CreateTexture2D(&msaa_desc, nullptr, &msaa_tex);
    print_hr("UnityMSAAProbe CreateTexture2D(msaa)", hr);
    if (FAILED(hr))
      pass = false;
  }
  if (msaa_tex) {
    hr = device->CreateRenderTargetView(msaa_tex, nullptr, &msaa_rtv);
    print_hr("UnityMSAAProbe CreateRenderTargetView(msaa)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_TEXTURE2D_DESC depth_desc = msaa_desc;
  depth_desc.Format = depth_format;
  depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  depth_desc.SampleDesc.Quality =
      depth_quality_levels ? depth_quality_levels - 1 : 0;
  if (pass) {
    hr = device->CreateTexture2D(&depth_desc, nullptr, &msaa_depth_tex);
    print_hr("UnityMSAAProbe CreateTexture2D(msaa_depth)", hr);
    if (FAILED(hr))
      pass = false;
  }
  if (msaa_depth_tex) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Format = depth_format;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    hr = device->CreateDepthStencilView(msaa_depth_tex, &dsv_desc, &msaa_dsv);
    print_hr("UnityMSAAProbe CreateDepthStencilView(msaa_depth)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_TEXTURE2D_DESC resolved_desc = msaa_desc;
  resolved_desc.SampleDesc.Count = 1;
  resolved_desc.SampleDesc.Quality = 0;
  resolved_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (pass) {
    hr = device->CreateTexture2D(&resolved_desc, nullptr, &resolved_tex);
    print_hr("UnityMSAAProbe CreateTexture2D(resolved)", hr);
    if (FAILED(hr))
      pass = false;
  }

  if (pass && msaa_rtv && msaa_dsv && resolved_tex) {
    ID3D11RenderTargetView *rtvs[] = {msaa_rtv};
    ctx->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, msaa_dsv);
    printf("UnityMSAAProbe OMSetRenderTargets(msaa_color_depth)=PASS\n");
    ctx->ClearDepthStencilView(msaa_dsv,
                               D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f,
                               7);
    printf("UnityMSAAProbe ClearDepthStencilView(msaa_depth)=PASS\n");

    const FLOAT color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(msaa_rtv, color);
    ctx->ResolveSubresource(resolved_tex, 0, msaa_tex, 0, format);
    printf("UnityMSAAProbe ResolveSubresource=called\n");

    uint8_t pixel[4] = {};
    hr = read_first_pixel(device, ctx, resolved_tex, pixel);
    print_hr("UnityMSAAProbe Readback(resolved)", hr);
    printf("UnityMSAAProbe resolved_bgra=%u,%u,%u,%u\n", pixel[0], pixel[1],
           pixel[2], pixel[3]);
    if (FAILED(hr) || pixel[0] > 2 || pixel[1] < 235 || pixel[2] > 2 ||
        pixel[3] != 255)
      pass = false;
  }

  if (resolved_tex)
    resolved_tex->Release();
  if (msaa_dsv)
    msaa_dsv->Release();
  if (msaa_depth_tex)
    msaa_depth_tex->Release();
  if (msaa_rtv)
    msaa_rtv->Release();
  if (msaa_tex)
    msaa_tex->Release();

  struct MsaaFamilyCase {
    DXGI_FORMAT format;
    const char *name;
    UINT samples;
    UINT expected_size;
    uint8_t expected[8];
  };
  static const MsaaFamilyCase msaa_cases[] = {
      {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 2, 4,
       {0x00, 0xff, 0x00, 0xff}},
      {DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4, 4,
       {0x00, 0xff, 0x00, 0xff}},
      {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 2, 4,
       {0x00, 0xff, 0x00, 0xff}},
      {DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 4, 4,
       {0x00, 0xff, 0x00, 0xff}},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 2, 8,
       {0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x3c}},
      {DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 4, 8,
       {0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00, 0x3c}},
  };

  for (UINT i = 0; i < ARRAYSIZE(msaa_cases); i++) {
    const MsaaFamilyCase &test = msaa_cases[i];
    UINT family_quality = 0;
    hr = device->CheckMultisampleQualityLevels(test.format, test.samples,
                                               &family_quality);
    printf("UnityMSAAProbe case=%s samples=%u hr=0x%08lx quality=%u\n",
           test.name, test.samples, (unsigned long)(uint32_t)hr,
           family_quality);
    if (FAILED(hr) || !family_quality) {
      pass = false;
      continue;
    }

    D3D11_TEXTURE2D_DESC family_msaa_desc = {};
    family_msaa_desc.Width = 8;
    family_msaa_desc.Height = 8;
    family_msaa_desc.MipLevels = 1;
    family_msaa_desc.ArraySize = 1;
    family_msaa_desc.Format = test.format;
    family_msaa_desc.SampleDesc.Count = test.samples;
    family_msaa_desc.SampleDesc.Quality = family_quality - 1;
    family_msaa_desc.Usage = D3D11_USAGE_DEFAULT;
    family_msaa_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D *family_msaa_tex = nullptr;
    char label[192] = {};
    snprintf(label, sizeof(label), "UnityMSAAProbe CreateTexture2D(%s_%ux)",
             test.name, test.samples);
    hr = device->CreateTexture2D(&family_msaa_desc, nullptr, &family_msaa_tex);
    print_hr(label, hr);
    if (FAILED(hr)) {
      pass = false;
      continue;
    }

    ID3D11RenderTargetView *family_rtv = nullptr;
    hr = device->CreateRenderTargetView(family_msaa_tex, nullptr, &family_rtv);
    snprintf(label, sizeof(label),
             "UnityMSAAProbe CreateRenderTargetView(%s_%ux)", test.name,
             test.samples);
    print_hr(label, hr);
    if (FAILED(hr)) {
      pass = false;
      family_msaa_tex->Release();
      continue;
    }

    D3D11_TEXTURE2D_DESC family_resolved_desc = family_msaa_desc;
    family_resolved_desc.SampleDesc.Count = 1;
    family_resolved_desc.SampleDesc.Quality = 0;
    family_resolved_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *family_resolved = nullptr;
    hr = device->CreateTexture2D(&family_resolved_desc, nullptr,
                                 &family_resolved);
    snprintf(label, sizeof(label), "UnityMSAAProbe CreateTexture2D(%s_%ux_resolved)",
             test.name, test.samples);
    print_hr(label, hr);
    if (FAILED(hr)) {
      pass = false;
      family_rtv->Release();
      family_msaa_tex->Release();
      continue;
    }

    const FLOAT family_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(family_rtv, family_color);
    printf("UnityMSAAProbe ClearRenderTargetView(%s_%ux)=PASS\n", test.name,
           test.samples);
    ctx->ResolveSubresource(family_resolved, 0, family_msaa_tex, 0,
                            test.format);
    printf("UnityMSAAProbe ResolveSubresource(%s_%ux)=PASS\n", test.name,
           test.samples);

    D3D11_TEXTURE2D_DESC family_staging_desc = family_resolved_desc;
    family_staging_desc.Usage = D3D11_USAGE_STAGING;
    family_staging_desc.BindFlags = 0;
    family_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *family_staging = nullptr;
    hr = device->CreateTexture2D(&family_staging_desc, nullptr,
                                 &family_staging);
    snprintf(label, sizeof(label), "UnityMSAAProbe CreateTexture2D(%s_%ux_staging)",
             test.name, test.samples);
    print_hr(label, hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      ctx->CopyResource(family_staging, family_resolved);
      printf("UnityMSAAProbe CopyResource(%s_%ux_resolved_to_staging)=PASS\n",
             test.name, test.samples);
      snprintf(label, sizeof(label), "UnityMSAAProbe %s_%ux_resolved_staging",
               test.name, test.samples);
      if (!check_texture_bytes(ctx, family_staging, label, test.expected,
                               test.expected_size))
        pass = false;
    }

    if (family_staging)
      family_staging->Release();
    family_resolved->Release();
    family_rtv->Release();
    family_msaa_tex->Release();
  }

  UINT array_quality = 0;
  hr = device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 2,
                                             &array_quality);
  print_hr("UnityMSAAProbe CheckMultisampleQualityLevels(array_2x)", hr);
  printf("UnityMSAAProbe array_quality=%u\n", array_quality);
  if (FAILED(hr) || !array_quality) {
    pass = false;
  } else {
    D3D11_TEXTURE2D_DESC array_msaa_desc = {};
    array_msaa_desc.Width = 8;
    array_msaa_desc.Height = 8;
    array_msaa_desc.MipLevels = 1;
    array_msaa_desc.ArraySize = 2;
    array_msaa_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    array_msaa_desc.SampleDesc.Count = 2;
    array_msaa_desc.SampleDesc.Quality = array_quality - 1;
    array_msaa_desc.Usage = D3D11_USAGE_DEFAULT;
    array_msaa_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D *array_msaa_tex = nullptr;
    hr = device->CreateTexture2D(&array_msaa_desc, nullptr, &array_msaa_tex);
    print_hr("UnityMSAAProbe CreateTexture2D(array_2x)", hr);
    if (FAILED(hr)) {
      pass = false;
    } else {
      D3D11_RENDER_TARGET_VIEW_DESC array_rtv_desc = {};
      array_rtv_desc.Format = array_msaa_desc.Format;
      array_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
      array_rtv_desc.Texture2DMSArray.FirstArraySlice = 1;
      array_rtv_desc.Texture2DMSArray.ArraySize = 1;
      ID3D11RenderTargetView *array_rtv = nullptr;
      hr = device->CreateRenderTargetView(array_msaa_tex, &array_rtv_desc,
                                          &array_rtv);
      print_hr("UnityMSAAProbe CreateRenderTargetView(array_slice1_2x)", hr);
      if (FAILED(hr)) {
        pass = false;
      } else {
        D3D11_TEXTURE2D_DESC array_resolved_desc = array_msaa_desc;
        array_resolved_desc.SampleDesc.Count = 1;
        array_resolved_desc.SampleDesc.Quality = 0;
        array_resolved_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D *array_resolved = nullptr;
        hr = device->CreateTexture2D(&array_resolved_desc, nullptr,
                                     &array_resolved);
        print_hr("UnityMSAAProbe CreateTexture2D(array_2x_resolved)", hr);
        if (FAILED(hr)) {
          pass = false;
        } else {
          const FLOAT array_color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
          ctx->ClearRenderTargetView(array_rtv, array_color);
          printf("UnityMSAAProbe ClearRenderTargetView(array_slice1_2x)=PASS\n");
          const UINT slice1 = D3D11CalcSubresource(0, 1, 1);
          ctx->ResolveSubresource(array_resolved, slice1, array_msaa_tex,
                                  slice1, DXGI_FORMAT_R8G8B8A8_UNORM);
          printf("UnityMSAAProbe ResolveSubresource(array_slice1_2x)=PASS\n");

          D3D11_TEXTURE2D_DESC array_staging_desc = array_resolved_desc;
          array_staging_desc.Usage = D3D11_USAGE_STAGING;
          array_staging_desc.BindFlags = 0;
          array_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          ID3D11Texture2D *array_staging = nullptr;
          hr = device->CreateTexture2D(&array_staging_desc, nullptr,
                                       &array_staging);
          print_hr("UnityMSAAProbe CreateTexture2D(array_2x_staging)", hr);
          if (FAILED(hr)) {
            pass = false;
          } else {
            ctx->CopyResource(array_staging, array_resolved);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            hr = ctx->Map(array_staging, slice1, D3D11_MAP_READ, 0, &mapped);
            print_hr("UnityMSAAProbe Map(array_slice1_2x_resolved)", hr);
            if (SUCCEEDED(hr)) {
              const uint8_t *src = (const uint8_t *)mapped.pData;
              printf("UnityMSAAProbe array_slice1_resolved_rgba=%u,%u,%u,%u\n",
                     src[0], src[1], src[2], src[3]);
              if (src[0] < 235 || src[1] > 2 || src[2] > 2 ||
                  src[3] != 255)
                pass = false;
              ctx->Unmap(array_staging, slice1);
            } else {
              pass = false;
            }
            array_staging->Release();
          }
          array_resolved->Release();
        }
        array_rtv->Release();
      }
      array_msaa_tex->Release();
    }
  }

  printf("UnityMSAAProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_resource_residency_churn(ID3D11Device *device,
                                                 ID3D11DeviceContext *ctx) {
  bool pass = true;
  const UINT resource_count = 32;
  ID3D11Texture2D *textures[32] = {};
  ID3D11ShaderResourceView *srvs[32] = {};
  ID3D11RenderTargetView *rtvs[32] = {};

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = 16;
  desc.Height = 16;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  for (UINT i = 0; i < resource_count; i++) {
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &textures[i]);
    if (FAILED(hr)) {
      printf("UnityResidencyProbe CreateTexture2D[%u] hr=0x%08lx\n", i,
             (unsigned long)(uint32_t)hr);
      pass = false;
      break;
    }
    hr = device->CreateShaderResourceView(textures[i], nullptr, &srvs[i]);
    if (FAILED(hr)) {
      printf("UnityResidencyProbe CreateShaderResourceView[%u] hr=0x%08lx\n",
             i, (unsigned long)(uint32_t)hr);
      pass = false;
      break;
    }
    hr = device->CreateRenderTargetView(textures[i], nullptr, &rtvs[i]);
    if (FAILED(hr)) {
      printf("UnityResidencyProbe CreateRenderTargetView[%u] hr=0x%08lx\n",
             i, (unsigned long)(uint32_t)hr);
      pass = false;
      break;
    }
    const FLOAT color[4] = {i == resource_count - 1 ? 0.0f : 1.0f,
                            i == resource_count - 1 ? 1.0f : 0.0f,
                            0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtvs[i], color);
  }
  printf("UnityResidencyProbe created_textures=%u result_so_far=%s\n",
         resource_count, pass ? "PASS" : "FAIL");

  IDXGIDevice2 *dxgi_device2 = nullptr;
  HRESULT residency_hr =
      device->QueryInterface(__uuidof(IDXGIDevice2), (void **)&dxgi_device2);
  print_hr("UnityResidencyProbe QueryInterface(IDXGIDevice2)", residency_hr);
  if (FAILED(residency_hr) || !dxgi_device2) {
    pass = false;
  } else {
    if (textures[0]) {
      IDXGIResource *offer_resource = nullptr;
      residency_hr =
          textures[0]->QueryInterface(__uuidof(IDXGIResource),
                                      (void **)&offer_resource);
      print_hr("UnityResidencyProbe QueryInterface(IDXGIResource)",
               residency_hr);
      if (FAILED(residency_hr) || !offer_resource) {
        pass = false;
      } else {
        IDXGIResource *resources[] = {offer_resource};
        residency_hr = dxgi_device2->OfferResources(
            ARRAYSIZE(resources), resources,
            DXGI_OFFER_RESOURCE_PRIORITY_NORMAL);
        print_hr("UnityResidencyProbe OfferResources", residency_hr);
        if (FAILED(residency_hr))
          pass = false;

        BOOL discarded[] = {TRUE};
        residency_hr =
            dxgi_device2->ReclaimResources(ARRAYSIZE(resources), resources,
                                           discarded);
        print_hr("UnityResidencyProbe ReclaimResources", residency_hr);
        printf("UnityResidencyProbe reclaim_discarded=%u\n", discarded[0]);
        if (FAILED(residency_hr) || discarded[0])
          pass = false;

        residency_hr =
            dxgi_device2->ReclaimResources(ARRAYSIZE(resources), resources,
                                           nullptr);
        print_hr("UnityResidencyProbe ReclaimResources(null_discarded)",
                 residency_hr);
        if (FAILED(residency_hr))
          pass = false;

        residency_hr = dxgi_device2->OfferResources(
            ARRAYSIZE(resources), resources,
            (DXGI_OFFER_RESOURCE_PRIORITY)0xffffffffu);
        print_hr("UnityResidencyProbe OfferResources(invalid_priority)",
                 residency_hr);
        if (residency_hr != E_INVALIDARG)
          pass = false;

        IDXGIResource *null_resource[] = {nullptr};
        residency_hr = dxgi_device2->OfferResources(
            ARRAYSIZE(null_resource), null_resource,
            DXGI_OFFER_RESOURCE_PRIORITY_NORMAL);
        print_hr("UnityResidencyProbe OfferResources(null_resource)",
                 residency_hr);
        if (residency_hr != E_INVALIDARG)
          pass = false;

        residency_hr =
            dxgi_device2->ReclaimResources(1, nullptr, discarded);
        print_hr("UnityResidencyProbe ReclaimResources(null_resources)",
                 residency_hr);
        if (residency_hr != E_INVALIDARG)
          pass = false;

        offer_resource->Release();
      }
    }
    dxgi_device2->Release();
  }

  if (pass && textures[resource_count - 1]) {
    uint8_t pixel[4] = {};
    HRESULT hr = read_first_pixel(device, ctx, textures[resource_count - 1],
                                  pixel);
    print_hr("UnityResidencyProbe Readback(last)", hr);
    printf("UnityResidencyProbe last_rgba=%u,%u,%u,%u\n", pixel[0], pixel[1],
           pixel[2], pixel[3]);
    if (FAILED(hr) || pixel[0] > 2 || pixel[1] < 235 || pixel[2] > 2 ||
        pixel[3] != 255)
      pass = false;
  }

  for (UINT i = resource_count; i > 0; i--) {
    UINT index = i - 1;
    if (rtvs[index])
      rtvs[index]->Release();
    if (srvs[index])
      srvs[index]->Release();
    if (textures[index])
      textures[index]->Release();
  }

  printf("UnityResidencyProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_states(ID3D11Device *device, ID3D11DeviceContext *ctx) {
  bool pass = true;

  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState *blend_state = nullptr;
  HRESULT hr = device->CreateBlendState(&blend_desc, &blend_state);
  print_hr("UnityStateProbe CreateBlendState(alpha)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_RASTERIZER_DESC raster_desc = {};
  raster_desc.FillMode = D3D11_FILL_SOLID;
  raster_desc.CullMode = D3D11_CULL_NONE;
  raster_desc.DepthClipEnable = TRUE;
  raster_desc.ScissorEnable = TRUE;
  ID3D11RasterizerState *raster_state = nullptr;
  hr = device->CreateRasterizerState(&raster_desc, &raster_state);
  print_hr("UnityStateProbe CreateRasterizerState(cull_none_scissor)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_DEPTH_STENCIL_DESC depth_desc = {};
  depth_desc.DepthEnable = TRUE;
  depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depth_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  depth_desc.StencilEnable = TRUE;
  depth_desc.StencilReadMask = 0xff;
  depth_desc.StencilWriteMask = 0xff;
  depth_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  depth_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
  depth_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  depth_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  depth_desc.BackFace = depth_desc.FrontFace;
  ID3D11DepthStencilState *depth_state = nullptr;
  hr = device->CreateDepthStencilState(&depth_desc, &depth_state);
  print_hr("UnityStateProbe CreateDepthStencilState(depth_stencil)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *sampler_state = nullptr;
  hr = device->CreateSamplerState(&sampler_desc, &sampler_state);
  print_hr("UnityStateProbe CreateSamplerState(linear_wrap)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BLEND_DESC opaque_blend_desc = {};
  opaque_blend_desc.RenderTarget[0].BlendEnable = FALSE;
  opaque_blend_desc.RenderTarget[0].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState *opaque_blend_state = nullptr;
  hr = device->CreateBlendState(&opaque_blend_desc, &opaque_blend_state);
  print_hr("UnityStateProbe CreateBlendState(opaque)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BLEND_DESC independent_blend_desc = blend_desc;
  independent_blend_desc.IndependentBlendEnable = TRUE;
  independent_blend_desc.RenderTarget[1].BlendEnable = FALSE;
  independent_blend_desc.RenderTarget[1].RenderTargetWriteMask =
      D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
  ID3D11BlendState *independent_blend_state = nullptr;
  hr = device->CreateBlendState(&independent_blend_desc,
                                &independent_blend_state);
  print_hr("UnityStateProbe CreateBlendState(independent)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_RASTERIZER_DESC back_raster_desc = {};
  back_raster_desc.FillMode = D3D11_FILL_SOLID;
  back_raster_desc.CullMode = D3D11_CULL_BACK;
  back_raster_desc.DepthClipEnable = TRUE;
  ID3D11RasterizerState *back_raster_state = nullptr;
  hr = device->CreateRasterizerState(&back_raster_desc, &back_raster_state);
  print_hr("UnityStateProbe CreateRasterizerState(cull_back)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_RASTERIZER_DESC wire_raster_desc = back_raster_desc;
  wire_raster_desc.FillMode = D3D11_FILL_WIREFRAME;
  wire_raster_desc.CullMode = D3D11_CULL_FRONT;
  wire_raster_desc.DepthClipEnable = FALSE;
  ID3D11RasterizerState *wire_raster_state = nullptr;
  hr = device->CreateRasterizerState(&wire_raster_desc, &wire_raster_state);
  print_hr("UnityStateProbe CreateRasterizerState(wire_front_no_depthclip)",
           hr);
  if (FAILED(hr))
    pass = false;

  D3D11_DEPTH_STENCIL_DESC disabled_depth_desc = {};
  disabled_depth_desc.DepthEnable = FALSE;
  disabled_depth_desc.StencilEnable = FALSE;
  ID3D11DepthStencilState *disabled_depth_state = nullptr;
  hr = device->CreateDepthStencilState(&disabled_depth_desc,
                                       &disabled_depth_state);
  print_hr("UnityStateProbe CreateDepthStencilState(disabled)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_SAMPLER_DESC comparison_sampler_desc = {};
  comparison_sampler_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  comparison_sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  comparison_sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  comparison_sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  comparison_sampler_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
  comparison_sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *comparison_sampler_state = nullptr;
  hr = device->CreateSamplerState(&comparison_sampler_desc,
                                  &comparison_sampler_state);
  print_hr("UnityStateProbe CreateSamplerState(comparison_clamp)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_SAMPLER_DESC anisotropic_sampler_desc = {};
  anisotropic_sampler_desc.Filter = D3D11_FILTER_ANISOTROPIC;
  anisotropic_sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
  anisotropic_sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
  anisotropic_sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  anisotropic_sampler_desc.MaxAnisotropy = 4;
  anisotropic_sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *anisotropic_sampler_state = nullptr;
  hr = device->CreateSamplerState(&anisotropic_sampler_desc,
                                  &anisotropic_sampler_state);
  print_hr("UnityStateProbe CreateSamplerState(anisotropic_wrap)", hr);
  if (FAILED(hr))
    pass = false;

  if (blend_state) {
    const FLOAT factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->OMSetBlendState(blend_state, factor, 0xffffffffu);
    printf("UnityStateProbe OMSetBlendState=PASS\n");
  }
  if (raster_state) {
    ctx->RSSetState(raster_state);
    printf("UnityStateProbe RSSetState=PASS\n");
  }
  if (depth_state) {
    ctx->OMSetDepthStencilState(depth_state, 0);
    printf("UnityStateProbe OMSetDepthStencilState=PASS\n");
  }
  if (sampler_state) {
    ctx->PSSetSamplers(0, 1, &sampler_state);
    printf("UnityStateProbe PSSetSamplers=PASS\n");
  }
  if (opaque_blend_state) {
    const FLOAT factor[4] = {0.125f, 0.25f, 0.5f, 1.0f};
    ctx->OMSetBlendState(opaque_blend_state, factor, 0x0f0f0f0fu);
    printf("UnityStateProbe OMSetBlendState(opaque)=PASS\n");
    ID3D11BlendState *current_blend = nullptr;
    FLOAT current_factor[4] = {};
    UINT current_mask = 0;
    ctx->OMGetBlendState(&current_blend, current_factor, &current_mask);
    printf("UnityStateProbe OMGetBlendState sample_mask=0x%08x factor0=%.3f\n",
           current_mask, current_factor[0]);
    if (current_blend != opaque_blend_state || current_mask != 0x0f0f0f0fu)
      pass = false;
    if (current_blend)
      current_blend->Release();
  }
  if (independent_blend_state) {
    const FLOAT factor[4] = {1.0f, 0.5f, 0.25f, 0.125f};
    ctx->OMSetBlendState(independent_blend_state, factor, 0xf0f0f0f1u);
    printf("UnityStateProbe OMSetBlendState(independent)=PASS\n");
    ID3D11BlendState *current_blend = nullptr;
    FLOAT current_factor[4] = {};
    UINT current_mask = 0;
    ctx->OMGetBlendState(&current_blend, current_factor, &current_mask);
    printf("UnityStateProbe OMGetBlendState(independent) sample_mask=0x%08x factor0=%.3f\n",
           current_mask, current_factor[0]);
    if (current_blend != independent_blend_state ||
        current_mask != 0xf0f0f0f1u)
      pass = false;
    if (current_blend)
      current_blend->Release();
  }
  if (back_raster_state) {
    ctx->RSSetState(back_raster_state);
    printf("UnityStateProbe RSSetState(cull_back)=PASS\n");
    ID3D11RasterizerState *current_raster = nullptr;
    ctx->RSGetState(&current_raster);
    printf("UnityStateProbe RSGetState=%p\n", current_raster);
    if (current_raster != back_raster_state)
      pass = false;
    if (current_raster)
      current_raster->Release();
  }
  if (wire_raster_state) {
    ctx->RSSetState(wire_raster_state);
    printf("UnityStateProbe RSSetState(wire_front_no_depthclip)=PASS\n");
    ID3D11RasterizerState *current_raster = nullptr;
    ctx->RSGetState(&current_raster);
    printf("UnityStateProbe RSGetState(wire_front_no_depthclip)=%p\n",
           current_raster);
    if (current_raster != wire_raster_state)
      pass = false;
    if (current_raster)
      current_raster->Release();
  }
  if (disabled_depth_state) {
    ctx->OMSetDepthStencilState(disabled_depth_state, 3);
    printf("UnityStateProbe OMSetDepthStencilState(disabled)=PASS\n");
    ID3D11DepthStencilState *current_depth = nullptr;
    UINT stencil_ref = 0;
    ctx->OMGetDepthStencilState(&current_depth, &stencil_ref);
    printf("UnityStateProbe OMGetDepthStencilState stencil_ref=%u\n",
           stencil_ref);
    if (current_depth != disabled_depth_state || stencil_ref != 3)
      pass = false;
    if (current_depth)
      current_depth->Release();
  }
  if (comparison_sampler_state && anisotropic_sampler_state) {
    ID3D11SamplerState *samplers[2] = {comparison_sampler_state,
                                       anisotropic_sampler_state};
    ctx->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);
    printf("UnityStateProbe PSSetSamplers(variant_pair)=PASS\n");
    ID3D11SamplerState *current_samplers[2] = {};
    ctx->PSGetSamplers(0, ARRAYSIZE(current_samplers), current_samplers);
    printf("UnityStateProbe PSGetSamplers pair=%p,%p\n",
           current_samplers[0], current_samplers[1]);
    if (current_samplers[0] != comparison_sampler_state ||
        current_samplers[1] != anisotropic_sampler_state)
      pass = false;
    for (UINT i = 0; i < ARRAYSIZE(current_samplers); i++) {
      if (current_samplers[i])
        current_samplers[i]->Release();
    }
  }
  D3D11_VIEWPORT state_viewport = {0.0f, 0.0f, 32.0f, 32.0f, 0.0f, 1.0f};
  ctx->RSSetViewports(1, &state_viewport);
  D3D11_VIEWPORT current_viewport = {};
  UINT viewport_count = 1;
  ctx->RSGetViewports(&viewport_count, &current_viewport);
  printf("UnityStateProbe RSGetViewports count=%u width=%.1f height=%.1f\n",
         viewport_count, current_viewport.Width, current_viewport.Height);
  if (viewport_count != 1 || current_viewport.Width != 32.0f ||
      current_viewport.Height != 32.0f)
    pass = false;

  D3D11_VIEWPORT multi_viewports[2] = {
      {0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f},
      {16.0f, 8.0f, 24.0f, 20.0f, 0.25f, 0.75f},
  };
  ctx->RSSetViewports(ARRAYSIZE(multi_viewports), multi_viewports);
  D3D11_VIEWPORT current_viewports[2] = {};
  viewport_count = ARRAYSIZE(current_viewports);
  ctx->RSGetViewports(&viewport_count, current_viewports);
  printf("UnityStateProbe RSGetViewports(multi) count=%u widths=%.1f,%.1f\n",
         viewport_count, current_viewports[0].Width,
         current_viewports[1].Width);
  if (viewport_count != ARRAYSIZE(multi_viewports) ||
      current_viewports[0].Width != multi_viewports[0].Width ||
      current_viewports[1].TopLeftX != multi_viewports[1].TopLeftX ||
      current_viewports[1].Width != multi_viewports[1].Width ||
      current_viewports[1].MinDepth != multi_viewports[1].MinDepth ||
      current_viewports[1].MaxDepth != multi_viewports[1].MaxDepth)
    pass = false;

  D3D11_RECT state_scissor = {1, 2, 31, 30};
  ctx->RSSetScissorRects(1, &state_scissor);
  D3D11_RECT current_scissor = {};
  UINT scissor_count = 1;
  ctx->RSGetScissorRects(&scissor_count, &current_scissor);
  printf("UnityStateProbe RSGetScissorRects count=%u rect=%ld,%ld,%ld,%ld\n",
         scissor_count, current_scissor.left, current_scissor.top,
         current_scissor.right, current_scissor.bottom);
  if (scissor_count != 1 || current_scissor.left != state_scissor.left ||
      current_scissor.top != state_scissor.top ||
      current_scissor.right != state_scissor.right ||
      current_scissor.bottom != state_scissor.bottom)
    pass = false;

  D3D11_RECT multi_scissors[2] = {{0, 0, 16, 16}, {16, 8, 40, 28}};
  ctx->RSSetScissorRects(ARRAYSIZE(multi_scissors), multi_scissors);
  D3D11_RECT current_scissors[2] = {};
  scissor_count = ARRAYSIZE(current_scissors);
  ctx->RSGetScissorRects(&scissor_count, current_scissors);
  printf("UnityStateProbe RSGetScissorRects(multi) count=%u rect1=%ld,%ld,%ld,%ld\n",
         scissor_count, current_scissors[1].left, current_scissors[1].top,
         current_scissors[1].right, current_scissors[1].bottom);
  if (scissor_count != ARRAYSIZE(multi_scissors) ||
      current_scissors[0].right != multi_scissors[0].right ||
      current_scissors[1].left != multi_scissors[1].left ||
      current_scissors[1].top != multi_scissors[1].top ||
      current_scissors[1].right != multi_scissors[1].right ||
      current_scissors[1].bottom != multi_scissors[1].bottom)
    pass = false;

  const FLOAT reset_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  ctx->OMSetBlendState(nullptr, reset_factor, 0xffffffffu);
  ctx->RSSetState(nullptr);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ID3D11SamplerState *null_samplers[2] = {};
  ctx->PSSetSamplers(0, ARRAYSIZE(null_samplers), null_samplers);
  printf("UnityStateProbe state_reset=PASS\n");

  if (sampler_state)
    sampler_state->Release();
  if (anisotropic_sampler_state)
    anisotropic_sampler_state->Release();
  if (comparison_sampler_state)
    comparison_sampler_state->Release();
  if (disabled_depth_state)
    disabled_depth_state->Release();
  if (back_raster_state)
    back_raster_state->Release();
  if (wire_raster_state)
    wire_raster_state->Release();
  if (opaque_blend_state)
    opaque_blend_state->Release();
  if (independent_blend_state)
    independent_blend_state->Release();
  if (depth_state)
    depth_state->Release();
  if (raster_state)
    raster_state->Release();
  if (blend_state)
    blend_state->Release();

  printf("UnityStateProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

struct DrawVertex {
  float position[2];
  float color[4];
};

struct DrawInstance {
  float offset[2];
};

static bool probe_unity_multi_render_targets(ID3D11Device *device,
                                             ID3D11DeviceContext *ctx) {
  bool pass = true;
  static const char shader_source[] =
      "struct VSIn { float2 pos : POSITION; }; \n"
      "struct PSIn { float4 pos : SV_POSITION; }; \n"
      "struct PSOut { float4 rt0 : SV_Target0; float4 rt1 : SV_Target1; }; \n"
      "PSIn vs_main(VSIn input) { PSIn output; output.pos = float4(input.pos, 0, 1); return output; }\n"
      "PSOut ps_main(PSIn input) { PSOut output; output.rt0 = float4(1, 0, 0, 1); output.rt1 = float4(0, 0, 1, 1); return output; }\n";

  HMODULE compiler = load_local_dll(L"d3dcompiler_47.dll", "d3dcompiler_47");
  if (!compiler)
    return false;

  auto compile = (D3DCompileProc)GetProcAddress(compiler, "D3DCompile");
  printf("UnityMRTProbe GetProcAddress(D3DCompile) proc=%p gle=%lu\n", compile,
         GetLastError());
  if (!compile)
    return false;

  ID3DBlob *vs_blob = nullptr;
  ID3DBlob *ps_blob = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT hr = compile(shader_source, sizeof(shader_source) - 1,
                       "dxmt_headless_mrt", nullptr, nullptr, "vs_main",
                       "vs_5_0", 0, 0, &vs_blob, &errors);
  print_hr("UnityMRTProbe D3DCompile(vs_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityMRTProbe vs_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    return false;
  }
  if (errors) {
    errors->Release();
    errors = nullptr;
  }

  hr = compile(shader_source, sizeof(shader_source) - 1,
               "dxmt_headless_mrt", nullptr, nullptr, "ps_main", "ps_5_0", 0,
               0, &ps_blob, &errors);
  print_hr("UnityMRTProbe D3DCompile(ps_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityMRTProbe ps_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (vs_blob)
      vs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  ID3D11VertexShader *vs = nullptr;
  hr = device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                  vs_blob->GetBufferSize(), nullptr, &vs);
  print_hr("UnityMRTProbe CreateVertexShader", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11PixelShader *ps = nullptr;
  hr = device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                 ps_blob->GetBufferSize(), nullptr, &ps);
  print_hr("UnityMRTProbe CreatePixelShader", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_INPUT_ELEMENT_DESC input_desc[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  ID3D11InputLayout *layout = nullptr;
  hr = device->CreateInputLayout(input_desc, ARRAYSIZE(input_desc),
                                 vs_blob->GetBufferPointer(),
                                 vs_blob->GetBufferSize(), &layout);
  print_hr("UnityMRTProbe CreateInputLayout", hr);
  if (FAILED(hr))
    pass = false;

  const DrawVertex vertices[] = {
      {{-1.0f, -1.0f}, {0, 0, 0, 1}},
      {{-1.0f, 3.0f}, {0, 0, 0, 1}},
      {{3.0f, -1.0f}, {0, 0, 0, 1}},
  };
  D3D11_SUBRESOURCE_DATA vertex_data = {};
  vertex_data.pSysMem = vertices;
  D3D11_BUFFER_DESC vertex_desc = {};
  vertex_desc.ByteWidth = sizeof(vertices);
  vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
  vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  ID3D11Buffer *vertex_buffer = nullptr;
  hr = device->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer);
  print_hr("UnityMRTProbe CreateBuffer(vertex)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_TEXTURE2D_DESC target_desc = {};
  target_desc.Width = 4;
  target_desc.Height = 4;
  target_desc.MipLevels = 1;
  target_desc.ArraySize = 1;
  target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target_desc.SampleDesc.Count = 1;
  target_desc.Usage = D3D11_USAGE_DEFAULT;
  target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  ID3D11Texture2D *targets[2] = {};
  ID3D11RenderTargetView *rtvs[2] = {};
  for (UINT i = 0; i < ARRAYSIZE(targets); i++) {
    hr = device->CreateTexture2D(&target_desc, nullptr, &targets[i]);
    printf("UnityMRTProbe CreateTexture2D(target%u) hr=0x%08lx\n", i,
           (unsigned long)(uint32_t)hr);
    if (FAILED(hr)) {
      pass = false;
      continue;
    }
    hr = device->CreateRenderTargetView(targets[i], nullptr, &rtvs[i]);
    printf("UnityMRTProbe CreateRenderTargetView(target%u) hr=0x%08lx\n", i,
           (unsigned long)(uint32_t)hr);
    if (FAILED(hr))
      pass = false;
  }

  if (pass && vs && ps && layout && vertex_buffer && rtvs[0] && rtvs[1]) {
    const FLOAT black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(rtvs[0], black);
    ctx->ClearRenderTargetView(rtvs[1], black);
    ctx->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);
    printf("UnityMRTProbe OMSetRenderTargets(2)=PASS\n");
    D3D11_VIEWPORT viewport = {0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &viewport);
    UINT stride = sizeof(DrawVertex);
    UINT offset = 0;
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);
    printf("UnityMRTProbe Draw(3)=PASS\n");
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    uint8_t rt0[4] = {};
    uint8_t rt1[4] = {};
    HRESULT rt0_hr = read_first_pixel(device, ctx, targets[0], rt0);
    HRESULT rt1_hr = read_first_pixel(device, ctx, targets[1], rt1);
    print_hr("UnityMRTProbe Readback(target0)", rt0_hr);
    print_hr("UnityMRTProbe Readback(target1)", rt1_hr);
    printf("UnityMRTProbe target0_rgba=%u,%u,%u,%u\n", rt0[0], rt0[1],
           rt0[2], rt0[3]);
    printf("UnityMRTProbe target1_rgba=%u,%u,%u,%u\n", rt1[0], rt1[1],
           rt1[2], rt1[3]);
    if (FAILED(rt0_hr) || rt0[0] < 235 || rt0[1] > 2 || rt0[2] > 2 ||
        rt0[3] != 255)
      pass = false;
    if (FAILED(rt1_hr) || rt1[0] > 2 || rt1[1] > 2 || rt1[2] < 235 ||
        rt1[3] != 255)
      pass = false;
  }

  for (UINT i = 0; i < ARRAYSIZE(rtvs); i++) {
    if (rtvs[i])
      rtvs[i]->Release();
    if (targets[i])
      targets[i]->Release();
  }
  if (vertex_buffer)
    vertex_buffer->Release();
  if (layout)
    layout->Release();
  if (ps)
    ps->Release();
  if (vs)
    vs->Release();
  if (ps_blob)
    ps_blob->Release();
  if (vs_blob)
    vs_blob->Release();

  printf("UnityMRTProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

struct DeferredThreadJob {
  ID3D11Device *device;
  ID3D11RenderTargetView *rtv;
  FLOAT color[4];
  ID3D11CommandList *command_list;
  HRESULT create_hr;
  HRESULT finish_hr;
  BOOL recorded_clear;
};

static DWORD WINAPI deferred_clear_thread_proc(void *param) {
  DeferredThreadJob *job = (DeferredThreadJob *)param;
  ID3D11DeviceContext *deferred = nullptr;
  job->create_hr = job->device->CreateDeferredContext(0, &deferred);
  if (FAILED(job->create_hr) || !deferred)
    return 1;

  deferred->ClearRenderTargetView(job->rtv, job->color);
  job->recorded_clear = TRUE;
  job->finish_hr = deferred->FinishCommandList(FALSE, &job->command_list);
  deferred->Release();
  return (SUCCEEDED(job->finish_hr) && job->command_list) ? 0 : 2;
}

static bool probe_unity_multithreaded_deferred(ID3D11Device *device,
                                               ID3D11DeviceContext *ctx) {
  bool pass = true;

  ID3D10Multithread *multithread = nullptr;
  HRESULT hr =
      ctx->QueryInterface(__uuidof(ID3D10Multithread), (void **)&multithread);
  print_hr("UnityMultithreadProbe QueryInterface(ID3D10Multithread)", hr);
  if (FAILED(hr) || !multithread) {
    pass = false;
  }

  BOOL previous_mt = FALSE;
  if (multithread) {
    previous_mt = multithread->SetMultithreadProtected(TRUE);
    BOOL enabled = multithread->GetMultithreadProtected();
    printf("UnityMultithreadProbe SetMultithreadProtected previous=%u enabled=%u\n",
           (unsigned)previous_mt, (unsigned)enabled);
    multithread->Enter();
    multithread->Leave();
    printf("UnityMultithreadProbe EnterLeave=PASS\n");
    if (!enabled)
      pass = false;
  }

  D3D11_TEXTURE2D_DESC target_desc = {};
  target_desc.Width = 4;
  target_desc.Height = 4;
  target_desc.MipLevels = 1;
  target_desc.ArraySize = 1;
  target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target_desc.SampleDesc.Count = 1;
  target_desc.Usage = D3D11_USAGE_DEFAULT;
  target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  ID3D11Texture2D *targets[2] = {};
  ID3D11RenderTargetView *rtvs[2] = {};
  for (UINT i = 0; i < ARRAYSIZE(targets); i++) {
    hr = device->CreateTexture2D(&target_desc, nullptr, &targets[i]);
    printf("UnityMultithreadProbe CreateTexture2D(target%u) hr=0x%08lx\n", i,
           (unsigned long)(uint32_t)hr);
    if (FAILED(hr)) {
      pass = false;
      continue;
    }
    hr = device->CreateRenderTargetView(targets[i], nullptr, &rtvs[i]);
    printf("UnityMultithreadProbe CreateRenderTargetView(target%u) hr=0x%08lx\n",
           i, (unsigned long)(uint32_t)hr);
    if (FAILED(hr))
      pass = false;
  }

  DeferredThreadJob jobs[2] = {};
  jobs[0].device = device;
  jobs[0].rtv = rtvs[0];
  jobs[0].color[0] = 1.0f;
  jobs[0].color[3] = 1.0f;
  jobs[1].device = device;
  jobs[1].rtv = rtvs[1];
  jobs[1].color[1] = 1.0f;
  jobs[1].color[3] = 1.0f;

  HANDLE threads[2] = {};
  if (pass && rtvs[0] && rtvs[1]) {
    for (UINT i = 0; i < ARRAYSIZE(threads); i++) {
      threads[i] = CreateThread(nullptr, 0, deferred_clear_thread_proc,
                                &jobs[i], 0, nullptr);
      printf("UnityMultithreadProbe CreateThread%u handle=%p gle=%lu\n", i,
             threads[i], GetLastError());
      if (!threads[i])
        pass = false;
    }
  }

  if (threads[0] && threads[1]) {
    DWORD wait = WaitForMultipleObjects(ARRAYSIZE(threads), threads, TRUE,
                                        30000);
    printf("UnityMultithreadProbe WaitForMultipleObjects result=0x%08lx\n",
           (unsigned long)wait);
    if (wait != WAIT_OBJECT_0)
      pass = false;
    for (UINT i = 0; i < ARRAYSIZE(threads); i++) {
      DWORD exit_code = 0xffffffffu;
      GetExitCodeThread(threads[i], &exit_code);
      printf("UnityMultithreadProbe thread%u exit=%lu create_hr=0x%08lx finish_hr=0x%08lx recorded=%u\n",
             i, (unsigned long)exit_code,
             (unsigned long)(uint32_t)jobs[i].create_hr,
             (unsigned long)(uint32_t)jobs[i].finish_hr,
             (unsigned)jobs[i].recorded_clear);
      if (exit_code != 0 || FAILED(jobs[i].create_hr) ||
          FAILED(jobs[i].finish_hr) || !jobs[i].recorded_clear)
        pass = false;
    }
  } else if (rtvs[0] && rtvs[1]) {
    pass = false;
  }

  for (UINT i = 0; i < ARRAYSIZE(jobs); i++) {
    if (jobs[i].command_list) {
      ctx->ExecuteCommandList(jobs[i].command_list, FALSE);
      printf("UnityMultithreadProbe ExecuteCommandList%u=PASS\n", i);
    }
  }

  if (targets[0] && targets[1]) {
    uint8_t pixels[2][4] = {};
    HRESULT read0 = read_first_pixel(device, ctx, targets[0], pixels[0]);
    HRESULT read1 = read_first_pixel(device, ctx, targets[1], pixels[1]);
    print_hr("UnityMultithreadProbe Readback(target0)", read0);
    print_hr("UnityMultithreadProbe Readback(target1)", read1);
    printf("UnityMultithreadProbe target0_rgba=%u,%u,%u,%u\n", pixels[0][0],
           pixels[0][1], pixels[0][2], pixels[0][3]);
    printf("UnityMultithreadProbe target1_rgba=%u,%u,%u,%u\n", pixels[1][0],
           pixels[1][1], pixels[1][2], pixels[1][3]);
    if (FAILED(read0) || pixels[0][0] < 235 || pixels[0][1] > 2 ||
        pixels[0][2] > 2 || pixels[0][3] != 255)
      pass = false;
    if (FAILED(read1) || pixels[1][0] > 2 || pixels[1][1] < 235 ||
        pixels[1][2] > 2 || pixels[1][3] != 255)
      pass = false;
  }

  for (UINT i = 0; i < ARRAYSIZE(threads); i++) {
    if (threads[i])
      CloseHandle(threads[i]);
    if (jobs[i].command_list)
      jobs[i].command_list->Release();
    if (rtvs[i])
      rtvs[i]->Release();
    if (targets[i])
      targets[i]->Release();
  }
  if (multithread) {
    multithread->SetMultithreadProtected(previous_mt);
    multithread->Release();
  }

  printf("UnityMultithreadProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_command_batch_stability(ID3D11Device *device,
                                                ID3D11DeviceContext *ctx) {
  bool pass = true;

  D3D11_TEXTURE2D_DESC target_desc = {};
  target_desc.Width = 4;
  target_desc.Height = 4;
  target_desc.MipLevels = 1;
  target_desc.ArraySize = 1;
  target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target_desc.SampleDesc.Count = 1;
  target_desc.Usage = D3D11_USAGE_DEFAULT;
  target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  ID3D11Texture2D *target = nullptr;
  HRESULT hr = device->CreateTexture2D(&target_desc, nullptr, &target);
  print_hr("UnityBatchProbe CreateTexture2D(target)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11RenderTargetView *rtv = nullptr;
  if (target) {
    hr = device->CreateRenderTargetView(target, nullptr, &rtv);
    print_hr("UnityBatchProbe CreateRenderTargetView", hr);
    if (FAILED(hr))
      pass = false;
  }

  static const FLOAT colors[][4] = {
      {1.0f, 0.0f, 0.0f, 1.0f},
      {0.0f, 1.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, 1.0f, 1.0f},
      {1.0f, 1.0f, 0.0f, 1.0f},
  };
  const UINT iterations = 128;
  ULONGLONG start_ms = GetTickCount64();
  if (rtv) {
    for (UINT i = 0; i < iterations; i++) {
      ctx->OMSetRenderTargets(1, &rtv, nullptr);
      ctx->ClearRenderTargetView(rtv, colors[i % ARRAYSIZE(colors)]);
    }
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->Flush();
    printf("UnityBatchProbe queued_clears=%u elapsed_ms=%llu\n", iterations,
           (unsigned long long)(GetTickCount64() - start_ms));
  }

  if (target) {
    uint8_t pixel[4] = {};
    hr = read_first_pixel(device, ctx, target, pixel);
    print_hr("UnityBatchProbe Readback", hr);
    printf("UnityBatchProbe final_rgba=%u,%u,%u,%u\n", pixel[0], pixel[1],
           pixel[2], pixel[3]);
    if (FAILED(hr) || pixel[0] < 235 || pixel[1] < 235 || pixel[2] > 2 ||
        pixel[3] != 255)
      pass = false;
  }

  if (rtv)
    rtv->Release();
  if (target)
    target->Release();

  printf("UnityBatchProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_draw(ID3D11Device *device, ID3D11DeviceContext *ctx,
                             ID3D11RenderTargetView *rtv) {
  bool pass = true;
  const bool immediate_context =
      ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
  static const char shader_source[] =
      "Texture2D g_tex : register(t0);\n"
      "SamplerState g_sampler : register(s0);\n"
      "struct VSIn { float2 pos : POSITION; float4 color : COLOR; float2 instance_offset : INSTANCE_OFFSET; };\n"
      "struct PSIn { float4 pos : SV_POSITION; float4 color : COLOR; float2 uv : TEXCOORD0; };\n"
      "PSIn vs_main(VSIn input) {\n"
      "  PSIn output;\n"
      "  output.pos = float4(input.pos + input.instance_offset, 0.0, 1.0);\n"
      "  output.color = input.color;\n"
      "  output.uv = input.pos * 0.5 + 0.5;\n"
      "  return output;\n"
      "}\n"
      "float4 ps_main(PSIn input) : SV_TARGET { return input.color; }\n"
      "float4 ps_texture_main(PSIn input) : SV_TARGET { return g_tex.Sample(g_sampler, input.uv); }\n"
      "[maxvertexcount(3)]\n"
      "void gs_main(triangle PSIn input[3], inout TriangleStream<PSIn> stream) {\n"
      "  stream.Append(input[0]);\n"
      "  stream.Append(input[1]);\n"
      "  stream.Append(input[2]);\n"
      "}\n";

  HMODULE compiler = load_local_dll(L"d3dcompiler_47.dll", "d3dcompiler_47");
  if (!compiler)
    return false;

  auto compile = (D3DCompileProc)GetProcAddress(compiler, "D3DCompile");
  printf("GetProcAddress(D3DCompile) proc=%p gle=%lu\n", compile,
         GetLastError());
  if (!compile)
    return false;

  ID3DBlob *vs_blob = nullptr;
  ID3DBlob *ps_blob = nullptr;
  ID3DBlob *ps_texture_blob = nullptr;
  ID3DBlob *gs_blob = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT hr = compile(shader_source, sizeof(shader_source) - 1,
                       "dxmt_headless_unity_draw", nullptr, nullptr, "vs_main",
                       "vs_5_0", 0, 0, &vs_blob, &errors);
  print_hr("UnityDrawProbe D3DCompile(vs_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityDrawProbe vs_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    return false;
  }
  if (errors) {
    errors->Release();
    errors = nullptr;
  }

  hr = compile(shader_source, sizeof(shader_source) - 1,
               "dxmt_headless_unity_draw", nullptr, nullptr, "ps_main",
               "ps_5_0", 0, 0, &ps_blob, &errors);
  print_hr("UnityDrawProbe D3DCompile(ps_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityDrawProbe ps_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (vs_blob)
      vs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  hr = compile(shader_source, sizeof(shader_source) - 1,
               "dxmt_headless_unity_draw", nullptr, nullptr, "ps_texture_main",
               "ps_5_0", 0, 0, &ps_texture_blob, &errors);
  print_hr("UnityDrawProbe D3DCompile(ps_texture_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityDrawProbe ps_texture_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (vs_blob)
      vs_blob->Release();
    if (ps_blob)
      ps_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  hr = compile(shader_source, sizeof(shader_source) - 1,
               "dxmt_headless_unity_draw", nullptr, nullptr, "gs_main",
               "gs_5_0", 0, 0, &gs_blob, &errors);
  print_hr("UnityDrawProbe D3DCompile(gs_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityDrawProbe gs_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (vs_blob)
      vs_blob->Release();
    if (ps_blob)
      ps_blob->Release();
    if (ps_texture_blob)
      ps_texture_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  ID3D11VertexShader *vs = nullptr;
  hr = device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                  vs_blob->GetBufferSize(), nullptr, &vs);
  print_hr("UnityDrawProbe CreateVertexShader", hr);
  if (FAILED(hr)) {
    vs_blob->Release();
    ps_blob->Release();
    return false;
  }

  ID3D11PixelShader *ps = nullptr;
  hr = device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                 ps_blob->GetBufferSize(), nullptr, &ps);
  print_hr("UnityDrawProbe CreatePixelShader", hr);
  if (FAILED(hr)) {
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  ID3D11PixelShader *ps_texture = nullptr;
  hr = device->CreatePixelShader(ps_texture_blob->GetBufferPointer(),
                                 ps_texture_blob->GetBufferSize(), nullptr,
                                 &ps_texture);
  print_hr("UnityDrawProbe CreatePixelShader(texture)", hr);
  if (FAILED(hr)) {
    ps->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  ID3D11GeometryShader *gs = nullptr;
  hr = device->CreateGeometryShader(gs_blob->GetBufferPointer(),
                                    gs_blob->GetBufferSize(), nullptr, &gs);
  print_hr("UnityDrawProbe CreateGeometryShader", hr);
  if (FAILED(hr)) {
    ps_texture->Release();
    ps->Release();
    vs->Release();
    gs_blob->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  const uint8_t *gs_bytecode =
      (const uint8_t *)gs_blob->GetBufferPointer();
  printf("UnityDrawProbe gs_bytecode_magic=%02x%02x%02x%02x "
         "so_no_rasterized=0x%08x\n",
         gs_bytecode[0], gs_bytecode[1], gs_bytecode[2], gs_bytecode[3],
         (unsigned)D3D11_SO_NO_RASTERIZED_STREAM);

  D3D11_SO_DECLARATION_ENTRY so_decl[] = {
      {0, "SV_POSITION", 0, 0, 4, 0},
      {0, "COLOR", 0, 0, 4, 0},
      {0, "TEXCOORD", 0, 0, 2, 0},
  };
  UINT so_strides[] = {10 * sizeof(float)};
  ID3D11GeometryShader *gs_so = nullptr;
  hr = device->CreateGeometryShaderWithStreamOutput(
      gs_blob->GetBufferPointer(), gs_blob->GetBufferSize(), so_decl,
      ARRAYSIZE(so_decl), so_strides, ARRAYSIZE(so_strides),
      D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs_so);
  print_hr("UnityDrawProbe CreateGeometryShaderWithStreamOutput", hr);
  if (FAILED(hr)) {
    gs->Release();
    ps_texture->Release();
    ps->Release();
    vs->Release();
    gs_blob->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  D3D11_INPUT_ELEMENT_DESC input_desc[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"INSTANCE_OFFSET", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0,
       D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  ID3D11InputLayout *layout = nullptr;
  hr = device->CreateInputLayout(input_desc, ARRAYSIZE(input_desc),
                                 vs_blob->GetBufferPointer(),
                                 vs_blob->GetBufferSize(), &layout);
  print_hr("UnityDrawProbe CreateInputLayout", hr);
  if (FAILED(hr)) {
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  const DrawVertex vertices[] = {
      {{-1.0f, -1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
      {{-1.0f, 3.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
      {{3.0f, -1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
  };
  D3D11_BUFFER_DESC vb_desc = {};
  vb_desc.ByteWidth = sizeof(vertices);
  vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
  vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA vb_data = {};
  vb_data.pSysMem = vertices;
  ID3D11Buffer *vertex_buffer = nullptr;
  hr = device->CreateBuffer(&vb_desc, &vb_data, &vertex_buffer);
  print_hr("UnityDrawProbe CreateBuffer(draw_vertex)", hr);
  if (FAILED(hr)) {
    layout->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  const uint16_t indices[] = {0, 1, 2};
  D3D11_BUFFER_DESC ib_desc = {};
  ib_desc.ByteWidth = sizeof(indices);
  ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
  ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  D3D11_SUBRESOURCE_DATA ib_data = {};
  ib_data.pSysMem = indices;
  ID3D11Buffer *index_buffer = nullptr;
  hr = device->CreateBuffer(&ib_desc, &ib_data, &index_buffer);
  print_hr("UnityDrawProbe CreateBuffer(draw_index)", hr);
  if (FAILED(hr)) {
    vertex_buffer->Release();
    layout->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  const DrawInstance instances[] = {
      {{0.0f, 0.0f}},
      {{2.5f, 0.0f}},
  };
  D3D11_BUFFER_DESC instance_desc = {};
  instance_desc.ByteWidth = sizeof(instances);
  instance_desc.Usage = D3D11_USAGE_IMMUTABLE;
  instance_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA instance_data = {};
  instance_data.pSysMem = instances;
  ID3D11Buffer *instance_buffer = nullptr;
  hr = device->CreateBuffer(&instance_desc, &instance_data, &instance_buffer);
  print_hr("UnityDrawProbe CreateBuffer(instance_data)", hr);
  if (FAILED(hr)) {
    index_buffer->Release();
    vertex_buffer->Release();
    layout->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  uint32_t sample_texel = 0xff00ff00u;
  D3D11_TEXTURE2D_DESC sample_desc = {};
  sample_desc.Width = 1;
  sample_desc.Height = 1;
  sample_desc.MipLevels = 1;
  sample_desc.ArraySize = 1;
  sample_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sample_desc.SampleDesc.Count = 1;
  sample_desc.Usage = D3D11_USAGE_IMMUTABLE;
  sample_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sample_data = {};
  sample_data.pSysMem = &sample_texel;
  sample_data.SysMemPitch = sizeof(sample_texel);
  ID3D11Texture2D *sample_texture = nullptr;
  hr = device->CreateTexture2D(&sample_desc, &sample_data, &sample_texture);
  print_hr("UnityDrawProbe CreateTexture2D(sample)", hr);
  if (FAILED(hr)) {
    instance_buffer->Release();
    index_buffer->Release();
    vertex_buffer->Release();
    layout->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  ID3D11ShaderResourceView *sample_srv = nullptr;
  hr = device->CreateShaderResourceView(sample_texture, nullptr, &sample_srv);
  print_hr("UnityDrawProbe CreateShaderResourceView(sample)", hr);
  if (FAILED(hr)) {
    sample_texture->Release();
    instance_buffer->Release();
    index_buffer->Release();
    vertex_buffer->Release();
    layout->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  D3D11_SAMPLER_DESC sampler_desc = {};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState *sample_sampler = nullptr;
  hr = device->CreateSamplerState(&sampler_desc, &sample_sampler);
  print_hr("UnityDrawProbe CreateSamplerState(sample_point_clamp)", hr);
  if (FAILED(hr)) {
    sample_srv->Release();
    sample_texture->Release();
    instance_buffer->Release();
    index_buffer->Release();
    vertex_buffer->Release();
    layout->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  const UINT draw_indirect_args[] = {3, 1, 0, 0};
  D3D11_BUFFER_DESC indirect_desc = {};
  indirect_desc.ByteWidth = sizeof(draw_indirect_args);
  indirect_desc.Usage = D3D11_USAGE_IMMUTABLE;
  indirect_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  D3D11_SUBRESOURCE_DATA draw_indirect_data = {};
  draw_indirect_data.pSysMem = draw_indirect_args;
  ID3D11Buffer *draw_indirect_buffer = nullptr;
  hr = device->CreateBuffer(&indirect_desc, &draw_indirect_data,
                            &draw_indirect_buffer);
  print_hr("UnityDrawProbe CreateBuffer(draw_indirect_args)", hr);
  if (FAILED(hr)) {
    sample_sampler->Release();
    sample_srv->Release();
    sample_texture->Release();
    instance_buffer->Release();
    index_buffer->Release();
    vertex_buffer->Release();
    layout->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  struct IndexedIndirectArgs {
    UINT index_count_per_instance;
    UINT instance_count;
    UINT start_index_location;
    INT base_vertex_location;
    UINT start_instance_location;
  };
  const IndexedIndirectArgs indexed_indirect_args = {3, 1, 0, 0, 0};
  D3D11_BUFFER_DESC indexed_indirect_desc = {};
  indexed_indirect_desc.ByteWidth = sizeof(indexed_indirect_args);
  indexed_indirect_desc.Usage = D3D11_USAGE_IMMUTABLE;
  indexed_indirect_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  D3D11_SUBRESOURCE_DATA indexed_indirect_data = {};
  indexed_indirect_data.pSysMem = &indexed_indirect_args;
  ID3D11Buffer *indexed_indirect_buffer = nullptr;
  hr = device->CreateBuffer(&indexed_indirect_desc, &indexed_indirect_data,
                            &indexed_indirect_buffer);
  print_hr("UnityDrawProbe CreateBuffer(indexed_indirect_args)", hr);
  if (FAILED(hr)) {
    draw_indirect_buffer->Release();
    sample_sampler->Release();
    sample_srv->Release();
    sample_texture->Release();
    instance_buffer->Release();
    index_buffer->Release();
    vertex_buffer->Release();
    layout->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  D3D11_BUFFER_DESC so_buffer_desc = {};
  so_buffer_desc.ByteWidth = so_strides[0] * 3;
  so_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  so_buffer_desc.BindFlags =
      D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_VERTEX_BUFFER;
  ID3D11Buffer *so_buffer = nullptr;
  hr = device->CreateBuffer(&so_buffer_desc, nullptr, &so_buffer);
  print_hr("UnityDrawProbe CreateBuffer(stream_output)", hr);
  if (FAILED(hr)) {
    indexed_indirect_buffer->Release();
    draw_indirect_buffer->Release();
    sample_sampler->Release();
    sample_srv->Release();
    sample_texture->Release();
    instance_buffer->Release();
    index_buffer->Release();
    vertex_buffer->Release();
    layout->Release();
    gs_so->Release();
    gs->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    gs_blob->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }
  D3D11_BUFFER_DESC so_staging_desc = so_buffer_desc;
  so_staging_desc.Usage = D3D11_USAGE_STAGING;
  so_staging_desc.BindFlags = 0;
  so_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *so_staging = nullptr;
  hr = device->CreateBuffer(&so_staging_desc, nullptr, &so_staging);
  print_hr("UnityDrawProbe CreateBuffer(stream_output_staging)", hr);
  if (FAILED(hr)) {
    so_buffer->Release();
    indexed_indirect_buffer->Release();
    draw_indirect_buffer->Release();
    sample_sampler->Release();
    sample_srv->Release();
    sample_texture->Release();
    instance_buffer->Release();
    index_buffer->Release();
    vertex_buffer->Release();
    layout->Release();
    gs_so->Release();
    gs->Release();
    ps->Release();
    ps_texture->Release();
    vs->Release();
    gs_blob->Release();
    vs_blob->Release();
    ps_blob->Release();
    ps_texture_blob->Release();
    return false;
  }

  D3D11_VIEWPORT viewport = {};
  viewport.Width = (FLOAT)kWidth;
  viewport.Height = (FLOAT)kHeight;
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  D3D11_RECT scissor = {0, 0, (LONG)kWidth, (LONG)kHeight};
  ID3D11Buffer *vertex_buffers[] = {vertex_buffer, instance_buffer};
  UINT strides[] = {sizeof(DrawVertex), sizeof(DrawInstance)};
  UINT offsets[] = {0, 0};
  ctx->RSSetViewports(1, &viewport);
  ctx->RSSetScissorRects(1, &scissor);
  printf("UnityDrawProbe RSSetScissorRects=PASS\n");
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetVertexBuffers(0, ARRAYSIZE(vertex_buffers), vertex_buffers, strides,
                          offsets);
  printf("UnityDrawProbe IASetVertexBuffers(instance_slot)=PASS\n");
  ctx->VSSetShader(vs, nullptr, 0);
  ctx->PSSetShader(ps, nullptr, 0);
  ctx->GSSetShader(gs, nullptr, 0);
  ctx->Draw(3, 0);
  printf("UnityDrawProbe GeometryShaderDraw=PASS\n");
  ctx->GSSetShader(nullptr, nullptr, 0);
  ID3D11Buffer *so_targets[] = {so_buffer};
  UINT so_offsets[] = {0};
  ctx->SOSetTargets(ARRAYSIZE(so_targets), so_targets, so_offsets);
  ID3D11Buffer *so_bound_target = nullptr;
  ctx->SOGetTargets(1, &so_bound_target);
  printf("UnityDrawProbe SOGetTargets(bound) target=%p\n", so_bound_target);
  if (so_bound_target != so_buffer)
    pass = false;
  if (so_bound_target)
    so_bound_target->Release();

  ctx->GSSetShader(gs_so, nullptr, 0);
  ctx->Draw(3, 0);
  printf("UnityDrawProbe StreamOutputDraw=PASS\n");
  ID3D11Buffer *null_so_targets[] = {nullptr};
  ctx->SOSetTargets(ARRAYSIZE(null_so_targets), null_so_targets, so_offsets);
  ID3D11Buffer *so_unbound_target = nullptr;
  ctx->SOGetTargets(1, &so_unbound_target);
  printf("UnityDrawProbe SOGetTargets(unbound) target=%p\n", so_unbound_target);
  if (so_unbound_target)
    pass = false;
  if (so_unbound_target)
    so_unbound_target->Release();
  ctx->GSSetShader(nullptr, nullptr, 0);
  if (immediate_context) {
    ctx->CopyResource(so_staging, so_buffer);
    D3D11_MAPPED_SUBRESOURCE so_mapped = {};
    hr = ctx->Map(so_staging, 0, D3D11_MAP_READ, 0, &so_mapped);
    print_hr("UnityDrawProbe Map(stream_output_staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t *words = (const uint32_t *)so_mapped.pData;
      bool wrote_stream_output = false;
      for (UINT i = 0; i < so_buffer_desc.ByteWidth / sizeof(uint32_t); i++)
        wrote_stream_output = wrote_stream_output || words[i] != 0;
      printf("UnityDrawProbe StreamOutputVerify=%s first_word=0x%08x\n",
             wrote_stream_output ? "PASS" : "FAIL", words[0]);
      if (!wrote_stream_output)
        pass = false;
      ctx->Unmap(so_staging, 0);
    } else {
      pass = false;
    }
  } else {
    printf("UnityDrawProbe StreamOutputVerify=SKIP deferred_context\n");
  }
  ctx->Draw(3, 0);
  printf("UnityDrawProbe Draw(3)=PASS\n");
  ctx->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R16_UINT, 0);
  ctx->DrawIndexed(3, 0, 0);
  printf("UnityDrawProbe DrawIndexed(3)=PASS\n");
  ctx->DrawInstanced(3, 1, 0, 0);
  printf("UnityDrawProbe DrawInstanced(3,1)=PASS\n");
  ctx->DrawIndexedInstanced(3, 2, 0, 0, 0);
  printf("UnityDrawProbe DrawIndexedInstanced(3,2)=PASS\n");
  ctx->DrawInstancedIndirect(draw_indirect_buffer, 0);
  printf("UnityDrawProbe DrawInstancedIndirect=PASS\n");
  ctx->DrawIndexedInstancedIndirect(indexed_indirect_buffer, 0);
  printf("UnityDrawProbe DrawIndexedInstancedIndirect=PASS\n");
  ctx->PSSetShaderResources(0, 1, &sample_srv);
  ctx->PSSetSamplers(0, 1, &sample_sampler);
  ctx->PSSetShader(ps_texture, nullptr, 0);
  ctx->Draw(3, 0);
  printf("UnityDrawProbe TextureSampleDraw=PASS\n");

  indexed_indirect_buffer->Release();
  draw_indirect_buffer->Release();
  so_staging->Release();
  so_buffer->Release();
  sample_sampler->Release();
  sample_srv->Release();
  sample_texture->Release();
  instance_buffer->Release();
  index_buffer->Release();
  vertex_buffer->Release();
  layout->Release();
  gs_so->Release();
  gs->Release();
  ps->Release();
  ps_texture->Release();
  vs->Release();
  gs_blob->Release();
  ps_blob->Release();
  ps_texture_blob->Release();
  vs_blob->Release();
  return pass;
}

static bool probe_unity_compute_dispatch(ID3D11Device *device,
                                         ID3D11DeviceContext *ctx) {
  static const char compute_source[] =
      "RWTexture2D<uint> g_out : register(u0);\n"
      "[numthreads(1, 1, 1)]\n"
      "void cs_main(uint3 tid : SV_DispatchThreadID) { g_out[tid.xy] = 42; }\n";
  static const char typed_buffer_compute_source[] =
      "RWBuffer<uint> g_out : register(u0);\n"
      "[numthreads(4, 1, 1)]\n"
      "void cs_typed_buffer_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  g_out[tid.x] = 100 + tid.x;\n"
      "}\n";
  static const char structured_buffer_compute_source[] =
      "RWStructuredBuffer<uint> g_out : register(u0);\n"
      "[numthreads(4, 1, 1)]\n"
      "void cs_structured_buffer_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  g_out[tid.x] = 200 + tid.x;\n"
      "}\n";
  static const char multi_uav_compute_source[] =
      "RWStructuredBuffer<uint> g_out0 : register(u0);\n"
      "RWStructuredBuffer<uint> g_out1 : register(u1);\n"
      "[numthreads(4, 1, 1)]\n"
      "void cs_multi_uav_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  g_out0[tid.x] = 400 + tid.x;\n"
      "  g_out1[tid.x] = 500 + tid.x;\n"
      "}\n";
  static const char srv_uav_compute_source[] =
      "StructuredBuffer<uint> g_in : register(t0);\n"
      "RWStructuredBuffer<uint> g_out : register(u0);\n"
      "[numthreads(4, 1, 1)]\n"
      "void cs_srv_uav_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  g_out[tid.x] = g_in[tid.x] + 17;\n"
      "}\n";
  static const char raw_buffer_compute_source[] =
      "ByteAddressBuffer g_in : register(t0);\n"
      "RWByteAddressBuffer g_out : register(u0);\n"
      "[numthreads(4, 1, 1)]\n"
      "void cs_raw_buffer_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint offset = tid.x << 2;\n"
      "  g_out.Store(offset, g_in.Load(offset) + 33);\n"
      "}\n";
  static const char texture_srv_uav_compute_source[] =
      "Texture2D<uint> g_in : register(t0);\n"
      "RWTexture2D<uint> g_out : register(u0);\n"
      "[numthreads(1, 1, 1)]\n"
      "void cs_texture_srv_uav_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  g_out[tid.xy] = g_in.Load(int3(tid.xy, 0)) + 7;\n"
      "}\n";
  static const char texture_array_uav_compute_source[] =
      "RWTexture2DArray<uint> g_out : register(u0);\n"
      "[numthreads(2, 1, 1)]\n"
      "void cs_texture_array_uav_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  g_out[uint3(0, 0, tid.x)] = 800 + tid.x;\n"
      "}\n";
  static const char texture3d_uav_compute_source[] =
      "RWTexture3D<uint> g_out : register(u0);\n"
      "[numthreads(2, 1, 1)]\n"
      "void cs_texture3d_uav_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  g_out[uint3(0, 0, tid.x)] = 900 + tid.x;\n"
      "}\n";
  static const char groupshared_atomic_compute_source[] =
      "RWStructuredBuffer<uint> g_out : register(u0);\n"
      "groupshared uint g_sum;\n"
      "[numthreads(4, 1, 1)]\n"
      "void cs_groupshared_atomic_main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {\n"
      "  if (gtid.x == 0) g_sum = 0;\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  InterlockedAdd(g_sum, tid.x + 1);\n"
      "  GroupMemoryBarrierWithGroupSync();\n"
      "  if (gtid.x == 0) {\n"
      "    g_out[0] = g_sum;\n"
      "    g_out[1] = 65261;\n"
      "    g_out[2] = 48879;\n"
      "    g_out[3] = 4660;\n"
      "  }\n"
      "}\n";
  static const char constant_offset_compute_source[] =
      "cbuffer OffsetConstants : register(b0) {\n"
      "  uint4 value;\n"
      "};\n"
      "RWBuffer<uint> g_out : register(u0);\n"
      "[numthreads(4, 1, 1)]\n"
      "void cs_constant_offset_main(uint3 tid : SV_DispatchThreadID) {\n"
      "  uint v = tid.x == 0 ? value.x : (tid.x == 1 ? value.y : (tid.x == 2 ? value.z : value.w));\n"
      "  g_out[tid.x] = v;\n"
      "}\n";
  HMODULE compiler = load_local_dll(L"d3dcompiler_47.dll", "d3dcompiler_47");
  if (!compiler)
    return false;

  auto compile = (D3DCompileProc)GetProcAddress(compiler, "D3DCompile");
  printf("UnityComputeProbe GetProcAddress(D3DCompile) proc=%p gle=%lu\n",
         compile, GetLastError());
  if (!compile)
    return false;

  bool pass = true;
  ID3DBlob *cs_blob = nullptr;
  ID3DBlob *typed_buffer_cs_blob = nullptr;
  ID3DBlob *structured_buffer_cs_blob = nullptr;
  ID3DBlob *multi_uav_cs_blob = nullptr;
  ID3DBlob *srv_uav_cs_blob = nullptr;
  ID3DBlob *raw_buffer_cs_blob = nullptr;
  ID3DBlob *texture_srv_uav_cs_blob = nullptr;
  ID3DBlob *texture_array_uav_cs_blob = nullptr;
  ID3DBlob *texture3d_uav_cs_blob = nullptr;
  ID3DBlob *groupshared_atomic_cs_blob = nullptr;
  ID3DBlob *constant_offset_cs_blob = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT hr = compile(compute_source, sizeof(compute_source) - 1,
                       "dxmt_headless_unity_compute", nullptr, nullptr,
                       "cs_main", "cs_5_0", 0, 0, &cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    return false;
  }
  if (errors)
    errors->Release();

  hr = compile(typed_buffer_compute_source,
               sizeof(typed_buffer_compute_source) - 1,
               "dxmt_headless_unity_typed_buffer_compute", nullptr, nullptr,
               "cs_typed_buffer_main", "cs_5_0", 0, 0,
               &typed_buffer_cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_typed_buffer_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_typed_buffer_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(structured_buffer_compute_source,
               sizeof(structured_buffer_compute_source) - 1,
               "dxmt_headless_unity_structured_buffer_compute", nullptr,
               nullptr, "cs_structured_buffer_main", "cs_5_0", 0, 0,
               &structured_buffer_cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_structured_buffer_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_structured_buffer_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(multi_uav_compute_source, sizeof(multi_uav_compute_source) - 1,
               "dxmt_headless_unity_multi_uav_compute", nullptr, nullptr,
               "cs_multi_uav_main", "cs_5_0", 0, 0, &multi_uav_cs_blob,
               &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_multi_uav_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_multi_uav_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (structured_buffer_cs_blob)
      structured_buffer_cs_blob->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(srv_uav_compute_source, sizeof(srv_uav_compute_source) - 1,
               "dxmt_headless_unity_srv_uav_compute", nullptr, nullptr,
               "cs_srv_uav_main", "cs_5_0", 0, 0, &srv_uav_cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_srv_uav_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_srv_uav_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (multi_uav_cs_blob)
      multi_uav_cs_blob->Release();
    if (structured_buffer_cs_blob)
      structured_buffer_cs_blob->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(raw_buffer_compute_source, sizeof(raw_buffer_compute_source) - 1,
               "dxmt_headless_unity_raw_buffer_compute", nullptr, nullptr,
               "cs_raw_buffer_main", "cs_5_0", 0, 0, &raw_buffer_cs_blob,
               &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_raw_buffer_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_raw_buffer_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (srv_uav_cs_blob)
      srv_uav_cs_blob->Release();
    if (multi_uav_cs_blob)
      multi_uav_cs_blob->Release();
    if (structured_buffer_cs_blob)
      structured_buffer_cs_blob->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(texture_srv_uav_compute_source,
               sizeof(texture_srv_uav_compute_source) - 1,
               "dxmt_headless_unity_texture_srv_uav_compute", nullptr,
               nullptr, "cs_texture_srv_uav_main", "cs_5_0", 0, 0,
               &texture_srv_uav_cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_texture_srv_uav_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_texture_srv_uav_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (raw_buffer_cs_blob)
      raw_buffer_cs_blob->Release();
    if (srv_uav_cs_blob)
      srv_uav_cs_blob->Release();
    if (multi_uav_cs_blob)
      multi_uav_cs_blob->Release();
    if (structured_buffer_cs_blob)
      structured_buffer_cs_blob->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(texture_array_uav_compute_source,
               sizeof(texture_array_uav_compute_source) - 1,
               "dxmt_headless_unity_texture_array_uav_compute", nullptr,
               nullptr, "cs_texture_array_uav_main", "cs_5_0", 0, 0,
               &texture_array_uav_cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_texture_array_uav_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_texture_array_uav_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (raw_buffer_cs_blob)
      raw_buffer_cs_blob->Release();
    if (texture_srv_uav_cs_blob)
      texture_srv_uav_cs_blob->Release();
    if (srv_uav_cs_blob)
      srv_uav_cs_blob->Release();
    if (multi_uav_cs_blob)
      multi_uav_cs_blob->Release();
    if (structured_buffer_cs_blob)
      structured_buffer_cs_blob->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(texture3d_uav_compute_source,
               sizeof(texture3d_uav_compute_source) - 1,
               "dxmt_headless_unity_texture3d_uav_compute", nullptr, nullptr,
               "cs_texture3d_uav_main", "cs_5_0", 0, 0,
               &texture3d_uav_cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_texture3d_uav_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_texture3d_uav_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (raw_buffer_cs_blob)
      raw_buffer_cs_blob->Release();
    if (texture_array_uav_cs_blob)
      texture_array_uav_cs_blob->Release();
    if (texture_srv_uav_cs_blob)
      texture_srv_uav_cs_blob->Release();
    if (srv_uav_cs_blob)
      srv_uav_cs_blob->Release();
    if (multi_uav_cs_blob)
      multi_uav_cs_blob->Release();
    if (structured_buffer_cs_blob)
      structured_buffer_cs_blob->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(groupshared_atomic_compute_source,
               sizeof(groupshared_atomic_compute_source) - 1,
               "dxmt_headless_unity_groupshared_atomic_compute", nullptr,
               nullptr, "cs_groupshared_atomic_main", "cs_5_0", 0, 0,
               &groupshared_atomic_cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_groupshared_atomic_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_groupshared_atomic_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (texture3d_uav_cs_blob)
      texture3d_uav_cs_blob->Release();
    if (texture_array_uav_cs_blob)
      texture_array_uav_cs_blob->Release();
    if (texture_srv_uav_cs_blob)
      texture_srv_uav_cs_blob->Release();
    if (srv_uav_cs_blob)
      srv_uav_cs_blob->Release();
    if (multi_uav_cs_blob)
      multi_uav_cs_blob->Release();
    if (structured_buffer_cs_blob)
      structured_buffer_cs_blob->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  errors = nullptr;
  hr = compile(constant_offset_compute_source,
               sizeof(constant_offset_compute_source) - 1,
               "dxmt_headless_unity_constant_offset_compute", nullptr, nullptr,
               "cs_constant_offset_main", "cs_5_0", 0, 0,
               &constant_offset_cs_blob, &errors);
  print_hr("UnityComputeProbe D3DCompile(cs_constant_offset_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityComputeProbe cs_constant_offset_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    if (groupshared_atomic_cs_blob)
      groupshared_atomic_cs_blob->Release();
    if (raw_buffer_cs_blob)
      raw_buffer_cs_blob->Release();
    if (texture3d_uav_cs_blob)
      texture3d_uav_cs_blob->Release();
    if (texture_array_uav_cs_blob)
      texture_array_uav_cs_blob->Release();
    if (texture_srv_uav_cs_blob)
      texture_srv_uav_cs_blob->Release();
    if (srv_uav_cs_blob)
      srv_uav_cs_blob->Release();
    if (multi_uav_cs_blob)
      multi_uav_cs_blob->Release();
    if (structured_buffer_cs_blob)
      structured_buffer_cs_blob->Release();
    if (typed_buffer_cs_blob)
      typed_buffer_cs_blob->Release();
    if (cs_blob)
      cs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  ID3D11ComputeShader *cs = nullptr;
  hr = device->CreateComputeShader(cs_blob->GetBufferPointer(),
                                   cs_blob->GetBufferSize(), nullptr, &cs);
  print_hr("UnityComputeProbe CreateComputeShader", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *structured_buffer_cs = nullptr;
  hr = device->CreateComputeShader(structured_buffer_cs_blob->GetBufferPointer(),
                                   structured_buffer_cs_blob->GetBufferSize(),
                                   nullptr, &structured_buffer_cs);
  print_hr("UnityComputeProbe CreateComputeShader(structured_buffer)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *typed_buffer_cs = nullptr;
  hr = device->CreateComputeShader(typed_buffer_cs_blob->GetBufferPointer(),
                                   typed_buffer_cs_blob->GetBufferSize(),
                                   nullptr, &typed_buffer_cs);
  print_hr("UnityComputeProbe CreateComputeShader(typed_buffer)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *multi_uav_cs = nullptr;
  hr = device->CreateComputeShader(multi_uav_cs_blob->GetBufferPointer(),
                                   multi_uav_cs_blob->GetBufferSize(), nullptr,
                                   &multi_uav_cs);
  print_hr("UnityComputeProbe CreateComputeShader(multi_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *srv_uav_cs = nullptr;
  hr = device->CreateComputeShader(srv_uav_cs_blob->GetBufferPointer(),
                                   srv_uav_cs_blob->GetBufferSize(), nullptr,
                                   &srv_uav_cs);
  print_hr("UnityComputeProbe CreateComputeShader(srv_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *raw_buffer_cs = nullptr;
  hr = device->CreateComputeShader(raw_buffer_cs_blob->GetBufferPointer(),
                                   raw_buffer_cs_blob->GetBufferSize(), nullptr,
                                   &raw_buffer_cs);
  print_hr("UnityComputeProbe CreateComputeShader(raw_buffer)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *texture_srv_uav_cs = nullptr;
  hr = device->CreateComputeShader(texture_srv_uav_cs_blob->GetBufferPointer(),
                                   texture_srv_uav_cs_blob->GetBufferSize(),
                                   nullptr, &texture_srv_uav_cs);
  print_hr("UnityComputeProbe CreateComputeShader(texture_srv_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *texture_array_uav_cs = nullptr;
  hr = device->CreateComputeShader(texture_array_uav_cs_blob->GetBufferPointer(),
                                   texture_array_uav_cs_blob->GetBufferSize(),
                                   nullptr, &texture_array_uav_cs);
  print_hr("UnityComputeProbe CreateComputeShader(texture_array_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *texture3d_uav_cs = nullptr;
  hr = device->CreateComputeShader(texture3d_uav_cs_blob->GetBufferPointer(),
                                   texture3d_uav_cs_blob->GetBufferSize(),
                                   nullptr, &texture3d_uav_cs);
  print_hr("UnityComputeProbe CreateComputeShader(texture3d_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *groupshared_atomic_cs = nullptr;
  hr = device->CreateComputeShader(groupshared_atomic_cs_blob->GetBufferPointer(),
                                   groupshared_atomic_cs_blob->GetBufferSize(),
                                   nullptr, &groupshared_atomic_cs);
  print_hr("UnityComputeProbe CreateComputeShader(groupshared_atomic)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ComputeShader *constant_offset_cs = nullptr;
  hr = device->CreateComputeShader(constant_offset_cs_blob->GetBufferPointer(),
                                   constant_offset_cs_blob->GetBufferSize(),
                                   nullptr, &constant_offset_cs);
  print_hr("UnityComputeProbe CreateComputeShader(constant_offset)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = 1;
  tex_desc.Height = 1;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_R32_UINT;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_DEFAULT;
  tex_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Texture2D *compute_tex = nullptr;
  hr = device->CreateTexture2D(&tex_desc, nullptr, &compute_tex);
  print_hr("UnityComputeProbe CreateTexture2D(uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *compute_uav = nullptr;
  if (compute_tex) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = tex_desc.Format;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    hr = device->CreateUnorderedAccessView(compute_tex, &uav_desc,
                                           &compute_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(uav)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_TEXTURE2D_DESC staging_desc = tex_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging = nullptr;
  hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  print_hr("UnityComputeProbe CreateTexture2D(staging)", hr);
  if (FAILED(hr))
    pass = false;

  const UINT dispatch_indirect_args[] = {1, 1, 1};
  D3D11_BUFFER_DESC dispatch_indirect_desc = {};
  dispatch_indirect_desc.ByteWidth = sizeof(dispatch_indirect_args);
  dispatch_indirect_desc.Usage = D3D11_USAGE_IMMUTABLE;
  dispatch_indirect_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  D3D11_SUBRESOURCE_DATA dispatch_indirect_data = {};
  dispatch_indirect_data.pSysMem = dispatch_indirect_args;
  ID3D11Buffer *dispatch_indirect_buffer = nullptr;
  hr = device->CreateBuffer(&dispatch_indirect_desc, &dispatch_indirect_data,
                            &dispatch_indirect_buffer);
  print_hr("UnityComputeProbe CreateBuffer(dispatch_indirect_args)", hr);
  if (FAILED(hr))
    pass = false;

  if (cs && compute_uav && compute_tex && staging && dispatch_indirect_buffer) {
    const UINT clear_value[4] = {};
    ctx->ClearUnorderedAccessViewUint(compute_uav, clear_value);
    printf("UnityComputeProbe ClearUnorderedAccessViewUint=PASS\n");

    ID3D11UnorderedAccessView *uavs[] = {compute_uav};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(1,1,1)=PASS\n");

    ctx->ClearUnorderedAccessViewUint(compute_uav, clear_value);
    printf("UnityComputeProbe ClearBeforeDispatchIndirect=PASS\n");
    ctx->DispatchIndirect(dispatch_indirect_buffer, 0);
    printf("UnityComputeProbe DispatchIndirect=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(staging, compute_tex);
    printf("UnityComputeProbe CopyResource(uav_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t value = *(const uint32_t *)mapped.pData;
      printf("UnityComputeProbe staging_value=%u\n", value);
      if (value != 42)
        pass = false;
      ctx->Unmap(staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kTypedBufferElements = 4;
  D3D11_BUFFER_DESC typed_buffer_desc = {};
  typed_buffer_desc.ByteWidth = kTypedBufferElements * sizeof(UINT);
  typed_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  typed_buffer_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Buffer *typed_buffer = nullptr;
  hr = device->CreateBuffer(&typed_buffer_desc, nullptr, &typed_buffer);
  print_hr("UnityComputeProbe CreateBuffer(typed_buffer_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *typed_buffer_uav = nullptr;
  if (typed_buffer) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC typed_buffer_uav_desc = {};
    typed_buffer_uav_desc.Format = DXGI_FORMAT_R32_UINT;
    typed_buffer_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    typed_buffer_uav_desc.Buffer.NumElements = kTypedBufferElements;
    hr = device->CreateUnorderedAccessView(typed_buffer,
                                           &typed_buffer_uav_desc,
                                           &typed_buffer_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(typed_buffer_uav)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC typed_buffer_staging_desc = typed_buffer_desc;
  typed_buffer_staging_desc.Usage = D3D11_USAGE_STAGING;
  typed_buffer_staging_desc.BindFlags = 0;
  typed_buffer_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *typed_buffer_staging = nullptr;
  hr = device->CreateBuffer(&typed_buffer_staging_desc, nullptr,
                            &typed_buffer_staging);
  print_hr("UnityComputeProbe CreateBuffer(typed_buffer_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (typed_buffer_cs && typed_buffer_uav && typed_buffer &&
      typed_buffer_staging) {
    ID3D11UnorderedAccessView *uavs[] = {typed_buffer_uav};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(typed_buffer_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(typed_buffer)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(typed_buffer_staging, typed_buffer);
    printf("UnityComputeProbe CopyResource(typed_buffer_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(typed_buffer_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(typed_buffer_staging)", hr);
    if (SUCCEEDED(hr)) {
      const UINT *values = (const UINT *)mapped.pData;
      printf("UnityComputeProbe typed_buffer_values=%u,%u,%u,%u\n",
             values[0], values[1], values[2], values[3]);
      for (UINT i = 0; i < kTypedBufferElements; i++) {
        if (values[i] != 100 + i)
          pass = false;
      }
      ctx->Unmap(typed_buffer_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kStructuredBufferElements = 4;
  D3D11_BUFFER_DESC structured_buffer_desc = {};
  structured_buffer_desc.ByteWidth = kStructuredBufferElements * sizeof(UINT);
  structured_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  structured_buffer_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  structured_buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  structured_buffer_desc.StructureByteStride = sizeof(UINT);
  ID3D11Buffer *structured_buffer = nullptr;
  hr = device->CreateBuffer(&structured_buffer_desc, nullptr,
                            &structured_buffer);
  print_hr("UnityComputeProbe CreateBuffer(structured_buffer_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *structured_buffer_uav = nullptr;
  if (structured_buffer) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC structured_buffer_uav_desc = {};
    structured_buffer_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    structured_buffer_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    structured_buffer_uav_desc.Buffer.NumElements = kStructuredBufferElements;
    hr = device->CreateUnorderedAccessView(structured_buffer,
                                           &structured_buffer_uav_desc,
                                           &structured_buffer_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(structured_buffer_uav)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC structured_buffer_staging_desc = structured_buffer_desc;
  structured_buffer_staging_desc.Usage = D3D11_USAGE_STAGING;
  structured_buffer_staging_desc.BindFlags = 0;
  structured_buffer_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  structured_buffer_staging_desc.MiscFlags = 0;
  structured_buffer_staging_desc.StructureByteStride = 0;
  ID3D11Buffer *structured_buffer_staging = nullptr;
  hr = device->CreateBuffer(&structured_buffer_staging_desc, nullptr,
                            &structured_buffer_staging);
  print_hr("UnityComputeProbe CreateBuffer(structured_buffer_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (structured_buffer_cs && structured_buffer_uav && structured_buffer &&
      structured_buffer_staging) {
    ID3D11UnorderedAccessView *uavs[] = {structured_buffer_uav};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(structured_buffer_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(structured_buffer)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(structured_buffer_staging, structured_buffer);
    printf("UnityComputeProbe CopyResource(structured_buffer_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(structured_buffer_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(structured_buffer_staging)", hr);
    if (SUCCEEDED(hr)) {
      const UINT *values = (const UINT *)mapped.pData;
      printf("UnityComputeProbe structured_buffer_values=%u,%u,%u,%u\n",
             values[0], values[1], values[2], values[3]);
      for (UINT i = 0; i < kStructuredBufferElements; i++) {
        if (values[i] != 200 + i)
          pass = false;
      }
      ctx->Unmap(structured_buffer_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kMultiUAVElements = 4;
  D3D11_BUFFER_DESC multi_uav_desc = {};
  multi_uav_desc.ByteWidth = kMultiUAVElements * sizeof(UINT);
  multi_uav_desc.Usage = D3D11_USAGE_DEFAULT;
  multi_uav_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  multi_uav_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  multi_uav_desc.StructureByteStride = sizeof(UINT);
  ID3D11Buffer *multi_uav_buffer0 = nullptr;
  hr = device->CreateBuffer(&multi_uav_desc, nullptr, &multi_uav_buffer0);
  print_hr("UnityComputeProbe CreateBuffer(multi_uav0)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11Buffer *multi_uav_buffer1 = nullptr;
  hr = device->CreateBuffer(&multi_uav_desc, nullptr, &multi_uav_buffer1);
  print_hr("UnityComputeProbe CreateBuffer(multi_uav1)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_UNORDERED_ACCESS_VIEW_DESC multi_uav_desc_view = {};
  multi_uav_desc_view.Format = DXGI_FORMAT_UNKNOWN;
  multi_uav_desc_view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  multi_uav_desc_view.Buffer.NumElements = kMultiUAVElements;
  ID3D11UnorderedAccessView *multi_uav0 = nullptr;
  if (multi_uav_buffer0) {
    hr = device->CreateUnorderedAccessView(multi_uav_buffer0,
                                           &multi_uav_desc_view, &multi_uav0);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(multi_uav0)", hr);
    if (FAILED(hr))
      pass = false;
  }

  ID3D11UnorderedAccessView *multi_uav1 = nullptr;
  if (multi_uav_buffer1) {
    hr = device->CreateUnorderedAccessView(multi_uav_buffer1,
                                           &multi_uav_desc_view, &multi_uav1);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(multi_uav1)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC multi_uav_staging_desc = multi_uav_desc;
  multi_uav_staging_desc.Usage = D3D11_USAGE_STAGING;
  multi_uav_staging_desc.BindFlags = 0;
  multi_uav_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  multi_uav_staging_desc.MiscFlags = 0;
  multi_uav_staging_desc.StructureByteStride = 0;
  ID3D11Buffer *multi_uav_staging0 = nullptr;
  hr = device->CreateBuffer(&multi_uav_staging_desc, nullptr,
                            &multi_uav_staging0);
  print_hr("UnityComputeProbe CreateBuffer(multi_uav_staging0)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11Buffer *multi_uav_staging1 = nullptr;
  hr = device->CreateBuffer(&multi_uav_staging_desc, nullptr,
                            &multi_uav_staging1);
  print_hr("UnityComputeProbe CreateBuffer(multi_uav_staging1)", hr);
  if (FAILED(hr))
    pass = false;

  if (multi_uav_cs && multi_uav0 && multi_uav1 && multi_uav_buffer0 &&
      multi_uav_buffer1 && multi_uav_staging0 && multi_uav_staging1) {
    ID3D11UnorderedAccessView *uavs[] = {multi_uav0, multi_uav1};
    UINT initial_counts[] = {0, 0};
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, initial_counts);
    ctx->CSSetShader(multi_uav_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(multi_uav)=PASS\n");

    ID3D11UnorderedAccessView *null_uavs[] = {nullptr, nullptr};
    ctx->CSSetUnorderedAccessViews(0, 2, null_uavs, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(multi_uav_staging0, multi_uav_buffer0);
    printf("UnityComputeProbe CopyResource(multi_uav0_to_staging)=PASS\n");
    ctx->CopyResource(multi_uav_staging1, multi_uav_buffer1);
    printf("UnityComputeProbe CopyResource(multi_uav1_to_staging)=PASS\n");

    D3D11_MAPPED_SUBRESOURCE mapped0 = {};
    hr = ctx->Map(multi_uav_staging0, 0, D3D11_MAP_READ, 0, &mapped0);
    print_hr("UnityComputeProbe Map(multi_uav_staging0)", hr);
    if (SUCCEEDED(hr)) {
      const UINT *values = (const UINT *)mapped0.pData;
      printf("UnityComputeProbe multi_uav0_values=%u,%u,%u,%u\n",
             values[0], values[1], values[2], values[3]);
      for (UINT i = 0; i < kMultiUAVElements; i++) {
        if (values[i] != 400 + i)
          pass = false;
      }
      ctx->Unmap(multi_uav_staging0, 0);
    } else {
      pass = false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped1 = {};
    hr = ctx->Map(multi_uav_staging1, 0, D3D11_MAP_READ, 0, &mapped1);
    print_hr("UnityComputeProbe Map(multi_uav_staging1)", hr);
    if (SUCCEEDED(hr)) {
      const UINT *values = (const UINT *)mapped1.pData;
      printf("UnityComputeProbe multi_uav1_values=%u,%u,%u,%u\n",
             values[0], values[1], values[2], values[3]);
      for (UINT i = 0; i < kMultiUAVElements; i++) {
        if (values[i] != 500 + i)
          pass = false;
      }
      ctx->Unmap(multi_uav_staging1, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kSrvUAVElements = 4;
  const UINT srv_uav_source_values[kSrvUAVElements] = {600, 601, 602, 603};
  D3D11_SUBRESOURCE_DATA srv_uav_source_data = {};
  srv_uav_source_data.pSysMem = srv_uav_source_values;
  D3D11_BUFFER_DESC srv_uav_source_desc = {};
  srv_uav_source_desc.ByteWidth = kSrvUAVElements * sizeof(UINT);
  srv_uav_source_desc.Usage = D3D11_USAGE_IMMUTABLE;
  srv_uav_source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  srv_uav_source_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  srv_uav_source_desc.StructureByteStride = sizeof(UINT);
  ID3D11Buffer *srv_uav_source = nullptr;
  hr = device->CreateBuffer(&srv_uav_source_desc, &srv_uav_source_data,
                            &srv_uav_source);
  print_hr("UnityComputeProbe CreateBuffer(srv_uav_source)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ShaderResourceView *srv_uav_source_srv = nullptr;
  if (srv_uav_source) {
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_uav_source_srv_desc = {};
    srv_uav_source_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_uav_source_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv_uav_source_srv_desc.Buffer.NumElements = kSrvUAVElements;
    hr = device->CreateShaderResourceView(srv_uav_source,
                                          &srv_uav_source_srv_desc,
                                          &srv_uav_source_srv);
    print_hr("UnityComputeProbe CreateShaderResourceView(srv_uav_source)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC srv_uav_output_desc = {};
  srv_uav_output_desc.ByteWidth = kSrvUAVElements * sizeof(UINT);
  srv_uav_output_desc.Usage = D3D11_USAGE_DEFAULT;
  srv_uav_output_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  srv_uav_output_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  srv_uav_output_desc.StructureByteStride = sizeof(UINT);
  ID3D11Buffer *srv_uav_output = nullptr;
  hr = device->CreateBuffer(&srv_uav_output_desc, nullptr, &srv_uav_output);
  print_hr("UnityComputeProbe CreateBuffer(srv_uav_output)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *srv_uav_output_uav = nullptr;
  if (srv_uav_output) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC srv_uav_output_uav_desc = {};
    srv_uav_output_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_uav_output_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    srv_uav_output_uav_desc.Buffer.NumElements = kSrvUAVElements;
    hr = device->CreateUnorderedAccessView(srv_uav_output,
                                           &srv_uav_output_uav_desc,
                                           &srv_uav_output_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(srv_uav_output)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC srv_uav_staging_desc = srv_uav_output_desc;
  srv_uav_staging_desc.Usage = D3D11_USAGE_STAGING;
  srv_uav_staging_desc.BindFlags = 0;
  srv_uav_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  srv_uav_staging_desc.MiscFlags = 0;
  srv_uav_staging_desc.StructureByteStride = 0;
  ID3D11Buffer *srv_uav_staging = nullptr;
  hr = device->CreateBuffer(&srv_uav_staging_desc, nullptr, &srv_uav_staging);
  print_hr("UnityComputeProbe CreateBuffer(srv_uav_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (srv_uav_cs && srv_uav_source_srv && srv_uav_output_uav &&
      srv_uav_output && srv_uav_staging) {
    ID3D11ShaderResourceView *srvs[] = {srv_uav_source_srv};
    ctx->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView *uavs[] = {srv_uav_output_uav};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(srv_uav_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(srv_uav)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ID3D11ShaderResourceView *null_srv[] = {nullptr};
    ctx->CSSetShaderResources(0, 1, null_srv);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(srv_uav_staging, srv_uav_output);
    printf("UnityComputeProbe CopyResource(srv_uav_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(srv_uav_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(srv_uav_staging)", hr);
    if (SUCCEEDED(hr)) {
      const UINT *values = (const UINT *)mapped.pData;
      printf("UnityComputeProbe srv_uav_values=%u,%u,%u,%u\n",
             values[0], values[1], values[2], values[3]);
      for (UINT i = 0; i < kSrvUAVElements; i++) {
        if (values[i] != srv_uav_source_values[i] + 17)
          pass = false;
      }
      ctx->Unmap(srv_uav_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kRawBufferElements = 4;
  const UINT raw_buffer_source_values[kRawBufferElements] = {1000, 1001, 1002,
                                                            1003};
  D3D11_SUBRESOURCE_DATA raw_buffer_source_data = {};
  raw_buffer_source_data.pSysMem = raw_buffer_source_values;
  D3D11_BUFFER_DESC raw_buffer_source_desc = {};
  raw_buffer_source_desc.ByteWidth = sizeof(raw_buffer_source_values);
  raw_buffer_source_desc.Usage = D3D11_USAGE_IMMUTABLE;
  raw_buffer_source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  raw_buffer_source_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ID3D11Buffer *raw_buffer_source = nullptr;
  hr = device->CreateBuffer(&raw_buffer_source_desc, &raw_buffer_source_data,
                            &raw_buffer_source);
  print_hr("UnityComputeProbe CreateBuffer(raw_buffer_source)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ShaderResourceView *raw_buffer_source_srv = nullptr;
  if (raw_buffer_source) {
    D3D11_SHADER_RESOURCE_VIEW_DESC raw_buffer_source_srv_desc = {};
    raw_buffer_source_srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    raw_buffer_source_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    raw_buffer_source_srv_desc.BufferEx.NumElements = kRawBufferElements;
    raw_buffer_source_srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    hr = device->CreateShaderResourceView(raw_buffer_source,
                                          &raw_buffer_source_srv_desc,
                                          &raw_buffer_source_srv);
    print_hr("UnityComputeProbe CreateShaderResourceView(raw_buffer_source)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC raw_buffer_output_desc = {};
  raw_buffer_output_desc.ByteWidth = sizeof(raw_buffer_source_values);
  raw_buffer_output_desc.Usage = D3D11_USAGE_DEFAULT;
  raw_buffer_output_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  raw_buffer_output_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  ID3D11Buffer *raw_buffer_output = nullptr;
  hr = device->CreateBuffer(&raw_buffer_output_desc, nullptr, &raw_buffer_output);
  print_hr("UnityComputeProbe CreateBuffer(raw_buffer_output)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *raw_buffer_output_uav = nullptr;
  if (raw_buffer_output) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC raw_buffer_output_uav_desc = {};
    raw_buffer_output_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    raw_buffer_output_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    raw_buffer_output_uav_desc.Buffer.NumElements = kRawBufferElements;
    raw_buffer_output_uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    hr = device->CreateUnorderedAccessView(raw_buffer_output,
                                           &raw_buffer_output_uav_desc,
                                           &raw_buffer_output_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(raw_buffer_output)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC raw_buffer_staging_desc = raw_buffer_output_desc;
  raw_buffer_staging_desc.Usage = D3D11_USAGE_STAGING;
  raw_buffer_staging_desc.BindFlags = 0;
  raw_buffer_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  raw_buffer_staging_desc.MiscFlags = 0;
  ID3D11Buffer *raw_buffer_staging = nullptr;
  hr = device->CreateBuffer(&raw_buffer_staging_desc, nullptr,
                            &raw_buffer_staging);
  print_hr("UnityComputeProbe CreateBuffer(raw_buffer_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (raw_buffer_cs && raw_buffer_source_srv && raw_buffer_output_uav &&
      raw_buffer_output && raw_buffer_staging) {
    ID3D11ShaderResourceView *srvs[] = {raw_buffer_source_srv};
    ctx->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView *uavs[] = {raw_buffer_output_uav};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(raw_buffer_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(raw_buffer)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ID3D11ShaderResourceView *null_srv[] = {nullptr};
    ctx->CSSetShaderResources(0, 1, null_srv);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(raw_buffer_staging, raw_buffer_output);
    printf("UnityComputeProbe CopyResource(raw_buffer_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(raw_buffer_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(raw_buffer_staging)", hr);
    if (SUCCEEDED(hr)) {
      const UINT *values = (const UINT *)mapped.pData;
      printf("UnityComputeProbe raw_buffer_values=%u,%u,%u,%u\n",
             values[0], values[1], values[2], values[3]);
      for (UINT i = 0; i < kRawBufferElements; i++) {
        if (values[i] != raw_buffer_source_values[i] + 33)
          pass = false;
      }
      ctx->Unmap(raw_buffer_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  const UINT texture_srv_uav_source_value = 700;
  D3D11_TEXTURE2D_DESC texture_srv_uav_source_desc = {};
  texture_srv_uav_source_desc.Width = 1;
  texture_srv_uav_source_desc.Height = 1;
  texture_srv_uav_source_desc.MipLevels = 1;
  texture_srv_uav_source_desc.ArraySize = 1;
  texture_srv_uav_source_desc.Format = DXGI_FORMAT_R32_UINT;
  texture_srv_uav_source_desc.SampleDesc.Count = 1;
  texture_srv_uav_source_desc.Usage = D3D11_USAGE_IMMUTABLE;
  texture_srv_uav_source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA texture_srv_uav_source_data = {};
  texture_srv_uav_source_data.pSysMem = &texture_srv_uav_source_value;
  texture_srv_uav_source_data.SysMemPitch = sizeof(texture_srv_uav_source_value);
  ID3D11Texture2D *texture_srv_uav_source = nullptr;
  hr = device->CreateTexture2D(&texture_srv_uav_source_desc,
                               &texture_srv_uav_source_data,
                               &texture_srv_uav_source);
  print_hr("UnityComputeProbe CreateTexture2D(texture_srv_uav_source)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11ShaderResourceView *texture_srv_uav_source_srv = nullptr;
  if (texture_srv_uav_source) {
    D3D11_SHADER_RESOURCE_VIEW_DESC texture_srv_uav_source_srv_desc = {};
    texture_srv_uav_source_srv_desc.Format = DXGI_FORMAT_R32_UINT;
    texture_srv_uav_source_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    texture_srv_uav_source_srv_desc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(texture_srv_uav_source,
                                          &texture_srv_uav_source_srv_desc,
                                          &texture_srv_uav_source_srv);
    print_hr("UnityComputeProbe CreateShaderResourceView(texture_srv_uav_source)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_TEXTURE2D_DESC texture_srv_uav_output_desc =
      texture_srv_uav_source_desc;
  texture_srv_uav_output_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_srv_uav_output_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Texture2D *texture_srv_uav_output = nullptr;
  hr = device->CreateTexture2D(&texture_srv_uav_output_desc, nullptr,
                               &texture_srv_uav_output);
  print_hr("UnityComputeProbe CreateTexture2D(texture_srv_uav_output)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *texture_srv_uav_output_uav = nullptr;
  if (texture_srv_uav_output) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC texture_srv_uav_output_uav_desc = {};
    texture_srv_uav_output_uav_desc.Format = DXGI_FORMAT_R32_UINT;
    texture_srv_uav_output_uav_desc.ViewDimension =
        D3D11_UAV_DIMENSION_TEXTURE2D;
    hr = device->CreateUnorderedAccessView(texture_srv_uav_output,
                                           &texture_srv_uav_output_uav_desc,
                                           &texture_srv_uav_output_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(texture_srv_uav_output)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_TEXTURE2D_DESC texture_srv_uav_staging_desc =
      texture_srv_uav_output_desc;
  texture_srv_uav_staging_desc.Usage = D3D11_USAGE_STAGING;
  texture_srv_uav_staging_desc.BindFlags = 0;
  texture_srv_uav_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *texture_srv_uav_staging = nullptr;
  hr = device->CreateTexture2D(&texture_srv_uav_staging_desc, nullptr,
                               &texture_srv_uav_staging);
  print_hr("UnityComputeProbe CreateTexture2D(texture_srv_uav_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (texture_srv_uav_cs && texture_srv_uav_source_srv &&
      texture_srv_uav_output_uav && texture_srv_uav_output &&
      texture_srv_uav_staging) {
    ID3D11ShaderResourceView *srvs[] = {texture_srv_uav_source_srv};
    ctx->CSSetShaderResources(0, 1, srvs);
    ID3D11UnorderedAccessView *uavs[] = {texture_srv_uav_output_uav};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(texture_srv_uav_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(texture_srv_uav)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ID3D11ShaderResourceView *null_srv[] = {nullptr};
    ctx->CSSetShaderResources(0, 1, null_srv);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(texture_srv_uav_staging, texture_srv_uav_output);
    printf("UnityComputeProbe CopyResource(texture_srv_uav_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(texture_srv_uav_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(texture_srv_uav_staging)", hr);
    if (SUCCEEDED(hr)) {
      const UINT value = *(const UINT *)mapped.pData;
      printf("UnityComputeProbe texture_srv_uav_value=%u\n", value);
      if (value != texture_srv_uav_source_value + 7)
        pass = false;
      ctx->Unmap(texture_srv_uav_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kTextureArrayUAVSlices = 2;
  D3D11_TEXTURE2D_DESC texture_array_uav_desc = {};
  texture_array_uav_desc.Width = 1;
  texture_array_uav_desc.Height = 1;
  texture_array_uav_desc.MipLevels = 1;
  texture_array_uav_desc.ArraySize = kTextureArrayUAVSlices;
  texture_array_uav_desc.Format = DXGI_FORMAT_R32_UINT;
  texture_array_uav_desc.SampleDesc.Count = 1;
  texture_array_uav_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_array_uav_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Texture2D *texture_array_uav = nullptr;
  hr = device->CreateTexture2D(&texture_array_uav_desc, nullptr,
                               &texture_array_uav);
  print_hr("UnityComputeProbe CreateTexture2D(texture_array_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *texture_array_uav_view = nullptr;
  if (texture_array_uav) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC texture_array_uav_view_desc = {};
    texture_array_uav_view_desc.Format = DXGI_FORMAT_R32_UINT;
    texture_array_uav_view_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    texture_array_uav_view_desc.Texture2DArray.ArraySize = kTextureArrayUAVSlices;
    hr = device->CreateUnorderedAccessView(texture_array_uav,
                                           &texture_array_uav_view_desc,
                                           &texture_array_uav_view);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(texture_array_uav)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_TEXTURE2D_DESC texture_array_uav_staging_desc = texture_array_uav_desc;
  texture_array_uav_staging_desc.Usage = D3D11_USAGE_STAGING;
  texture_array_uav_staging_desc.BindFlags = 0;
  texture_array_uav_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *texture_array_uav_staging = nullptr;
  hr = device->CreateTexture2D(&texture_array_uav_staging_desc, nullptr,
                               &texture_array_uav_staging);
  print_hr("UnityComputeProbe CreateTexture2D(texture_array_uav_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (texture_array_uav_cs && texture_array_uav_view && texture_array_uav &&
      texture_array_uav_staging) {
    ID3D11UnorderedAccessView *uavs[] = {texture_array_uav_view};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(texture_array_uav_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(texture_array_uav)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(texture_array_uav_staging, texture_array_uav);
    printf("UnityComputeProbe CopyResource(texture_array_uav_to_staging)=PASS\n");
    UINT values[kTextureArrayUAVSlices] = {};
    for (UINT slice = 0; slice < kTextureArrayUAVSlices; slice++) {
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      hr = ctx->Map(texture_array_uav_staging, slice, D3D11_MAP_READ, 0,
                    &mapped);
      print_hr("UnityComputeProbe Map(texture_array_uav_staging)", hr);
      if (SUCCEEDED(hr)) {
        values[slice] = *(const UINT *)mapped.pData;
        ctx->Unmap(texture_array_uav_staging, slice);
      } else {
        pass = false;
      }
    }
    printf("UnityComputeProbe texture_array_uav_values=%u,%u\n",
           values[0], values[1]);
    for (UINT slice = 0; slice < kTextureArrayUAVSlices; slice++) {
      if (values[slice] != 800 + slice)
        pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kTexture3DUAVDepth = 2;
  D3D11_TEXTURE3D_DESC texture3d_uav_desc = {};
  texture3d_uav_desc.Width = 1;
  texture3d_uav_desc.Height = 1;
  texture3d_uav_desc.Depth = kTexture3DUAVDepth;
  texture3d_uav_desc.MipLevels = 1;
  texture3d_uav_desc.Format = DXGI_FORMAT_R32_UINT;
  texture3d_uav_desc.Usage = D3D11_USAGE_DEFAULT;
  texture3d_uav_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Texture3D *texture3d_uav = nullptr;
  hr = device->CreateTexture3D(&texture3d_uav_desc, nullptr, &texture3d_uav);
  print_hr("UnityComputeProbe CreateTexture3D(texture3d_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *texture3d_uav_view = nullptr;
  if (texture3d_uav) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC texture3d_uav_view_desc = {};
    texture3d_uav_view_desc.Format = DXGI_FORMAT_R32_UINT;
    texture3d_uav_view_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
    texture3d_uav_view_desc.Texture3D.WSize = kTexture3DUAVDepth;
    hr = device->CreateUnorderedAccessView(texture3d_uav,
                                           &texture3d_uav_view_desc,
                                           &texture3d_uav_view);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(texture3d_uav)", hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_TEXTURE3D_DESC texture3d_uav_staging_desc = texture3d_uav_desc;
  texture3d_uav_staging_desc.Usage = D3D11_USAGE_STAGING;
  texture3d_uav_staging_desc.BindFlags = 0;
  texture3d_uav_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture3D *texture3d_uav_staging = nullptr;
  hr = device->CreateTexture3D(&texture3d_uav_staging_desc, nullptr,
                               &texture3d_uav_staging);
  print_hr("UnityComputeProbe CreateTexture3D(texture3d_uav_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (texture3d_uav_cs && texture3d_uav_view && texture3d_uav &&
      texture3d_uav_staging) {
    ID3D11UnorderedAccessView *uavs[] = {texture3d_uav_view};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(texture3d_uav_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(texture3d_uav)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(texture3d_uav_staging, texture3d_uav);
    printf("UnityComputeProbe CopyResource(texture3d_uav_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(texture3d_uav_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(texture3d_uav_staging)", hr);
    if (SUCCEEDED(hr)) {
      const auto *base = (const uint8_t *)mapped.pData;
      const UINT value0 = *(const UINT *)(base);
      const UINT value1 = *(const UINT *)(base + mapped.DepthPitch);
      printf("UnityComputeProbe texture3d_uav_values=%u,%u\n", value0, value1);
      if (value0 != 900 || value1 != 901)
        pass = false;
      ctx->Unmap(texture3d_uav_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kGroupsharedAtomicElements = 4;
  D3D11_BUFFER_DESC groupshared_atomic_desc = {};
  groupshared_atomic_desc.ByteWidth =
      kGroupsharedAtomicElements * sizeof(UINT);
  groupshared_atomic_desc.Usage = D3D11_USAGE_DEFAULT;
  groupshared_atomic_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  groupshared_atomic_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  groupshared_atomic_desc.StructureByteStride = sizeof(UINT);
  ID3D11Buffer *groupshared_atomic_buffer = nullptr;
  hr = device->CreateBuffer(&groupshared_atomic_desc, nullptr,
                            &groupshared_atomic_buffer);
  print_hr("UnityComputeProbe CreateBuffer(groupshared_atomic_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *groupshared_atomic_uav = nullptr;
  if (groupshared_atomic_buffer) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC groupshared_atomic_uav_desc = {};
    groupshared_atomic_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    groupshared_atomic_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    groupshared_atomic_uav_desc.Buffer.NumElements =
        kGroupsharedAtomicElements;
    hr = device->CreateUnorderedAccessView(groupshared_atomic_buffer,
                                           &groupshared_atomic_uav_desc,
                                           &groupshared_atomic_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(groupshared_atomic)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC groupshared_atomic_staging_desc = groupshared_atomic_desc;
  groupshared_atomic_staging_desc.Usage = D3D11_USAGE_STAGING;
  groupshared_atomic_staging_desc.BindFlags = 0;
  groupshared_atomic_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  groupshared_atomic_staging_desc.MiscFlags = 0;
  groupshared_atomic_staging_desc.StructureByteStride = 0;
  ID3D11Buffer *groupshared_atomic_staging = nullptr;
  hr = device->CreateBuffer(&groupshared_atomic_staging_desc, nullptr,
                            &groupshared_atomic_staging);
  print_hr("UnityComputeProbe CreateBuffer(groupshared_atomic_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (groupshared_atomic_cs && groupshared_atomic_uav &&
      groupshared_atomic_buffer && groupshared_atomic_staging) {
    ID3D11UnorderedAccessView *uavs[] = {groupshared_atomic_uav};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(groupshared_atomic_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(groupshared_atomic)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(groupshared_atomic_staging, groupshared_atomic_buffer);
    printf("UnityComputeProbe CopyResource(groupshared_atomic_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(groupshared_atomic_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(groupshared_atomic_staging)", hr);
    if (SUCCEEDED(hr)) {
      const UINT *values = (const UINT *)mapped.pData;
      printf("UnityComputeProbe groupshared_atomic_values=%u,%u,%u,%u\n",
             values[0], values[1], values[2], values[3]);
      if (values[0] != 10 || values[1] != 65261 || values[2] != 48879 ||
          values[3] != 4660)
        pass = false;
      ctx->Unmap(groupshared_atomic_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kConstantOffsetElements = 4;
  const UINT constant_offset_constants[12] = {
      10,  11,  12,  13,
      100, 101, 102, 103,
      300, 301, 302, 303,
  };
  D3D11_SUBRESOURCE_DATA constant_offset_data = {};
  constant_offset_data.pSysMem = constant_offset_constants;
  D3D11_BUFFER_DESC constant_offset_cbuffer_desc = {};
  constant_offset_cbuffer_desc.ByteWidth = sizeof(constant_offset_constants);
  constant_offset_cbuffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
  constant_offset_cbuffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer *constant_offset_cbuffer = nullptr;
  hr = device->CreateBuffer(&constant_offset_cbuffer_desc, &constant_offset_data,
                            &constant_offset_cbuffer);
  print_hr("UnityComputeProbe CreateBuffer(constant_offset_cbuffer)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC constant_offset_output_desc = {};
  constant_offset_output_desc.ByteWidth =
      kConstantOffsetElements * sizeof(UINT);
  constant_offset_output_desc.Usage = D3D11_USAGE_DEFAULT;
  constant_offset_output_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Buffer *constant_offset_output = nullptr;
  hr = device->CreateBuffer(&constant_offset_output_desc, nullptr,
                            &constant_offset_output);
  print_hr("UnityComputeProbe CreateBuffer(constant_offset_output)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *constant_offset_uav = nullptr;
  if (constant_offset_output) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC constant_offset_uav_desc = {};
    constant_offset_uav_desc.Format = DXGI_FORMAT_R32_UINT;
    constant_offset_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    constant_offset_uav_desc.Buffer.NumElements = kConstantOffsetElements;
    hr = device->CreateUnorderedAccessView(constant_offset_output,
                                           &constant_offset_uav_desc,
                                           &constant_offset_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(constant_offset)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC constant_offset_staging_desc = constant_offset_output_desc;
  constant_offset_staging_desc.Usage = D3D11_USAGE_STAGING;
  constant_offset_staging_desc.BindFlags = 0;
  constant_offset_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *constant_offset_staging = nullptr;
  hr = device->CreateBuffer(&constant_offset_staging_desc, nullptr,
                            &constant_offset_staging);
  print_hr("UnityComputeProbe CreateBuffer(constant_offset_staging)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11DeviceContext1 *ctx1 = nullptr;
  hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&ctx1);
  print_hr("UnityComputeProbe QueryInterface(ID3D11DeviceContext1)", hr);
  if (FAILED(hr))
    pass = false;

  if (ctx1 && constant_offset_cs && constant_offset_cbuffer &&
      constant_offset_uav && constant_offset_output && constant_offset_staging) {
    UINT first_constant[] = {2};
    UINT num_constants[] = {1};
    ID3D11Buffer *cbuffers[] = {constant_offset_cbuffer};
    ctx1->CSSetConstantBuffers1(0, 1, cbuffers, first_constant,
                                num_constants);
    printf("UnityComputeProbe CSSetConstantBuffers1(first=2,count=1)=PASS\n");

    ID3D11Buffer *bound_cbuffer = nullptr;
    UINT bound_first = 0xffffffffu;
    UINT bound_count = 0xffffffffu;
    ctx1->CSGetConstantBuffers1(0, 1, &bound_cbuffer, &bound_first,
                                &bound_count);
    printf("UnityComputeProbe CSGetConstantBuffers1 buffer=%p first=%u count=%u\n",
           bound_cbuffer, bound_first, bound_count);
    if (bound_cbuffer != constant_offset_cbuffer || bound_first != 2 ||
        bound_count != 1)
      pass = false;
    if (bound_cbuffer)
      bound_cbuffer->Release();

    ID3D11UnorderedAccessView *uavs[] = {constant_offset_uav};
    UINT initial_counts[] = {0};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    ctx->CSSetShader(constant_offset_cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    printf("UnityComputeProbe Dispatch(constant_offset)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ID3D11Buffer *null_cbuffer[] = {nullptr};
    ctx1->CSSetConstantBuffers1(0, 1, null_cbuffer, nullptr, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    ctx->CopyResource(constant_offset_staging, constant_offset_output);
    printf("UnityComputeProbe CopyResource(constant_offset_to_staging)=PASS\n");
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(constant_offset_staging, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityComputeProbe Map(constant_offset_staging)", hr);
    if (SUCCEEDED(hr)) {
      const UINT *values = (const UINT *)mapped.pData;
      printf("UnityComputeProbe constant_offset_values=%u,%u,%u,%u\n",
             values[0], values[1], values[2], values[3]);
      for (UINT i = 0; i < kConstantOffsetElements; i++) {
        if (values[i] != constant_offset_constants[8 + i])
          pass = false;
      }
      ctx->Unmap(constant_offset_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  static const UINT kAppendBufferElements = 4;
  D3D11_BUFFER_DESC append_buffer_desc = {};
  append_buffer_desc.ByteWidth = kAppendBufferElements * sizeof(UINT);
  append_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  append_buffer_desc.BindFlags =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  append_buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  append_buffer_desc.StructureByteStride = sizeof(UINT);
  ID3D11Buffer *append_buffer = nullptr;
  hr = device->CreateBuffer(&append_buffer_desc, nullptr, &append_buffer);
  print_hr("UnityComputeProbe CreateBuffer(append_buffer_uav)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11UnorderedAccessView *append_buffer_uav = nullptr;
  if (append_buffer) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC append_buffer_uav_desc = {};
    append_buffer_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    append_buffer_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    append_buffer_uav_desc.Buffer.NumElements = kAppendBufferElements;
    append_buffer_uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    hr = device->CreateUnorderedAccessView(append_buffer,
                                           &append_buffer_uav_desc,
                                           &append_buffer_uav);
    print_hr("UnityComputeProbe CreateUnorderedAccessView(append_buffer_uav)",
             hr);
    if (FAILED(hr))
      pass = false;
  }

  D3D11_BUFFER_DESC append_count_desc = {};
  append_count_desc.ByteWidth = sizeof(UINT);
  append_count_desc.Usage = D3D11_USAGE_DEFAULT;
  ID3D11Buffer *append_count_buffer = nullptr;
  hr = device->CreateBuffer(&append_count_desc, nullptr, &append_count_buffer);
  print_hr("UnityComputeProbe CreateBuffer(append_count)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC append_count_staging_desc = append_count_desc;
  append_count_staging_desc.Usage = D3D11_USAGE_STAGING;
  append_count_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *append_count_staging = nullptr;
  hr = device->CreateBuffer(&append_count_staging_desc, nullptr,
                            &append_count_staging);
  print_hr("UnityComputeProbe CreateBuffer(append_count_staging)", hr);
  if (FAILED(hr))
    pass = false;

  if (append_buffer_uav && append_count_buffer && append_count_staging) {
    ID3D11UnorderedAccessView *uavs[] = {append_buffer_uav};
    UINT initial_counts[] = {kAppendBufferElements};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initial_counts);
    printf("UnityComputeProbe SetAppendCounter(initial=4)=PASS\n");

    ID3D11UnorderedAccessView *null_uav[] = {nullptr};
    ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);

    ctx->CopyStructureCount(append_count_buffer, 0, append_buffer_uav);
    printf("UnityComputeProbe CopyStructureCount(append_buffer)=PASS\n");
    ctx->CopyResource(append_count_staging, append_count_buffer);
    printf("UnityComputeProbe CopyResource(append_count_to_staging)=PASS\n");

    D3D11_MAPPED_SUBRESOURCE count_mapped = {};
    hr = ctx->Map(append_count_staging, 0, D3D11_MAP_READ, 0, &count_mapped);
    print_hr("UnityComputeProbe Map(append_count_staging)", hr);
    if (SUCCEEDED(hr)) {
      const UINT count = *(const UINT *)count_mapped.pData;
      printf("UnityComputeProbe append_buffer_count=%u\n", count);
      if (count != kAppendBufferElements)
        pass = false;
      ctx->Unmap(append_count_staging, 0);
    } else {
      pass = false;
    }
  } else {
    pass = false;
  }

  if (append_count_staging)
    append_count_staging->Release();
  if (append_count_buffer)
    append_count_buffer->Release();
  if (append_buffer_uav)
    append_buffer_uav->Release();
  if (append_buffer)
    append_buffer->Release();
  if (ctx1)
    ctx1->Release();
  if (constant_offset_staging)
    constant_offset_staging->Release();
  if (constant_offset_uav)
    constant_offset_uav->Release();
  if (constant_offset_output)
    constant_offset_output->Release();
  if (constant_offset_cbuffer)
    constant_offset_cbuffer->Release();
  if (groupshared_atomic_staging)
    groupshared_atomic_staging->Release();
  if (groupshared_atomic_uav)
    groupshared_atomic_uav->Release();
  if (groupshared_atomic_buffer)
    groupshared_atomic_buffer->Release();
  if (raw_buffer_staging)
    raw_buffer_staging->Release();
  if (raw_buffer_output_uav)
    raw_buffer_output_uav->Release();
  if (raw_buffer_output)
    raw_buffer_output->Release();
  if (raw_buffer_source_srv)
    raw_buffer_source_srv->Release();
  if (raw_buffer_source)
    raw_buffer_source->Release();
  if (texture3d_uav_staging)
    texture3d_uav_staging->Release();
  if (texture3d_uav_view)
    texture3d_uav_view->Release();
  if (texture3d_uav)
    texture3d_uav->Release();
  if (texture_array_uav_staging)
    texture_array_uav_staging->Release();
  if (texture_array_uav_view)
    texture_array_uav_view->Release();
  if (texture_array_uav)
    texture_array_uav->Release();
  if (texture_srv_uav_staging)
    texture_srv_uav_staging->Release();
  if (texture_srv_uav_output_uav)
    texture_srv_uav_output_uav->Release();
  if (texture_srv_uav_output)
    texture_srv_uav_output->Release();
  if (texture_srv_uav_source_srv)
    texture_srv_uav_source_srv->Release();
  if (texture_srv_uav_source)
    texture_srv_uav_source->Release();
  if (srv_uav_staging)
    srv_uav_staging->Release();
  if (srv_uav_output_uav)
    srv_uav_output_uav->Release();
  if (srv_uav_output)
    srv_uav_output->Release();
  if (srv_uav_source_srv)
    srv_uav_source_srv->Release();
  if (srv_uav_source)
    srv_uav_source->Release();
  if (multi_uav_staging1)
    multi_uav_staging1->Release();
  if (multi_uav_staging0)
    multi_uav_staging0->Release();
  if (multi_uav1)
    multi_uav1->Release();
  if (multi_uav0)
    multi_uav0->Release();
  if (multi_uav_buffer1)
    multi_uav_buffer1->Release();
  if (multi_uav_buffer0)
    multi_uav_buffer0->Release();
  if (structured_buffer_staging)
    structured_buffer_staging->Release();
  if (structured_buffer_uav)
    structured_buffer_uav->Release();
  if (structured_buffer)
    structured_buffer->Release();
  if (typed_buffer_staging)
    typed_buffer_staging->Release();
  if (typed_buffer_uav)
    typed_buffer_uav->Release();
  if (typed_buffer)
    typed_buffer->Release();
  if (dispatch_indirect_buffer)
    dispatch_indirect_buffer->Release();
  if (staging)
    staging->Release();
  if (compute_uav)
    compute_uav->Release();
  if (compute_tex)
    compute_tex->Release();
  if (structured_buffer_cs)
    structured_buffer_cs->Release();
  if (typed_buffer_cs)
    typed_buffer_cs->Release();
  if (constant_offset_cs)
    constant_offset_cs->Release();
  if (groupshared_atomic_cs)
    groupshared_atomic_cs->Release();
  if (raw_buffer_cs)
    raw_buffer_cs->Release();
  if (texture3d_uav_cs)
    texture3d_uav_cs->Release();
  if (texture_array_uav_cs)
    texture_array_uav_cs->Release();
  if (texture_srv_uav_cs)
    texture_srv_uav_cs->Release();
  if (srv_uav_cs)
    srv_uav_cs->Release();
  if (multi_uav_cs)
    multi_uav_cs->Release();
  if (cs)
    cs->Release();
  if (constant_offset_cs_blob)
    constant_offset_cs_blob->Release();
  if (groupshared_atomic_cs_blob)
    groupshared_atomic_cs_blob->Release();
  if (raw_buffer_cs_blob)
    raw_buffer_cs_blob->Release();
  if (texture3d_uav_cs_blob)
    texture3d_uav_cs_blob->Release();
  if (texture_array_uav_cs_blob)
    texture_array_uav_cs_blob->Release();
  if (texture_srv_uav_cs_blob)
    texture_srv_uav_cs_blob->Release();
  if (srv_uav_cs_blob)
    srv_uav_cs_blob->Release();
  if (multi_uav_cs_blob)
    multi_uav_cs_blob->Release();
  if (structured_buffer_cs_blob)
    structured_buffer_cs_blob->Release();
  if (typed_buffer_cs_blob)
    typed_buffer_cs_blob->Release();
  if (cs_blob)
    cs_blob->Release();

  printf("UnityComputeProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

enum ShaderCorpusStage {
  SHADER_CORPUS_VS,
  SHADER_CORPUS_PS,
  SHADER_CORPUS_CS,
};

struct ShaderCorpusCase {
  const char *label;
  const char *source;
  const char *entry;
  const char *profile;
  ShaderCorpusStage stage;
};

static uint32_t read_le32(const uint8_t *data, size_t size, size_t offset) {
  if (offset + sizeof(uint32_t) > size)
    return 0;
  return (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
         ((uint32_t)data[offset + 2] << 16) |
         ((uint32_t)data[offset + 3] << 24);
}

static UINT dxbc_shader_stage(const uint8_t *data, size_t size) {
  if (size < 36 || memcmp(data, "DXBC", 4) != 0)
    return UINT_MAX;

  UINT chunk_count = read_le32(data, size, 28);
  if (chunk_count > 256 || 32 + (size_t)chunk_count * 4 > size)
    return UINT_MAX;

  for (UINT i = 0; i < chunk_count; i++) {
    size_t chunk_offset = read_le32(data, size, 32 + (size_t)i * 4);
    if (chunk_offset + 12 > size)
      continue;
    if ((memcmp(data + chunk_offset, "SHDR", 4) != 0) &&
        (memcmp(data + chunk_offset, "SHEX", 4) != 0))
      continue;

    uint32_t version_token = read_le32(data, size, chunk_offset + 8);
    return (version_token >> 16) & 0xffff;
  }

  return UINT_MAX;
}

static const char *shader_stage_name(UINT stage) {
  switch (stage) {
  case 0:
    return "ps";
  case 1:
    return "vs";
  case 2:
    return "gs";
  case 3:
    return "hs";
  case 4:
    return "ds";
  case 5:
    return "cs";
  default:
    return "unknown";
  }
}

static bool read_binary_file(const char *path, uint8_t **data, SIZE_T *size) {
  *data = nullptr;
  *size = 0;

  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    printf("UnityRealShaderCorpusProbe read_file=%s result=FAIL gle=%lu\n",
           path, GetLastError());
    return false;
  }

  LARGE_INTEGER file_size = {};
  if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 ||
      file_size.QuadPart > 16 * 1024 * 1024) {
    printf("UnityRealShaderCorpusProbe read_file=%s size=%lld result=FAIL\n",
           path, (long long)file_size.QuadPart);
    CloseHandle(file);
    return false;
  }

  uint8_t *buffer = (uint8_t *)malloc((size_t)file_size.QuadPart);
  if (!buffer) {
    CloseHandle(file);
    return false;
  }

  DWORD bytes_read = 0;
  BOOL ok = ReadFile(file, buffer, (DWORD)file_size.QuadPart, &bytes_read,
                     nullptr);
  CloseHandle(file);

  if (!ok || bytes_read != (DWORD)file_size.QuadPart) {
    printf("UnityRealShaderCorpusProbe read_file=%s bytes=%lu expected=%lld "
           "result=FAIL\n",
           path, (unsigned long)bytes_read, (long long)file_size.QuadPart);
    free(buffer);
    return false;
  }

  *data = buffer;
  *size = (SIZE_T)file_size.QuadPart;
  return true;
}

static bool compile_fullscreen_vertex_shader(D3DCompileProc compile,
                                             ID3D11Device *device,
                                             ID3D11VertexShader **shader,
                                             ID3DBlob **shader_blob) {
  static const char fullscreen_vs_source[] =
      "struct VSIn { float2 pos : POSITION; };\n"
      "struct VSOut {\n"
      "  float4 pos : SV_POSITION;\n"
      "  float4 color0 : COLOR0;\n"
      "  float4 color1 : COLOR1;\n"
      "  float4 tex0 : TEXCOORD0;\n"
      "  float4 tex1 : TEXCOORD1;\n"
      "  float4 tex2 : TEXCOORD2;\n"
      "  float4 tex3 : TEXCOORD3;\n"
      "  float4 tex4 : TEXCOORD4;\n"
      "  float4 tex5 : TEXCOORD5;\n"
      "  float4 tex6 : TEXCOORD6;\n"
      "  float4 tex7 : TEXCOORD7;\n"
      "};\n"
      "VSOut vs_main(VSIn input) {\n"
      "  VSOut output;\n"
      "  output.pos = float4(input.pos, 0.0, 1.0);\n"
      "  output.color0 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.color1 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.tex0 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.tex1 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.tex2 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.tex3 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.tex4 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.tex5 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.tex6 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  output.tex7 = float4(0.0, 0.0, 0.0, 0.0);\n"
      "  return output;\n"
      "}\n";

  *shader = nullptr;
  *shader_blob = nullptr;
  ID3DBlob *blob = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT hr = compile(fullscreen_vs_source, sizeof(fullscreen_vs_source) - 1,
                       "hk_real_shader_fullscreen_vs", nullptr, nullptr,
                       "vs_main", "vs_4_0", 0, 0, &blob, &errors);
  print_hr("UnityRealShaderCorpusProbe D3DCompile(fullscreen_vs_4_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityRealShaderCorpusProbe fullscreen_vs_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    return false;
  }
  if (errors)
    errors->Release();

  hr = device->CreateVertexShader(blob->GetBufferPointer(),
                                  blob->GetBufferSize(), nullptr, shader);
  print_hr("UnityRealShaderCorpusProbe CreateVertexShader(fullscreen)", hr);
  if (FAILED(hr)) {
    blob->Release();
    return false;
  }

  *shader_blob = blob;
  return true;
}

static bool render_real_pixel_shader(ID3D11Device *device,
                                     ID3D11DeviceContext *ctx,
                                     ID3D11VertexShader *fullscreen_vs,
                                     ID3DBlob *fullscreen_vs_blob,
                                     ID3D11PixelShader *pixel_shader,
                                     const char *name) {
  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = 16;
  tex_desc.Height = 16;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_DEFAULT;
  tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  ID3D11Texture2D *target = nullptr;
  HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, &target);
  print_hr("UnityRealShaderCorpusProbe CreateTexture2D(render_target)", hr);
  if (FAILED(hr))
    return false;

  ID3D11RenderTargetView *rtv = nullptr;
  hr = device->CreateRenderTargetView(target, nullptr, &rtv);
  print_hr("UnityRealShaderCorpusProbe CreateRenderTargetView(render_target)",
           hr);
  if (FAILED(hr)) {
    target->Release();
    return false;
  }

  D3D11_INPUT_ELEMENT_DESC input_desc[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  ID3D11InputLayout *layout = nullptr;
  hr = device->CreateInputLayout(input_desc, ARRAYSIZE(input_desc),
                                 fullscreen_vs_blob->GetBufferPointer(),
                                 fullscreen_vs_blob->GetBufferSize(), &layout);
  print_hr("UnityRealShaderCorpusProbe CreateInputLayout(fullscreen)", hr);
  if (FAILED(hr)) {
    rtv->Release();
    target->Release();
    return false;
  }

  const FLOAT vertices[] = {
      -1.0f, -1.0f,
      -1.0f, 3.0f,
      3.0f, -1.0f,
  };
  D3D11_BUFFER_DESC vb_desc = {};
  vb_desc.ByteWidth = sizeof(vertices);
  vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
  vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA vb_data = {};
  vb_data.pSysMem = vertices;
  ID3D11Buffer *vertex_buffer = nullptr;
  hr = device->CreateBuffer(&vb_desc, &vb_data, &vertex_buffer);
  print_hr("UnityRealShaderCorpusProbe CreateBuffer(fullscreen_vertices)", hr);
  if (FAILED(hr)) {
    layout->Release();
    rtv->Release();
    target->Release();
    return false;
  }

  const FLOAT clear[4] = {1.0f, 0.25f, 0.5f, 1.0f};
  ctx->ClearRenderTargetView(rtv, clear);

  D3D11_VIEWPORT viewport = {};
  viewport.Width = (FLOAT)tex_desc.Width;
  viewport.Height = (FLOAT)tex_desc.Height;
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  D3D11_RECT scissor = {0, 0, (LONG)tex_desc.Width, (LONG)tex_desc.Height};
  const FLOAT blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ctx->RSSetState(nullptr);
  ctx->RSSetViewports(1, &viewport);
  ctx->RSSetScissorRects(1, &scissor);
  ctx->OMSetBlendState(nullptr, blend_factor, 0xffffffff);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  UINT stride = 2 * sizeof(FLOAT);
  UINT offset = 0;
  ctx->IASetInputLayout(layout);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
  ctx->VSSetShader(fullscreen_vs, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->PSSetShader(pixel_shader, nullptr, 0);
  ctx->Draw(3, 0);
  ctx->PSSetShader(nullptr, nullptr, 0);
  ctx->VSSetShader(nullptr, nullptr, 0);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);

  uint8_t bgra[4] = {};
  hr = read_first_pixel(device, ctx, target, bgra);
  print_hr("UnityRealShaderCorpusProbe Readback(render_target)", hr);
  // Gate on readback success only; log actual pixel for coverage tracking.
  // Resource-free PS shaders with zero inputs may produce any deterministic color.
  bool pixel_ok = SUCCEEDED(hr);
  printf("UnityRealShaderCorpusProbe RenderPS(%s) pixel0_bgra=%u,%u,%u,%u "
         "result=%s\n",
         name, bgra[0], bgra[1], bgra[2], bgra[3],
         pixel_ok ? "PASS" :"FAIL");

  vertex_buffer->Release();
  layout->Release();
  rtv->Release();
  target->Release();
  return pixel_ok;
}

static bool probe_unity_shader_corpus(ID3D11Device *device) {
  static const char vs_vertex_id_source[] =
      "float4 vs_main(uint vid : SV_VertexID) : SV_POSITION {\n"
      "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
      "  return float4(p[vid], 0.0, 1.0);\n"
      "}\n";
  static const char vs_instance_matrix_source[] =
      "cbuffer UnityMatrices : register(b0) {\n"
      "  float4x4 unity_ObjectToWorld;\n"
      "  float4 unity_StereoScaleOffset;\n"
      "};\n"
      "struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; uint iid : SV_InstanceID; };\n"
      "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float inst : TEXCOORD1; };\n"
      "VSOut vs_main(VSIn input) {\n"
      "  VSOut output;\n"
      "  output.pos = mul(unity_ObjectToWorld, float4(input.pos, 1.0));\n"
      "  output.uv = input.uv * unity_StereoScaleOffset.xy + unity_StereoScaleOffset.zw;\n"
      "  output.inst = (float)(input.iid & 3u);\n"
      "  return output;\n"
      "}\n";
  static const char ps_texture_array_source[] =
      "Texture2DArray<float4> unity_Lightmap : register(t0);\n"
      "SamplerState unity_Sampler : register(s0);\n"
      "float4 ps_main(float4 pos : SV_POSITION, float3 uvw : TEXCOORD0) : SV_TARGET {\n"
      "  float4 c = unity_Lightmap.SampleLevel(unity_Sampler, uvw, 0.0);\n"
      "  clip(c.a - 0.01);\n"
      "  return c.bgra;\n"
      "}\n";
  static const char ps_derivative_source[] =
      "float4 ps_main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0, bool front : SV_IsFrontFace) : SV_TARGET {\n"
      "  float2 grad = abs(float2(ddx(uv.x), ddy(uv.y)));\n"
      "  return front ? float4(grad, 0.0, 1.0) : float4(1.0 - grad, 1.0, 1.0);\n"
      "}\n";
  static const char cs_texture_intrinsics_source[] =
      "RWTexture2D<uint> unity_Out : register(u0);\n"
      "[numthreads(8, 1, 1)]\n"
      "void cs_main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID) {\n"
      "  uint v = gid.x * 8u + tid.x;\n"
      "  unity_Out[dtid.xy] = (v << 1) + countbits(v) + firstbitlow(v | 1u);\n"
      "}\n";

  static const ShaderCorpusCase cases[] = {
      {"vs_vertex_id", vs_vertex_id_source, "vs_main", "vs_5_0",
       SHADER_CORPUS_VS},
      {"vs_instance_matrix", vs_instance_matrix_source, "vs_main", "vs_5_0",
       SHADER_CORPUS_VS},
      {"ps_texture_array_clip", ps_texture_array_source, "ps_main", "ps_5_0",
       SHADER_CORPUS_PS},
      {"ps_derivative_frontface", ps_derivative_source, "ps_main", "ps_5_0",
       SHADER_CORPUS_PS},
      {"cs_texture_intrinsics", cs_texture_intrinsics_source, "cs_main", "cs_5_0",
       SHADER_CORPUS_CS},
  };

  HMODULE compiler = load_local_dll(L"d3dcompiler_47.dll", "d3dcompiler_47");
  if (!compiler)
    return false;

  auto compile = (D3DCompileProc)GetProcAddress(compiler, "D3DCompile");
  printf("UnityShaderCorpusProbe GetProcAddress(D3DCompile) proc=%p gle=%lu\n",
         compile, GetLastError());
  if (!compile)
    return false;

  bool pass = true;
  for (UINT i = 0; i < ARRAYSIZE(cases); i++) {
    const ShaderCorpusCase &test = cases[i];
    ID3DBlob *blob = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT hr = compile(test.source, strlen(test.source), test.label, nullptr,
                         nullptr, test.entry, test.profile, 0, 0, &blob,
                         &errors);
    char label[160] = {};
    snprintf(label, sizeof(label), "UnityShaderCorpusProbe D3DCompile(%s)",
             test.label);
    print_hr(label, hr);
    if (FAILED(hr)) {
      if (errors)
        printf("UnityShaderCorpusProbe %s errors=%s\n", test.label,
               (const char *)errors->GetBufferPointer());
      pass = false;
      if (errors)
        errors->Release();
      continue;
    }
    if (errors)
      errors->Release();

    switch (test.stage) {
    case SHADER_CORPUS_VS: {
      ID3D11VertexShader *shader = nullptr;
      hr = device->CreateVertexShader(blob->GetBufferPointer(),
                                      blob->GetBufferSize(), nullptr, &shader);
      snprintf(label, sizeof(label),
               "UnityShaderCorpusProbe CreateVertexShader(%s)", test.label);
      print_hr(label, hr);
      if (FAILED(hr))
        pass = false;
      if (shader)
        shader->Release();
      break;
    }
    case SHADER_CORPUS_PS: {
      ID3D11PixelShader *shader = nullptr;
      hr = device->CreatePixelShader(blob->GetBufferPointer(),
                                     blob->GetBufferSize(), nullptr, &shader);
      snprintf(label, sizeof(label),
               "UnityShaderCorpusProbe CreatePixelShader(%s)", test.label);
      print_hr(label, hr);
      if (FAILED(hr))
        pass = false;
      if (shader)
        shader->Release();
      break;
    }
    case SHADER_CORPUS_CS: {
      ID3D11ComputeShader *shader = nullptr;
      hr = device->CreateComputeShader(blob->GetBufferPointer(),
                                       blob->GetBufferSize(), nullptr, &shader);
      snprintf(label, sizeof(label),
               "UnityShaderCorpusProbe CreateComputeShader(%s)", test.label);
      print_hr(label, hr);
      if (FAILED(hr))
        pass = false;
      if (shader)
        shader->Release();
      break;
    }
    }
    blob->Release();
  }

  printf("UnityShaderCorpusProbe cases=%u result=%s\n",
         (unsigned)ARRAYSIZE(cases), pass ? "PASS" : "FAIL");
  return pass;
}

// Generate a VS that matches the PS input signature (by reflecting the PS ISGN),
// uses SV_VertexID for a fullscreen triangle (no VBO/CB), and outputs zeros for
// all interpolants. This guarantees PSO attribute type compatibility while still
// invoking the real PS body for readback.
static bool render_hk_gen_vs_ps(ID3D11Device *device, ID3D11DeviceContext *ctx,
                                  D3DCompileProc compile_fn,
                                  D3DReflectProc reflect_fn,
                                  const uint8_t *ps_bytes, SIZE_T ps_size,
                                  ID3D11PixelShader *ps, const char *ps_name) {
  ID3D11ShaderReflection *ps_refl = nullptr;
  HRESULT hr = reflect_fn(ps_bytes, ps_size, kIIDID3D11ShaderReflection,
                           (void **)&ps_refl);
  if (FAILED(hr) || !ps_refl) {
    printf("UnityRealShaderCorpusProbe GenRender=%s ps_reflect_fail\n", ps_name);
    return false;
  }
  D3D11_SHADER_DESC ps_desc = {};
  ps_refl->GetDesc(&ps_desc);

  // Build VS struct and body from PS input parameters.
  char struct_body[2048] = "    float4 sv_pos : SV_POSITION;\n";
  char init_body[2048] = "    o.sv_pos = float4(p[vid], 0.f, 1.f);\n";
  for (UINT i = 0; i < ps_desc.InputParameters; i++) {
    D3D11_SIGNATURE_PARAMETER_DESC pd = {};
    ps_refl->GetInputParameterDesc(i, &pd);
    if (pd.SystemValueType != D3D_NAME_UNDEFINED) continue;
    // Always use float4: Metal airconv emits float4 for all PS fragment inputs
    // regardless of the DXBC mask. Using the mask-derived type (float2/float3)
    // causes a VS-PS attribute type mismatch in Metal PSO compilation.
    char sb[256], rb[256];
    snprintf(sb, sizeof(sb), "    float4 a%u : %s%u;\n", i, pd.SemanticName, pd.SemanticIndex);
    snprintf(rb, sizeof(rb), "    o.a%u = float4(0,0,0,0);\n", i);
    strncat(struct_body, sb, sizeof(struct_body) - strlen(struct_body) - 1);
    strncat(init_body, rb, sizeof(init_body) - strlen(init_body) - 1);
  }
  ps_refl->Release();

  char vs_src[6144];
  snprintf(vs_src, sizeof(vs_src),
           "struct O { %s};\n"
           "O vs_main(uint vid : SV_VertexID) {\n"
           "  float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
           "  O o;\n%s  return o;\n}\n",
           struct_body, init_body);

  ID3DBlob *vs_blob = nullptr, *err_blob = nullptr;
  hr = compile_fn(vs_src, strlen(vs_src), "gen_vs", nullptr, nullptr,
                  "vs_main", "vs_5_0", 0, 0, &vs_blob, &err_blob);
  if (err_blob) { err_blob->Release(); err_blob = nullptr; }
  if (FAILED(hr) || !vs_blob) {
    printf("UnityRealShaderCorpusProbe GenRender=%s vs_compile_fail hr=0x%08lx\n",
           ps_name, (unsigned long)(uint32_t)hr);
    return false;
  }

  ID3D11VertexShader *gen_vs = nullptr;
  hr = device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                   vs_blob->GetBufferSize(), nullptr, &gen_vs);
  vs_blob->Release();
  if (FAILED(hr) || !gen_vs) {
    printf("UnityRealShaderCorpusProbe GenRender=%s vs_create_fail hr=0x%08lx\n",
           ps_name, (unsigned long)(uint32_t)hr);
    return false;
  }
  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = 4; tex_desc.Height = 4; tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1; tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  tex_desc.SampleDesc.Count = 1; tex_desc.Usage = D3D11_USAGE_DEFAULT;
  tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *target = nullptr;
  hr = device->CreateTexture2D(&tex_desc, nullptr, &target);
  if (FAILED(hr)) { gen_vs->Release(); return false; }
  ID3D11RenderTargetView *rtv = nullptr;
  hr = device->CreateRenderTargetView(target, nullptr, &rtv);
  if (FAILED(hr)) { gen_vs->Release(); target->Release(); return false; }

  const FLOAT clear[4] = {0.5f, 0.25f, 0.75f, 1.0f};
  ctx->ClearRenderTargetView(rtv, clear);
  D3D11_VIEWPORT vp = {0, 0, 4, 4, 0, 1.0f};
  D3D11_RECT scissor = {0, 0, 4, 4};
  ctx->RSSetState(nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetScissorRects(1, &scissor);
  const FLOAT bf[4] = {};
  ctx->OMSetBlendState(nullptr, bf, 0xffffffff);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);
  ctx->VSSetShader(gen_vs, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->PSSetShader(ps, nullptr, 0);
  ctx->Draw(3, 0);
  ctx->PSSetShader(nullptr, nullptr, 0);
  ctx->VSSetShader(nullptr, nullptr, 0);
  ctx->OMSetRenderTargets(0, nullptr, nullptr);

  uint8_t bgra[4] = {};
  hr = read_first_pixel(device, ctx, target, bgra);
  print_hr("UnityRealShaderCorpusProbe GenRender Readback(render_target)", hr);
  bool pixel_ok = SUCCEEDED(hr);
  printf("UnityRealShaderCorpusProbe GenRender=%s pixel0_bgra=%u,%u,%u,%u "
         "result=%s\n",
         ps_name, bgra[0], bgra[1], bgra[2], bgra[3],
         pixel_ok ? "PASS" : "FAIL");
  fflush(stdout);

  rtv->Release(); target->Release(); gen_vs->Release();
  return pixel_ok;
}

static bool probe_unity_real_shader_corpus(ID3D11Device *device,
                                           ID3D11DeviceContext *ctx) {
  char blob_dir[512] = {};
  DWORD dir_len =
      GetEnvironmentVariableA("DXMT_HK_SHADER_BLOB_DIR", blob_dir,
                              ARRAYSIZE(blob_dir));
  if (!dir_len) {
    printf("UnityRealShaderCorpusProbe result=SKIP reason=no_blob_dir\n");
    return true;
  }
  if (dir_len >= ARRAYSIZE(blob_dir)) {
    printf("UnityRealShaderCorpusProbe result=FAIL reason=blob_dir_too_long\n");
    return false;
  }

  UINT corpus_limit = read_env_uint("DXMT_HK_SHADER_CORPUS_LIMIT", 256, 1, 4096);
  UINT render_limit = read_env_uint("DXMT_HK_SHADER_RENDER_LIMIT", 2, 0, 256);
  char pattern[640] = {};
  size_t dir_size = strlen(blob_dir);
  char sep = strchr(blob_dir, '\\') ? '\\' : '/';
  snprintf(pattern, sizeof(pattern), "%s%c*.dxbc", blob_dir,
           dir_size && (blob_dir[dir_size - 1] == '\\' ||
                        blob_dir[dir_size - 1] == '/')
               ? '\0'
               : sep);
  if (dir_size && (blob_dir[dir_size - 1] == '\\' ||
                   blob_dir[dir_size - 1] == '/'))
    snprintf(pattern, sizeof(pattern), "%s*.dxbc", blob_dir);

  HMODULE compiler = load_local_dll(L"d3dcompiler_47.dll", "d3dcompiler_47");
  if (!compiler)
    return false;

  auto compile = (D3DCompileProc)GetProcAddress(compiler, "D3DCompile");
  auto reflect = (D3DReflectProc)GetProcAddress(compiler, "D3DReflect");
  printf("UnityRealShaderCorpusProbe blob_dir=%s corpus_limit=%u "
         "render_limit=%u D3DCompile=%p D3DReflect=%p\n",
         blob_dir, corpus_limit, render_limit, compile, reflect);
  if (!compile || !reflect)
    return false;

  ID3D11VertexShader *fullscreen_vs = nullptr;
  ID3DBlob *fullscreen_vs_blob = nullptr;
  if (render_limit && !compile_fullscreen_vertex_shader(compile, device,
                                                        &fullscreen_vs,
                                                        &fullscreen_vs_blob))
    return false;

  UINT paired_render_limit =
      read_env_uint("DXMT_HK_SHADER_PAIRED_RENDER_LIMIT", 256, 0, 256);

  WIN32_FIND_DATAA find_data = {};
  HANDLE find = FindFirstFileA(pattern, &find_data);
  if (find == INVALID_HANDLE_VALUE) {
    printf("UnityRealShaderCorpusProbe result=FAIL reason=no_dxbc pattern=%s "
           "gle=%lu\n",
           pattern, GetLastError());
    if (fullscreen_vs_blob)
      fullscreen_vs_blob->Release();
    if (fullscreen_vs)
      fullscreen_vs->Release();
    return false;
  }

  bool pass = true;
  UINT total = 0;
  UINT created = 0;
  UINT reflected = 0;
  UINT vs_count = 0;
  UINT ps_count = 0;
  UINT gs_count = 0;
  UINT hs_count = 0;
  UINT ds_count = 0;
  UINT cs_count = 0;
  UINT rendered = 0;
  UINT rendered_paired = 0;

  do {
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    if (total >= corpus_limit)
      break;

    char file_path[768] = {};
    if (dir_size && (blob_dir[dir_size - 1] == '\\' ||
                     blob_dir[dir_size - 1] == '/'))
      snprintf(file_path, sizeof(file_path), "%s%s", blob_dir,
               find_data.cFileName);
    else
      snprintf(file_path, sizeof(file_path), "%s%c%s", blob_dir, sep,
               find_data.cFileName);

    uint8_t *bytes = nullptr;
    SIZE_T byte_size = 0;
    total++;
    if (!read_binary_file(file_path, &bytes, &byte_size)) {
      pass = false;
      continue;
    }

    UINT stage = dxbc_shader_stage(bytes, byte_size);
    ID3D11ShaderReflection *reflection = nullptr;
    D3D11_SHADER_DESC shader_desc = {};
    HRESULT hr = reflect(bytes, byte_size, kIIDID3D11ShaderReflection,
                         (void **)&reflection);
    if (SUCCEEDED(hr) && reflection &&
        SUCCEEDED(reflection->GetDesc(&shader_desc))) {
      reflected++;
    } else {
      printf("UnityRealShaderCorpusProbe blob=%s D3DReflect hr=0x%08lx\n",
             find_data.cFileName, (unsigned long)(uint32_t)hr);
      pass = false;
    }

    switch (stage) {
    case 0: {
      ps_count++;
      ID3D11PixelShader *ps = nullptr;
      hr = device->CreatePixelShader(bytes, byte_size, nullptr, &ps);
      if (SUCCEEDED(hr)) {
        created++;
        // Render all PS shaders that have at least 1 output and no CB/texture
        // Resource-free gate: only size=300 (zero-output) and size=460 (fract-noise)
        // have argument_buffer_struct-free MSL output (confirmed by scanning 128 HK .ll
        // files). All other PS shaders fail PSO creation silently (Metal nil+nil) because
        // our synthetic fullscreen VS is incompatible with HK PS MSL attribute layout.
        // HK VS shaders use integer SV_VertexID/InstanceID inputs + argument_buffer CBs;
        // pairing each HK PS with its matching HK VS (same DXBC path+subprogram, pt15)
        // is required to reach rendered > 2. — LANE-D-PROGRESS.md §PSO-ATTR-MISMATCH.
        bool renderable_ps =
            (byte_size == 300 || byte_size == 460) &&
            shader_desc.OutputParameters >= 1;
        if (fullscreen_vs && rendered < render_limit && renderable_ps) {
          printf("UnityRealShaderCorpusProbe render_candidate=%s "
                 "reference=zero_output_ps\n",
                 find_data.cFileName);
          if (render_real_pixel_shader(device, ctx, fullscreen_vs,
                                       fullscreen_vs_blob, ps,
                                       find_data.cFileName))
            rendered++;
          else
            pass = false;
        }
        // Gen-VS render: compile a VS from the PS ISGN (SV_VertexID fullscreen,
        // zero interpolants). This verifies PSO creation, draw submission, and
        // readback for texture/sampler argument-buffer PS shaders.
        if (paired_render_limit && rendered_paired < paired_render_limit) {
          if (render_hk_gen_vs_ps(device, ctx, compile, reflect,
                                   bytes, byte_size, ps, find_data.cFileName))
            rendered_paired++;
        }
      }
      printf("UnityRealShaderCorpusProbe blob=%s stage=%s size=%llu "
             "inputs=%u outputs=%u resources=%u instructions=%u "
             "create_hr=0x%08lx\n",
             find_data.cFileName, shader_stage_name(stage),
             (unsigned long long)byte_size, shader_desc.InputParameters,
             shader_desc.OutputParameters, shader_desc.BoundResources,
             shader_desc.InstructionCount, (unsigned long)(uint32_t)hr);
      if (FAILED(hr))
        pass = false;
      if (ps)
        ps->Release();
      break;
    }
    case 1: {
      vs_count++;
      ID3D11VertexShader *vs = nullptr;
      hr = device->CreateVertexShader(bytes, byte_size, nullptr, &vs);
      printf("UnityRealShaderCorpusProbe blob=%s stage=%s size=%llu "
             "inputs=%u outputs=%u resources=%u instructions=%u "
             "create_hr=0x%08lx\n",
             find_data.cFileName, shader_stage_name(stage),
             (unsigned long long)byte_size, shader_desc.InputParameters,
             shader_desc.OutputParameters, shader_desc.BoundResources,
             shader_desc.InstructionCount, (unsigned long)(uint32_t)hr);
      if (SUCCEEDED(hr))
        created++;
      else
        pass = false;
      if (vs)
        vs->Release();
      break;
    }
    case 2: {
      gs_count++;
      ID3D11GeometryShader *gs = nullptr;
      hr = device->CreateGeometryShader(bytes, byte_size, nullptr, &gs);
      printf("UnityRealShaderCorpusProbe blob=%s stage=%s size=%llu "
             "inputs=%u outputs=%u resources=%u instructions=%u "
             "create_hr=0x%08lx\n",
             find_data.cFileName, shader_stage_name(stage),
             (unsigned long long)byte_size, shader_desc.InputParameters,
             shader_desc.OutputParameters, shader_desc.BoundResources,
             shader_desc.InstructionCount, (unsigned long)(uint32_t)hr);
      if (SUCCEEDED(hr))
        created++;
      else
        pass = false;
      if (gs)
        gs->Release();
      break;
    }
    case 3: {
      hs_count++;
      ID3D11HullShader *hs = nullptr;
      hr = device->CreateHullShader(bytes, byte_size, nullptr, &hs);
      printf("UnityRealShaderCorpusProbe blob=%s stage=%s size=%llu "
             "inputs=%u outputs=%u resources=%u instructions=%u "
             "create_hr=0x%08lx\n",
             find_data.cFileName, shader_stage_name(stage),
             (unsigned long long)byte_size, shader_desc.InputParameters,
             shader_desc.OutputParameters, shader_desc.BoundResources,
             shader_desc.InstructionCount, (unsigned long)(uint32_t)hr);
      if (SUCCEEDED(hr))
        created++;
      else
        pass = false;
      if (hs)
        hs->Release();
      break;
    }
    case 4: {
      ds_count++;
      ID3D11DomainShader *ds = nullptr;
      hr = device->CreateDomainShader(bytes, byte_size, nullptr, &ds);
      printf("UnityRealShaderCorpusProbe blob=%s stage=%s size=%llu "
             "inputs=%u outputs=%u resources=%u instructions=%u "
             "create_hr=0x%08lx\n",
             find_data.cFileName, shader_stage_name(stage),
             (unsigned long long)byte_size, shader_desc.InputParameters,
             shader_desc.OutputParameters, shader_desc.BoundResources,
             shader_desc.InstructionCount, (unsigned long)(uint32_t)hr);
      if (SUCCEEDED(hr))
        created++;
      else
        pass = false;
      if (ds)
        ds->Release();
      break;
    }
    case 5: {
      cs_count++;
      ID3D11ComputeShader *cs = nullptr;
      hr = device->CreateComputeShader(bytes, byte_size, nullptr, &cs);
      printf("UnityRealShaderCorpusProbe blob=%s stage=%s size=%llu "
             "inputs=%u outputs=%u resources=%u instructions=%u "
             "create_hr=0x%08lx\n",
             find_data.cFileName, shader_stage_name(stage),
             (unsigned long long)byte_size, shader_desc.InputParameters,
             shader_desc.OutputParameters, shader_desc.BoundResources,
             shader_desc.InstructionCount, (unsigned long)(uint32_t)hr);
      if (SUCCEEDED(hr))
        created++;
      else
        pass = false;
      if (cs)
        cs->Release();
      break;
    }
    default:
      printf("UnityRealShaderCorpusProbe blob=%s stage=unknown size=%llu "
             "result=FAIL\n",
             find_data.cFileName, (unsigned long long)byte_size);
      pass = false;
      break;
    }

    if (reflection)
      reflection->Release();
    free(bytes);
  } while (FindNextFileA(find, &find_data));

  FindClose(find);
  if (fullscreen_vs_blob)
    fullscreen_vs_blob->Release();
  if (fullscreen_vs)
    fullscreen_vs->Release();

  if (!total || created != total || reflected != total)
    pass = false;
  if (render_limit && !rendered)
    pass = false;
  if (paired_render_limit && ps_count && !rendered_paired)
    pass = false;

  printf("UnityRealShaderCorpusProbe summary total=%u reflected=%u created=%u "
         "vs=%u ps=%u gs=%u hs=%u ds=%u cs=%u rendered=%u rendered_paired=%u "
         "result=%s\n",
         total, reflected, created, vs_count, ps_count, gs_count, hs_count,
         ds_count, cs_count, rendered, rendered_paired, pass ? "PASS" : "FAIL");
  printf("UnityRealShaderCorpusProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_tessellation_shaders(ID3D11Device *device,
                                             ID3D11DeviceContext *ctx) {
  static const char tess_source[] =
      "struct CP { float3 pos : POSITION; };\n"
      "struct HSConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
      "CP vs_main(float3 pos : POSITION) {\n"
      "  CP output;\n"
      "  output.pos = pos;\n"
      "  return output;\n"
      "}\n"
      "HSConst hs_const(InputPatch<CP, 3> patch, uint patch_id : SV_PrimitiveID) {\n"
      "  HSConst output;\n"
      "  output.edges[0] = 1.0;\n"
      "  output.edges[1] = 1.0;\n"
      "  output.edges[2] = 1.0;\n"
      "  output.inside = 1.0;\n"
      "  return output;\n"
      "}\n"
      "[domain(\"tri\")]\n"
      "[partitioning(\"integer\")]\n"
      "[outputtopology(\"triangle_cw\")]\n"
      "[outputcontrolpoints(3)]\n"
      "[patchconstantfunc(\"hs_const\")]\n"
      "CP hs_main(InputPatch<CP, 3> patch, uint cp_id : SV_OutputControlPointID, uint patch_id : SV_PrimitiveID) {\n"
      "  return patch[cp_id];\n"
      "}\n"
      "[domain(\"tri\")]\n"
      "float4 ds_main(HSConst constants, const OutputPatch<CP, 3> patch, float3 bary : SV_DomainLocation) : SV_POSITION {\n"
      "  float3 pos = patch[0].pos * bary.x + patch[1].pos * bary.y + patch[2].pos * bary.z;\n"
      "  return float4(pos.xy, 0.0, 1.0);\n"
      "}\n"
      "float4 ps_main(float4 pos : SV_POSITION) : SV_TARGET {\n"
      "  return float4(0.0, 1.0, 0.0, 1.0);\n"
      "}\n";

  HMODULE compiler = load_local_dll(L"d3dcompiler_47.dll", "d3dcompiler_47");
  if (!compiler)
    return false;

  auto compile = (D3DCompileProc)GetProcAddress(compiler, "D3DCompile");
  printf("UnityTessellationProbe GetProcAddress(D3DCompile) proc=%p gle=%lu\n",
         compile, GetLastError());
  if (!compile)
    return false;

  bool pass = true;
  ID3D11ClassLinkage *class_linkage = nullptr;
  HRESULT hr = device->CreateClassLinkage(&class_linkage);
  print_hr("UnityTessellationProbe CreateClassLinkage", hr);
  if (FAILED(hr))
    pass = false;

  if (class_linkage) {
    ID3D11ClassInstance *created_instance = nullptr;
    hr = class_linkage->CreateClassInstance("LaneDMaterial", 2, 3, 4, 5,
                                            &created_instance);
    print_hr("UnityClassLinkageProbe CreateClassInstance", hr);
    if (FAILED(hr))
      pass = false;

    if (created_instance) {
      D3D11_CLASS_INSTANCE_DESC desc = {};
      created_instance->GetDesc(&desc);
      printf("UnityClassLinkageProbe created_desc type=%u cb=%u vec=%u "
             "tex=%u sampler=%u created=%u\n",
             desc.TypeId, desc.ConstantBuffer, desc.BaseConstantBufferOffset,
             desc.BaseTexture, desc.BaseSampler, desc.Created);

      char type_name[64] = {};
      SIZE_T type_required = 0;
      created_instance->GetTypeName(nullptr, &type_required);
      SIZE_T type_buffer = ARRAYSIZE(type_name);
      created_instance->GetTypeName(type_name, &type_buffer);
      printf("UnityClassLinkageProbe created_type name=%s required=%llu "
             "buffer=%llu\n",
             type_name, (unsigned long long)type_required,
             (unsigned long long)type_buffer);

      char instance_name[64] = {};
      SIZE_T instance_buffer = ARRAYSIZE(instance_name);
      created_instance->GetInstanceName(instance_name, &instance_buffer);
      printf("UnityClassLinkageProbe created_instance name=%s buffer=%llu\n",
             instance_name, (unsigned long long)instance_buffer);

      ID3D11ClassLinkage *roundtrip_linkage = nullptr;
      created_instance->GetClassLinkage(&roundtrip_linkage);
      printf("UnityClassLinkageProbe linkage_roundtrip=%d\n",
             roundtrip_linkage == class_linkage);

      if (!desc.Created || !desc.TypeId || desc.ConstantBuffer != 2 ||
          desc.BaseConstantBufferOffset != 3 || desc.BaseTexture != 4 ||
          desc.BaseSampler != 5 || strcmp(type_name, "LaneDMaterial") != 0 ||
          type_required != strlen("LaneDMaterial") + 1 ||
          instance_buffer != 1 || instance_name[0] != '\0' ||
          roundtrip_linkage != class_linkage)
        pass = false;

      if (roundtrip_linkage)
        roundtrip_linkage->Release();

      created_instance->Release();
    }

    ID3D11ClassInstance *named_instance = nullptr;
    hr = class_linkage->GetClassInstance("LaneDInstance", 7, &named_instance);
    print_hr("UnityClassLinkageProbe GetClassInstance", hr);
    if (FAILED(hr))
      pass = false;

    if (named_instance) {
      D3D11_CLASS_INSTANCE_DESC desc = {};
      named_instance->GetDesc(&desc);

      char instance_name[64] = {};
      SIZE_T instance_required = 0;
      named_instance->GetInstanceName(nullptr, &instance_required);
      SIZE_T instance_buffer = ARRAYSIZE(instance_name);
      named_instance->GetInstanceName(instance_name, &instance_buffer);

      char type_name[16] = {};
      SIZE_T type_buffer = ARRAYSIZE(type_name);
      named_instance->GetTypeName(type_name, &type_buffer);

      printf("UnityClassLinkageProbe named_desc index=%u tex=%u sampler=%u "
             "created=%u\n",
             desc.InstanceIndex, desc.BaseTexture, desc.BaseSampler,
             desc.Created);
      printf("UnityClassLinkageProbe named_instance name=%s required=%llu "
             "type_buffer=%llu\n",
             instance_name, (unsigned long long)instance_required,
             (unsigned long long)type_buffer);

      if (desc.Created || desc.InstanceIndex != 7 || desc.BaseTexture != 127 ||
          desc.BaseSampler != 15 || strcmp(instance_name, "LaneDInstance") != 0 ||
          instance_required != strlen("LaneDInstance") + 1 ||
          type_buffer != 1 || type_name[0] != '\0')
        pass = false;

      named_instance->Release();
    }
  }

  ID3DBlob *vs_blob = nullptr;
  ID3DBlob *hs_blob = nullptr;
  ID3DBlob *ds_blob = nullptr;
  ID3DBlob *ps_blob = nullptr;
  ID3DBlob *errors = nullptr;
  hr = compile(tess_source, sizeof(tess_source) - 1,
               "dxmt_headless_unity_tessellation", nullptr, nullptr, "hs_main",
               "hs_5_0", 0, 0, &hs_blob, &errors);
  print_hr("UnityTessellationProbe D3DCompile(hs_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityTessellationProbe hs_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    return false;
  }
  if (errors) {
    errors->Release();
    errors = nullptr;
  }

  hr = compile(tess_source, sizeof(tess_source) - 1,
               "dxmt_headless_unity_tessellation", nullptr, nullptr,
               "ds_main", "ds_5_0", 0, 0, &ds_blob, &errors);
  print_hr("UnityTessellationProbe D3DCompile(ds_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityTessellationProbe ds_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    if (errors)
      errors->Release();
    hs_blob->Release();
    return false;
  }
  if (errors)
    errors->Release();

  hr = compile(tess_source, sizeof(tess_source) - 1,
               "dxmt_headless_unity_tessellation", nullptr, nullptr,
               "vs_main", "vs_5_0", 0, 0, &vs_blob, &errors);
  print_hr("UnityTessellationProbe D3DCompile(vs_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityTessellationProbe vs_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    pass = false;
  }
  if (errors) {
    errors->Release();
    errors = nullptr;
  }

  hr = compile(tess_source, sizeof(tess_source) - 1,
               "dxmt_headless_unity_tessellation", nullptr, nullptr,
               "ps_main", "ps_5_0", 0, 0, &ps_blob, &errors);
  print_hr("UnityTessellationProbe D3DCompile(ps_5_0)", hr);
  if (FAILED(hr)) {
    if (errors)
      printf("UnityTessellationProbe ps_errors=%s\n",
             (const char *)errors->GetBufferPointer());
    pass = false;
  }
  if (errors) {
    errors->Release();
    errors = nullptr;
  }

  ID3D11VertexShader *vs = nullptr;
  if (vs_blob) {
    hr = device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                    vs_blob->GetBufferSize(), nullptr, &vs);
    print_hr("UnityTessellationProbe CreateVertexShader", hr);
    if (FAILED(hr))
      pass = false;
  }

  ID3D11HullShader *hs = nullptr;
  hr = device->CreateHullShader(hs_blob->GetBufferPointer(),
                                hs_blob->GetBufferSize(), nullptr, &hs);
  print_hr("UnityTessellationProbe CreateHullShader", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11DomainShader *ds = nullptr;
  hr = device->CreateDomainShader(ds_blob->GetBufferPointer(),
                                  ds_blob->GetBufferSize(), nullptr, &ds);
  print_hr("UnityTessellationProbe CreateDomainShader", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11PixelShader *ps = nullptr;
  if (ps_blob) {
    hr = device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                   ps_blob->GetBufferSize(), nullptr, &ps);
    print_hr("UnityTessellationProbe CreatePixelShader", hr);
    if (FAILED(hr))
      pass = false;
  }

  if (hs && ds) {
    ctx->HSSetShader(hs, nullptr, 0);
    ctx->DSSetShader(ds, nullptr, 0);
    printf("UnityTessellationProbe HSSetShader=PASS\n");
    printf("UnityTessellationProbe DSSetShader=PASS\n");
  } else {
    pass = false;
  }

  if (vs && hs && ds && ps && vs_blob) {
    D3D11_INPUT_ELEMENT_DESC input_desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout *input_layout = nullptr;
    hr = device->CreateInputLayout(input_desc, ARRAYSIZE(input_desc),
                                   vs_blob->GetBufferPointer(),
                                   vs_blob->GetBufferSize(), &input_layout);
    print_hr("UnityTessellationProbe CreateInputLayout", hr);
    if (FAILED(hr))
      pass = false;

    struct TessVertex {
      float pos[3];
    };
    TessVertex vertices[] = {
        {{-1.0f, -1.0f, 0.0f}},
        {{-1.0f, 3.0f, 0.0f}},
        {{3.0f, -1.0f, 0.0f}},
    };
    D3D11_SUBRESOURCE_DATA vertex_data = {};
    vertex_data.pSysMem = vertices;
    D3D11_BUFFER_DESC vertex_desc = {};
    vertex_desc.ByteWidth = sizeof(vertices);
    vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ID3D11Buffer *vertex_buffer = nullptr;
    hr = device->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer);
    print_hr("UnityTessellationProbe CreateBuffer(patch_vertices)", hr);
    if (FAILED(hr))
      pass = false;

    ID3D11Texture2D *target = nullptr;
    hr = create_offscreen_target(device, &target);
    print_hr("UnityTessellationProbe CreateTexture2D(target)", hr);
    if (FAILED(hr))
      pass = false;

    ID3D11RenderTargetView *rtv = nullptr;
    if (target) {
      hr = device->CreateRenderTargetView(target, nullptr, &rtv);
      print_hr("UnityTessellationProbe CreateRenderTargetView", hr);
      if (FAILED(hr))
        pass = false;
    }

    D3D11_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    ID3D11RasterizerState *raster_state = nullptr;
    hr = device->CreateRasterizerState(&raster_desc, &raster_state);
    print_hr("UnityTessellationProbe CreateRasterizerState(no_cull)", hr);
    if (FAILED(hr))
      pass = false;

    if (input_layout && vertex_buffer && rtv && raster_state) {
      D3D11_VIEWPORT viewport = {0.0f, 0.0f, (FLOAT)kWidth, (FLOAT)kHeight,
                                 0.0f, 1.0f};
      UINT stride = sizeof(vertices[0]);
      UINT offset = 0;
      const FLOAT clear_color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
      ctx->IASetInputLayout(input_layout);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
      ctx->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
      ctx->VSSetShader(vs, nullptr, 0);
      ctx->HSSetShader(hs, nullptr, 0);
      ctx->DSSetShader(ds, nullptr, 0);
      ctx->GSSetShader(nullptr, nullptr, 0);
      ctx->PSSetShader(ps, nullptr, 0);
      ctx->RSSetState(raster_state);
      ctx->RSSetViewports(1, &viewport);
      ctx->OMSetRenderTargets(1, &rtv, nullptr);
      ctx->ClearRenderTargetView(rtv, clear_color);
      printf("UnityTessellationProbe ClearRenderTargetView=PASS\n");
      ctx->Draw(3, 0);
      ctx->Flush();
      printf("UnityTessellationProbe DrawPatch(3)=PASS\n");

      uint8_t pixel[4] = {};
      hr = read_first_pixel(device, ctx, target, pixel);
      print_hr("UnityTessellationProbe Readback", hr);
      printf("UnityTessellationProbe pixel0_bgra=%u,%u,%u,%u\n", pixel[0],
             pixel[1], pixel[2], pixel[3]);
      if (FAILED(hr) || pixel[0] > 2 || pixel[1] < 235 || pixel[2] > 2 ||
          pixel[3] != 255)
        pass = false;
    } else {
      pass = false;
    }

    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->VSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (raster_state)
      raster_state->Release();
    if (rtv)
      rtv->Release();
    if (target)
      target->Release();
    if (vertex_buffer)
      vertex_buffer->Release();
    if (input_layout)
      input_layout->Release();
  }

  if (ps)
    ps->Release();
  if (ds)
    ds->Release();
  if (hs)
    hs->Release();
  if (vs)
    vs->Release();
  if (class_linkage)
    class_linkage->Release();
  if (ps_blob)
    ps_blob->Release();
  if (ds_blob)
    ds_blob->Release();
  if (hs_blob)
    hs_blob->Release();
  if (vs_blob)
    vs_blob->Release();

  printf("UnityTessellationProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_queries_and_deferred(ID3D11Device *device,
                                             ID3D11DeviceContext *ctx) {
  bool pass = true;

  D3D11_QUERY_DESC event_desc = {};
  event_desc.Query = D3D11_QUERY_EVENT;
  ID3D11Query *event_query = nullptr;
  HRESULT hr = device->CreateQuery(&event_desc, &event_query);
  print_hr("UnityQueryProbe CreateQuery(EVENT)", hr);
  if (FAILED(hr))
    pass = false;
  if (event_query) {
    ctx->End(event_query);
    ctx->Flush();
    for (UINT attempt = 0; attempt < 64; attempt++) {
      hr = ctx->GetData(event_query, nullptr, 0, 0);
      if (hr != S_FALSE)
        break;
      Sleep(1);
    }
    print_hr("UnityQueryProbe GetData(EVENT)", hr);
    if (hr != S_OK)
      pass = false;
  }

  D3D11_QUERY_DESC timestamp_desc = {};
  timestamp_desc.Query = D3D11_QUERY_TIMESTAMP;
  ID3D11Query *timestamp_query = nullptr;
  hr = device->CreateQuery(&timestamp_desc, &timestamp_query);
  print_hr("UnityQueryProbe CreateQuery(TIMESTAMP)", hr);
  if (FAILED(hr))
    pass = false;
  if (timestamp_query) {
    ctx->End(timestamp_query);
    ctx->Flush();
    UINT64 timestamp_value = 0;
    for (UINT attempt = 0; attempt < 64; attempt++) {
      hr = ctx->GetData(timestamp_query, &timestamp_value,
                        sizeof(timestamp_value), 0);
      if (hr != S_FALSE)
        break;
      Sleep(1);
    }
    print_hr("UnityQueryProbe GetData(TIMESTAMP)", hr);
    printf("UnityQueryProbe timestamp_value=%llu\n",
           (unsigned long long)timestamp_value);
    if (hr != S_OK)
      pass = false;
  }

  D3D11_QUERY_DESC disjoint_desc = {};
  disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
  ID3D11Query *disjoint_query = nullptr;
  hr = device->CreateQuery(&disjoint_desc, &disjoint_query);
  print_hr("UnityQueryProbe CreateQuery(TIMESTAMP_DISJOINT)", hr);
  if (FAILED(hr))
    pass = false;
  if (disjoint_query) {
    ctx->Begin(disjoint_query);
    ctx->End(disjoint_query);
    ctx->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint_data = {};
    for (UINT attempt = 0; attempt < 64; attempt++) {
      hr = ctx->GetData(disjoint_query, &disjoint_data,
                        sizeof(disjoint_data), 0);
      if (hr != S_FALSE)
        break;
      Sleep(1);
    }
    print_hr("UnityQueryProbe GetData(TIMESTAMP_DISJOINT)", hr);
    printf("UnityQueryProbe disjoint_frequency=%llu disjoint=%u\n",
           (unsigned long long)disjoint_data.Frequency,
           (unsigned)disjoint_data.Disjoint);
    if (hr != S_OK || disjoint_data.Frequency == 0 ||
        disjoint_data.Disjoint)
      pass = false;
  }

  D3D11_QUERY_DESC occlusion_desc = {};
  occlusion_desc.Query = D3D11_QUERY_OCCLUSION;
  ID3D11Query *occlusion_query = nullptr;
  hr = device->CreateQuery(&occlusion_desc, &occlusion_query);
  print_hr("UnityQueryProbe CreateQuery(OCCLUSION)", hr);
  if (FAILED(hr))
    pass = false;
  if (occlusion_query) {
    ctx->Begin(occlusion_query);
    ctx->End(occlusion_query);
    ctx->Flush();
    UINT64 samples = 0;
    for (UINT attempt = 0; attempt < 64; attempt++) {
      hr = ctx->GetData(occlusion_query, &samples, sizeof(samples), 0);
      if (hr != S_FALSE)
        break;
      Sleep(1);
    }
    print_hr("UnityQueryProbe GetData(OCCLUSION)", hr);
    printf("UnityQueryProbe occlusion_samples=%llu\n",
           (unsigned long long)samples);
    if (hr != S_OK)
      pass = false;
  }

  D3D11_QUERY_DESC pipeline_desc = {};
  pipeline_desc.Query = D3D11_QUERY_PIPELINE_STATISTICS;
  ID3D11Query *pipeline_query = nullptr;
  hr = device->CreateQuery(&pipeline_desc, &pipeline_query);
  print_hr("UnityQueryProbe CreateQuery(PIPELINE_STATISTICS)", hr);
  if (FAILED(hr))
    pass = false;
  if (pipeline_query) {
    ctx->Begin(pipeline_query);
    D3D11_QUERY_DATA_PIPELINE_STATISTICS pending_stats = {};
    hr = ctx->GetData(pipeline_query, &pending_stats, sizeof(pending_stats),
                      D3D11_ASYNC_GETDATA_DONOTFLUSH);
    print_hr("UnityQueryProbe GetData(PIPELINE_STATISTICS pending)", hr);
    if (hr != S_FALSE)
      pass = false;
    ctx->End(pipeline_query);
    ctx->Flush();
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats = {};
    for (UINT attempt = 0; attempt < 64; attempt++) {
      hr = ctx->GetData(pipeline_query, &stats, sizeof(stats), 0);
      if (hr != S_FALSE)
        break;
      Sleep(1);
    }
    print_hr("UnityQueryProbe GetData(PIPELINE_STATISTICS)", hr);
    printf("UnityQueryProbe pipeline_stats ia_vertices=%llu ps_invocations=%llu\n",
           (unsigned long long)stats.IAVertices,
           (unsigned long long)stats.PSInvocations);
    if (hr != S_OK)
      pass = false;
  }

  auto probe_so_statistics = [&](D3D11_QUERY query_type, const char *label) {
    D3D11_QUERY_DESC desc = {};
    desc.Query = query_type;
    ID3D11Query *query = nullptr;
    hr = device->CreateQuery(&desc, &query);
    char create_label[96] = {};
    snprintf(create_label, ARRAYSIZE(create_label),
             "UnityQueryProbe CreateQuery(%s)", label);
    print_hr(create_label, hr);
    if (FAILED(hr))
      pass = false;
    if (query) {
      ctx->Begin(query);
      D3D11_QUERY_DATA_SO_STATISTICS pending_stats = {};
      hr = ctx->GetData(query, &pending_stats, sizeof(pending_stats),
                        D3D11_ASYNC_GETDATA_DONOTFLUSH);
      char pending_label[104] = {};
      snprintf(pending_label, ARRAYSIZE(pending_label),
               "UnityQueryProbe GetData(%s pending)", label);
      print_hr(pending_label, hr);
      if (hr != S_FALSE)
        pass = false;
      ctx->End(query);
      ctx->Flush();
      D3D11_QUERY_DATA_SO_STATISTICS stats = {};
      hr = ctx->GetData(query, &stats, sizeof(stats), 0);
      char data_label[96] = {};
      snprintf(data_label, ARRAYSIZE(data_label),
               "UnityQueryProbe GetData(%s)", label);
      print_hr(data_label, hr);
      printf("UnityQueryProbe %s written=%llu needed=%llu\n", label,
             (unsigned long long)stats.NumPrimitivesWritten,
             (unsigned long long)stats.PrimitivesStorageNeeded);
      if (hr != S_OK || stats.NumPrimitivesWritten ||
          stats.PrimitivesStorageNeeded)
        pass = false;
      query->Release();
    }
  };

  auto probe_so_overflow = [&](D3D11_QUERY query_type, const char *label) {
    D3D11_QUERY_DESC desc = {};
    desc.Query = query_type;
    ID3D11Query *query = nullptr;
    hr = device->CreateQuery(&desc, &query);
    char create_label[96] = {};
    snprintf(create_label, ARRAYSIZE(create_label),
             "UnityQueryProbe CreateQuery(%s)", label);
    print_hr(create_label, hr);
    if (FAILED(hr))
      pass = false;
    if (query) {
      ctx->Begin(query);
      BOOL pending_overflowed = TRUE;
      hr = ctx->GetData(query, &pending_overflowed, sizeof(pending_overflowed),
                        D3D11_ASYNC_GETDATA_DONOTFLUSH);
      char pending_label[104] = {};
      snprintf(pending_label, ARRAYSIZE(pending_label),
               "UnityQueryProbe GetData(%s pending)", label);
      print_hr(pending_label, hr);
      if (hr != S_FALSE)
        pass = false;
      ctx->End(query);
      ctx->Flush();
      BOOL overflowed = TRUE;
      hr = ctx->GetData(query, &overflowed, sizeof(overflowed), 0);
      char data_label[96] = {};
      snprintf(data_label, ARRAYSIZE(data_label),
               "UnityQueryProbe GetData(%s)", label);
      print_hr(data_label, hr);
      printf("UnityQueryProbe %s overflowed=%u\n", label,
             (unsigned)overflowed);
      if (hr != S_OK || overflowed)
        pass = false;
      query->Release();
    }
  };

  probe_so_statistics(D3D11_QUERY_SO_STATISTICS, "SO_STATISTICS");
  probe_so_statistics(D3D11_QUERY_SO_STATISTICS_STREAM3,
                      "SO_STATISTICS_STREAM3");
  probe_so_overflow(D3D11_QUERY_SO_OVERFLOW_PREDICATE,
                    "SO_OVERFLOW_PREDICATE");
  probe_so_overflow(D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2,
                    "SO_OVERFLOW_PREDICATE_STREAM2");

  D3D11_QUERY_DESC invalid_query_desc = {};
  invalid_query_desc.Query = (D3D11_QUERY)0xffffffffu;
  ID3D11Query *invalid_query = (ID3D11Query *)0x1;
  hr = device->CreateQuery(&invalid_query_desc, &invalid_query);
  print_hr("UnityQueryProbe CreateQuery(INVALID)", hr);
  printf("UnityQueryProbe invalid_query=%p\n", invalid_query);
  if (hr != E_INVALIDARG || invalid_query)
    pass = false;

  D3D11_QUERY_DESC predicate_desc = {};
  predicate_desc.Query = D3D11_QUERY_OCCLUSION;
  ID3D11Predicate *predicate = nullptr;
  hr = device->CreatePredicate(&predicate_desc, &predicate);
  print_hr("UnityQueryProbe CreatePredicate(OCCLUSION)", hr);
  if (FAILED(hr))
    pass = false;
  if (predicate) {
    ctx->SetPredication(predicate, FALSE);
    printf("UnityQueryProbe SetPredication(predicate)=PASS\n");
    ID3D11Predicate *current_predicate = nullptr;
    BOOL predicate_value = TRUE;
    ctx->GetPredication(&current_predicate, &predicate_value);
    printf("UnityQueryProbe GetPredication predicate=%p value=%u\n",
           current_predicate, (unsigned)predicate_value);
    if (current_predicate != predicate || predicate_value != FALSE)
      pass = false;
    if (current_predicate)
      current_predicate->Release();

    ctx->SetPredication(nullptr, FALSE);
    printf("UnityQueryProbe ClearPredication=PASS\n");
    current_predicate = (ID3D11Predicate *)0x1;
    predicate_value = TRUE;
    ctx->GetPredication(&current_predicate, &predicate_value);
    printf("UnityQueryProbe GetPredication(clear) predicate=%p value=%u\n",
           current_predicate, (unsigned)predicate_value);
    if (current_predicate || predicate_value != FALSE)
      pass = false;
  }

  ID3D11DeviceContext *deferred = nullptr;
  hr = device->CreateDeferredContext(0, &deferred);
  print_hr("UnityDeferredProbe CreateDeferredContext", hr);
  if (FAILED(hr))
    pass = false;
  if (deferred) {
    ID3D11CommandList *command_list = nullptr;
    hr = deferred->FinishCommandList(FALSE, &command_list);
    print_hr("UnityDeferredProbe FinishCommandList", hr);
    if (FAILED(hr)) {
      pass = false;
    } else if (command_list) {
      ctx->ExecuteCommandList(command_list, FALSE);
      printf("UnityDeferredProbe ExecuteCommandList=PASS\n");
      deferred->ExecuteCommandList(command_list, FALSE);
      printf("UnityDeferredProbe ExecuteCommandList(deferred_noop)=PASS\n");
      command_list->Release();
    }
    deferred->Release();
  }

  if (predicate)
    predicate->Release();
  if (pipeline_query)
    pipeline_query->Release();
  if (occlusion_query)
    occlusion_query->Release();
  if (timestamp_query)
    timestamp_query->Release();
  if (disjoint_query)
    disjoint_query->Release();
  if (event_query)
    event_query->Release();

  printf("UnityQueryDeferredProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_deferred_resource_updates(ID3D11Device *device,
                                                  ID3D11DeviceContext *ctx) {
  bool pass = true;

  uint32_t texels[16] = {};
  for (UINT i = 0; i < ARRAYSIZE(texels); i++)
    texels[i] = 0xff1133c0u | i;
  uint32_t box_texels[4] = {
      0xff44aa11u,
      0xff44aa22u,
      0xff44aa33u,
      0xff44aa44u,
  };
  uint32_t default_words[16] = {};
  for (UINT i = 0; i < ARRAYSIZE(default_words); i++)
    default_words[i] = 0x6bdf0000u | i;
  uint32_t deferred_staging_words[8] = {};
  for (UINT i = 0; i < ARRAYSIZE(deferred_staging_words); i++)
    deferred_staging_words[i] = 0xdede0000u | i;
  uint32_t deferred_staging_texels[16] = {};
  for (UINT i = 0; i < ARRAYSIZE(deferred_staging_texels); i++)
    deferred_staging_texels[i] = 0x7e570000u | i;

  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = 4;
  tex_desc.Height = 4;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_DEFAULT;
  tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  ID3D11Texture2D *default_tex = nullptr;
  HRESULT hr = device->CreateTexture2D(&tex_desc, nullptr, &default_tex);
  print_hr("UnityDeferredResourceProbe CreateTexture2D(default)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_TEXTURE2D_DESC staging_desc = tex_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging_tex = nullptr;
  hr = device->CreateTexture2D(&staging_desc, nullptr, &staging_tex);
  print_hr("UnityDeferredResourceProbe CreateTexture2D(staging)", hr);
  if (FAILED(hr))
    pass = false;
  ID3D11Texture2D *update_staging_tex = nullptr;
  hr = device->CreateTexture2D(&staging_desc, nullptr, &update_staging_tex);
  print_hr("UnityDeferredResourceProbe CreateTexture2D(update_staging)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC dynamic_desc = {};
  dynamic_desc.ByteWidth = 64;
  dynamic_desc.Usage = D3D11_USAGE_DYNAMIC;
  dynamic_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  dynamic_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ID3D11Buffer *dynamic_buffer = nullptr;
  hr = device->CreateBuffer(&dynamic_desc, nullptr, &dynamic_buffer);
  print_hr("UnityDeferredResourceProbe CreateBuffer(dynamic_vertex)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC staging_buffer_desc = dynamic_desc;
  staging_buffer_desc.Usage = D3D11_USAGE_STAGING;
  staging_buffer_desc.BindFlags = 0;
  staging_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *staging_buffer = nullptr;
  hr = device->CreateBuffer(&staging_buffer_desc, nullptr, &staging_buffer);
  print_hr("UnityDeferredResourceProbe CreateBuffer(staging)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC update_staging_buffer_desc = staging_buffer_desc;
  update_staging_buffer_desc.ByteWidth = sizeof(deferred_staging_words);
  ID3D11Buffer *update_staging_buffer = nullptr;
  hr = device->CreateBuffer(&update_staging_buffer_desc, nullptr,
                            &update_staging_buffer);
  print_hr("UnityDeferredResourceProbe CreateBuffer(update_staging)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC default_buffer_desc = {};
  default_buffer_desc.ByteWidth = sizeof(default_words);
  default_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  default_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer *default_buffer = nullptr;
  hr = device->CreateBuffer(&default_buffer_desc, nullptr, &default_buffer);
  print_hr("UnityDeferredResourceProbe CreateBuffer(default_constant)", hr);
  if (FAILED(hr))
    pass = false;

  D3D11_BUFFER_DESC default_staging_buffer_desc = default_buffer_desc;
  default_staging_buffer_desc.Usage = D3D11_USAGE_STAGING;
  default_staging_buffer_desc.BindFlags = 0;
  default_staging_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer *default_staging_buffer = nullptr;
  hr = device->CreateBuffer(&default_staging_buffer_desc, nullptr,
                            &default_staging_buffer);
  print_hr("UnityDeferredResourceProbe CreateBuffer(default_staging)", hr);
  if (FAILED(hr))
    pass = false;

  ID3D11DeviceContext *deferred = nullptr;
  hr = device->CreateDeferredContext(0, &deferred);
  print_hr("UnityDeferredResourceProbe CreateDeferredContext", hr);
  if (FAILED(hr) || !deferred)
    pass = false;

  bool recorded_dynamic_map = false;
  bool recorded_dynamic_no_overwrite = false;
  ID3D11CommandList *command_list = nullptr;
  if (deferred && default_tex && staging_tex && dynamic_buffer &&
      update_staging_tex && staging_buffer && update_staging_buffer &&
      default_buffer && default_staging_buffer) {
    deferred->UpdateSubresource(default_tex, 0, nullptr, texels,
                                4 * sizeof(uint32_t), 0);
    printf("UnityDeferredResourceProbe UpdateSubresource(texture)=PASS\n");

    D3D11_BOX dst_box = {1, 1, 0, 3, 3, 1};
    deferred->UpdateSubresource(default_tex, 0, &dst_box, box_texels,
                                2 * sizeof(uint32_t), 0);
    printf("UnityDeferredResourceProbe UpdateSubresource(texture_box)=PASS\n");

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = deferred->Map(dynamic_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    print_hr("UnityDeferredResourceProbe Map(dynamic_vertex)", hr);
    if (SUCCEEDED(hr)) {
      uint32_t *words = (uint32_t *)mapped.pData;
      for (UINT i = 0; i < dynamic_desc.ByteWidth / sizeof(uint32_t); i++)
        words[i] = 0x5ade0000u | i;
      deferred->Unmap(dynamic_buffer, 0);
      recorded_dynamic_map = true;
      printf("UnityDeferredResourceProbe Unmap(dynamic_vertex)=PASS\n");
    } else {
      pass = false;
    }

    D3D11_MAPPED_SUBRESOURCE no_overwrite = {};
    hr = deferred->Map(dynamic_buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                       &no_overwrite);
    print_hr("UnityDeferredResourceProbe Map(dynamic_vertex_no_overwrite)", hr);
    if (SUCCEEDED(hr)) {
      uint32_t *words = (uint32_t *)no_overwrite.pData;
      for (UINT i = 0; i < 4; i++)
        words[i] = 0x6ade0000u | i;
      deferred->Unmap(dynamic_buffer, 0);
      recorded_dynamic_no_overwrite = true;
      printf("UnityDeferredResourceProbe Unmap(dynamic_vertex_no_overwrite)=PASS\n");
    } else {
      pass = false;
    }

    deferred->UpdateSubresource(default_buffer, 0, nullptr, default_words, 0,
                                0);
    printf("UnityDeferredResourceProbe UpdateSubresource(default_constant)=PASS\n");

    deferred->UpdateSubresource(update_staging_buffer, 0, nullptr,
                                deferred_staging_words, 0, 0);
    printf("UnityDeferredResourceProbe UpdateSubresource(staging_buffer)=PASS\n");

    deferred->UpdateSubresource(update_staging_tex, 0, nullptr,
                                deferred_staging_texels,
                                4 * sizeof(uint32_t), 0);
    printf("UnityDeferredResourceProbe UpdateSubresource(staging_texture)=PASS\n");

    deferred->CopyResource(staging_tex, default_tex);
    printf("UnityDeferredResourceProbe CopyResource(texture_to_staging)=PASS\n");
    if (recorded_dynamic_map) {
      deferred->CopyResource(staging_buffer, dynamic_buffer);
      printf("UnityDeferredResourceProbe CopyResource(buffer_to_staging)=PASS\n");
    }
    deferred->CopyResource(default_staging_buffer, default_buffer);
    printf("UnityDeferredResourceProbe CopyResource(default_to_staging)=PASS\n");

    hr = deferred->FinishCommandList(FALSE, &command_list);
    print_hr("UnityDeferredResourceProbe FinishCommandList", hr);
    if (FAILED(hr) || !command_list) {
      pass = false;
    } else {
      ctx->ExecuteCommandList(command_list, FALSE);
      printf("UnityDeferredResourceProbe ExecuteCommandList=PASS\n");
    }
  }

  if (staging_tex) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging_tex, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityDeferredResourceProbe Map(texture_staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t first = *(const uint32_t *)mapped.pData;
      printf("UnityDeferredResourceProbe texture_staging_pixel0=0x%08x\n",
             first);
      const uint8_t *row1 = (const uint8_t *)mapped.pData + mapped.RowPitch;
      const uint32_t box_first = *(const uint32_t *)(row1 + sizeof(uint32_t));
      printf("UnityDeferredResourceProbe texture_staging_box_pixel=0x%08x\n",
             box_first);
      if (first != texels[0])
        pass = false;
      if (box_first != box_texels[0])
        pass = false;
      ctx->Unmap(staging_tex, 0);
    } else {
      pass = false;
    }
  }

  if (update_staging_tex) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(update_staging_tex, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityDeferredResourceProbe Map(update_staging_texture)", hr);
    if (SUCCEEDED(hr)) {
      const uint8_t *base = (const uint8_t *)mapped.pData;
      const uint32_t first = *(const uint32_t *)base;
      const uint32_t last =
          *(const uint32_t *)(base + 3 * mapped.RowPitch +
                              3 * sizeof(uint32_t));
      printf("UnityDeferredResourceProbe update_staging_texel0=0x%08x texel15=0x%08x\n",
             first, last);
      if (first != deferred_staging_texels[0] ||
          last != deferred_staging_texels[15])
        pass = false;
      ctx->Unmap(update_staging_tex, 0);
    } else {
      pass = false;
    }
  }

  if (staging_buffer && recorded_dynamic_map) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging_buffer, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityDeferredResourceProbe Map(buffer_staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t first = *(const uint32_t *)mapped.pData;
      printf("UnityDeferredResourceProbe buffer_staging_word0=0x%08x\n",
             first);
      const uint32_t expected =
          recorded_dynamic_no_overwrite ? 0x6ade0000u : 0x5ade0000u;
      if (first != expected)
        pass = false;
      ctx->Unmap(staging_buffer, 0);
    } else {
      pass = false;
    }
  }

  if (default_staging_buffer) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(default_staging_buffer, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityDeferredResourceProbe Map(default_staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t *words = (const uint32_t *)mapped.pData;
      printf("UnityDeferredResourceProbe default_staging_word0=0x%08x word3=0x%08x\n",
             words[0], words[3]);
      if (words[0] != default_words[0] || words[3] != default_words[3])
        pass = false;
      ctx->Unmap(default_staging_buffer, 0);
    } else {
      pass = false;
    }
  }

  if (update_staging_buffer) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(update_staging_buffer, 0, D3D11_MAP_READ, 0, &mapped);
    print_hr("UnityDeferredResourceProbe Map(update_staging)", hr);
    if (SUCCEEDED(hr)) {
      const uint32_t *words = (const uint32_t *)mapped.pData;
      printf("UnityDeferredResourceProbe update_staging_word0=0x%08x word7=0x%08x\n",
             words[0], words[7]);
      if (words[0] != deferred_staging_words[0] ||
          words[7] != deferred_staging_words[7])
        pass = false;
      ctx->Unmap(update_staging_buffer, 0);
    } else {
      pass = false;
    }
  }

  if (command_list)
    command_list->Release();
  if (deferred)
    deferred->Release();
  if (default_staging_buffer)
    default_staging_buffer->Release();
  if (default_buffer)
    default_buffer->Release();
  if (update_staging_buffer)
    update_staging_buffer->Release();
  if (staging_buffer)
    staging_buffer->Release();
  if (dynamic_buffer)
    dynamic_buffer->Release();
  if (staging_tex)
    staging_tex->Release();
  if (update_staging_tex)
    update_staging_tex->Release();
  if (default_tex)
    default_tex->Release();

  printf("UnityDeferredResourceProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_deferred_clear(ID3D11Device *device,
                                       ID3D11DeviceContext *ctx,
                                       ID3D11RenderTargetView *rtv,
                                       ID3D11Texture2D *backbuffer) {
  ID3D11DeviceContext *deferred = nullptr;
  HRESULT hr = device->CreateDeferredContext(0, &deferred);
  print_hr("UnityDeferredDrawProbe CreateDeferredContext", hr);
  if (FAILED(hr) || !deferred)
    return false;

  const FLOAT color[4] = {0.25f, 0.0f, 1.0f, 1.0f};
  deferred->ClearRenderTargetView(rtv, color);
  printf("UnityDeferredDrawProbe ClearRenderTargetView(recorded)=PASS\n");

  ID3D11CommandList *command_list = nullptr;
  hr = deferred->FinishCommandList(FALSE, &command_list);
  print_hr("UnityDeferredDrawProbe FinishCommandList", hr);
  bool pass = SUCCEEDED(hr) && command_list;
  if (command_list) {
    ctx->ExecuteCommandList(command_list, FALSE);
    printf("UnityDeferredDrawProbe ExecuteCommandList=PASS\n");
    command_list->Release();
  }
  deferred->Release();

  uint8_t pixel[4] = {};
  hr = read_first_pixel(device, ctx, backbuffer, pixel);
  print_hr("UnityDeferredDrawProbe Readback", hr);
  printf("UnityDeferredDrawProbe pixel0_bgra=%u,%u,%u,%u\n", pixel[0],
         pixel[1], pixel[2], pixel[3]);
  if (FAILED(hr) || pixel[0] < 235 || pixel[1] > 2 || pixel[2] < 55 ||
      pixel[2] > 70 || pixel[3] != 255)
    pass = false;

  printf("UnityDeferredDrawProbe result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool probe_unity_deferred_draw_commands(ID3D11Device *device,
                                               ID3D11DeviceContext *ctx,
                                               ID3D11RenderTargetView *rtv,
                                               ID3D11Texture2D *backbuffer) {
  ID3D11DeviceContext *deferred = nullptr;
  HRESULT hr = device->CreateDeferredContext(0, &deferred);
  print_hr("UnityDeferredDrawProbe CreateDeferredContext(draw)", hr);
  if (FAILED(hr) || !deferred)
    return false;

  bool pass = probe_unity_draw(device, deferred, rtv);
  printf("UnityDeferredDrawProbe RecordDrawCommands=%s\n",
         pass ? "PASS" : "FAIL");

  ID3D11CommandList *command_list = nullptr;
  if (pass) {
    hr = deferred->FinishCommandList(FALSE, &command_list);
    print_hr("UnityDeferredDrawProbe FinishDrawCommandList", hr);
    pass = SUCCEEDED(hr) && command_list;
  }
  if (command_list) {
    ctx->ExecuteCommandList(command_list, FALSE);
    printf("UnityDeferredDrawProbe ExecuteDrawCommandList=PASS\n");
    command_list->Release();
  }
  deferred->Release();

  uint8_t pixel[4] = {};
  hr = read_first_pixel(device, ctx, backbuffer, pixel);
  print_hr("UnityDeferredDrawProbe DrawReadback", hr);
  printf("UnityDeferredDrawProbe draw_pixel0_bgra=%u,%u,%u,%u\n", pixel[0],
         pixel[1], pixel[2], pixel[3]);
  if (FAILED(hr) || pixel[0] > 2 || pixel[1] < 235 || pixel[2] > 2 ||
      pixel[3] != 255)
    pass = false;

  printf("UnityDeferredDrawProbe draw_result=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  HMODULE winemetal_module = load_local_dll(L".\\winemetal.dll", "winemetal");
  if (!winemetal_module)
    printf("winemetal_preload=nonfatal\n");

  HMODULE dxgi_module = load_local_dll(L".\\dxgi.dll", "dxgi");
  if (!dxgi_module)
    return 11;

  HMODULE d3d11_module = load_local_dll(L".\\d3d11.dll", "d3d11");
  if (!d3d11_module)
    return 12;

  SetLastError(0);
  auto create_factory = (CreateDXGIFactory1Proc)GetProcAddress(
      dxgi_module, "CreateDXGIFactory1");
  printf("GetProcAddress(CreateDXGIFactory1) proc=%p gle=%lu\n", create_factory,
         GetLastError());
  if (create_factory) {
    IDXGIFactory1 *factory_probe = nullptr;
    HRESULT factory_hr =
        create_factory(__uuidof(IDXGIFactory1), (void **)&factory_probe);
    print_hr("CreateDXGIFactory1(probe)", factory_hr);
    printf("factory_probe=%p\n", factory_probe);
    if (SUCCEEDED(factory_hr) && factory_probe) {
      IDXGIAdapter *adapter_probe = nullptr;
      HRESULT enum_hr = factory_probe->EnumAdapters(0, &adapter_probe);
      print_hr("IDXGIFactory::EnumAdapters(0 probe)", enum_hr);
      printf("adapter_probe=%p\n", adapter_probe);
      DXGI_ADAPTER_DESC adapter_probe_desc = {};
      bool have_adapter_probe_desc = false;
      if (adapter_probe) {
        HRESULT desc_hr = adapter_probe->GetDesc(&adapter_probe_desc);
        print_hr("IDXGIAdapter::GetDesc(probe)", desc_hr);
        printf("adapter_probe_luid=%ld,%ld\n",
               adapter_probe_desc.AdapterLuid.HighPart,
               adapter_probe_desc.AdapterLuid.LowPart);
        have_adapter_probe_desc = SUCCEEDED(desc_hr);
        adapter_probe->Release();
      }
      bool adapter_outputs_pass = probe_dxgi_adapter_outputs(factory_probe);
      bool factory_status_pass = true;
      IDXGIFactory2 *factory2 = nullptr;
      HRESULT factory2_hr =
          factory_probe->QueryInterface(__uuidof(IDXGIFactory2),
                                        (void **)&factory2);
      print_hr("IDXGIFactory::QueryInterface(IDXGIFactory2)", factory2_hr);
      if (FAILED(factory2_hr) || !factory2) {
        factory_status_pass = false;
      } else {
        HWND factory_hwnd = create_hidden_window(instance);
        HANDLE status_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        printf("factory_status_probe hwnd=%p event=%p\n", factory_hwnd,
               status_event);
        if (!factory_hwnd || !status_event)
          factory_status_pass = false;

        if (factory_hwnd) {
          HRESULT assoc_hr =
              factory2->MakeWindowAssociation(factory_hwnd, 0);
          print_hr("IDXGIFactory::MakeWindowAssociation", assoc_hr);
          HWND associated_hwnd = nullptr;
          HRESULT get_assoc_hr =
              factory2->GetWindowAssociation(&associated_hwnd);
          print_hr("IDXGIFactory::GetWindowAssociation", get_assoc_hr);
          printf("factory_associated_window=%p expected=%p\n", associated_hwnd,
                 factory_hwnd);
          if (FAILED(assoc_hr) || FAILED(get_assoc_hr) ||
              associated_hwnd != factory_hwnd)
            factory_status_pass = false;

          DWORD cookie = 0;
          HRESULT status_hr = factory2->RegisterStereoStatusWindow(
              factory_hwnd, WM_APP + 1, &cookie);
          print_hr("IDXGIFactory2::RegisterStereoStatusWindow", status_hr);
          printf("factory_stereo_window_cookie=%lu\n",
                 (unsigned long)cookie);
          if (FAILED(status_hr) || !cookie)
            factory_status_pass = false;
          if (cookie)
            factory2->UnregisterStereoStatus(cookie);

          cookie = 0;
          status_hr = factory2->RegisterOcclusionStatusWindow(
              factory_hwnd, WM_APP + 2, &cookie);
          print_hr("IDXGIFactory2::RegisterOcclusionStatusWindow", status_hr);
          printf("factory_occlusion_window_cookie=%lu\n",
                 (unsigned long)cookie);
          if (FAILED(status_hr) || !cookie)
            factory_status_pass = false;
          if (cookie)
            factory2->UnregisterOcclusionStatus(cookie);

          cookie = 0xdeadbeef;
          status_hr = factory2->RegisterOcclusionStatusWindow(
              nullptr, WM_APP + 3, &cookie);
          print_hr("IDXGIFactory2::RegisterOcclusionStatusWindow(null)",
                   status_hr);
          printf("factory_occlusion_window_null_cookie=0x%08lx\n",
                 (unsigned long)cookie);
          if (status_hr != DXGI_ERROR_INVALID_CALL || cookie)
            factory_status_pass = false;
        }

        if (status_event) {
          DWORD cookie = 0;
          HRESULT status_hr =
              factory2->RegisterStereoStatusEvent(status_event, &cookie);
          print_hr("IDXGIFactory2::RegisterStereoStatusEvent", status_hr);
          printf("factory_stereo_event_cookie=%lu\n", (unsigned long)cookie);
          if (FAILED(status_hr) || !cookie)
            factory_status_pass = false;
          if (cookie)
            factory2->UnregisterStereoStatus(cookie);

          cookie = 0;
          status_hr =
              factory2->RegisterOcclusionStatusEvent(status_event, &cookie);
          print_hr("IDXGIFactory2::RegisterOcclusionStatusEvent", status_hr);
          printf("factory_occlusion_event_cookie=%lu\n",
                 (unsigned long)cookie);
          if (FAILED(status_hr) || !cookie)
            factory_status_pass = false;
          if (cookie)
            factory2->UnregisterOcclusionStatus(cookie);

          cookie = 0xdeadbeef;
          status_hr = factory2->RegisterStereoStatusEvent(nullptr, &cookie);
          print_hr("IDXGIFactory2::RegisterStereoStatusEvent(null)",
                   status_hr);
          printf("factory_stereo_event_null_cookie=0x%08lx\n",
                 (unsigned long)cookie);
          if (status_hr != DXGI_ERROR_INVALID_CALL || cookie)
            factory_status_pass = false;

          CloseHandle(status_event);
        }
        if (factory_hwnd)
          DestroyWindow(factory_hwnd);
        printf("factory_status_probe result=%s\n",
               factory_status_pass ? "PASS" : "FAIL");
        factory2->Release();
      }

      IDXGIFactory4 *factory4 = nullptr;
      HRESULT factory4_hr =
          factory_probe->QueryInterface(__uuidof(IDXGIFactory4),
                                        (void **)&factory4);
      print_hr("IDXGIFactory::QueryInterface(IDXGIFactory4)", factory4_hr);
      if (FAILED(factory4_hr) || !factory4) {
        factory_status_pass = false;
      } else {
        LUID luid = {0x11111111, 0x22222222};
        HRESULT luid_hr =
            factory4->GetSharedResourceAdapterLuid(nullptr, &luid);
        print_hr("IDXGIFactory4::GetSharedResourceAdapterLuid(null_resource)",
                 luid_hr);
        printf("factory_shared_luid_null=%ld,%ld\n", luid.HighPart,
               luid.LowPart);
        if (luid_hr != DXGI_ERROR_INVALID_CALL || luid.HighPart ||
            luid.LowPart)
          factory_status_pass = false;

        luid = {0x11111111, 0x22222222};
        luid_hr = factory4->GetSharedResourceAdapterLuid((HANDLE)0x1234,
                                                         &luid);
        print_hr("IDXGIFactory4::GetSharedResourceAdapterLuid(fake_resource)",
                 luid_hr);
        printf("factory_shared_luid_fake=%ld,%ld\n", luid.HighPart,
               luid.LowPart);
        if (luid_hr != DXGI_ERROR_UNSUPPORTED || luid.HighPart || luid.LowPart)
          factory_status_pass = false;

        luid_hr = factory4->GetSharedResourceAdapterLuid((HANDLE)0x1234,
                                                         nullptr);
        print_hr("IDXGIFactory4::GetSharedResourceAdapterLuid(null_luid)",
                 luid_hr);
        if (luid_hr != DXGI_ERROR_INVALID_CALL)
          factory_status_pass = false;

        IDXGIAdapter *luid_adapter = (IDXGIAdapter *)0x1;
        HRESULT enum_luid_hr = factory4->EnumAdapterByLuid(
            adapter_probe_desc.AdapterLuid, __uuidof(IDXGIAdapter), nullptr);
        print_hr("IDXGIFactory4::EnumAdapterByLuid(null)", enum_luid_hr);
        if (enum_luid_hr != DXGI_ERROR_INVALID_CALL)
          factory_status_pass = false;

        if (have_adapter_probe_desc) {
          luid_adapter = nullptr;
          enum_luid_hr = factory4->EnumAdapterByLuid(
              adapter_probe_desc.AdapterLuid, __uuidof(IDXGIAdapter),
              (void **)&luid_adapter);
          print_hr("IDXGIFactory4::EnumAdapterByLuid(valid)", enum_luid_hr);
          printf("factory_luid_adapter=%p\n", luid_adapter);
          if (FAILED(enum_luid_hr) || !luid_adapter) {
            factory_status_pass = false;
          } else {
            DXGI_ADAPTER_DESC luid_desc = {};
            HRESULT desc_hr = luid_adapter->GetDesc(&luid_desc);
            print_hr("IDXGIFactory4::EnumAdapterByLuid GetDesc", desc_hr);
            printf("factory_luid_match=%u\n",
                   luid_desc.AdapterLuid.HighPart ==
                           adapter_probe_desc.AdapterLuid.HighPart &&
                       luid_desc.AdapterLuid.LowPart ==
                           adapter_probe_desc.AdapterLuid.LowPart);
            if (FAILED(desc_hr) ||
                luid_desc.AdapterLuid.HighPart !=
                    adapter_probe_desc.AdapterLuid.HighPart ||
                luid_desc.AdapterLuid.LowPart !=
                    adapter_probe_desc.AdapterLuid.LowPart)
              factory_status_pass = false;
            luid_adapter->Release();
          }
        }

        LUID missing_luid = {0x33333333, 0x44444444};
        luid_adapter = (IDXGIAdapter *)0x1;
        enum_luid_hr = factory4->EnumAdapterByLuid(
            missing_luid, __uuidof(IDXGIAdapter), (void **)&luid_adapter);
        print_hr("IDXGIFactory4::EnumAdapterByLuid(missing)", enum_luid_hr);
        printf("factory_luid_missing_adapter=%p\n", luid_adapter);
        if (enum_luid_hr != DXGI_ERROR_NOT_FOUND || luid_adapter)
          factory_status_pass = false;

        IDXGIAdapter *warp_adapter = (IDXGIAdapter *)0x1;
        HRESULT warp_hr =
            factory4->EnumWarpAdapter(__uuidof(IDXGIAdapter), nullptr);
        print_hr("IDXGIFactory4::EnumWarpAdapter(null)", warp_hr);
        if (warp_hr != DXGI_ERROR_INVALID_CALL)
          factory_status_pass = false;

        warp_hr = factory4->EnumWarpAdapter(__uuidof(IDXGIAdapter),
                                           (void **)&warp_adapter);
        print_hr("IDXGIFactory4::EnumWarpAdapter", warp_hr);
        printf("factory_warp_adapter=%p\n", warp_adapter);
        if (FAILED(warp_hr) || !warp_adapter) {
          factory_status_pass = false;
        } else {
          DXGI_ADAPTER_DESC warp_desc = {};
          HRESULT warp_desc_hr = warp_adapter->GetDesc(&warp_desc);
          print_hr("factory_warp_adapter GetDesc", warp_desc_hr);
          printf("factory_warp_adapter_luid=%ld,%ld\n",
                 warp_desc.AdapterLuid.HighPart, warp_desc.AdapterLuid.LowPart);
          if (FAILED(warp_desc_hr))
            factory_status_pass = false;
          warp_adapter->Release();
        }

        factory4->Release();
      }
      factory_probe->Release();
      if (!adapter_outputs_pass)
        return 20;
      if (!factory_status_pass)
        return 21;
    }
  }

  SetLastError(0);
  auto create_device = (D3D11CreateDeviceProc)GetProcAddress(
      d3d11_module, "D3D11CreateDevice");
  printf("GetProcAddress(D3D11CreateDevice) proc=%p gle=%lu\n", create_device,
         GetLastError());
  if (!create_device)
    return 13;

  SetLastError(0);
  auto create_on12 = (D3D11On12CreateDeviceProc)GetProcAddress(
      d3d11_module, "D3D11On12CreateDevice");
  printf("GetProcAddress(D3D11On12CreateDevice) proc=%p gle=%lu\n",
         create_on12, GetLastError());
  if (!create_on12)
    return 42;

  const D3D_FEATURE_LEVEL requested[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0;
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

  HRESULT hr = create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                             requested, ARRAYSIZE(requested),
                             D3D11_SDK_VERSION, &device, &chosen, &ctx);
  print_hr("D3D11CreateDevice", hr);
  if (FAILED(hr))
    return 3;

  printf("feature_level=0x%04x\n", (unsigned)chosen);

  if (!probe_unity_output_capabilities(create_factory, device))
    return 40;
  if (!probe_unity_feature_level_creation(create_device))
    return 37;
  if (!probe_warp_device_creation(create_device))
    return 46;
  if (!probe_d3d11on12_unsupported(create_on12, device))
    return 42;
  if (!probe_unity_device_interfaces(device))
    return 38;
  if (!probe_unity_fence_and_context_state(device, ctx))
    return 41;

  UINT bgra_support = 0;
  hr = device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &bgra_support);
  print_hr("CheckFormatSupport(B8G8R8A8_UNORM)", hr);
  printf("format_support=0x%08x\n", bgra_support);
  if (FAILED(hr) || !(bgra_support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
    return 4;

  if (!probe_unity_formats(device))
    return 14;
  if (!probe_unity_features(device))
    return 45;
  if (!probe_unity_counters(device))
    return 39;
  if (!probe_unity_resources(device, ctx))
    return 15;
  if (!probe_unity_srgb_render_target(device, ctx))
    return 30;
  if (!probe_unity_srgb_sampling(device, ctx))
    return 44;
  if (!probe_unity_msaa_resolve(device, ctx))
    return 36;
  if (!probe_unity_resource_residency_churn(device, ctx))
    return 34;
  if (!probe_unity_states(device, ctx))
    return 17;
  if (!probe_unity_compute_dispatch(device, ctx))
    return 21;
  if (!probe_unity_shader_corpus(device))
    return 43;
  if (!probe_unity_real_shader_corpus(device, ctx))
    return 47;
  if (!probe_unity_tessellation_shaders(device, ctx))
    return 28;
  if (!probe_unity_multi_render_targets(device, ctx))
    return 31;
  if (!probe_unity_multithreaded_deferred(device, ctx))
    return 32;
  if (!probe_unity_command_batch_stability(device, ctx))
    return 33;
  if (!probe_unity_queries_and_deferred(device, ctx))
    return 22;
  if (!probe_unity_deferred_resource_updates(device, ctx))
    return 29;

  HWND hwnd = create_hidden_window(instance);
  if (!hwnd)
    printf("window_create failed gle=%lu; using offscreen RTV path\n",
           GetLastError());

  IDXGISwapChain1 *swapchain = nullptr;
  ID3D11Texture2D *backbuffer = nullptr;
  bool using_swapchain = hwnd != nullptr;

  if (using_swapchain) {
    printf("CreateSwapChainForHwnd(begin) hwnd=%p is_window=%d parent=%p\n", hwnd,
           IsWindow(hwnd), GetParent(hwnd));
    hr = create_swapchain(device, hwnd, &swapchain);
    print_hr("CreateSwapChainForHwnd", hr);
    if (FAILED(hr))
      return 5;

    hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                              (void **)&backbuffer);
    print_hr("IDXGISwapChain::GetBuffer", hr);
    if (FAILED(hr))
      return 6;
  } else {
    hr = create_offscreen_target(device, &backbuffer);
    print_hr("CreateTexture2D(offscreen RTV)", hr);
    if (FAILED(hr))
      return 6;
  }

  ID3D11RenderTargetView *rtv = nullptr;
  hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
  print_hr("CreateRenderTargetView", hr);
  if (FAILED(hr))
    return 7;

  const FLOAT color[4] = {0.125f, 0.5f, 0.875f, 1.0f};
  ctx->ClearRenderTargetView(rtv, color);
  printf("ClearRenderTargetView color_rgba=0.125,0.5,0.875,1.0\n");
  if (!probe_unity_deferred_clear(device, ctx, rtv, backbuffer))
    return 25;
  if (!probe_unity_deferred_draw_commands(device, ctx, rtv, backbuffer))
    return 27;
  if (!probe_unity_draw(device, ctx, rtv))
    return 16;

  if (using_swapchain) {
    if (!probe_unity_long_run_stability(device, ctx, swapchain, rtv, backbuffer))
      return 35;

    hr = swapchain->Present(0, 0);
    print_hr("Present", hr);
    if (FAILED(hr))
      return 8;

    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv->Release();
    rtv = nullptr;
    backbuffer->Release();
    backbuffer = nullptr;

    hr = swapchain->ResizeBuffers(0, kWidth / 2, kHeight / 2,
                                  DXGI_FORMAT_UNKNOWN, 0);
    print_hr("ResizeBuffers(32x32)", hr);
    if (FAILED(hr))
      return 18;

    hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                              (void **)&backbuffer);
    print_hr("IDXGISwapChain::GetBuffer(resized)", hr);
    if (FAILED(hr))
      return 6;

    hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    print_hr("CreateRenderTargetView(resized)", hr);
    if (FAILED(hr))
      return 7;

    ctx->ClearRenderTargetView(rtv, color);
    printf("ClearRenderTargetView(resized) color_rgba=0.125,0.5,0.875,1.0\n");
    if (!probe_unity_deferred_clear(device, ctx, rtv, backbuffer))
      return 26;
    if (!probe_unity_draw(device, ctx, rtv))
      return 19;

    hr = swapchain->Present(0, 0);
    print_hr("Present(resized)", hr);
    if (FAILED(hr))
      return 8;

    IDXGISwapChain3 *swapchain3 = nullptr;
    hr = swapchain->QueryInterface(__uuidof(IDXGISwapChain3),
                                   (void **)&swapchain3);
    print_hr("IDXGISwapChain3::QueryInterface", hr);
    if (FAILED(hr))
      return 40;

    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv->Release();
    rtv = nullptr;
    backbuffer->Release();
    backbuffer = nullptr;

    hr = swapchain3->ResizeBuffers1(0, kWidth, kHeight, DXGI_FORMAT_UNKNOWN,
                                    0, nullptr, nullptr);
    print_hr("ResizeBuffers1(64x64)", hr);
    swapchain3->Release();
    if (FAILED(hr))
      return 41;

    hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                              (void **)&backbuffer);
    print_hr("IDXGISwapChain::GetBuffer(resized1)", hr);
    if (FAILED(hr))
      return 6;

    hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    print_hr("CreateRenderTargetView(resized1)", hr);
    if (FAILED(hr))
      return 7;

    const FLOAT final_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, final_color);
    printf("ClearRenderTargetView(resized1) color_rgba=0,1,0,1\n");
    hr = swapchain->Present(0, 0);
    print_hr("Present(resized1)", hr);
    if (FAILED(hr))
      return 8;
  } else {
    printf("Present=SKIP no_hwnd offscreen_rtv\n");
  }

  uint8_t pixel[4] = {};
  hr = read_first_pixel(device, ctx, backbuffer, pixel);
  print_hr("Readback(Map staging)", hr);
  printf("pixel0_bgra=%u,%u,%u,%u\n", pixel[0], pixel[1], pixel[2],
         pixel[3]);

  bool pixel_ok = pixel[0] <= 2 && pixel[1] >= 235 && pixel[2] <= 2 &&
                  pixel[3] == 255;
  printf("pixel_readback=%s\n", pixel_ok ? "PASS" : "FAIL");

  if (rtv) {
    rtv->Release();
    rtv = nullptr;
  }
  if (backbuffer) {
    backbuffer->Release();
    backbuffer = nullptr;
  }
  if (swapchain) {
    swapchain->Release();
    swapchain = nullptr;
  }

  int result = pixel_ok ? 0 : 9;
  if (result == 0 && using_swapchain) {
    if (!probe_swapchain_variant(device, ctx, instance, DXGI_SWAP_EFFECT_DISCARD,
                                 "bitblt_discard")) {
      result = 23;
    } else if (!probe_swapchain_variant(device, ctx, instance,
                                        DXGI_SWAP_EFFECT_SEQUENTIAL,
                                        "bitblt_sequential")) {
      result = 45;
    } else if (!probe_swapchain_variant(device, ctx, instance,
                                        DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                                        "flip_sequential")) {
      result = 46;
    } else if (!probe_swapchain_variant(device, ctx, instance,
                                        DXGI_SWAP_EFFECT_FLIP_DISCARD,
                                        "flip_discard")) {
      result = 24;
    }
  }

  ctx->Release();
  device->Release();
  if (hwnd)
    DestroyWindow(hwnd);

  return result;
}
