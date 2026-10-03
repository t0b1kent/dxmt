/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d12_device.hpp"
#include "stage_linkage.hpp"
#include "d3d12_frame_trace.hpp"
#include "d3d12_diagnostic_counters.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_shader_capture.hpp"
#include "d3d12_gs_passthrough.hpp"
#include "dxmt_format.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "sha1/sha1_util.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include "d3d12_gs_point.hpp"
#include "d3d12_gs_contract_reader.hpp"
#include "../shared/point_root_contract.hpp"
#include <cmath>
#include <array>
#include <mutex>

namespace dxmt {

#include "d3d12_dxil_artifact.hpp"

static bool
PipelineCaptureEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_CAPTURE", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

static uint32_t
AdmitPipelineCapture() {
  if (!PipelineCaptureEnabled()) return 0;
  static std::atomic<uint32_t> attempts{0};
  uint32_t n = attempts.load(std::memory_order_relaxed);
  while (n < 2049)
    if (attempts.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) return n + 1;
  return 0;
}

template<typename... Args>
static void
TracePipelineCapture(uint32_t id, const void *owner, const char *op, const char *format, Args... args) {
  if (!id) return;
  const DWORD error = GetLastError();
  char detail[512], line[768];
  const int size = std::snprintf(detail, sizeof(detail), format, args...);
  if (size < 0 || size_t(size) >= sizeof(detail))
    std::snprintf(detail, sizeof(detail), "truncated=1 reason=format_limit");
  const int length = std::snprintf(line, sizeof(line), "dx12_pso_capture attempt=%u owner=%p op=%s %s\n", id, owner, op, detail);
  DWORD written;
  if (length > 0 && size_t(length) < sizeof(line))
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
  SetLastError(error);
}

static HRESULT
TraceUnsupportedPipeline(const char *function, unsigned line) {
  static std::atomic<uint32_t> trace{0};
  TraceFrame(trace, "pso.unsupported", nullptr, "function=%s line=%u hr=80004001", function, line);
  return E_NOTIMPL;
}

static void
TraceRejectedPipelineBytecode(uint32_t id, const void *owner, const char *stage, D3D12_SHADER_BYTECODE bytecode) {
  if (!id || !bytecode.pShaderBytecode || !bytecode.BytecodeLength) return;
  const DWORD error = GetLastError();
  constexpr size_t blob_limit = 256 * 1024, total_limit = 8 * 1024 * 1024;
  const char *skip = nullptr;
  microsoft::CDXBCParser parser;
  std::vector<uint8_t> inspection;
  if (bytecode.BytecodeLength > blob_limit) skip = "blob_limit";
  else if (!AlignDXBCForInspection(bytecode, inspection) ||
           FAILED(parser.ReadDXBC(inspection.data(), inspection.size()))) skip = "malformed";
  if (!skip) {
    static std::atomic<size_t> total{0};
    size_t used = total.load(std::memory_order_relaxed);
    while (true) {
      if (used > total_limit - bytecode.BytecodeLength) { skip = "total_limit"; break; }
      if (total.compare_exchange_weak(used, used + bytecode.BytecodeLength, std::memory_order_relaxed)) break;
    }
  }
  if (skip) {
    TracePipelineCapture(id, owner, "blob.skip", "stage=%s reason=%s length=%llu", stage, skip,
                         (unsigned long long)bytecode.BytecodeLength);
    SetLastError(error);
    return;
  }
  const auto digest = Sha1HashState::compute(bytecode.pShaderBytecode, bytecode.BytecodeLength);
  constexpr char hex[] = "0123456789abcdef";
  char hash[41] = {};
  for (unsigned i = 0; i < 20; ++i) { hash[2*i] = hex[digest.data[i] >> 4]; hash[2*i+1] = hex[digest.data[i] & 15]; }
  const char *container = parser.FindNextMatchingBlob(microsoft::DXBC_DXIL) != DXBC_BLOB_NOT_FOUND ? "DXIL" : "DXBC";
  TracePipelineCapture(id, owner, "blob.begin", "stage=%s length=%llu sha1=%s container=%s", stage,
                       (unsigned long long)bytecode.BytecodeLength, hash, container);
  // Bounded chunks share the existing run-owned stream; the extractor checks offsets and SHA1 before writing files.
  const auto *bytes = static_cast<const uint8_t *>(bytecode.pShaderBytecode);
  for (size_t offset = 0; offset < bytecode.BytecodeLength; offset += 128) {
    char encoded[257] = {};
    const size_t count = std::min<size_t>(128, bytecode.BytecodeLength - offset);
    for (size_t i = 0; i < count; ++i) { encoded[2*i] = hex[bytes[offset+i] >> 4]; encoded[2*i+1] = hex[bytes[offset+i] & 15]; }
    TracePipelineCapture(id, owner, "blob.chunk", "stage=%s offset=%llu hex=%s", stage, (unsigned long long)offset, encoded);
  }
  TracePipelineCapture(id, owner, "blob.end", "stage=%s length=%llu sha1=%s", stage,
                       (unsigned long long)bytecode.BytecodeLength, hash);
  SetLastError(error);
}

void
TraceRejectedComputeBytecode(const void *owner, D3D12_SHADER_BYTECODE bytecode, HRESULT result) {
  if (!PipelineCaptureEnabled() || !bytecode.pShaderBytecode || !bytecode.BytecodeLength) return;
  const DWORD error = GetLastError();
  // Keep repeated failed PSOs from exhausting the shared capture budget before graphics initialization.
  static std::mutex mutex;
  static std::array<std::array<uint8_t, 20>, 128> seen{};
  static size_t count = 0, bytes = 0;
  static bool limit_reported = false;
  bool capture = false, report_limit = false;
  if (bytecode.BytecodeLength <= 256 * 1024) {
    const auto hash = Sha1HashState::compute(bytecode.pShaderBytecode, bytecode.BytecodeLength);
    std::lock_guard<std::mutex> lock(mutex);
    bool duplicate = false;
    for (size_t i = 0; i < count; ++i)
      duplicate |= std::memcmp(seen[i].data(), hash.data, 20) == 0;
    if (!duplicate) {
      if (count == seen.size() || bytecode.BytecodeLength > 2 * 1024 * 1024 - bytes) {
        report_limit = !limit_reported;
        limit_reported = true;
      } else {
        std::memcpy(seen[count++].data(), hash.data, 20);
        bytes += bytecode.BytecodeLength;
        capture = true;
      }
    }
  } else {
    std::lock_guard<std::mutex> lock(mutex);
    report_limit = !limit_reported;
    limit_reported = true;
  }
  if (capture || report_limit) {
    const uint32_t id = AdmitPipelineCapture();
    TracePipelineCapture(id, owner, "compute.result", "hr=%08x", unsigned(result));
    if (capture) TraceRejectedPipelineBytecode(id, owner, "CS", bytecode);
    else TracePipelineCapture(id, owner, "blob.skip", "stage=CS reason=compute_limit length=%llu",
                              (unsigned long long)bytecode.BytecodeLength);
  }
  SetLastError(error);
}

HRESULT
LoadDiagnosticComputeArtifact(const void *owner, D3D12_SHADER_BYTECODE original,
                              std::vector<uint8_t> &storage, D3D12_SHADER_BYTECODE &result) {
  const DWORD error = GetLastError();
  const HRESULT hr = LoadDiagnosticDXILArtifact(original, 5, storage, result);
  if (hr != S_FALSE && PipelineCaptureEnabled()) {
    static std::atomic<unsigned> count{0};
    if (count.fetch_add(1, std::memory_order_relaxed) < 128)
      TracePipelineCapture(AdmitPipelineCapture(), owner, "compute.dxil_artifact", "hr=%08x bytes=%llu",
                           unsigned(hr), (unsigned long long)result.BytecodeLength);
  }
  SetLastError(error);
  return hr;
}

constexpr WMTCompareFunction kCompareFunctionMap[] = {
    WMTCompareFunctionNever, // padding 0
    WMTCompareFunctionNever, // 1 - 1
    WMTCompareFunctionLess,    WMTCompareFunctionEqual,    WMTCompareFunctionLessEqual,
    WMTCompareFunctionGreater, WMTCompareFunctionNotEqual, WMTCompareFunctionGreaterEqual,
    WMTCompareFunctionAlways // 8 - 1
};

constexpr WMTStencilOperation kStencilOperationMap[] = {
    WMTStencilOperationZero, // invalid
    WMTStencilOperationKeep,
    WMTStencilOperationZero,
    WMTStencilOperationReplace,
    // D3D11_STENCIL_OP_INCR_SAT: Increment the stencil value by 1, and clamp
    // the result.
    WMTStencilOperationIncrementClamp,
    WMTStencilOperationDecrementClamp,
    WMTStencilOperationInvert,
    // D3D11_STENCIL_OP_INCR:Increment the stencil value by 1, and wrap the
    // result if necessary.

    WMTStencilOperationIncrementWrap,
    WMTStencilOperationDecrementWrap,

};

constexpr WMTBlendOperation kBlendOpMap[] = {
    WMTBlendOperationAdd, // padding 0
    WMTBlendOperationAdd, WMTBlendOperationSubtract, WMTBlendOperationReverseSubtract,
    WMTBlendOperationMin, WMTBlendOperationMax,
};

constexpr WMTLogicOperation kLogicOpMap[] = {
    WMTLogicOperationClear,        WMTLogicOperationSet,         WMTLogicOperationCopy,
    WMTLogicOperationCopyInverted, WMTLogicOperationNoOp,        WMTLogicOperationInvert,
    WMTLogicOperationAnd,          WMTLogicOperationNand,        WMTLogicOperationOr,
    WMTLogicOperationNor,          WMTLogicOperationXor,         WMTLogicOperationEquiv,
    WMTLogicOperationAndReverse,   WMTLogicOperationAndInverted, WMTLogicOperationOrReverse,
    WMTLogicOperationOrInverted,
};

constexpr WMTBlendFactor kBlendFactorMap[] = {
    WMTBlendFactorZero, // padding 0
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,       // invalid,12
    WMTBlendFactorZero,       // invalid,13
    WMTBlendFactorBlendColor, // BLEND_FACTOR
    WMTBlendFactorOneMinusBlendColor,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
    WMTBlendFactorBlendAlpha,
    WMTBlendFactorOneMinusBlendAlpha,
};

constexpr WMTBlendFactor kBlendAlphaFactorMap[] = {
    WMTBlendFactorZero, // padding 0
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,       // invalid,12
    WMTBlendFactorZero,       // invalid,13
    WMTBlendFactorBlendAlpha, // BLEND_FACTOR
    WMTBlendFactorOneMinusBlendAlpha,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
};

constexpr WMTColorWriteMask kColorWriteMaskMap[] = {
    // 0000
    WMTColorWriteMaskNone,
    // 0001
    WMTColorWriteMaskRed,
    // 0010
    WMTColorWriteMaskGreen,
    // 0011,
    WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    // 0100
    WMTColorWriteMaskBlue,
    // 0101
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    // 0110
    WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    // 0111
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,

    // 1000
    WMTColorWriteMaskAlpha,
    // 1001
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed,
    // 1010
    WMTColorWriteMaskAlpha | WMTColorWriteMaskGreen,
    // 1011,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    // 1100
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue,
    // 0101
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    // 1110
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    // 1111
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
};

HRESULT
ExtractMTLInputLayoutElements(
    MTLD3D12Device *device, const void *pShaderBytecodeWithInputSignature,
    const D3D12_INPUT_ELEMENT_DESC *pInputElementDescs, uint32_t NumElements, SM50_IA_INPUT_ELEMENT *pInputLayout,
    uint32_t *pNumElementsOut
) {

  using namespace microsoft;
  uint16_t append_offset[32] = {0};
  uint32_t register_mask = 0;

  CSignatureParser parser;
  HRESULT hr = DXBCGetInputSignature(pShaderBytecodeWithInputSignature, &parser);
  if (FAILED(hr)) {
    return hr;
  }
  const D3D11_SIGNATURE_PARAMETER *pParameters;
  auto num_parameters = parser.GetParameters(&pParameters);

  UINT attribute_count = 0;
  for (UINT j = 0; j < NumElements; j++) {
    auto &desc = pInputElementDescs[j];

    MTL_DXGI_FORMAT_DESC metal_format;
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), desc.Format, metal_format))) {
      ERR("CreateInputLayout: Unsupported vertex format: ", desc.Format);
      return E_FAIL;
    }

    if (!metal_format.AttributeFormat) {
      ERR("CreateInputLayout: Unsupported vertex format: ", desc.Format);
      return E_INVALIDARG;
    }
    if (!metal_format.BytesPerTexel) {
      ERR("CreateInputLayout: not an ordinary or packed format: ", desc.Format);
      return E_INVALIDARG;
    }
    auto aligned_byte_offset = desc.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT
                                   ? align(append_offset[desc.InputSlot], std::min(4u, metal_format.BytesPerTexel))
                                   : desc.AlignedByteOffset;
    append_offset[desc.InputSlot] = aligned_byte_offset + metal_format.BytesPerTexel;

    auto pSig = std::find_if(pParameters, pParameters + num_parameters, [&](const D3D11_SIGNATURE_PARAMETER &inputSig) {
      return desc.SemanticIndex == inputSig.SemanticIndex && strcasecmp(desc.SemanticName, inputSig.SemanticName) == 0;
    });
    if (pSig == pParameters + num_parameters)
      continue; // shader has no such input register, so skip it
    auto &inputSig = *pSig;
    auto &attribute = pInputLayout[attribute_count++];

    attribute.format = metal_format.AttributeFormat;

    attribute.slot = desc.InputSlot;
    attribute.reg = inputSig.Register;
    attribute.aligned_byte_offset = aligned_byte_offset;
    // the layout stride is provided in IASetVertexBuffer
    attribute.step_function = desc.InputSlotClass;
    attribute.step_rate =
        desc.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? desc.InstanceDataStepRate : 1;
    register_mask |= (1 << inputSig.Register);
  }
  for (UINT i = 0; i < num_parameters; i++) {
    auto &inputSig = pParameters[i];
    if (inputSig.SystemValue != D3D10_SB_NAME_UNDEFINED)
      continue; // ignore SIV & SGV
    if (!(register_mask & (1 << inputSig.Register))) {
      WARN(
          "CreateInputLayout: Vertex shader expects ", inputSig.SemanticName, "_", inputSig.SemanticIndex,
          " but it's not in input layout element descriptors"
      );
      return E_INVALIDARG;
    }
  }
  *pNumElementsOut = attribute_count;

  return S_OK;
}

