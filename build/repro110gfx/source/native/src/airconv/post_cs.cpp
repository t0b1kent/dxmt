// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// MacRunner POST-CS 25.10.2026: the pixel shader of a one-target post-process pass as
//   - a compute kernel with the same body (SM50_POST_CS_KERNEL), and
//   - the fragment function of the pass's uv-map (SM50_POST_CS_UVMAP),
// plus the content-key query of the pass's vertex shader (SM50_SHADER_POST_CS_VS_KEY_QUERY). Contract: airconv_public.h.
//
// Why a uv-map: the interpolated inputs of a pixel are not reproducible by a formula in a kernel bit for bit (ENC-CENSUS
// 25.10, case 30); the rasterizer's own values, written once into an RGBA32Float map by a fragment function that declares
// the inputs exactly as the pixel shader does, and read by the kernel, are (case 31). The body of the kernel is the body of
// the fragment function: the same DXIL lowering (dxil::convert_basicblocks), the same module flags.

#include "dxbc_converter.hpp"
#include "dxil_converter.hpp"
#include "airconv_error.hpp"
#include "nt/air_builder.hpp"
#include "nt/dxbc_io_helper.hpp"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace dxmt::dxbc {

namespace {

using dxil::ComponentType;
using dxil::InterpolationMode;
using dxil::SemanticKind;

llvm::Error
refuse(const std::string &why) {
  return llvm::make_error<UnsupportedFeature>("post_cs: " + why);
}

uint32_t
declared_mask(const dxil::SignatureElement &e) {
  return ((1u << e.cols) - 1u) << e.start_col;
}

// The layout of the uv-map: one RGBA32Float map per interpolated input register the shader reads, ascending registers;
// coverage in the first component of map 0 that no input element of that register declares.
llvm::Error
map_layout(const SM50ShaderInternal &shader, SM50_SHADER_PS_AS_COMPUTE_DATA &out, uint32_t &position_reg) {
  out.map_count = 0;
  out.position_mask = 0;
  position_reg = ~0u;
  uint32_t masks[32] = {};
  for (const auto &e : shader.dxil_entry.inputs) {
    if (e.start_row < 0 || e.start_col < 0 || e.rows != 1 || e.start_row >= 32)
      return refuse("input layout of " + e.name);
    switch (e.semantic_kind) {
    case SemanticKind::Position: {
      // No usage tag (usage_mask 0) = not read (DXIL writes UsageCompMask only for read components).
      const uint32_t usage = e.usage_mask << e.start_col;
      if (usage & 0b1100u)
        return refuse("SV_Position.zw is read");
      out.position_mask = usage;
      position_reg = (uint32_t)e.start_row;
      break;
    }
    case SemanticKind::Arbitrary:
      if (e.usage_mask == 0)
        break;   // not read: not declared by the pixel shader either (dxil_converter.cpp, initialize_pixel_signature)
      if (e.component_type != ComponentType::F32)
        return refuse("input " + e.name + " is not float");
      switch (e.interpolation) {
      case InterpolationMode::Undefined:
      case InterpolationMode::Constant:
      case InterpolationMode::Linear:
      case InterpolationMode::LinearCentroid:
      case InterpolationMode::LinearNoperspective:
      case InterpolationMode::LinearNoperspectiveCentroid:
        break;
      default:
        return refuse("input " + e.name + " interpolates per sample");
      }
      masks[e.start_row] |= declared_mask(e);
      break;
    default:
      return refuse("system value input " + e.name);
    }
  }
  for (uint32_t reg = 0; reg < 32; ++reg) {
    if (!masks[reg])
      continue;
    if (reg == position_reg)
      return refuse("an interpolant shares the SV_Position register");
    if (out.map_count == SM50_POST_CS_MAX_MAPS)
      return refuse("more than 4 interpolated registers");
    out.map_reg[out.map_count] = reg;
    out.map_mask[out.map_count] = masks[reg];
    ++out.map_count;
  }
  if (!out.map_count)
    return refuse("no interpolated input (nothing to read coverage from)");
  out.cov_comp = 4;
  out.map_format = 4;
  if (!out.coverage) {   // no coverage channel: maps as narrow as their registers allow
    bool narrow = true;
    for (uint32_t i = 0; i < out.map_count; ++i) narrow &= !(out.map_mask[i] & ~0b0011u);
    out.map_format = narrow ? 2 : 4;
    return llvm::Error::success();
  }
  for (uint32_t c = 0; c < 4; ++c)
    if (!(out.map_mask[0] & (1u << c))) {
      out.cov_comp = c;
      break;
    }
  if (out.cov_comp == 4)
    return refuse("map 0 has no free component for coverage");
  return llvm::Error::success();
}

// dx.op names a kernel can execute exactly as the fragment function does. Everything else (implicit-LOD sampling,
// derivatives, discard, evaluation at offsets, coverage, UAV writes, atomics, wave and quad operations, ...) refuses.
bool
kernel_op_allowed(llvm::StringRef op) {
  static const char *const allowed[] = {
      "loadInput", "storeOutput", "createHandle", "createHandleFromBinding", "createHandleFromHeap", "annotateHandle",
      "cbufferLoadLegacy", "cbufferLoad", "sampleLevel", "sampleGrad", "sampleCmpLevelZero", "sampleCmpLevel",
      "textureLoad", "textureGather", "textureGatherCmp", "bufferLoad", "rawBufferLoad", "getDimensions",
      "checkAccessFullyMapped", "unary", "unaryBits", "binary", "binaryWithCarry", "binaryWithTwoOuts", "tertiary",
      "quaternary", "dot2", "dot3", "dot4", "isSpecialFloat", "fma", "imad", "umad", "msad", "ibfe", "ubfe", "bfi",
      "legacyF32ToF16", "legacyF16ToF32", "legacyDoubleToFloat", "legacyDoubleToSInt32", "legacyDoubleToUInt32",
      "makeDouble", "splitDouble", "countBits", "firstbitLo", "firstbitHi", "firstbitSHi", "dot2AddHalf",
      "dot4AddI8Packed", "dot4AddU8Packed", "pack4x8", "unpack4x8",
  };
  for (const char *name : allowed)
    if (op == name)
      return true;
  return false;
}

llvm::Error
check_kernel_body(const SM50ShaderInternal &shader) {
  const llvm::Function *f = shader.dxil_entry.function;
  if (!f)
    return refuse("no DXIL entry function");
  for (const auto &bb : *f)
    for (const auto &inst : bb) {
      const auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (!call)
        continue;
      const llvm::Function *callee = call->getCalledFunction();
      if (!callee)
        return refuse("indirect call");
      llvm::StringRef name = callee->getName();
      if (name.startswith("llvm.lifetime") || name.startswith("llvm.dbg"))
        continue;
      if (!name.startswith("dx.op."))
        return refuse("call to " + name.str());
      llvm::StringRef op = name.drop_front(6).split('.').first;
      if (!kernel_op_allowed(op))
        return refuse("operation " + op.str());
    }
  return llvm::Error::success();
}

uint32_t
target_channels(air::MTLPixelFormat format) {
  switch (format) {
  case air::MTLPixelFormat::R8Unorm:
  case air::MTLPixelFormat::R16Float:
  case air::MTLPixelFormat::R32Float:
    return 1;
  case air::MTLPixelFormat::RG8Unorm:
  case air::MTLPixelFormat::RG16Float:
    return 2;
  case air::MTLPixelFormat::RG11B10Float:
    return 3;
  case air::MTLPixelFormat::RGBA8Unorm:
  case air::MTLPixelFormat::BGRA8Unorm:
  case air::MTLPixelFormat::RGB10A2Unorm:
  case air::MTLPixelFormat::RGBA16Float:
  case air::MTLPixelFormat::RGBA32Float:
    return 4;
  default:
    return 0;
  }
}

struct Frame {
  llvm::Function *function;
  llvm::BasicBlock *entry;
  llvm::BasicBlock *epilogue;
};

void
allocate_registers(
    SM50ShaderInternal *shader, io_binding_map &map, air::AirType &types, llvm::IRBuilder<> &builder, llvm::Module &module
) {
  const uint32_t inputs = std::max(1u, shader->max_input_register), outputs = std::max(1u, shader->max_output_register);
  map.input.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, inputs));
  map.input.ptr_float4 =
      builder.CreateBitCast(map.input.ptr_int4, llvm::ArrayType::get(types._float4, inputs)->getPointerTo());
  map.input_element_count = inputs;
  map.output.ptr_int4 = builder.CreateAlloca(llvm::ArrayType::get(types._int4, outputs));
  map.output.ptr_float4 =
      builder.CreateBitCast(map.output.ptr_int4, llvm::ArrayType::get(types._float4, outputs)->getPointerTo());
  map.output_element_count = outputs;
  setup_temp_register(&shader->shader_info, map, types, module, builder);
  setup_immediate_constant_buffer(&shader->shader_info, map, types, module, builder);
}

