// MacRunner dxil-tess 17.10.2026: HS/DS/GS из DXIL — см. dxil_converter_stages.hpp.
#include "dxil_converter_stages.hpp"

#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "dxil_converter.hpp"
#include "nt/dxbc_io_helper.hpp"
#include "nt/dxil_converter_base.hpp"

#include "llvm/IR/Instructions.h"

#include <algorithm>

using namespace llvm;

namespace dxmt::dxil {

// Определены в dxil_converter.cpp (внешняя связь).
std::optional<uint32_t> asU32(llvm::Value *value); // nt/dxil_converter_base.cpp
std::optional<air::MSLScalerOrVectorType> to_signature_msl_type(ComponentType type);
std::optional<dxbc::RegisterComponentType> to_register_component_type(ComponentType type);
uint32_t element_usage_mask(const SignatureElement &element);
uint32_t element_register_mask(const SignatureElement &element);

namespace {

const Property *
find_property(const EntryPoint &entry, EntryPropertyTag tag) {
  for (auto &property : entry.properties)
    if (property.tag == tag)
      return &property;
  return nullptr;
}

bool
function_uses_dxil_op(const llvm::Function *function, uint32_t opcode) {
  if (!function)
    return false;
  for (auto &bb : *function) {
    for (auto &inst : bb) {
      auto *ci = dyn_cast<CallInst>(&inst);
      if (!ci)
        continue;
      auto *callee = ci->getCalledFunction();
      if (!callee || !callee->getName().startswith("dx.op."))
        continue;
      auto *op = dyn_cast<ConstantInt>(ci->getArgOperand(0));
      if (op && op->getZExtValue() == opcode)
        return true;
    }
  }
  return false;
}

bool
register_semantic(SemanticKind kind) {
  switch (kind) {
  case SemanticKind::Arbitrary:
  case SemanticKind::Position:
  case SemanticKind::ClipDistance:
  case SemanticKind::CullDistance:
    return true;
  default:
    return false;
  }
}

// Индекс множителя тесселяции в порядке D3D11_SB_NAME_FINAL_* (как dxbc_signature.cpp: siv − первый SIV домена).
// Проверено по фикстурам vkd3d (tools/dxbc_siv.py): FXC даёт SV_TessFactor0 → FINAL_LINE_DENSITY (22), SV_TessFactor1 →
// FINAL_LINE_DETAIL (21); квад/треугольник — по порядку рёбер.
int8_t
tess_factor_index(uint32_t domain, bool inside, uint32_t row) {
  switch (domain) {
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
    if (!inside && row < 4)
      return (int8_t)row;
    if (inside && row < 2)
      return (int8_t)(4 + row);
    return -2;
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_TRI:
    if (!inside && row < 3)
      return (int8_t)row;
    if (inside && row == 0)
      return 3;
    return -2;
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
    if (!inside && row == 0)
      return 1; // density
    if (!inside && row == 1)
      return 0; // detail
    return -2;
  default:
    return -2;
  }
}

llvm::Error
check_register_elements(const std::vector<SignatureElement> &elements, const char *what) {
  for (auto &e : elements) {
    if (e.start_row < 0 || e.start_col < 0)
      return unsupported(std::string("dxil: ") + what + " element is not mapped: " + e.name);
    if (!register_semantic(e.semantic_kind))
      return unsupported(
          std::string("dxil: unsupported ") + what + " semantic kind=" + std::to_string((uint32_t)e.semantic_kind) +
          " name=" + e.name
      );
  }
  return llvm::Error::success();
}

// Выходы mesh-функции (DS и GS) — зеркало handle_signature_ds/gs для DCL_OUTPUT/DCL_OUTPUT_SIV.
llvm::Error
initialize_mesh_outputs(dxbc::SM50ShaderInternal *shader, const std::vector<SignatureElement> &outputs) {
  using namespace dxmt::dxbc;
  auto &func_signature = shader->func_signature;
  auto &handlers = shader->mesh_output_handlers;
  uint32_t &num_mesh_vertex_data = shader->num_mesh_vertex_data;
  uint32_t max_output_register = 0;
  for (auto &e : outputs) {
    if (e.output_stream != 0)
      return unsupported("dxil: output stream other than 0 is not supported: " + e.name);
    if (e.start_row < 0 || e.start_col < 0)
      return unsupported("dxil: output element is not mapped: " + e.name);
    uint32_t mask = element_register_mask(e);
    for (uint32_t r = 0; r < std::max(1u, e.rows); ++r) {
      uint32_t reg = (uint32_t)e.start_row + r;
      max_output_register = std::max(max_output_register, reg + 1);
      switch (e.semantic_kind) {
      case SemanticKind::Position:
        func_signature.DefineMeshVertexOutput(air::OutputPosition{.type = air::msl_float4});
        handlers.push_back([=](MeshOutputContext &output, IOHelper &io) {
          io.PopulateMeshOutputPosition(reg, mask, output.vertex_id);
        });
        break;
      case SemanticKind::RenderTargetArrayIndex:
        func_signature.DefineMeshPrimitiveOutput(air::OutputRenderTargetArrayIndex{});
        handlers.push_back([=](MeshOutputContext &output, IOHelper &io) {
          io.PopulateMeshOutputRenderTargetArrayIndex(reg, mask, output.primitive_id);
        });
        break;
      case SemanticKind::ViewPortArrayIndex:
        func_signature.DefineMeshPrimitiveOutput(air::OutputViewportArrayIndex{});
        handlers.push_back([=](MeshOutputContext &output, IOHelper &io) {
          io.PopulateMeshOutputViewportArrayIndex(reg, mask, output.primitive_id);
        });
        break;
      case SemanticKind::ClipDistance:
        for (unsigned c = 0; c < 4; c++)
          if (mask & (1u << c))
            shader->clip_distance_scalars.push_back({.component = (uint8_t)c, .reg = (uint8_t)reg});
        break;
      case SemanticKind::CullDistance:
        for (unsigned c = 0; c < 4; c++)
          if (mask & (1u << c))
            shader->cull_distance_scalars.push_back({.component = (uint8_t)c, .reg = (uint8_t)reg});
        break;
      case SemanticKind::Arbitrary: {
        auto component = to_register_component_type(e.component_type);
        if (!component)
          return unsupported("dxil: unsupported mesh output component type: " + e.name);
        auto type = to_msl_type(*component);
        // имя — по объявленной упаковке (row, start_col), как у VS→PS в dxil_converter.cpp (element_attribute_name)
        std::string user = "reg" + std::to_string(reg) + "_" + std::to_string(e.start_col);
        uint32_t index = num_mesh_vertex_data++;
        func_signature.DefineMeshVertexOutput(air::OutputMeshData{.user = user, .type = type, .index = index});
        handlers.push_back([=](MeshOutputContext &output, IOHelper &io) {
          io.PopulateMeshOutputVertexData(reg, mask, index, output.vertex_id, type);
        });
        break;
      }
      default:
        return unsupported(
            "dxil: unsupported mesh output semantic kind=" + std::to_string((uint32_t)e.semantic_kind) +
            " name=" + e.name
        );
      }
    }
  }
  shader->max_output_register = max_output_register;
  return llvm::Error::success();
}

} // namespace

uint32_t
max_register(const std::vector<SignatureElement> &elements) {
  uint32_t max_reg = 0;
  for (auto &e : elements)
    if (e.start_row >= 0)
      max_reg = std::max(max_reg, (uint32_t)e.start_row + std::max(1u, e.rows));
  return max_reg;
}

std::vector<dxmt::stage_linkage::Row>
linkage_rows(const std::vector<SignatureElement> &elements, bool output) {
  std::vector<dxmt::stage_linkage::Row> rows;
  for (auto &e : elements) {
    uint32_t system = 0;
    switch (e.semantic_kind) {
    case SemanticKind::Position:
      system = microsoft::D3D10_SB_NAME_POSITION;
      break;
    case SemanticKind::ClipDistance:
      system = microsoft::D3D10_SB_NAME_CLIP_DISTANCE;
      break;
    case SemanticKind::CullDistance:
      system = microsoft::D3D10_SB_NAME_CULL_DISTANCE;
      break;
    default:
      break;
    }
    auto component = to_register_component_type(e.component_type);
    uint32_t type = component ? (uint32_t)*component : 0;
    uint32_t mask = element_register_mask(e);
    for (uint32_t r = 0; r < std::max(1u, e.rows); ++r) {
      rows.push_back({std::string_view(e.name), e.semantic_index + r, system, type, (uint32_t)e.start_row + r, mask,
                      output ? 0u : mask, 0u, 0u});
    }
  }
  return rows;
}

llvm::Error
initialize_hull_shader(dxbc::SM50ShaderInternal *shader, const Info &info) {
  using namespace microsoft;
  const EntryPoint &entry = shader->dxil_entry;
  shader->shader_type = D3D11_SB_HULL_SHADER;

  auto *hs = find_property(entry, EntryPropertyTag::HSState);
  if (!hs)
    return unsupported("dxil: hull shader without HSState");
  const HSState &st = hs->hs_state;
  if (!st.patch_constant_function)
    return unsupported("dxil: hull shader without patch constant function");
  if (st.input_control_point_count == 0 || st.input_control_point_count > 32 || st.output_control_point_count == 0 ||
      st.output_control_point_count > 32)
    return unsupported("dxil: hull shader control point count out of range");
  if (st.domain < D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE || st.domain > D3D11_SB_TESSELLATOR_DOMAIN_QUAD)
    return unsupported("dxil: hull shader with undefined tessellator domain");
  if (st.partitioning < D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER ||
      st.partitioning > D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN)
    return unsupported("dxil: hull shader with undefined tessellator partitioning");
  if (st.output_primitive < D3D11_SB_TESSELLATOR_OUTPUT_POINT || st.output_primitive > D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW)
    return unsupported("dxil: hull shader with undefined tessellator output primitive");

  shader->input_control_point_count = st.input_control_point_count;
  shader->output_control_point_count = st.output_control_point_count;
  shader->tessellation_domain = (D3D11_SB_TESSELLATOR_DOMAIN)st.domain;
  shader->tessellation_partition = (D3D11_SB_TESSELLATOR_PARTITIONING)st.partitioning;
  shader->tessellator_output_primitive = (D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE)st.output_primitive;
  // dcl_hs_max_tessfactor у DXBC; DXC пишет 64 по умолчанию. Прежний путь требует её (ReadTessellationStage).
  shader->max_tesselation_factor = st.max_tess_factor > 0.0f ? st.max_tess_factor : 64.0f;
  // DXBC: max(число входных КТ, выходных КТ, экземпляров fork/join). У DXIL patch-constant функция — один поток.
  shader->hull_maximum_threads_per_patch = std::max(st.input_control_point_count, st.output_control_point_count);
  // В DXIL фаза контрольных точек есть всегда (main); «passthrough без фазы» — только у DXBC.
  shader->shader_info.no_control_point_phase_passthrough = true;

  shader->dxil_patch_constant_entry = entry;
  shader->dxil_patch_constant_entry.function = st.patch_constant_function;

  if (auto err = check_register_elements(entry.inputs, "hull shader input"))
    return err;
  shader->max_input_register = max_register(entry.inputs);
  if (auto err = check_register_elements(entry.outputs, "hull shader output"))
    return err;
  shader->max_output_register = max_register(entry.outputs);

  uint32_t max_pc_register = 0;
  for (auto &e : entry.patch_constants) {
    if (e.start_row < 0 || e.start_col < 0)
      return unsupported("dxil: patch constant element is not mapped: " + e.name);
    uint32_t mask = element_register_mask(e);
    for (uint32_t r = 0; r < std::max(1u, e.rows); ++r) {
      uint32_t reg = (uint32_t)e.start_row + r;
      max_pc_register = std::max(max_pc_register, reg + 1);
      int8_t tf = -1;
      switch (e.semantic_kind) {
      case SemanticKind::TessFactor:
        tf = tess_factor_index(st.domain, false, r);
        break;
      case SemanticKind::InsideTessFactor:
        tf = tess_factor_index(st.domain, true, r);
        break;
      case SemanticKind::Arbitrary:
        break;
      default:
        return unsupported(
            "dxil: unsupported patch constant semantic kind=" + std::to_string((uint32_t)e.semantic_kind) +
            " name=" + e.name
        );
      }
      if (tf < -1)
        return unsupported("dxil: tessellation factor row out of range for the domain: " + e.name);
      for (unsigned c = 0; c < 4; c++)
        if (mask & (1u << c))
          shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)c, .reg = (uint8_t)reg, .tess_factor_index = tf}
          );
    }
  }
  shader->max_patch_constant_output_register = max_pc_register;

  // Чтение выходных КТ (dx.op.loadOutputControlPoint = 103) → КТ держатся в threadgroup-памяти и копируются в payload.
  shader->shader_info.output_control_point_read =
      function_uses_dxil_op(st.patch_constant_function, 103) || function_uses_dxil_op(entry.function, 103);
  return llvm::Error::success();
}

