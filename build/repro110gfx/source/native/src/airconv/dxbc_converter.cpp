// Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "dxbc_converter.hpp"
#include "dxbc_reuse_audit.hpp"
#include <cstdlib>
#include <cstring>
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/ShaderBinary.h"
#include "airconv_error.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/FMF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dxil_converter.hpp"
#include "metallib_writer.hpp"
#include "nt/air_builder.hpp"
#include "shader_common.hpp"

#include "airconv_context.hpp"

#include "ftl.hpp"

namespace {

bool macrunner_fragment_translation_probe_enabled() {
  static const bool enabled = [] {
    const char *value =
        std::getenv("MACRUNNER_HB_FRAGMENT_SHADER_TRANSLATION_PROBE");
    return value && value[0] && value[0] != '0';
  }();
  return enabled;
}

bool macrunner_causal_ladder_enabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("MACRUNNER_HB_CAUSAL_CONTROL");
    return value &&
           ((std::strlen(value) == 2 && value[0] == 'C' &&
             value[1] >= '0' && value[1] <= '3') ||
            std::strcmp(value, "C0,C2,C3") == 0);
  }();
  return enabled;
}

bool macrunner_causal_ladder_capture_claim(
    microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type,
    const char *function_name) {
  if (!macrunner_causal_ladder_enabled() || !function_name)
    return false;
  const bool pixel_capture =
      shader_type == microsoft::D3D10_SB_PIXEL_SHADER &&
      (std::strcmp(function_name,
                   "ps_bef43b20_f8e9cf38dc7294cc7203c7fb6cd68b27ed713b6a"
                   "_mr_c0_fragment_sidechannel") == 0 ||
       std::strcmp(function_name,
                   "ps_bef43b20_f8e9cf38dc7294cc7203c7fb6cd68b27ed713b6a"
                   "_mr_c1_fragment_final_magenta_sidechannel") == 0 ||
       std::strcmp(function_name,
                   "ps_bef43b20_f8e9cf38dc7294cc7203c7fb6cd68b27ed713b6a"
                   "_mr_c2_fragment_input_magenta") == 0);
  return pixel_capture ||
         (shader_type == microsoft::D3D10_SB_VERTEX_SHADER &&
          std::strcmp(
              function_name,
              "vs_59c2a0b5_adcd61f38fa5038495d5d87e7f3a67bb8d794d86"
              "_mr_c3_vertex_reg1_magenta") == 0);
}

unsigned macrunner_fragment_translation_probe_limit() {
  static const unsigned limit = [] {
    const char *value =
        std::getenv("MACRUNNER_HB_FRAGMENT_SHADER_TRANSLATION_PROBE_MAX");
    char *end = nullptr;
    unsigned long parsed = value && value[0] ? std::strtoul(value, &end, 0) : 0;
    return end && end != value && parsed > 0 && parsed <= 256
               ? static_cast<unsigned>(parsed)
               : 64u;
  }();
  return limit;
}

bool macrunner_fragment_translation_probe_claim(
    microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type,
    const char *function_name, unsigned *ordinal) {
  const bool causal_capture =
      macrunner_causal_ladder_capture_claim(shader_type, function_name);
  if (!causal_capture &&
      (!macrunner_fragment_translation_probe_enabled() ||
       shader_type != microsoft::D3D10_SB_PIXEL_SHADER || !function_name ||
       std::strncmp(function_name, "ps_", 3) != 0))
    return false;
  static std::atomic_uint count{0};
  unsigned current = count.fetch_add(1, std::memory_order_relaxed);
  if (!causal_capture && current >= macrunner_fragment_translation_probe_limit())
    return false;
  if (ordinal)
    *ordinal = current + 1;
  return true;
}

void macrunner_store_magenta_register(
    llvm::IRBuilder<> &builder, dxmt::air::AirType &types,
    llvm::Value *register_file,
    unsigned register_index) {
  auto *array_type = llvm::cast<llvm::ArrayType>(
      register_file->getType()->getNonOpaquePointerElementType());
  auto *slot = builder.CreateInBoundsGEP(
      array_type, register_file,
      {builder.getInt32(0), builder.getInt32(register_index)});
  auto *zero = llvm::ConstantFP::get(types._float, 0.0);
  auto *one = llvm::ConstantFP::get(types._float, 1.0);
  auto *magenta = llvm::ConstantVector::get({one, zero, one, one});
  builder.CreateStore(magenta, slot);
}

constexpr uint32_t macrunner_causal_sidechannel_width = 1024;
constexpr uint32_t macrunner_causal_sidechannel_height = 768;
constexpr uint32_t macrunner_causal_sidechannel_buffer_index = 28;

std::string macrunner_fragment_translation_probe_dir() {
  const char *base = std::getenv("DXMT_LOG_PATH");
  std::string dir = base && base[0] ? base : ".";
  dir += "/fragment-translation";
  std::error_code ec = llvm::sys::fs::create_directories(dir);
  if (ec)
    std::fprintf(stderr,
                 "macrunner-hb-fragment-translation: side=airconv "
                 "phase=mkdir-error path=%s error=%s\n",
                 dir.c_str(), ec.message().c_str());
  return dir;
}

std::string macrunner_fragment_translation_probe_stem(
    const char *function_name) {
  std::string stem = function_name ? function_name : "unknown";
  for (char &c : stem)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
      c = '_';
  return macrunner_fragment_translation_probe_dir() + "/" + stem;
}

bool macrunner_fragment_translation_write_bytes(
    const std::string &path, const void *data, size_t size) {
  std::error_code ec;
  llvm::raw_fd_ostream output(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    std::fprintf(stderr,
                 "macrunner-hb-fragment-translation: side=airconv "
                 "phase=write-error path=%s error=%s\n",
                 path.c_str(), ec.message().c_str());
    return false;
  }
  output.write(static_cast<const char *>(data), size);
  output.flush();
  return !output.has_error();
}

bool macrunner_fragment_translation_write_module(
    const std::string &path, const llvm::Module &module) {
  std::error_code ec;
  llvm::raw_fd_ostream output(path, ec, llvm::sys::fs::OF_Text);
  if (ec) {
    std::fprintf(stderr,
                 "macrunner-hb-fragment-translation: side=airconv "
                 "phase=write-error path=%s error=%s\n",
                 path.c_str(), ec.message().c_str());
    return false;
  }
  module.print(output, nullptr);
  output.flush();
  return !output.has_error();
}

std::string macrunner_fragment_translation_return_type(
    llvm::Module &module, const char *function_name) {
  llvm::Function *function = module.getFunction(function_name);
  if (!function)
    return "missing";
  std::string result;
  llvm::raw_string_ostream stream(result);
  function->getReturnType()->print(stream);
  stream.flush();
  return result;
}

} // namespace

class SM50CompiledBitcodeInternal {
public:
  llvm::SmallVector<char, 0> vec;
};

namespace dxmt::dxbc {

inline dxmt::shader::common::ResourceType
to_shader_resource_type(microsoft::D3D10_SB_RESOURCE_DIMENSION dim) {
  switch (dim) {
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_UNKNOWN:
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_BUFFER:
  case microsoft::D3D11_SB_RESOURCE_DIMENSION_RAW_BUFFER:
  case microsoft::D3D11_SB_RESOURCE_DIMENSION_STRUCTURED_BUFFER:
    return shader::common::ResourceType::TextureBuffer;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D:
    return shader::common::ResourceType::Texture1D;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D:
    return shader::common::ResourceType::Texture2D;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS:
    return shader::common::ResourceType::Texture2DMultisampled;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D:
    return shader::common::ResourceType::Texture3D;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE:
    return shader::common::ResourceType::TextureCube;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY:
    return shader::common::ResourceType::Texture1DArray;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY:
    return shader::common::ResourceType::Texture2DArray;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY:
    return shader::common::ResourceType::Texture2DMultisampledArray;
  case microsoft::D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY:
    return shader::common::ResourceType::TextureCubeArray;
  }
  assert(0 && "invalid D3D10_SB_RESOURCE_DIMENSION");
};

inline dxmt::shader::common::ScalerDataType
to_shader_scaler_type(microsoft::D3D10_SB_RESOURCE_RETURN_TYPE type) {
  switch (type) {

  case microsoft::D3D10_SB_RETURN_TYPE_UNORM:
  case microsoft::D3D10_SB_RETURN_TYPE_SNORM:
  case microsoft::D3D10_SB_RETURN_TYPE_FLOAT:
    return shader::common::ScalerDataType::Float;
  case microsoft::D3D10_SB_RETURN_TYPE_SINT:
    return shader::common::ScalerDataType::Int;
  case microsoft::D3D10_SB_RETURN_TYPE_UINT:
    return shader::common::ScalerDataType::Uint;
  case microsoft::D3D10_SB_RETURN_TYPE_MIXED:
    return shader::common::ScalerDataType::Uint; // kinda weird but ok
  case microsoft::D3D11_SB_RETURN_TYPE_DOUBLE:
  case microsoft::D3D11_SB_RETURN_TYPE_CONTINUED:
  case microsoft::D3D11_SB_RETURN_TYPE_UNUSED:
    break;
  }
  assert(0 && "invalid D3D10_SB_RESOURCE_RETURN_TYPE");
}

uint32_t next_pow2(uint32_t x) {
  return x == 1 ? 1 : 1 << (32 - __builtin_clz(x - 1));
}


void setup_immediate_constant_buffer(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
) {
  if (!shader_info->immConstantBufferData.size()) {
    return;
  }
  auto &context = module.getContext();
  auto type = llvm::ArrayType::get(
    types._int4, shader_info->immConstantBufferData.size()
  );
  auto const_data = llvm::ConstantArray::get(
    type,
    shader_info->immConstantBufferData |
      [&](auto data) {
        return llvm::ConstantVector::get(
          {llvm::ConstantInt::get(context, llvm::APInt{32, data[0], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[1], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[2], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[3], false})}
        );
      }
  );
  llvm::GlobalVariable *icb = new llvm::GlobalVariable(
    module, type, true, llvm::GlobalValue::InternalLinkage, const_data, "icb",
    nullptr, llvm::GlobalValue::NotThreadLocal, 2
  );
  icb->setAlignment(llvm::Align(4));
  resource_map.icb = icb;
  resource_map.icb_float = builder.CreateBitCast(
    resource_map.icb, llvm::ArrayType::get(
                        types._float4, shader_info->immConstantBufferData.size()
                      )
                        ->getPointerTo(2)
  );
}

void setup_tgsm(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module
) {
  for (auto &[id, tgsm] : shader_info->tgsmMap) {
    auto type = llvm::ArrayType::get(types._int, tgsm.size_in_uint);
    llvm::GlobalVariable *tgsm_h = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::InternalLinkage,
      llvm::UndefValue::get(type), "g" + std::to_string(id), nullptr,
      llvm::GlobalValue::NotThreadLocal, 3
    );
    tgsm_h->setAlignment(llvm::Align(4));
    resource_map.tgsm_map[id] = {tgsm.structured ? tgsm.stride : 0, tgsm_h};
  }
}