HRESULT
MTLD3D12PipelineState::InitializeShader(
    D3D12_SHADER_BYTECODE Bytecode, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl
) {
  using namespace microsoft;
  HRESULT hr;
  CDXBCParser Parser;
  if (FAILED(hr = Parser.ReadDXBC(Bytecode.pShaderBytecode, Bytecode.BytecodeLength)))
    return hr;

  if (Parser.FindNextMatchingBlob(microsoft::DXBC_DXIL) != DXBC_BLOB_NOT_FOUND)
    return TraceUnsupportedPipeline(__func__, __LINE__);

  SM50Error error;
  if (SM50Initialize(Bytecode.pShaderBytecode, Bytecode.BytecodeLength, ppShader, pRefl, &error)) {
    ERR("Failed to initialize shader: ", SM50GetErrorMessageString(error));
    return E_FAIL;
  }

  return S_OK;
}

namespace {

struct TessellationStageDeclarations {
  uint32_t input_control_points = 0;
  uint32_t output_control_points = 0;
  uint32_t domain = 0;
  uint32_t partition = 0;
  uint32_t output_primitive = 0;
  float max_factor = 0;
  uint32_t patch_scalars = 0;
  uint32_t patch_registers = 0;
};

// Retain the Sol073 domain-specific correction in the common integration fork.
bool
TessellationFactorSemanticMatchesDomain(uint32_t domain, uint32_t semantic) {
  using namespace microsoft;
  switch (domain) {
  case D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
    return semantic >= D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR &&
        semantic <= D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR;
  case D3D11_SB_TESSELLATOR_DOMAIN_TRI:
    return semantic >= D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR &&
        semantic <= D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR;
  case D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
    return semantic == D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR ||
        semantic == D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR;
  default: return false;
  }
}

// Reflection does not expose domains/control-point counts, and SM50Initialize
// may lower MaxFactor to fit its payload. Keep the original declarations too.
HRESULT
ReadTessellationStage(
    D3D12_SHADER_BYTECODE bytecode, microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE stage,
    TessellationStageDeclarations &declarations, bool require_sm50 = false
) {
  using namespace microsoft;
  if (!bytecode.pShaderBytecode || !bytecode.BytecodeLength)
    return E_INVALIDARG;
  CDXBCParser parser;
  HRESULT hr = parser.ReadDXBC(bytecode.pShaderBytecode, bytecode.BytecodeLength);
  if (FAILED(hr))
    return hr;
  if (parser.FindNextMatchingBlob(DXBC_DXIL) != DXBC_BLOB_NOT_FOUND)
    return TraceUnsupportedPipeline(__func__, __LINE__);
  auto code = parser.FindNextMatchingBlob(DXBC_GenericShaderEx);
  if (code == DXBC_BLOB_NOT_FOUND)
    code = parser.FindNextMatchingBlob(DXBC_GenericShader);
  if (code == DXBC_BLOB_NOT_FOUND)
    return E_INVALIDARG;
  const auto bytes = parser.GetBlobSize(code);
  if (bytes < 2 * sizeof(uint32_t) || bytes % sizeof(uint32_t))
    return E_INVALIDARG;
  std::vector<uint32_t> words(bytes / sizeof(uint32_t));
  memcpy(words.data(), parser.GetBlob(code), bytes);
  if (words[1] != words.size() || DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(words[0]) != stage)
    return E_INVALIDARG;
  if (DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(words[0]) != 5 ||
      DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(words[0]) > (require_sm50 ? 0u : 1u))
    return TraceUnsupportedPipeline(__func__, __LINE__);

  unsigned hull_phase = 0;
  bool hull_declarations = false, explicit_control_points = false;
  D3D10_SB_OPCODE_TYPE previous_op = D3D10_SB_OPCODE_RESERVED0;
  for (size_t at = 2; at < words.size();) {
    const auto token = words[at];
    const auto op = DECODE_D3D10_SB_OPCODE_TYPE(token);
    // Guard the donor's unchecked g_InstructionInfo[op] lookup.
    if (op >= D3D10_SB_NUM_OPCODES || op == D3D10_SB_OPCODE_RESERVED0 ||
        op == D3D10_1_SB_OPCODE_RESERVED1 || op == D3D11_SB_OPCODE_RESERVED0 ||
        op == D3D11_1_SB_OPCODE_RESERVED0 || op == D3DWDDM1_3_SB_OPCODE_RESERVED0)
      return E_INVALIDARG;
    auto length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
    if (op == D3D10_SB_OPCODE_CUSTOMDATA) {
      if (words.size() - at < 2 || words[at + 1] < 2)
        return E_INVALIDARG;
      length = words[at + 1];
    }
    if (!length || length > words.size() - at)
      return E_INVALIDARG;
    uint32_t *field = nullptr;
    uint32_t value = 0;
    switch (op) {
    case D3D11_SB_OPCODE_HS_DECLS:
      if (stage != D3D11_SB_HULL_SHADER || at != 2 || length != 1 || hull_declarations ||
          (token & 0x80fff800u)) return E_INVALIDARG;
      hull_declarations = true;
      break;
    case D3D11_SB_OPCODE_HS_CONTROL_POINT_PHASE:
    case D3D11_SB_OPCODE_HS_FORK_PHASE:
    case D3D11_SB_OPCODE_HS_JOIN_PHASE:
      if (stage != D3D11_SB_HULL_SHADER || !hull_declarations || length != 1 || (token & 0x80fff800u) ||
          (hull_phase && previous_op != D3D10_SB_OPCODE_RET)) return E_INVALIDARG;
      if (op == D3D11_SB_OPCODE_HS_CONTROL_POINT_PHASE) {
        if (hull_phase || explicit_control_points) return E_INVALIDARG;
        explicit_control_points = true; hull_phase = 1;
      } else {
        if (hull_phase == 3 && op == D3D11_SB_OPCODE_HS_FORK_PHASE) return E_INVALIDARG;
        hull_phase = op == D3D11_SB_OPCODE_HS_FORK_PHASE ? 2 : 3;
      }
      break;
    case D3D10_SB_OPCODE_DCL_OUTPUT:
    case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
      if (stage == D3D11_SB_HULL_SHADER && hull_phase >= 2) {
        // Mirror the donor's scalar list, including declarations repeated in phases.
        const bool siv = op == D3D10_SB_OPCODE_DCL_OUTPUT_SIV;
        if (length != (siv ? 4u : 3u) || (token & 0x80000000u)) return E_NOTIMPL;
        const auto operand = words[at + 1], mask = (operand >> 4) & 15u;
        if (!mask || operand != (0x00102002u | (mask << 4)) || words[at + 2] >= 32) return E_NOTIMPL;
        if (siv && ((mask & (mask - 1)) ||
            !TessellationFactorSemanticMatchesDomain(declarations.domain, words[at + 3]))) return E_NOTIMPL;
        for (unsigned bit = 0; bit < 4; ++bit) declarations.patch_scalars += (mask >> bit) & 1u;
        if (declarations.patch_scalars > 128) return E_NOTIMPL;
        declarations.patch_registers = std::max(declarations.patch_registers, words[at + 2] + 1);
      }
      break;
    case D3D10_SB_OPCODE_DCL_OUTPUT_SGV:
      if (stage == D3D11_SB_HULL_SHADER) return E_NOTIMPL;
      break;
    case D3D11_SB_OPCODE_DCL_INPUT_CONTROL_POINT_COUNT:
      field = &declarations.input_control_points;
      value = DECODE_D3D11_SB_INPUT_CONTROL_POINT_COUNT(token);
      if (value > 32) return E_INVALIDARG;
      break;
    case D3D11_SB_OPCODE_DCL_OUTPUT_CONTROL_POINT_COUNT:
      if (stage != D3D11_SB_HULL_SHADER) return E_INVALIDARG;
      field = &declarations.output_control_points;
      value = DECODE_D3D11_SB_OUTPUT_CONTROL_POINT_COUNT(token);
      if (value > 32) return E_INVALIDARG;
      break;
    case D3D11_SB_OPCODE_DCL_TESS_DOMAIN:
      field = &declarations.domain;
      value = DECODE_D3D11_SB_TESS_DOMAIN(token);
      if (value != D3D11_SB_TESSELLATOR_DOMAIN_TRI && value != D3D11_SB_TESSELLATOR_DOMAIN_QUAD &&
          value != D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      break;
    case D3D11_SB_OPCODE_DCL_TESS_PARTITIONING:
      if (stage != D3D11_SB_HULL_SHADER) return E_INVALIDARG;
      field = &declarations.partition;
      value = DECODE_D3D11_SB_TESS_PARTITIONING(token);
      // The donor's pow2 conversion truncates fractional maxima; do not inherit it.
      if (value != D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER &&
          value != D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD &&
          value != D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      break;
    case D3D11_SB_OPCODE_DCL_TESS_OUTPUT_PRIMITIVE:
      if (stage != D3D11_SB_HULL_SHADER) return E_INVALIDARG;
      field = &declarations.output_primitive;
      value = DECODE_D3D11_SB_TESS_OUTPUT_PRIMITIVE(token);
      if (value != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
          value != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW && value != D3D11_SB_TESSELLATOR_OUTPUT_LINE)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      break;
    case D3D11_SB_OPCODE_DCL_HS_MAX_TESSFACTOR:
      if (stage != D3D11_SB_HULL_SHADER || hull_phase || length != 2 || declarations.max_factor || (token & 0x80000000u))
        return E_INVALIDARG;
      memcpy(&declarations.max_factor, &words[at + 1], sizeof(float));
      if (!std::isfinite(declarations.max_factor) || declarations.max_factor < 1 || declarations.max_factor > 64)
        return E_INVALIDARG;
      break;
    case D3D11_SB_OPCODE_DCL_HS_FORK_PHASE_INSTANCE_COUNT:
    case D3D11_SB_OPCODE_DCL_HS_JOIN_PHASE_INSTANCE_COUNT:
      if (stage != D3D11_SB_HULL_SHADER || length != 2 || !words[at + 1] || (token & 0x80000000u))
        return E_INVALIDARG;
      if (hull_phase != (op == D3D11_SB_OPCODE_DCL_HS_FORK_PHASE_INSTANCE_COUNT ? 2u : 3u)) return E_INVALIDARG;
      if (words[at + 1] > 32) return TraceUnsupportedPipeline(__func__, __LINE__);
      break;
    default:
      break;
    }
    if (field) {
      if ((stage != D3D11_SB_HULL_SHADER && stage != D3D11_SB_DOMAIN_SHADER) ||
          (stage == D3D11_SB_HULL_SHADER && hull_phase) ||
          length != 1 || (token & 0x80000000u) || *field || !value)
        return E_INVALIDARG;
      *field = value;
    }
    if (stage == D3D11_SB_HULL_SHADER && !hull_declarations) return E_INVALIDARG;
    previous_op = op;
    at += length;
  }
  if (stage == D3D11_SB_HULL_SHADER &&
      (!declarations.input_control_points || !declarations.output_control_points || !declarations.domain ||
       !declarations.partition || !declarations.output_primitive || !declarations.max_factor ||
       !hull_phase || previous_op != D3D10_SB_OPCODE_RET ||
       (!explicit_control_points && declarations.input_control_points != declarations.output_control_points)))
    return E_INVALIDARG;
  if (stage == D3D11_SB_DOMAIN_SHADER && (!declarations.input_control_points || !declarations.domain))
    return E_INVALIDARG;
  if (stage == D3D11_SB_HULL_SHADER &&
      ((declarations.domain == D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE) !=
       (declarations.output_primitive == D3D11_SB_TESSELLATOR_OUTPUT_LINE)))
    return E_INVALIDARG;
  return S_OK;
}

// DXILTS01 is an offline-validated metadata witness, bound to BOTH container digests.
// It is not an in-driver LLVM parser or an authentication boundary for untrusted caches.
HRESULT
ValidateTessellationArtifactContract(D3D12_SHADER_BYTECODE original, D3D12_SHADER_BYTECODE target, unsigned stage) {
  const DWORD saved_error = GetLastError();
  struct LastError { DWORD value; ~LastError() { SetLastError(value); } } preserve{saved_error};
  char directory[4096];
  const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_DXIL_ARTIFACT_DIR", directory, sizeof(directory));
  if (!n || n >= sizeof(directory)) return E_INVALIDARG;
  const auto source_hash = Sha1HashState::compute(original.pShaderBytecode, original.BytecodeLength);
  const auto target_hash = Sha1HashState::compute(target.pShaderBytecode, target.BytecodeLength);
  char key[41]; constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 20; ++i) { key[i * 2] = digits[source_hash.data[i] >> 4]; key[i * 2 + 1] = digits[source_hash.data[i] & 15]; }
  key[40] = 0;
  const auto file_name = std::string(directory) + "\\" + key + ".dxil-tess";
  HANDLE file = CreateFileA(file_name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return E_NOTIMPL;
  std::array<uint8_t, 80> bytes{}; LARGE_INTEGER size{}; DWORD read = 0;
  const bool complete = GetFileSizeEx(file, &size) && size.QuadPart == bytes.size() &&
      ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size();
  CloseHandle(file);
  const auto u32 = [&](unsigned at) { uint32_t v; memcpy(&v, bytes.data() + at, 4); return v; };
  if (!complete || memcmp(bytes.data(), "DXILTS01", 8) || u32(8) != 1 || u32(12) != stage ||
      memcmp(bytes.data() + 16, source_hash.data, 20) || memcmp(bytes.data() + 36, target_hash.data, 20)) return E_INVALIDARG;
  TessellationStageDeclarations d;
  const HRESULT hr = ReadTessellationStage(target, microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE(stage), d, true);
  if (FAILED(hr)) return hr;
  if (u32(56) != d.input_control_points || u32(60) != d.output_control_points || u32(64) != d.domain ||
      u32(68) != d.partition || u32(72) != d.output_primitive || memcmp(bytes.data() + 76, &d.max_factor, 4)) return E_INVALIDARG;
  return S_OK;
}


HRESULT
ValidatePointRootResources(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
  using namespace microsoft;
  if (!desc.pRootSignature) return E_INVALIDARG;
  const void *blob = nullptr, *raw = nullptr; UINT raw_size = 0;
  static_cast<MTLD3D12RootSignature *>(desc.pRootSignature)->GetBlob(&blob);
  RootSignatureDeserializer root;
  if (FAILED(DXBCGetRootSignature(blob, &raw, &raw_size)) || FAILED(root.Deserialize(raw, raw_size))) return E_INVALIDARG;
  const D3D12_SHADER_BYTECODE stages[] = {desc.VS,desc.GS,desc.PS};
  const D3D12_SHADER_VISIBILITY visibility[] = {D3D12_SHADER_VISIBILITY_VERTEX,D3D12_SHADER_VISIBILITY_GEOMETRY,D3D12_SHADER_VISIBILITY_PIXEL};
  for (unsigned i=0;i<3;++i) {
    CDXBCParser parser;
    if (FAILED(parser.ReadDXBC(stages[i].pShaderBytecode,stages[i].BytecodeLength))) return E_INVALIDARG;
    const auto at=parser.FindNextMatchingBlob(static_cast<DXBCFourCC>(0x46454452u));
    if (at == DXBC_BLOB_NOT_FOUND || !point_root::rdef(root.desc_1_1_.Desc_1_1,visibility[i],
        static_cast<const uint8_t *>(parser.GetBlob(at)),parser.GetBlobSize(at))) return E_NOTIMPL;
  }
  return S_OK;
}

// Like DXILTS01, this metadata witness is generated only after offline compiler,
// original-source ABI and resource verification; it is not a shader substitute.
HRESULT
ValidatePointGeometryArtifact(D3D12_SHADER_BYTECODE original, D3D12_SHADER_BYTECODE target, gs_point::Contract &contract) {
  using namespace microsoft;
  const DWORD saved_error = GetLastError();
  struct LastError { DWORD value; ~LastError() { SetLastError(value); } } preserve{saved_error};
  const auto status = gs_contract_reader::Read(
      {static_cast<const uint8_t *>(target.pShaderBytecode), target.BytecodeLength}, contract);
  if (status != gs_contract_reader::Status::Success)
    return status == gs_contract_reader::Status::Unsupported ? E_NOTIMPL : E_INVALIDARG;
  char directory[4096];
  const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_DXIL_ARTIFACT_DIR", directory, sizeof(directory));
  if (!n || n >= sizeof(directory)) return E_INVALIDARG;
  const auto source_hash = Sha1HashState::compute(original.pShaderBytecode, original.BytecodeLength);
  const auto target_hash = Sha1HashState::compute(target.pShaderBytecode, target.BytecodeLength);
  char key[41]; constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 20; ++i) { key[i*2] = digits[source_hash.data[i] >> 4]; key[i*2+1] = digits[source_hash.data[i] & 15]; }
  key[40] = 0;
  const auto name = std::string(directory) + "\\" + key + ".dxil-gs";
  HANDLE file = CreateFileA(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return E_NOTIMPL;
  std::array<uint8_t, 136> bytes{}; LARGE_INTEGER length{}; DWORD read = 0;
  const bool complete = GetFileSizeEx(file, &length) && length.QuadPart == bytes.size() &&
      ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size();
  CloseHandle(file);
  return complete && gs_point::witness(bytes.data(), bytes.size(), source_hash.data, target_hash.data, contract) ? S_OK : E_INVALIDARG;
}

bool
TessellationSignaturesMatch(const microsoft::CSignatureParser &output, const microsoft::CSignatureParser &input) {
  using namespace microsoft;
  const D3D11_SIGNATURE_PARAMETER *out, *in;
  const auto num_out = output.GetParameters(&out), num_in = input.GetParameters(&in);
  for (UINT i = 0; i < num_in; ++i) {
    const auto &p = in[i];
    if (p.Register >= 32 || !p.Mask || (p.Mask & ~15u) || p.Stream ||
        p.MinPrecision != D3D_MIN_PRECISION_DEFAULT)
      return false;
    bool matched = false;
    for (UINT j = 0; j < num_out; ++j) {
      const auto &o = out[j];
      if (o.Register == p.Register && !(p.Mask & ~o.Mask) && !(o.NeverWrites_Mask & p.Mask) &&
          o.ComponentType == p.ComponentType && o.SystemValue == p.SystemValue &&
          o.MinPrecision == p.MinPrecision && !o.Stream && o.SemanticIndex == p.SemanticIndex &&
          !strcasecmp(o.SemanticName, p.SemanticName))
        matched = true;
    }
    if (!matched)
      return false;
  }
  return true;
}

HRESULT
BuildTessellationVertexLink(
    const microsoft::CSignatureParser &output, const microsoft::CSignatureParser &input,
    stage_linkage::VertexHullPlan &plan
) {
  const auto rows = [](const microsoft::CSignatureParser &signature) {
    std::vector<stage_linkage::Row> result;
    const microsoft::D3D11_SIGNATURE_PARAMETER *parameters;
    const auto count = signature.GetParameters(&parameters);
    if (count > 32) return result;
    for (UINT i = 0; i < count; ++i) {
      const auto &p = parameters[i];
      result.push_back({p.SemanticName ? p.SemanticName : "", p.SemanticIndex, uint32_t(p.SystemValue),
          uint32_t(p.ComponentType), p.Register, p.Mask, p.AlwaysReads_Mask, p.Stream, uint32_t(p.MinPrecision)});
    }
    return result;
  };
  return stage_linkage::vertex_hull(rows(output), rows(input), plan) ? S_OK : E_INVALIDARG;
}

HRESULT
ValidateTessellationSignatures(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
  using namespace microsoft;
  CSignatureParser vs, hs_in, hs_out, ds_in, hs_patch, ds_patch;
  if (FAILED(DXBCGetOutputSignature(desc.VS.pShaderBytecode, &vs)) ||
      FAILED(DXBCGetInputSignature(desc.HS.pShaderBytecode, &hs_in)) ||
      FAILED(DXBCGetOutputSignature(desc.HS.pShaderBytecode, &hs_out)) ||
      FAILED(DXBCGetInputSignature(desc.DS.pShaderBytecode, &ds_in)) ||
      FAILED(DXBCGetPatchConstantSignature(desc.HS.pShaderBytecode, &hs_patch)) ||
      FAILED(DXBCGetPatchConstantSignature(desc.DS.pShaderBytecode, &ds_patch)))
    return E_INVALIDARG;
  stage_linkage::VertexHullPlan vertex_link;
  if (FAILED(BuildTessellationVertexLink(vs, hs_in, vertex_link)) || !TessellationSignaturesMatch(hs_out, ds_in) ||
      !TessellationSignaturesMatch(hs_patch, ds_patch))
    return TraceUnsupportedPipeline(__func__, __LINE__);
  return S_OK;
}

bool
TessellationPatchSemanticsMatch(const microsoft::CSignatureParser &source, const microsoft::CSignatureParser &target, bool output) {
  using namespace microsoft;
  const D3D11_SIGNATURE_PARAMETER *a, *b;
  const auto na = source.GetParameters(&a), nb = target.GetParameters(&b);
  if (!na || na > 32 || na != nb) return false;
  const auto count = [](unsigned mask) { unsigned n = 0; for (; mask; mask >>= 1) n += mask & 1u; return n; };
  for (UINT i = 0; i < na; ++i) {
    if (!a[i].Mask || (a[i].Mask & ~15u) || a[i].Stream || a[i].MinPrecision != D3D_MIN_PRECISION_DEFAULT) return false;
    for (UINT k = 0; k < i; ++k)
      if (!strcasecmp(a[i].SemanticName, a[k].SemanticName) && a[i].SemanticIndex == a[k].SemanticIndex) return false;
    unsigned matches = 0;
    for (UINT j = 0; j < nb; ++j) {
      if (!b[j].Mask || (b[j].Mask & ~15u) || b[j].Stream || b[j].MinPrecision != D3D_MIN_PRECISION_DEFAULT) return false;
      if (strcasecmp(a[i].SemanticName, b[j].SemanticName) || a[i].SemanticIndex != b[j].SemanticIndex) continue;
      if (a[i].SystemValue != b[j].SystemValue || a[i].ComponentType != b[j].ComponentType || count(a[i].Mask) != count(b[j].Mask)) return false;
      if (output) {
        if ((a[i].Mask & a[i].NeverWrites_Mask) || (b[j].Mask & b[j].NeverWrites_Mask)) return false;
      } else if (count(a[i].AlwaysReads_Mask & a[i].Mask) != count(b[j].AlwaysReads_Mask & b[j].Mask)) return false;
      ++matches;
    }
    if (matches != 1) return false;
  }
  return true;
}

HRESULT
BuildTessellationPixelLink(
    const microsoft::CSignatureParser &output, const microsoft::CSignatureParser &input,
    SM50_SHADER_PS_INPUT_LINKAGE_DATA &link
) {
  using namespace microsoft;
  link = {}; link.type = SM50_SHADER_PS_INPUT_LINKAGE;
  const D3D11_SIGNATURE_PARAMETER *out, *in;
  const auto no = output.GetParameters(&out), ni = input.GetParameters(&in);
  if (!no || no > 32 || ni > 32) return E_INVALIDARG;
  const auto valid = [](const D3D11_SIGNATURE_PARAMETER &p) {
    return p.SemanticName && *p.SemanticName && p.Register < 32 && p.Mask && !(p.Mask & ~15u) &&
        !p.Stream && p.MinPrecision == D3D_MIN_PRECISION_DEFAULT && p.ComponentType >= 1 && p.ComponentType <= 3;
  };
  // Reject aliases and overlapping signature rows, including builtin/user aliases.
  for (unsigned side = 0; side < 2; ++side) {
    const auto *rows = side ? in : out; const auto count = side ? ni : no;
    for (UINT i = 0; i < count; ++i) {
      if (!valid(rows[i])) return E_INVALIDARG;
      for (UINT j = 0; j < i; ++j)
        if ((rows[i].Register == rows[j].Register && (rows[i].Mask & rows[j].Mask)) ||
            (rows[i].SystemValue == rows[j].SystemValue && rows[i].SemanticIndex == rows[j].SemanticIndex &&
             !strcasecmp(rows[i].SemanticName, rows[j].SemanticName))) return E_INVALIDARG;
    }
  }
  for (UINT i = 0; i < ni; ++i) {
    const auto &p = in[i];
    // Front facing is generated by rasterization, not by the preceding shader.
    if (p.SystemValue == D3D10_SB_NAME_IS_FRONT_FACE) {
      if (p.ComponentType != 1 || p.Mask != 1 || p.SemanticIndex ||
          strcasecmp(p.SemanticName, "SV_IsFrontFace") || (p.AlwaysReads_Mask & ~p.Mask)) return E_INVALIDARG;
      continue;
    }
    // Other builtins still require their explicitly supported producer contract.
    if (p.SystemValue && (p.SystemValue != D3D10_SB_NAME_POSITION || p.ComponentType != 3 || p.Mask != 15))
      return E_NOTIMPL;
    const D3D11_SIGNATURE_PARAMETER *match = nullptr;
    for (UINT j = 0; j < no; ++j) {
      const auto &o = out[j];
      if (p.SystemValue != o.SystemValue || p.SemanticIndex != o.SemanticIndex || strcasecmp(p.SemanticName, o.SemanticName)) continue;
      if (match || p.ComponentType != o.ComponentType || p.Mask != o.Mask || (o.NeverWrites_Mask & p.Mask)) return E_INVALIDARG;
      match = &o;
    }
    if (!match) return E_INVALIDARG;
    if (!p.SystemValue)
      link.entries[link.count++] = {p.Register, p.Mask, match->Register, match->Mask};
  }
  return S_OK;
}

HRESULT
BuildTessellationPixelLink(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, SM50_SHADER_PS_INPUT_LINKAGE_DATA &link, bool geometry = false) {
  link = {}; link.type = SM50_SHADER_PS_INPUT_LINKAGE;
  // The existing wow64 argument thunk does not carry the optional linkage node.
  if (sizeof(void *) != 8) return E_NOTIMPL;
  if (!desc.PS.pShaderBytecode && !desc.PS.BytecodeLength) return S_OK;
  if (!desc.PS.pShaderBytecode || !desc.PS.BytecodeLength) return E_INVALIDARG;
  microsoft::CSignatureParser ds, ps;
  if (FAILED(microsoft::DXBCGetOutputSignature((geometry ? desc.GS : desc.DS).pShaderBytecode, &ds)) ||
      FAILED(microsoft::DXBCGetInputSignature(desc.PS.pShaderBytecode, &ps))) return E_INVALIDARG;
  return BuildTessellationPixelLink(ds, ps, link);
}

HRESULT
ValidateTessellationArtifactLinkage(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &source, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &target) {
  using namespace microsoft;
  auto aligned = source; std::vector<uint8_t> vs, hs, ds, ps;
  if (!AlignDXBCForInspection(source.VS, vs) || !AlignDXBCForInspection(source.HS, hs) || !AlignDXBCForInspection(source.DS, ds)) return E_INVALIDARG;
  aligned.VS = {vs.data(), vs.size()}; aligned.HS = {hs.data(), hs.size()}; aligned.DS = {ds.data(), ds.size()};
  if (source.PS.pShaderBytecode || source.PS.BytecodeLength) {
    if (!AlignDXBCForInspection(source.PS, ps)) return E_INVALIDARG;
    aligned.PS = {ps.data(), ps.size()};
  }
  SM50_SHADER_PS_INPUT_LINKAGE_DATA pixel_link;
  if (FAILED(BuildTessellationPixelLink(aligned, pixel_link)) || FAILED(BuildTessellationPixelLink(target, pixel_link))) return E_INVALIDARG;
  HRESULT hr = ValidateTessellationSignatures(aligned);
  if (FAILED(hr)) return hr;
  hr = ValidateTessellationSignatures(target);
  if (FAILED(hr)) return hr;
  CSignatureParser source_hs, source_ds, target_hs, target_ds;
  if (FAILED(DXBCGetPatchConstantSignature(aligned.HS.pShaderBytecode, &source_hs)) ||
      FAILED(DXBCGetPatchConstantSignature(aligned.DS.pShaderBytecode, &source_ds)) ||
      FAILED(DXBCGetPatchConstantSignature(target.HS.pShaderBytecode, &target_hs)) ||
      FAILED(DXBCGetPatchConstantSignature(target.DS.pShaderBytecode, &target_ds)) ||
      !TessellationPatchSemanticsMatch(source_hs, target_hs, true) ||
      !TessellationPatchSemanticsMatch(source_ds, target_ds, false)) return E_INVALIDARG;
  return S_OK;
}

struct TessellationAllocation {
  uint32_t factor = 0, payload_bytes = 0, mesh_bytes = 0, mesh_slots = 0, mesh_groups = 0, threadgroup_bytes = 0;
  bool split_workload = false;
};

HRESULT
ValidateTessellationReflection(
    const TessellationStageDeclarations &hs, const MTL_SHADER_REFLECTION &ref_hs,
    const MTL_SHADER_REFLECTION &ref_ds, const MTL_SHADER_REFLECTION &ref_vs, TessellationAllocation &allocation,
    uint32_t linked_input_registers = 0
) {
  using namespace microsoft;
  if (linked_input_registers > 32 || !std::isfinite(hs.max_factor) || hs.max_factor < 1 || hs.max_factor > 64 ||
      !hs.input_control_points || hs.input_control_points > 32 || !hs.output_control_points || hs.output_control_points > 32 ||
      (hs.partition != D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER &&
       hs.partition != D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD &&
       hs.partition != D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN) ||
      ref_hs.Tessellator.MaxFactor != hs.max_factor || ref_hs.Tessellator.Partition != hs.partition ||
      uint32_t(ref_hs.Tessellator.OutputPrimitive) != hs.output_primitive ||
      !ref_hs.ThreadsPerPatch || ref_hs.ThreadsPerPatch > 32 ||
      (ref_hs.ThreadsPerPatch & (ref_hs.ThreadsPerPatch - 1)) ||
      ref_hs.ThreadsPerPatch < std::max(hs.input_control_points, hs.output_control_points) ||
      !ref_hs.NumOutputElement || ref_hs.NumOutputElement > 32 ||
      !ref_ds.NumOutputElement || ref_ds.NumOutputElement > 32 ||
      !ref_vs.NumOutputElement || ref_vs.NumOutputElement > 32 ||
      !hs.patch_scalars || hs.patch_scalars > 128 || !hs.patch_registers || hs.patch_registers > 32)
    return TraceUnsupportedPipeline(__func__, __LINE__);

  // Match the donor's partition rounding before supplying its capacity argument.
  // Reject instead of allowing get_final_maxtessfactor to reduce the declaration.
  uint32_t factor = uint32_t(std::ceil(hs.max_factor));
  if (hs.partition == D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD) {
    // 63 is the API-defined fractional-odd maximum, even with a declared maxtessfactor of 64.
    factor = std::min(63u, factor) | 1u;
  } else if (hs.partition == D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN) {
    factor = (std::max(2u, factor) + 1u) & ~1u;
  }
  if (!ref_ds.PostTessellator.MaxPotentialTessFactor ||
      (hs.domain != D3D11_SB_TESSELLATOR_DOMAIN_TRI && hs.domain != D3D11_SB_TESSELLATOR_DOMAIN_QUAD &&
       hs.domain != D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE) || factor > 64 ||
      ((hs.domain == D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE) != (hs.output_primitive == D3D11_SB_TESSELLATOR_OUTPUT_LINE)))
    return TraceUnsupportedPipeline(__func__, __LINE__);
  // Mirror the native domain's workload/payload layout, using post-initializer
  // ThreadsPerPatch; do not inherit its optional factor reduction. Integer arithmetic
  // is bounded above before multiplication. The 56-byte workload is the donor ABI.
  const uint64_t patches = 32 / ref_hs.ThreadsPerPatch;
  const bool isoline = hs.domain == D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE;
  const uint64_t workloads = isoline ? uint32_t(std::ceil(hs.max_factor)) :
      ((factor - 1 + 3) / 4) * (hs.domain == D3D11_SB_TESSELLATOR_DOMAIN_QUAD ? 4u : 3u) + (factor & 1);
  uint64_t slots = isoline ? factor + 1 : ((factor + 1 + 2 - ((factor + 1) & 1)) * 2) + 1;
  const uint64_t control_bytes = 16ull * ref_hs.NumOutputElement * hs.output_control_points * patches;
  const uint64_t patch_bytes = 4ull * hs.patch_scalars * patches;
  const uint64_t payload = (control_bytes + patch_bytes + 4 + workloads * patches * 56 + 15) & ~15ull;
  // Conservative index/primitive allowance in addition to all vec4 output registers.
  uint64_t mesh_bytes = slots * (16ull * ref_ds.NumOutputElement + 16);
  const bool split = factor > ref_ds.PostTessellator.MaxPotentialTessFactor || mesh_bytes > 32768;
  if (split) {
    if (isoline || (hs.output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW &&
                    hs.output_primitive != D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW) ||
        factor > ref_ds.PostTessellator.MaxPotentialSplitTessFactor)
      return TraceUnsupportedPipeline(__func__, __LINE__);
    slots = 2 * (factor + 1);
    // Mesh vertex storage is rounded in groups of eight. Indices are not vertex
    // attributes. Full Metal creation still checks actual clip/primitive outputs.
    mesh_bytes = ((slots + 7) & ~7ull) * 16ull * ref_ds.NumOutputElement;
  }
  const uint64_t mesh_groups = workloads * patches * (split ? 2 : 1);
  // Upper bound for simultaneous VS, optional HS control-point, patch and workload-count shared storage.
  const uint64_t shared = 16ull * std::max(ref_vs.NumOutputElement, linked_input_registers) * hs.input_control_points * patches +
      control_bytes + 16ull * hs.patch_registers * patches + 16;
  // Payload and mesh output have separate limits. The M1 Pro capacity probe
  // accepts the 11-register case and rejects 34816-byte mesh output above 32768.
  // This is only preflight: ALL three Metal PSO creations remain mandatory gates.
  if (payload > 16384 || mesh_bytes > 32768 || slots > 256 || mesh_groups > 1024 || shared > 32768)
    return TraceUnsupportedPipeline(__func__, __LINE__);
  allocation = {factor, uint32_t(payload), uint32_t(mesh_bytes), uint32_t(slots), uint32_t(mesh_groups), uint32_t(shared), split};
  return S_OK;
}

HRESULT
ValidateBoundedGeometryShader(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, microsoft::CDXBCParser &container, uint32_t code, bool source_bound_geometry = false
) {
  using namespace microsoft;
  if (source_bound_geometry ? (desc.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT &&
                              desc.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE &&
                              desc.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) :
                             desc.PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
    return E_INVALIDARG;
  // Reuse the container/stage, instruction-length and reserved-opcode checks.
  TessellationStageDeclarations unused;
  HRESULT hr;
  if (FAILED(hr = ReadTessellationStage(desc.VS, D3D10_SB_VERTEX_SHADER, unused, true)) ||
      FAILED(hr = ReadTessellationStage(desc.GS, D3D10_SB_GEOMETRY_SHADER, unused, true)) ||
      FAILED(hr = ReadTessellationStage(desc.PS, D3D10_SB_PIXEL_SHADER, unused, true)))
    return hr;

  CSignatureParser vs_input, vs_output, gs_input, ps_input;
  CSignatureParser5 gs_output;
  if (FAILED(DXBCGetInputSignature(desc.VS.pShaderBytecode, &vs_input)) ||
      FAILED(DXBCGetOutputSignature(desc.VS.pShaderBytecode, &vs_output)) ||
      FAILED(DXBCGetInputSignature(desc.GS.pShaderBytecode, &gs_input)) ||
      FAILED(DXBCGetOutputSignature(desc.GS.pShaderBytecode, &gs_output)) ||
      FAILED(DXBCGetInputSignature(desc.PS.pShaderBytecode, &ps_input)))
    return E_INVALIDARG;
  if (gs_output.NumStreams() != 1 || gs_output.RasterizedStream() != 0 ||
      !TessellationSignaturesMatch(vs_output, gs_input))
    return TraceUnsupportedPipeline(__func__, __LINE__);
  SM50_SHADER_PS_INPUT_LINKAGE_DATA pixel_link{};
  if (!TessellationSignaturesMatch(*gs_output.Signature(0), ps_input) &&
      FAILED(BuildTessellationPixelLink(*gs_output.Signature(0), ps_input, pixel_link)))
    return TraceUnsupportedPipeline(__func__, __LINE__);

  std::array<uint8_t, 32> input_masks = {}, output_masks = {}, required_inputs = {};
  const D3D11_SIGNATURE_PARAMETER *inputs, *outputs, *vs_params;
  const auto num_inputs = gs_input.GetParameters(&inputs);
  const auto num_outputs = gs_output.Signature(0)->GetParameters(&outputs);
  // Bound IA register shifts and the donor's fixed 16256-byte VS payload.
  for (auto *signature : {&vs_input, &vs_output}) {
    const auto count = signature->GetParameters(&vs_params);
    for (UINT i = 0; i < count; ++i)
      if (vs_params[i].Register >= 32 || !vs_params[i].Mask || (vs_params[i].Mask & ~15u) ||
          vs_params[i].Stream || vs_params[i].MinPrecision != D3D_MIN_PRECISION_DEFAULT)
        return TraceUnsupportedPipeline(__func__, __LINE__);
  }
  bool position = false;
  for (unsigned output = 0; output < 2; ++output) {
    const auto *parameters = output ? outputs : inputs;
    const auto count = output ? num_outputs : num_inputs;
    auto &masks = output ? output_masks : input_masks;
    for (UINT i = 0; i < count; ++i) {
      const auto &p = parameters[i];
      if (p.Register >= 32 || !p.Mask || (p.Mask & ~15u) || (masks[p.Register] & p.Mask) || p.Stream ||
          p.MinPrecision != D3D_MIN_PRECISION_DEFAULT || p.ComponentType == D3D10_SB_REGISTER_COMPONENT_UNKNOWN)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      if (p.SystemValue != D3D10_SB_NAME_UNDEFINED && p.SystemValue != D3D10_SB_NAME_POSITION &&
          p.SystemValue != D3D10_SB_NAME_CLIP_DISTANCE &&
          !(output && (p.SystemValue == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX ||
                       p.SystemValue == D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX)))
        return TraceUnsupportedPipeline(__func__, __LINE__);
      if (p.SystemValue == D3D10_SB_NAME_POSITION) {
        if (p.Mask != 15 || p.ComponentType != D3D10_SB_REGISTER_COMPONENT_FLOAT32 || (output && position))
          return TraceUnsupportedPipeline(__func__, __LINE__);
        if (output) position = true;
      }
      if (p.SystemValue == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX || p.SystemValue == D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX)
        if ((p.Mask & (p.Mask - 1)) || p.ComponentType != D3D10_SB_REGISTER_COMPONENT_UINT32)
          return TraceUnsupportedPipeline(__func__, __LINE__);
      masks[p.Register] |= p.Mask;
      if (!output)
        required_inputs[p.Register] |= p.AlwaysReads_Mask & p.Mask;
    }
  }
  if (!position)
    return TraceUnsupportedPipeline(__func__, __LINE__);

  const auto bytes = container.GetBlobSize(code);
  std::vector<uint32_t> words(bytes / sizeof(uint32_t));
  memcpy(words.data(), container.GetBlob(code), bytes);
  if (source_bound_geometry) {
    if (FAILED(hr = ValidatePointRootResources(desc))) return hr;
    gs_point::Contract contract;
    if (!gs_point::validate(words, input_masks, output_masks, contract)) return E_NOTIMPL;
    return gs_point::input_vertices(contract.primitive) == uint32_t(desc.PrimitiveTopologyType) ? S_OK : E_INVALIDARG;
  }
  std::array<uint8_t, 32> declared_input = {}, declared_output = {}, written = {};
  std::array<uint32_t, 14> constant_buffers = {};
  uint32_t temps = 0, emits = 0;
  bool input_primitive = false, output_topology = false, max_vertices = false, instances = false;
  bool stream = false, temps_declared = false, body = false, cut = false, ret = false;
  for (size_t at = 2; at < words.size();) {
    const auto token = words[at];
    const auto op = DECODE_D3D10_SB_OPCODE_TYPE(token);
    const auto n = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
    if (!n || n > words.size() - at || (token & 0x80000000u) || ret)
      return E_INVALIDARG;
    const bool stream_zero = n == 3 && !DECODE_IS_D3D10_SB_OPERAND_EXTENDED(words[at + 1]) &&
        DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(words[at + 1]) == D3D10_SB_OPERAND_0_COMPONENT &&
        DECODE_D3D10_SB_OPERAND_TYPE(words[at + 1]) == D3D11_SB_OPERAND_TYPE_STREAM &&
        DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(words[at + 1]) == D3D10_SB_OPERAND_INDEX_1D &&
        DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, words[at + 1]) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 &&
        words[at + 2] == 0;
    unsigned operands = 0;
    switch (op) {
    case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE:
      if (body || n != 1 || input_primitive) return E_INVALIDARG;
      if (DECODE_D3D10_SB_GS_INPUT_PRIMITIVE(token) != D3D10_SB_PRIMITIVE_TRIANGLE) return TraceUnsupportedPipeline(__func__, __LINE__);
      input_primitive = true;
      break;
    case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
      if (body || n != 1 || output_topology) return E_INVALIDARG;
      if (DECODE_D3D10_SB_GS_OUTPUT_PRIMITIVE_TOPOLOGY(token) != D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      output_topology = true;
      break;
    case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:
      if (body || n != 2 || max_vertices) return E_INVALIDARG;
      if (words[at + 1] != 3) return TraceUnsupportedPipeline(__func__, __LINE__);
      max_vertices = true;
      break;
    case D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT:
      if (body || n != 2 || instances) return E_INVALIDARG;
      if (words[at + 1] != 1) return TraceUnsupportedPipeline(__func__, __LINE__);
      instances = true;
      break;
    case D3D11_SB_OPCODE_DCL_STREAM:
      if (body || stream || !stream_zero) return E_INVALIDARG;
      stream = true;
      break;
    case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS:
      if (body || n != 1) return E_INVALIDARG;
      if (DECODE_D3D10_SB_GLOBAL_FLAGS(token) & ~D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED) return TraceUnsupportedPipeline(__func__, __LINE__);
      break;
    case D3D10_SB_OPCODE_DCL_TEMPS:
      if (body || n != 2 || temps_declared) return E_INVALIDARG;
      if (words[at + 1] > 4096) return TraceUnsupportedPipeline(__func__, __LINE__);
      temps = words[at + 1];
      temps_declared = true;
      break;
    case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER: {
      if (body || n != 4) return E_INVALIDARG;
      const auto operand = words[at + 1], slot = words[at + 2], count = words[at + 3];
      if (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(operand) ||
          DECODE_D3D10_SB_OPERAND_TYPE(operand) != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER ||
          DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(operand) != D3D10_SB_OPERAND_4_COMPONENT ||
          DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(operand) > D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE ||
          DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(operand) != D3D10_SB_OPERAND_INDEX_2D ||
          DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, operand) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
          DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, operand) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
          slot >= constant_buffers.size() || constant_buffers[slot] || !count || count > 4096)
        return E_INVALIDARG;
      constant_buffers[slot] = count;
      break;
    }
    case D3D10_SB_OPCODE_DCL_INPUT:
    case D3D10_SB_OPCODE_DCL_INPUT_SIV:
    case D3D10_SB_OPCODE_DCL_OUTPUT:
    case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
      const bool input = op == D3D10_SB_OPCODE_DCL_INPUT || op == D3D10_SB_OPCODE_DCL_INPUT_SIV;
      const bool system = op == D3D10_SB_OPCODE_DCL_INPUT_SIV || op == D3D10_SB_OPCODE_DCL_OUTPUT_SIV;
      if (body || n != unsigned(3 + input + system)) return E_INVALIDARG;
      const auto operand = words[at + 1];
      if (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(operand) ||
          DECODE_D3D10_SB_OPERAND_TYPE(operand) != (input ? D3D10_SB_OPERAND_TYPE_INPUT : D3D10_SB_OPERAND_TYPE_OUTPUT) ||
          DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(operand) != D3D10_SB_OPERAND_4_COMPONENT ||
          DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(operand) != D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE ||
          DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(operand) != (input ? D3D10_SB_OPERAND_INDEX_2D : D3D10_SB_OPERAND_INDEX_1D) ||
          DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, operand) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
          (input && (words[at + 2] != 3 ||
                     DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, operand) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)))
        return E_INVALIDARG;
      const auto reg = words[at + 2 + input];
      const auto mask = DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(operand) >> 4;
      const auto name = system ? words[at + n - 1] : unsigned(D3D10_SB_NAME_UNDEFINED);
      if (system && name == D3D10_SB_NAME_UNDEFINED) return E_INVALIDARG;
      auto &declared = input ? declared_input : declared_output;
      if (reg >= 32 || !mask || (declared[reg] & mask)) return E_INVALIDARG;
      uint32_t matched = 0;
      const auto *parameters = input ? inputs : outputs;
      const auto count = input ? num_inputs : num_outputs;
      for (UINT i = 0; i < count; ++i)
        if (parameters[i].Register == reg && uint32_t(parameters[i].SystemValue) == name)
          matched |= parameters[i].Mask;
      if (mask & ~matched) return E_INVALIDARG;
      declared[reg] |= mask;
      break;
    }
    case D3D10_SB_OPCODE_EMIT:
    case D3D11_SB_OPCODE_EMIT_STREAM:
    case D3D10_SB_OPCODE_EMITTHENCUT:
    case D3D11_SB_OPCODE_EMITTHENCUT_STREAM: {
      const bool stream_op = op == D3D11_SB_OPCODE_EMIT_STREAM || op == D3D11_SB_OPCODE_EMITTHENCUT_STREAM;
      const bool also_cut = op == D3D10_SB_OPCODE_EMITTHENCUT || op == D3D11_SB_OPCODE_EMITTHENCUT_STREAM;
      body = true;
      if ((stream_op ? !stream_zero : n != 1) || cut || emits >= 3 || written != output_masks)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      ++emits;
      written.fill(0);
      if (also_cut) {
        if (emits != 3) return TraceUnsupportedPipeline(__func__, __LINE__);
        cut = true;
      }
      break;
    }
    case D3D10_SB_OPCODE_CUT:
    case D3D11_SB_OPCODE_CUT_STREAM:
      body = true;
      if ((op == D3D10_SB_OPCODE_CUT ? n != 1 : !stream_zero) || cut || emits != 3)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      cut = true;
      break;
    case D3D10_SB_OPCODE_RET:
      if (n != 1 || emits != 3) return TraceUnsupportedPipeline(__func__, __LINE__);
      ret = true;
      break;
    case D3D10_SB_OPCODE_MOV:
    case D3D10_SB_OPCODE_FTOI:
    case D3D10_SB_OPCODE_FTOU:
    case D3D10_SB_OPCODE_ITOF:
    case D3D10_SB_OPCODE_UTOF:
    case D3D10_SB_OPCODE_INEG:
    case D3D10_SB_OPCODE_NOT:
      operands = 2;
      break;
    case D3D10_SB_OPCODE_ADD:
    case D3D10_SB_OPCODE_MUL:
    case D3D10_SB_OPCODE_DIV:
    case D3D10_SB_OPCODE_MIN:
    case D3D10_SB_OPCODE_MAX:
    case D3D10_SB_OPCODE_DP2:
    case D3D10_SB_OPCODE_DP3:
    case D3D10_SB_OPCODE_DP4:
    case D3D10_SB_OPCODE_EQ:
    case D3D10_SB_OPCODE_NE:
    case D3D10_SB_OPCODE_GE:
    case D3D10_SB_OPCODE_LT:
    case D3D10_SB_OPCODE_IEQ:
    case D3D10_SB_OPCODE_INE:
    case D3D10_SB_OPCODE_IADD:
    case D3D10_SB_OPCODE_AND:
    case D3D10_SB_OPCODE_OR:
    case D3D10_SB_OPCODE_XOR:
      operands = 3;
      break;
    case D3D10_SB_OPCODE_MAD:
    case D3D10_SB_OPCODE_MOVC:
      operands = 4;
      break;
    default:
      // No loops/branches, dynamic addressing, extra streams, textures or UAVs.
      return TraceUnsupportedPipeline(__func__, __LINE__);
    }
    if (operands) {
      body = true;
      if (cut) return TraceUnsupportedPipeline(__func__, __LINE__);
      size_t cursor = at + 1, end = at + n;
      for (unsigned i = 0; i < operands; ++i) {
        if (cursor == end) return E_INVALIDARG;
        const auto operand = words[cursor++];
        const auto type = DECODE_D3D10_SB_OPERAND_TYPE(operand);
        const auto components = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(operand);
        const auto dimensions = DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(operand);
        if (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(operand)) {
          if (!i || cursor == end) return E_INVALIDARG;
          const auto extension = words[cursor++];
          if (DECODE_D3D10_SB_EXTENDED_OPERAND_TYPE(extension) != D3D10_SB_EXTENDED_OPERAND_MODIFIER ||
              DECODE_D3D10_SB_OPERAND_MODIFIER(extension) > D3D10_SB_OPERAND_MODIFIER_ABSNEG ||
              (extension & ~(D3D10_SB_EXTENDED_OPERAND_TYPE_MASK | D3D10_SB_OPERAND_MODIFIER_MASK)))
            return TraceUnsupportedPipeline(__func__, __LINE__);
        }
        if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
          const size_t count = components == D3D10_SB_OPERAND_1_COMPONENT ? 1 :
                               components == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 0;
          if (!i || dimensions || !count || count > end - cursor) return E_INVALIDARG;
          cursor += count;
          continue;
        }
        if (components != D3D10_SB_OPERAND_4_COMPONENT || dimensions < 1 || dimensions > 2)
          return TraceUnsupportedPipeline(__func__, __LINE__);
        const auto selection = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(operand);
        uint32_t mask = 0;
        if (!i) {
          if (selection != D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) return E_INVALIDARG;
          mask = DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(operand) >> 4;
          if (!mask) return E_INVALIDARG;
        } else if (selection == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) {
          for (unsigned lane = 0; lane < 4; ++lane)
            mask |= 1u << DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(operand, lane);
        } else if (selection == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) {
          mask = 1u << DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(operand);
        } else {
          return TraceUnsupportedPipeline(__func__, __LINE__);
        }
        uint32_t indices[2] = {};
        for (unsigned index = 0; index < unsigned(dimensions); ++index) {
          if (DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(index, operand) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
            return TraceUnsupportedPipeline(__func__, __LINE__);
          if (cursor == end) return E_INVALIDARG;
          indices[index] = words[cursor++];
        }
        if (type == D3D10_SB_OPERAND_TYPE_TEMP) {
          if (dimensions != 1 || indices[0] >= temps) return E_INVALIDARG;
        } else if (type == D3D10_SB_OPERAND_TYPE_OUTPUT && !i) {
          if (dimensions != 1 || indices[0] >= 32 || (mask & ~declared_output[indices[0]])) return E_INVALIDARG;
          written[indices[0]] |= mask;
        } else if (type == D3D10_SB_OPERAND_TYPE_INPUT && i) {
          if (dimensions != 2 || indices[0] >= 3 || indices[1] >= 32 || (mask & ~declared_input[indices[1]]))
            return E_INVALIDARG;
        } else if (type == D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER && i) {
          if (dimensions != 2 || indices[0] >= constant_buffers.size() || indices[1] >= constant_buffers[indices[0]])
            return E_INVALIDARG;
        } else {
          return TraceUnsupportedPipeline(__func__, __LINE__);
        }
      }
      if (cursor != end) return E_INVALIDARG;
    }
    at += n;
  }
  for (unsigned i = 0; i < 32; ++i)
    if (required_inputs[i] & ~declared_input[i]) return E_INVALIDARG;
  return input_primitive && output_topology && max_vertices && ret && declared_output == output_masks ? S_OK : E_INVALIDARG;
}

} // namespace