llvm::Error
initialize_domain_shader(dxbc::SM50ShaderInternal *shader, const Info &info) {
  using namespace microsoft;
  const EntryPoint &entry = shader->dxil_entry;
  shader->shader_type = D3D11_SB_DOMAIN_SHADER;

  auto *ds = find_property(entry, EntryPropertyTag::DSState);
  if (!ds)
    return unsupported("dxil: domain shader without DSState");
  shader->tessellation_domain = (D3D11_SB_TESSELLATOR_DOMAIN)ds->ds_state.domain;
  shader->input_control_point_count = ds->ds_state.input_control_point_count;

  if (auto err = check_register_elements(entry.inputs, "domain shader input"))
    return err;
  for (auto &e : entry.patch_constants) {
    if (e.start_row < 0 || e.start_col < 0)
      return unsupported("dxil: patch constant input element is not mapped: " + e.name);
    switch (e.semantic_kind) {
    case SemanticKind::Arbitrary:
    case SemanticKind::TessFactor:
    case SemanticKind::InsideTessFactor:
      break;
    default:
      return unsupported("dxil: unsupported patch constant input semantic: " + e.name);
    }
  }
  // «for domain shader, it refers to patch constant input count»
  shader->max_input_register = max_register(entry.patch_constants);
  return initialize_mesh_outputs(shader, entry.outputs);
}

