#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const UINT kWidth = 64;
static const UINT kHeight = 64;

typedef HRESULT(WINAPI *D3D11CreateDeviceProc)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);

static void print_hr(const char *label, HRESULT hr) {
  printf("%s hr=0x%08lx\n", label, (unsigned long)(uint32_t)hr);
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

static void print_module_path(const char *label, HMODULE module) {
  WCHAR path[MAX_PATH] = {};
  DWORD len = GetModuleFileNameW(module, path, ARRAYSIZE(path));
  printf("HKPresentProbe loaded_%s_path_len=%lu path=", label, len);
  for (DWORD i = 0; i < len; i++)
    printf("%c", path[i] < 128 ? (char)path[i] : '?');
  printf("\n");
}

static HMODULE load_local_dll(LPCWSTR name, const char *label, bool required) {
  SetLastError(0);
  HMODULE module = LoadLibraryExW(name, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  DWORD gle = GetLastError();
  printf("HKPresentProbe LoadLibraryExW(%s) module=%p gle=%lu\n", label, module,
         gle);
  if (module) {
    print_module_path(label, module);
  } else if (required) {
    printf("HKPresentProbe missing_required_dll=%s\n", label);
  }
  return module;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HWND create_probe_window(HINSTANCE instance) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"DXMTHKPresentProbeWindow";

  SetLastError(0);
  ATOM atom = RegisterClassExW(&wc);
  DWORD register_error = GetLastError();
  printf("HKPresentProbe RegisterClassExW atom=%u gle=%lu\n", (unsigned)atom,
         register_error);
  if (!atom && register_error != ERROR_CLASS_ALREADY_EXISTS)
    return nullptr;

  SetLastError(0);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DXMT HK present probe",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr,
                              instance, nullptr);
  printf("HKPresentProbe CreateWindowExW hwnd=%p gle=%lu\n", hwnd,
         GetLastError());

  UINT visible = read_env_uint("DXMT_HK_PRESENT_VISIBLE_WINDOW", 0, 0, 1);
  if (hwnd && visible) {
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
  }
  printf("HKPresentProbe window_visible=%u is_window=%d\n", visible,
         hwnd ? IsWindow(hwnd) : 0);
  return hwnd;
}

static HRESULT create_swapchain(ID3D11Device *device, HWND hwnd,
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

  IDXGIFactory2 *factory = nullptr;
  hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
  adapter->Release();
  if (FAILED(hr))
    return hr;

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

  hr = factory->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr,
                                       swapchain);
  factory->Release();
  return hr;
}

static HRESULT read_first_pixel(ID3D11Device *device, ID3D11DeviceContext *ctx,
                                ID3D11Texture2D *texture, uint8_t bgra[4]) {
  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
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

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  ULONGLONG start_ms = GetTickCount64();
  printf("HKPresentProbe begin width=%u height=%u\n", kWidth, kHeight);

  load_local_dll(L".\\winemetal.dll", "winemetal", false);
  HMODULE dxgi_module = load_local_dll(L".\\dxgi.dll", "dxgi", true);
  if (!dxgi_module)
    return 11;
  HMODULE d3d11_module = load_local_dll(L".\\d3d11.dll", "d3d11", true);
  if (!d3d11_module)
    return 12;

  auto create_device = (D3D11CreateDeviceProc)GetProcAddress(
      d3d11_module, "D3D11CreateDevice");
  printf("HKPresentProbe GetProcAddress(D3D11CreateDevice) proc=%p gle=%lu\n",
         create_device, GetLastError());
  if (!create_device)
    return 13;

  const D3D_FEATURE_LEVEL requested[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0;
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  HRESULT hr = create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested,
                             ARRAYSIZE(requested), D3D11_SDK_VERSION, &device,
                             &chosen, &ctx);
  print_hr("HKPresentProbe D3D11CreateDevice", hr);
  printf("HKPresentProbe feature_level=0x%04x\n", (unsigned)chosen);
  if (FAILED(hr))
    return 3;

  HWND hwnd = create_probe_window(instance);
  if (!hwnd)
    return 4;

  IDXGISwapChain1 *swapchain = nullptr;
  hr = create_swapchain(device, hwnd, &swapchain);
  print_hr("HKPresentProbe CreateSwapChainForHwnd", hr);
  if (FAILED(hr))
    return 5;

  ID3D11Texture2D *backbuffer = nullptr;
  hr = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            (void **)&backbuffer);
  print_hr("HKPresentProbe IDXGISwapChain::GetBuffer", hr);
  if (FAILED(hr))
    return 6;

  ID3D11RenderTargetView *rtv = nullptr;
  hr = device->CreateRenderTargetView(backbuffer, nullptr, &rtv);
  print_hr("HKPresentProbe CreateRenderTargetView", hr);
  if (FAILED(hr))
    return 7;

  const FLOAT color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ctx->ClearRenderTargetView(rtv, color);
  printf("HKPresentProbe ClearRenderTargetView color_rgba=0,1,0,1\n");

  hr = swapchain->Present(0, 0);
  print_hr("HKPresentProbe Present", hr);
  if (FAILED(hr))
    return 8;

  uint8_t pixel[4] = {};
  hr = read_first_pixel(device, ctx, backbuffer, pixel);
  print_hr("HKPresentProbe Readback(Map staging)", hr);
  printf("HKPresentProbe pixel0_bgra=%u,%u,%u,%u\n", pixel[0], pixel[1],
         pixel[2], pixel[3]);

  bool pixel_ok = SUCCEEDED(hr) && pixel[0] <= 2 && pixel[1] >= 235 &&
                  pixel[2] <= 2 && pixel[3] == 255;
  printf("HKPresentProbe pixel_readback=%s\n", pixel_ok ? "PASS" : "FAIL");
  printf("HKPresentProbe elapsed_ms=%llu\n",
         (unsigned long long)(GetTickCount64() - start_ms));
  printf("HKPresentProbe result=%s\n", pixel_ok ? "PASS" : "FAIL");

  rtv->Release();
  backbuffer->Release();
  swapchain->Release();
  ctx->Release();
  device->Release();
  DestroyWindow(hwnd);

  return pixel_ok ? 0 : 9;
}
