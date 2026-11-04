#include "Metal.hpp"
#include "d3d11_private.h"
#include "d3d11_pipeline.hpp"
#include "d3d11_device.hpp"
#include "d3d11_shader.hpp"
#include "log/log.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dxmt {

bool MacRunnerFragmentOutputProbeEnabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("MACRUNNER_HB_FRAGMENT_OUTPUT_PROBE");
    return value && value[0] && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

static bool macrunner_fragment_output_probe_take_slot() {
  static std::atomic_uint count{0};
  if (!MacRunnerFragmentOutputProbeEnabled())
    return false;
  static const unsigned limit = [] {
    const char *value = std::getenv("MACRUNNER_HB_FRAGMENT_OUTPUT_PROBE_MAX");
    char *end = nullptr;
    unsigned long parsed = value && value[0] ? std::strtoul(value, &end, 0) : 0;
    return static_cast<unsigned>(end && end != value && parsed > 0 && parsed <= 4096
                                     ? parsed
                                     : 64);
  }();
  return count.fetch_add(1, std::memory_order_relaxed) < limit;
}

void MacRunnerFragmentOutputProbeLogBind(uint64_t pso_id,
                                         DXGI_FORMAT d3d_rt0_format) {
  if (!macrunner_fragment_output_probe_take_slot())
    return;
  std::fprintf(stderr,
               "macrunner-hb-fragment-output: side=d3d11 phase=bind "
               "pso_id=0x%016llx d3d_rt0_format=%u\n",
               static_cast<unsigned long long>(pso_id), d3d_rt0_format);
  std::fflush(stderr);
}

static bool macrunner_render_pipeline_probe_enabled() {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = std::getenv("MACRUNNER_HB_RENDER_PIPELINE_PROBE");
    enabled = value && value[0] && std::strcmp(value, "0") != 0;
  }
  return enabled != 0;
}

static bool macrunner_render_pipeline_probe_take_slot() {
  static std::atomic_uint count{0};
  static unsigned limit = 0;
  if (!macrunner_render_pipeline_probe_enabled())
    return false;
  if (!limit) {
    const char *value = std::getenv("MACRUNNER_HB_RENDER_PIPELINE_PROBE_MAX");
    char *end = nullptr;
    unsigned long parsed = value && value[0] ? std::strtoul(value, &end, 0) : 0;
    limit = end && end != value && parsed > 0 && parsed <= 1000000 ? parsed : 8192;
  }
  return count.fetch_add(1, std::memory_order_relaxed) < limit;
}

static void macrunner_render_pipeline_probe_log(
    const char *phase, const void *pipeline, const void *vertex_shader,
    const void *pixel_shader, const void *state, unsigned num_rtvs,
    unsigned depth_format, unsigned topology, unsigned sample_count,
    unsigned rasterization_enabled, const char *detail) {
  if (!macrunner_render_pipeline_probe_take_slot())
    return;
  std::fprintf(stderr,
               "macrunner-hb-render-pipeline: layer=dxmt-pso phase=%s "
               "tid=%lu pipeline=%p vs=%p ps=%p state=%p rtvs=%u depth=%u "
               "topology=%u samples=%u raster=%u detail=%s\n",
               phase, static_cast<unsigned long>(GetCurrentThreadId()), pipeline,
               vertex_shader, pixel_shader, state, num_rtvs, depth_format,
               topology, sample_count, rasterization_enabled,
               detail ? detail : "-");
  std::fflush(stderr);
}

