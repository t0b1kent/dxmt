#include <windows.h>

#include <d3d9.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const UINT kWidth = 64;
static const UINT kHeight = 64;

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
  printf("D3D9PresentProbe loaded_%s_path_len=%lu path=", label, len);
  for (DWORD i = 0; i < len; i++)
    printf("%c", path[i] < 128 ? (char)path[i] : '?');
  printf("\n");
}

static HMODULE load_local_dll(LPCWSTR name, const char *label, bool required) {
  SetLastError(0);
  HMODULE module = LoadLibraryExW(name, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  DWORD gle = GetLastError();
  printf("D3D9PresentProbe LoadLibraryExW(%s) module=%p gle=%lu\n", label,
         module, gle);
  if (module)
    print_module_path(label, module);
  else if (required)
    printf("D3D9PresentProbe missing_required_dll=%s\n", label);
  return module;
}

struct ProbeVertex {
  float x;
  float y;
  float z;
  float rhw;
  DWORD color;
  float u;
  float v;
};

typedef IDirect3D9 *(*Direct3DCreate9Proc)(UINT sdk_version);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  switch (msg) {
  case WM_CLOSE:
    PostQuitMessage(0);
    return 0;
  case WM_DESTROY:
    return 0;
  default:
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

static HWND create_probe_window(HINSTANCE instance) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"DXMTD3D9PresentProbeWindow";

  SetLastError(0);
  ATOM atom = RegisterClassExW(&wc);
  DWORD register_error = GetLastError();
  printf("D3D9PresentProbe RegisterClassExW atom=%u gle=%lu\n", atom,
         register_error);
  if (!atom && register_error != ERROR_CLASS_ALREADY_EXISTS)
    return nullptr;

  SetLastError(0);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DXMT D3D9 present probe",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, (int)kWidth, (int)kHeight,
                              nullptr, nullptr, instance, nullptr);
  printf("D3D9PresentProbe CreateWindowExW hwnd=%p gle=%lu\n", hwnd,
         GetLastError());

  UINT visible = read_env_uint("D3D9_PRESENT_VISIBLE_WINDOW", 1, 0, 1);
  if (hwnd && visible) {
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
  }
  printf("D3D9PresentProbe window_visible=%u is_window=%d\n", visible,
         hwnd ? IsWindow(hwnd) : 0);
  return hwnd;
}

static void pump_messages() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

static HRESULT create_device(IDirect3D9 *d3d9, HWND hwnd,
                            IDirect3DDevice9 **device) {
  if (!d3d9 || !device)
    return E_INVALIDARG;

  *device = nullptr;
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = kWidth;
  pp.BackBufferHeight = kHeight;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferCount = 1;
  pp.MultiSampleType = D3DMULTISAMPLE_NONE;
  pp.MultiSampleQuality = 0;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;
  pp.Windowed = TRUE;
  pp.EnableAutoDepthStencil = FALSE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  UINT create_flags[] = {
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE,
      D3DCREATE_HARDWARE_VERTEXPROCESSING,
      D3DCREATE_MIXED_VERTEXPROCESSING,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING,
  };

  for (size_t i = 0; i < ARRAYSIZE(create_flags); ++i) {
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   create_flags[i], &pp, device);
    printf("D3D9PresentProbe CreateDevice flags=0x%08x hr=0x%08lx\n",
           create_flags[i], (unsigned long)(uint32_t)hr);
    if (SUCCEEDED(hr))
      return hr;
  }

  return E_FAIL;
}

static HRESULT setup_scene(IDirect3DDevice9 *device,
                          IDirect3DTexture9 **texture) {
  if (!device || !texture)
    return E_INVALIDARG;

  *texture = nullptr;
  HRESULT hr = device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_MANAGED, texture, nullptr);
  if (FAILED(hr)) {
    print_hr("D3D9PresentProbe CreateTexture", hr);
    return hr;
  }

  D3DLOCKED_RECT lock_rect = {};
  hr = (*texture)->LockRect(0, &lock_rect, nullptr, 0);
  if (FAILED(hr))
    return hr;

  DWORD *pixels = (DWORD *)lock_rect.pBits;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      pixels[y * (lock_rect.Pitch / sizeof(DWORD)) + x] =
          D3DCOLOR_ARGB(255, 255, 0, 255);
    }
  }

  hr = (*texture)->UnlockRect(0);
  if (FAILED(hr))
    return hr;

  hr = device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  if (FAILED(hr))
    return hr;
  hr = device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  if (FAILED(hr))
    return hr;
  hr = device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  if (FAILED(hr))
    return hr;
  hr = device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
  if (FAILED(hr))
    return hr;

  return S_OK;
}

static HRESULT draw_colored_triangle(IDirect3DDevice9 *device,
                                    IDirect3DTexture9 *texture) {
  HRESULT hr = device->SetRenderState(D3DRS_LIGHTING, FALSE);
  if (FAILED(hr))
    return hr;
  hr = device->SetRenderState(D3DRS_ZENABLE, FALSE);
  if (FAILED(hr))
    return hr;
  hr = device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  if (FAILED(hr))
    return hr;

  hr = device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
  if (FAILED(hr))
    return hr;
  hr = device->SetTexture(0, texture);
  if (FAILED(hr))
    return hr;

  ProbeVertex vertices[] = {
      {16.0f, 16.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 255), 0.0f, 0.0f},
      {48.0f, 16.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 255), 1.0f, 0.0f},
      {16.0f, 48.0f, 0.0f, 1.0f, D3DCOLOR_XRGB(255, 255, 255), 0.0f,
       1.0f},
  };

  hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(8, 0, 8), 1.0f,
                     0);
  if (FAILED(hr))
    return hr;

  hr = device->BeginScene();
  if (FAILED(hr))
    return hr;

  hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices,
                               sizeof(ProbeVertex));
  if (FAILED(hr)) {
    device->EndScene();
    return hr;
  }

  hr = device->EndScene();
  if (FAILED(hr))
    return hr;

  return S_OK;
}

