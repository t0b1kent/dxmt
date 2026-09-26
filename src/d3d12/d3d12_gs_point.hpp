// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gs_point {
using namespace microsoft;

struct Contract {
  uint32_t primitive = 0, max_vertices = 0, stream_mask = 0, topology = 0, instances = 1;
  std::array<uint32_t, 14> cbuffers{};
};

inline uint32_t input_vertices(uint32_t primitive) {
  switch (primitive) {
  case D3D10_SB_PRIMITIVE_POINT: return 1;
  case D3D10_SB_PRIMITIVE_LINE: return 2;
  case D3D10_SB_PRIMITIVE_TRIANGLE: return 3;
  default: return 0;
  }
}

inline uint32_t vertices_per_group(uint32_t primitive) {
  const auto vertices = input_vertices(primitive);
  return vertices ? 32 / vertices * vertices : 0;
}

inline bool capacity(uint32_t vs_registers, uint32_t output_scalars, uint32_t vertices,
                     uint32_t primitive = D3D10_SB_PRIMITIVE_POINT) {
  const auto group = vertices_per_group(primitive);
  return group && vs_registers && vs_registers <= 32 && 16ull + uint64_t(group) * vs_registers * 16ull <= 16256ull &&
         output_scalars && output_scalars <= 128 && vertices && vertices <= 256 &&
         uint64_t(output_scalars) * vertices <= 1024;
}

inline bool draw_groups(uint32_t vertices, uint32_t instances, uint32_t &groups) {
  groups = vertices ? 1 + (vertices - 1) / 32 : 0;
  return vertices && instances && uint64_t(groups) * instances <= 1024;
}

// Same offline-witness trust boundary as DXILTS01: both complete container
// hashes are bound, and declarations/resources must match the admitted source.
inline bool witness(const uint8_t *bytes, size_t size, const uint8_t *source, const uint8_t *target,
                    const Contract &c) {
  if (size != 136 || memcmp(bytes, "DXILGS01", 8) || memcmp(bytes + 16, source, 20) ||
      memcmp(bytes + 36, target, 20)) return false;
  const auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, bytes + at, 4); return v; };
  if (u32(8) != 1 || u32(12) != 2 || u32(56) != c.primitive || u32(60) != c.max_vertices ||
      u32(64) != c.stream_mask || u32(68) != c.topology || u32(72) != c.instances || u32(76)) return false;
  for (unsigned i = 0; i < c.cbuffers.size(); ++i)
    if (u32(80 + i * 4) != c.cbuffers[i]) return false;
  return true;
}

struct Operand {
  uint32_t type = 0, mask = 0;
  std::array<uint32_t, 3> index{};
  std::array<bool, 3> relative{};
  std::array<uint32_t, 3> relative_register{};
  unsigned dimensions = 0;
};

