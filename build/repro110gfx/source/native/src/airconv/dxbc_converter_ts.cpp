// Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "air_signature.hpp"
#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "dxil_converter.hpp"
#include "dxil_converter_stages.hpp"
#include "dxbc_converter_tess_prepass.hpp"   // GPU-PASS TESS_PREPASS
#include "dxbc_tess_records.hpp"
#include "stage_linkage.hpp"
#include "nt/air_builder.hpp"
#include "nt/dxbc_converter_base.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/AtomicOrdering.h"

namespace dxmt::dxbc {

static void canonicalize_tessellation_math(llvm::Function &function);

static SM50_SHADER_TESS_SPLIT_WORKLOAD_DATA *
get_split_workload(SM50_SHADER_COMPILATION_ARGUMENT_DATA *args) {
  SM50_SHADER_TESS_SPLIT_WORKLOAD_DATA *split = nullptr;
  args_get_data<SM50_SHADER_TESS_SPLIT_WORKLOAD, SM50_SHADER_TESS_SPLIT_WORKLOAD_DATA>(args, &split);
  return split;
}

struct TessMeshWorkload {
  short inner0[2];
  short inner1[2];
  short outer0[2];
  short outer1[2];
  int inner_factor;
  int outer_factor;
  char inner_factor_i;
  char outer_factor_i;
  bool has_complement;
  bool isoline;
  short inner2[2];
  short inner3[2];
  short outer2[2];
  short outer3[2];
  int inner_factor_23;
  int outer_factor_23;
  char inner_factor_23_i;
  char outer_factor_23_i;
  short patch_index;
};

TessellatorPartitioning
get_partitioning(SM50ShaderInternal *pHullStage) {
  TessellatorPartitioning partitioning;
  switch (pHullStage->tessellation_partition) {
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_POW2:
    partitioning = TessellatorPartitioning::pow2;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD:
    partitioning = TessellatorPartitioning::fractional_odd;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN:
    partitioning = TessellatorPartitioning::fractional_even;
    break;
  default:
    partitioning = TessellatorPartitioning::integer;
    break;
  }
  return partitioning;
}

TessellatorOutputPrimitive
get_output_primitive(SM50ShaderInternal *pHullStage) {
  TessellatorOutputPrimitive primitive;
  switch (pHullStage->tessellator_output_primitive) {
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE:
    primitive = TessellatorOutputPrimitive::line;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW:
    primitive = TessellatorOutputPrimitive::triangle;
    break;
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW:
    primitive = TessellatorOutputPrimitive::triangle_ccw;
    break;
  default:
    primitive = TessellatorOutputPrimitive::point;
    break;
  }
  return primitive;
}

uint32_t
get_integer_factor(float factor, SM50ShaderInternal *pHullStage) {
  uint32_t integer_factor = 0;
  switch (pHullStage->tessellation_partition) {
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_UNDEFINED:
    return 0;
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER: {
    factor = std::clamp<float>(factor, 1.0, 64.0);
    integer_factor = std::ceil(factor);
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_POW2: {
    integer_factor = std::clamp<float>(factor, 1.0, 64.0);
    integer_factor = integer_factor == 1 ? 1 : 1 << (32 - __builtin_clz(integer_factor - 1));
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD: {
    factor = std::clamp<float>(factor, 1.0, 63.0);
    integer_factor = std::ceil(factor);
    integer_factor = integer_factor & 1 ? integer_factor : integer_factor + 1;
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN: {
    factor = std::clamp<float>(factor, 2.0, 64.0);
    integer_factor = std::ceil(factor);
    integer_factor = integer_factor & 1 ? integer_factor + 1 : integer_factor;
    break;
  }
  }
  return integer_factor;
}

uint32_t
get_max_potential_workload_count(uint32_t max_tess_factor, SM50ShaderInternal *pHullStage, float max_density_factor) {

  switch (pHullStage->tessellation_domain) {
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_UNDEFINED:
    return 0;
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
    return uint32_t(std::ceil(std::clamp(max_density_factor, 1.0f, 64.0f)));
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_TRI:
    return std::ceil((max_tess_factor - 1) / 4.0) * 3 + (max_tess_factor & 1);
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
    return std::ceil((max_tess_factor - 1) / 4.0) * 4 + (max_tess_factor & 1);
    ;
  }
}

size_t
estimate_payload_size(SM50ShaderInternal *pHullStage, float factor, uint32_t patch_per_group) {
  auto max_hs_output_register = pHullStage->max_output_register;
  auto cp_size_per_point = sizeof(uint32_t) * 4 * max_hs_output_register;
  auto cp_size_per_patch = cp_size_per_point * pHullStage->output_control_point_count;
  auto cp_size_per_group = cp_size_per_patch * patch_per_group;
  auto pc_size_per_patch = sizeof(uint32_t) * pHullStage->patch_constant_scalars.size();
  auto pc_size_per_group = pc_size_per_patch * patch_per_group;

  auto factor_int = get_integer_factor(factor, pHullStage);
  uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage, factor);
  constexpr uint32_t size_workload_info = sizeof(TessMeshWorkload);

  return cp_size_per_group + pc_size_per_group + sizeof(uint32_t) +
         max_workload_count * patch_per_group * size_workload_info;
};

size_t estimate_mesh_size(SM50ShaderInternal *pDomainStage, uint32_t max_potential_factor_int) {
  /*
  to calculate mesh size:
  max_vertex_count * size_per_vertex
  = calc_max_vertex_count(tess_factor) * ds_output_register_count * sizeof(vec4)

  NOTE: it assumes 1.all output registers are vec4 2.primitive count <= vertex count
  */
  size_t size_per_vertex = pDomainStage->max_output_register * 16;
  size_t max_edge_point = max_potential_factor_int + 1;
  return ((max_edge_point + 2 - (max_edge_point & 1)) * 2 + 1) * size_per_vertex;
}

std::pair<float, uint32_t>
get_final_maxtessfactor(SM50ShaderInternal *pHullStage, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs) {
  /*
  the factor received by domain (mesh) shader will be strictly less or equal to returned value
  so we can reserve enough spaces for output mesh
  */
  float maxtessfactor = pHullStage->max_tesselation_factor;
  uint32_t maxtessfactor_int = get_integer_factor(maxtessfactor, pHullStage);
  SM50_SHADER_PSO_TESSELLATOR_DATA *pso_tess = nullptr;
  if (args_get_data<SM50_SHADER_PSO_TESSELLATOR, SM50_SHADER_PSO_TESSELLATOR_DATA>(pArgs, &pso_tess) &&
      maxtessfactor_int > pso_tess->max_potential_tess_factor) {
    maxtessfactor = pso_tess->max_potential_tess_factor;
    do {
      maxtessfactor_int = get_integer_factor(maxtessfactor, pHullStage);
      if (maxtessfactor_int <= pso_tess->max_potential_tess_factor)
        return {maxtessfactor, maxtessfactor_int};
      maxtessfactor = maxtessfactor - 1.0f;
    } while (maxtessfactor > 1.0f);
    auto minimal_factor = get_integer_factor(1.0f, pHullStage);
    return {minimal_factor, minimal_factor};
  }
  return {maxtessfactor, maxtessfactor_int};
}

llvm::Error
convert_dxbc_vertex_hull_shader(
    SM50ShaderInternal *pVertexStage, SM50ShaderInternal *pHullStage, const char *name, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  // NOTE: func_signature of vertex stage only defines some outputs
  // that we don't need them in vertex-hull object shader.
  // And func_signature of hull stage simply has nothing defined yet
  // TODO: should get rid of func_signature in SM50ShaderInternal and
  // always create a new fresh instance + use signature handlers
  auto func_signature = pHullStage->func_signature;
  auto &vertex_shader_info = pVertexStage->shader_info;
  auto &hull_shader_info = pHullStage->shader_info;

  uint32_t max_vs_input_register = pVertexStage->max_input_register;
  uint32_t max_vs_output_register = 0;
  for (auto &out : pVertexStage->output_signature) {
    max_vs_output_register = std::max(max_vs_output_register, out.reg() + 1);
  }
  if (pVertexStage->dxil)   // MacRunner dxil-tess 17.10: у VS из DXIL подпись — в dxil_entry
    max_vs_output_register = std::max(max_vs_output_register, dxil::max_register(pVertexStage->dxil_entry.outputs));
  uint32_t max_hs_output_register = pHullStage->max_output_register;
  uint32_t max_patch_constant_output_register = pHullStage->max_patch_constant_output_register;

  const auto link_rows = [](const std::vector<Signature> &signatures) {
    std::vector<stage_linkage::Row> rows;
    for (const auto &s : signatures)
      rows.push_back({s.semanticName(), s.semanticIndex(), s.systemValue(), uint32_t(s.componentType()),
                     s.reg(), s.mask(), s.usageMask(), s.stream(), s.precision()});
    return rows;
  };
  stage_linkage::VertexHullPlan vertex_link;
  // MacRunner dxil-tess 17.10: строки линковки у DXIL-ступеней — по подписи DXIL (имя+индекс семантики)
  auto vs_rows = pVertexStage->dxil ? dxil::linkage_rows(pVertexStage->dxil_entry.outputs, true)
                                    : link_rows(pVertexStage->output_signature);
  auto hs_rows = pHullStage->dxil ? dxil::linkage_rows(pHullStage->dxil_entry.inputs, false)
                                  : link_rows(pHullStage->input_signature);
  if (!stage_linkage::vertex_hull(vs_rows, hs_rows, vertex_link))
    return llvm::make_error<UnsupportedFeature>("Invalid VS-HS semantic linkage");
  SM50_SHADER_TESS_VERTEX_LINKAGE_DATA *vertex_link_arg = nullptr;
  args_get_data<SM50_SHADER_TESS_VERTEX_LINKAGE, SM50_SHADER_TESS_VERTEX_LINKAGE_DATA>(pArgs, &vertex_link_arg);
  if (vertex_link_arg) vertex_link_arg->applied = 0;
  if (!vertex_link.identity && !vertex_link_arg)
    return llvm::make_error<UnsupportedFeature>("VS-HS relocation requires an acknowledged linkage argument");

  auto [final_maxtessfactor, factor_int] = get_final_maxtessfactor(pHullStage, pArgs);

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig_vs = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE2, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig_vs);

  bool is_indexed_draw = ia_layout && ia_layout->index_buffer_format > 0;

  Prologues prologue_vs;
  Prologues prologue_hs;
  Epilogues epilogue_vs;
  Epilogues epilogue_hs;

  io_binding_map resource_map_vs;
  io_binding_map resource_map_hs;
  air::AirType types(context);

  {
    SignatureContext sig_ctx_vs(prologue_vs, epilogue_vs, func_signature, resource_map_vs);
    sig_ctx_vs.ia_layout = ia_layout;
    sig_ctx_vs.skip_vertex_output = true;
    for (auto &p : pVertexStage->signature_handlers) {
      p(sig_ctx_vs);
    }
    SignatureContext sig_ctx_hs(prologue_hs, epilogue_hs, func_signature, resource_map_hs);
    for (auto &p : pHullStage->signature_handlers) {
      p(sig_ctx_hs);
    }
  }

  auto binding_map = rootsig_vs ? setup_binding_rootsig(
                                   &vertex_shader_info, func_signature, module, D3D10_SB_VERTEX_SHADER,
                                   rootsig_vs->bytecode, rootsig_vs->bytecode_length
                               )
                             : setup_binding_table2(
                                   &vertex_shader_info, func_signature, module, SM50_BINDING_INDEX_CONSTANT_BUFFER2,
                                   SM50_BINDING_INDEX_ARGUMENT_TABLE2
                               );
  auto binding_map_hs = rootsig ? setup_binding_rootsig(
                                      &hull_shader_info, func_signature, module, D3D11_SB_HULL_SHADER,
                                      rootsig->bytecode, rootsig->bytecode_length
                                  )
                                : setup_binding_table2(&hull_shader_info, func_signature, module);

  uint32_t threads_per_patch = next_pow2(pHullStage->hull_maximum_threads_per_patch);
  uint32_t patch_per_group = next_pow2(32 / threads_per_patch);

  auto hs_output_per_point_type = llvm::ArrayType::get(types._int4, max_hs_output_register);
  auto hs_output_per_patch_type =
      llvm::ArrayType::get(hs_output_per_point_type, pHullStage->output_control_point_count);
  auto hs_output_per_group_type = llvm::ArrayType::get(hs_output_per_patch_type, patch_per_group);
  auto hs_output_per_point_type_float = llvm::ArrayType::get(types._float4, max_hs_output_register);
  auto hs_output_per_patch_type_float =
      llvm::ArrayType::get(hs_output_per_point_type_float, pHullStage->output_control_point_count);

  auto hs_pcout_per_patch_type = llvm::ArrayType::get(types._int4, max_patch_constant_output_register);
  auto hs_pcout_per_group_type = llvm::ArrayType::get(hs_pcout_per_patch_type, patch_per_group);
  auto hs_pcout_per_patch_type_float = llvm::ArrayType::get(types._float4, max_patch_constant_output_register);

  auto hs_pcout_per_patch_scaler_type = llvm::ArrayType::get(types._int, pHullStage->patch_constant_scalars.size());
  auto hs_pcout_per_group_scaler_type = llvm::ArrayType::get(hs_pcout_per_patch_scaler_type, patch_per_group);

  uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage, final_maxtessfactor);

  // GPU-PASS TESS_PREPASS: the same object function as a kernel; its payload is one device slot per threadgroup.
  SM50_SHADER_TESS_PREPASS_DATA *prepass = nullptr;
  args_get_data<SM50_SHADER_TESS_PREPASS, SM50_SHADER_TESS_PREPASS_DATA>(pArgs, &prepass);
  if (prepass) {
    prepass->applied = 0;
    if (prepass->mode != SM50_TESS_PREPASS_HULL_KERNEL)
      return llvm::make_error<UnsupportedFeature>("tess prepass: wrong mode for the hull stage");
    if (pHullStage->tessellation_domain == microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE ||
        (pHullStage->tessellator_output_primitive != microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
         pHullStage->tessellator_output_primitive != microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW))
      return llvm::make_error<UnsupportedFeature>("tess prepass: only triangle output of triangle / quad domains");
  }
  const bool prepass_kernel = prepass != nullptr;

  auto *split_workload = prepass_kernel ? nullptr : get_split_workload(pArgs);
  if (split_workload && pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
      pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW)
    return llvm::make_error<UnsupportedFeature>("Split tessellation requires triangle output");
  if (prepass_kernel)
    func_signature.UseMaxWorkgroupSize(threads_per_patch * patch_per_group);
  else
    func_signature.UseMaxMeshWorkgroupSize(max_workload_count * patch_per_group * (split_workload ? 2 : 1));

  constexpr uint32_t size_workload_info = sizeof(TessMeshWorkload);

  // GPU-PASS TESS_PREPASS v2: the slot of the hull kernel holds one 4-byte record per workload instead of the workload
  auto payload_struct_type = llvm::StructType::create(
      context,
      {hs_output_per_group_type, hs_pcout_per_group_scaler_type, types._int,
       llvm::ArrayType::get(types._int, max_workload_count * patch_per_group * (prepass_kernel ? 1 : size_workload_info / 4))},
      "payload"
  );
  uint32_t payload_struct_size = module.getDataLayout().getTypeAllocSize(payload_struct_type);

  uint32_t payload_idx = prepass_kernel ? ~0u : func_signature.DefineInput(air::InputPayload{.size = payload_struct_size});
  uint32_t thread_id_idx = func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  // (batched_patch_id_start, instance_id, 0)
  uint32_t tg_id_idx = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  uint32_t prepass_tgs_idx = ~0u, prepass_slots_idx = ~0u, prepass_groups_idx = ~0u;
  if (!prepass_kernel) {
    func_signature.DefineInput(air::InputMeshGridProperties{});
  } else {
    auto device_uint_buffer = [](uint32_t index, const char *name) {
      return air::ArgumentBindingBuffer{
          .buffer_size = {}, .location_index = index, .array_size = 0, .memory_access = air::MemoryAccess::read_write,
          .address_space = air::AddressSpace::device, .type = air::msl_uint, .arg_name = name, .raster_order_group = {}};
    };
    prepass_tgs_idx = func_signature.DefineInput(air::InputThreadgroupsPerGrid{});
    prepass_slots_idx = func_signature.DefineInput(device_uint_buffer(SM50_TESS_PREPASS_BINDING_SLOTS, "tess_slots"));
    prepass_groups_idx = func_signature.DefineInput(device_uint_buffer(SM50_TESS_PREPASS_BINDING_GROUPS, "tess_groups"));
  }
  uint32_t draw_argument_idx = func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = SM50_BINDING_INDEX_DRAW_ARGUMENTS,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = is_indexed_draw //
                  ? air::MSLWhateverStruct{"draw_indexed_arguments", types._dxmt_draw_indexed_arguments}
                  : air::MSLWhateverStruct{"draw_arguments", types._dxmt_draw_arguments},
      .arg_name = "draw_arguments",
      .raster_order_group = {}
  });

  uint32_t index_buffer_idx = ~0u;
  if (is_indexed_draw) {
    index_buffer_idx = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {},
        .location_index = SM50_BINDING_INDEX_INDEX_BUFFER,
        .array_size = 0,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::device,
        .type = ia_layout->index_buffer_format == 1 ? air::MSLRepresentableType(air::MSLUshort{})
                                                    : air::MSLRepresentableType(air::MSLUint{}),
        .raster_order_group = {}
    });
  }

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, true);

  auto entry_bb_global = llvm::BasicBlock::Create(context, "entry", function);
  auto vertex_stage_end = llvm::BasicBlock::Create(context, "vertex_end", function);
  llvm::IRBuilder<> builder(entry_bb_global);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  // these values are initialized by vertex stage and accessed by hull stage
  llvm::Value *instance_id = nullptr;
  llvm::Value *batched_patch_start = nullptr;
  llvm::Value *patch_id = nullptr;
  llvm::Value *patch_count = nullptr;

  setup_temp_register(&vertex_shader_info, resource_map_vs, types, module, builder);
  setup_temp_register(&hull_shader_info, resource_map_hs, types, module, builder);
  setup_immediate_constant_buffer(&vertex_shader_info, resource_map_vs, types, module, builder);
  setup_immediate_constant_buffer(&hull_shader_info, resource_map_hs, types, module, builder);

  auto output_reg_per_point_type = llvm::ArrayType::get(types._int4, vertex_link.registers);
  auto output_reg_per_patch_type =
      llvm::ArrayType::get(output_reg_per_point_type, pHullStage->input_control_point_count);
  auto output_reg_per_group_type = llvm::ArrayType::get(output_reg_per_patch_type, patch_per_group);
  auto output_reg_per_point_type_float = llvm::ArrayType::get(types._float4, vertex_link.registers);
  auto output_reg_per_patch_type_float =
      llvm::ArrayType::get(output_reg_per_point_type_float, pHullStage->input_control_point_count);

  llvm::GlobalVariable *vertex_out = new llvm::GlobalVariable(
      module, output_reg_per_group_type, false, llvm::GlobalValue::InternalLinkage,
      llvm::UndefValue::get(output_reg_per_group_type), "vertex_out_hull_in", nullptr,
      llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
  );
  vertex_out->setAlignment(llvm::Align(4));

  auto thread_position_in_group = function->getArg(thread_id_idx);
  auto control_point_id_in_patch = builder.CreateExtractElement(thread_position_in_group, (uint32_t)0);
  auto patch_offset_in_group = builder.CreateExtractElement(thread_position_in_group, 1);

  {
    /* Vertex Shader Zone */
    auto shader_info = vertex_shader_info;
    auto &resource_map = resource_map_vs;
    auto &prologue = prologue_vs;
    auto &epilogue = epilogue_vs;
    auto max_input_register = max_vs_input_register;
    auto max_output_register = max_vs_output_register;

    auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue_vertex", function);
    setup_metal_version(module, metal_version);

    auto active = llvm::BasicBlock::Create(context, "active_vertex", function);

    auto threadgroup_position_in_grid = function->getArg(tg_id_idx);
    batched_patch_start = builder.CreateMul(
        builder.CreateExtractElement(threadgroup_position_in_grid, (uint32_t)0),
        builder.getInt32(32 / threads_per_patch)
    );
    patch_id = builder.CreateAdd(batched_patch_start, patch_offset_in_group);
    instance_id = builder.CreateExtractElement(threadgroup_position_in_grid, (uint32_t)1);
    auto control_point_index = builder.CreateAdd(
        builder.CreateMul(patch_id, builder.getInt32(pHullStage->input_control_point_count)), control_point_id_in_patch
    );

    auto draw_arguments = builder.CreateLoad(
        is_indexed_draw ? types._dxmt_draw_indexed_arguments : types._dxmt_draw_arguments,
        function->getArg(draw_argument_idx)
    );

    patch_count = builder.CreateUDiv(
        builder.CreateExtractValue(draw_arguments, 0), builder.getInt32(pHullStage->input_control_point_count)
    );

    resource_map.input.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
    resource_map.input.ptr_float4 = builder.CreateBitCast(
        resource_map.input.ptr_int4, llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
    );
    resource_map.input_element_count = max_input_register;

    // of type "output_reg_per_point_type"
    auto shared_output = builder.CreateGEP(
        output_reg_per_group_type, vertex_out, {builder.getInt32(0), patch_offset_in_group, control_point_id_in_patch}
    );
    // Keep the producer's register layout private while relocating scalar bits.
    // In-place copies could overwrite another semantic before it is read.
    resource_map.output.ptr_int4 = vertex_link.identity ? shared_output :
        builder.CreateAlloca(output_reg_per_point_type, nullptr, "vertex_link_source");
    resource_map.output.ptr_float4 = builder.CreateBitCast(
        resource_map.output.ptr_int4, output_reg_per_point_type_float->getPointerTo(
            resource_map.output.ptr_int4->getType()->getPointerAddressSpace())
    );

    resource_map.output_element_count = max_output_register;

    struct context ctx {
        .builder = builder,
        .air = air,
        .binding = *binding_map,
        .llvm = context,
        .module = module,
        .function = function,
        .resource = resource_map,
        .types = types,
        .pso_sample_mask = 0xffffffff,
        .shader_type = pVertexStage->shader_type,
        .metal_version = metal_version,
    };

    builder.CreateCondBr(
        builder.CreateLogicalAnd(
            builder.CreateICmp(
                llvm::CmpInst::ICMP_ULT, control_point_id_in_patch,
                builder.getInt32(pHullStage->input_control_point_count)
            ),
            builder.CreateICmp(llvm::CmpInst::ICMP_ULT, patch_id, patch_count)
        ),
        active, vertex_stage_end
    );
    builder.SetInsertPoint(active);

    if (index_buffer_idx != ~0u) {
      auto start_index = builder.CreateExtractValue(draw_arguments, 2);
      auto index_buffer = function->getArg(index_buffer_idx);
      auto index_buffer_element_type = index_buffer->getType()->getNonOpaquePointerElementType();
      auto vertex_id = builder.CreateLoad(
          index_buffer_element_type,
          builder.CreateGEP(
              index_buffer_element_type, index_buffer, {builder.CreateAdd(start_index, control_point_index)}
          )
      );
      resource_map.vertex_id = builder.CreateZExt(vertex_id, types._int);
    } else {
      resource_map.vertex_id = control_point_index;
    }

    resource_map.base_vertex_id = builder.CreateExtractValue(draw_arguments, is_indexed_draw ? 3 : 2);
    resource_map.instance_id = instance_id;
    resource_map.vertex_id_with_base = builder.CreateAdd(resource_map.vertex_id, resource_map.base_vertex_id);
    resource_map.base_instance_id = builder.CreateExtractValue(draw_arguments, is_indexed_draw ? 4 : 3);
    resource_map.instance_id_with_base = builder.CreateAdd(resource_map.instance_id, resource_map.base_instance_id);

    IOHelper io(air, function, types, resource_map);

    if (auto err = prologue.Run(io)) {
      return err;
    }
    auto real_entry = pVertexStage->dxil   // MacRunner dxil-tess 17.10: тело VS из DXIL
                          ? dxil::convert_basicblocks(pVertexStage->dxil_entry, ctx, epilogue_bb)
                          : convert_basicblocks(pVertexStage->entry(), ctx, epilogue_bb);
    if (auto err = real_entry.takeError()) {
      return err;
    }
    builder.CreateBr(real_entry.get());

    builder.SetInsertPoint(epilogue_bb);
    auto epilogue_result = epilogue.Run(function->getReturnType(), io);
    if (auto err = epilogue_result.takeError()) {
      return err;
    }

    if (!vertex_link.identity) {
      for (uint32_t i = 0; i < vertex_link.count; ++i) {
        const auto &lane = vertex_link.lanes[i];
        auto source = builder.CreateGEP(output_reg_per_point_type, resource_map.output.ptr_int4,
            {builder.getInt32(0), builder.getInt32(lane.source_register), builder.getInt32(lane.source_component)});
        auto target = builder.CreateGEP(output_reg_per_point_type, shared_output,
            {builder.getInt32(0), builder.getInt32(lane.target_register), builder.getInt32(lane.target_component)});
        builder.CreateStore(builder.CreateLoad(types._int, source), target);
      }
    }

    builder.CreateBr(vertex_stage_end);
    builder.SetInsertPoint(vertex_stage_end);

    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  }

  {

    /* Hull Shader Zone */
    auto shader_info = hull_shader_info;
    auto &resource_map = resource_map_hs;
    auto &prologue = prologue_hs;
    auto &epilogue = epilogue_hs;

    auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue_hull", function);

    uint32_t vertex_max_output_register = vertex_link.registers;

    llvm::Value *payload = nullptr;
    llvm::Value *prepass_group = nullptr;   // linear object threadgroup: x + y * groups_x (grid order of the mesh path)
    if (!prepass_kernel) {
      payload = builder.CreateBitCast(function->getArg(payload_idx), payload_struct_type->getPointerTo(6));
    } else {
      auto tg = function->getArg(tg_id_idx);
      auto tgs = function->getArg(prepass_tgs_idx);
      prepass_group = builder.CreateAdd(
          builder.CreateExtractElement(tg, uint64_t(0)),
          builder.CreateMul(builder.CreateExtractElement(tg, uint64_t(1)), builder.CreateExtractElement(tgs, uint64_t(0)))
      );
      auto slot = builder.CreateGEP(
          types._int, function->getArg(prepass_slots_idx), {builder.CreateMul(prepass_group, builder.getInt32(payload_struct_size / 4))}
      );
      payload = builder.CreateBitCast(slot, payload_struct_type->getPointerTo(1));
    }

    assert(pHullStage->input_control_point_count != ~0u);

    resource_map.input.ptr_int4 =
        builder.CreateGEP(output_reg_per_group_type, vertex_out, {builder.getInt32(0), patch_offset_in_group});
    resource_map.input.ptr_float4 = builder.CreateBitCast(
        resource_map.input.ptr_int4, output_reg_per_patch_type_float->getPointerTo(vertex_out->getAddressSpace())
    );
    resource_map.input_element_count = vertex_max_output_register;

    resource_map.instance_id = instance_id;

    resource_map.patch_id = patch_id;

    resource_map.thread_id_in_patch = builder.CreateSelect(
        builder.CreateICmp(llvm::CmpInst::ICMP_ULT, resource_map.patch_id, patch_count), control_point_id_in_patch,
        builder.getInt32(32) // so all phases are effectively skipped
    );

    if (shader_info.no_control_point_phase_passthrough) {
      // TODO: check this out carefully......
      assert(pHullStage->output_control_point_count != ~0u);

      if (shader_info.output_control_point_read) {
        llvm::GlobalVariable *control_point_phase_out = new llvm::GlobalVariable(
            module, hs_output_per_group_type, false, llvm::GlobalValue::InternalLinkage,
            llvm::UndefValue::get(hs_output_per_group_type), "hull_control_point_phase_out", nullptr,
            llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
        );
        control_point_phase_out->setAlignment(llvm::Align(4));
        resource_map.output.ptr_int4 = builder.CreateGEP(
            hs_output_per_group_type, control_point_phase_out, {builder.getInt32(0), patch_offset_in_group}
        );

        resource_map.hull_cp_passthrough_dst = builder.CreateGEP(
            payload_struct_type, payload,
            {builder.getInt32(0), builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
        );
        resource_map.hull_cp_passthrough_src = builder.CreateGEP(
            hs_output_per_group_type, control_point_phase_out,
            {builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
        );
        resource_map.hull_cp_passthrough_type = hs_output_per_point_type;
      } else {
        resource_map.output.ptr_int4 = builder.CreateGEP(
            payload_struct_type, payload,
            {builder.getInt32(0), builder.getInt32(0) /* field: control points*/, patch_offset_in_group}
        );
      }
      resource_map.output.ptr_float4 = builder.CreateBitCast(
          resource_map.output.ptr_int4,
          hs_output_per_patch_type_float->getPointerTo(
              cast<llvm::PointerType>(resource_map.output.ptr_int4->getType())->getPointerAddressSpace()
          )
      );
      resource_map.output_element_count = max_hs_output_register;
    } else {
      /* since no control point phase */
      if (vertex_max_output_register < max_hs_output_register) {
        return llvm::make_error<UnsupportedFeature>(
            "Hull shader has control point phase pass-through, but vertex shader "
            "output signature doesn't match hull shader input signature."
        );
      }
      resource_map.output.ptr_float4 = resource_map.input.ptr_float4;
      resource_map.output.ptr_int4 = resource_map.input.ptr_int4;
      resource_map.output_element_count = resource_map.input_element_count;

      resource_map.hull_cp_passthrough_dst = builder.CreateGEP(
          payload_struct_type, payload,
          {builder.getInt32(0), builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
      );
      resource_map.hull_cp_passthrough_src = builder.CreateGEP(
          output_reg_per_group_type, vertex_out,
          {builder.getInt32(0), patch_offset_in_group, resource_map.thread_id_in_patch}
      );
      resource_map.hull_cp_passthrough_type = hs_output_per_point_type;
    }

    if (max_patch_constant_output_register) {
      /* all instances write to the same output (threadgroup memory) */
      llvm::GlobalVariable *patch_constant_out = new llvm::GlobalVariable(
          module, hs_pcout_per_group_type, false, llvm::GlobalValue::InternalLinkage,
          llvm::UndefValue::get(hs_pcout_per_group_type), "hull_patch_constant_out", nullptr,
          llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
      );
      patch_constant_out->setAlignment(llvm::Align(4));
      resource_map.patch_constant_output.ptr_int4 =
          builder.CreateGEP(hs_pcout_per_group_type, patch_constant_out, {builder.getInt32(0), patch_offset_in_group});
      resource_map.patch_constant_output.ptr_float4 = builder.CreateBitCast(
          resource_map.patch_constant_output.ptr_int4,
          hs_pcout_per_patch_type_float->getPointerTo((uint32_t)air::AddressSpace::threadgroup)
      );
    }

    struct context ctx {
        .builder = builder,
        .air = air,
        .binding = *binding_map_hs,
        .llvm = context,
        .module = module,
        .function = function,
        .resource = resource_map,
        .types = types,
        .pso_sample_mask = 0xffffffff,
        .shader_type = pHullStage->shader_type,
        .metal_version = metal_version,
    };
    dxbc::Converter dxbc(ctx.air, ctx, ctx.resource);
    dxbc.TessPrepassDevice = prepass_kernel;   // GPU-PASS TESS_PREPASS: workload generators in their device copies

    IOHelper io(air, function, types, resource_map);

    if (auto err = prologue.Run(io)) {
      return err;
    }
    auto real_entry = pHullStage->dxil   // MacRunner dxil-tess 17.10: тело HS из DXIL (main + patch-constant)
                          ? dxil::convert_hull_shader_body(pHullStage, ctx, epilogue_bb)
                          : convert_basicblocks(pHullStage->entry(), ctx, epilogue_bb);
    if (auto err = real_entry.takeError()) {
      return err;
    }
    builder.CreateBr(real_entry.get());

    builder.SetInsertPoint(epilogue_bb);

    /* populate patch constant output */

    auto write_patch_constant = llvm::BasicBlock::Create(context, "write_patch_constant", function);
    auto workload_ready = llvm::BasicBlock::Create(context, "workload_ready", function);
    auto dispatch_mesh = llvm::BasicBlock::Create(context, "dispatch_mesh", function);
    auto real_return = llvm::BasicBlock::Create(context, "real_return", function);

    llvm::GlobalVariable *workload_count = new llvm::GlobalVariable(
        module, types._int, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(types._int),
        "workload_count", nullptr, llvm::GlobalValue::NotThreadLocal, (uint32_t)air::AddressSpace::threadgroup
    );
    workload_count->setAlignment(llvm::Align(4));
    // All lanes, including inactive final patches, initialize and rendezvous before producers diverge.
    air.CreateAtomicRMW(llvm::AtomicRMWInst::And, workload_count, builder.getInt32(0));
    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    builder.CreateCondBr(
        builder.CreateICmp(llvm::CmpInst::ICMP_EQ, resource_map.thread_id_in_patch, builder.getInt32(0)),
        write_patch_constant, workload_ready
    );

    builder.SetInsertPoint(write_patch_constant);
    auto epilogue_result = epilogue.Run(function->getReturnType(), io);
    if (auto err = epilogue_result.takeError()) {
      return err;
    }

    std::array<llvm::Value *, 6> tess_factors;
    tess_factors.fill(air.getFloat(0));

    auto max_tess_factor_value = air.getFloat(final_maxtessfactor);

    for (unsigned i = 0; i < pHullStage->patch_constant_scalars.size(); i++) {
      auto pc_scalar = pHullStage->patch_constant_scalars[i];
      auto dst_ptr = builder.CreateGEP(
          payload_struct_type, payload,
          {builder.getInt32(0),   // struct
           builder.getInt32(1),   // pcout
           patch_offset_in_group, // patch_id,
           builder.getInt32(i)}
      );
      auto src_ptr = builder.CreateGEP(
          hs_pcout_per_patch_type, resource_map.patch_constant_output.ptr_int4,
          {builder.getInt32(0), builder.getInt32(pc_scalar.reg), builder.getInt32(pc_scalar.component)}
      );
      if (pc_scalar.tess_factor_index >= 0 && pc_scalar.tess_factor_index < 6) {
        auto value = builder.CreateBitCast(builder.CreateLoad(types._int, src_ptr), types._float);
        auto value_clamp = air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, value, max_tess_factor_value);
        // fmin selects the numeric operand for NaN, but a NaN edge must cull the patch.
        value_clamp = builder.CreateSelect(builder.CreateFCmpUNO(value, value), value, value_clamp);
        tess_factors[pc_scalar.tess_factor_index] = value_clamp;
        builder.CreateStore(builder.CreateBitCast(value_clamp, types._int), dst_ptr);
      } else {
        builder.CreateStore(builder.CreateLoad(types._int, src_ptr), dst_ptr);
      }
    };

    switch (pHullStage->tessellation_domain) {
    case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
      dxbc.HullGenerateWorkloadForIsoline(
          patch_offset_in_group, workload_count,
          builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}),
          get_partitioning(pHullStage), tess_factors[1], tess_factors[0]
      );
      break;
    case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
      dxbc.HullGenerateWorkloadForQuad(
          patch_offset_in_group, workload_count,
          builder.CreateGEP(
              payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}
          ),
          get_partitioning(pHullStage), tess_factors[4], tess_factors[5], tess_factors[0], tess_factors[1],
          tess_factors[2], tess_factors[3]
      );
      break;
    default: {
      dxbc.HullGenerateWorkloadForTriangle(
          patch_offset_in_group, workload_count,
          builder.CreateGEP(
              payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}
          ),
          get_partitioning(pHullStage), tess_factors[3], tess_factors[0], tess_factors[1], tess_factors[2]
      );
    }
    }

    builder.CreateBr(workload_ready);
    builder.SetInsertPoint(workload_ready);
    // Publish every patch's payload and count before the unique dispatch lane reads either.
    air.CreateBarrier(llvm::air::MemFlags(uint32_t(llvm::air::MemFlags::Threadgroup) |
                                       uint32_t(prepass_kernel ? llvm::air::MemFlags::Device : llvm::air::MemFlags::ObjectData)));
    if (prepass_kernel) {
      // GPU-PASS TESS_PREPASS: one lane of the group sums its workload records instead of dispatching mesh threadgroups;
      // batched_patch_start goes to the slot like the payload field it is.
      auto lane0 = builder.CreateAnd(
          builder.CreateICmpEQ(builder.CreateExtractElement(function->getArg(thread_id_idx), 0ull), builder.getInt32(0)),
          builder.CreateICmpEQ(builder.CreateExtractElement(function->getArg(thread_id_idx), 1ull), builder.getInt32(0))
      );
      builder.CreateCondBr(lane0, dispatch_mesh, real_return);
      builder.SetInsertPoint(dispatch_mesh);
      auto *count = builder.CreateLoad(types._int, workload_count);
      auto *meta = builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)});
      auto *group = builder.CreateGEP(types._int, function->getArg(prepass_groups_idx), {builder.CreateMul(prepass_group, builder.getInt32(4))});
      dxbc.PrepassGroupTotals(count, meta, group);
      builder.CreateStore(
          batched_patch_start,
          builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(2)})
      );
      builder.CreateBr(real_return);
    } else {
    builder.CreateCondBr(
        builder.CreateICmp(llvm::CmpInst::ICMP_EQ, builder.CreateExtractElement(function->getArg(thread_id_idx), 0ull), builder.getInt32(0)), dispatch_mesh,
        real_return
    );

    builder.SetInsertPoint(dispatch_mesh);

    llvm::Value *meshgroup_to_dispatch = air.getInt3(1, 1, 1);
    auto *mesh_count = builder.CreateLoad(types._int, workload_count);
    llvm::Value *dispatch_count = split_workload ? builder.CreateMul(mesh_count, builder.getInt32(2)) : mesh_count;
    meshgroup_to_dispatch = builder.CreateInsertElement(meshgroup_to_dispatch, dispatch_count, (uint64_t)0);

    air.CreateSetMeshProperties(meshgroup_to_dispatch);

    builder.CreateStore(
        batched_patch_start,
        builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(2)})
    );

    builder.CreateBr(real_return);
    }   // GPU-PASS TESS_PREPASS: end of the mesh-dispatch branch
    builder.SetInsertPoint(real_return);
  }

  air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  builder.CreateRetVoid();

  canonicalize_tessellation_math(*function);
  module.getOrInsertNamedMetadata(prepass_kernel ? "air.kernel" : "air.object")->addOperand(function_metadata);
  if (split_workload) split_workload->applied = SM50_TESS_SPLIT_OBJECT_APPLIED;
  if (vertex_link_arg) vertex_link_arg->applied = SM50_TESS_VERTEX_LINKAGE_APPLIED;
  if (prepass) {
    const auto *layout = module.getDataLayout().getStructLayout(payload_struct_type);
    prepass->slot_stride = payload_struct_size;
    prepass->meta_offset = uint32_t(layout->getElementOffset(3));
    prepass->workloads_per_group = max_workload_count * patch_per_group;
    prepass->patches_per_group = patch_per_group;
    prepass->threads_per_group = threads_per_patch * patch_per_group;
    prepass->applied = SM50_TESS_PREPASS_APPLIED;
  }
  return llvm::Error::success();
}

