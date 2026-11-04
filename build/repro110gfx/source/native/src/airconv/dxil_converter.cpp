#include <cstdlib>
#include "dxil_converter.hpp"
#include "llvm/IR/Operator.h"
#include "dxbc_converter.hpp"
#include "dxil_metadata.hpp"
#include "dxil_converter_stages.hpp"

#include "llvm/IR/Instructions.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include "airconv_error.hpp"
#include "nt/dxil_converter_base.hpp"
#include "dxil_select_to_branch.hpp"
#include "llvm/IR/Verifier.h"

#include <memory>
#include <utility>

using namespace llvm;

namespace dxmt::dxil {

std::optional<dxmt::shader::common::ScalerDataType>
to_shader_scaler(ComponentType type) {
  using dxmt::shader::common::ScalerDataType;
  switch (type) {
  case ComponentType::I32:
    return ScalerDataType::Int;
  case ComponentType::U32:
    return ScalerDataType::Uint;
  case ComponentType::F32:
  case ComponentType::SNormF32:
  case ComponentType::UNormF32:
    return ScalerDataType::Float;
  case ComponentType::I16:
    return ScalerDataType::Short;
  case ComponentType::U16:
    return ScalerDataType::UShort;
  case ComponentType::F16:
  case ComponentType::SNormF16:
  case ComponentType::UNormF16:
    return ScalerDataType::Half;
  default:
    return std::nullopt;
  }
}

std::optional<dxmt::shader::common::ResourceType>
to_shader_resource_type(ResourceKind kind) {
  using dxmt::shader::common::ResourceType;
  switch (kind) {
  case ResourceKind::Texture1D:
    return ResourceType::Texture1D;
  case ResourceKind::Texture1DArray:
    return ResourceType::Texture1DArray;
  case ResourceKind::Texture2D:
    return ResourceType::Texture2D;
  case ResourceKind::Texture2DArray:
    return ResourceType::Texture2DArray;
  case ResourceKind::Texture2DMS:
    return ResourceType::Texture2DMultisampled;
  case ResourceKind::Texture2DMSArray:
    return ResourceType::Texture2DMultisampledArray;
  case ResourceKind::Texture3D:
    return ResourceType::Texture3D;
  case ResourceKind::TextureCube:
    return ResourceType::TextureCube;
  case ResourceKind::TextureCubeArray:
    return ResourceType::TextureCubeArray;
  case ResourceKind::TypedBuffer:
    return ResourceType::TextureBuffer;
  case ResourceKind::RawBuffer:
  case ResourceKind::StructuredBuffer:
    return ResourceType::NonApplicable;
  default:
    return std::nullopt;
  }
}

// constants for resolving resource handles
constexpr uint32_t kOpCreateHandle = 57;
constexpr uint32_t kOpTextureLoad = 66;
constexpr uint32_t kOpTextureStore = 67;
constexpr uint32_t kOpBufferLoad = 68;
constexpr uint32_t kOpBufferStore = 69;
constexpr uint32_t kOpAtomicBinOp = 78;
constexpr uint32_t kOpAtomicCmpXchg = 79;
constexpr uint32_t kOpSample = 60;
constexpr uint32_t kOpSampleBias = 61;
constexpr uint32_t kOpSampleLevel = 62;
constexpr uint32_t kOpSampleGrad = 63;
constexpr uint32_t kOpSampleCmp = 64;
constexpr uint32_t kOpSampleCmpLevelZero = 65;
constexpr uint32_t kOpTextureGather = 73;
constexpr uint32_t kOpTextureGatherCmp = 74;

// defined in `nt/dxil_converter_base.hpp`
std::optional<uint32_t> asU32(llvm::Value *value);

llvm::Expected<std::pair<uint8_t, uint32_t>>
resolve_handle(const llvm::Value *handle) {
  if (auto *ci = dyn_cast<CallInst>(handle)) {
    if (auto *callee = ci->getCalledFunction()) {
      StringRef name = callee->getName();
      if (name == "dx.op.createHandle") {
        auto res_class = asU32(ci->getArgOperand(1));
        auto range = asU32(ci->getArgOperand(2));
        if (!res_class || !range)
          return unsupported("dxil: non-constant createHandle arguments");
        return std::make_pair((uint8_t)*res_class, *range);
      }
      return unsupported("dxil: unsupported resource handle operation " + name.str());
    }
  }
  return unsupported("dxil: unresolvable resource handle");
}

using dxmt::air::InputAttributeComponentType;
using dxmt::air::Interpolation;

std::optional<air::MSLScalerOrVectorType>
to_signature_msl_type(ComponentType type) {
  switch (type) {
  case ComponentType::I32:
    return air::msl_int4;
  case ComponentType::U32:
    return air::msl_uint4;
  case ComponentType::F32:
    return air::msl_float4;
  case ComponentType::F16:
    return air::msl_half4;
  case ComponentType::I16:
    return air::msl_short4;
  case ComponentType::U16:
    return air::msl_ushort4;
  default:
    return std::nullopt;
  }
}

std::optional<dxbc::RegisterComponentType>
to_register_component_type(ComponentType type) {
  switch (type) {
  case ComponentType::I32:
    return dxbc::RegisterComponentType::Int;
  case ComponentType::U32:
    return dxbc::RegisterComponentType::Uint;
  case ComponentType::F32:
    return dxbc::RegisterComponentType::Float;
  default:
    return std::nullopt;
  }
}

std::optional<InputAttributeComponentType>
to_attribute_component_type(ComponentType type) {
  switch (type) {
  case ComponentType::I32:
    return InputAttributeComponentType::Int;
  case ComponentType::U32:
    return InputAttributeComponentType::Uint;
  case ComponentType::F32:
    return InputAttributeComponentType::Float;
  default:
    return std::nullopt;
  }
}

uint32_t
element_usage_mask(const SignatureElement &element) {
  return element.usage_mask != 0 ? element.usage_mask : ((1u << element.cols) - 1u);
}

uint32_t
element_register_mask(const SignatureElement &element) {
  return element_usage_mask(element) << element.start_col;
}

std::string
element_attribute_name(const SignatureElement &element) {
  // MacRunner shader-prewarm 17.10: имя атрибута — по ОБЪЯВЛЕННОЙ упаковке (row, start_col), общей для выхода VS и входа PS.
  // usage_mask у ступеней разный (VS пишет xyzw, PS читает только w): reg2_0 против reg2_3 → Metal отказывал в линковке
  // («Fragment input(s) user(reg2_3) … not written by vertex shader»), 23 из 60 конвейеров первого списка er-pso.
  return "reg" + std::to_string(element.start_row) + "_" + std::to_string(element.start_col);
}

llvm::Error
initialize_vertex_signature(dxmt::dxbc::SM50ShaderInternal *shader) {
  using namespace dxmt::dxbc;

  const EntryPoint &entry = shader->dxil_entry;
  uint32_t input_count = (uint32_t)entry.inputs.size();
  uint32_t max_input_register = 0;
  for (uint32_t i = 0; i < input_count; ++i) {
    const auto &element = entry.inputs[i];
    if (element.rows != 1 || element.start_row < 0 || element.start_col < 0)
      return unsupported("dxil: unsupported vertex shader input layout");
    uint32_t reg = element.start_row;
    uint32_t mask = element_register_mask(element);
    max_input_register = std::max(max_input_register, reg + 1);

    switch (element.semantic_kind) {
    case SemanticKind::VertexID:
    case SemanticKind::InstanceID: {
      bool instance = element.semantic_kind == SemanticKind::InstanceID;
      shader->signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue.Add([=](IOHelper &io) -> llvm::Error {
          return instance ? io.InitializeInstanceIDInput(reg, mask) : io.InitializeVertexIDInput(reg, mask);
        });
      });
      break;
    }
    case SemanticKind::Arbitrary: {
      auto attribute_type = to_attribute_component_type(element.component_type);
      if (!attribute_type)
        return unsupported("dxil: unsupported vertex shader input component type");
      std::string name = element_attribute_name(element);
      shader->signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.ia_layout) {
          for (unsigned j = 0; j < ctx.ia_layout->num_elements; ++j) {
            if (ctx.ia_layout->elements[j].reg != reg)
              continue;
            auto element_info = ctx.ia_layout->elements[j];
            auto slot_mask = ctx.ia_layout->slot_mask;
            auto vbuf_table = ctx.func_signature.DefineInput(
                air::ArgumentBindingBuffer{
                    .buffer_size = {},
                    .location_index = SM50_BINDING_INDEX_VERTEX_BUFFER,
                    .array_size = 0,
                    .memory_access = air::MemoryAccess::read,
                    .address_space = air::AddressSpace::constant,
                    .type = air::msl_uint,
                    .arg_name = "vertex_buffers",
                    .raster_order_group = {},
                }
            );
            ctx.prologue.Add([=](IOHelper &io) -> llvm::Error {
              return io.PullVertexInput(vbuf_table, reg, mask, element_info, slot_mask);
            });
            return;
          }
        }
        uint32_t arg = ctx.func_signature.DefineInput(
            air::InputVertexStageIn{.attribute = reg, .type = *attribute_type, .name = name}
        );
        ctx.prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(arg, reg, mask); });
      });
      break;
    }
    default:
      return unsupported("dxil: unsupported vertex shader input semantic");
    }
  }
  shader->max_input_register = max_input_register;

  uint32_t output_count = (uint32_t)entry.outputs.size();
  uint32_t max_output_register = 0;
  for (uint32_t i = 0; i < output_count; ++i) {
    const auto &element = entry.outputs[i];
    if (element.rows != 1 || element.start_row < 0 || element.start_col < 0)
      return unsupported("dxil: unsupported vertex shader output layout");
    uint32_t reg = element.start_row;
    uint32_t mask = element_register_mask(element);
    max_output_register = std::max(max_output_register, reg + 1);
    switch (element.semantic_kind) {
    case SemanticKind::Position: {
      shader->signature_handlers.push_back([=](SignatureContext &ctx) {
        uint32_t out = ctx.func_signature.DefineOutput(air::OutputPosition{.type = air::msl_float4});
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
          return io.PopulateOutputSanitizePosition(reg, mask, ret, out);
        });
      });
      break;
    }
    case SemanticKind::Arbitrary: {
      auto msl_type = to_signature_msl_type(element.component_type);
      if (!msl_type)
        return unsupported("dxil: unsupported vertex shader output component type");
      std::string user = element_attribute_name(element);
      // MacRunner GBUF-VS 24.10: MACRUNNER_AIRCONV_VS_OUT_NARROW=1 — выход VS объявляется шириной до старшей занятой полосы
      // (маска xyz -> float3, xy -> float2), полосы на своих местах: вход PS (float4, полосы по объявленной упаковке) читает те же
      // компоненты, лишняя полоса не пишется в буфер параметров. Элемент с занятой полосой w остаётся float4. Только
      // растеризуемый VS (skip_vertex_output = false); выходы mesh/GS-эмуляции не трогаются.
      static const bool narrow_out = [] {
        const char *e = std::getenv("MACRUNNER_AIRCONV_VS_OUT_NARROW");
        return e && e[0] == '1';
      }();
      const uint32_t hi_lane = 31u - (uint32_t)__builtin_clz(mask | 1u);
      shader->signature_handlers.push_back([=](SignatureContext &ctx) {
        auto out_type = *msl_type;
        if (narrow_out && !ctx.skip_vertex_output && hi_lane < 3)
          if (auto *vec = std::get_if<air::MSLVector>(&*msl_type))
            out_type = air::MSLVector{hi_lane + 1, vec->scaler};
        uint32_t out = ctx.func_signature.DefineOutput(air::OutputVertex{.user = user, .type = out_type});
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
          return io.PopulateOutput(reg, mask, ret, out);
        });
      });
      break;
    }
    case SemanticKind::ClipDistance: {   // MacRunner shader-prewarm 17.10: как DXBC (dxbc_signature.cpp:176–195)
      for (unsigned c = 0; c < 4; c++)
        if (mask & (1u << c))
          shader->clip_distance_scalars.push_back({.component = (uint8_t)c, .reg = (uint8_t)reg});
      std::string user = "SV_ClipDistance" + std::to_string(reg);
      shader->signature_handlers.push_back([=](SignatureContext &ctx) {
        uint32_t out = ctx.func_signature.DefineOutput(air::OutputVertex{.user = user, .type = air::msl_float4});
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
          return io.PopulateOutput(reg, mask, ret, out);
        });
      });
      break;
    }
    case SemanticKind::CullDistance: {   // MacRunner shader-prewarm 17.10: как DXBC (dxbc_signature.cpp:198–215)
      for (unsigned c = 0; c < 4; c++)
        if (mask & (1u << c))
          shader->cull_distance_scalars.push_back({.component = (uint8_t)c, .reg = (uint8_t)reg});
      std::string user = "SV_CullDistance" + std::to_string(reg);
      shader->signature_handlers.push_back([=](SignatureContext &ctx) {
        uint32_t out = ctx.func_signature.DefineOutput(air::OutputVertex{.user = user, .type = air::msl_float4});
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
          return io.PopulateOutput(reg, mask, ret, out);
        });
      });
      break;
    }
    default:
      return unsupported("dxil: unsupported vertex shader output semantic");
    }
  }
  shader->max_output_register = max_output_register;
  return llvm::Error::success();
}

