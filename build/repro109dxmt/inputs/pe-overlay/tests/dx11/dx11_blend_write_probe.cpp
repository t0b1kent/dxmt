#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

template <typename T> static void release(T *&object) {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

static bool check_hr(const char *label, HRESULT hr) {
  printf("blend-write-probe: %s hr=0x%08lx %s\n", label,
         (unsigned long)hr, SUCCEEDED(hr) ? "PASS" : "FAIL");
  return SUCCEEDED(hr);
}

static bool compile_shader(const char *source, const char *entry,
                           const char *target, ID3DBlob **blob) {
  ID3DBlob *errors = nullptr;
  HRESULT hr = D3DCompile(source, strlen(source), "blend-write-probe", nullptr,
                          nullptr, entry, target, 0, 0, blob, &errors);
  if (errors) {
    fprintf(stderr, "%.*s\n", (int)errors->GetBufferSize(),
            (const char *)errors->GetBufferPointer());
    errors->Release();
  }
  return check_hr(target, hr);
}

static uint8_t unorm8(float value) {
  return (uint8_t)lroundf(value * 255.0f);
}

static bool close_byte(uint8_t actual, uint8_t expected) {
  return actual + 1 >= expected && actual <= expected + 1;
}

struct BlendCase {
  const char *name;
  UINT8 write_mask;
  BOOL blend_enable;
  D3D11_BLEND src_rgb;
  D3D11_BLEND dst_rgb;
  D3D11_BLEND_OP rgb_op;
  D3D11_BLEND src_alpha;
  D3D11_BLEND dst_alpha;
  D3D11_BLEND_OP alpha_op;
  float clear[4];
  float expected[4];
};

static bool run_case(ID3D11Device *device, ID3D11DeviceContext *context,
                     ID3D11RenderTargetView *rtv, ID3D11Texture2D *render_target,
                     ID3D11Texture2D *staging, const BlendCase &test) {
  D3D11_BLEND_DESC desc = {};
  desc.RenderTarget[0].RenderTargetWriteMask = test.write_mask;
  desc.RenderTarget[0].BlendEnable = test.blend_enable;
  desc.RenderTarget[0].SrcBlend = test.src_rgb;
  desc.RenderTarget[0].DestBlend = test.dst_rgb;
  desc.RenderTarget[0].BlendOp = test.rgb_op;
  desc.RenderTarget[0].SrcBlendAlpha = test.src_alpha;
  desc.RenderTarget[0].DestBlendAlpha = test.dst_alpha;
  desc.RenderTarget[0].BlendOpAlpha = test.alpha_op;

  ID3D11BlendState *blend = nullptr;
  HRESULT hr = device->CreateBlendState(&desc, &blend);
  if (!check_hr(test.name, hr))
    return false;

  context->ClearRenderTargetView(rtv, test.clear);
  context->OMSetBlendState(blend, nullptr, 0xffffffffu);
  context->Draw(3, 0);
  context->CopyResource(staging, render_target);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (!check_hr("Map", hr)) {
    release(blend);
    return false;
  }
  const uint8_t *pixel = (const uint8_t *)mapped.pData + mapped.RowPitch + 4;
  bool pass = true;
  for (unsigned channel = 0; channel < 4; channel++)
    pass &= close_byte(pixel[channel], unorm8(test.expected[channel]));
  printf("blend-write-probe: case=%s rgba=%u,%u,%u,%u expected=%u,%u,%u,%u %s\n",
         test.name, pixel[0], pixel[1], pixel[2], pixel[3],
         unorm8(test.expected[0]), unorm8(test.expected[1]),
         unorm8(test.expected[2]), unorm8(test.expected[3]),
         pass ? "PASS" : "FAIL");
  context->Unmap(staging, 0);
  release(blend);
  return pass;
}

int main() {
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  D3D_FEATURE_LEVEL level;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &device, &level,
                                 &context);
  if (!check_hr("D3D11CreateDevice", hr))
    return 1;

  static const char shader_source[] =
      "float4 vs(uint id : SV_VertexID) : SV_Position {"
      " float2 p[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};"
      " return float4(p[id],0,1); }"
      "float4 ps() : SV_Target0 { return float4(1,0,1,0.5); }";
  ID3DBlob *vs_blob = nullptr, *ps_blob = nullptr;
  if (!compile_shader(shader_source, "vs", "vs_5_0", &vs_blob) ||
      !compile_shader(shader_source, "ps", "ps_5_0", &ps_blob))
    return 1;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  if (!check_hr("CreateVertexShader",
                device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                           vs_blob->GetBufferSize(), nullptr, &vs)) ||
      !check_hr("CreatePixelShader",
                device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                          ps_blob->GetBufferSize(), nullptr, &ps)))
    return 1;

  D3D11_TEXTURE2D_DESC texture_desc = {};
  texture_desc.Width = 4;
  texture_desc.Height = 4;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *render_target = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  if (!check_hr("CreateTexture2D(render)", device->CreateTexture2D(&texture_desc, nullptr, &render_target)) ||
      !check_hr("CreateRenderTargetView", device->CreateRenderTargetView(render_target, nullptr, &rtv)))
    return 1;
  texture_desc.BindFlags = 0;
  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging = nullptr;
  if (!check_hr("CreateTexture2D(staging)", device->CreateTexture2D(&texture_desc, nullptr, &staging)))
    return 1;

  D3D11_VIEWPORT viewport = {0, 0, 4, 4, 0, 1};
  context->RSSetViewports(1, &viewport);
  context->OMSetRenderTargets(1, &rtv, nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vs, nullptr, 0);
  context->PSSetShader(ps, nullptr, 0);

  const BlendCase cases[] = {
      {"all-no-blend", D3D11_COLOR_WRITE_ENABLE_ALL, FALSE,
       D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
       D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
       {0.25f, 0.5f, 0.75f, 0.25f}, {1, 0, 1, 0.5f}},
      {"alpha-only", D3D11_COLOR_WRITE_ENABLE_ALPHA, FALSE,
       D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
       D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
       {0.25f, 0.5f, 0.75f, 0.25f}, {0.25f, 0.5f, 0.75f, 0.5f}},
      {"rgb-only", D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                       D3D11_COLOR_WRITE_ENABLE_BLUE,
       FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
       D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
       {0.25f, 0.5f, 0.75f, 0.25f}, {1, 0, 1, 0.25f}},
      {"alpha-blend", D3D11_COLOR_WRITE_ENABLE_ALL, TRUE,
       D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
       D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
       {0, 0.5f, 0, 1}, {0.5f, 0.25f, 0.5f, 0.5f}},
  };

  bool pass = true;
  for (const BlendCase &test : cases)
    pass &= run_case(device, context, rtv, render_target, staging, test);

  release(staging);
  release(rtv);
  release(render_target);
  release(ps);
  release(vs);
  release(ps_blob);
  release(vs_blob);
  release(context);
  release(device);
  printf("blend-write-probe: RESULT=%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