class MTLCompiledGraphicsPipelineImpl
    : public MTLCompiledGraphicsPipeline {
public:
  MTLCompiledGraphicsPipelineImpl(MTLD3D11Device *pDevice,
                              MTL_GRAPHICS_PIPELINE_DESC *pDesc)
      : num_rtvs(pDesc->NumColorAttachments),
        depth_stencil_format(pDesc->DepthStencilFormat),
        topology_class(pDesc->TopologyClass), device_(pDevice),
        pBlendState(pDesc->BlendState),
        RasterizationEnabled(pDesc->RasterizationEnabled),
        SampleCount(pDesc->SampleCount) {
    uint32_t unorm_output_reg_mask = 0;
    for (unsigned i = 0; i < num_rtvs; i++) {
      rtv_formats[i] = pDesc->ColorAttachmentFormats[i];
      unorm_output_reg_mask |= (uint32_t(IsUnorm8RenderTargetFormat(pDesc->ColorAttachmentFormats[i])) << i);
    }

    if (pDesc->SOLayout) {
      VertexShader =
          pDesc->VertexShader->get_shader(ShaderVariantVertexStreamOutput{
              pDesc->InputLayout, (uint64_t)pDesc->SOLayout});
    } else {
      VertexShader = pDesc->VertexShader->get_shader(ShaderVariantVertex{
          pDesc->InputLayout, pDesc->GSPassthrough, !pDesc->RasterizationEnabled});
    }

    if (pDesc->PixelShader) {
      PixelShader = pDesc->PixelShader->get_shader(ShaderVariantPixel{
          pDesc->SampleMask, pDesc->BlendState->IsDualSourceBlending(),
          depth_stencil_format == WMTPixelFormatInvalid,
          unorm_output_reg_mask});
      ps_valid_render_targets = pDesc->PixelShader->reflection().PSValidRenderTargets;
    } else {
      PixelShader = nullptr;
      ps_valid_render_targets = 0;
    }
    auto &vs_reflection = pDesc->VertexShader->reflection();
    vs_constant_buffers = vs_reflection.NumConstantBuffers;
    vs_arguments = vs_reflection.NumArguments;
    if (pDesc->PixelShader) {
      auto &ps_reflection = pDesc->PixelShader->reflection();
      ps_constant_buffers = ps_reflection.NumConstantBuffers;
      ps_arguments = ps_reflection.NumArguments;
    } else {
      ps_constant_buffers = 0;
      ps_arguments = 0;
    }
    macrunner_render_pipeline_probe_log(
        "create", this, VertexShader, PixelShader, nullptr, num_rtvs,
        depth_stencil_format, topology_class, SampleCount,
        RasterizationEnabled, "descriptor-ready");
  }

  void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pPipeline) final {
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {state_, macrunner_probe_id_};
    macrunner_render_pipeline_probe_log(
        state_ ? "get-ready" : "get-null", this, VertexShader, PixelShader,
        reinterpret_cast<const void *>(static_cast<uintptr_t>(state_.handle)),
        num_rtvs, depth_stencil_format, topology_class,
        SampleCount, RasterizationEnabled,
        state_ ? "setpso-may-encode" : "setpso-will-be-omitted");
  }

  ThreadpoolWork *RunThreadpoolWork() {

    TRACE("Start compiling 1 PSO");

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER vs, ps;
    if (!VertexShader->GetShader(&vs)) {
      macrunner_render_pipeline_probe_log(
          "wait-vs", this, VertexShader, PixelShader, nullptr, num_rtvs,
          depth_stencil_format, topology_class, SampleCount,
          RasterizationEnabled, "vertex-shader-not-ready");
      return VertexShader;
    }
    if (PixelShader && !PixelShader->GetShader(&ps)) {
      macrunner_render_pipeline_probe_log(
          "wait-ps", this, VertexShader, PixelShader, nullptr, num_rtvs,
          depth_stencil_format, topology_class, SampleCount,
          RasterizationEnabled, "pixel-shader-not-ready");
      return PixelShader;
    }

    WMTRenderPipelineInfo info;
    WMT::InitializeRenderPipelineInfo(info);

    info.vertex_function = vs.Function;
    info.causal_vertex_output_function = vs.CausalVariantFunction;

    if (PixelShader) {
      info.fragment_function = ps.Function;
      info.causal_fragment_capture_function = ps.CausalFragmentCaptureFunction;
      info.causal_fragment_magenta_function = ps.CausalFragmentMagentaFunction;
      info.causal_fragment_input_function = ps.CausalVariantFunction;
    }
    info.rasterization_enabled = RasterizationEnabled;

    for (unsigned i = 0; i < num_rtvs; i++) {
      if (rtv_formats[i] == WMTPixelFormatInvalid)
        continue;
      info.colors[i].pixel_format = rtv_formats[i];
    }

    if (depth_stencil_format != WMTPixelFormatInvalid) {
      info.depth_pixel_format = depth_stencil_format;
    }
    if (DepthStencilPlanarFlags(depth_stencil_format) & 2) {
      info.stencil_pixel_format = depth_stencil_format;
    }

    if (pBlendState) {
      pBlendState->SetupMetalPipelineDescriptor((WMTRenderPipelineBlendInfo *)&info, num_rtvs, ps_valid_render_targets);
    }

    info.input_primitive_topology = topology_class;
    info.raster_sample_count = SampleCount;
    info.immutable_vertex_buffers = (1 << 16) | (1 << 29) | (1 << 30);
    info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

    const char *vs_name = VertexShader->GetFunctionName();
    const char *ps_name = PixelShader ? PixelShader->GetFunctionName() : "";
    macrunner_probe_id_ = WMTComputeRenderPipelineProbeID(&info, vs_name, ps_name);
    if (macrunner_fragment_output_probe_take_slot()) {
      D3D11_BLEND_DESC1 blend_desc{};
      if (pBlendState) {
        pBlendState->GetDesc1(&blend_desc);
      } else {
        auto &default_rt0 = blend_desc.RenderTarget[0];
        default_rt0.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        default_rt0.SrcBlend = D3D11_BLEND_ONE;
        default_rt0.DestBlend = D3D11_BLEND_ZERO;
        default_rt0.BlendOp = D3D11_BLEND_OP_ADD;
        default_rt0.SrcBlendAlpha = D3D11_BLEND_ONE;
        default_rt0.DestBlendAlpha = D3D11_BLEND_ZERO;
        default_rt0.BlendOpAlpha = D3D11_BLEND_OP_ADD;
      }
      const auto &rt0 = blend_desc.RenderTarget[0];
      std::fprintf(
          stderr,
          "macrunner-hb-fragment-output: side=d3d11 phase=create "
          "pso_id=0x%016llx vs=%s ps=%s ps_hash=%016llx "
          "PSValidRenderTargets=0x%x wmt_rt0_format=%u d3d_blend_state=%u "
          "d3d_write_mask=0x%x d3d_blend=%u d3d_rgb_op=%u d3d_alpha_op=%u "
          "d3d_src_rgb=%u d3d_dst_rgb=%u d3d_src_alpha=%u d3d_dst_alpha=%u\n",
          static_cast<unsigned long long>(macrunner_probe_id_), vs_name,
          ps_name, static_cast<unsigned long long>(WMTComputeStringProbeHash(ps_name)),
          ps_valid_render_targets, info.colors[0].pixel_format,
          pBlendState != nullptr, rt0.RenderTargetWriteMask,
          rt0.BlendEnable, rt0.BlendOp,
          rt0.BlendOpAlpha, rt0.SrcBlend, rt0.DestBlend,
          rt0.SrcBlendAlpha, rt0.DestBlendAlpha);
      std::fflush(stderr);
    }

    state_ = device_->GetMTLDevice().newRenderPipelineState(info, err);

    if (state_ == nullptr) {
      auto err_text = err ? err.description().getUTF8String() : std::string("<nil NSError>");
      auto ps_function = PixelShader ? ps.Function.handle : 0;
      macrunner_render_pipeline_probe_log(
          "metal-fail", this, VertexShader, PixelShader, nullptr, num_rtvs,
          depth_stencil_format, topology_class, SampleCount,
          RasterizationEnabled, err_text.c_str());
      ERR("Failed to create PSO: ", err_text,
          " vs_fn=", vs.Function.handle,
          " ps_fn=", ps_function,
          " rtvs=", num_rtvs,
          " ps_valid_rt=", ps_valid_render_targets,
          " depth=", depth_stencil_format,
          " topology=", topology_class,
          " sample_count=", SampleCount,
          " raster=", RasterizationEnabled,
          " vs_cb=", vs_constant_buffers,
          " vs_args=", vs_arguments,
          " ps_cb=", ps_constant_buffers,
          " ps_args=", ps_arguments,
          " immutable_vbuf=", info.immutable_vertex_buffers,
          " immutable_fbuf=", info.immutable_fragment_buffers);
      return this;
    }

    macrunner_render_pipeline_probe_log(
        "metal-ok", this, VertexShader, PixelShader,
        reinterpret_cast<const void *>(static_cast<uintptr_t>(state_.handle)), num_rtvs,
        depth_stencil_format, topology_class, SampleCount,
        RasterizationEnabled, "compiled");

    TRACE("Compiled 1 PSO");

    return this;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  UINT num_rtvs;
  UINT ps_valid_render_targets;
  WMTPixelFormat rtv_formats[8];
  WMTPixelFormat depth_stencil_format;
  WMTPrimitiveTopologyClass topology_class;
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  CompiledShader *VertexShader;
  CompiledShader *PixelShader;
  IMTLD3D11BlendState *pBlendState;
  WMT::Reference<WMT::RenderPipelineState> state_;
  bool RasterizationEnabled;
  UINT SampleCount;
  UINT vs_constant_buffers;
  UINT vs_arguments;
  UINT ps_constant_buffers;
  UINT ps_arguments;
  uint64_t macrunner_probe_id_ = 0;
};