void setup_temp_register(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
) {
  resource_map.temp.ptr_int4 = builder.CreateAlloca(
    llvm::ArrayType::get(types._int, 4 * shader_info->tempRegisterCount)
  );
  resource_map.temp.ptr_float4 = builder.CreateBitCast(
    resource_map.temp.ptr_int4,
    llvm::ArrayType::get(types._float, 4 * shader_info->tempRegisterCount)
      ->getPointerTo()
  );
  for (auto &phase : shader_info->phases) {
    resource_map.phases.push_back({});
    auto &phase_temp = resource_map.phases.back();

    phase_temp.temp.ptr_int4 = builder.CreateAlloca(
      llvm::ArrayType::get(types._int, 4 * phase.tempRegisterCount)
    );
    phase_temp.temp.ptr_float4 = builder.CreateBitCast(
      phase_temp.temp.ptr_int4,
      llvm::ArrayType::get(types._float, 4 * phase.tempRegisterCount)
        ->getPointerTo()
    );

    for (auto &[idx, info] : phase.indexableTempRegisterCounts) {
      auto &[numRegisters, mask] = info;
      auto channel_count = std::bit_width(mask);
      auto ptr_int_vec = builder.CreateAlloca(llvm::ArrayType::get(types._int, numRegisters * channel_count));
      auto ptr_float_vec = builder.CreateBitCast(
          ptr_int_vec, llvm::ArrayType::get(types._float, numRegisters * channel_count)->getPointerTo()
      );
      phase_temp.indexable_temp_map[idx] = {
        ptr_int_vec, ptr_float_vec, (uint32_t)channel_count
      };
    }
  }
  for (auto &[idx, info] : shader_info->indexableTempRegisterCounts) {
    auto &[numRegisters, mask] = info;
    auto channel_count = std::bit_width(mask);
    auto ptr_int_vec = builder.CreateAlloca(llvm::ArrayType::get(types._int, numRegisters * channel_count));
    auto ptr_float_vec = builder.CreateBitCast(
        ptr_int_vec, llvm::ArrayType::get(types._float, numRegisters * channel_count)->getPointerTo()
    );
    resource_map.indexable_temp_map[idx] = {
      ptr_int_vec, ptr_float_vec, (uint32_t)channel_count
    };
  }
  if (shader_info->use_cmp_exch) {
    resource_map.cmp_exch_temp = builder.CreateAlloca(types._int);
  }
}

// MacRunner module-meta 17.10 (куратор 12:47/12:51). MACRUNNER_AIRCONV_MODULE_META: не задано/off — как прежде (ничего);
// auto — по условиям (fast+fnattrs если dxil_all_fast; denorm если режим ftz/any/пусто); иначе список через запятую из
// fast, denorm, fbfetch, fnattrs, ver29 — принудительно (только для опытов A/B).
void setup_module_meta(llvm::Module &module, llvm::Function *function, const dxmt::dxbc::SM50ShaderInternal *shader) {
  using namespace llvm;
  static const char *env = getenv("MACRUNNER_AIRCONV_MODULE_META");
  static const bool trace = getenv("MACRUNNER_AIRCONV_MODULE_META_TRACE") != nullptr;
  if (!env || !*env || strcmp(env, "off") == 0) return;
  // HKGPU 04.11.2026: `safe` / `safeall` — fast math WITHOUT nnan/ninf/nsz/reassoc: arcp+contract+afn on the floating-point
  // instructions of DXBC shaders that do not allow refactoring (no dcl_globalFlags refactoringAllowed — all 49 HK shaders; the
  // converter then emits every instruction without flags). `safe`: fragment and compute stages only (the converter's own
  // invariance policy keeps pre-raster stages without contraction); `safeall`: every stage. Shaders WITH refactoringAllowed are
  // left as the converter emits them (flags per instruction, `precise` respected). No compile option, no denormal flush (the
  // tape: denorms_disable alone +2,3 % GPU). The instruction flags are added after optimisation (applyPostMeta).
  bool fast = false, denorm = false, fbfetch = false, fnattrs = false, ver29 = false, safe = false, safeall = false;
  if (strcmp(env, "auto") == 0) {
    if (shader && shader->dxil) {
      fast = fnattrs = shader->dxil_all_fast;
      // DXIL.rst («Denorm mode»): атрибут функции "fp32-denorm-mode" ∈ {any, preserve, ftz}; без атрибута — умолчание any
      // (куратор 12:58: без атрибута = any → denorms_disable допустим). Явный preserve — отказ от флага, правило полное.
      if (shader->dxil_denorm_mode == "preserve") denorm = false;
      else denorm = shader->dxil_denorm_mode.empty() || shader->dxil_denorm_mode == "ftz" || shader->dxil_denorm_mode == "any";
    }
  } else {
    std::string e(env);
    auto has = [&](const char *t) { return (("," + e + ",").find(std::string(",") + t + ",") != std::string::npos); };
    fast = has("fast"); denorm = has("denorm"); fbfetch = has("fbfetch"); fnattrs = has("fnattrs"); ver29 = has("ver29");
    safe = has("safe"); safeall = has("safeall");
  }
  auto &context = module.getContext();
  auto opt = [&](const char *name) { return MDTuple::get(context, {MDString::get(context, name)}); };
  if (denorm || fast || fbfetch) {
    auto co = module.getOrInsertNamedMetadata("air.compile_options");
    if (denorm) co->addOperand(opt("air.compile.denorms_disable"));
    co->addOperand(opt(fast ? "air.compile.fast_math_enable" : "air.compile.fast_math_disable"));
    if (fbfetch) co->addOperand(opt("air.compile.framebuffer_fetch_enable"));
  }
  if ((safe || safeall) && shader && !shader->dxil && !shader->shader_info.refactoringAllowed) {
    const auto t = shader->shader_type;
    const bool stage_ok = safeall || t == microsoft::D3D10_SB_PIXEL_SHADER || t == microsoft::D3D11_SB_COMPUTE_SHADER;
    if (stage_ok && !module.getNamedMetadata("macrunner.safe_fp_flags"))
      module.getOrInsertNamedMetadata("macrunner.safe_fp_flags");   // marker for applyPostMeta, removed there
  }
  if (fnattrs && function) {
    for (const char *a : {"approx-func-fp-math", "no-infs-fp-math", "no-nans-fp-math", "no-signed-zeros-fp-math", "unsafe-fp-math", "no-trapping-math"})
      function->addFnAttr(a, "true");
  }
  if (ver29) {
    if (auto *v = module.getNamedMetadata("air.version")) module.eraseNamedMetadata(v);
    auto ci = [&](uint32_t x) { return ConstantAsMetadata::get(ConstantInt::get(context, APInt{32, x, false})); };
    module.getOrInsertNamedMetadata("air.version")->addOperand(MDTuple::get(context, {ci(2), ci(9), ci(0)}));
  }
  if (trace)
    fprintf(stderr, "module_meta fast=%d denorm=%d fbfetch=%d fnattrs=%d ver29=%d safe=%d dxil=%d all_fast=%d denorm_mode=%s\n", fast, denorm, fbfetch, fnattrs, ver29, safe,
            shader && shader->dxil ? 1 : 0, shader ? shader->dxil_all_fast : 0, shader && !shader->dxil_denorm_mode.empty() ? shader->dxil_denorm_mode.c_str() : "-");
}

