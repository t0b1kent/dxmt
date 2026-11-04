// Own descriptor controls; the existing pixel-slot-index.exe remains the pixel gate.
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
template<class T> struct Com {
  T *p = nullptr;
  ~Com() { if (p) p->Release(); }
  T *operator->() const { return p; }
  void **put() { return reinterpret_cast<void **>(&p); }
};
static void check(HRESULT hr, const char *stage) {
  if (FAILED(hr)) { std::printf("WALL %s hr=%08lx\n", stage, (unsigned long)hr); throw std::runtime_error(stage); }
}
static HRESULT compile(const char *source, const char *profile, Com<ID3DBlob> &out) {
  Com<ID3DBlob> errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), "own-pso-state", nullptr, nullptr, "main", profile, 0, 0,
      reinterpret_cast<ID3DBlob **>(out.put()), reinterpret_cast<ID3DBlob **>(errors.put()));
  if (FAILED(hr) && errors.p) std::printf("COMPILER %.*s\n", int(errors->GetBufferSize()), static_cast<const char *>(errors->GetBufferPointer()));
  return hr;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  try {
    Com<ID3D12Device> device;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, device.put()), "device");
    const char vs[] = "cbuffer C : register(b10) { float4 scale; }; float4 main(float2 p : POSITION) : SV_Position { return float4(p*scale.x,0,1); }";
    const char ps[] = "float4 main() : SV_Target { return float4(1,0,0,1); }";
    const char ps_other[] = "float4 main() : SV_Target { return float4(0,1,0,1); }";
    Com<ID3DBlob> vertex, pixel, other;
    check(compile(vs, "vs_5_0", vertex), "vs"); check(compile(ps, "ps_5_0", pixel), "ps");
    check(compile(ps_other, "ps_5_0", other), "ps_other");
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 2, 4, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 7, 4, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 5};
    D3D12_ROOT_PARAMETER1 parameters[3] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[0].Constants = {10, 0, 4};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].Descriptor = {3, 2, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderRegister = 5; sampler.RegisterSpace = 2;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd = {};
    rd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rd.Desc_1_1 = {3, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    Com<ID3DBlob> blob, changed_blob;
    check(D3D12SerializeVersionedRootSignature(&rd, reinterpret_cast<ID3DBlob **>(blob.put()), nullptr), "serialize11");
    parameters[0].Constants.Num32BitValues = 8;
    check(D3D12SerializeVersionedRootSignature(&rd, reinterpret_cast<ID3DBlob **>(changed_blob.put()), nullptr), "serialize_changed");
    Com<ID3D12RootSignature> roots[3];
    for (unsigned i = 0; i < 3; ++i) {
      auto *source = i == 2 ? changed_blob.p : blob.p;
      check(device->CreateRootSignature(0, source->GetBufferPointer(), source->GetBufferSize(),
                                       IID_ID3D12RootSignature, roots[i].put()), "root");
    }
    // Independent addresses and descriptor padding must not change the fingerprint.
    char semantic_a[] = "POSITION", semantic_b[] = "POSITION";
    D3D12_INPUT_ELEMENT_DESC elements[2] = {{semantic_a, 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                                           {semantic_b, 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC base = {};
    base.pRootSignature = roots[0].p; base.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    base.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()}; base.SampleMask = UINT32_MAX;
    base.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; base.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    base.RasterizerState.DepthClipEnable = TRUE;
    base.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    base.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    base.NumRenderTargets = 1; base.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    base.SampleDesc.Count = 1; base.InputLayout = {elements, 1};
    for (unsigned i = 0; i < 6; ++i) {
      auto d = base;
      if (i == 1) { d.pRootSignature = roots[1].p; d.InputLayout.pInputElementDescs = elements + 1; }
      if (i == 2) { elements[1].AlignedByteOffset = 4; d.InputLayout.pInputElementDescs = elements + 1; }
      if (i == 3) d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
      if (i == 4) d.pRootSignature = roots[2].p;
      if (i == 5) d.PS = {other->GetBufferPointer(), other->GetBufferSize()};
      Com<ID3D12PipelineState> pso;
      check(device->CreateGraphicsPipelineState(&d, IID_ID3D12PipelineState, pso.put()), "state_control_pso");
      std::printf("STATE_CONTROL index=%u hr=00000000\n", i);
    }
    std::printf("STATE_RESULT controls=6 status=PASS\n");
    return 0;
  } catch (const std::exception &error) {
    std::printf("PRODUCT_WALL %s\n", error.what()); return 2;
  }
}