llvm::Error
initialize_geometry_shader(dxbc::SM50ShaderInternal *shader, const Info &info) {
  using namespace microsoft;
  const EntryPoint &entry = shader->dxil_entry;
  shader->shader_type = D3D10_SB_GEOMETRY_SHADER;

  auto *gs = find_property(entry, EntryPropertyTag::GSState);
  if (!gs)
    return unsupported("dxil: geometry shader without GSState");
  const GSState &st = gs->gs_state;
  switch (st.input_primitive) {
  case D3D10_SB_PRIMITIVE_POINT:
  case D3D10_SB_PRIMITIVE_LINE:
  case D3D10_SB_PRIMITIVE_TRIANGLE:
  case D3D10_SB_PRIMITIVE_LINE_ADJ:
  case D3D10_SB_PRIMITIVE_TRIANGLE_ADJ:
    break;
  default:
    return unsupported("dxil: unsupported geometry shader input primitive " + std::to_string(st.input_primitive));
  }
  switch (st.output_topology) {
  case D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST:
  case D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP:
  case D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    break;
  default:
    return unsupported("dxil: unsupported geometry shader output topology " + std::to_string(st.output_topology));
  }
  if (st.active_stream_mask & ~1u)
    return unsupported("dxil: geometry shader with multiple output streams");
  if (st.max_vertex_count == 0)
    return unsupported("dxil: geometry shader without max vertex count");
  if (st.instance_count > 32) // предел D3D; экземпляры диспетчеризует object-функция (CreateSetMeshProperties: примитивы × экземпляры)
    return unsupported("dxil: geometry shader instance count above 32");
  shader->gs_input_primitive = (D3D10_SB_PRIMITIVE)st.input_primitive;
  shader->gs_output_topology = (D3D10_SB_PRIMITIVE_TOPOLOGY)st.output_topology;
  shader->gs_max_vertex_output = st.max_vertex_count;
  shader->gs_instance_count = st.instance_count ? st.instance_count : 1;

  if (auto err = check_register_elements(entry.inputs, "geometry shader input"))
    return err;
  shader->max_input_register = max_register(entry.inputs);
  return initialize_mesh_outputs(shader, entry.outputs);
}