void
read_common(
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, SM50_SHADER_METAL_VERSION &metal_version, SM50_SHADER_FLAG &flags,
    SM50_SHADER_ROOT_SIGNATURE_DATA *&rootsig
) {
  metal_version = SM50_SHADER_METAL_310;
  flags = {};
  SM50_SHADER_COMMON_DATA *common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &common)) {
    metal_version = common->metal_version;
    flags = common->flags;
  }
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
}

// ---- SM50_POST_CS_UVMAP ---------------------------------------------------------------------------------------------

llvm::Error
convert_uvmap(
    SM50ShaderInternal *shader, const char *name, llvm::LLVMContext &context, llvm::Module &module,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const SM50_SHADER_PS_AS_COMPUTE_DATA &layout
) {
  SM50_SHADER_METAL_VERSION metal_version;
  SM50_SHADER_FLAG flags;
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  read_common(pArgs, metal_version, flags, rootsig);

  auto func_signature = shader->func_signature;   // copy, as convert_dxbc_pixel_shader does
  Prologues prologue;
  Epilogues epilogue;
  io_binding_map resource_map;
  air::AirType types(context);
  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.skip_pixel_output = true;
    sig_ctx.pixel_input_linkage = PixelInputLinkage(nullptr);
    for (const auto &e : shader->dxil_entry.inputs) {
      if (e.semantic_kind != SemanticKind::Arbitrary || e.start_row < 0 || e.start_col < 0)
        continue;
      uint32_t usage = e.usage_mask << e.start_col;
      sig_ctx.pixel_input_linkage.signature((uint32_t)e.start_row, declared_mask(e), usage);
    }
    for (auto &handler : shader->signature_handlers)
      handler(sig_ctx);
    if (!sig_ctx.pixel_input_linkage.complete())
      return refuse("uv-map input linkage");
  }
  uint32_t outputs[SM50_POST_CS_MAX_MAPS];
  for (uint32_t i = 0; i < layout.map_count; ++i)
    outputs[i] = func_signature.DefineOutput(air::OutputRenderTarget{.dual_source_blending = false, .index = i, .type = air::msl_float4});

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, false);
  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {.sampleNaNToZero = bool(flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO), .defuseFma = bool(flags & SM50_SHADER_FLAG_DEFUSE_FMA)},
      builder, nulldbg
  );
  setup_metal_version(module, metal_version);
  setup_module_meta(module, function, shader);
  allocate_registers(shader, resource_map, types, builder, module);
  IOHelper io(air, function, types, resource_map);
  if (auto err = prologue.Run(io))
    return err;
  builder.CreateBr(epilogue_bb);
  builder.SetInsertPoint(epilogue_bb);
  llvm::Value *ret = llvm::UndefValue::get(function->getReturnType());
  auto *zero = llvm::ConstantFP::get(types._float, 0.0), *one = llvm::ConstantFP::get(types._float, 1.0);
  for (uint32_t i = 0; i < layout.map_count; ++i) {
    auto *array_ty = llvm::ArrayType::get(types._float4, resource_map.input_element_count);
    auto *ptr = builder.CreateGEP(array_ty, resource_map.input.ptr_float4, {builder.getInt32(0), builder.getInt32(layout.map_reg[i])});
    llvm::Value *v = builder.CreateLoad(types._float4, ptr);
    for (uint32_t c = 0; c < 4; ++c) {
      if (layout.map_mask[i] & (1u << c))
        continue;
      v = builder.CreateInsertElement(v, i == 0 && c == layout.cov_comp ? one : zero, uint64_t(c));
    }
    ret = builder.CreateInsertValue(ret, v, {outputs[i]});
  }
  builder.CreateRet(ret);
  module.getOrInsertNamedMetadata("air.fragment")->addOperand(function_metadata);
  return llvm::Error::success();
}