inline bool operand(const std::vector<uint32_t> &w, size_t &at, size_t end, Operand &v,
                    bool destination, unsigned depth = 0) {
  if (at >= end || depth > 1) return false;
  const uint32_t token = w[at++];
  v.type = DECODE_D3D10_SB_OPERAND_TYPE(token);
  v.dimensions = DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token);
  const auto components = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token);
  if (v.type == D3D10_SB_OPERAND_TYPE_NULL)
    return destination && token == (uint32_t(D3D10_SB_OPERAND_TYPE_NULL) << 12);
  // Immediate vectors carry literal words, not an ordinary register swizzle.
  if (v.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
    const size_t count = components == D3D10_SB_OPERAND_4_COMPONENT ? 4 : components == D3D10_SB_OPERAND_1_COMPONENT ? 1 : 0;
    if (destination || v.dimensions || !count || count > end - at ||
        token != (uint32_t(components) | (uint32_t(D3D10_SB_OPERAND_TYPE_IMMEDIATE32) << 12))) return false;
    v.mask = (1u << count) - 1; at += count; return true;
  }
  if (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(token)) {
    if (destination || at >= end) return false;
    const uint32_t e = w[at++];
    if (DECODE_D3D10_SB_EXTENDED_OPERAND_TYPE(e) != D3D10_SB_EXTENDED_OPERAND_MODIFIER ||
        DECODE_D3D10_SB_OPERAND_MODIFIER(e) > D3D10_SB_OPERAND_MODIFIER_ABSNEG ||
        (e & ~(D3D10_SB_EXTENDED_OPERAND_TYPE_MASK | D3D10_SB_OPERAND_MODIFIER_MASK))) return false;
  }
  if (components == D3D10_SB_OPERAND_4_COMPONENT) {
    const auto selection = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(token);
    if (destination) {
      if (selection != D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) return false;
      v.mask = DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(token) >> 4;
    } else if (selection == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) {
      for (unsigned i = 0; i < 4; ++i) v.mask |= 1u << ((token >> (4 + 2 * i)) & 3);
    } else if (selection == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE)
      v.mask = 1u << ((token >> 4) & 3);
    else return false;
  } else if (components == D3D10_SB_OPERAND_1_COMPONENT) v.mask = 1;
  else if (components != D3D10_SB_OPERAND_0_COMPONENT) return false;
  for (unsigned i = 0; i < v.dimensions; ++i) {
    const auto representation = DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(i, token);
    if (representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 || representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE) {
      if (at == end) return false;
      v.index[i] = w[at++];
    } else if (representation != D3D10_SB_OPERAND_INDEX_RELATIVE) return false;
    if (representation == D3D10_SB_OPERAND_INDEX_RELATIVE || representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE) {
      Operand relative;
      if (!operand(w, at, end, relative, false, depth + 1) || relative.type != D3D10_SB_OPERAND_TYPE_TEMP ||
          relative.dimensions != 1 || relative.relative[0] || relative.index[0] >= 4096 ||
          !relative.mask || (relative.mask & (relative.mask - 1))) return false;
      v.relative[i] = true;
      v.relative_register[i] = relative.index[0];
    }
  }
  return true;
}