llvm::Error
fill_stage_reflection(dxbc::SM50ShaderInternal *shader, MTL_SHADER_REFLECTION *pRefl) {
  using namespace microsoft;
  using namespace dxmt::dxbc;
  switch (shader->shader_type) {
  case D3D11_SB_HULL_SHADER: {
    auto threads_per_patch = next_pow2(shader->hull_maximum_threads_per_patch);
    if (threads_per_patch > 32)
      return unsupported("Threadgroup size of tessellation pipeline is too large.");
    auto patch_per_group = 32 / threads_per_patch;
    float max_tesselation_factor = shader->max_tesselation_factor;
    while (estimate_payload_size(shader, max_tesselation_factor, patch_per_group) > 16384) {
      if (patch_per_group == 1) {
        if (max_tesselation_factor > 1.0f) {
          max_tesselation_factor = std::max(max_tesselation_factor - 2.0f, 1.0f);
        } else {
          return unsupported("Payload size of tessellation pipeline is too large.");
        }
      } else {
        patch_per_group = patch_per_group >> 1;
      }
    }
    shader->hull_maximum_threads_per_patch = 32 / patch_per_group;
    shader->max_tesselation_factor = max_tesselation_factor;
    pRefl->ThreadsPerPatch = shader->hull_maximum_threads_per_patch;
    pRefl->Tessellator = {
        .Partition = shader->tessellation_partition,
        .MaxFactor = shader->max_tesselation_factor,
        .OutputPrimitive = (MTL_TESSELLATOR_OUTPUT_PRIMITIVE)shader->tessellator_output_primitive,
    };
    break;
  }
  case D3D11_SB_DOMAIN_SHADER: {
    uint32_t max_potential_tess_factor = 1;
    for (int tess_factor = 1; tess_factor <= 64; tess_factor++) {
      auto x = estimate_mesh_size(shader, tess_factor);
      if (x > 32768)
        break;
      max_potential_tess_factor = tess_factor;
    }
    uint32_t max_split_factor = 0;
    for (uint32_t factor = 1; factor <= 64; ++factor) {
      const size_t vertices = (2 * (factor + 1) + 7) & ~7u;
      if (vertices * size_t(shader->max_output_register) * 16 > 32768)
        break;
      max_split_factor = factor;
    }
    pRefl->PostTessellator = {
        .MaxPotentialTessFactor = max_potential_tess_factor,
        .MaxPotentialSplitTessFactor = max_split_factor,
    };
    break;
  }
  case D3D10_SB_GEOMETRY_SHADER:
    pRefl->GeometryShader.GSPassThrough = ~0u; // сквозной GS по DXIL не распознаём — всегда полный mesh-путь
    pRefl->GeometryShader.Primitive = shader->gs_input_primitive;
    break;
  default:
    break;
  }
  return llvm::Error::success();
}