void setup_metal_version(llvm::Module &module, SM50_SHADER_METAL_VERSION metal_version) {
  using namespace llvm;
  auto &context = module.getContext();
  auto createUnsignedInteger = [&](uint32_t s) {
    return ConstantAsMetadata::get(ConstantInt::get(context, APInt{32, s, false}));
  };
  auto createString = [&](auto s) { return MDString::get(context, s); };

  auto airVersion = module.getOrInsertNamedMetadata("air.version");
  auto airLangVersion = module.getOrInsertNamedMetadata("air.language_version");
  switch (metal_version) {
  case SM50_SHADER_METAL_320: {
    airVersion->addOperand(
        MDTuple::get(context, {createUnsignedInteger(2), createUnsignedInteger(7), createUnsignedInteger(0)})
    );
    airLangVersion->addOperand(MDTuple::get(
        context, {createString("Metal"), createUnsignedInteger(3), createUnsignedInteger(2), createUnsignedInteger(0)}
    ));
    module.setTargetTriple("air64-apple-macosx15.0.0");
    break;
  }
  default: {
    airVersion->addOperand(
        MDTuple::get(context, {createUnsignedInteger(2), createUnsignedInteger(6), createUnsignedInteger(0)})
    );
    airLangVersion->addOperand(MDTuple::get(
        context, {createString("Metal"), createUnsignedInteger(3), createUnsignedInteger(1), createUnsignedInteger(0)}
    ));
    break;
  }
  }
}

RegisterComponentType
component_type_from_pixel_format(air::MTLPixelFormat format) {
  switch (format) {
  case air::MTLPixelFormat::R8Sint:
  case air::MTLPixelFormat::R16Sint:
  case air::MTLPixelFormat::RG8Sint:
  case air::MTLPixelFormat::R32Sint:
  case air::MTLPixelFormat::RG16Sint:
  case air::MTLPixelFormat::RGBA8Sint:
  case air::MTLPixelFormat::RG32Sint:
  case air::MTLPixelFormat::RGBA16Sint:
  case air::MTLPixelFormat::RGBA32Sint:
    return RegisterComponentType::Int;
  case air::MTLPixelFormat::R8Uint:
  case air::MTLPixelFormat::R16Uint:
  case air::MTLPixelFormat::RG8Uint:
  case air::MTLPixelFormat::R32Uint:
  case air::MTLPixelFormat::RG16Uint:
  case air::MTLPixelFormat::RGBA8Uint:
  case air::MTLPixelFormat::RGB10A2Uint:
  case air::MTLPixelFormat::RG32Uint:
  case air::MTLPixelFormat::RGBA16Uint:
  case air::MTLPixelFormat::RGBA32Uint:
    return RegisterComponentType::Uint;
  case air::MTLPixelFormat::A8Unorm:
  case air::MTLPixelFormat::R8Unorm:
  case air::MTLPixelFormat::R8Unorm_sRGB:
  case air::MTLPixelFormat::R8Snorm:
  case air::MTLPixelFormat::R16Unorm:
  case air::MTLPixelFormat::R16Snorm:
  case air::MTLPixelFormat::R16Float:
  case air::MTLPixelFormat::RG8Unorm:
  case air::MTLPixelFormat::RG8Unorm_sRGB:
  case air::MTLPixelFormat::RG8Snorm:
  case air::MTLPixelFormat::B5G6R5Unorm:
  case air::MTLPixelFormat::A1BGR5Unorm:
  case air::MTLPixelFormat::ABGR4Unorm:
  case air::MTLPixelFormat::BGR5A1Unorm:
  case air::MTLPixelFormat::R32Float:
  case air::MTLPixelFormat::RG16Unorm:
  case air::MTLPixelFormat::RG16Snorm:
  case air::MTLPixelFormat::RG16Float:
  case air::MTLPixelFormat::RGBA8Unorm:
  case air::MTLPixelFormat::RGBA8Unorm_sRGB:
  case air::MTLPixelFormat::RGBA8Snorm:
  case air::MTLPixelFormat::BGRA8Unorm:
  case air::MTLPixelFormat::BGRA8Unorm_sRGB:
  case air::MTLPixelFormat::RGB10A2Unorm:
  case air::MTLPixelFormat::RG11B10Float:
  case air::MTLPixelFormat::RGB9E5Float:
  case air::MTLPixelFormat::BGR10A2Unorm:
  case air::MTLPixelFormat::BGR10_XR:
  case air::MTLPixelFormat::BGR10_XR_sRGB:
  case air::MTLPixelFormat::RG32Float:
  case air::MTLPixelFormat::RGBA16Unorm:
  case air::MTLPixelFormat::RGBA16Snorm:
  case air::MTLPixelFormat::RGBA16Float:
  case air::MTLPixelFormat::BGRA10_XR:
  case air::MTLPixelFormat::BGRA10_XR_sRGB:
  case air::MTLPixelFormat::RGBA32Float:
  case air::MTLPixelFormat::BC1_RGBA:
  case air::MTLPixelFormat::BC1_RGBA_sRGB:
  case air::MTLPixelFormat::BC2_RGBA:
  case air::MTLPixelFormat::BC2_RGBA_sRGB:
  case air::MTLPixelFormat::BC3_RGBA:
  case air::MTLPixelFormat::BC3_RGBA_sRGB:
  case air::MTLPixelFormat::BC4_RUnorm:
  case air::MTLPixelFormat::BC4_RSnorm:
  case air::MTLPixelFormat::BC5_RGUnorm:
  case air::MTLPixelFormat::BC5_RGSnorm:
  case air::MTLPixelFormat::BC6H_RGBFloat:
  case air::MTLPixelFormat::BC6H_RGBUfloat:
  case air::MTLPixelFormat::BC7_RGBAUnorm:
  case air::MTLPixelFormat::BC7_RGBAUnorm_sRGB:
    return RegisterComponentType::Float;
  default:
    break;
  }
  return RegisterComponentType::Unknown;
}