static HRESULT read_center_pixel(IDirect3DDevice9 *device, uint8_t bgra[4]) {
  IDirect3DSurface9 *backbuffer = nullptr;
  HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
  if (FAILED(hr))
    return hr;

  IDirect3DSurface9 *staging = nullptr;
  hr = device->CreateOffscreenPlainSurface(kWidth, kHeight, D3DFMT_A8R8G8B8,
                                          D3DPOOL_SYSTEMMEM, &staging,
                                          nullptr);
  if (FAILED(hr)) {
    backbuffer->Release();
    return hr;
  }

  hr = device->GetRenderTargetData(backbuffer, staging);
  backbuffer->Release();
  if (FAILED(hr)) {
    staging->Release();
    return hr;
  }

  const int sample_x = (int)(kWidth / 2);
  const int sample_y = (int)(kHeight / 2);
  RECT sample_rect = {sample_x, sample_y, sample_x + 1, sample_y + 1};
  D3DLOCKED_RECT lock = {};
  hr = staging->LockRect(&lock, &sample_rect, D3DLOCK_READONLY);
  if (SUCCEEDED(hr)) {
    const uint8_t *src = (const uint8_t *)lock.pBits;
    bgra[0] = src[0];
    bgra[1] = src[1];
    bgra[2] = src[2];
    bgra[3] = src[3];
    staging->UnlockRect();
  }

  staging->Release();
  return hr;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);

  printf("D3D9PresentProbe begin width=%u height=%u\n", kWidth, kHeight);

  HMODULE d3d9_local_module = load_local_dll(L".\\d3d9.dll", "d3d9", true);
  if (!d3d9_local_module)
    return 11;

  auto direct3d_create =
      (Direct3DCreate9Proc)GetProcAddress(d3d9_local_module, "Direct3DCreate9");
  printf("D3D9PresentProbe GetProcAddress(Direct3DCreate9) proc=%p gle=%lu\n",
         direct3d_create, GetLastError());
  if (!direct3d_create)
    return 12;

  IDirect3D9 *d3d9 = direct3d_create(D3D_SDK_VERSION);
  printf("D3D9PresentProbe Direct3DCreate9 module=%p\n", d3d9);
  if (!d3d9)
    return 13;

  HWND hwnd = create_probe_window(instance);
  if (!hwnd) {
    d3d9->Release();
    return 14;
  }

  IDirect3DDevice9 *device = nullptr;
  HRESULT hr = create_device(d3d9, hwnd, &device);
  print_hr("D3D9PresentProbe CreateDevice", hr);
  if (FAILED(hr)) {
    d3d9->Release();
    DestroyWindow(hwnd);
    return 15;
  }

  IDirect3DTexture9 *texture = nullptr;
  hr = setup_scene(device, &texture);
  print_hr("D3D9PresentProbe setup_scene", hr);
  if (FAILED(hr)) {
    device->Release();
    d3d9->Release();
    DestroyWindow(hwnd);
    return 16;
  }

  hr = draw_colored_triangle(device, texture);
  print_hr("D3D9PresentProbe DrawPrimitiveUP", hr);
  if (FAILED(hr)) {
    texture->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(hwnd);
    return 17;
  }

  hr = device->Present(nullptr, nullptr, nullptr, nullptr);
  print_hr("D3D9PresentProbe Present", hr);
  if (FAILED(hr)) {
    texture->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(hwnd);
    return 18;
  }

  uint8_t pixel[4] = {};
  hr = read_center_pixel(device, pixel);
  print_hr("D3D9PresentProbe Readback(pixel center)", hr);
  printf("D3D9PresentProbe pixel0_bgra=%u,%u,%u,%u\n", pixel[0], pixel[1],
         pixel[2], pixel[3]);

  bool pixel_ok = FAILED(hr);
  if (SUCCEEDED(hr)) {
    bool has_non_background =
        (pixel[3] >= 0x80) &&
        ((int)pixel[0] > 24 || (int)pixel[1] > 24 || (int)pixel[2] > 24);
    bool colorful_pixel =
        ((int)pixel[0] >= 180 && (int)pixel[2] >= 180 &&
         (pixel[2] > pixel[1] + 24 || pixel[0] > pixel[1] + 24));
    pixel_ok = has_non_background && colorful_pixel;
  }

  UINT hold_ms = read_env_uint("D3D9_PRESENT_HOLD_MS", 0, 0, 120000);
  UINT steady_ms = read_env_uint("D3D9_PRESENT_STEADY_MS", 0, 0, 120000);
  if (steady_ms) {
    ULONGLONG end_ms = GetTickCount64() + steady_ms;
    while (GetTickCount64() < end_ms) {
      hr = device->Present(nullptr, nullptr, nullptr, nullptr);
      if (FAILED(hr))
        break;
      pump_messages();
      Sleep(16);
    }
  }

  if (hold_ms)
    Sleep(hold_ms);

  printf("D3D9PresentProbe result=%s\n", pixel_ok ? "PASS" : "FAIL");

  texture->Release();
  device->Release();
  d3d9->Release();
  DestroyWindow(hwnd);

  return pixel_ok ? 0 : 19;
}
