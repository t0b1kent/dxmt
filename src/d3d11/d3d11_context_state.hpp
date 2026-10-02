#pragma once
#include <utility>

#include "d3d11_device_child.hpp"

#include "com/com_pointer.hpp"
#include "d3d11_input_layout.hpp"
#include "d3d11_shader.hpp"
#include "d3d11_state_object.hpp"
#include "d3d11_view.hpp"
#include "dxmt_binding_set.hpp"
#include "log/log.hpp"
#include "d3d11_resource.hpp"
#include "util_string.hpp"

namespace dxmt {

struct UAV_B {
  IUnknown *RawPointer = 0;
  Com<D3D11UnorderedAccessView, false> View;
};

typedef BindingSet<UAV_B, 64> UAVBindingSet;

template <> struct redundant_binding_trait<UAV_B> {
  static bool is_redundant(const UAV_B &left, const UAV_B &right) {
    return left.RawPointer == right.RawPointer;
  }
};

struct D3D11ComputeStageState {
  UAVBindingSet UAVs;
};

struct CONSTANT_BUFFER_B {
  IUnknown *RawPointer = 0;
  Com<D3D11ResourceCommon, false> Buffer;
  UINT FirstConstant;
  UINT NumConstants;
};

typedef BindingSet<CONSTANT_BUFFER_B, 14> ConstantBufferBindingSet;

template <> struct redundant_binding_trait<CONSTANT_BUFFER_B> {
  static bool is_redundant(const CONSTANT_BUFFER_B &left,
                          const CONSTANT_BUFFER_B &right) {
    return left.RawPointer == right.RawPointer;
  }
};

struct SAMPLER_B {
  IUnknown *RawPointer = 0;
  D3D11SamplerState* Sampler;
};

typedef BindingSet<SAMPLER_B, 16> SamplerBindingSet;

template <> struct redundant_binding_trait<SAMPLER_B> {
  static bool is_redundant(const SAMPLER_B &left, const SAMPLER_B &right) {
    return left.RawPointer == right.RawPointer;
  }
};

struct SRV_B {
  IUnknown *RawPointer = 0;
  Com<D3D11ShaderResourceView, false> SRV;
};

typedef BindingSet<SRV_B, 128> SRVBindingSet;

template <> struct redundant_binding_trait<SRV_B> {
  static bool is_redundant(const SRV_B &left, const SRV_B &right) {
    return left.RawPointer == right.RawPointer;
  }
};

struct D3D11ShaderStageState {
  SRVBindingSet SRVs;
  SamplerBindingSet Samplers;
  ConstantBufferBindingSet ConstantBuffers;
  Com<IMTLD3D11Shader> Shader;
};

struct VERTEX_BUFFER_B {
  IUnknown *RawPointer = 0;
  Com<D3D11ResourceCommon, false> Buffer;
  UINT Stride;
  UINT Offset;
};

template <> struct redundant_binding_trait<VERTEX_BUFFER_B> {
  static bool is_redundant(const VERTEX_BUFFER_B &left,
                          const VERTEX_BUFFER_B &right) {
    return left.RawPointer == right.RawPointer;
  }
};

struct D3D11InputAssemblerStageState {
  Com<IMTLD3D11InputLayout> InputLayout;
  BindingSet<VERTEX_BUFFER_B, 16> VertexBuffers;
  Com<D3D11ResourceCommon, false> IndexBuffer;
  /**
  either DXGI_FORMAT_R16_UINT or DXGI_FORMAT_R32_UINT
  */
  DXGI_FORMAT IndexBufferFormat;
  UINT IndexBufferOffset;
  D3D11_PRIMITIVE_TOPOLOGY Topology;
};

struct D3D11OutputMergerStageState {
  Com<D3D11RenderTargetView, false> RTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
  Com<D3D11DepthStencilView, false> DSV;
  UINT NumRTVs;

  UAVBindingSet UAVs;
  UINT MinUAVBinding = D3D11_1_UAV_SLOT_COUNT;
  UINT MaxUAVBinding = 0;

  IMTLD3D11DepthStencilState* DepthStencilState;
  UINT StencilRef;

  IMTLD3D11BlendState* BlendState;
  FLOAT BlendFactor[4];

  UINT SampleMask = 0xffffffff;

  // state derived from valid RTV/DSV
  UINT SampleCount = 1;
  UINT ArrayLength = 0;
  UINT RenderTargetWidth = 0;
  UINT RenderTargetHeight = 0;
};

struct STREAM_OUTPUT_BUFFER_B {
  IUnknown *RawPointer = 0;
  Com<D3D11ResourceCommon, false> Buffer;
  UINT Offset;
};

template <> struct redundant_binding_trait<STREAM_OUTPUT_BUFFER_B> {
  static bool is_redundant(const STREAM_OUTPUT_BUFFER_B &left,
                          const STREAM_OUTPUT_BUFFER_B &right) {
    return left.RawPointer == right.RawPointer;
  }
};

struct D3D11StreamOutputStageState {
  BindingSet<STREAM_OUTPUT_BUFFER_B, 4> Targets;
};

struct D3D11RasterizerStageState {
  D3D11_RECT
  scissor_rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {
      {}};
  D3D11_VIEWPORT
  viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {{}};
  UINT NumScissorRects;
  UINT NumViewports;
  IMTLD3D11RasterizerState* RasterizerState;
};

template <typename T> class D3D11StagesState: public std::array<T, 6> {
public:
  T &
  operator[](PipelineStage stage) {
    return this->data()[(uint32_t)stage];
  }

  const T &
  operator[](PipelineStage stage) const {
    return this->data()[(uint32_t)stage];
  }
};

struct D3D11ContextState {
  D3D11StagesState<D3D11ShaderStageState> ShaderStages = {{}};
  D3D11ComputeStageState ComputeStageUAV = {};
  D3D11StreamOutputStageState StreamOutput = {};
  D3D11InputAssemblerStageState InputAssembler = {};
  D3D11OutputMergerStageState OutputMerger = {};
  D3D11RasterizerStageState Rasterizer = {};