// This is an additional, source-witness-gated list-primitive path. It is not a substitute
// for the unchanged straight-line triangle validator or offline compiler proof.
inline bool validate(const std::vector<uint32_t> &w, const std::array<uint8_t, 32> &inputs,
                     const std::array<uint8_t, 32> &outputs, Contract &contract) {
  if (w.size() < 3 || w.size() > 65536 || w[0] != 0x00020050u || w[1] != w.size()) return false;
  contract = {};
  std::array<uint8_t, 32> declared_in{}, declared_out{};
  std::array<std::array<uint32_t, 2>, 32> indexable{};
  struct Resource { uint32_t dimension = 0, stride = 0, returns = 0; };
  std::array<Resource, 128> resources{};
  std::array<bool, 16> samplers{};
  uint32_t immediate_constants = 0;
  uint32_t temps = 0;
  uint32_t declared_input_vertices = 0;
  bool body = false, primitive = false, topology = false, max_vertices = false, instance = false, stream = false, temp_decl = false, returned = false;
  std::vector<unsigned> flow;
  for (size_t at = 2; at < w.size();) {
    const uint32_t token = w[at], op = DECODE_D3D10_SB_OPCODE_TYPE(token);
    size_t n = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
    if (op == D3D10_SB_OPCODE_CUSTOMDATA) {
      if (body || immediate_constants || at + 1 >= w.size() ||
          token != (D3D10_SB_OPCODE_CUSTOMDATA | (D3D10_SB_CUSTOMDATA_DCL_IMMEDIATE_CONSTANT_BUFFER << 11))) return false;
      n = w[at + 1];
      if (n < 6 || n > w.size() - at || (n - 2) % 4 || (n - 2) / 4 > 4096) return false;
      immediate_constants = (n - 2) / 4; at += n; continue;
    }
    if (!n || n > w.size() - at) return false;
    const size_t end = at + n;
    size_t operands_at = at + 1;
    uint32_t extension_mask = 0, resource_dimension = 0, resource_stride = 0, resource_returns = 0;
    const bool texture_op = op == D3D10_SB_OPCODE_LD || op == D3D10_SB_OPCODE_SAMPLE_L;
    const bool resource_op = texture_op || op == D3D11_SB_OPCODE_LD_STRUCTURED;
    for (uint32_t extended = token & 0x80000000u; extended;) {
      if (!resource_op || operands_at >= end) return false;
      const uint32_t e = w[operands_at++], type = DECODE_D3D10_SB_EXTENDED_OPCODE_TYPE(e);
      if (type < 1 || type > 3 || (extension_mask & (1u << type))) return false;
      extension_mask |= 1u << type;
      extended = e & 0x80000000u;
      if (type == D3D10_SB_EXTENDED_OPCODE_SAMPLE_CONTROLS) {
        if (!texture_op || (e & ~0x801ffe3fu)) return false;
      } else if (type == D3D11_SB_EXTENDED_OPCODE_RESOURCE_DIM) {
        if (e & ~0x807fffffu) return false;
        resource_dimension = DECODE_D3D11_SB_EXTENDED_RESOURCE_DIMENSION(e);
        resource_stride = DECODE_D3D11_SB_EXTENDED_RESOURCE_DIMENSION_STRUCTURE_STRIDE(e);
      } else {
        if (e & ~0x803fffffu) return false;
        resource_returns = (e >> 6) & 0xffff;
      }
    }
    unsigned sources = 0, destinations = 0;
    bool declaration = false;
    switch (op) {
    case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE:
      if (body || primitive || n != 1 || !input_vertices(DECODE_D3D10_SB_GS_INPUT_PRIMITIVE(token))) return false;
      primitive = true; contract.primitive = DECODE_D3D10_SB_GS_INPUT_PRIMITIVE(token); declaration = true; break;
    case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
      if (body || topology || n != 1) return false;
      contract.topology = DECODE_D3D10_SB_GS_OUTPUT_PRIMITIVE_TOPOLOGY(token);
      if (contract.topology != D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP &&
          contract.topology != D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP &&
          contract.topology != D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST) return false;
      topology = true; declaration = true; break;
    case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:
      if (body || max_vertices || n != 2 || !w[at + 1] || w[at + 1] > 256) return false;
      max_vertices = true; contract.max_vertices = w[at + 1]; declaration = true; break;
    case D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT:
      if (body || instance || n != 2 || w[at + 1] != 1) return false;
      instance = true; declaration = true; break;
    case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS:
      if (body || n != 1 || (DECODE_D3D10_SB_GLOBAL_FLAGS(token) & ~D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED)) return false;
      declaration = true; break;
    case D3D10_SB_OPCODE_DCL_TEMPS:
      if (body || temp_decl || n != 2 || w[at + 1] > 4096) return false;
      temp_decl = true; temps = w[at + 1]; declaration = true; break;
    case D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP:
      if (body || n != 4 || w[at + 1] >= indexable.size() || indexable[w[at + 1]][0] ||
          !w[at + 2] || w[at + 2] > 4096 || !w[at + 3] || w[at + 3] > 4) return false;
      indexable[w[at + 1]] = {{w[at + 2], w[at + 3]}}; declaration = true; break;
    case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER: {
      if (body || n != 4) return false;
      Operand v; size_t cursor = at + 1;
      if (!operand(w, cursor, end, v, false) || cursor != end || v.type != D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER ||
          v.dimensions != 2 || v.relative[0] || v.relative[1] || v.index[0] >= contract.cbuffers.size() ||
          contract.cbuffers[v.index[0]] || !v.index[1] || v.index[1] > 4096) return false;
      contract.cbuffers[v.index[0]] = v.index[1]; declaration = true; break;
    }
    case D3D10_SB_OPCODE_DCL_SAMPLER: {
      if (body || n != 3 || DECODE_D3D10_SB_SAMPLER_MODE(token) != D3D10_SB_SAMPLER_MODE_DEFAULT) return false;
      Operand v; size_t cursor = at + 1;
      if (!operand(w, cursor, end, v, false) || cursor != end || v.type != D3D10_SB_OPERAND_TYPE_SAMPLER ||
          v.mask || v.dimensions != 1 || v.relative[0] || v.index[0] >= samplers.size() || samplers[v.index[0]]) return false;
      samplers[v.index[0]] = true; declaration = true; break;
    }
    case D3D10_SB_OPCODE_DCL_RESOURCE:
    case D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED: {
      if (body || n != 4) return false;
      Operand v; size_t cursor = at + 1;
      if (!operand(w, cursor, end - 1, v, false) || cursor != end - 1 || v.type != D3D10_SB_OPERAND_TYPE_RESOURCE ||
          v.mask || v.dimensions != 1 || v.relative[0] || v.index[0] >= resources.size() || resources[v.index[0]].dimension) return false;
      auto &r = resources[v.index[0]];
      if (op == D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED) {
        r.dimension = D3D11_SB_RESOURCE_DIMENSION_STRUCTURED_BUFFER; r.stride = w[end - 1]; r.returns = 0x6666;
        if (!r.stride || r.stride > 2048 || r.stride % 4) return false;
      } else {
        r.dimension = DECODE_D3D10_SB_RESOURCE_DIMENSION(token); r.returns = w[end - 1];
        if (!r.dimension || r.dimension > D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY ||
            r.dimension == D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS || r.dimension == D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY ||
            (token & D3D10_SB_RESOURCE_SAMPLE_COUNT_MASK) || (r.returns & ~0xffffu)) return false;
        for (unsigned c = 0; c < 4; ++c) {
          const auto type = (r.returns >> (c * 4)) & 15;
          if (type < D3D10_SB_RETURN_TYPE_UNORM || type > D3D10_SB_RETURN_TYPE_FLOAT) return false;
        }
      }
      declaration = true; break;
    }
    case D3D10_SB_OPCODE_DCL_INPUT:
    case D3D10_SB_OPCODE_DCL_INPUT_SIV:
    case D3D10_SB_OPCODE_DCL_OUTPUT:
    case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
      const bool input = op == D3D10_SB_OPCODE_DCL_INPUT || op == D3D10_SB_OPCODE_DCL_INPUT_SIV;
      const bool system = op == D3D10_SB_OPCODE_DCL_INPUT_SIV || op == D3D10_SB_OPCODE_DCL_OUTPUT_SIV;
      if (body || n != size_t(3 + input + system)) return false;
      Operand v; size_t cursor = at + 1;
      if (!operand(w, cursor, end - system, v, true) || cursor != end - system || !v.mask ||
          v.type != (input ? D3D10_SB_OPERAND_TYPE_INPUT : D3D10_SB_OPERAND_TYPE_OUTPUT) ||
          v.dimensions != unsigned(1 + input) || v.relative[0] || v.relative[1]) return false;
      // FXC may place input declarations before the primitive declaration.
      if (input) {
        if (!v.index[0] || v.index[0] > 3 || (declared_input_vertices && v.index[0] != declared_input_vertices)) return false;
        declared_input_vertices = v.index[0];
      }
      const auto reg = v.index[input];
      auto &declared = input ? declared_in : declared_out;
      const auto &signature = input ? inputs : outputs;
      if (reg >= 32 || (v.mask & ~signature[reg]) || (declared[reg] & v.mask) ||
          (system && w[end - 1] != D3D10_SB_NAME_POSITION &&
           !(!input && w[end - 1] == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX && !(v.mask & (v.mask - 1))))) return false;
      declared[reg] |= v.mask; declaration = true; break;
    }
    case D3D11_SB_OPCODE_DCL_STREAM:
    case D3D11_SB_OPCODE_EMIT_STREAM:
    case D3D11_SB_OPCODE_CUT_STREAM:
    case D3D11_SB_OPCODE_EMITTHENCUT_STREAM: {
      if (n != 3) return false;
      Operand v; size_t cursor = at + 1;
      if (!operand(w, cursor, end, v, false) || cursor != end || v.type != D3D11_SB_OPERAND_TYPE_STREAM ||
          v.mask || v.dimensions != 1 || v.relative[0] || v.index[0]) return false;
      if (op == D3D11_SB_OPCODE_DCL_STREAM) {
        if (body || stream) return false;
        stream = true; declaration = true;
      }
      break;
    }
    case D3D10_SB_OPCODE_EMIT: case D3D10_SB_OPCODE_CUT: case D3D10_SB_OPCODE_EMITTHENCUT:
    case D3D10_SB_OPCODE_NOP:
      if (n != 1) return false;
      break;
    case D3D10_SB_OPCODE_IF: flow.push_back(1); sources = 1; break;
    case D3D10_SB_OPCODE_ELSE:
      if (n != 1 || flow.empty() || flow.back() != 1) return false;
      flow.back() = 2; break;
    case D3D10_SB_OPCODE_ENDIF:
      if (n != 1 || flow.empty() || flow.back() > 2) return false;
      flow.pop_back(); break;
    case D3D10_SB_OPCODE_LOOP:
      if (n != 1) return false;
      flow.push_back(3); break;
    case D3D10_SB_OPCODE_ENDLOOP:
      if (n != 1 || flow.empty() || flow.back() != 3) return false;
      flow.pop_back(); break;
    case D3D10_SB_OPCODE_BREAK: case D3D10_SB_OPCODE_CONTINUE:
    case D3D10_SB_OPCODE_BREAKC: case D3D10_SB_OPCODE_CONTINUEC: {
      bool loop = false; for (auto f : flow) loop |= f == 3;
      if (!loop) return false;
      sources = op == D3D10_SB_OPCODE_BREAKC || op == D3D10_SB_OPCODE_CONTINUEC ? 1 : 0;
      if (!sources && n != 1) return false;
      break;
    }
    case D3D10_SB_OPCODE_RET:
      if (n != 1) return false;
      returned = true; break;
    case D3D10_SB_OPCODE_MOV: case D3D10_SB_OPCODE_FTOI: case D3D10_SB_OPCODE_FTOU:
    case D3D10_SB_OPCODE_ITOF: case D3D10_SB_OPCODE_UTOF: case D3D10_SB_OPCODE_INEG:
    case D3D10_SB_OPCODE_NOT: case D3D10_SB_OPCODE_SQRT: case D3D10_SB_OPCODE_RSQ:
    case D3D10_SB_OPCODE_EXP: case D3D10_SB_OPCODE_LOG: case D3D10_SB_OPCODE_FRC:
    case D3D10_SB_OPCODE_ROUND_NE: case D3D10_SB_OPCODE_ROUND_NI: case D3D10_SB_OPCODE_ROUND_PI:
    case D3D10_SB_OPCODE_ROUND_Z: case D3D11_SB_OPCODE_F32TOF16: case D3D11_SB_OPCODE_F16TOF32:
      destinations = 1; sources = 1; break;
    case D3D10_SB_OPCODE_ADD: case D3D10_SB_OPCODE_MUL: case D3D10_SB_OPCODE_DIV:
    case D3D10_SB_OPCODE_MIN: case D3D10_SB_OPCODE_MAX: case D3D10_SB_OPCODE_DP2:
    case D3D10_SB_OPCODE_DP3: case D3D10_SB_OPCODE_DP4: case D3D10_SB_OPCODE_EQ:
    case D3D10_SB_OPCODE_NE: case D3D10_SB_OPCODE_GE: case D3D10_SB_OPCODE_LT:
    case D3D10_SB_OPCODE_IEQ: case D3D10_SB_OPCODE_INE: case D3D10_SB_OPCODE_IGE:
    case D3D10_SB_OPCODE_ILT: case D3D10_SB_OPCODE_UGE: case D3D10_SB_OPCODE_ULT:
    case D3D10_SB_OPCODE_IADD: case D3D10_SB_OPCODE_IMIN: case D3D10_SB_OPCODE_IMAX:
    case D3D10_SB_OPCODE_UMIN: case D3D10_SB_OPCODE_UMAX: case D3D10_SB_OPCODE_AND:
    case D3D10_SB_OPCODE_OR: case D3D10_SB_OPCODE_XOR: case D3D10_SB_OPCODE_ISHL:
    case D3D10_SB_OPCODE_ISHR: case D3D10_SB_OPCODE_USHR:
      destinations = 1; sources = 2; break;
    case D3D10_SB_OPCODE_MAD: case D3D10_SB_OPCODE_MOVC: case D3D10_SB_OPCODE_IMAD:
    case D3D10_SB_OPCODE_UMAD: case D3D11_SB_OPCODE_UBFE:
      destinations = 1; sources = 3; break;
    case D3D11_SB_OPCODE_BFI:
      destinations = 1; sources = 4; break;
    case D3D10_SB_OPCODE_SINCOS:
      destinations = 2; sources = 1; break;
    case D3D10_SB_OPCODE_IMUL: case D3D10_SB_OPCODE_UMUL: case D3D10_SB_OPCODE_UDIV:
      destinations = 2; sources = 2; break;
    case D3D10_SB_OPCODE_LD:
      destinations = 1; sources = 2; break;
    case D3D10_SB_OPCODE_SAMPLE_L:
      destinations = 1; sources = 4; break;
    case D3D11_SB_OPCODE_LD_STRUCTURED:
      destinations = 1; sources = 3; break;
    default: return false;
    }
    if (flow.size() > 64) return false;
    body |= !declaration;
    size_t cursor = operands_at;
    for (unsigned i = 0; i < sources + destinations; ++i) {
      const bool destination = i < destinations;
      Operand v;
      if (!operand(w, cursor, end, v, destination)) return false;
      const bool resource_position = resource_op && i == (op == D3D11_SB_OPCODE_LD_STRUCTURED ? 3u : 2u);
      const bool sampler_position = op == D3D10_SB_OPCODE_SAMPLE_L && i == 3;
      if (resource_position) {
        if (v.type != D3D10_SB_OPERAND_TYPE_RESOURCE || v.dimensions != 1 || v.relative[0] || !v.mask ||
            v.index[0] >= resources.size()) return false;
        const auto &r = resources[v.index[0]];
        if (!r.dimension || ((extension_mask & 4) && (resource_dimension != r.dimension || resource_stride != r.stride)) ||
            ((extension_mask & 8) && resource_returns != r.returns)) return false;
        if (op == D3D11_SB_OPCODE_LD_STRUCTURED) {
          if (r.dimension != D3D11_SB_RESOURCE_DIMENSION_STRUCTURED_BUFFER) return false;
        } else if (r.stride || (op == D3D10_SB_OPCODE_SAMPLE_L && r.dimension == D3D10_SB_RESOURCE_DIMENSION_BUFFER) ||
                   (op == D3D10_SB_OPCODE_LD && (r.dimension == D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE ||
                    r.dimension == D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY))) return false;
        continue;
      }
      if (sampler_position) {
        if (v.type != D3D10_SB_OPERAND_TYPE_SAMPLER || v.dimensions != 1 || v.relative[0] || v.mask ||
            v.index[0] >= samplers.size() || !samplers[v.index[0]]) return false;
        continue;
      }
      // Only multi-result instructions may discard a result using the null operand.
      if (v.type == D3D10_SB_OPERAND_TYPE_NULL) {
        if (!destination || destinations != 2 || v.dimensions || v.mask) return false;
        continue;
      }
      if (v.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) continue;
      if (!v.mask) return false;
      for (unsigned j=0;j<v.dimensions;++j) if (v.relative[j] && v.relative_register[j] >= temps) return false;
      if (v.type == D3D10_SB_OPERAND_TYPE_TEMP) {
        if (v.dimensions != 1 || v.relative[0] || v.index[0] >= temps) return false;
      } else if (v.type == D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP) {
        if (v.dimensions != 2 || v.relative[0] || v.index[0] >= indexable.size() || !indexable[v.index[0]][0] ||
            (!v.relative[1] && v.index[1] >= indexable[v.index[0]][0]) ||
            (v.mask & ~((1u << indexable[v.index[0]][1]) - 1))) return false;
      } else if (v.type == D3D10_SB_OPERAND_TYPE_INPUT) {
        if (destination || v.dimensions != 2 || v.relative[1] ||
            (!v.relative[0] && v.index[0] >= input_vertices(contract.primitive)) ||
            v.index[1] >= 32 || (v.mask & ~declared_in[v.index[1]])) return false;
      } else if (v.type == D3D10_SB_OPERAND_TYPE_OUTPUT) {
        if (!destination || v.dimensions != 1 || v.relative[0] || v.index[0] >= 32 || (v.mask & ~declared_out[v.index[0]])) return false;
      } else if (v.type == D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER) {
        if (destination || v.dimensions != 2 || v.relative[0] || v.index[0] >= contract.cbuffers.size() ||
            !contract.cbuffers[v.index[0]] || (!v.relative[1] && v.index[1] >= contract.cbuffers[v.index[0]])) return false;
      } else if (v.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE_CONSTANT_BUFFER) {
        if (destination || v.dimensions != 1 || !immediate_constants ||
            (!v.relative[0] && v.index[0] >= immediate_constants)) return false;
      } else return false;
    }
    if (sources + destinations && cursor != end) return false;
    at = end;
  }
  contract.stream_mask = 1;
  unsigned scalars = 0; for (auto mask : outputs) for (unsigned b = 0; b < 4; ++b) scalars += (mask >> b) & 1;
  return primitive && topology && max_vertices && returned && flow.empty() && declared_out == outputs &&
         (!declared_input_vertices || declared_input_vertices == input_vertices(contract.primitive)) &&
         capacity(1, scalars, contract.max_vertices, contract.primitive);
}
} // namespace gs_point