llvm::Error convert_dxbc_pixel_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  uint32_t pso_sample_mask = 0xffffffff;
  bool pso_dual_source_blending = false;
  bool pso_disable_depth_output = false;
  uint32_t pso_unorm_output_reg_mask = 0;
  uint32_t *pso_pixel_formats = nullptr;
  SM50_SHADER_PS_INPUT_LINKAGE_DATA *pixel_linkage = nullptr;
  args_get_data<SM50_SHADER_PS_INPUT_LINKAGE, SM50_SHADER_PS_INPUT_LINKAGE_DATA>(pArgs, &pixel_linkage);
  if (!PixelInputLinkage(pixel_linkage).valid())
    return llvm::make_error<UnsupportedFeature>("Invalid pixel input linkage");
  SM50_SHADER_PSO_PIXEL_SHADER_DATA *pso_data = nullptr;
  if (args_get_data<SM50_SHADER_PSO_PIXEL_SHADER, SM50_SHADER_PSO_PIXEL_SHADER_DATA>(pArgs, &pso_data)) {
    pso_dual_source_blending = pso_data->dual_source_blending;
    pso_disable_depth_output = pso_data->disable_depth_output;
    pso_unorm_output_reg_mask = pso_data->unorm_output_reg_mask;
    pso_sample_mask = pso_data->sample_mask;
    pso_pixel_formats = pso_data->pixel_formats;
  }
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  Prologues prologue;
  Epilogues epilogue;

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.dual_source_blending = pso_dual_source_blending;
    sig_ctx.disable_depth_output = pso_disable_depth_output;
    sig_ctx.pull_mode_reg_mask = shader_info->pull_mode_reg_mask;
    sig_ctx.unorm_output_reg_mask = pso_unorm_output_reg_mask;
    sig_ctx.pixel_input_linkage = PixelInputLinkage(pixel_linkage);
    for (const auto &input : pShaderInternal->input_signature)
      if (!input.isSystemValue())
        sig_ctx.pixel_input_linkage.signature(input.reg(), input.mask(), input.usageMask());
    if (pShaderInternal->dxil) {   // MacRunner dxil-tess 17.10: у PS из DXIL подпись — в dxil_entry (input_signature пуст)
      for (const auto &e : pShaderInternal->dxil_entry.inputs) {
        if (e.semantic_kind != dxil::SemanticKind::Arbitrary || e.start_row < 0 || e.start_col < 0)
          continue;
        // Zero usage means the PS does not read this input; its handler is omitted.
        // Keep the signature entry, but require only components actually read.
        uint32_t usage = e.usage_mask << e.start_col;
        uint32_t mask = ((1u << e.cols) - 1u) << e.start_col;
        for (uint32_t r = 0; r < std::max(1u, e.rows); ++r)
          sig_ctx.pixel_input_linkage.signature((uint32_t)e.start_row + r, mask, usage);
      }
    }
    sig_ctx.pso_sample_mask = pso_sample_mask;
    if (pso_pixel_formats)
      memcpy(sig_ctx.pixel_formats, pso_pixel_formats, sizeof(sig_ctx.pixel_formats));
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
    if (!sig_ctx.pixel_input_linkage.complete())
      return llvm::make_error<UnsupportedFeature>("Incomplete or unsupported pixel input linkage");
    // Acknowledge the optional argument so the caller can tell this provider
    // apart from one that silently ignored an unknown argument type.
    if (pixel_linkage)
      pixel_linkage->applied = SM50_SHADER_PS_INPUT_LINKAGE_APPLIED;
  }
  if (pso_sample_mask != 0xffffffff) {
    auto assigned_index =
      func_signature.DefineOutput(air::OutputCoverageMask{});
    epilogue.Add([=](llvm::Value *value, IOHelper &io) -> llvm::Expected<llvm::Value *> {
      return io.PopulateDefaultCoverageMask(value, assigned_index, pso_sample_mask);
    });
  }

  const bool causal_sidechannel =
      shader_flags & SM50_SHADER_FLAG_CAUSAL_FRAGMENT_SIDECHANNEL;
  uint32_t causal_position_arg = UINT32_MAX;
  uint32_t causal_buffer_arg = UINT32_MAX;
  if (causal_sidechannel) {
    causal_position_arg = func_signature.DefineInput(air::InputPosition{
        .interpolation = air::Interpolation::center_no_perspective,
    });
    causal_buffer_arg = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = macrunner_causal_sidechannel_width *
                       macrunner_causal_sidechannel_height * 16u,
        .location_index = macrunner_causal_sidechannel_buffer_index,
        .array_size = 1,
        .memory_access = air::MemoryAccess::write,
        .address_space = air::AddressSpace::device,
        .type = air::msl_float4,
        .arg_name = "macrunner_causal_fragment_output",
        .raster_order_group = {},
    });
  }

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D10_SB_PIXEL_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);

  auto [function, function_metadata] =
    func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
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
  setup_module_meta(module, function, pShaderInternal);

  resource_map.input.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.input.ptr_float4 = builder.CreateBitCast(
    resource_map.input.ptr_int4,
    llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );
  resource_map.input_element_count = max_input_register;
  resource_map.output.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register)
    );
  resource_map.output.ptr_float4 = builder.CreateBitCast(
    resource_map.output.ptr_int4,
    llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo()
  );
  resource_map.output_element_count = max_output_register;

  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

  struct context ctx{
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = pso_sample_mask,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
  };

  IOHelper io(air, function, types, resource_map);

  if (auto err = prologue.Run(io)) {
    return err;
  }
  if (shader_flags & SM50_SHADER_FLAG_CAUSAL_FRAGMENT_INPUT_MAGENTA) {
    if (max_input_register <= 1) {
      std::fprintf(stderr,
                   "macrunner-hb-causal-ladder: phase=c2-inject result=fail "
                   "function=%s reason=missing-input-reg1 count=%u\n",
                   name ? name : "(null)", max_input_register);
    } else {
      macrunner_store_magenta_register(
          builder, types, resource_map.input.ptr_float4, 1);
      std::fprintf(stderr,
                   "macrunner-hb-causal-ladder: phase=c2-inject result=ok "
                   "function=%s input=reg1 value=1,0,1,1\n",
                   name ? name : "(null)");
    }
  }
  llvm::Expected<llvm::BasicBlock *> real_entry =
      pShaderInternal->dxil ? dxil::convert_basicblocks(pShaderInternal->dxil_entry, ctx, epilogue_bb)
                            : convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);
  auto epilogue_result = epilogue.Run(function->getReturnType(), io);
  if (auto err = epilogue_result.takeError()) {
    return err;
  }
  auto value = epilogue_result.get();
  if (shader_flags & SM50_SHADER_FLAG_CAUSAL_FRAGMENT_FINAL_MAGENTA) {
    auto *return_type = value ? llvm::dyn_cast<llvm::StructType>(value->getType())
                              : nullptr;
    if (!return_type || !return_type->getNumElements() ||
        return_type->getElementType(0) != types._float4)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MacRunner C1 requires float4 render target 0");
    auto *zero = llvm::ConstantFP::get(types._float, 0.0);
    auto *one = llvm::ConstantFP::get(types._float, 1.0);
    auto *magenta = llvm::ConstantVector::get({one, zero, one, one});
    value = builder.CreateInsertValue(value, magenta, {0});
    std::fprintf(stderr,
                 "macrunner-hb-causal-ladder: phase=c1-inject result=ok "
                 "function=%s output=rt0 value=1,0,1,1\n",
                 name ? name : "(null)");
  }
  if (causal_sidechannel) {
    if (!value || causal_position_arg >= function->arg_size() ||
        causal_buffer_arg >= function->arg_size())
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MacRunner fragment side-channel signature is incomplete");
    auto *return_type = llvm::dyn_cast<llvm::StructType>(value->getType());
    auto *position = function->getArg(causal_position_arg);
    auto *buffer = function->getArg(causal_buffer_arg);
    if (!return_type || !return_type->getNumElements() ||
        return_type->getElementType(0) != types._float4 ||
        position->getType() != types._float4 || !buffer->getType()->isPointerTy())
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MacRunner fragment side-channel types are incompatible");

    auto *x_float = builder.CreateExtractElement(position, builder.getInt32(0));
    auto *y_float = builder.CreateExtractElement(position, builder.getInt32(1));
    auto *zero_float = llvm::ConstantFP::get(types._float, 0.0);
    auto *width_float = llvm::ConstantFP::get(
        types._float, static_cast<double>(macrunner_causal_sidechannel_width));
    auto *height_float = llvm::ConstantFP::get(
        types._float, static_cast<double>(macrunner_causal_sidechannel_height));
    auto *in_bounds = builder.CreateAnd(
        builder.CreateAnd(builder.CreateFCmpOGE(x_float, zero_float),
                          builder.CreateFCmpOLT(x_float, width_float)),
        builder.CreateAnd(builder.CreateFCmpOGE(y_float, zero_float),
                          builder.CreateFCmpOLT(y_float, height_float)));
    auto *store_bb = llvm::BasicBlock::Create(
        context, "macrunner.causal.sidechannel.store", function);
    auto *return_bb = llvm::BasicBlock::Create(
        context, "macrunner.causal.sidechannel.return", function);
    builder.CreateCondBr(in_bounds, store_bb, return_bb);
    builder.SetInsertPoint(store_bb);
    auto *x = builder.CreateFPToUI(x_float, types._int);
    auto *y = builder.CreateFPToUI(y_float, types._int);
    auto *pixel = builder.CreateAdd(
        builder.CreateMul(y,
                          builder.getInt32(macrunner_causal_sidechannel_width)),
        x);
    auto *slot = builder.CreateInBoundsGEP(types._float4, buffer, pixel);
    builder.CreateStore(builder.CreateExtractValue(value, {0}), slot);
    builder.CreateBr(return_bb);
    builder.SetInsertPoint(return_bb);
    std::fprintf(stderr,
                 "macrunner-hb-causal-ladder: phase=sidechannel-inject "
                 "result=ok function=%s buffer=%u extent=%ux%u\n",
                 name ? name : "(null)",
                 macrunner_causal_sidechannel_buffer_index,
                 macrunner_causal_sidechannel_width,
                 macrunner_causal_sidechannel_height);
  }
  if (value == nullptr) {
    builder.CreateRetVoid();
  } else {
    builder.CreateRet(value);
  }

  module.getOrInsertNamedMetadata("air.fragment")
    ->addOperand(function_metadata);

  return llvm::Error::success();
};

llvm::Error convert_dxbc_compute_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

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

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D11_SB_COMPUTE_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);
  setup_tgsm(shader_info, resource_map, types, module);

  auto [function, function_metadata] =
    func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
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
  setup_module_meta(module, function, pShaderInternal);
  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

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
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
  };

  IOHelper io(air, function, types, resource_map);

  if (auto err = prologue.Run(io)) {
    return err;
  }
  llvm::Expected<llvm::BasicBlock *> real_entry =
      pShaderInternal->dxil ? dxil::convert_basicblocks(pShaderInternal->dxil_entry, ctx, epilogue_bb)
                            : convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);

  auto epilogue_result = epilogue.Run(function->getReturnType(), io);
  if (auto err = epilogue_result.takeError()) {
    return err;
  }
  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.kernel")->addOperand(function_metadata);

  return llvm::Error::success();
};