std::optional<Interpolation>
to_air_interpolation(InterpolationMode mode) {
  switch (mode) {
  case InterpolationMode::Constant:
    return Interpolation::flat;
  case InterpolationMode::Linear:
    return Interpolation::center_perspective;
  case InterpolationMode::LinearCentroid:
    return Interpolation::centroid_perspective;
  case InterpolationMode::LinearNoperspective:
    return Interpolation::center_no_perspective;
  case InterpolationMode::LinearNoperspectiveCentroid:
    return Interpolation::centroid_no_perspective;
  case InterpolationMode::LinearSample:
    return Interpolation::sample_perspective;
  case InterpolationMode::LinearNoperspectiveSample:
    return Interpolation::sample_no_perspective;
  default:
    return std::nullopt;
  }
}

llvm::Error
initialize_pixel_signature(dxmt::dxbc::SM50ShaderInternal *shader) {
  using namespace dxmt::dxbc;

  const EntryPoint &entry = shader->dxil_entry;
  uint32_t input_count = (uint32_t)entry.inputs.size();
  uint32_t max_input_register = 0;
  for (unsigned phase = 0; phase < 2; ++phase) {
    for (uint32_t i = 0; i < input_count; ++i) {
      const auto &element = entry.inputs[i];
      bool system_value = element.semantic_kind != SemanticKind::Arbitrary;
      if (system_value != (phase == 0))
        continue;

      if (element.start_row < 0 || element.rows != 1 || element.start_col < 0)
        return unsupported("dxil: unsupported pixel shader input layout");
      uint32_t reg = (uint32_t)element.start_row;
      uint32_t mask = element_register_mask(element);
      max_input_register = std::max(max_input_register, reg + 1);
      // MacRunner shader-prewarm 17.10 11:15: элемент входа, который шейдер не читает (usage_mask == 0 в ISG1), интерполянтом НЕ
      // объявляется — иначе растеризатор интерполирует лишний float4 на каждый пиксель (TileBasedLighting: TEXCOORD0 с usage 0000
      // объявлялся как reg1_0; у MSL-пути его нет; 186 из 326 PS несут такие элементы). Выход VS без потребителя в Metal допустим.
      if (element.semantic_kind == SemanticKind::Arbitrary && element.usage_mask == 0)
        continue;

      switch (element.semantic_kind) {
      case SemanticKind::Position: {
        auto interpolation = to_air_interpolation(element.interpolation);
        if (!interpolation)
          return unsupported("dxil: unsupported interpolation mode");
        shader->signature_handlers.push_back([=](SignatureContext &ctx) {
          uint32_t arg = ctx.func_signature.DefineInput(air::InputPosition{.interpolation = *interpolation});
          ctx.prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(arg, reg, mask, true); });
        });
        break;
      }
      case SemanticKind::Arbitrary: {
        auto msl_type = to_signature_msl_type(element.component_type);
        auto interpolation = to_air_interpolation(element.interpolation);
        if (!msl_type)
          return unsupported("dxil: unsupported pixel shader input component type");
        if (!interpolation)
          return unsupported("dxil: unsupported interpolation mode");
        std::string user = element_attribute_name(element);
        // Линковка PS-входа с выходом DS/GS (слой передаёт SM50_SHADER_PS_INPUT_LINKAGE для mesh-конвейеров; Fork 17.10 10:51):
        // запись ищется по (регистр, ОБЪЯВЛЕННАЯ маска элемента), имя входа становится reg<out_reg>_<ctz(out_mask)>; без линковки —
        // имя по объявленной упаковке. Маска IO остаётся usage-based.
        const uint32_t declared_mask = ((1u << element.cols) - 1u) << element.start_col;
        // MacRunner GBUF-VS 24.10: MACRUNNER_AIRCONV_VS_OUT_NARROW=1 — вход PS той же ширины, что выход VS под этим ключом
        // (до старшей полосы объявленной маски элемента: xyz -> float3); Metal требует совпадения типов выхода VS и входа PS.
        // Только для конвейера без mesh-линковки (DS/GS-эмуляция пишет float4): pixel_input_linkage неактивна.
        static const bool narrow_in = [] {
          const char *e = std::getenv("MACRUNNER_AIRCONV_VS_OUT_NARROW");
          return e && e[0] == '1';
        }();
        const uint32_t hi_lane = 31u - (uint32_t)__builtin_clz(declared_mask | 1u);
        shader->signature_handlers.push_back([=](SignatureContext &ctx) {
          const std::string linked = ctx.pixel_input_linkage.attribute(reg, declared_mask, declared_mask, false, user);
          auto in_type = *msl_type;
          if (narrow_in && !ctx.pixel_input_linkage.active() && hi_lane < 3)
            if (auto *vec = std::get_if<air::MSLVector>(&*msl_type))
              in_type = air::MSLVector{hi_lane + 1, vec->scaler};
          uint32_t arg = ctx.func_signature.DefineInput(
              air::InputFragmentStageIn{
                  .user = linked, .type = in_type, .interpolation = *interpolation, .pull_mode = false
              }
          );
          ctx.prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(arg, reg, mask); });
        });
        break;
      }
      case SemanticKind::IsFrontFace:
      case SemanticKind::SampleIndex:
      case SemanticKind::PrimitiveID: {   // MacRunner shader-prewarm 17.10: системные входы PS как у DXBC (dxbc_signature.cpp:428–447)
        auto kind = element.semantic_kind;
        shader->signature_handlers.push_back([=](SignatureContext &ctx) {
          uint32_t arg = kind == SemanticKind::IsFrontFace ? ctx.func_signature.DefineInput(air::InputFrontFacing{})
                         : kind == SemanticKind::SampleIndex ? ctx.func_signature.DefineInput(air::InputSampleIndex{})
                                                             : ctx.func_signature.DefineInput(air::InputPrimitiveID{});
          ctx.prologue.Add([=](IOHelper &io) -> llvm::Error { return io.InitializeInput(arg, reg, mask); });
        });
        break;
      }
      default:
        return unsupported("dxil: unsupported pixel shader input semantic");
      }
    }
  }
  shader->max_input_register = max_input_register;

  uint32_t output_count = (uint32_t)entry.outputs.size();
  uint32_t max_output_register = 0;
  for (uint32_t i = 0; i < output_count; ++i) {
    const auto &element = entry.outputs[i];
    if (element.rows != 1)
      return unsupported("dxil: unsupported pixel shader output layout");
    uint32_t reg = (uint32_t)element.start_row;
    uint32_t mask = element_register_mask(element);
    switch (element.semantic_kind) {
    case SemanticKind::Target: {
      if (element.start_row < 0 || element.start_col < 0 || reg >= 8)
        return unsupported("dxil: unsupported pixel shader output layout");
      auto component_type = to_register_component_type(element.component_type);
      if (!component_type)
        return unsupported("dxil: unsupported pixel shader output component type");
      max_output_register = std::max(max_output_register, reg + 1);
      shader->pso_valid_output_reg_mask |= (1u << reg);
      shader->signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_pixel_output)   // MacRunner POST-CS: the uv-map function defines its own outputs
          return;
        auto type = component_type_from_pixel_format(ctx.pixel_formats[reg]);
        if (ctx.dual_source_blending || type == RegisterComponentType::Unknown)
          type = *component_type;
        if (ctx.dual_source_blending && reg > 1)
          return;
        uint32_t out = ctx.func_signature.DefineOutput(
            air::OutputRenderTarget{
                .dual_source_blending = ctx.dual_source_blending, .index = reg, .type = dxbc::to_msl_type(type)
            }
        );
        if (type == RegisterComponentType::Float && (ctx.unorm_output_reg_mask & (1u << reg)))
          ctx.epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
            return io.PopulateOutputFixUnorm(reg, mask, ret, out);
          });
        else
          ctx.epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
            return io.PopulateOutput(reg, mask, ret, out);
          });
      });
      break;
    }
    case SemanticKind::Depth:
    case SemanticKind::DepthLessEqual:
    case SemanticKind::DepthGreaterEqual: {   // MacRunner shader-prewarm 17.10: как DXBC (dxbc_signature.cpp:535–552)
      auto kind = element.semantic_kind;
      shader->signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.disable_depth_output || ctx.skip_pixel_output)
          return;
        auto out = ctx.func_signature.DefineOutput(air::OutputDepth{
            .depth_argument = kind == SemanticKind::DepthGreaterEqual ? air::DepthArgument::greater
                              : kind == SemanticKind::DepthLessEqual  ? air::DepthArgument::less
                                                                      : air::DepthArgument::any});
        ctx.prologue.Add([=](IOHelper &io) -> llvm::Error { return io.CreateDepthOutputRegister(); });
        ctx.epilogue.Add([=](llvm::Value *ret, IOHelper &io) -> llvm::Expected<llvm::Value *> {
          return io.PopulateOutputDepth(ret, out);
        });
      });
      break;
    }
    default:
      return unsupported("dxil: unsupported pixel shader output semantic");
    }
  }
  shader->max_output_register = max_output_register;
  return llvm::Error::success();
}