// ---- SM50_POST_CS_KERNEL --------------------------------------------------------------------------------------------

llvm::Error
convert_kernel(
    SM50ShaderInternal *shader, const char *name, llvm::LLVMContext &context, llvm::Module &module,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const SM50_SHADER_PS_AS_COMPUTE_DATA &layout, uint32_t position_reg
) {
  using namespace microsoft;
  SM50_SHADER_METAL_VERSION metal_version;
  SM50_SHADER_FLAG flags;
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  read_common(pArgs, metal_version, flags, rootsig);
  if (!rootsig)
    return refuse("no root signature");
  if (auto err = check_kernel_body(*shader))
    return err;

  // The one colour output: SV_Target0, float, covering every channel of the target.
  const auto format = air::MTLPixelFormat(layout.target_format);
  const uint32_t channels = target_channels(format);
  if (!channels || component_type_from_pixel_format(format) != RegisterComponentType::Float)
    return refuse("target format " + std::to_string(layout.target_format));
  uint32_t target_reg = ~0u, target_mask = 0;
  for (const auto &e : shader->dxil_entry.outputs) {
    if (e.semantic_kind != SemanticKind::Target || e.rows != 1 || e.start_row != 0 || e.start_col != 0 ||
        e.component_type != ComponentType::F32 || target_reg != ~0u)
      return refuse("output " + e.name);
    target_reg = 0;
    target_mask = declared_mask(e);
  }
  if (target_reg == ~0u)
    return refuse("no SV_Target0");
  if ((target_mask & ((1u << channels) - 1u)) != ((1u << channels) - 1u))
    return refuse("SV_Target0 leaves a channel of the target unwritten");

  air::FunctionSignatureBuilder func_signature;   // a kernel: none of the fragment inputs of the pixel shader
  auto binding_map = setup_binding_rootsig(
      &shader->shader_info, func_signature, module, D3D10_SB_PIXEL_SHADER, rootsig->bytecode, rootsig->bytecode_length
  );
  const uint32_t arg_tid = func_signature.DefineInput(air::InputThreadPositionInGrid{});
  const uint32_t arg_target = func_signature.DefineInput(air::ArgumentBindingTexture{
      .location_index = SM50_POST_CS_TARGET_SLOT,
      .array_size = 1,
      .memory_access = air::MemoryAccess::write,
      .type = air::MSLTexture{
          .component_type = air::msl_float,
          .memory_access = air::MemoryAccess::write,
          .resource_kind = air::TextureKind::texture_2d,
          .resource_kind_logical = air::TextureKind::texture_2d,
      },
      .arg_name = "pcs_target",
      .raster_order_group = {},
  });
  uint32_t arg_map[SM50_POST_CS_MAX_MAPS];
  for (uint32_t i = 0; i < layout.map_count; ++i)
    arg_map[i] = func_signature.DefineInput(air::ArgumentBindingTexture{
        .location_index = SM50_POST_CS_MAP_SLOT + i,
        .array_size = 1,
        .memory_access = air::MemoryAccess::read,
        .type = air::MSLTexture{
            .component_type = air::msl_float,
            .memory_access = air::MemoryAccess::read,
            .resource_kind = air::TextureKind::texture_2d,
            .resource_kind_logical = air::TextureKind::texture_2d,
        },
        .arg_name = "pcs_map" + std::to_string(i),
        .raster_order_group = {},
    });
  const uint32_t arg_params = func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = uint32_t(sizeof(SM50_POST_CS_PARAMS)),
      .location_index = SM50_POST_CS_PARAMS_SLOT,
      .array_size = 1,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = air::msl_uint,
      .arg_name = "pcs_params",
      .raster_order_group = {},
  });
  const uint32_t arg_witness = func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = SM50_POST_CS_WITNESS_SLOT,
      .array_size = 1,
      .memory_access = air::MemoryAccess::read_write,
      .address_space = air::AddressSpace::device,
      .type = air::msl_uint,
      .arg_name = "pcs_witness",
      .raster_order_group = {},
  });
  uint32_t arg_source[SM50_POST_CS_MAX_SOURCES];
  for (uint32_t r = 0; r < SM50_POST_CS_MAX_SOURCES; ++r)
    arg_source[r] = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {},
        .location_index = SM50_POST_CS_SOURCE_SLOT + r,
        .array_size = 1,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::device,
        .type = air::msl_uint,
        .arg_name = "pcs_source" + std::to_string(r),
        .raster_order_group = {},
    });

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, false);
  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {.sampleNaNToZero = bool(flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO), .defuseFma = bool(flags & SM50_SHADER_FLAG_DEFUSE_FMA)},
      builder, nulldbg
  );
  setup_metal_version(module, metal_version);
  setup_module_meta(module, function, shader);
  io_binding_map resource_map;
  air::AirType types(context);
  allocate_registers(shader, resource_map, types, builder, module);

  auto *i32 = builder.getInt32Ty();
  auto *slot_offset = builder.CreateAlloca(i32);   // witness loop state: entry-block allocas (promoted by mem2reg)
  auto *slot_index = builder.CreateAlloca(i32);
  auto *slot_bad = builder.CreateAlloca(i32);
  auto *params = function->getArg(arg_params);
  const auto param = [&](llvm::Value *index) {
    return builder.CreateLoad(i32, builder.CreateGEP(i32, params, index));
  };
  auto *tid = function->getArg(arg_tid);
  auto *tx = builder.CreateExtractElement(tid, uint64_t(0)), *ty = builder.CreateExtractElement(tid, uint64_t(1));

  // Witness (thread 0, 0): the words the vertex stage reads at GPU time against the words the layer keyed the map with.
  {
    auto *witness_bb = llvm::BasicBlock::Create(context, "pcs.witness", function);
    auto *after_bb = llvm::BasicBlock::Create(context, "pcs.after_witness", function);
    auto *count = param(builder.getInt32(2));
    auto *first = builder.CreateICmpEQ(builder.CreateOr(tx, ty), builder.getInt32(0));
    builder.CreateCondBr(builder.CreateAnd(first, builder.CreateICmpNE(count, builder.getInt32(0))), witness_bb, after_bb);
    builder.SetInsertPoint(witness_bb);
    builder.CreateStore(builder.getInt32(0), slot_offset);
    builder.CreateStore(builder.getInt32(0), slot_bad);
    for (uint32_t r = 0; r < SM50_POST_CS_MAX_SOURCES; ++r) {
      auto *loop_bb = llvm::BasicBlock::Create(context, "pcs.witness.loop", function);
      auto *body_bb = llvm::BasicBlock::Create(context, "pcs.witness.body", function);
      auto *next_bb = llvm::BasicBlock::Create(context, "pcs.witness.next", function);
      auto *skip_bb = llvm::BasicBlock::Create(context, "pcs.witness.skip", function);
      // source r present?
      auto *present = builder.CreateICmpULT(builder.getInt32(r), count);
      auto *enter_bb = llvm::BasicBlock::Create(context, "pcs.witness.enter", function);
      builder.CreateCondBr(present, enter_bb, skip_bb);
      builder.SetInsertPoint(enter_bb);
      auto *words = param(builder.getInt32(4 + r));
      builder.CreateStore(builder.getInt32(0), slot_index);
      builder.CreateBr(loop_bb);
      builder.SetInsertPoint(loop_bb);
      auto *i = builder.CreateLoad(i32, slot_index);
      builder.CreateCondBr(builder.CreateICmpULT(i, words), body_bb, next_bb);
      builder.SetInsertPoint(body_bb);
      auto *seen = builder.CreateLoad(i32, builder.CreateGEP(i32, function->getArg(arg_source[r]), i));
      auto *at = builder.CreateAdd(builder.CreateLoad(i32, slot_offset), i);
      auto *expect = param(builder.CreateAdd(builder.getInt32(8), at));
      auto *differs = builder.CreateZExt(builder.CreateICmpNE(seen, expect), i32);
      builder.CreateStore(builder.CreateOr(builder.CreateLoad(i32, slot_bad), differs), slot_bad);
      builder.CreateStore(builder.CreateAdd(i, builder.getInt32(1)), slot_index);
      builder.CreateBr(loop_bb);
      builder.SetInsertPoint(next_bb);
      builder.CreateStore(builder.CreateAdd(builder.CreateLoad(i32, slot_offset), words), slot_offset);
      builder.CreateBr(skip_bb);
      builder.SetInsertPoint(skip_bb);
    }
    auto *report_bb = llvm::BasicBlock::Create(context, "pcs.witness.report", function);
    builder.CreateCondBr(builder.CreateICmpNE(builder.CreateLoad(i32, slot_bad), builder.getInt32(0)), report_bb, after_bb);
    builder.SetInsertPoint(report_bb);
    auto *counter = function->getArg(arg_witness);
    air.CreateAtomicRMW(llvm::AtomicRMWInst::Add, counter, builder.getInt32(1));
    builder.CreateStore(param(builder.getInt32(3)), builder.CreateGEP(i32, counter, builder.getInt32(1)));
    builder.CreateBr(after_bb);
    builder.SetInsertPoint(after_bb);
  }

  // Pixel of this thread; coverage from map 0; inputs into the input registers.
  auto *px = builder.CreateAdd(tx, param(builder.getInt32(0)));
  auto *py = builder.CreateAdd(ty, param(builder.getInt32(1)));
  llvm::Value *pos = llvm::UndefValue::get(llvm::FixedVectorType::get(i32, 2));
  pos = builder.CreateInsertElement(pos, px, uint64_t(0));
  pos = builder.CreateInsertElement(pos, py, uint64_t(1));
  const llvm::air::Texture map_texture{llvm::air::Texture::texture2d, llvm::air::Texture::sample_float, llvm::air::Texture::access_read};
  const llvm::air::Texture target_texture{llvm::air::Texture::texture2d, llvm::air::Texture::sample_float, llvm::air::Texture::access_write};
  auto *float4_array = llvm::ArrayType::get(types._float4, resource_map.input_element_count);
  auto *covered_bb = layout.cov_comp < 4 ? llvm::BasicBlock::Create(context, "pcs.covered", function) : nullptr;
  auto *skip_bb = layout.cov_comp < 4 ? llvm::BasicBlock::Create(context, "pcs.uncovered", function) : nullptr;
  for (uint32_t i = 0; i < layout.map_count; ++i) {
    auto [value, residency] =
        air.CreateRead(map_texture, function->getArg(arg_map[i]), pos, nullptr, nullptr, builder.getInt32(0));
    (void)residency;
    auto *reg = builder.CreateGEP(float4_array, resource_map.input.ptr_float4, {builder.getInt32(0), builder.getInt32(layout.map_reg[i])});
    builder.CreateStore(value, reg);
    if (i == 0 && layout.cov_comp < 4) {
      auto *coverage = builder.CreateExtractElement(value, uint64_t(layout.cov_comp));
      builder.CreateCondBr(builder.CreateFCmpOEQ(coverage, llvm::ConstantFP::get(types._float, 1.0)), covered_bb, skip_bb);
      builder.SetInsertPoint(covered_bb);
    }
  }
  if (layout.position_mask) {
    auto *fx = builder.CreateFAdd(builder.CreateUIToFP(px, types._float), llvm::ConstantFP::get(types._float, 0.5));
    auto *fy = builder.CreateFAdd(builder.CreateUIToFP(py, types._float), llvm::ConstantFP::get(types._float, 0.5));
    llvm::Value *p4 = llvm::ConstantVector::get(
        {llvm::ConstantFP::get(types._float, 0.0), llvm::ConstantFP::get(types._float, 0.0),
         llvm::ConstantFP::get(types._float, 0.0), llvm::ConstantFP::get(types._float, 1.0)}
    );
    p4 = builder.CreateInsertElement(p4, fx, uint64_t(0));
    p4 = builder.CreateInsertElement(p4, fy, uint64_t(1));
    builder.CreateStore(p4, builder.CreateGEP(float4_array, resource_map.input.ptr_float4, {builder.getInt32(0), builder.getInt32(position_reg)}));
  }

  struct context ctx{
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = D3D10_SB_PIXEL_SHADER,
      .metal_version = metal_version,
  };
  llvm::Expected<llvm::BasicBlock *> real_entry = dxil::convert_basicblocks(shader->dxil_entry, ctx, epilogue_bb);
  if (auto err = real_entry.takeError())
    return err;
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);
  auto *out_array = llvm::ArrayType::get(types._float4, resource_map.output_element_count);
  auto *color = builder.CreateLoad(types._float4, builder.CreateGEP(out_array, resource_map.output.ptr_float4, {builder.getInt32(0), builder.getInt32(target_reg)}));
  air.CreateWrite(target_texture, function->getArg(arg_target), pos, nullptr, nullptr, builder.getInt32(0), color);
  builder.CreateRetVoid();
  if (skip_bb) {
    builder.SetInsertPoint(skip_bb);
    builder.CreateRetVoid();
  }

  module.getOrInsertNamedMetadata("air.kernel")->addOperand(function_metadata);
  return llvm::Error::success();
}

} // namespace