llvm::Error convert_dxbc_vertex_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *vertex_so = nullptr;
  args_get_data<SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT, SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA>(
      pArgs, &vertex_so
  );
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  SM50_SHADER_GS_PASS_THROUGH_DATA *gs_passthrough = nullptr;
  bool rasterization_disabled =
      (args_get_data<SM50_SHADER_GS_PASS_THROUGH, SM50_SHADER_GS_PASS_THROUGH_DATA>(pArgs, &gs_passthrough) &&
       gs_passthrough->RasterizationDisabled) ||
      (vertex_so != nullptr);
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);

  Prologues prologue;
  Epilogues epilogue;

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.ia_layout = ia_layout;
    sig_ctx.skip_vertex_output = rasterization_disabled;
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }

    for (auto& out : pShaderInternal->output_signature) {
      if(out.isSystemValue()) continue;
      func_signature.DefineOutput(air::OutputVertex{
        .user = out.consistentAttributeName(),
        .type = to_msl_type(out.componentType()),
      });
    }
  }
  if (vertex_so) {
    auto bv = func_signature.DefineInput(air::InputBaseVertex{});
    auto vid = func_signature.DefineInput(air::InputVertexID{});
    auto so_table = func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = SM50_BINDING_INDEX_STREAM_OUTPUT0,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = air::MSLUint{},
      .arg_name = "stream_outputs",
      .raster_order_group = {}
    });
    epilogue.Add([=](llvm::Value *value, IOHelper &io) -> llvm::Expected<llvm::Value *> {
      return io.PopulateOutputStreamOutput(value, vertex_so, bv, vid, so_table);
    });
  }

  uint32_t vertex_idx = func_signature.DefineInput(air::InputVertexID{});
  uint32_t base_vertex_idx = func_signature.DefineInput(air::InputBaseVertex{});
  uint32_t instance_idx = func_signature.DefineInput(air::InputInstanceID{});
  uint32_t base_instance_idx =
    func_signature.DefineInput(air::InputBaseInstance{});

  uint32_t clip_distance_out_idx = ~0u;
  if (pShaderInternal->clip_distance_scalars.size()) {
    clip_distance_out_idx = func_signature.DefineOutput(
      air::OutputClipDistance{pShaderInternal->clip_distance_scalars.size()}
    );
  };

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D10_SB_VERTEX_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);

  uint32_t rta_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.RenderTargetArrayIndexReg != 255) {
    rta_idx_out =
      func_signature.DefineOutput(air::OutputRenderTargetArrayIndex{});
  }
  uint32_t va_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.ViewportArrayIndexReg != 255) {
    va_idx_out = func_signature.DefineOutput(air::OutputViewportArrayIndex{});
  }

  auto [function, function_metadata] = func_signature.CreateFunction(
    name, context, module, 0,
    rasterization_disabled
  );

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
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
  setup_module_meta(module, function, pShaderInternal);

  resource_map.vertex_id_with_base = function->getArg(vertex_idx);
  resource_map.base_vertex_id = function->getArg(base_vertex_idx);
  resource_map.instance_id_with_base = function->getArg(instance_idx);
  resource_map.base_instance_id = function->getArg(base_instance_idx);
  resource_map.vertex_id = builder.CreateSub(
    resource_map.vertex_id_with_base, resource_map.base_vertex_id
  );
  resource_map.instance_id = builder.CreateSub(
    resource_map.instance_id_with_base, resource_map.base_instance_id
  );

  resource_map.input.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.input.ptr_float4 = builder.CreateBitCast(
    resource_map.input.ptr_int4,
    llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );
  resource_map.input_element_count = max_input_register;
  resource_map.output.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register)
    );
  resource_map.output.ptr_float4 = builder.CreateBitCast(
    resource_map.output.ptr_int4,
    llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo()
  );
  resource_map.output_element_count = max_output_register;

  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

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
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
  };

  IOHelper io(air, function, types, resource_map);

  if (auto err = prologue.Run(io)) {
    return err;
  }
  llvm::Expected<llvm::BasicBlock *> real_entry =
      pShaderInternal->dxil ? dxil::convert_basicblocks(pShaderInternal->dxil_entry, ctx, epilogue_bb)
                            : convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);
  if (shader_flags & SM50_SHADER_FLAG_CAUSAL_VERTEX_REG1_MAGENTA) {
    if (max_output_register <= 1) {
      std::fprintf(stderr,
                   "macrunner-hb-causal-ladder: phase=c3-inject result=fail "
                   "function=%s reason=missing-output-reg1 count=%u\n",
                   name ? name : "(null)", max_output_register);
    } else {
      macrunner_store_magenta_register(
          builder, types, resource_map.output.ptr_float4, 1);
      std::fprintf(stderr,
                   "macrunner-hb-causal-ladder: phase=c3-inject result=ok "
                   "function=%s output=user(reg1_0) value=1,0,1,1\n",
                   name ? name : "(null)");
    }
  }
  auto epilogue_result = epilogue.Run(function->getReturnType(), io);
  if (auto err = epilogue_result.takeError()) {
    return err;
  }
  auto value = epilogue_result.get();

  if (pShaderInternal->clip_distance_scalars.size()) {
    auto clip_distance_ty = llvm::ArrayType::get(
      types._float, pShaderInternal->clip_distance_scalars.size()
    );
    pvalue clip_distance_array = llvm::ConstantAggregateZero::get(clip_distance_ty);
    unsigned clip_distance_idx = 0;
    for (auto &scalar : pShaderInternal->clip_distance_scalars) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_float4->getType()
          ->getNonOpaquePointerElementType(),
        resource_map.output.ptr_float4,
        {builder.getInt32(0), builder.getInt32(scalar.reg),
         builder.getInt32(scalar.component)}
      );
      clip_distance_array = builder.CreateInsertValue(
        clip_distance_array, builder.CreateLoad(types._float, src_ptr), {clip_distance_idx++}
      );
    }
    value = builder.CreateInsertValue(
      value, clip_distance_array, {clip_distance_out_idx}
    );
  };

  if (value == nullptr) {
    builder.CreateRetVoid();
  } else {
    if (rta_idx_out != ~0u) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(
        ),
        resource_map.output.ptr_int4,
        {builder.getInt32(0),
         builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexComponent)}
      );
      value = builder.CreateInsertValue(
        value, builder.CreateLoad(types._int, src_ptr), {rta_idx_out}
      );
    }
    if (va_idx_out != ~0u) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(
        ),
        resource_map.output.ptr_int4,
        {builder.getInt32(0),
         builder.getInt32(gs_passthrough->Data.ViewportArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.ViewportArrayIndexComponent)}
      );
      value = builder.CreateInsertValue(
        value, builder.CreateLoad(types._int, src_ptr), {va_idx_out}
      );
    }
    builder.CreateRet(value);
  }

  module.getOrInsertNamedMetadata("air.vertex")->addOperand(function_metadata);
  return llvm::Error::success();
};

llvm::Error convertDXBC(
  sm50_shader_t pShader, const char *name, llvm::LLVMContext &context,
  llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto pShaderInternal = (SM50ShaderInternal *)pShader;

  switch (pShaderInternal->shader_type) {
  case microsoft::D3D10_SB_PIXEL_SHADER: {
    SM50_SHADER_PS_AS_COMPUTE_DATA *post_cs = nullptr;   // MacRunner POST-CS 25.10.2026
    if (args_get_data<SM50_SHADER_PS_AS_COMPUTE, SM50_SHADER_PS_AS_COMPUTE_DATA>(pArgs, &post_cs))
      return convert_post_cs(pShaderInternal, name, context, module, pArgs, post_cs);
    return convert_dxbc_pixel_shader(
      pShaderInternal, name, context, module, pArgs
    );
  }
  case microsoft::D3D10_SB_VERTEX_SHADER:
    return convert_dxbc_vertex_shader(
      pShaderInternal, name, context, module, pArgs
    );
  case microsoft::D3D11_SB_COMPUTE_SHADER:
    return convert_dxbc_compute_shader(
      pShaderInternal, name, context, module, pArgs
    );
  case microsoft::D3D11_SB_HULL_SHADER:
  case microsoft::D3D11_SB_DOMAIN_SHADER:
    return llvm::make_error<UnsupportedFeature>(
      "Hull and domain shader cannot be independently converted."
    );
  case microsoft::D3D10_SB_GEOMETRY_SHADER:
    return llvm::make_error<UnsupportedFeature>(
      "Geometry shader cannot be independently converted."
    );
  case microsoft::D3D12_SB_MESH_SHADER:
  case microsoft::D3D12_SB_AMPLIFICATION_SHADER:
  case microsoft::D3D11_SB_RESERVED0:
    break;
  }
  return llvm::make_error<UnsupportedFeature>("Not supported shader type");
};
} // namespace dxmt::dxbc

bool CheckGSBBIsPassThrough(dxmt::dxbc::BasicBlock *bb) {
  using namespace dxmt::dxbc;
  if (bb->instructions.any(patterns{
        [](InstNop &) { return false; }, [](InstMov &mov) { return false; },
        [](InstEmit &) { return false; }, [](InstCut &) { return false; },
        [](auto &) { return true; }
      }))
    return false;

  return std::visit(
    patterns{
      [](dxmt::dxbc::BasicBlockReturn) { return true; },
      [](dxmt::dxbc::BasicBlockUnconditionalBranch uncond) {
        return CheckGSBBIsPassThrough(uncond.target);
      },
      [](auto) { return false; }
    },
    bb->target
  );
}

bool CheckGSSignatureIsPassThrough(
  microsoft::CSignatureParser &input, microsoft::CSignatureParser5 &output,
  MTL_GEOMETRY_SHADER_PASS_THROUGH &data
) {
  using namespace microsoft;

  data.RenderTargetArrayIndexComponent = 255;
  data.RenderTargetArrayIndexReg = 255;
  data.ViewportArrayIndexComponent = 255;
  data.ViewportArrayIndexReg = 255;

  if (output.NumStreams() > 1) {
    return false;
  }
  if (output.RasterizedStream()) {
    return false;
  }

  const D3D11_SIGNATURE_PARAMETER *input_parameters, *output_parameters;
  input.GetParameters(&input_parameters);
  output.Signature(0)->GetParameters(&output_parameters);
  size_t num_input, num_output;
  num_input = input.GetNumParameters();
  num_output = output.Signature(0)->GetNumParameters();
  if (num_output > num_input) {
    // at least it is a non-trivial shader that generates new data
    return false;
  }
  bool has_match_record;
  for (unsigned i = 0; i < num_output; i++) {
    has_match_record = false;
    const D3D11_SIGNATURE_PARAMETER &out = output_parameters[i];
    for (unsigned j = 0; j < num_input; j++) {
      const D3D11_SIGNATURE_PARAMETER &in = input_parameters[j];
      if (out.Register == in.Register && (out.Mask & in.Mask) == out.Mask) {
        if (out.SystemValue == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX) {
          data.RenderTargetArrayIndexReg = out.Register;
          data.RenderTargetArrayIndexComponent = __builtin_ctz(out.Mask);
        } else if (out.SystemValue == D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX) {
          data.ViewportArrayIndexReg = out.Register;
          data.ViewportArrayIndexComponent = __builtin_ctz(out.Mask);
        } else {
          if (out.SemanticIndex != in.SemanticIndex) {
            return false;
          }
          if (strcasecmp(out.SemanticName, in.SemanticName)) {
            return false;
          }
        }
        has_match_record = true;
        break;
      }
    }
    if (!has_match_record) {
      return false;
    }
  }

  return true;
};