  Com<ID3D11Predicate> predicate = nullptr;
  BOOL predicate_value = FALSE;
};

template <typename Element, size_t NumElements>
static BindingSet<Element, NumElements>
CloneBindingSet(const BindingSet<Element, NumElements> &src) {
  BindingSet<Element, NumElements> dst;
  for (const auto &[slot, binding] : src) {
    bool replacement = false;
    dst.bind(slot, Element(binding), replacement);
  }
  return dst;
}

static D3D11ContextState
CloneD3D11ContextState(const D3D11ContextState &src) {
  D3D11ContextState dst = {};

  for (size_t i = 0; i < dst.ShaderStages.size(); i++) {
    dst.ShaderStages.data()[i].SRVs =
        CloneBindingSet(src.ShaderStages.data()[i].SRVs);
    dst.ShaderStages.data()[i].Samplers =
        CloneBindingSet(src.ShaderStages.data()[i].Samplers);
    dst.ShaderStages.data()[i].ConstantBuffers =
        CloneBindingSet(src.ShaderStages.data()[i].ConstantBuffers);
    dst.ShaderStages.data()[i].Shader = src.ShaderStages.data()[i].Shader;
  }

  dst.ComputeStageUAV.UAVs = CloneBindingSet(src.ComputeStageUAV.UAVs);
  dst.StreamOutput.Targets = CloneBindingSet(src.StreamOutput.Targets);

  dst.InputAssembler.InputLayout = src.InputAssembler.InputLayout;
  dst.InputAssembler.VertexBuffers =
      CloneBindingSet(src.InputAssembler.VertexBuffers);
  dst.InputAssembler.IndexBuffer = src.InputAssembler.IndexBuffer;
  dst.InputAssembler.IndexBufferFormat = src.InputAssembler.IndexBufferFormat;
  dst.InputAssembler.IndexBufferOffset = src.InputAssembler.IndexBufferOffset;
  dst.InputAssembler.Topology = src.InputAssembler.Topology;

  for (size_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
    dst.OutputMerger.RTVs[i] = src.OutputMerger.RTVs[i];
  dst.OutputMerger.DSV = src.OutputMerger.DSV;
  dst.OutputMerger.NumRTVs = src.OutputMerger.NumRTVs;
  dst.OutputMerger.UAVs = CloneBindingSet(src.OutputMerger.UAVs);
  dst.OutputMerger.MinUAVBinding = src.OutputMerger.MinUAVBinding;
  dst.OutputMerger.MaxUAVBinding = src.OutputMerger.MaxUAVBinding;
  dst.OutputMerger.DepthStencilState = src.OutputMerger.DepthStencilState;
  dst.OutputMerger.StencilRef = src.OutputMerger.StencilRef;
  dst.OutputMerger.BlendState = src.OutputMerger.BlendState;
  for (size_t i = 0; i < 4; i++)
    dst.OutputMerger.BlendFactor[i] = src.OutputMerger.BlendFactor[i];
  dst.OutputMerger.SampleMask = src.OutputMerger.SampleMask;
  dst.OutputMerger.SampleCount = src.OutputMerger.SampleCount;
  dst.OutputMerger.ArrayLength = src.OutputMerger.ArrayLength;
  dst.OutputMerger.RenderTargetWidth = src.OutputMerger.RenderTargetWidth;
  dst.OutputMerger.RenderTargetHeight = src.OutputMerger.RenderTargetHeight;

  for (size_t i = 0; i < D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
       i++) {
    dst.Rasterizer.scissor_rects[i] = src.Rasterizer.scissor_rects[i];
    dst.Rasterizer.viewports[i] = src.Rasterizer.viewports[i];
  }
  dst.Rasterizer.NumScissorRects = src.Rasterizer.NumScissorRects;
  dst.Rasterizer.NumViewports = src.Rasterizer.NumViewports;
  dst.Rasterizer.RasterizerState = src.Rasterizer.RasterizerState;

  dst.predicate = src.predicate;
  dst.predicate_value = src.predicate_value;

  return dst;
}

class MTLD3D11DeviceContextState
    : public MTLD3D11DeviceChild<ID3DDeviceContextState> {

public:
  MTLD3D11DeviceContextState(MTLD3D11Device *pDevice)
      : MTLD3D11DeviceChild<ID3DDeviceContextState>(pDevice) {}
  MTLD3D11DeviceContextState(MTLD3D11Device *pDevice,
                             const D3D11ContextState &state)
      : MTLD3D11DeviceChild<ID3DDeviceContextState>(pDevice),
        context_state_(CloneD3D11ContextState(state)) {}

  ~MTLD3D11DeviceContextState() {}

  const D3D11ContextState &contextState() const {
    return context_state_;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3DDeviceContextState)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3DDeviceContextState), riid)) {
      WARN("D3DDeviceContextState: Unknown interface query ",
           str::format(riid));
    }

    return E_NOINTERFACE;
  }

private:
  D3D11ContextState context_state_ = {};
};

} // namespace dxmt