class MTLD3D12GraphicsPipelineStateImpl : public MTLD3D12Pageable<MTLD3D12GraphicsPipelineState> {
protected:
  MTL_SHADER_REFLECTION ref_vs = {};
  MTL_SHADER_REFLECTION ref_ps = {};

  WMT::Reference<WMT::DepthStencilState> dsso;
  WMT::Reference<WMT::DepthStencilState> dsso_depth_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_stencil_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_readonly;
  WMT::Reference<WMT::DepthStencilState> dsso_no_stencil;
  WMT::Reference<WMT::DepthStencilState> dsso_readonly_no_stencil;

public:
  uint32_t capture_id = 0;

  MTLD3D12GraphicsPipelineStateImpl(MTLD3D12Device *pDevice) :
      MTLD3D12Pageable<MTLD3D12GraphicsPipelineState>(pDevice) {
    IsComputePipelineState = FALSE;
  }

  bool
  BlendFactorIsDualSource(D3D12_BLEND Blend) {
    return (Blend >= D3D12_BLEND_SRC1_COLOR) && (Blend <= D3D12_BLEND_INV_SRC1_ALPHA);
  }

  template <typename RenderPipelineInfo>
  HRESULT
  InitializePSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, RenderPipelineInfo &info, bool &dual_source_blending) {
    HRESULT hr;
    MTL_DXGI_FORMAT_DESC format_desc;
    uint32_t effective_dual_source_rtvs = 0;
    for (unsigned i = 0; i < pDesc->NumRenderTargets; i++) {
      if (pDesc->RTVFormats[i] == DXGI_FORMAT_UNKNOWN)
        continue;
      if (i >= 2 && dual_source_blending)
        break;
      auto &rt = info.colors[i];
      auto Format = pDesc->RTVFormats[i];
      if (FAILED(hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), Format, format_desc))) {
        return hr;
      }
      rt.pixel_format = format_desc.PixelFormat;

      auto renderTarget = pDesc->BlendState.RenderTarget[pDesc->BlendState.IndependentBlendEnable ? i : 0];

      if (renderTarget.BlendEnable && renderTarget.LogicOpEnable)
        return E_INVALIDARG;

      if (pDesc->BlendState.IndependentBlendEnable && renderTarget.LogicOpEnable)
        return E_INVALIDARG;

      rt.write_mask = kColorWriteMaskMap[renderTarget.RenderTargetWriteMask];

      if (rt.pixel_format == WMTPixelFormatRGB9E5Float)
        rt.write_mask = (rt.write_mask & ~WMTColorWriteMaskAlpha) ? WMTColorWriteMaskAll : 0;

      if (renderTarget.BlendEnable) {
        if (!any_bit_set(device_->GetMTLPixelFormatCapability(rt.pixel_format) & FormatCapability::Blend)) {
          WARN("CreateGraphicsPipelineState: pixel format ", rt.pixel_format, " is not blendable");
          return E_INVALIDARG;
        }
        if (BlendFactorIsDualSource(renderTarget.SrcBlendAlpha) || BlendFactorIsDualSource(renderTarget.SrcBlend) ||
            BlendFactorIsDualSource(renderTarget.DestBlendAlpha) || BlendFactorIsDualSource(renderTarget.DestBlend)) {
          dual_source_blending = true;
        }
        rt.alpha_blend_operation = kBlendOpMap[renderTarget.BlendOpAlpha];
        rt.rgb_blend_operation = kBlendOpMap[renderTarget.BlendOp];
        rt.blending_enabled = renderTarget.BlendEnable;
        rt.src_alpha_blend_factor = kBlendAlphaFactorMap[renderTarget.SrcBlendAlpha];
        rt.src_rgb_blend_factor = kBlendFactorMap[renderTarget.SrcBlend];
        rt.dst_alpha_blend_factor = kBlendAlphaFactorMap[renderTarget.DestBlendAlpha];
        rt.dst_rgb_blend_factor = kBlendFactorMap[renderTarget.DestBlend];
      }
      if (i < 2)
        effective_dual_source_rtvs++;
    }

    if (dual_source_blending && effective_dual_source_rtvs > 1)
      return E_INVALIDARG;

    if (pDesc->DSVFormat != DXGI_FORMAT_UNKNOWN) {
      if (FAILED(hr = MTLQueryDXGIFormat(device_->GetMTLDevice(), pDesc->DSVFormat, format_desc))) {
        return hr;
      }
      auto dsv_flags = DepthStencilPlanarFlags(format_desc.PixelFormat);
      if (dsv_flags & 1)
        info.depth_pixel_format = format_desc.PixelFormat;
      if (dsv_flags & 2)
        info.stencil_pixel_format = format_desc.PixelFormat;
    }

    if constexpr (std::is_same_v<RenderPipelineInfo, WMTRenderPipelineInfo>) {
      switch (pDesc->PrimitiveTopologyType) {
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
        info.input_primitive_topology = WMTPrimitiveTopologyClassPoint;
        break;
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
        info.input_primitive_topology = WMTPrimitiveTopologyClassLine;
        break;
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
      case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH:
        info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
        break;
      default:
        break;
      }
    }

    info.raster_sample_count = pDesc->SampleDesc.Count;
    info.support_indirect_command_buffers = true;

    info.alpha_to_coverage_enabled = pDesc->BlendState.AlphaToCoverageEnable && !ref_ps.PixelShader.HasCoverageOutput;

    return S_OK;
  }

  void
  InitializeDSSO(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc) {
    WMTDepthStencilInfo info;
    info.depth_compare_function = WMTCompareFunctionAlways;
    info.depth_write_enabled = false;
    info.front_stencil.enabled = false;
    info.back_stencil.enabled = false;
    if (pDesc->DepthStencilState.DepthEnable) {
      info.depth_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.DepthFunc];
      info.depth_write_enabled = pDesc->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
    }

    if (pDesc->DepthStencilState.StencilEnable) {
      info.front_stencil.enabled = true;
      info.back_stencil.enabled = true;

      info.front_stencil.depth_stencil_pass_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilPassOp];
      info.front_stencil.stencil_fail_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilFailOp];
      info.front_stencil.depth_fail_op = kStencilOperationMap[pDesc->DepthStencilState.FrontFace.StencilDepthFailOp];
      info.front_stencil.stencil_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.FrontFace.StencilFunc];
      info.front_stencil.write_mask = pDesc->DepthStencilState.StencilWriteMask;
      info.front_stencil.read_mask = pDesc->DepthStencilState.StencilReadMask;

      info.back_stencil.depth_stencil_pass_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilPassOp];
      info.back_stencil.stencil_fail_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilFailOp];
      info.back_stencil.depth_fail_op = kStencilOperationMap[pDesc->DepthStencilState.BackFace.StencilDepthFailOp];
      info.back_stencil.stencil_compare_function = kCompareFunctionMap[pDesc->DepthStencilState.BackFace.StencilFunc];
      info.back_stencil.write_mask = pDesc->DepthStencilState.StencilWriteMask;
      info.back_stencil.read_mask = pDesc->DepthStencilState.StencilReadMask;
    }

    auto metal = device_->GetMTLDevice();
    dsso = metal.newDepthStencilState(info);
    {
      auto info_depth_readonly = info;
      info_depth_readonly.depth_write_enabled = false;
      dsso_depth_readonly = metal.newDepthStencilState(info_depth_readonly);
    }
    {
      auto info_stencil_readonly = info;
      info_stencil_readonly.back_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.back_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_stencil_readonly.back_stencil.depth_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_stencil_readonly.front_stencil.depth_fail_op = WMTStencilOperationKeep;
      dsso_stencil_readonly = metal.newDepthStencilState(info_stencil_readonly);
    }
    {
      auto info_readonly = info;
      info_readonly.depth_write_enabled = false;
      info_readonly.back_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_readonly.back_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_readonly.back_stencil.depth_fail_op = WMTStencilOperationKeep;
      info_readonly.front_stencil.stencil_fail_op = WMTStencilOperationKeep;
      info_readonly.front_stencil.depth_stencil_pass_op = WMTStencilOperationKeep;
      info_readonly.front_stencil.depth_fail_op = WMTStencilOperationKeep;
      dsso_readonly = metal.newDepthStencilState(info_readonly);
    }
    {
      auto info_nostencil = info;
      info_nostencil.back_stencil.enabled = false;
      info_nostencil.front_stencil.enabled = false;
      dsso_no_stencil = metal.newDepthStencilState(info_nostencil);
    }
    {
      auto info_nostencil = info;
      info_nostencil.depth_write_enabled = false;
      info_nostencil.back_stencil.enabled = false;
      info_nostencil.front_stencil.enabled = false;
      dsso_readonly_no_stencil = metal.newDepthStencilState(info_nostencil);
    }
  }

  void
  InitializeRasterizerState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc) {
    fill_mode =
        pDesc->RasterizerState.FillMode == D3D12_FILL_MODE_SOLID ? WMTTriangleFillModeFill : WMTTriangleFillModeLines;
    switch (pDesc->RasterizerState.CullMode) {
    case D3D12_CULL_MODE_BACK:
      cull_mode = WMTCullModeBack;
      break;
    case D3D12_CULL_MODE_FRONT:
      cull_mode = WMTCullModeFront;
      break;
    case D3D12_CULL_MODE_NONE:
      cull_mode = WMTCullModeNone;
      break;
    }
    depth_clip_mode = pDesc->RasterizerState.DepthClipEnable ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
    depth_bias = pDesc->RasterizerState.DepthBias;
    scole_scale = pDesc->RasterizerState.SlopeScaledDepthBias;
    depth_bias_clamp = pDesc->RasterizerState.DepthBiasClamp;
    winding = pDesc->RasterizerState.FrontCounterClockwise ? WMTWindingCounterClockwise : WMTWindingClockwise;
    forced_sample_count = pDesc->RasterizerState.ForcedSampleCount;
  }

  virtual HRESULT
  Initialize(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc) {
    if (!pDesc || (pDesc->pRootSignature && static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->IsLocal))
      return E_INVALIDARG;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC compiled_desc = *pDesc;
    std::vector<uint8_t> compiled_vs, compiled_ps, compiled_hs, compiled_ds, compiled_gs;
    const HRESULT vs_artifact = LoadDiagnosticDXILArtifact(pDesc->VS, 1, compiled_vs, compiled_desc.VS);
    const HRESULT ps_artifact = LoadDiagnosticDXILArtifact(pDesc->PS, 0, compiled_ps, compiled_desc.PS);
    const HRESULT hs_artifact = LoadDiagnosticDXILArtifact(pDesc->HS, 3, compiled_hs, compiled_desc.HS);
    const HRESULT ds_artifact = LoadDiagnosticDXILArtifact(pDesc->DS, 4, compiled_ds, compiled_desc.DS);
    if (vs_artifact != S_FALSE || ps_artifact != S_FALSE)
      TracePipelineCapture(capture_id, this, "dxil_artifact", "vs_hr=%08x ps_hr=%08x vs_bytes=%llu ps_bytes=%llu",
                           unsigned(vs_artifact), unsigned(ps_artifact),
                           (unsigned long long)compiled_desc.VS.BytecodeLength, (unsigned long long)compiled_desc.PS.BytecodeLength);
    if (FAILED(vs_artifact)) return vs_artifact;
    if (FAILED(ps_artifact)) return ps_artifact;
    if (FAILED(hs_artifact)) return hs_artifact;
    if (FAILED(ds_artifact)) return ds_artifact;
    const HRESULT gs_artifact = LoadDiagnosticDXILArtifact(pDesc->GS, 2, compiled_gs, compiled_desc.GS);
    if (FAILED(gs_artifact)) return gs_artifact;
    const bool translated_geometry = gs_artifact == S_OK;
    gs_point::Contract geometry_contract;
    if (translated_geometry) {
      if (pDesc->HS.BytecodeLength || pDesc->HS.pShaderBytecode || pDesc->DS.BytecodeLength || pDesc->DS.pShaderBytecode) return E_NOTIMPL;
      const HRESULT result = ValidatePointGeometryArtifact(pDesc->GS, compiled_desc.GS, geometry_contract);
      if (FAILED(result)) return result;
      if (gs_point::input_vertices(geometry_contract.primitive) != uint32_t(pDesc->PrimitiveTopologyType)) return E_INVALIDARG;
    }
    const bool native_list_geometry = gs_artifact == S_FALSE && pDesc->GS.pShaderBytecode &&
        pDesc->GS.BytecodeLength && (pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT ||
                                    pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    if (native_list_geometry) {
      const auto status = gs_contract_reader::Read(
          {static_cast<const uint8_t *>(pDesc->GS.pShaderBytecode), pDesc->GS.BytecodeLength}, geometry_contract);
      if (status != gs_contract_reader::Status::Success)
        return status == gs_contract_reader::Status::Unsupported ? E_NOTIMPL : E_INVALIDARG;
      if (gs_point::input_vertices(geometry_contract.primitive) != uint32_t(pDesc->PrimitiveTopologyType))
        return E_INVALIDARG;
    }
    const bool contracted_geometry = translated_geometry || native_list_geometry;
    const bool translated_tessellation = hs_artifact == S_OK || ds_artifact == S_OK;
    if (translated_tessellation) {
      // No mixed converted/unconverted HS/DS pair and no unbound metadata defaults.
      if (hs_artifact != S_OK || ds_artifact != S_OK || pDesc->PrimitiveTopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) return E_NOTIMPL;
      HRESULT result = ValidateTessellationArtifactContract(pDesc->HS, compiled_desc.HS, 3);
      if (FAILED(result)) return result;
      result = ValidateTessellationArtifactContract(pDesc->DS, compiled_desc.DS, 4);
      if (FAILED(result)) return result;
      result = ValidateTessellationArtifactLinkage(*pDesc, compiled_desc);
      if (FAILED(result)) return result;
    }
    if (vs_artifact == S_OK || ps_artifact == S_OK || translated_tessellation || translated_geometry) {
      // Keep the application's root layout; the compiler artifact has no rootsig.
      if (!pDesc->pRootSignature || ((pDesc->GS.pShaderBytecode || pDesc->GS.BytecodeLength) && !translated_geometry) ||
          ((pDesc->HS.pShaderBytecode || pDesc->HS.BytecodeLength || pDesc->DS.pShaderBytecode || pDesc->DS.BytecodeLength) && !translated_tessellation))
        return E_NOTIMPL;
      pDesc = &compiled_desc;
    }
    const auto initialize_stage = [&](const char *stage, D3D12_SHADER_BYTECODE bytecode,
                                      sm50_shader_t *shader, MTL_SHADER_REFLECTION *reflection) {
      const HRESULT result = InitializeShader(bytecode, shader, reflection);
      if (capture_id) {
        const DWORD error = GetLastError();
        char hash[41] = "not_captured";
        const char *container = "unknown";
        if (bytecode.pShaderBytecode && bytecode.BytecodeLength && bytecode.BytecodeLength <= 16 * 1024 * 1024) {
          microsoft::CDXBCParser parser;
          if (SUCCEEDED(parser.ReadDXBC(bytecode.pShaderBytecode, bytecode.BytecodeLength))) {
            container = parser.FindNextMatchingBlob(microsoft::DXBC_DXIL) != DXBC_BLOB_NOT_FOUND ? "DXIL" : "DXBC";
            const auto digest = Sha1HashState::compute(bytecode.pShaderBytecode, bytecode.BytecodeLength);
            constexpr char hex[] = "0123456789abcdef";
            for (unsigned i = 0; i < 20; ++i) { hash[2*i] = hex[digest.data[i] >> 4]; hash[2*i+1] = hex[digest.data[i] & 15]; }
            hash[40] = 0;
          }
        }
        TracePipelineCapture(capture_id, this, "shader", "stage=%s length=%llu container=%s sha1=%s hr=%08x",
                             stage, (unsigned long long)bytecode.BytecodeLength, container, hash, unsigned(result));
        SetLastError(error);
      }
      return result;
    };
    static std::atomic<uint32_t> desc_trace{0};
    TraceFrame(desc_trace, "pso.graphics.desc", this,
               "vs=%llu ps=%llu gs=%llu hs=%llu ds=%llu so_entries=%u so_strides=%u topology=%u rts=%u samples=%u",
               static_cast<unsigned long long>(pDesc->VS.BytecodeLength),
               static_cast<unsigned long long>(pDesc->PS.BytecodeLength),
               static_cast<unsigned long long>(pDesc->GS.BytecodeLength),
               static_cast<unsigned long long>(pDesc->HS.BytecodeLength),
               static_cast<unsigned long long>(pDesc->DS.BytecodeLength),
               unsigned(pDesc->StreamOutput.NumEntries), unsigned(pDesc->StreamOutput.NumStrides),
               unsigned(pDesc->PrimitiveTopologyType), unsigned(pDesc->NumRenderTargets), unsigned(pDesc->SampleDesc.Count));
    if (pDesc->StreamOutput.NumEntries || pDesc->StreamOutput.NumStrides) {
      ERR("CreatePipelineState: SO not supported");
      return TraceUnsupportedPipeline(__func__, __LINE__);
    }

    HRESULT hr;
    const bool has_hs = pDesc->HS.pShaderBytecode || pDesc->HS.BytecodeLength;
    const bool has_ds = pDesc->DS.pShaderBytecode || pDesc->DS.BytecodeLength;
    const bool tessellation = has_hs && has_ds;
    if (has_hs != has_ds || tessellation != (pDesc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH))
      return E_INVALIDARG;

    SM50Shader shader_vs, shader_ps, shader_hs, shader_ds, shader_gs;
    MTL_SHADER_REFLECTION ref_hs = {}, ref_ds = {}, ref_gs = {};
    TessellationStageDeclarations hs_declarations, ds_declarations;
    TessellationAllocation tess_allocation;
    bool geometry = false;
    SM50Error sm50_err;
    auto metal = device_->GetMTLDevice();
    WMT::Reference<WMT::Error> err;
    WMT::Reference<WMT::Function> vs_func, ps_func, ds_func, gs_func;
    WMT::Reference<WMT::Function> indexed_vs_func[2];

    if (tessellation) {
      if (pDesc->GS.pShaderBytecode || pDesc->GS.BytecodeLength || !pDesc->pRootSignature ||
          !metal.supportsFamily(WMTGPUFamilyApple7) ||
          pDesc->IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED ||
          pDesc->RasterizerState.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF ||
          pDesc->RasterizerState.ForcedSampleCount)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      if (pDesc->NumRenderTargets > 8 || pDesc->InputLayout.NumElements > 32 ||
          (pDesc->InputLayout.NumElements && !pDesc->InputLayout.pInputElementDescs))
        return E_INVALIDARG;
      for (UINT i = 0; i < pDesc->NumRenderTargets; ++i) {
        if (pDesc->BlendState.RenderTarget[pDesc->BlendState.IndependentBlendEnable ? i : 0].LogicOpEnable)
          return TraceUnsupportedPipeline(__func__, __LINE__);
      }
      for (UINT i = 0; i < pDesc->InputLayout.NumElements; ++i) {
        const auto &element = pDesc->InputLayout.pInputElementDescs[i];
        if (!element.SemanticName || element.InputSlot >= 32 ||
            (element.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
             element.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA))
          return E_INVALIDARG;
      }
      TessellationStageDeclarations unused;
      if (FAILED(hr = ReadTessellationStage(pDesc->VS, microsoft::D3D10_SB_VERTEX_SHADER, unused)) ||
          FAILED(hr = ReadTessellationStage(pDesc->HS, microsoft::D3D11_SB_HULL_SHADER, hs_declarations)) ||
          FAILED(hr = ReadTessellationStage(pDesc->DS, microsoft::D3D11_SB_DOMAIN_SHADER, ds_declarations)))
        return hr;
      if (pDesc->PS.pShaderBytecode || pDesc->PS.BytecodeLength) {
        if (FAILED(hr = ReadTessellationStage(pDesc->PS, microsoft::D3D10_SB_PIXEL_SHADER, unused)))
          return hr;
      }
      if (hs_declarations.domain != ds_declarations.domain ||
          hs_declarations.output_control_points != ds_declarations.input_control_points)
        return E_INVALIDARG;
      if (FAILED(hr = ValidateTessellationSignatures(*pDesc)))
        return hr;
      if (FAILED(hr = initialize_stage("HS", pDesc->HS, &shader_hs, &ref_hs)) ||
          FAILED(hr = initialize_stage("DS", pDesc->DS, &shader_ds, &ref_ds)))
        return hr;
    }

    SM50_SHADER_COMMON_DATA common;
    common.flags = {};
    common.type = SM50_SHADER_COMMON;
    common.metal_version = (SM50_SHADER_METAL_VERSION)device_->GetShaderMetalVersion();
    common.next = nullptr;

    SM50_SHADER_GS_PASS_THROUGH_DATA gs_passthrough = {};
    gs_passthrough.type = SM50_SHADER_GS_PASS_THROUGH;
    gs_passthrough.DataEncoded = ~0u;
    gs_passthrough.next = &common;
    if (pDesc->GS.pShaderBytecode || pDesc->GS.BytecodeLength) {
      using namespace microsoft;
      CDXBCParser parser;
      if (!pDesc->GS.pShaderBytecode || !pDesc->GS.BytecodeLength)
        return E_INVALIDARG;
      if (FAILED(hr = parser.ReadDXBC(pDesc->GS.pShaderBytecode, pDesc->GS.BytecodeLength)))
        return hr;
      if (parser.FindNextMatchingBlob(DXBC_DXIL) != DXBC_BLOB_NOT_FOUND)
        return TraceUnsupportedPipeline(__func__, __LINE__);
      auto code = parser.FindNextMatchingBlob(DXBC_GenericShaderEx);
      if (code == DXBC_BLOB_NOT_FOUND)
        code = parser.FindNextMatchingBlob(DXBC_GenericShader);
      if (code == DXBC_BLOB_NOT_FOUND || parser.GetBlobSize(code) < 2 * sizeof(uint32_t))
        return E_INVALIDARG;
      uint32_t version;
      memcpy(&version, parser.GetBlob(code), sizeof(version));
      if (DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(version) != D3D10_SB_GEOMETRY_SHADER)
        return E_INVALIDARG;

      bool topology_mismatch;
      if (!ValidateGSPassthrough(*pDesc, parser, code, gs_passthrough.Data, topology_mismatch)) {
        if (topology_mismatch)
          return E_INVALIDARG;
        if (!pDesc->pRootSignature || !pDesc->PS.pShaderBytecode ||
            !metal.supportsFamily(WMTGPUFamilyApple7) ||
            pDesc->IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED ||
            pDesc->RasterizerState.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF ||
            pDesc->RasterizerState.ForcedSampleCount)
          return TraceUnsupportedPipeline(__func__, __LINE__);
        if (pDesc->NumRenderTargets > 8 || pDesc->InputLayout.NumElements > 32 ||
            (pDesc->InputLayout.NumElements && !pDesc->InputLayout.pInputElementDescs))
          return E_INVALIDARG;
        for (UINT i = 0; i < pDesc->NumRenderTargets; ++i)
          if (pDesc->BlendState.RenderTarget[pDesc->BlendState.IndependentBlendEnable ? i : 0].LogicOpEnable)
            return TraceUnsupportedPipeline(__func__, __LINE__);
        for (UINT i = 0; i < pDesc->InputLayout.NumElements; ++i) {
          const auto &element = pDesc->InputLayout.pInputElementDescs[i];
          if (!element.SemanticName || element.InputSlot >= 32 ||
              (element.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
               element.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA))
            return E_INVALIDARG;
        }
        if (FAILED(hr = ValidateBoundedGeometryShader(*pDesc, parser, code, contracted_geometry)))
          return hr;
        if (FAILED(hr = initialize_stage("GS", pDesc->GS, &shader_gs, &ref_gs)))
          return hr;
        if (ref_gs.GeometryShader.Primitive != (contracted_geometry ? geometry_contract.primitive : uint32_t(D3D10_SB_PRIMITIVE_TRIANGLE)) ||
            !ref_gs.NumOutputElement || ref_gs.NumOutputElement > 32)
          return TraceUnsupportedPipeline(__func__, __LINE__);
        geometry = true;
      }
    }
    const bool mesh_pipeline = tessellation || geometry;

    if (pDesc->VS.pShaderBytecode) {
      if (FAILED(hr = initialize_stage("VS", pDesc->VS, &shader_vs, &ref_vs)))
        return hr;
      stage_linkage::VertexHullPlan vertex_link;
      if (tessellation) {
        microsoft::CSignatureParser vs_output, hs_input;
        if (FAILED(microsoft::DXBCGetOutputSignature(pDesc->VS.pShaderBytecode, &vs_output)) ||
            FAILED(microsoft::DXBCGetInputSignature(pDesc->HS.pShaderBytecode, &hs_input)) ||
            FAILED(hr = BuildTessellationVertexLink(vs_output, hs_input, vertex_link))) return E_INVALIDARG;
        if (!vertex_link.identity && sizeof(void *) != 8) return E_NOTIMPL;
        if (FAILED(hr = ValidateTessellationReflection(hs_declarations, ref_hs, ref_ds, ref_vs, tess_allocation,
                                                       vertex_link.registers))) return hr;
        TracePipelineCapture(capture_id, this, "tessellation.allocation",
            "max=%g factor=%u payload=%u mesh_bytes=%u slots=%u mesh_groups=%u shared=%u threads_per_patch=%u",
            double(hs_declarations.max_factor), tess_allocation.factor, tess_allocation.payload_bytes,
            tess_allocation.mesh_bytes, tess_allocation.mesh_slots, tess_allocation.mesh_groups,
            tess_allocation.threadgroup_bytes, ref_hs.ThreadsPerPatch);
      }
      if (geometry && (!ref_vs.NumOutputElement || ref_vs.NumOutputElement > 32 ||
                       16ull + (contracted_geometry ? uint64_t(gs_point::vertices_per_group(geometry_contract.primitive)) : 30ull) * ref_vs.NumOutputElement * 16ull > 16256ull))
        return TraceUnsupportedPipeline(__func__, __LINE__);
      SM50_SHADER_IA_INPUT_LAYOUT_DATA data_ia_layout = {};
      data_ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
      data_ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
      std::vector<SM50_IA_INPUT_ELEMENT> elements(pDesc->InputLayout.NumElements);
      hr = ExtractMTLInputLayoutElements(
          device_, pDesc->VS.pShaderBytecode, pDesc->InputLayout.pInputElementDescs, pDesc->InputLayout.NumElements,
          elements.data(), &data_ia_layout.num_elements
      );
      if (FAILED(hr)) {
        return hr;
      }
      elements.resize(data_ia_layout.num_elements);
      data_ia_layout.elements = elements.data();
      slot_mask = 0;
      for (auto &element : elements) {
        slot_mask |= (1 << element.slot);
      }
      data_ia_layout.slot_mask = slot_mask;
      data_ia_layout.next = pDesc->GS.pShaderBytecode && !geometry ? (void *)&gs_passthrough : (void *)&common;

      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
      rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
      if (pDesc->pRootSignature) {
        rootsig.bytecode_length =
            static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&rootsig.bytecode);
      } else {
        rootsig.bytecode = pDesc->VS.pShaderBytecode;
        rootsig.bytecode_length = pDesc->VS.BytecodeLength;
      }
      rootsig.next = &data_ia_layout;

      SM50_SHADER_PSO_TESSELLATOR_DATA tess_args = {};
      SM50_SHADER_TESS_SPLIT_WORKLOAD_DATA tess_split = {};
      SM50_SHADER_TESS_VERTEX_LINKAGE_DATA tess_vertex_link = {};
      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_vs = {};
      SM50_SHADER_PSO_GEOMETRY_SHADER_DATA geometry_args = {};
      if (mesh_pipeline && (!rootsig.bytecode || !rootsig.bytecode_length))
        return E_INVALIDARG;
      if (tessellation) {
        tess_args.type = SM50_SHADER_PSO_TESSELLATOR;
        tess_args.max_potential_tess_factor = tess_allocation.factor;
        tess_args.next = &common;
        if (tess_allocation.split_workload) {
          tess_split.type = SM50_SHADER_TESS_SPLIT_WORKLOAD;
          tess_split.next = &common;
          tess_args.next = &tess_split;
        }
        data_ia_layout.next = &tess_args;
        if (!vertex_link.identity) {
          tess_vertex_link.type = SM50_SHADER_TESS_VERTEX_LINKAGE;
          tess_vertex_link.next = &tess_args;
          data_ia_layout.next = &tess_vertex_link;
        }
        // The combined compiler uses ROOT_SIGNATURE for HS and ROOT_SIGNATURE2
        // for VS. Omitting the second silently selects D3D11 bindings for VS.
        rootsig_vs = rootsig;
        rootsig_vs.type = SM50_SHADER_ROOT_SIGNATURE2;
        rootsig.next = &rootsig_vs;
      } else if (geometry) {
        geometry_args.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
        geometry_args.strip_topology = false;
        geometry_args.next = &data_ia_layout;
        rootsig.next = &geometry_args;
      }

      auto compile_vertex_function = [&](const char *name, WMT::Reference<WMT::Function> &function) -> HRESULT {
        SM50ShaderBitcode bitcode;
        SM50Error compile_error;
        tess_split.applied = 0;
        tess_vertex_link.applied = 0;
        auto *args = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig;
        diagnostic::Add(tessellation ? diagnostic::Counter::CompileHS : diagnostic::Counter::CompileVS);
        const int result = geometry
            ? SM50CompileGeometryPipelineVertex(shader_vs, shader_gs, args, name, &bitcode, &compile_error)
            : tessellation
                ? SM50CompileTessellationPipelineHull(shader_vs, shader_hs, args, name, &bitcode, &compile_error)
                : SM50Compile(shader_vs, args, name, &bitcode, &compile_error);
        if (result || !sm50_bitcode_t(bitcode) ||
            (tessellation && !vertex_link.identity && tess_vertex_link.applied != SM50_TESS_VERTEX_LINKAGE_APPLIED) ||
            (tess_allocation.split_workload && tess_split.applied != SM50_TESS_SPLIT_OBJECT_APPLIED)) {
          ERR("Failed to compile ", name);
          return E_FAIL;
        }

        SM50_COMPILED_BITCODE compiled = {};
        SM50GetCompiledBitcode(bitcode, &compiled);
        if (!compiled.Data || !compiled.Size)
          return E_FAIL;
        auto data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
        auto library = metal.newLibrary(data, err);
        if (err || !library) {
          ERR("Failed to create library for ", name);
          return E_FAIL;
        }
        function = library.newFunction(name);
        if (!function) {
          ERR("Failed to create function ", name);
          return E_FAIL;
        }
        return S_OK;
      };
      if (FAILED(hr = compile_vertex_function(geometry ? "vsgs_main" : tessellation ? "vshs_main" : "vs_main", vs_func)))
        return hr;

      if (tessellation || geometry) {
        const SM50_INDEX_BUFFER_FORMAT indexed_formats[2] = {
            SM50_INDEX_BUFFER_FORMAT_UINT16, SM50_INDEX_BUFFER_FORMAT_UINT32
        };
        const char *indexed_names[2] = {
            geometry ? "vsgs_main_uint16" : "vshs_main_uint16",
            geometry ? "vsgs_main_uint32" : "vshs_main_uint32"
        };
        for (unsigned i = 0; i < 2; ++i) {
          // Keep the existing root/geometry/IA or HS/VS/IA chain for every variant.
          data_ia_layout.index_buffer_format = indexed_formats[i];
          if (FAILED(hr = compile_vertex_function(indexed_names[i], indexed_vs_func[i])))
            return hr;
        }
        data_ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
      }
      if (tessellation) {
        SM50_SHADER_GS_PASS_THROUGH_DATA domain_passthrough = {};
        domain_passthrough.type = SM50_SHADER_GS_PASS_THROUGH;
        domain_passthrough.DataEncoded = ~0u;
        domain_passthrough.RasterizationDisabled = false;
        domain_passthrough.next = &tess_args;
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_ds = rootsig;
        rootsig_ds.next = &domain_passthrough;
        SM50ShaderBitcode ds_bitcode;
        SM50Error ds_error;
        tess_split.applied = 0;
        diagnostic::Add(diagnostic::Counter::CompileDS);
        if (SM50CompileTessellationPipelineDomain(
                shader_hs, shader_ds, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig_ds,
                "ds_main", &ds_bitcode, &ds_error
            ) || !sm50_bitcode_t(ds_bitcode) ||
            (tess_allocation.split_workload && tess_split.applied != SM50_TESS_SPLIT_DOMAIN_APPLIED)) {
          ERR("Failed to compile ds_main");
          return E_FAIL;
        }
        SM50_COMPILED_BITCODE ds_bitcode_compiled = {};
        SM50GetCompiledBitcode(ds_bitcode, &ds_bitcode_compiled);
        if (!ds_bitcode_compiled.Data || !ds_bitcode_compiled.Size)
          return E_FAIL;
        auto ds_data = WMT::MakeDispatchData(ds_bitcode_compiled.Data, ds_bitcode_compiled.Size);
        auto ds_lib = metal.newLibrary(ds_data, err);
        if (err || !ds_lib) {
          ERR("Failed to create domain library");
          return E_FAIL;
        }
        ds_func = ds_lib.newFunction("ds_main");
        if (!ds_func) {
          ERR("Failed to create function ds_main");
          return E_FAIL;
        }
      } else if (geometry) {
        // Unlike VS+HS, these are separate functions: each compiler consumes
        // ROOT_SIGNATURE with its own VS/GS visibility, never ROOT_SIGNATURE2.
        SM50_SHADER_PSO_GEOMETRY_SHADER_DATA gs_args = geometry_args;
        gs_args.next = &common;
        SM50_SHADER_ROOT_SIGNATURE_DATA rootsig_gs = rootsig;
        rootsig_gs.next = &gs_args;
        SM50ShaderBitcode gs_bitcode;
        SM50Error gs_error;
        diagnostic::Add(diagnostic::Counter::CompileGS);
        if (SM50CompileGeometryPipelineGeometry(
                shader_vs, shader_gs, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig_gs,
                "gs_main", &gs_bitcode, &gs_error
            ) || !sm50_bitcode_t(gs_bitcode)) {
          ERR("Failed to compile gs_main");
          return E_FAIL;
        }
        SM50_COMPILED_BITCODE compiled = {};
        SM50GetCompiledBitcode(gs_bitcode, &compiled);
        if (!compiled.Data || !compiled.Size)
          return E_FAIL;
        auto data = WMT::MakeDispatchData(compiled.Data, compiled.Size);
        auto library = metal.newLibrary(data, err);
        if (err || !library) {
          ERR("Failed to create geometry library");
          return E_FAIL;
        }
        gs_func = library.newFunction("gs_main");
        if (!gs_func) {
          ERR("Failed to create function gs_main");
          return E_FAIL;
        }
      }
    } else {
      ERR("no vertex shader");
      return E_INVALIDARG;
    }

    WMTRenderPipelineInfo info;
    WMT::InitializeRenderPipelineInfo(info);
    WMTMeshRenderPipelineInfo mesh_info;
    WMT::InitializeMeshRenderPipelineInfo(mesh_info);

    bool dual_source_blending = false;

    // InitializePSO consumes HasCoverageOutput, including when no PS is present.
    if (pDesc->PS.pShaderBytecode && FAILED(hr = initialize_stage("PS", pDesc->PS, &shader_ps, &ref_ps)))
      return hr;
    hr = mesh_pipeline ? InitializePSO(pDesc, mesh_info, dual_source_blending)
                       : InitializePSO(pDesc, info, dual_source_blending);
    if (FAILED(hr))
      return hr;

    if (pDesc->PS.pShaderBytecode) {
      auto sha1 = Sha1HashState::compute(pDesc->PS.pShaderBytecode, pDesc->PS.BytecodeLength);

      std::string ps_name = "ps_main" + sha1.string().substr(0, 8);

      SM50_SHADER_PSO_PIXEL_SHADER_DATA data_ps;
      data_ps.dual_source_blending = dual_source_blending;
      data_ps.disable_depth_output = false;
      data_ps.unorm_output_reg_mask = 0;
      data_ps.sample_mask = pDesc->SampleMask;
      data_ps.type = SM50_SHADER_PSO_PIXEL_SHADER;
      data_ps.next = &common;

      memset(data_ps.pixel_formats, 0, sizeof(data_ps.pixel_formats));
      for (unsigned i = 0; i < pDesc->NumRenderTargets; i++) {
        data_ps.pixel_formats[i] =
            ORIGINAL_FORMAT(mesh_pipeline ? mesh_info.colors[i].pixel_format : info.colors[i].pixel_format);
      }

      SM50_SHADER_ROOT_SIGNATURE_DATA rootsig;
      rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
      if (pDesc->pRootSignature) {
        rootsig.bytecode_length =
            static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&rootsig.bytecode);
      } else {
        rootsig.bytecode = pDesc->PS.pShaderBytecode;
        rootsig.bytecode_length = pDesc->PS.BytecodeLength;
      }
      rootsig.next = &data_ps;

      SM50_SHADER_PS_INPUT_LINKAGE_DATA pixel_linkage{};
      bool link_pixel = tessellation;
      if (geometry) {
        microsoft::CSignatureParser5 output;
        microsoft::CSignatureParser input;
        if (FAILED(microsoft::DXBCGetOutputSignature(pDesc->GS.pShaderBytecode, &output)) ||
            FAILED(microsoft::DXBCGetInputSignature(pDesc->PS.pShaderBytecode, &input)) ||
            output.NumStreams() != 1 || !output.Signature(0)) return E_INVALIDARG;
        // Preserve already-supported same-register subset inputs; relocation requires exact component widths.
        link_pixel = !TessellationSignaturesMatch(*output.Signature(0), input);
      }
      if (link_pixel) {
        hr = BuildTessellationPixelLink(*pDesc, pixel_linkage, geometry);
        if (FAILED(hr)) return hr;
        pixel_linkage.next = &data_ps;
        rootsig.next = &pixel_linkage;
      }

      SM50ShaderBitcode ps_bitcode;
      diagnostic::Add(diagnostic::Counter::CompilePS);
      if (SM50Compile(
              shader_ps, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&rootsig, ps_name.c_str(), &ps_bitcode, &sm50_err
          ) || !sm50_bitcode_t(ps_bitcode)) {
        ERR("Failed to compile ps shader");
        return E_FAIL;
      }
      // A provider without SM50_SHADER_PS_INPUT_LINKAGE skips the unknown node and
      // succeeds with the original, unlinked fragment input. Refuse that pairing
      // instead of rasterizing a pixel shader that reads the wrong register.
      if (link_pixel && pixel_linkage.applied != SM50_SHADER_PS_INPUT_LINKAGE_APPLIED) {
        ERR("CreatePipelineState: the AIR provider did not acknowledge the pixel input linkage argument");
        return E_NOTIMPL;
      }
      SM50_COMPILED_BITCODE ps_bitcode_compiled = {};
      SM50GetCompiledBitcode(ps_bitcode, &ps_bitcode_compiled);
      if (!ps_bitcode_compiled.Data || !ps_bitcode_compiled.Size)
        return E_FAIL;
      auto ps_data = WMT::MakeDispatchData(ps_bitcode_compiled.Data, ps_bitcode_compiled.Size);
      auto ps_lib = metal.newLibrary(ps_data, err);
      if (err || !ps_lib) {
        ERR("Failed to create pixel library");
        return E_FAIL;
      }
      ps_func = ps_lib.newFunction(ps_name.c_str());
      if (!ps_func) {
        ERR("Failed to create pixel function");
        return E_FAIL;
      }
    }

    // PSO
    {
      if (mesh_pipeline) {
        mesh_info.object_function = vs_func.handle;
        mesh_info.mesh_function = geometry ? gs_func.handle : ds_func.handle;
        mesh_info.fragment_function = ps_func.handle;
        mesh_info.payload_memory_length = geometry ? 16256 : tess_allocation.payload_bytes;
        mesh_info.object_tgsize_is_multiple_of_sgwidth = tessellation;
        mesh_info.mesh_tgsize_is_multiple_of_sgwidth = tessellation;
        const uint32_t root_buffers = (1u << SM50_BINDING_INDEX_ROOT_ARGUMENTS) |
                                      (1u << SM50_BINDING_INDEX_STATIC_SAMPLERS);
        mesh_info.immutable_object_buffers = root_buffers | (1u << SM50_BINDING_INDEX_VERTEX_BUFFER) |
                                             (1u << SM50_BINDING_INDEX_DRAW_ARGUMENTS);
        mesh_info.immutable_mesh_buffers = root_buffers;
        mesh_info.immutable_fragment_buffers = root_buffers;
        mesh_info.support_indirect_command_buffers = false;
        {
          WMT::Reference<WMT::RenderPipelineState> mesh_psos[3];
          for (unsigned i = 0; i < 3; ++i) {
            auto variant_info = mesh_info;
            if (i) {
              variant_info.object_function = indexed_vs_func[i - 1].handle;
              variant_info.immutable_object_buffers |= (1u << SM50_BINDING_INDEX_INDEX_BUFFER);
            }
            mesh_psos[i] = metal.newRenderPipelineState(variant_info, err);
            if (err || !mesh_psos[i]) {
              ERR("Failed to create mesh PSO variant ", i);
              return E_FAIL;
            }
          }
          // Publish only a complete set; failure of either indexed variant rejects the PSO.
          pso = mesh_psos[0];
          mesh_indexed_pso[0] = mesh_psos[1];
          mesh_indexed_pso[1] = mesh_psos[2];
        }
      } else {
        info.vertex_function = vs_func.handle;
        info.fragment_function = ps_func.handle;
        pso = metal.newRenderPipelineState(info, err);
      }

      if (!pso) {
        ERR("Failed to create PSO: ", err.description().getUTF8String());
        return E_FAIL;
      }
    }

    // DSSO
    InitializeDSSO(pDesc);

    InitializeRasterizerState(pDesc);

    geometry_pipeline = geometry;
    geometry_input_vertices = contracted_geometry ? gs_point::input_vertices(geometry_contract.primitive) : 3;
    geometry_vertices_per_group = contracted_geometry ? gs_point::vertices_per_group(geometry_contract.primitive) : 30;
    if (tessellation) {
      // Draw topology counts input control points, not reflected output registers.
      tess_control_points = hs_declarations.input_control_points;
      tess_threads_per_patch = ref_hs.ThreadsPerPatch;
    }

    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid)) {
      WARN("D3D12GraphicsPipelineState: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetCachedBlob(ID3DBlob **blob) {
    IMPLEMENT_ME
    return TraceUnsupportedPipeline(__func__, __LINE__);
  }

  virtual WMT::DepthStencilState
  GetDepthStencilState(UINT DSVPlanar, UINT DSVReadonlyFlags) {
    if (!DSVPlanar)
      return device_->default_depth_stencil_state;

    if (DSVPlanar == 1) {
      if (DSVReadonlyFlags & 1)
        return dsso_readonly_no_stencil;
      else
        return dsso_no_stencil;
    }

    assert(DSVPlanar == 3);

    switch (DSVReadonlyFlags) {
    case 3:
      return dsso_readonly;
    case 2:
      return dsso_stencil_readonly;
    case 1:
      return dsso_depth_readonly;
    default:
      return dsso;
    }
  }
};