std::unique_ptr<MTLCompiledGraphicsPipeline>
CreateGraphicsPipeline(MTLD3D11Device *pDevice,
                       MTL_GRAPHICS_PIPELINE_DESC *pDesc) {
  return std::make_unique<MTLCompiledGraphicsPipelineImpl>(pDevice, pDesc);
}

class MTLCompiledComputePipelineImpl
    : public MTLCompiledComputePipeline {
public:
  MTLCompiledComputePipelineImpl(MTLD3D11Device *pDevice, ManagedShader shader)
      : device_(pDevice) {
    ComputeShader = shader->get_shader(ShaderVariantDefault{});
    uint32_t total_tgsize = shader->reflection().ThreadgroupSize[0] *
                            shader->reflection().ThreadgroupSize[1] *
                            shader->reflection().ThreadgroupSize[2];
    // FIXME: might be different on AMD GPU, if it's ever supported
    tgsize_is_multiple_of_sgwidth = (total_tgsize % 32) == 0;
  }

  void GetPipeline(MTL_COMPILED_COMPUTE_PIPELINE *pPipeline) final {
    ready_.wait(false, std::memory_order_acquire);
    *pPipeline = {state_};
  }

  ThreadpoolWork *RunThreadpoolWork() {

    TRACE("Start compiling 1 PSO");

    WMT::Reference<WMT::Error> err;
    MTL_COMPILED_SHADER cs;
    if (!ComputeShader->GetShader(&cs)) {
      return ComputeShader;
    }

    WMTComputePipelineInfo info;
    WMT::InitializeComputePipelineInfo(info);

    info.compute_function = cs.Function;
    info.tgsize_is_multiple_of_sgwidth = tgsize_is_multiple_of_sgwidth;
    info.immutable_buffers = (1 << 29) | (1 << 30);

    state_ = device_->GetMTLDevice().newComputePipelineState(info, err);

    if (!state_) {
      ERR("Failed to create compute PSO: ", err.description().getUTF8String());
      return this;
    }

    TRACE("Compiled 1 PSO");

    return this;
  }

  bool GetIsDone() { return ready_; }

  void SetIsDone(bool state) {
    ready_.store(state);
    ready_.notify_all();
  }

private:
  MTLD3D11Device *device_;
  std::atomic_bool ready_;
  CompiledShader *ComputeShader;
  WMT::Reference<WMT::ComputePipelineState> state_;
  bool tgsize_is_multiple_of_sgwidth;
};

std::unique_ptr<MTLCompiledComputePipeline>
CreateComputePipeline(MTLD3D11Device *pDevice, ManagedShader ComputeShader) {
  return std::make_unique<MTLCompiledComputePipelineImpl>(pDevice, ComputeShader);
}

} // namespace dxmt