llvm::Error
initialize_shader(dxmt::dxbc::SM50ShaderInternal *shader, const Info &info) {
  using namespace microsoft;

  if (info.entry_points.size() != 1)
    return llvm::make_error<UnsupportedFeature>("dxil: only a single entry point is supported");
  const EntryPoint &entry = info.entry_points.front();

  shader->dxil_entry = entry;

  const char *stage = info.shader_model.stage.c_str();
  if (info.shader_model.stage == "cs") {
    shader->shader_type = microsoft::D3D11_SB_COMPUTE_SHADER;
  } else if (info.shader_model.stage == "vs") {
    shader->shader_type = microsoft::D3D10_SB_VERTEX_SHADER;
    if (auto err = initialize_vertex_signature(shader))
      return err;
  } else if (info.shader_model.stage == "ps") {
    shader->shader_type = microsoft::D3D10_SB_PIXEL_SHADER;
    if (auto err = initialize_pixel_signature(shader))
      return err;
  } else if (info.shader_model.stage == "hs") {   // MacRunner dxil-tess 17.10: ступени HS/DS/GS — dxil_converter_stages.cpp
    if (auto err = initialize_hull_shader(shader, info))
      return err;
  } else if (info.shader_model.stage == "ds") {
    if (auto err = initialize_domain_shader(shader, info))
      return err;
  } else if (info.shader_model.stage == "gs") {
    if (auto err = initialize_geometry_shader(shader, info))
      return err;
  } else {
    return llvm::make_error<UnsupportedFeature>("dxil: unsupported shader stage " + std::string(stage));
  }

  auto &func_signature = shader->func_signature;
  if (shader->shader_type == microsoft::D3D11_SB_COMPUTE_SHADER) {
    for (auto &property : entry.properties) {
      if (property.tag != EntryPropertyTag::NumThreads)
        continue;
      auto n = property.num_threads;
      shader->threadgroup_size[0] = n.x;
      shader->threadgroup_size[1] = n.y;
      shader->threadgroup_size[2] = n.z;
      func_signature.UseMaxWorkgroupSize(n.x * n.y * n.z);
    }
  }

  auto &shader_info = shader->shader_info;
  for (auto &resource : entry.resources) {
    dxbc::ResourceRange range{resource.id, resource.lower_bound, resource.range_size, resource.space};

    switch (resource.resource_class) {
    case ResourceClass::Sampler: {
      dxbc::SamplerInfo sampler{};
      sampler.range = range;
      shader_info.samplerMap[resource.id] = sampler;
      break;
    }
    case ResourceClass::CBV: {
      dxbc::ConstantBufferInfo cbuffer{};
      cbuffer.range = range;
      cbuffer.size_in_vec4 = (resource.cbuffer_size + 15) / 16;
      shader_info.cbufferMap[resource.id] = cbuffer;
      break;
    }
    case ResourceClass::SRV: {
      auto type = to_shader_resource_type(resource.kind);
      if (!type)
        return unsupported("dxil: unsupported shader resource kind");
      auto scaler = resource.kind == ResourceKind::StructuredBuffer || resource.kind == ResourceKind::RawBuffer
                        ? std::optional(dxmt::shader::common::ScalerDataType::Uint)
                        : to_shader_scaler((ComponentType)resource.element_type);
      if (!scaler)
        return unsupported("dxil: unsupported shader resource component type");
      dxbc::ShaderResourceViewInfo srv{};
      srv.range = range;
      srv.scaler_type = *scaler;
      srv.resource_type = *type;
      srv.structure_stride = resource.element_stride;
      shader_info.srvMap[resource.id] = srv;
      break;
    }
    case ResourceClass::UAV: {
      auto type = to_shader_resource_type(resource.kind);
      if (!type)
        return unsupported("dxil: unsupported unordered access resource kind");
      auto scaler = resource.kind == ResourceKind::StructuredBuffer || resource.kind == ResourceKind::RawBuffer
                        ? std::optional(dxmt::shader::common::ScalerDataType::Uint)
                        : to_shader_scaler((ComponentType)resource.element_type);
      if (!scaler)
        return unsupported("dxil: unsupported shader resource component type");
      dxbc::UnorderedAccessViewInfo uav{};
      uav.range = range;
      uav.scaler_type = *scaler;
      uav.resource_type = *type;
      uav.global_coherent = resource.globally_coherent;
      uav.rasterizer_order = resource.rasterizer_ordered;
      uav.with_counter = resource.has_counter;
      uav.structure_stride = resource.element_stride;
      shader_info.uavMap[resource.id] = uav;
      break;
    }
    default:
      break;
    }
  }

  for (auto &bb : *entry.function) {
    for (auto &inst : bb) {
      auto *ci = dyn_cast<CallInst>(&inst);
      if (!ci)
        continue;
      auto *callee = ci->getCalledFunction();
      if (!callee || !callee->getName().startswith("dx.op."))
        continue;
      auto opcode = asU32(ci->getArgOperand(0));
      if (!opcode)
        continue;

      auto record_usage = [&](bool read, bool written, bool sampled, bool compared) -> llvm::Error {
        auto key = resolve_handle(ci->getArgOperand(1));
        if (!key)
          return key.takeError();
        switch ((ResourceClass)key->first) {
        case ResourceClass::SRV: {
          auto found = shader_info.srvMap.find(key->second);
          if (found != shader_info.srvMap.end()) {
            found->second.read |= read;
            found->second.sampled |= sampled;
            found->second.compared |= compared;
          }
          break;
        }
        case ResourceClass::UAV: {
          auto found = shader_info.uavMap.find(key->second);
          if (found != shader_info.uavMap.end()) {
            found->second.read |= read;
            found->second.written |= written;
          }
          break;
        }
        default:
          break;
        }
        return llvm::Error::success();
      };

      switch (*opcode) {
      case kOpCreateHandle: {
        auto key = resolve_handle(ci);
        if (!key)
          return key.takeError();
        break;
      }
      case kOpTextureLoad:
      case kOpBufferLoad:
        if (auto err = record_usage(true, false, false, false))
          return err;
        break;
      case kOpTextureStore:
      case kOpBufferStore:
        if (auto err = record_usage(false, true, false, false))
          return err;
        break;
      case kOpAtomicBinOp:
      case kOpAtomicCmpXchg:
        if (auto err = record_usage(true, true, false, false))
          return err;
        break;
      case kOpSample:
      case kOpSampleBias:
      case kOpSampleLevel:
      case kOpSampleGrad:
      case kOpTextureGather:
        if (auto err = record_usage(false, false, true, false))
          return err;
        break;
      case kOpSampleCmp:
      case kOpSampleCmpLevelZero:
      case kOpTextureGatherCmp:
        if (auto err = record_usage(false, false, true, true))
          return err;
        break;
      default:
        break;
      }
    }
  }

  return llvm::Error::success();
}

