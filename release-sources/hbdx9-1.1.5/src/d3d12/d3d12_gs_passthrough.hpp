// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"
#include <array>
#include <cstring>
#include <vector>

namespace dxmt {

// Elision is safe only for this deliberately small identity program. The
// converter's legacy reflection payload alone does not prove GS equivalence.
inline bool
ValidateGSPassthrough(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, microsoft::CDXBCParser &container,
    uint32_t code_index, MTL_GEOMETRY_SHADER_PASS_THROUGH &remap, bool &topology_mismatch
) {
  using namespace microsoft;
  topology_mismatch = false;
  CSignatureParser vs, input, ps;
  CSignatureParser5 output;
  CDXBCParser other;
  if (!desc.VS.pShaderBytecode ||
      FAILED(other.ReadDXBC(desc.VS.pShaderBytecode, desc.VS.BytecodeLength)) ||
      FAILED(DXBCGetOutputSignature(desc.VS.pShaderBytecode, &vs)) ||
      FAILED(DXBCGetInputSignature(desc.GS.pShaderBytecode, &input)) ||
      FAILED(DXBCGetOutputSignature(desc.GS.pShaderBytecode, &output)) ||
      output.NumStreams() != 1 || output.RasterizedStream() != 0)
    return false;
  if (desc.PS.pShaderBytecode &&
      (FAILED(other.ReadDXBC(desc.PS.pShaderBytecode, desc.PS.BytecodeLength)) ||
       FAILED(DXBCGetInputSignature(desc.PS.pShaderBytecode, &ps))))
    return false;

  const D3D11_SIGNATURE_PARAMETER *v, *in, *out, *p;
  const auto nv = vs.GetParameters(&v), ni = input.GetParameters(&in);
  const auto no = output.Signature(0)->GetParameters(&out), np = ps.GetParameters(&p);
  if (!no || nv != ni || ni != no) return false;
  std::array<uint8_t, 32> masks = {};
  remap = {255, 255, 255, 255};
  auto same = [](const auto &a, const auto &b) {
    return a.Register == b.Register && a.Mask == b.Mask && a.ComponentType == b.ComponentType &&
           a.MinPrecision == b.MinPrecision && a.SystemValue == b.SystemValue &&
           a.SemanticIndex == b.SemanticIndex && !strcasecmp(a.SemanticName, b.SemanticName);
  };
  for (UINT i = 0; i < no; ++i) {
    const auto &o = out[i];
    if (o.Register >= masks.size() || !o.Mask || (o.Mask & ~15u) || (masks[o.Register] & o.Mask) ||
        o.MinPrecision != D3D_MIN_PRECISION_DEFAULT || (o.NeverWrites_Mask & o.Mask) || o.Stream)
      return false;
    const D3D11_SIGNATURE_PARAMETER *source = nullptr;
    for (UINT j = 0; j < ni; ++j)
      if (in[j].Register == o.Register && in[j].Mask == o.Mask) source = &in[j];
    if (!source || source->ComponentType != o.ComponentType || source->MinPrecision != o.MinPrecision)
      return false;
    bool linked = false;
    for (UINT j = 0; j < nv; ++j)
      if (same(v[j], *source) && !(v[j].NeverWrites_Mask & v[j].Mask)) linked = true;
    if (!linked) return false;
    if (o.SystemValue == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX ||
        o.SystemValue == D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX) {
      if (source->SystemValue != D3D10_SB_NAME_UNDEFINED ||
          o.ComponentType != D3D10_SB_REGISTER_COMPONENT_UINT32 || (o.Mask & (o.Mask - 1)))
        return false;
      auto &reg = o.SystemValue == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX
                      ? remap.RenderTargetArrayIndexReg : remap.ViewportArrayIndexReg;
      auto &lane = o.SystemValue == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX
                       ? remap.RenderTargetArrayIndexComponent : remap.ViewportArrayIndexComponent;
      if (reg != 255) return false;
      reg = o.Register;
      lane = __builtin_ctz(o.Mask);
    } else if (!same(o, *source) ||
               (o.SystemValue != D3D10_SB_NAME_UNDEFINED && o.SystemValue != D3D10_SB_NAME_POSITION &&
                o.SystemValue != D3D10_SB_NAME_CLIP_DISTANCE)) {
      return false;
    }
    masks[o.Register] |= o.Mask;
  }
  for (UINT i = 0; i < np; ++i) {
    if (p[i].SystemValue == D3D10_SB_NAME_PRIMITIVE_ID || p[i].Register >= masks.size()) return false;
    bool linked = false;
    for (UINT j = 0; j < no; ++j) {
      auto expected = out[j];
      expected.Mask = p[i].Mask;
      if (!(p[i].Mask & ~out[j].Mask) && same(p[i], expected)) linked = true;
    }
    if (!linked) return false;
  }

  const auto bytes = container.GetBlobSize(code_index);
  if (bytes < 8 || bytes % sizeof(uint32_t)) return false;
  std::vector<uint32_t> words(bytes / sizeof(uint32_t));
  memcpy(words.data(), container.GetBlob(code_index), bytes);
  if (words[1] != words.size()) return false;
  unsigned vertices = 0, emits = 0, declared_vertices = 0;
  bool saw_input = false, saw_output = false, saw_max = false, body = false, cut = false, ret = false;
  std::array<uint8_t, 32> written = {};
  std::array<uint8_t, 32> declared_input = {}, declared_output = {};
  for (size_t at = 2; at < words.size();) {
    const auto token = words[at];
    const auto op = DECODE_D3D10_SB_OPCODE_TYPE(token);
    const auto n = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
    if (!n || n > words.size() - at || (token & 0x80000000u) || ret) return false;
    const bool stream_zero = n == 3 && !DECODE_IS_D3D10_SB_OPERAND_EXTENDED(words[at + 1]) &&
        DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(words[at + 1]) == D3D10_SB_OPERAND_0_COMPONENT &&
        DECODE_D3D10_SB_OPERAND_TYPE(words[at + 1]) == D3D11_SB_OPERAND_TYPE_STREAM &&
        DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(words[at + 1]) == D3D10_SB_OPERAND_INDEX_1D &&
        DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, words[at + 1]) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 &&
        words[at + 2] == 0;
    switch (op) {
    case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE: {
      if (body || n != 1 || saw_input) return false;
      saw_input = true;
      const auto prim = DECODE_D3D10_SB_GS_INPUT_PRIMITIVE(token);
      vertices = prim == D3D10_SB_PRIMITIVE_POINT ? 1 : prim == D3D10_SB_PRIMITIVE_LINE ? 2 :
                 prim == D3D10_SB_PRIMITIVE_TRIANGLE ? 3 : 0;
      if (!vertices) return false;
      const auto topology = vertices == 1 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT :
                            vertices == 2 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      if (topology != desc.PrimitiveTopologyType) { topology_mismatch = true; return false; }
      break;
    }
    case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY: {
      if (body || n != 1 || saw_output || !saw_input) return false;
      saw_output = true;
      const auto topology = DECODE_D3D10_SB_GS_OUTPUT_PRIMITIVE_TOPOLOGY(token);
      if (topology != (vertices == 1 ? D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST :
                       vertices == 2 ? D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP))
        return false;
      break;
    }
    case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:
      if (body || n != 2 || saw_max || !saw_input || words[at + 1] != vertices) return false;
      saw_max = true;
      break;
    case D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT:
      if (body || n != 2 || words[at + 1] != 1) return false;
      break;
    case D3D11_SB_OPCODE_DCL_STREAM:
      if (body || !stream_zero) return false;
      break;
    case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS:
      if (body || n != 1) return false;
      break;
    case D3D10_SB_OPCODE_DCL_INPUT:
    case D3D10_SB_OPCODE_DCL_INPUT_SIV:
    case D3D10_SB_OPCODE_DCL_INPUT_SGV:
    case D3D10_SB_OPCODE_DCL_OUTPUT:
    case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
    case D3D10_SB_OPCODE_DCL_OUTPUT_SGV: {
      const bool is_input = op == D3D10_SB_OPCODE_DCL_INPUT || op == D3D10_SB_OPCODE_DCL_INPUT_SIV ||
                            op == D3D10_SB_OPCODE_DCL_INPUT_SGV;
      const bool system = op != D3D10_SB_OPCODE_DCL_INPUT && op != D3D10_SB_OPCODE_DCL_OUTPUT;
      if (body || n != unsigned(3 + is_input + system)) return false;
      const auto operand = words[at + 1];
      if (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(operand) ||
          DECODE_D3D10_SB_OPERAND_TYPE(operand) !=
              (is_input ? D3D10_SB_OPERAND_TYPE_INPUT : D3D10_SB_OPERAND_TYPE_OUTPUT) ||
          DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(operand) != D3D10_SB_OPERAND_4_COMPONENT ||
          DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(operand) != D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE ||
          DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(operand) !=
              (is_input ? D3D10_SB_OPERAND_INDEX_2D : D3D10_SB_OPERAND_INDEX_1D) ||
          DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, operand) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
          (is_input && DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, operand) !=
                           D3D10_SB_OPERAND_INDEX_IMMEDIATE32))
        return false;
      if (is_input) {
        const auto count = words[at + 2];
        if (!count || count > 3 || (declared_vertices && declared_vertices != count)) return false;
        declared_vertices = count;
      }
      const auto reg = words[at + 2 + is_input];
      const auto mask = DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(operand) >> 4;
      const auto name = system ? words[at + n - 1] : unsigned(D3D10_SB_NAME_UNDEFINED);
      auto &declared = is_input ? declared_input : declared_output;
      if (reg >= masks.size() || !mask || (mask & ~masks[reg]) || (mask & declared[reg])) return false;
      unsigned matched = 0;
      const auto *signature = is_input ? in : out;
      for (UINT i = 0; i < no; ++i)
        if (signature[i].Register == reg && unsigned(signature[i].SystemValue) == name)
          matched |= signature[i].Mask;
      if (mask & ~matched) return false;
      declared[reg] |= mask;
      break;
    }
    case D3D10_SB_OPCODE_MOV: {
      body = true;
      if (cut || n != 6 || emits >= vertices || (token & D3D10_SB_INSTRUCTION_SATURATE_MASK)) return false;
      const auto dst = words[at + 1], src = words[at + 3], reg = words[at + 2];
      if (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(dst) || DECODE_IS_D3D10_SB_OPERAND_EXTENDED(src) ||
          DECODE_D3D10_SB_OPERAND_TYPE(dst) != D3D10_SB_OPERAND_TYPE_OUTPUT ||
          DECODE_D3D10_SB_OPERAND_TYPE(src) != D3D10_SB_OPERAND_TYPE_INPUT ||
          DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(dst) != D3D10_SB_OPERAND_INDEX_1D ||
          DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(src) != D3D10_SB_OPERAND_INDEX_2D ||
          DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, dst) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
          DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, src) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
          DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, src) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
          reg >= masks.size() || reg != words[at + 5] || words[at + 4] != emits ||
          DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(dst) != D3D10_SB_OPERAND_4_COMPONENT ||
          DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(src) != D3D10_SB_OPERAND_4_COMPONENT ||
          DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(dst) != D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE)
        return false;
      const auto mask = DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(dst) >> 4;
      if (!mask || (mask & ~masks[reg])) return false;
      const auto selection = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(src);
      for (unsigned lane = 0; lane < 4; ++lane) if (mask & (1u << lane)) {
        const auto source_lane = selection == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE
            ? DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(src, lane)
            : selection == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE
                ? DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(src) : D3D10_SB_4_COMPONENT_NAME(4);
        if (unsigned(source_lane) != lane) return false;
      }
      written[reg] |= mask;
      break;
    }
    case D3D11_SB_OPCODE_EMIT_STREAM:
    case D3D10_SB_OPCODE_EMIT:
      body = true;
      if ((op == D3D10_SB_OPCODE_EMIT ? n != 1 : !stream_zero) || cut || emits >= vertices || written != masks)
        return false;
      ++emits;
      written.fill(0);
      break;
    case D3D11_SB_OPCODE_CUT_STREAM:
    case D3D10_SB_OPCODE_CUT:
      body = true;
      if ((op == D3D10_SB_OPCODE_CUT ? n != 1 : !stream_zero) || cut || emits != vertices ||
          written != std::array<uint8_t, 32>{}) return false;
      cut = true;
      break;
    case D3D10_SB_OPCODE_RET:
      if (n != 1 || emits != vertices || written != std::array<uint8_t, 32>{}) return false;
      ret = true;
      break;
    default: return false;
    }
    at += n;
  }
  return saw_input && saw_output && saw_max && ret && emits == vertices && declared_vertices == vertices &&
         declared_input == masks && declared_output == masks;
}

} // namespace dxmt