llvm::Error
convert_post_cs(
    SM50ShaderInternal *shader, const char *name, llvm::LLVMContext &context, llvm::Module &module,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, SM50_SHADER_PS_AS_COMPUTE_DATA *post_cs
) {
  post_cs->applied = 0;
  if (!shader->dxil)
    return refuse("not a DXIL shader");
  uint32_t position_reg = ~0u;
  if (auto err = map_layout(*shader, *post_cs, position_reg))
    return err;
  llvm::Error err = llvm::Error::success();
  if (post_cs->mode == SM50_POST_CS_UVMAP)
    err = convert_uvmap(shader, name, context, module, pArgs, *post_cs);
  else if (post_cs->mode == SM50_POST_CS_KERNEL)
    err = convert_kernel(shader, name, context, module, pArgs, *post_cs, position_reg);
  else
    return refuse("unknown mode " + std::to_string(post_cs->mode));
  if (err)
    return err;
  post_cs->applied = SM50_POST_CS_APPLIED;
  return llvm::Error::success();
}

void
post_cs_vertex_key(const SM50ShaderInternal &shader, SM50_SHADER_POST_CS_VS_KEY_DATA &key) {
  key.applied = SM50_POST_CS_APPLIED;
  key.ok = 0;
  key.row_count = 0;
  key.reason[0] = 0;
  const auto fail = [&](const std::string &why) {
    std::snprintf(key.reason, sizeof(key.reason), "%s", why.c_str());
    key.ok = 0;
  };
  if (!shader.dxil || !shader.dxil_entry.function)
    return fail("not a DXIL shader");
  for (const auto &e : shader.dxil_entry.inputs)
    if (e.semantic_kind != SemanticKind::Arbitrary)
      return fail("system value input " + e.name);
  // createHandle(class, range id, index, non-uniform): which constant buffer a handle names (range id -> metadata).
  const auto resource_of = [&](uint32_t range_id, uint32_t resource_class) -> const dxil::Resource * {
    for (const auto &r : shader.dxil_entry.resources)
      if (r.id == range_id && uint32_t(r.resource_class) == resource_class)
        return &r;
    return nullptr;
  };
  const auto constant = [](const llvm::Value *v, uint32_t &out) {
    if (const auto *c = llvm::dyn_cast<llvm::ConstantInt>(v)) {
      out = uint32_t(c->getZExtValue());
      return true;
    }
    return false;
  };
  for (const auto &bb : *shader.dxil_entry.function)
    for (const auto &inst : bb) {
      const auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (!call)
        continue;
      const llvm::Function *callee = call->getCalledFunction();
      if (!callee)
        return fail("indirect call");
      llvm::StringRef name = callee->getName();
      if (name.startswith("llvm.lifetime") || name.startswith("llvm.dbg"))
        continue;
      if (!name.startswith("dx.op."))
        return fail("call to " + name.str());
      llvm::StringRef op = name.drop_front(6).split('.').first;
      if (op == "loadInput" || op == "storeOutput" || op == "unary" || op == "binary" || op == "tertiary" ||
          op == "dot2" || op == "dot3" || op == "dot4" || op == "fma" || op == "isSpecialFloat" || op == "imad" ||
          op == "umad" || op == "unaryBits" || op == "legacyF32ToF16" || op == "legacyF16ToF32")
        continue;
      if (op == "createHandle") {
        uint32_t resource_class = 0, range_id = 0, index = 0;
        if (call->arg_size() < 5 || !constant(call->getArgOperand(1), resource_class) ||
            !constant(call->getArgOperand(2), range_id) || !constant(call->getArgOperand(3), index))
          return fail("dynamic createHandle");
        if (resource_class != uint32_t(dxil::ResourceClass::CBV))
          return fail("reads a resource other than a constant buffer");
        if (!resource_of(range_id, resource_class))
          return fail("unknown constant buffer range");
        continue;
      }
      if (op == "cbufferLoadLegacy") {
        const auto *handle = llvm::dyn_cast<llvm::CallInst>(call->getArgOperand(1));
        uint32_t row = 0, resource_class = 0, range_id = 0, index = 0;
        if (!handle || !handle->getCalledFunction() || !handle->getCalledFunction()->getName().startswith("dx.op.createHandle") ||
            handle->arg_size() < 5 || !constant(handle->getArgOperand(1), resource_class) ||
            !constant(handle->getArgOperand(2), range_id) || !constant(handle->getArgOperand(3), index) ||
            !constant(call->getArgOperand(2), row))
          return fail("constant buffer row is not constant");
        const auto *resource = resource_of(range_id, resource_class);
        if (!resource)
          return fail("unknown constant buffer range");
        const uint32_t reg = index;   // createHandle index = register (range lower bound included, DXIL rules)
        bool seen = false;
        for (uint32_t k = 0; k < key.row_count; ++k)
          seen |= key.rows[k].space == resource->space && key.rows[k].reg == reg && key.rows[k].row == row;
        if (!seen) {
          if (key.row_count == SM50_POST_CS_VS_MAX_ROWS)
            return fail("too many constant rows");
          key.rows[key.row_count++] = {resource->space, reg, row};
        }
        continue;
      }
      return fail("operation " + op.str());
    }
  // Where SV_Position comes from (the caller may then prove full coverage of a pass on the CPU).
  for (uint32_t c = 0; c < 4; ++c) key.pos_kind[c] = SM50_POST_CS_POS_OTHER;
  uint32_t pos_id = ~0u;
  for (uint32_t i = 0; i < shader.dxil_entry.outputs.size(); ++i)
    if (shader.dxil_entry.outputs[i].semantic_kind == SemanticKind::Position) pos_id = i;
  int32_t pos_element = -1;
  uint32_t pos_stores[4] = {};
  const auto row_index = [&](uint32_t space, uint32_t reg, uint32_t row) -> int32_t {
    for (uint32_t k = 0; k < key.row_count; ++k)
      if (key.rows[k].space == space && key.rows[k].reg == reg && key.rows[k].row == row) return int32_t(k);
    return -1;
  };
  for (const auto &bb : *shader.dxil_entry.function)
    for (const auto &inst : bb) {
      const auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
      if (!call || !call->getCalledFunction() || !call->getCalledFunction()->getName().startswith("dx.op.storeOutput")) continue;
      uint32_t id = 0, row = 0, comp = 0;
      if (!constant(call->getArgOperand(1), id) || id != pos_id || !constant(call->getArgOperand(2), row) || row ||
          !constant(call->getArgOperand(3), comp) || comp > 3)
        continue;
      const llvm::Value *value = call->getArgOperand(4);
      key.pos_kind[comp] = SM50_POST_CS_POS_OTHER;
      if (++pos_stores[comp] > 1) continue;   // stored on several paths: not a single known source
      if (const auto *fp = llvm::dyn_cast<llvm::ConstantFP>(value)) {
        key.pos_kind[comp] = SM50_POST_CS_POS_LITERAL;
        key.pos_value[comp] = fp->getValueAPF().convertToFloat();
      } else if (const auto *load = llvm::dyn_cast<llvm::CallInst>(value)) {
        uint32_t element = 0, erow = 0, ecomp = 0;
        if (load->getCalledFunction() && load->getCalledFunction()->getName().startswith("dx.op.loadInput") &&
            constant(load->getArgOperand(1), element) && constant(load->getArgOperand(2), erow) && !erow &&
            constant(load->getArgOperand(3), ecomp) && element < shader.dxil_entry.inputs.size() &&
            (pos_element < 0 || pos_element == int32_t(element))) {
          pos_element = int32_t(element);
          key.pos_kind[comp] = SM50_POST_CS_POS_INPUT;
          key.pos_b[comp] = ecomp;
        }
      } else if (const auto *ev = llvm::dyn_cast<llvm::ExtractValueInst>(value)) {
        const auto *cb = llvm::dyn_cast<llvm::CallInst>(ev->getAggregateOperand());
        uint32_t cbrow = 0, resource_class = 0, range_id = 0, index = 0;
        if (cb && cb->getCalledFunction() && cb->getCalledFunction()->getName().startswith("dx.op.cbufferLoadLegacy") &&
            ev->getNumIndices() == 1 && constant(cb->getArgOperand(2), cbrow)) {
          const auto *handle = llvm::dyn_cast<llvm::CallInst>(cb->getArgOperand(1));
          if (handle && handle->arg_size() >= 5 && constant(handle->getArgOperand(1), resource_class) &&
              constant(handle->getArgOperand(2), range_id) && constant(handle->getArgOperand(3), index))
            if (const auto *resource = resource_of(range_id, resource_class)) {
              const int32_t k = row_index(resource->space, index, cbrow);
              if (k >= 0) {
                key.pos_kind[comp] = SM50_POST_CS_POS_CB;
                key.pos_a[comp] = uint32_t(k);
                key.pos_b[comp] = ev->getIndices()[0];
              }
            }
        }
      }
    }
  if (pos_element >= 0) {
    const auto &e = shader.dxil_entry.inputs[uint32_t(pos_element)];
    std::snprintf(key.pos_semantic, sizeof(key.pos_semantic), "%s", e.name.c_str());
    key.pos_semantic_index = e.semantic_index;
  }
  key.ok = 1;
}

} // namespace dxmt::dxbc