llvm::Expected<llvm::BasicBlock *>
convert_hull_shader_body(dxbc::SM50ShaderInternal *hull, dxbc::context &ctx, llvm::BasicBlock *epilogue_bb) {
  auto &builder = ctx.builder;
  auto &context = ctx.llvm;
  auto function = ctx.function;
  auto tid = ctx.resource.thread_id_in_patch;
  if (!tid)
    return unsupported("dxil: hull shader thread id in patch is not available");
  // Как convert_basicblocks: точка вставки вызывающего (зона HS, блок vertex_end) восстанавливается перед возвратом —
  // вызывающий ставит `br entry` именно там.
  auto saved_block = builder.GetInsertBlock();

  auto dispatch = llvm::BasicBlock::Create(context, "hull_dxil_entry", function);
  auto cp_end = llvm::BasicBlock::Create(context, "control_point_end", function);
  auto pc_end = llvm::BasicBlock::Create(context, "patch_constant_end", function);

  // Фаза контрольных точек: main по потоку на выходную КТ (у неактивных патчей thread_id_in_patch = 32 → мимо).
  auto cp_entry = convert_basicblocks(hull->dxil_entry, ctx, cp_end);
  if (!cp_entry)
    return cp_entry.takeError();
  builder.SetInsertPoint(dispatch);
  builder.CreateCondBr(
      builder.CreateICmpULT(tid, builder.getInt32(hull->output_control_point_count)), *cp_entry, cp_end
  );

  // Конец фазы — барьер threadgroup (как InstSync у DXBC), затем patch-constant функция одним потоком патча.
  builder.SetInsertPoint(cp_end);
  ctx.air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  auto pc_entry = convert_basicblocks(hull->dxil_patch_constant_entry, ctx, pc_end);
  if (!pc_entry)
    return pc_entry.takeError();
  builder.SetInsertPoint(cp_end);
  builder.CreateCondBr(builder.CreateICmpEQ(tid, builder.getInt32(0)), *pc_entry, pc_end);

  builder.SetInsertPoint(pc_end);
  if (hull->shader_info.output_control_point_read) {
    // КТ лежали в threadgroup-памяти (их читала patch-constant функция) — копия в payload, как BasicBlockHullShaderWriteOutput.
    auto active = llvm::BasicBlock::Create(context, "write_control_point", function);
    auto sync = llvm::BasicBlock::Create(context, "write_control_point_end", function);
    builder.CreateCondBr(
        builder.CreateICmpULT(tid, builder.getInt32(hull->output_control_point_count)), active, sync
    );
    builder.SetInsertPoint(active);
    auto src = ctx.resource.hull_cp_passthrough_src;
    if (!src || !ctx.resource.hull_cp_passthrough_dst || !ctx.resource.hull_cp_passthrough_type)
      return unsupported("dxil: hull control point pass-through buffers are not available");
    if (src->getType()->getNonOpaquePointerElementType() != ctx.resource.hull_cp_passthrough_type) {
      src = builder.CreateBitCast(
          src, ctx.resource.hull_cp_passthrough_type->getPointerTo(src->getType()->getPointerAddressSpace())
      );
    }
    builder.CreateStore(
        builder.CreateLoad(ctx.resource.hull_cp_passthrough_type, src), ctx.resource.hull_cp_passthrough_dst
    );
    builder.CreateBr(sync);
    builder.SetInsertPoint(sync);
  }
  ctx.air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  builder.CreateBr(epilogue_bb);
  builder.SetInsertPoint(saved_block);
  return dispatch;
}