HRESULT
CreateGraphicsPipelineState(
    MTLD3D12Device *pDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
) {
  shader_capture::Scope originals("graphics", pDevice, pDesc);
  InitReturnPtr(ppPipelineState);
  auto pso = Com(new MTLD3D12GraphicsPipelineStateImpl(pDevice));
  const uint32_t capture = AdmitPipelineCapture();
  if (capture == 2049)
    TracePipelineCapture(capture, pso.ptr(), "limit", "truncated=1 attempt_limit=2048");
  else if (capture) {
    pso->capture_id = capture;
    TracePipelineCapture(capture, pso.ptr(), "begin",
        "root=%p topology=%u rts=%u formats=%u,%u,%u,%u,%u,%u,%u,%u dsv=%u samples=%u vs=%llu ps=%llu",
        static_cast<void *>(pDesc->pRootSignature), unsigned(pDesc->PrimitiveTopologyType), pDesc->NumRenderTargets,
        unsigned(pDesc->RTVFormats[0]), unsigned(pDesc->RTVFormats[1]), unsigned(pDesc->RTVFormats[2]), unsigned(pDesc->RTVFormats[3]),
        unsigned(pDesc->RTVFormats[4]), unsigned(pDesc->RTVFormats[5]), unsigned(pDesc->RTVFormats[6]), unsigned(pDesc->RTVFormats[7]),
        unsigned(pDesc->DSVFormat), pDesc->SampleDesc.Count, (unsigned long long)pDesc->VS.BytecodeLength,
        (unsigned long long)pDesc->PS.BytecodeLength);
  }
  HRESULT hr = pso->Initialize(pDesc);
  if (FAILED(hr) && pso->capture_id) {
    TraceRejectedPipelineBytecode(pso->capture_id, pso.ptr(), "VS", pDesc->VS);
    TraceRejectedPipelineBytecode(pso->capture_id, pso.ptr(), "PS", pDesc->PS);
    TraceRejectedPipelineBytecode(pso->capture_id, pso.ptr(), "HS", pDesc->HS);
    TraceRejectedPipelineBytecode(pso->capture_id, pso.ptr(), "DS", pDesc->DS);
    TraceRejectedPipelineBytecode(pso->capture_id, pso.ptr(), "GS", pDesc->GS);
  }
  if (SUCCEEDED(hr)) hr = pso->QueryInterface(riid, ppPipelineState);
  if (pso->capture_id)
    TracePipelineCapture(pso->capture_id, pso.ptr(), "result", "hr=%08x output=%p native=%llx",
                         unsigned(hr), ppPipelineState ? *ppPipelineState : nullptr, (unsigned long long)pso->pso.handle);
  return originals.finish(hr);
};

} // namespace dxmt