int
initialize(
    const void *pDxilBlob, size_t DxilBlobSize, sm50_shader_t *ppShader, MTL_SHADER_REFLECTION *pRefl,
    sm50_error_t *ppError
) {
  auto errorObj = std::make_unique<dxmt::dxbc::SM50ErrorInternal>();
  llvm::raw_svector_ostream errorOut(errorObj->buf);

  auto parsed = parse_dxil_blob(llvm::StringRef((const char *)pDxilBlob, DxilBlobSize));
  if (!parsed) {
    errorOut << llvm::toString(parsed.takeError()) << '\0';
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }
  auto shader = std::make_unique<dxmt::dxbc::SM50ShaderInternal>();
  shader->dxil_context = std::move(parsed->context);
  shader->dxil = std::move(parsed->module);

  if (auto err = initialize_shader(shader.get(), parsed->info)) {
    errorOut << llvm::toString(std::move(err)) << '\0';
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  // MacRunner module-meta 17.10 (куратор 12:51): модульный fast_math и fp-атрибуты — только если все fp-инструкции
  // несут полный `fast` и нет !dx.precise; denorms_disable — по атрибуту fp32-denorm-mode (ftz/any; пусто = умолчание any).
  if (auto *F = shader->dxil_entry.function) {
    bool all_fast = true;
    for (auto &BB : *F)
      for (auto &I : BB) {
        if (I.getMetadata("dx.precise")) { all_fast = false; break; }
        if (llvm::isa<llvm::CallInst>(I)) continue;   // DXIL (LLVM 3.7) не кодирует FMF на call
        if (llvm::isa<llvm::FPMathOperator>(I) && I.getType()->isFPOrFPVectorTy() == I.getType()->isFPOrFPVectorTy()) {
          unsigned op = I.getOpcode();
          if (op == llvm::Instruction::FAdd || op == llvm::Instruction::FSub || op == llvm::Instruction::FMul ||
              op == llvm::Instruction::FDiv || op == llvm::Instruction::FRem || op == llvm::Instruction::FNeg ||
              op == llvm::Instruction::FCmp)
            if (!I.getFastMathFlags().isFast()) { all_fast = false; break; }
        }
      }
    shader->dxil_all_fast = all_fast;
    if (F->hasFnAttribute("fp32-denorm-mode")) shader->dxil_denorm_mode = F->getFnAttribute("fp32-denorm-mode").getValueAsString().str();
  }

  if (pRefl) {
    *pRefl = {};
    pRefl->ConstanttBufferTableBindIndex = ~0u;
    pRefl->ArgumentBufferBindIndex = ~0u;
    pRefl->NumOutputElement = shader->max_output_register;
    switch (shader->shader_type) {
    case microsoft::D3D11_SB_COMPUTE_SHADER:
      pRefl->ThreadgroupSize[0] = shader->threadgroup_size[0];
      pRefl->ThreadgroupSize[1] = shader->threadgroup_size[1];
      pRefl->ThreadgroupSize[2] = shader->threadgroup_size[2];
      break;
    case microsoft::D3D10_SB_PIXEL_SHADER:
      pRefl->PixelShader.ValidRenderTargets = shader->pso_valid_output_reg_mask;
      pRefl->PixelShader.HasCoverageOutput = shader->ps_has_coverage_output;
      break;
    case microsoft::D3D11_SB_HULL_SHADER:   // MacRunner dxil-tess 17.10
    case microsoft::D3D11_SB_DOMAIN_SHADER:
    case microsoft::D3D10_SB_GEOMETRY_SHADER:
      if (auto err = fill_stage_reflection(shader.get(), pRefl)) {
        errorOut << llvm::toString(std::move(err)) << '\0';
        SM50_RETURN_ERROR(errorObj, ppError, 1);
      }
      break;
    default:
      break;
    }
  }

  *ppShader = (sm50_shader_t)shader.release();
  return 0;
}

llvm::Expected<llvm::BasicBlock *>
convert_basicblocks(const EntryPoint & entry, dxmt::dxbc::context &ctx, llvm::BasicBlock *return_bb) {
  auto saved_block = ctx.builder.GetInsertBlock();
  if (entry.function) {   // MacRunner shader-prewarm 17.10: select с дорогой стороной → ветви (dxil_select_to_branch.cpp); идемпотентно
    auto *function = const_cast<llvm::Function *>(entry.function);
    auto stats = SelectToBranch(*function, &entry);
    if (stats.converted && llvm::verifyFunction(*function, &llvm::errs()))
      return llvm::make_error<UnsupportedFeature>("dxil: select-to-branch produced invalid IR");
  }
  Converter converter(entry, ctx);
  auto result = converter.Run(return_bb);
  ctx.builder.SetInsertPoint(saved_block);
  return result;
}

} // namespace dxmt::dxil