AIRCONV_API int SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
  MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError
) {
  using namespace microsoft;
  using namespace dxmt::dxbc;
  using namespace dxmt::air;
  using namespace dxmt::shader::common;
  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = std::make_unique<SM50ErrorInternal>();
  llvm::raw_svector_ostream errorOut(errorObj->buf);

  if (ppShader == nullptr) {
    errorOut << "ppShader can not be null\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  CDXBCParser DXBCParser;
  if (DXBCParser.ReadDXBC(pBytecode, BytecodeSize) != S_OK) {
    errorOut << "Invalid DXBC bytecode\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  UINT32 dxilBlobIdx = DXBCParser.FindNextMatchingBlob(microsoft::DXBC_DXIL, 0);
  if (dxilBlobIdx != DXBC_BLOB_NOT_FOUND)
    return dxmt::dxil::initialize(
      DXBCParser.GetBlob(dxilBlobIdx), DXBCParser.GetBlobSize(dxilBlobIdx), ppShader,
      pRefl, ppError
    );

  UINT codeBlobIdx = DXBCParser.FindNextMatchingBlob(DXBC_GenericShaderEx);
  if (codeBlobIdx == DXBC_BLOB_NOT_FOUND) {
    codeBlobIdx = DXBCParser.FindNextMatchingBlob(DXBC_GenericShader);
  }
  if (codeBlobIdx == DXBC_BLOB_NOT_FOUND) {
    errorOut << "Invalid DXBC bytecode: shader blob not found\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }
  const void *codeBlob = DXBCParser.GetBlob(codeBlobIdx);

  CShaderToken *ShaderCode = (CShaderToken *)(BYTE *)codeBlob;
  // 1. Collect information about the shader.
  D3D10ShaderBinary::CShaderCodeParser CodeParser(ShaderCode);
  CSignatureParser inputParser;
  if (DXBCGetInputSignature(pBytecode, &inputParser) != S_OK) {
    errorOut << "Invalid DXBC bytecode: input signature not found\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }
  CSignatureParser5 outputParser;
  if (DXBCGetOutputSignature(pBytecode, &outputParser) != S_OK) {
    errorOut << "Invalid DXBC bytecode: output signature not found\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  auto sm50_shader = std::make_unique<SM50ShaderInternal>();
  sm50_shader->shader_type = CodeParser.ShaderType();
  if ((macrunner_fragment_translation_probe_enabled() &&
       sm50_shader->shader_type == microsoft::D3D10_SB_PIXEL_SHADER) ||
      (macrunner_causal_ladder_enabled() &&
       (sm50_shader->shader_type == microsoft::D3D10_SB_PIXEL_SHADER ||
        sm50_shader->shader_type == microsoft::D3D10_SB_VERTEX_SHADER))) {
    const auto *begin = static_cast<const uint8_t *>(pBytecode);
    sm50_shader->fragment_translation_probe_dxbc.assign(
        begin, begin + BytecodeSize);
  }
  auto shader_info = &(sm50_shader->shader_info);

  {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    inputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < inputParser.GetNumParameters(); ++i)
      sm50_shader->input_signature.push_back(Signature(parameters[i]));
    outputParser.RastSignature()->GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.RastSignature()->GetNumParameters(); i++) {
      sm50_shader->output_signature.push_back(Signature(parameters[i]));
    }
  }

  sm50_shader->bbs = read_control_flow(CodeParser, sm50_shader.get(), inputParser, outputParser);

  auto &binding_table = shader_info->binding_table;
  auto &binding_table_cbuffer = shader_info->binding_table_cbuffer;

  uint16_t binding_cbuffer_mask = 0;
  uint16_t binding_sampler_mask = 0;
  uint64_t binding_uav_mask = 0;
  uint64_t binding_srv_hi_mask = 0;
  uint64_t binding_srv_lo_mask = 0;

  for (auto &[range_id, cbv] : shader_info->cbufferMap) {
    // TODO: abstract SM 5.0 binding
    cbv.arg_index = binding_table_cbuffer.DefineBuffer(
      "cb" + std::to_string(range_id), AddressSpace::constant,
      MemoryAccess::read, msl_uint4,
      GetArgumentIndex(SM50BindingType::ConstantBuffer, range_id)
    );
    sm50_shader->args_reflection_cbuffer.push_back({
      .Type = SM50BindingType::ConstantBuffer,
      .SM50BindingSlot = range_id,
      .Flags =
        MTL_SM50_SHADER_ARGUMENT_BUFFER | MTL_SM50_SHADER_ARGUMENT_READ_ACCESS,
      .StructurePtrOffset = cbv.arg_index,
    });
    binding_cbuffer_mask |= (1 << range_id);
  }
  for (auto &[range_id, sampler] : shader_info->samplerMap) {
    // TODO: abstract SM 5.0 binding
    auto attr_index = GetArgumentIndex(SM50BindingType::Sampler, range_id);
    sampler.arg_index =
      binding_table.DefineSampler("s" + std::to_string(range_id), attr_index);
    sampler.arg_cube_index =
      binding_table.DefineSampler("cube_s" + std::to_string(range_id), attr_index + 1);
    sampler.arg_metadata_index = binding_table.DefineInteger64(
      "meta_s" + std::to_string(range_id), attr_index + 2
    );
    sm50_shader->args_reflection.push_back({
      .Type = SM50BindingType::Sampler,
      .SM50BindingSlot = range_id,
      .Flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0,
      .StructurePtrOffset = sampler.arg_index,
    });
    binding_sampler_mask |= (1 << range_id);
  }
  for (auto &[range_id, srv] : shader_info->srvMap) {
    auto attr_index = GetArgumentIndex(SM50BindingType::SRV, range_id);
    if (srv.resource_type != ResourceType::NonApplicable) {
      // TODO: abstract SM 5.0 binding
      auto access = srv.sampled ? MemoryAccess::sample : MemoryAccess::read;
      auto texture_kind = to_air_resource_type(srv.resource_type, srv.compared);
      auto scaler_type = to_air_scaler_type(srv.scaler_type);
      srv.arg_index = binding_table.DefineTexture(
        "t" + std::to_string(range_id), texture_kind, access, scaler_type,
        attr_index
      );
    } else {
      srv.arg_index = binding_table.DefineBuffer(
        "t" + std::to_string(range_id),
        AddressSpace::device, // it needs to be `device` since `constant` has
                              // size and alignment restriction
        MemoryAccess::read, msl_uint, attr_index
      );
    }
    srv.arg_metadata_index = binding_table.DefineInteger64(
      "meta_t" + std::to_string(range_id), attr_index + 1
    );
    MTL_SM50_SHADER_ARGUMENT_FLAG flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0;
    if (srv.resource_type == ResourceType::NonApplicable) {
      flags |= MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH |
               MTL_SM50_SHADER_ARGUMENT_BUFFER;
    } else if (srv.resource_type == ResourceType::TextureBuffer) {
      flags |= MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
    } else {
      flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
      switch (srv.resource_type) {
      case ResourceType::Texture1DArray:
      case ResourceType::Texture2DArray:
      case ResourceType::Texture2DMultisampledArray:
      case ResourceType::TextureCubeArray:
        flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
        break;
      default:
        break;
      }
    }
    if (srv.read || srv.sampled || srv.compared) {
      flags |= MTL_SM50_SHADER_ARGUMENT_READ_ACCESS;
    }
    sm50_shader->args_reflection.push_back({
      .Type = SM50BindingType::SRV,
      .SM50BindingSlot = range_id,
      .Flags = flags,
      .StructurePtrOffset = srv.arg_index,
    });
    if (range_id & 64) {
      binding_srv_hi_mask |= (1ULL << (range_id - 64));
    } else {
      binding_srv_lo_mask |= (1ULL << range_id);
    }
  }
  for (auto &[range_id, uav] : shader_info->uavMap) {
    auto access =
      uav.written ? (uav.read ? MemoryAccess::read_write : MemoryAccess::write)
                  : MemoryAccess::read;
    auto attr_index = GetArgumentIndex(SM50BindingType::UAV, range_id);
    if (uav.resource_type != ResourceType::NonApplicable) {
      auto texture_kind = to_air_resource_type(uav.resource_type);
      auto scaler_type = to_air_scaler_type(uav.scaler_type);
      uav.arg_index = binding_table.DefineTexture(
        "u" + std::to_string(range_id), texture_kind, access, scaler_type,
        attr_index, uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    } else {
      uav.arg_index = binding_table.DefineBuffer(
        "u" + std::to_string(range_id), AddressSpace::device, access, msl_uint,
        attr_index, uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    }
    uav.arg_metadata_index = binding_table.DefineInteger64(
      "meta_u" + std::to_string(range_id), attr_index + 1
    );
    if (uav.with_counter) {
      uav.arg_counter_index = binding_table.DefineBuffer(
        "counter" + std::to_string(range_id), AddressSpace::device,
        MemoryAccess::read_write, msl_uint, attr_index + 2,
        uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    }
    MTL_SM50_SHADER_ARGUMENT_FLAG flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0;
    if (uav.resource_type == ResourceType::NonApplicable) {
      flags |= MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH |
               MTL_SM50_SHADER_ARGUMENT_BUFFER;
    } else if (uav.resource_type == ResourceType::TextureBuffer) {
      flags |= MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
    } else {
      flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
      switch (uav.resource_type) {
      case ResourceType::Texture1DArray:
      case ResourceType::Texture2DArray:
      case ResourceType::Texture2DMultisampledArray:
      case ResourceType::TextureCubeArray:
        flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
        break;
      default:
        break;
      }
    }
    if (uav.read) {
      flags |= MTL_SM50_SHADER_ARGUMENT_READ_ACCESS;
    }
    if (uav.written) {
      flags |= MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS;
    }
    if (uav.with_counter) {
      flags |= MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER;
    }
    sm50_shader->args_reflection.push_back({
      .Type = SM50BindingType::UAV,
      .SM50BindingSlot = range_id,
      .Flags = flags,
      .StructurePtrOffset = uav.arg_index,
    });
    binding_uav_mask |= (1ULL << range_id);
  }

  if (sm50_shader->shader_type == microsoft::D3D11_SB_HULL_SHADER &&
      !sm50_shader->shader_info.no_control_point_phase_passthrough) {
    assert(sm50_shader->max_output_register == 0);
    uint32_t num_input_params = inputParser.GetNumParameters();
    D3D11_SIGNATURE_PARAMETER const * params;
    inputParser.GetParameters(&params);
    for (unsigned i = 0; i < num_input_params; i++) {
      sm50_shader->max_output_register = std::max(
        sm50_shader->max_output_register,
        params[i].Register + 1
      );
    }
  }

  if (pRefl) {
    pRefl->ConstanttBufferTableBindIndex =
      sm50_shader->args_reflection_cbuffer.size() > 0 ? 29 : ~0u;
    pRefl->ArgumentBufferBindIndex =
      sm50_shader->args_reflection.size() > 0 ? 30 : ~0u;
    pRefl->ConstantBufferSlotMask = binding_cbuffer_mask;
    pRefl->SamplerSlotMask = binding_sampler_mask;
    pRefl->SRVSlotMaskHi = binding_srv_hi_mask;
    pRefl->SRVSlotMaskLo = binding_srv_lo_mask;
    pRefl->UAVSlotMask = binding_uav_mask;
    pRefl->NumConstantBuffers = sm50_shader->args_reflection_cbuffer.size();
    pRefl->NumArguments = sm50_shader->args_reflection.size();
    if (sm50_shader->shader_type == microsoft::D3D11_SB_COMPUTE_SHADER) {
      pRefl->ThreadgroupSize[0] = sm50_shader->threadgroup_size[0];
      pRefl->ThreadgroupSize[1] = sm50_shader->threadgroup_size[1];
      pRefl->ThreadgroupSize[2] = sm50_shader->threadgroup_size[2];
    }
    if (sm50_shader->shader_type == microsoft::D3D11_SB_HULL_SHADER) {
      auto threads_per_patch = next_pow2(sm50_shader->hull_maximum_threads_per_patch);
      if (threads_per_patch > 32) {
        errorOut << "Threadgroup size of tessellation pipeline is too large.";
        SM50_RETURN_ERROR(errorObj, ppError, 1);
      }
      auto patch_per_group = 32 / threads_per_patch;
      float max_tesselation_factor = sm50_shader->max_tesselation_factor;

      while (estimate_payload_size(sm50_shader.get(), max_tesselation_factor, patch_per_group) > 16384) {
        if (patch_per_group == 1) {
          if (max_tesselation_factor > 1.0f) {
            max_tesselation_factor = std::max(max_tesselation_factor - 2.0f, 1.0f);
          } else {
            errorOut << "Payload size of tessellation pipeline is too large.";
            SM50_RETURN_ERROR(errorObj, ppError, 1);
          }
        } else {
          patch_per_group = patch_per_group >> 1;
        }
      }
      sm50_shader->hull_maximum_threads_per_patch = 32 / patch_per_group;
      sm50_shader->max_tesselation_factor = max_tesselation_factor;

      pRefl->ThreadsPerPatch = sm50_shader->hull_maximum_threads_per_patch;
      pRefl->Tessellator = {
        .Partition = sm50_shader->tessellation_partition,
        .MaxFactor = sm50_shader->max_tesselation_factor,
        .OutputPrimitive = (MTL_TESSELLATOR_OUTPUT_PRIMITIVE
        )sm50_shader->tessellator_output_primitive,
      };
    }
    if (sm50_shader->shader_type == microsoft::D3D11_SB_DOMAIN_SHADER) {
      uint32_t max_potential_tess_factor = 1;

      for (int tess_factor = 1; tess_factor <= 64; tess_factor++) {
        auto x = estimate_mesh_size(sm50_shader.get(), tess_factor);
        if (x > 32768)
          break;
        max_potential_tess_factor = tess_factor;
      }

      uint32_t max_split_factor = 0;
      for (uint32_t factor = 1; factor <= 64; ++factor) {
        const size_t vertices = (2 * (factor + 1) + 7) & ~7u;
        if (vertices * size_t(sm50_shader->max_output_register) * 16 > 32768)
          break;
        max_split_factor = factor;
      }
      pRefl->PostTessellator = {
          .MaxPotentialTessFactor = max_potential_tess_factor,
          .MaxPotentialSplitTessFactor = max_split_factor,
      };
    }
    if (sm50_shader->shader_type == microsoft::D3D10_SB_GEOMETRY_SHADER) {
      if (binding_cbuffer_mask || binding_sampler_mask || binding_uav_mask ||
          binding_srv_hi_mask || binding_srv_lo_mask ||
          !CheckGSBBIsPassThrough(sm50_shader->entry())) {
        pRefl->GeometryShader.GSPassThrough = ~0u;
      } else {
        CheckGSSignatureIsPassThrough(
          inputParser, outputParser, pRefl->GeometryShader.Data
        );
      }
      pRefl->GeometryShader.Primitive = sm50_shader->gs_input_primitive;
    }
    if (sm50_shader->shader_type == microsoft::D3D10_SB_PIXEL_SHADER) {
      pRefl->PixelShader.ValidRenderTargets = sm50_shader->pso_valid_output_reg_mask;
      pRefl->PixelShader.HasCoverageOutput = sm50_shader->ps_has_coverage_output;
    }
    pRefl->NumOutputElement = sm50_shader->max_output_register;
    pRefl->ArgumentTableQwords = binding_table.Size();
  }

  *ppShader = (sm50_shader_t)sm50_shader.release();
  return 0;
};

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
) {
  auto sm50_shader = (dxmt::dxbc::SM50ShaderInternal *)pShader;
  if (pConstantBuffers && sm50_shader->args_reflection_cbuffer.size())
    memcpy(
      pConstantBuffers, sm50_shader->args_reflection_cbuffer.data(),
      sm50_shader->args_reflection_cbuffer.size() *
        sizeof(struct MTL_SM50_SHADER_ARGUMENT)
    );
  if (pArguments && sm50_shader->args_reflection.size())
    memcpy(
      pArguments, sm50_shader->args_reflection.data(),
      sm50_shader->args_reflection.size() *
        sizeof(struct MTL_SM50_SHADER_ARGUMENT)
    );
}

AIRCONV_API int SM50GetDxilResourceBindings(
  sm50_shader_t pShader, struct SM50_DXIL_RESOURCE_BINDING *pBindings, uint32_t Capacity, uint32_t *pCount
) {
  if (!pCount) return 1;
  *pCount = 0;
  auto *shader = (dxmt::dxbc::SM50ShaderInternal *)pShader;
  if (!shader || !shader->dxil) return 1;
  // Direct heap indexing has no complete declared read set. Never turn an empty metadata table into known-empty.
  for (const auto &function : *shader->dxil)
    if (function.getName().startswith("dx.op.createHandleFromHeap") && !function.use_empty()) return 1;
  const auto &resources = shader->dxil_entry.resources;
  if (resources.size() > 1024 || (pBindings && Capacity < resources.size())) return 1;
  for (const auto &r : resources) {
    const auto type = uint32_t(r.resource_class);
    if (type > 3 || !r.range_size || r.range_size > 1024 ||
        uint64_t(r.lower_bound) + r.range_size - 1 > UINT32_MAX) return 1;
    if (r.resource_class == dxmt::dxil::ResourceClass::CBV && (!r.cbuffer_size || r.cbuffer_size > 65536)) return 1;
  }
  if (pBindings) {
    uint32_t i = 0;
    for (const auto &r : resources)
      pBindings[i++] = {uint32_t(r.resource_class), r.lower_bound, r.lower_bound + r.range_size - 1, r.space,
                       r.resource_class == dxmt::dxil::ResourceClass::CBV ? r.cbuffer_size : 0};
  }
  *pCount = uint32_t(resources.size());
  return 0;
}

AIRCONV_API void SM50Destroy(sm50_shader_t pShader) {
  delete (dxmt::dxbc::SM50ShaderInternal *)pShader;
}

AIRCONV_API int SM50Compile(
  sm50_shader_t pShader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;
  using namespace dxmt::dxbc;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = std::make_unique<SM50ErrorInternal>();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  LLVMContext context;

  context.setOpaquePointers(false);

  auto *shader_internal =
      static_cast<dxmt::dxbc::SM50ShaderInternal *>(pShader);
  auto &shader_info = shader_internal->shader_info;
  for (auto *arg = pArgs; arg; arg = static_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(arg->next)) {
    if (arg->type == SM50_SHADER_REUSE_AUDIT_QUERY) {
      *ppBitcode = nullptr;
      reuse_audit::Run(*shader_internal, *reinterpret_cast<SM50_SHADER_REUSE_AUDIT_DATA *>(arg));
      return 0;
    }
    if (arg->type == SM50_SHADER_POST_CS_VS_KEY_QUERY) {   // MacRunner POST-CS 25.10.2026: read-only query
      *ppBitcode = nullptr;
      post_cs_vertex_key(*shader_internal, *reinterpret_cast<SM50_SHADER_POST_CS_VS_KEY_DATA *>(arg));
      return 0;
    }
  }
  unsigned fragment_translation_probe_ordinal = 0;
  const bool fragment_translation_probe =
      macrunner_fragment_translation_probe_claim(
          shader_internal->shader_type, FunctionName,
          &fragment_translation_probe_ordinal);
  std::string fragment_translation_probe_stem;
  const SM50_SHADER_PSO_PIXEL_SHADER_DATA *pixel_pso = nullptr;
  for (auto *arg = pArgs; arg;
       arg = static_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(arg->next)) {
    if (arg->type == SM50_SHADER_PSO_PIXEL_SHADER) {
      pixel_pso = reinterpret_cast<const SM50_SHADER_PSO_PIXEL_SHADER_DATA *>(arg);
      break;
    }
  }

  if (fragment_translation_probe) {
    fragment_translation_probe_stem =
        macrunner_fragment_translation_probe_stem(FunctionName);
    const auto &dxbc = shader_internal->fragment_translation_probe_dxbc;
    macrunner_fragment_translation_write_bytes(
        fragment_translation_probe_stem + ".dxbc", dxbc.data(), dxbc.size());
    std::fprintf(
        stderr,
        "macrunner-hb-fragment-translation: side=airconv phase=compile-start "
        "ordinal=%u function=%s dxbc_bytes=%zu output_signature_count=%zu "
        "valid_rt_mask=0x%x max_output_register=%u sample_mask=0x%x "
        "unorm_output_reg_mask=0x%x dual_source_blending=%u "
        "disable_depth_output=%u\n",
        fragment_translation_probe_ordinal,
        FunctionName ? FunctionName : "(null)", dxbc.size(),
        shader_internal->output_signature.size(),
        shader_internal->pso_valid_output_reg_mask,
        shader_internal->max_output_register,
        pixel_pso ? pixel_pso->sample_mask : 0,
        pixel_pso ? pixel_pso->unorm_output_reg_mask : 0,
        pixel_pso ? static_cast<unsigned>(pixel_pso->dual_source_blending) : 0,
        pixel_pso ? static_cast<unsigned>(pixel_pso->disable_depth_output) : 0);
    for (const auto &signature : shader_internal->output_signature) {
      const auto semantic = signature.semanticName();
      std::fprintf(
          stderr,
          "macrunner-hb-fragment-translation: side=airconv "
          "phase=output-signature ordinal=%u function=%s semantic=%.*s "
          "semantic_index=%u reg=%u mask=0x%x component_type=%u "
          "system_value=%u\n",
          fragment_translation_probe_ordinal,
          FunctionName ? FunctionName : "(null)",
          static_cast<int>(semantic.size()), semantic.data(),
          signature.semanticIndex(), signature.reg(), signature.mask(),
          static_cast<unsigned>(signature.componentType()),
          static_cast<unsigned>(signature.systemValue()));
    }
  }

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = dxmt::dxbc::convertDXBC(
        pShader, FunctionName, context, *pModule, pArgs
      )) {
    errorOut << llvm::toString(std::move(err));
    if (fragment_translation_probe) {
      std::fprintf(
          stderr,
          "macrunner-hb-fragment-translation: side=airconv "
          "phase=compile-error ordinal=%u function=%s error=%.*s\n",
          fragment_translation_probe_ordinal,
          FunctionName ? FunctionName : "(null)",
          static_cast<int>(errorObj->buf.size()), errorObj->buf.data());
    }
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);

  if (fragment_translation_probe) {
    const auto return_type = macrunner_fragment_translation_return_type(
        *pModule, FunctionName);
    const auto path = fragment_translation_probe_stem + ".preopt.ll";
    const bool wrote =
        macrunner_fragment_translation_write_module(path, *pModule);
    std::fprintf(
        stderr,
        "macrunner-hb-fragment-translation: side=airconv phase=preopt "
        "ordinal=%u function=%s return_type=%s path=%s wrote=%u\n",
        fragment_translation_probe_ordinal,
        FunctionName ? FunctionName : "(null)", return_type.c_str(),
        path.c_str(), static_cast<unsigned>(wrote));
  }

  runOptimizationPasses(*pModule);

  if (fragment_translation_probe) {
    const auto return_type = macrunner_fragment_translation_return_type(
        *pModule, FunctionName);
    const auto path = fragment_translation_probe_stem + ".postopt.ll";
    const bool wrote =
        macrunner_fragment_translation_write_module(path, *pModule);
    std::fprintf(
        stderr,
        "macrunner-hb-fragment-translation: side=airconv phase=postopt "
        "ordinal=%u function=%s return_type=%s path=%s wrote=%u\n",
        fragment_translation_probe_ordinal,
        FunctionName ? FunctionName : "(null)", return_type.c_str(),
        path.c_str(), static_cast<unsigned>(wrote));
  }

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  if (fragment_translation_probe) {
    const auto path = fragment_translation_probe_stem + ".metallib";
    const bool wrote = macrunner_fragment_translation_write_bytes(
        path, compiled->vec.data(), compiled->vec.size());
    std::fprintf(
        stderr,
        "macrunner-hb-fragment-translation: side=airconv phase=complete "
        "ordinal=%u function=%s metallib_bytes=%zu path=%s wrote=%u\n",
        fragment_translation_probe_ordinal,
        FunctionName ? FunctionName : "(null)", compiled->vec.size(),
        path.c_str(), static_cast<unsigned>(wrote));
  }

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int SM50CompileTessellationPipelineHull(
  sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;
  using namespace dxmt::dxbc;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = std::make_unique<SM50ErrorInternal>();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  LLVMContext context;

  context.setOpaquePointers(false);

  auto &shader_info =
    ((dxmt::dxbc::SM50ShaderInternal *)pHullShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = dxmt::dxbc::convert_dxbc_vertex_hull_shader(
        (dxbc::SM50ShaderInternal *)pVertexShader, (dxbc::SM50ShaderInternal *)pHullShader, 
        FunctionName, context, *pModule, pHullShaderArgs
      )) {
    errorOut << llvm::toString(std::move(err));
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);
  linkTessellation(*pModule);

  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int SM50CompileTessellationPipelineDomain(
  sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;
  using namespace dxmt::dxbc;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = std::make_unique<SM50ErrorInternal>();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  LLVMContext context;

  context.setOpaquePointers(false);

  auto &shader_info =
    ((dxmt::dxbc::SM50ShaderInternal *)pDomainShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = dxmt::dxbc::convert_dxbc_tesselator_domain_shader(
        (dxbc::SM50ShaderInternal *)pDomainShader, FunctionName,
        (dxbc::SM50ShaderInternal *)pHullShader, context, *pModule,
        pDomainShaderArgs
      )) {
    errorOut << llvm::toString(std::move(err));
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);
  linkTessellation(*pModule);
  
  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int SM50CompileGeometryPipelineVertex(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;
  using namespace dxmt::dxbc;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = std::make_unique<SM50ErrorInternal>();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  LLVMContext context;

  context.setOpaquePointers(false);

  auto &shader_info = ((dxmt::dxbc::SM50ShaderInternal *)pGeometryShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = dxmt::dxbc::convert_dxbc_vertex_for_geometry_shader(
        (dxbc::SM50ShaderInternal *)pVertexShader, FunctionName,
        (dxbc::SM50ShaderInternal *)pGeometryShader, context, *pModule,
        pVertexShaderArgs
      )) {
    errorOut << llvm::toString(std::move(err));
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);

  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API int SM50CompileGeometryPipelineGeometry(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;
  using namespace dxmt::dxbc;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = std::make_unique<SM50ErrorInternal>();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  LLVMContext context;

  context.setOpaquePointers(false);

  auto &shader_info =
    ((dxmt::dxbc::SM50ShaderInternal *)pGeometryShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);

  if (auto err = dxmt::dxbc::convert_dxbc_geometry_shader(
        (dxbc::SM50ShaderInternal *)pGeometryShader, FunctionName,
        (dxbc::SM50ShaderInternal *)pVertexShader, context, *pModule,
        pGeometryShaderArgs
      )) {
    errorOut << llvm::toString(std::move(err));
    SM50_RETURN_ERROR(errorObj, ppError, 1);
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);
  
  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = new SM50CompiledBitcodeInternal();

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  writer.Write(*pModule, OS);

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled;
  return 0;
}

AIRCONV_API void SM50GetCompiledBitcode(
  sm50_bitcode_t pBitcode, SM50_COMPILED_BITCODE *pData
) {
  auto pBitcodeInternal = (SM50CompiledBitcodeInternal *)pBitcode;
  pData->Data = pBitcodeInternal->vec.data();
  pData->Size = pBitcodeInternal->vec.size();
}

AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode) {
  auto pBitcodeInternal = (SM50CompiledBitcodeInternal *)pBitcode;
  delete pBitcodeInternal;
}

AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) {
  auto pInternal = (dxmt::dxbc::SM50ErrorInternal *)pError;
  auto str_len = std::min(pInternal->buf.size(), BufferSize - 1);
  memcpy(pBuffer, pInternal->buf.data(), str_len);
  pBuffer[str_len] = '\0';
  return str_len;
}

AIRCONV_API void SM50FreeError(sm50_error_t pError) {
  if (pError == nullptr)
    return;
  auto pInternal = (dxmt::dxbc::SM50ErrorInternal *)pError;
  delete pInternal;
}