// ---- dx.op ступеней внутри dxil::Converter ----

namespace {

llvm::Value *
to_bits32(IRBuilder<> &ir, llvm::Value *value) {
  auto *type = value->getType();
  if (type->isIntegerTy(32))
    return value;
  if (type->isIntegerTy())
    return type->getIntegerBitWidth() < 32 ? ir.CreateZExt(value, ir.getInt32Ty()) : ir.CreateTrunc(value, ir.getInt32Ty());
  if (type->isFloatTy())
    return ir.CreateBitCast(value, ir.getInt32Ty());
  if (type->isHalfTy())
    return ir.CreateZExt(ir.CreateBitCast(value, ir.getInt16Ty()), ir.getInt32Ty());
  return nullptr;
}

// `type` ОБЯЗАН быть типом целевого контекста (MapType(Call.getType())): тип из контекста модуля DXIL в инструкции
// целевого модуля — UB (порча ValueHandle-карты LLVM: зависание SROA / падение в оптимизаторе).
llvm::Value *
from_bits32(IRBuilder<> &ir, llvm::Value *bits, llvm::Type *type) {
  if (type->isIntegerTy(32))
    return bits;
  if (type->isIntegerTy())
    return type->getIntegerBitWidth() < 32 ? ir.CreateTrunc(bits, type) : nullptr;
  if (type->isFloatTy())
    return ir.CreateBitCast(bits, type);
  if (type->isHalfTy())
    return ir.CreateBitCast(ir.CreateTrunc(bits, ir.getInt16Ty()), type);
  return nullptr;
}

llvm::ArrayType *
pointee_array(llvm::Value *handle) {
  if (!handle)
    return nullptr;
  return dyn_cast<llvm::ArrayType>(handle->getType()->getNonOpaquePointerElementType());
}

} // namespace

// loadInput в HS/DS/GS: входной файл — двумерный [вершина][регистр] int4 (как SrcOperandInputICP у DXBC).
llvm::Expected<llvm::Value *>
Converter::EmitLoadInputIndexed(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Row = asU32(Call.getArgOperand(2));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Row || !Component)
    return unsupported("dxil: non-constant loadInput index");
  if (*Element >= entry.inputs.size())
    return unsupported("dxil: loadInput element index out of range");
  const auto &signature = entry.inputs[*Element];
  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: input signature element is not mapped");
  if (!register_semantic(signature.semantic_kind))
    return unsupported("dxil: unsupported loadInput semantic kind=" + std::to_string((uint32_t)signature.semantic_kind) + " name=" + signature.name);
  auto Handle = ctx.resource.input.ptr_int4;
  auto TyHandle = pointee_array(Handle);
  if (!TyHandle || !TyHandle->getElementType()->isArrayTy())
    return unsupported("dxil: per-vertex input register file is not available");
  if (Call.arg_size() < 5)
    return unsupported("dxil: loadInput without vertex index in a per-vertex stage");
  auto Vertex = Get(Call.getArgOperand(4));
  if (!Vertex)
    return Vertex.takeError();
  if (!*Vertex)
    return unsupported("dxil: missing loadInput vertex index");
  uint32_t Reg = (uint32_t)signature.start_row + *Row;
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: loadInput component out of range");
  uint32_t VertexCount = TyHandle->getNumElements();
  uint32_t RegCount = cast<llvm::ArrayType>(TyHandle->getElementType())->getNumElements();
  if (Reg >= RegCount)
    return unsupported("dxil: loadInput register out of range of the previous stage output");
  llvm::Value *VertexIndex = *Vertex;
  llvm::Value *InRange = nullptr;
  if (auto *Const = dyn_cast<ConstantInt>(VertexIndex)) {
    if (Const->getZExtValue() >= VertexCount)
      return unsupported("dxil: loadInput vertex index out of range");
  } else {
    // Ограничить адрес до чтения, вне диапазона — ноль (как у GS в DXBC-пути).
    InRange = ir.CreateICmpULT(VertexIndex, ir.getInt32(VertexCount));
    VertexIndex = ir.CreateSelect(InRange, VertexIndex, ir.getInt32(0));
  }
  auto *Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), VertexIndex, ir.getInt32(Reg), ir.getInt32(Lane)});
  llvm::Value *Bits = ir.CreateLoad(ir.getInt32Ty(), Ptr);
  if (InRange)
    Bits = ir.CreateSelect(InRange, Bits, ir.getInt32(0));
  auto *Value = from_bits32(ir, Bits, MapType(Call.getType()));
  if (!Value)
    return unsupported("dxil: unsupported loadInput type");
  return Value;
}