// GPU-PASS TESS_PREPASS v2 MATERIALIZE_KERNEL: one threadgroup per group of the hull kernel, lane = patch. From the slot (the
// patch constants as the hull stored them and the 4-byte workload records in emission order) it regenerates every patch's
// workloads in program order into the table at their emission positions and writes their indices (flat << 8 | vertex).
static llvm::Error
convert_tess_prepass_materialize(
    SM50ShaderInternal *pShaderInternal, const char *name, SM50ShaderInternal *pHullStage, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, SM50_SHADER_TESS_PREPASS_DATA *prepass
) {
  using namespace microsoft;
  if (pHullStage->tessellation_domain == D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE ||
      (pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
       pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW))
    return llvm::make_error<UnsupportedFeature>("tess prepass: only triangle output of triangle / quad domains");
  air::AirType types(context);
  auto [final_maxtessfactor, factor_int] = get_final_maxtessfactor(pHullStage, pArgs);
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common))
    metal_version = sm50_common->metal_version;
  const uint32_t threads_per_patch = next_pow2(pHullStage->hull_maximum_threads_per_patch);
  const uint32_t patch_per_group = next_pow2(32 / threads_per_patch);
  const uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage, final_maxtessfactor);
  auto hs_output_per_point_type = llvm::ArrayType::get(types._int4, pHullStage->max_output_register);
  auto hs_output_per_patch_type = llvm::ArrayType::get(hs_output_per_point_type, pHullStage->output_control_point_count);
  auto hs_output_per_group_type = llvm::ArrayType::get(hs_output_per_patch_type, patch_per_group);
  auto hs_pcout_per_patch_scaler_type = llvm::ArrayType::get(types._int, pHullStage->patch_constant_scalars.size());
  auto hs_pcout_per_group_scaler_type = llvm::ArrayType::get(hs_pcout_per_patch_scaler_type, patch_per_group);
  auto slot_type = llvm::StructType::create(
      context,
      {hs_output_per_group_type, hs_pcout_per_group_scaler_type, types._int, llvm::ArrayType::get(types._int, max_workload_count * patch_per_group)},
      "payload"
  );
  const uint32_t slot_size = module.getDataLayout().getTypeAllocSize(slot_type);
  const uint32_t meta_offset = uint32_t(module.getDataLayout().getStructLayout(slot_type)->getElementOffset(3));

  air::FunctionSignatureBuilder signature;
  auto buffer = [](uint32_t index, const char *arg, air::MemoryAccess access) {
    return air::ArgumentBindingBuffer{
        .buffer_size = {}, .location_index = index, .array_size = 0, .memory_access = access,
        .address_space = air::AddressSpace::device, .type = air::msl_uint, .arg_name = arg, .raster_order_group = {}};
  };
  const uint32_t thread_idx = signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  const uint32_t tg_idx = signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  const uint32_t slots_idx = signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_SLOTS, "tess_slots", air::MemoryAccess::read));
  const uint32_t groups_idx = signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_GROUPS, "tess_groups", air::MemoryAccess::read));
  const uint32_t offsets_idx = signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_OFFSETS, "tess_offsets", air::MemoryAccess::read));
  const uint32_t table_idx = signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_TABLE, "tess_table", air::MemoryAccess::write));
  const uint32_t indices_idx = signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_INDICES, "tess_indices", air::MemoryAccess::write));
  signature.UseMaxWorkgroupSize(32);
  auto [function, function_metadata] = signature.CreateFunction(name, context, module, 0, true);
  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto active_bb = llvm::BasicBlock::Create(context, "active", function);
  auto return_bb = llvm::BasicBlock::Create(context, "return", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(builder, nulldbg);
  setup_metal_version(module, metal_version);
  auto *lane = builder.CreateExtractElement(function->getArg(thread_idx), uint64_t(0));
  auto *group = builder.CreateExtractElement(function->getArg(tg_idx), uint64_t(0));
  builder.CreateCondBr(builder.CreateICmpULT(lane, builder.getInt32(patch_per_group)), active_bb, return_bb);
  builder.SetInsertPoint(active_bb);
  auto *slot = builder.CreateBitCast(
      builder.CreateGEP(types._int, function->getArg(slots_idx), {builder.CreateMul(group, builder.getInt32(slot_size / 4))}),
      slot_type->getPointerTo(1)
  );
  auto load_at = [&](uint32_t arg, llvm::Value *index) {
    return builder.CreateLoad(types._int, builder.CreateGEP(types._int, function->getArg(arg), {index}));
  };
  auto *count = load_at(groups_idx, builder.CreateMul(group, builder.getInt32(4)));
  auto *flat_base = load_at(offsets_idx, builder.CreateMul(group, builder.getInt32(4)));
  auto *index_base = load_at(offsets_idx, builder.CreateAdd(builder.CreateMul(group, builder.getInt32(4)), builder.getInt32(2)));
  auto *meta = builder.CreateGEP(slot_type, slot, {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)});
  std::array<llvm::Value *, 6> f;
  f.fill(air.getFloat(0));
  for (unsigned i = 0; i < pHullStage->patch_constant_scalars.size(); i++) {
    auto pc = pHullStage->patch_constant_scalars[i];
    if (pc.tess_factor_index >= 0 && pc.tess_factor_index < 6)
      f[pc.tess_factor_index] = builder.CreateBitCast(
          builder.CreateLoad(types._int, builder.CreateGEP(slot_type, slot, {builder.getInt32(0), builder.getInt32(1), lane, builder.getInt32(i)})),
          types._float
      );
  }
  const bool quad = pHullStage->tessellation_domain == D3D11_SB_TESSELLATOR_DOMAIN_QUAD;
  const bool ccw = pHullStage->tessellator_output_primitive == D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW;
  std::vector<llvm::Value *> args{lane, count, meta, function->getArg(table_idx), flat_base, group, function->getArg(indices_idx),
                                  index_base, builder.getInt32(ccw ? 3 : 0)};
  if (quad) args.insert(args.end(), {f[4], f[5], f[0], f[1], f[2], f[3]});
  else args.insert(args.end(), {f[3], f[0], f[1], f[2]});
  const char *part = ".integer";
  switch (get_partitioning(pHullStage)) {
  case TessellatorPartitioning::integer: part = ".integer"; break;
  case TessellatorPartitioning::pow2: part = ".pow2"; break;
  case TessellatorPartitioning::fractional_odd: part = ".odd"; break;
  case TessellatorPartitioning::fractional_even: part = ".even"; break;
  }
  llvm::SmallVector<llvm::Type *> tys;
  for (auto *a : args) tys.push_back(a->getType());
  auto callee = module.getOrInsertFunction(std::string("dxmt.prepass.materialize.") + (quad ? "quad" : "triangle") + part,
                                           llvm::FunctionType::get(builder.getVoidTy(), tys, false));
  builder.CreateCall(callee, args);
  builder.CreateBr(return_bb);
  builder.SetInsertPoint(return_bb);
  builder.CreateRetVoid();
  module.getOrInsertNamedMetadata("air.kernel")->addOperand(function_metadata);
  prepass->slot_stride = slot_size;
  prepass->meta_offset = meta_offset;
  prepass->workloads_per_group = max_workload_count * patch_per_group;
  prepass->patches_per_group = patch_per_group;
  prepass->threads_per_group = 32;
  prepass->applied = SM50_TESS_PREPASS_APPLIED;
  (void)pShaderInternal;
  return llvm::Error::success();
}