// storeOutput в HS (фаза контрольных точек): выходной файл — [КТ][регистр] int4, индекс КТ = thread_id_in_patch.
llvm::Expected<llvm::Value *>
Converter::EmitStoreOutputHull(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Row = asU32(Call.getArgOperand(2));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Row || !Component)
    return unsupported("dxil: non-constant storeOutput index");
  if (*Element >= entry.outputs.size())
    return unsupported("dxil: storeOutput element index out of range");
  auto Value = Get(Call.getArgOperand(4));
  if (!Value)
    return Value.takeError();
  if (!*Value)
    return unsupported("dxil: missing storeOutput value");
  const auto &signature = entry.outputs[*Element];
  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: output signature element is not mapped");
  if (!register_semantic(signature.semantic_kind))
    return unsupported("dxil: unsupported hull storeOutput semantic kind=" + std::to_string((uint32_t)signature.semantic_kind) + " name=" + signature.name);
  auto Handle = ctx.resource.output.ptr_int4;
  auto TyHandle = pointee_array(Handle);
  if (!TyHandle || !TyHandle->getElementType()->isArrayTy() || !ctx.resource.thread_id_in_patch)
    return unsupported("dxil: hull output register file is not available");
  uint32_t Reg = (uint32_t)signature.start_row + *Row;
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: storeOutput component out of range");
  auto *Bits = to_bits32(ir, *Value);
  if (!Bits)
    return unsupported("dxil: unsupported storeOutput type");
  auto *Ptr = ir.CreateGEP(
      TyHandle, Handle, {ir.getInt32(0), ctx.resource.thread_id_in_patch, ir.getInt32(Reg), ir.getInt32(Lane)}
  );
  ir.CreateStore(Bits, Ptr);
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitStageOp(CallInst &Call) {
  auto *callee = Call.getCalledFunction();
  StringRef op = callee->getName().drop_front(strlen("dx.op."));
  auto dot = op.rfind('.');
  if (dot != StringRef::npos)
    op = op.take_front(dot);

  if (op == "outputControlPointID") {
    if (!ctx.resource.thread_id_in_patch)
      return unsupported("dxil: outputControlPointID outside of a hull shader");
    return ctx.resource.thread_id_in_patch;
  }
  if (op == "primitiveID") {
    if (!ctx.resource.patch_id)
      return unsupported("dxil: primitiveID is not available in this stage");
    return ctx.resource.patch_id;
  }
  if (op == "gsInstanceID") {
    if (!ctx.resource.gs_instance_id)
      return unsupported("dxil: gsInstanceID outside of a geometry shader");
    return ctx.resource.gs_instance_id;
  }
  if (op == "domainLocation") {
    auto Component = asU32(Call.getArgOperand(1));
    if (!Component || *Component > 2)
      return unsupported("dxil: domainLocation component out of range");
    if (!ctx.resource.domain)
      return unsupported("dxil: domainLocation outside of a domain shader");
    return ir.CreateExtractElement(ctx.resource.domain, *Component);
  }
  if (op == "storePatchConstant") {
    auto Element = asU32(Call.getArgOperand(1));
    auto Row = asU32(Call.getArgOperand(2));
    auto Component = asU32(Call.getArgOperand(3));
    if (!Element || !Row || !Component)
      return unsupported("dxil: non-constant storePatchConstant index");
    if (*Element >= entry.patch_constants.size())
      return unsupported("dxil: storePatchConstant element index out of range");
    auto Value = Get(Call.getArgOperand(4));
    if (!Value)
      return Value.takeError();
    if (!*Value)
      return unsupported("dxil: missing storePatchConstant value");
    const auto &signature = entry.patch_constants[*Element];
    auto Handle = ctx.resource.patch_constant_output.ptr_int4;
    auto TyHandle = pointee_array(Handle);
    if (!TyHandle)
      return unsupported("dxil: patch constant output register file is not available");
    uint32_t Reg = (uint32_t)signature.start_row + *Row;
    uint32_t Lane = (uint32_t)signature.start_col + *Component;
    if (Lane > 3 || Reg >= TyHandle->getNumElements())
      return unsupported("dxil: storePatchConstant register out of range");
    auto *Bits = to_bits32(ir, *Value);
    if (!Bits)
      return unsupported("dxil: unsupported storePatchConstant type");
    ir.CreateStore(Bits, ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}));
    return (llvm::Value *)nullptr;
  }
  if (op == "loadPatchConstant") {
    auto Element = asU32(Call.getArgOperand(1));
    auto Row = asU32(Call.getArgOperand(2));
    auto Component = asU32(Call.getArgOperand(3));
    if (!Element || !Row || !Component)
      return unsupported("dxil: non-constant loadPatchConstant index");
    if (*Element >= entry.patch_constants.size())
      return unsupported("dxil: loadPatchConstant element index out of range");
    const auto &signature = entry.patch_constants[*Element];
    auto Handle = ctx.resource.patch_constant_output.ptr_int4;
    auto TyHandle = pointee_array(Handle);
    if (!TyHandle)
      return unsupported("dxil: patch constant input register file is not available");
    uint32_t Reg = (uint32_t)signature.start_row + *Row;
    uint32_t Lane = (uint32_t)signature.start_col + *Component;
    if (Lane > 3 || Reg >= TyHandle->getNumElements())
      return unsupported("dxil: loadPatchConstant register out of range");
    auto *Bits = ir.CreateLoad(ir.getInt32Ty(), ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}));
    auto *Value = from_bits32(ir, Bits, MapType(Call.getType()));
    if (!Value)
      return unsupported("dxil: unsupported loadPatchConstant type");
    return Value;
  }
  if (op == "loadOutputControlPoint") {
    auto Element = asU32(Call.getArgOperand(1));
    auto Row = asU32(Call.getArgOperand(2));
    auto Component = asU32(Call.getArgOperand(3));
    if (!Element || !Row || !Component)
      return unsupported("dxil: non-constant loadOutputControlPoint index");
    if (*Element >= entry.outputs.size())
      return unsupported("dxil: loadOutputControlPoint element index out of range");
    const auto &signature = entry.outputs[*Element];
    auto Index = Get(Call.getArgOperand(4));
    if (!Index)
      return Index.takeError();
    if (!*Index)
      return unsupported("dxil: missing loadOutputControlPoint index");
    auto Handle = ctx.resource.output.ptr_int4;
    auto TyHandle = pointee_array(Handle);
    if (!TyHandle || !TyHandle->getElementType()->isArrayTy())
      return unsupported("dxil: hull output control points are not available for reading");
    uint32_t Reg = (uint32_t)signature.start_row + *Row;
    uint32_t Lane = (uint32_t)signature.start_col + *Component;
    if (Lane > 3)
      return unsupported("dxil: loadOutputControlPoint component out of range");
    auto *Ptr = ir.CreateGEP(TyHandle, Handle, {ir.getInt32(0), *Index, ir.getInt32(Reg), ir.getInt32(Lane)});
    auto *Value = from_bits32(ir, ir.CreateLoad(ir.getInt32Ty(), Ptr), MapType(Call.getType()));
    if (!Value)
      return unsupported("dxil: unsupported loadOutputControlPoint type");
    return Value;
  }
  if (op == "emitStream" || op == "cutStream" || op == "emitThenCutStream") {
    auto Stream = asU32(Call.getArgOperand(1));
    if (!Stream || *Stream != 0)
      return unsupported("dxil: geometry shader stream other than 0");
    if (!ctx.resource.call_emit || !ctx.resource.call_cut)
      return unsupported("dxil: emit/cut outside of a geometry shader");
    if (op != "cutStream")
      ctx.resource.call_emit();
    if (op != "emitStream")
      ctx.resource.call_cut();
    return (llvm::Value *)nullptr;
  }
  return unsupported("dxil: unsupported operation dx.op." + op.str());
}

} // namespace dxmt::dxil