// Each application stage needs the same evaluation graph across tessellation backends:
// VS+HS object versus compute, and DS mesh versus pull/fallback vertex.
// Opportunistic FP rewrites are stage dependent: tp19's resource-free fast CP interpolation
// differs by up to 26 ULP with contraction. Removing contract fixes that reduction, but does
// not make a general fast DS graph invariant when it is recompiled as a different Metal stage.
// A tessellation pre-pass needs the same arithmetic evaluation graph in mesh, pull and fallback.
// Keep explicit FMA/intrinsic operations, but do not authorize reassociation, reciprocal or
// approximate substitutions (or NaN/Inf/signed-zero assumptions) solely because a DS is "fast".
static void
canonicalize_tessellation_math(llvm::Function &function) {
  for (auto &block : function)
    for (auto &instruction : block)
      if (llvm::isa<llvm::FPMathOperator>(instruction))
        instruction.copyFastMathFlags(llvm::FastMathFlags{});
  for (const char *attribute : {"approx-func-fp-math", "no-infs-fp-math", "no-nans-fp-math",
                               "no-signed-zeros-fp-math", "unsafe-fp-math"})
    function.removeFnAttr(attribute);
}

llvm::Error
convert_dxbc_tesselator_domain_shader(
    SM50ShaderInternal *pShaderInternal, const char *name, SM50ShaderInternal *pHullStage, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  SM50_SHADER_GS_PASS_THROUGH_DATA *gs_passthrough = nullptr;
  bool rasterization_disabled =
      (args_get_data<SM50_SHADER_GS_PASS_THROUGH, SM50_SHADER_GS_PASS_THROUGH_DATA>(pArgs, &gs_passthrough) &&
       gs_passthrough->RasterizationDisabled);
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  auto [final_maxtessfactor, factor_int] = get_final_maxtessfactor(pHullStage, pArgs);

  // GPU-PASS TESS_PREPASS v2: PULL_VERTEX = the domain body in a vertex function whose workload comes with the index value;
  // FALLBACK_VERTEX = the same body for the non-indexed fallback draw (workload regenerated from the slot);
  // MATERIALIZE_KERNEL is built separately (it needs no domain body).
  SM50_SHADER_TESS_PREPASS_DATA *prepass = nullptr;
  args_get_data<SM50_SHADER_TESS_PREPASS, SM50_SHADER_TESS_PREPASS_DATA>(pArgs, &prepass);
  auto *record_node = prepass && prepass->diagnostic_records ? prepass : nullptr;
  const bool record_mesh = prepass && prepass->mode == SM50_TESS_PREPASS_DIAGNOSTIC_MESH;
  if (record_mesh) { record_node = prepass; prepass = nullptr; }
  if (prepass) {
    prepass->applied = 0;
    if (prepass->mode == SM50_TESS_PREPASS_MATERIALIZE_KERNEL)
      return convert_tess_prepass_materialize(pShaderInternal, name, pHullStage, context, module, pArgs, prepass);
    if (prepass->mode != SM50_TESS_PREPASS_PULL_VERTEX && prepass->mode != SM50_TESS_PREPASS_FALLBACK_VERTEX &&
        prepass->mode != SM50_TESS_PREPASS_LINEAR_PULL_VERTEX)
      return llvm::make_error<UnsupportedFeature>("tess prepass: wrong mode for the domain stage");
    if (pHullStage->tessellation_domain == D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE)
      return llvm::make_error<UnsupportedFeature>("tess prepass: isoline domain");
    if (pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
        pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW)
      return llvm::make_error<UnsupportedFeature>("tess prepass: only triangle output");
    if (gs_passthrough && (gs_passthrough->Data.RenderTargetArrayIndexReg != 255 || gs_passthrough->Data.ViewportArrayIndexReg != 255 ||
                           gs_passthrough->RasterizationDisabled))
      return llvm::make_error<UnsupportedFeature>("tess prepass: geometry pass-through outputs are not supported");
  }
  const bool prepass_linear = prepass && prepass->mode == SM50_TESS_PREPASS_LINEAR_PULL_VERTEX;
  const bool prepass_pull = prepass && (prepass->mode == SM50_TESS_PREPASS_PULL_VERTEX || prepass_linear);
  const bool prepass_fallback = prepass && prepass->mode == SM50_TESS_PREPASS_FALLBACK_VERTEX;
  const bool prepass_ccw = pHullStage->tessellator_output_primitive == D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW;

  auto *split_workload = prepass ? nullptr : get_split_workload(pArgs);
  if (split_workload &&
      ((pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
        pHullStage->tessellator_output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW) ||
       (gs_passthrough && (gs_passthrough->Data.RenderTargetArrayIndexReg != 255 ||
                           gs_passthrough->Data.ViewportArrayIndexReg != 255))))
    return llvm::make_error<UnsupportedFeature>("Split tessellation requires plain triangle output");
  Prologues prologue;
  Epilogues epilogue;

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
  }

  // TESS TODO: 17e30e0855effcaa4da9a05c018a3ba7189d7653 changes

  auto &ds_output_handlers = pShaderInternal->mesh_output_handlers;

  tess_prepass::RecordLayout prepass_layout;
  if (prepass) {
    auto layout = tess_prepass::MakeRecordLayout(func_signature, context, pShaderInternal->clip_distance_scalars.size());
    if (auto err = layout.takeError())
      return err;
    prepass_layout = *layout;
    func_signature.ClearMeshOutputs();
  }

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D11_SB_DOMAIN_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);

  uint32_t rta_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.RenderTargetArrayIndexReg != 255) {
    rta_idx_out = func_signature.DefineMeshPrimitiveOutput(air::OutputRenderTargetArrayIndex{});
  }
  uint32_t va_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.ViewportArrayIndexReg != 255) {
    va_idx_out = func_signature.DefineMeshPrimitiveOutput(air::OutputViewportArrayIndex{});
  }

  uint32_t thread_id_idx = prepass ? ~0u : func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  uint32_t tg_id_idx = prepass ? ~0u : func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  uint32_t prepass_vid_idx = ~0u, prepass_slots_idx = ~0u, prepass_groups_idx = ~0u, prepass_offsets_idx = ~0u,
           prepass_table_idx = ~0u, prepass_constants_idx = ~0u;
  uint32_t record_buffer_idx = ~0u;
  if (record_node) {
    record_buffer_idx = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {}, .location_index = 26, .array_size = 0, .memory_access = air::MemoryAccess::read_write,
        .address_space = air::AddressSpace::device, .type = air::msl_uint, .arg_name = "tess_record_compare", .raster_order_group = {}});
    record_node->diagnostic_stride = TessRecordScalars(pHullStage, pShaderInternal);
    record_node->diagnostic_vertices = 2 * (factor_int + 3 - ((factor_int + 1) & 1)) + 1;
    if (record_mesh) record_node->applied = SM50_TESS_PREPASS_APPLIED;
  }
  std::vector<uint32_t> prepass_out_indices;
  uint32_t prepass_indices_idx = ~0u;
  uint32_t prepass_linear_mode_idx = ~0u;
  if (prepass) {
    auto buffer = [](uint32_t index, const char *name, air::AddressSpace space) {
      return air::ArgumentBindingBuffer{
          .buffer_size = {}, .location_index = index, .array_size = 0, .memory_access = air::MemoryAccess::read,
          .address_space = space, .type = air::msl_uint, .arg_name = name, .raster_order_group = {}};
    };
    using air::AddressSpace;
    prepass_slots_idx = func_signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_SLOTS, "tess_slots", AddressSpace::device));
    if (prepass_pull) {
      prepass_table_idx = func_signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_TABLE, "tess_table", AddressSpace::device));
      if (prepass_linear) {
        prepass_indices_idx = func_signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_INDICES, "tess_indices", AddressSpace::device));
        prepass_linear_mode_idx = func_signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_CONSTANTS, "tess_linear_mode", AddressSpace::constant));
      }
    } else {
      prepass_groups_idx = func_signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_GROUPS, "tess_groups", AddressSpace::device));
      prepass_offsets_idx = func_signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_OFFSETS, "tess_offsets", AddressSpace::device));
      prepass_constants_idx = func_signature.DefineInput(buffer(SM50_TESS_PREPASS_BINDING_CONSTANTS, "tess_constants", AddressSpace::constant));
    }
    prepass_vid_idx = func_signature.DefineInput(air::InputVertexID{});
    prepass_out_indices = tess_prepass::DefineVertexOutputs(func_signature, prepass_layout);
  }

  uint32_t threads_per_patch = next_pow2(pHullStage->hull_maximum_threads_per_patch);
  uint32_t patch_per_group = next_pow2(32 / threads_per_patch);

  auto hs_output_per_point_type = llvm::ArrayType::get(types._int4, pHullStage->max_output_register);
  auto hs_output_per_patch_type =
      llvm::ArrayType::get(hs_output_per_point_type, pHullStage->output_control_point_count);
  auto hs_output_per_group_type = llvm::ArrayType::get(hs_output_per_patch_type, patch_per_group);
  auto hs_output_per_point_type_float = llvm::ArrayType::get(types._float4, pHullStage->max_output_register);
  auto hs_output_per_patch_type_float =
      llvm::ArrayType::get(hs_output_per_point_type_float, pHullStage->output_control_point_count);

  auto hs_pcout_per_patch_scaler_type = llvm::ArrayType::get(types._int, pHullStage->patch_constant_scalars.size());
  auto hs_pcout_per_group_scaler_type = llvm::ArrayType::get(hs_pcout_per_patch_scaler_type, patch_per_group);

  // n segments has n + 1 points
  uint32_t max_edge_point = factor_int + 1;
  uint32_t max_workload_count = get_max_potential_workload_count(factor_int, pHullStage, final_maxtessfactor);
  constexpr uint32_t size_workload_info = sizeof(TessMeshWorkload);
  auto payload_struct_type = llvm::StructType::create(
      context,
      {hs_output_per_group_type, hs_pcout_per_group_scaler_type, types._int,
       llvm::ArrayType::get(types._int, max_workload_count * patch_per_group * (prepass ? 1 : size_workload_info / 4))},
      "payload"
  );
  uint32_t payload_struct_size = module.getDataLayout().getTypeAllocSize(payload_struct_type);

  uint32_t payload_idx = prepass ? ~0u : func_signature.DefineInput(air::InputPayload{.size = payload_struct_size});

  bool point_rasterization = false;

  if (!prepass)   // GPU-PASS TESS_PREPASS: no mesh object in the kernel / vertex variants
  switch (pHullStage->tessellator_output_primitive) {
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_POINT: {
    func_signature.DefineInput(
        air::InputMesh{(uint32_t)max_edge_point * 2, (uint32_t)max_edge_point * 2, air::MeshOutputTopology::Point}
    );
    point_rasterization = true; // TESS TODO: not necessary... if combined with GS/SO
    break;
  }
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE: {
    func_signature.DefineInput(
        air::InputMesh{(uint32_t)max_edge_point, (uint32_t)max_edge_point - 1, air::MeshOutputTopology::Line}
    );
    break;
  }
  default: {
    const uint32_t mesh_vertices = split_workload ? 2 * max_edge_point :
        (max_edge_point + 2 - (max_edge_point & 1)) * 2 + 1;
    func_signature.DefineInput(air::InputMesh{
        mesh_vertices,
        split_workload ? 2 * factor_int : mesh_vertices,
        air::MeshOutputTopology::Triangle
    });
    break;
  }
  }

  if (point_rasterization) {
    func_signature.DefineMeshVertexOutput(air::OutputPointSize {});
  }

  if (pShaderInternal->clip_distance_scalars.size() > 0 && !prepass) {
    func_signature.DefineMeshVertexOutput(
        air::OutputClipDistance{.count = pShaderInternal->clip_distance_scalars.size()}
    );
  }

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, prepass ? false : rasterization_disabled);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto vertex_start = llvm::BasicBlock::Create(context, "vertex_start", function);
  auto vertex_emit = llvm::BasicBlock::Create(context, "vertex_emit", function);
  auto vertex_end = llvm::BasicBlock::Create(context, "vertex_end", function);
  auto generate_primitive_pre = llvm::BasicBlock::Create(context, "generate_primitive_pre", function);
  auto generate_primitive = llvm::BasicBlock::Create(context, "generate_primitive", function);
  auto real_return = llvm::BasicBlock::Create(context, "real_return", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  setup_metal_version(module, metal_version);
  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(shader_info, resource_map, types, module, builder);

  llvm::Value *workload_index = nullptr, *thread_index = nullptr, *payload = nullptr;
  llvm::Value *prepass_pull_vertex = nullptr, *prepass_pull_record = nullptr, *prepass_patch = nullptr, *prepass_location = nullptr,
              *prepass_entry = nullptr;
  llvm::Value *record_group = nullptr;
  if (!prepass) {
    workload_index = builder.CreateExtractElement(function->getArg(tg_id_idx), 0ull);
    thread_index = builder.CreateExtractElement(function->getArg(thread_id_idx), 0ull);
    payload = builder.CreateBitCast(function->getArg(payload_idx), payload_struct_type->getPointerTo(6));
  }

  struct context ctx {
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
  };
  dxbc::Converter dxbc(ctx.air, ctx, ctx.resource);

  if (prepass) {
    // GPU-PASS TESS_PREPASS v2: the slot of the workload's group stands where the payload was
    dxbc.TessPrepassDevice = true;
    const uint32_t meta_offset = uint32_t(module.getDataLayout().getStructLayout(payload_struct_type)->getElementOffset(3));
    llvm::Value *vid = function->getArg(prepass_vid_idx);
    if (prepass_linear) {
      // All submissions use this exact function. Modes1/3 expand an index stream;
      // mode2 is indexed and keeps its encoded VertexID. A safe element0 load in
      // mode2 avoids a speculative out-of-bounds address for the encoded ID.
      auto *mode = builder.CreateLoad(types._int, builder.CreateGEP(types._int, function->getArg(prepass_linear_mode_idx), {builder.getInt32(1)}));
      auto *expand = builder.CreateICmpNE(mode, builder.getInt32(2));
      auto *at = builder.CreateSelect(expand, vid, builder.getInt32(0));
      auto *encoded = builder.CreateLoad(types._int, builder.CreateGEP(types._int, function->getArg(prepass_indices_idx), {at}));
      vid = builder.CreateSelect(expand, encoded, vid);
    }
    llvm::Value *group = nullptr, *ordinal = nullptr, *corner = nullptr;
    workload_index = builder.getInt32(0);
    if (prepass_pull) {
      // Native identity carries one provoking/other-corner role bit. Strip it
      // after any linear/identity expansion, before table/point/record lookup.
      vid = builder.CreateLShr(vid, 1);
      // logical index = (flat workload << 8) | local vertex: read, never searched
      prepass_entry = builder.CreateGEP(types._int, function->getArg(prepass_table_idx), {builder.CreateMul(builder.CreateLShr(vid, 8), builder.getInt32(16))});
      prepass_pull_vertex = builder.CreateAnd(vid, builder.getInt32(255));
      group = builder.CreateLoad(types._int, builder.CreateGEP(types._int, prepass_entry, {builder.getInt32(14)}));
    } else {
      auto *groups = builder.CreateLoad(types._int, function->getArg(prepass_constants_idx));
      auto *decoded = dxbc.PrepassPullDecode(vid, function->getArg(prepass_offsets_idx), function->getArg(prepass_groups_idx),
                                             function->getArg(prepass_slots_idx), groups, payload_struct_size / 4, meta_offset / 4);
      group = builder.CreateExtractElement(decoded, uint64_t(0));
      prepass_patch = builder.CreateExtractElement(decoded, uint64_t(1));
      ordinal = builder.CreateExtractElement(decoded, uint64_t(2));
      corner = builder.CreateExtractElement(decoded, uint64_t(3));
    }
    auto *slot = builder.CreateGEP(
        types._int, function->getArg(prepass_slots_idx), {builder.CreateMul(group, builder.getInt32(payload_struct_size / 4))}
    );
    payload = builder.CreateBitCast(slot, payload_struct_type->getPointerTo(1));
    record_group = group;
    if (prepass_fallback) {
      // the patch's tessellation factors as the hull stored them (clamped), then the corner's domain point
      std::array<llvm::Value *, 6> f;
      f.fill(air.getFloat(0));
      for (unsigned i = 0; i < pHullStage->patch_constant_scalars.size(); i++) {
        auto pc = pHullStage->patch_constant_scalars[i];
        if (pc.tess_factor_index >= 0 && pc.tess_factor_index < 6)
          f[pc.tess_factor_index] = builder.CreateBitCast(
              builder.CreateLoad(types._int, builder.CreateGEP(payload_struct_type, payload,
                                                               {builder.getInt32(0), builder.getInt32(1), prepass_patch, builder.getInt32(i)})),
              types._float
          );
      }
      const bool quad = pHullStage->tessellation_domain == D3D11_SB_TESSELLATOR_DOMAIN_QUAD;
      std::vector<llvm::Value *> args{prepass_patch, ordinal, corner, builder.getInt32(prepass_ccw ? 3 : 0)};
      if (quad) args.insert(args.end(), {f[4], f[5], f[0], f[1], f[2], f[3]});
      else args.insert(args.end(), {f[3], f[0], f[1], f[2]});
      prepass_location = dxbc.PrepassPullPoint(quad, get_partitioning(pHullStage), args);
    }
    auto *record = builder.CreateAlloca(llvm::ArrayType::get(types._int, prepass_layout.stride_ints));
    auto *record_ints = builder.CreateBitCast(record, types._int->getPointerTo());
    for (uint32_t i = 0; i < prepass_layout.stride_ints; i++)   // unused lanes read 0
      builder.CreateStore(builder.getInt32(0), builder.CreateGEP(types._int, record_ints, {builder.getInt32(i)}));
    tess_prepass::InstallSinks(air, builder, prepass_layout, [record_ints](llvm::Value *) { return record_ints; });
    prepass_pull_record = record_ints;   // returned as the vertex outputs after the body
  }
  auto data = prepass_entry ? prepass_entry : builder.CreateGEP(
      payload_struct_type, payload,
      {builder.getInt32(0), builder.getInt32(3), builder.getInt32(0)}
  );

  auto batched_patch_start = builder.CreateLoad(
      types._int, builder.CreateGEP(payload_struct_type, payload, {builder.getInt32(0), builder.getInt32(2)})
  );

  auto patch_index = prepass_patch ? prepass_patch : dxbc.DomainGetPatchIndex(
      split_workload ? builder.CreateLShr(workload_index, 1) : workload_index, data);

  resource_map.patch_id = builder.CreateAdd(batched_patch_start, patch_index);

  resource_map.input.ptr_int4 = builder.CreateGEP(
      payload_struct_type, payload,
      {builder.getInt32(0), // struct
       builder.getInt32(0), // controlpoint
       patch_index}
  );
  resource_map.input.ptr_float4 =
      builder.CreateBitCast(resource_map.input.ptr_int4, hs_output_per_patch_type_float->getPointerTo(prepass ? 1 : 6));
  resource_map.input_element_count = pHullStage->max_output_register;

  resource_map.patch_constant_output.ptr_int4 =
      builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.patch_constant_output.ptr_float4 = builder.CreateBitCast(
      resource_map.patch_constant_output.ptr_int4,
      llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );

  resource_map.output.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register));
  resource_map.output.ptr_float4 = builder.CreateBitCast(
      resource_map.output.ptr_int4,
      llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo(
          cast<llvm::PointerType>(resource_map.output.ptr_int4->getType())->getPointerAddressSpace()
      )
  );
  resource_map.output_element_count = max_output_register;

  /* setup patch constant register */
  for (auto x : llvm::enumerate(pHullStage->patch_constant_scalars)) {
    if (x.value().reg >= max_input_register)
      continue;
    auto src_ptr = builder.CreateGEP(
        payload_struct_type, payload,
        {builder.getInt32(0), // struct
         builder.getInt32(1), // pcout
         patch_index, builder.getInt32(x.index())}
    );
    auto dst_ptr = builder.CreateGEP(
        llvm::ArrayType::get(types._int4, max_input_register), resource_map.patch_constant_output.ptr_int4,
        {builder.getInt32(0), builder.getInt32(x.value().reg), builder.getInt32(x.value().component)}
    );
    builder.CreateStore(builder.CreateLoad(types._int, src_ptr), dst_ptr);
  }

  if (prepass) {
    // GPU-PASS TESS_PREPASS: one vertex per invocation -- its domain point, the domain body, the record returned
    auto location = prepass_location ? prepass_location
                                     : dxbc.DomainGetPointOne(workload_index, prepass_pull_vertex, data, get_partitioning(pHullStage));
    llvm::Value *domain_bary = llvm::UndefValue::get(air.getFloatTy(3));
    llvm::Value *u = builder.CreateExtractElement(location, 0ull);
    llvm::Value *v = builder.CreateExtractElement(location, 1ull);
    llvm::Value *w = builder.CreateFSub(air.getFloat(1.0f), builder.CreateFAdd(u, v));
    domain_bary = builder.CreateInsertElement(domain_bary, u, 0ull);
    domain_bary = builder.CreateInsertElement(domain_bary, v, 1ull);
    domain_bary = builder.CreateInsertElement(domain_bary, w, 2ull);
    resource_map.domain = domain_bary;
    auto real_entry = pShaderInternal->dxil
                          ? dxil::convert_basicblocks(pShaderInternal->dxil_entry, ctx, vertex_emit)
                          : convert_basicblocks(pShaderInternal->entry(), ctx, vertex_emit);
    if (auto err = real_entry.takeError())
      return err;
    builder.CreateBr(real_entry.get());
    builder.SetInsertPoint(vertex_emit);
    if (record_node && prepass_pull)
      EmitTessRecords(builder, function, resource_map, types, pHullStage, pShaderInternal,
          function->getArg(record_buffer_idx), record_group,
          builder.CreateLoad(types._int, builder.CreateGEP(types._int, prepass_entry, {builder.getInt32(15)})),
          prepass_pull_vertex, false);
    auto *out_vertex = prepass_pull_vertex ? prepass_pull_vertex : builder.getInt32(0);
    MeshOutputContext out_ctx{out_vertex, out_vertex};
    IOHelper io(air, function, types, resource_map);
    for (auto &h : ds_output_handlers)
      h(out_ctx, io);
    for (auto x : llvm::enumerate(pShaderInternal->clip_distance_scalars)) {
      if (x.value().reg >= max_output_register)
        continue;
      auto src_ptr = builder.CreateGEP(
          llvm::ArrayType::get(types._float4, max_output_register), resource_map.output.ptr_float4,
          {builder.getInt32(0), builder.getInt32(x.value().reg), builder.getInt32(x.value().component)}
      );
      air.CreateSetMeshClipDistance(out_vertex, builder.getInt32(x.index()), builder.CreateLoad(types._float, src_ptr));
    }
    builder.CreateRet(tess_prepass::LoadVertexOutputs(builder, function, prepass_layout, prepass_out_indices, prepass_pull_record));
    for (auto *bb : {vertex_start, vertex_end, generate_primitive_pre, generate_primitive, real_return})
      bb->eraseFromParent();
    air.MeshPositionSink = nullptr; air.MeshVertexDataSink = nullptr; air.MeshClipDistanceSink = nullptr;
    module.getOrInsertNamedMetadata("air.vertex")->addOperand(function_metadata);
    prepass->slot_stride = payload_struct_size;
    prepass->meta_offset = uint32_t(module.getDataLayout().getStructLayout(payload_struct_type)->getElementOffset(3));
    prepass->workloads_per_group = get_max_potential_workload_count(factor_int, pHullStage, final_maxtessfactor) * patch_per_group;
    prepass->patches_per_group = patch_per_group;
    prepass->applied = SM50_TESS_PREPASS_APPLIED;
    canonicalize_tessellation_math(*function);
    return llvm::Error::success();
  }

  builder.CreateBr(vertex_start);

  builder.SetInsertPoint(vertex_start);

  auto phi_thread_index_base = builder.CreatePHI(types._int, 2);
  phi_thread_index_base->addIncoming(air.getInt(0), entry_bb);

  auto actual_thread_index = builder.CreateAdd(phi_thread_index_base, thread_index);

  auto [location, active, iterate] = dxbc.DomainGetLocation(
      workload_index, actual_thread_index, data, get_partitioning(pHullStage), split_workload != nullptr
  );

  // It accidentally works on quad as well
  {
    llvm::Value *domain_bary = llvm::UndefValue::get(air.getFloatTy(3));
    llvm::Value *u = builder.CreateExtractElement(location, 0ull);
    llvm::Value *v = builder.CreateExtractElement(location, 1ull);
    llvm::Value *w = builder.CreateFSub(air.getFloat(1.0f), builder.CreateFAdd(u, v));
    domain_bary = builder.CreateInsertElement(domain_bary, u, 0ull);
    domain_bary = builder.CreateInsertElement(domain_bary, v, 1ull);
    domain_bary = builder.CreateInsertElement(domain_bary, w, 2ull);
    resource_map.domain = domain_bary;
  }

  auto real_entry = pShaderInternal->dxil   // MacRunner dxil-tess 17.10: тело DS из DXIL
                        ? dxil::convert_basicblocks(pShaderInternal->dxil_entry, ctx, vertex_emit)
                        : convert_basicblocks(pShaderInternal->entry(), ctx, vertex_emit);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateCondBr(active, real_entry.get(), vertex_end);

  builder.SetInsertPoint(vertex_emit);

  auto vertex_id = actual_thread_index;
  if (record_node && record_mesh)
    EmitTessRecords(builder, function, resource_map, types, pHullStage, pShaderInternal,
        function->getArg(record_buffer_idx), builder.CreateUDiv(batched_patch_start, builder.getInt32(patch_per_group)),
        workload_index, vertex_id, true);
  auto primitive_id = actual_thread_index; // 
  MeshOutputContext gs_out_ctx{vertex_id, primitive_id};
  IOHelper io(air, function, types, resource_map);
  for (auto &h : ds_output_handlers) {
    h(gs_out_ctx, io);
  }

  for (auto x : llvm::enumerate(pShaderInternal->clip_distance_scalars)) {
    if (x.value().reg >= max_output_register)
      continue;
    auto src_ptr = builder.CreateGEP(
        llvm::ArrayType::get(types._float4, max_output_register), resource_map.output.ptr_float4,
        {builder.getInt32(0), builder.getInt32(x.value().reg), builder.getInt32(x.value().component)}
    );
    air.CreateSetMeshClipDistance(
        vertex_id, builder.getInt32(x.index()), builder.CreateLoad(types._float, src_ptr)
    );
  }

  if (rta_idx_out != ~0u) {
    auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(), resource_map.output.ptr_int4,
        {builder.getInt32(0), builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexComponent)}
    );
    air.CreateSetMeshRenderTargetArrayIndex(primitive_id, builder.CreateLoad(types._int, src_ptr));
  }
  if (va_idx_out != ~0u) {
    auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(), resource_map.output.ptr_int4,
        {builder.getInt32(0), builder.getInt32(gs_passthrough->Data.ViewportArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.ViewportArrayIndexComponent)}
    );
    air.CreateSetMeshViewportArrayIndex(primitive_id, builder.CreateLoad(types._int, src_ptr));
  }

  if (point_rasterization) {
    air.CreateSetMeshPointSize(vertex_id, air.getFloat(1.0));
  }

  builder.CreateBr(vertex_end);

  builder.SetInsertPoint(vertex_end);
  phi_thread_index_base->addIncoming(builder.CreateAdd(phi_thread_index_base, air.getInt(32)), vertex_end);
  builder.CreateCondBr(iterate, vertex_start, generate_primitive_pre);

  builder.SetInsertPoint(generate_primitive_pre);
  builder.CreateCondBr(
      builder.CreateICmp(llvm::CmpInst::ICMP_EQ, thread_index, builder.getInt32(0)), generate_primitive, real_return
  );
  builder.SetInsertPoint(generate_primitive);

  dxbc.DomainGeneratePrimitives(workload_index, data, get_output_primitive(pHullStage), split_workload != nullptr);

  builder.CreateBr(real_return);
  builder.SetInsertPoint(real_return);
  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.mesh")->addOperand(function_metadata);
  if (split_workload) split_workload->applied = SM50_TESS_SPLIT_DOMAIN_APPLIED;
  canonicalize_tessellation_math(*function);
  return llvm::Error::success();
}

} // namespace dxmt::dxbc
