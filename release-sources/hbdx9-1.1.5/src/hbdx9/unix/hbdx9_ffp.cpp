/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 native core — fixed function as generated D3D9 tokens (lane HBDX9, 07.11.2026).
 * The generated vs_3_0 / ps_3_0 streams go through the same dxso -> DXBC -> airconv path as game shaders, so there is
 * one translator and one cache.  Covered now: XYZRHW and XYZ (world*view*projection) positions, diffuse/specular with
 * defaults, texcoord pass-through, texture stages with the common D3DTOP_* ops (2D/cube/volume), lighting (point,
 * spot, directional; material sources; specular; local viewer) and the fog coordinate, both following wined3d's GLSL
 * fixed-function pipeline so the wined3d-GL control is the reference.  Not yet: texture transforms, TEXCOORDINDEX
 * generation modes, vertex blending, bump mapping stages, RESULTARG TEMP. */
#include "hbdx9_core.h"
#include <cstring>

namespace hb {

std::vector<hbdx9_vertex_element> fvf_to_elements(uint32_t fvf) {
  std::vector<hbdx9_vertex_element> e;
  uint16_t off = 0;
  auto add = [&](uint8_t type, uint8_t usage, uint8_t index) {
    e.push_back({0, off, type, 0, usage, index});
    off += (uint16_t)element_size(type);
  };
  uint32_t pos = fvf & FVF_POSITION_MASK;
  if (pos == FVF_XYZRHW) add(DT_FLOAT4, DU_POSITIONT, 0);
  else if (pos == FVF_XYZW) add(DT_FLOAT4, DU_POSITION, 0);
  else if (pos) {
    add(DT_FLOAT3, DU_POSITION, 0);
    uint32_t betas = pos == FVF_XYZB1 ? 1 : pos == FVF_XYZB2 ? 2 : pos == FVF_XYZB3 ? 3 : pos == FVF_XYZB4 ? 4 : pos == FVF_XYZB5 ? 5 : 0;
    bool last_index = fvf & (FVF_LASTBETA_UBYTE4 | FVF_LASTBETA_D3DCOLOR);
    uint32_t weights = last_index ? betas - 1 : betas;
    if (weights) add((uint8_t)(DT_FLOAT1 + weights - 1), DU_BLENDWEIGHT, 0);
    if (last_index) add(fvf & FVF_LASTBETA_UBYTE4 ? DT_UBYTE4 : DT_D3DCOLOR, DU_BLENDINDICES, 0);
  }
  if (fvf & FVF_NORMAL) add(DT_FLOAT3, DU_NORMAL, 0);
  if (fvf & FVF_PSIZE) add(DT_FLOAT1, DU_PSIZE, 0);
  if (fvf & FVF_DIFFUSE) add(DT_D3DCOLOR, DU_COLOR, 0);
  if (fvf & FVF_SPECULAR) add(DT_D3DCOLOR, DU_COLOR, 1);
  uint32_t tc = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
  for (uint32_t i = 0; i < tc && i < 8; i++) {
    uint32_t fmt = (fvf >> (16 + 2 * i)) & 3; /* 0: 2 floats, 1: 3, 2: 4, 3: 1 */
    uint8_t t = fmt == 0 ? DT_FLOAT2 : fmt == 1 ? DT_FLOAT3 : fmt == 2 ? DT_FLOAT4 : DT_FLOAT1;
    add(t, DU_TEXCOORD, (uint8_t)i);
  }
  return e;
}

uint32_t element_size(uint8_t type) {
  switch (type) {
  case DT_FLOAT1: return 4; case DT_FLOAT2: return 8; case DT_FLOAT3: return 12; case DT_FLOAT4: return 16;
  case DT_D3DCOLOR: case DT_UBYTE4: case DT_UBYTE4N: case DT_SHORT2: case DT_SHORT2N: case DT_USHORT2N:
  case DT_UDEC3: case DT_DEC3N: case DT_FLOAT16_2: return 4;
  case DT_SHORT4: case DT_SHORT4N: case DT_USHORT4N: case DT_FLOAT16_4: return 8;
  default: return 0;
  }
}

namespace {
/* D3D9 register files */
enum : uint32_t { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_OUTPUT = 6, R_COLOROUT = 8, R_SAMPLER = 10 };
enum : uint32_t {
  OP_MOV = 1, OP_ADD = 2, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7, OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11,
  OP_SLT = 12, OP_SGE = 13, OP_LRP = 18, OP_DCL = 31, OP_POW = 32, OP_NRM = 36, OP_TEX = 66, OP_DEF = 81, OP_CMP = 88,
};
constexpr uint32_t XYZW = 0xE4, XXXX = 0x00, YYYY = 0x55, ZZZZ = 0xAA, WWWW = 0xFF, ZWZW = 0xEE;
constexpr uint32_t MOD_NEG = 1, MOD_SIGN = 4, MOD_COMP = 6;

struct Asm {
  std::vector<uint32_t> t;
  static uint32_t rt(uint32_t type) { return ((type & 7) << 28) | ((type & 0x18) << 8); }
  static uint32_t dst(uint32_t type, uint32_t n, uint32_t mask = 0xF, bool sat = false) {
    return 0x80000000u | rt(type) | n | (mask << 16) | (sat ? (1u << 20) : 0);
  }
  static uint32_t src(uint32_t type, uint32_t n, uint32_t swz = XYZW, uint32_t mod = 0) {
    return 0x80000000u | rt(type) | n | (swz << 16) | (mod << 24);
  }
  void op(uint32_t code, std::initializer_list<uint32_t> params) {
    t.push_back(code | ((uint32_t)params.size() << 24));
    t.insert(t.end(), params.begin(), params.end());
  }
  void dcl(uint32_t usage, uint32_t index, uint32_t d) { op(OP_DCL, {0x80000000u | usage | (index << 16), d}); }
  void dcl_2d(uint32_t s, uint32_t type = 2) { op(OP_DCL, {0x80000000u | (type << 27), dst(R_SAMPLER, s)}); }
};
} // namespace

std::vector<uint32_t> ffp_vs_tokens(const FfpVsKey &k) {
  Asm a;
  a.t.push_back(0xFFFE0300);
  auto C = [](uint32_t n, uint32_t swz = XYZW, uint32_t mod = 0) { return Asm::src(R_CONST, n, swz, mod); };
  auto R = [](uint32_t n, uint32_t swz = XYZW, uint32_t mod = 0) { return Asm::src(R_TEMP, n, swz, mod); };
  auto D = [](uint32_t n, uint32_t mask = 0xF, bool sat = false) { return Asm::dst(R_TEMP, n, mask, sat); };
  const bool light = k.lighting && !k.pretransformed;
  const uint32_t V_NORMAL = 11;
  a.op(OP_DEF, {Asm::dst(R_CONST, 21), 0x00000000u, 0x3F800000u, 0x3F000000u, 0x00000000u});
  a.dcl(k.pretransformed ? DU_POSITIONT : DU_POSITION, 0, Asm::dst(R_INPUT, 0));
  if (k.has_color0) a.dcl(DU_COLOR, 0, Asm::dst(R_INPUT, 1));
  if (k.has_color1) a.dcl(DU_COLOR, 1, Asm::dst(R_INPUT, 2));
  for (uint32_t i = 0; i < k.texcoords; i++) a.dcl(DU_TEXCOORD, i, Asm::dst(R_INPUT, 3 + i));
  if (light && k.has_normal) a.dcl(DU_NORMAL, 0, Asm::dst(R_INPUT, V_NORMAL));
  a.dcl(DU_POSITION, 0, Asm::dst(R_OUTPUT, 0));
  a.dcl(DU_COLOR, 0, Asm::dst(R_OUTPUT, 1));
  a.dcl(DU_COLOR, 1, Asm::dst(R_OUTPUT, 2));
  for (uint32_t i = 0; i < 8; i++) if (k.tc_src[i] != 0xFF) a.dcl(DU_TEXCOORD, i, Asm::dst(R_OUTPUT, 3 + i));
  if (k.fog_coord) a.dcl(DU_FOG, 0, Asm::dst(R_OUTPUT, 11));
  if (k.point & 1) { a.dcl(DU_PSIZE, 0, Asm::dst(R_OUTPUT, 12)); if (k.point & 2) a.dcl(DU_PSIZE, 0, Asm::dst(R_INPUT, 12)); }
  const bool blend = k.blend >= 2 && !k.pretransformed;   /* (DX9FULL) */
  if (blend) a.dcl(DU_BLENDWEIGHT, 0, Asm::dst(R_INPUT, 13));
  if (blend) {
    /* (DX9FULL 13.11.2026) D3DRS_VERTEXBLEND (non-indexed): eye = sum w_i (pos x world_i x view), the last weight 1 - sum of the
       others (D3D9 / DXVK); clip = eye x projection; the normal blended the same way with each matrix's normal matrix */
    static const uint32_t kSel[4] = {XXXX, YYYY, ZZZZ, WWWW};
    a.op(OP_MOV, {D(10, 0x1), C(21, XXXX)});
    for (uint32_t i = 0; i + 1 < k.blend; i++) a.op(OP_ADD, {D(10, 0x1), R(10, XXXX), Asm::src(R_INPUT, 13, kSel[i])});
    a.op(OP_ADD, {D(10, 0x2), C(21, YYYY), R(10, XXXX, MOD_NEG)});              /* r10.y = last weight */
    for (uint32_t i = 0; i < k.blend; i++) {
      uint32_t w = i + 1 < k.blend ? Asm::src(R_INPUT, 13, kSel[i]) : R(10, YYYY);
      for (uint32_t c = 0; c < 4; c++) a.op(OP_DP4, {D(12, 1u << c), Asm::src(R_INPUT, 0), C(FFP_BLEND_WV + 4 * i + c)});
      if (i == 0) a.op(OP_MUL, {D(0), R(12), w}); else a.op(OP_MAD, {D(0), R(12), w, R(0)});
    }
    for (uint32_t c = 0; c < 4; c++) a.op(OP_DP4, {Asm::dst(R_OUTPUT, 0, 1u << c), R(0), C(FFP_BLEND_PROJ + c)});
  } else if (k.pretransformed) {
    a.op(OP_MAD, {D(0, 0x3), Asm::src(R_INPUT, 0), C(0), C(0, ZWZW)});
    a.op(OP_MAD, {D(0, 0x4), Asm::src(R_INPUT, 0, ZZZZ), C(1, XXXX), C(1, YYYY)});
    a.op(OP_RCP, {D(1, 0x1), Asm::src(R_INPUT, 0, WWWW)});
    a.op(OP_MUL, {Asm::dst(R_OUTPUT, 0, 0x7), R(0), R(1, XXXX)});
    a.op(OP_MOV, {Asm::dst(R_OUTPUT, 0, 0x8), R(1, XXXX)});
  } else {
    for (uint32_t i = 0; i < 4; i++) a.op(OP_DP4, {Asm::dst(R_OUTPUT, 0, 1u << i), Asm::src(R_INPUT, 0), C(4 + i)});
  }
  bool texgen = false;
  for (uint32_t i = 0; i < 8; i++) if (k.tc_src[i] != 0xFF && (k.tc_src[i] >> 4)) texgen = true;
  texgen = texgen && !k.pretransformed;
  bool need_eye = !k.pretransformed && (light || texgen || k.fog_coord == FFP_FOG_EYE_Z || k.fog_coord == FFP_FOG_RANGE ||
                                        (k.point & 4));
  if (need_eye && !blend) for (uint32_t i = 0; i < 4; i++) a.op(OP_DP4, {D(0, 1u << i), Asm::src(R_INPUT, 0), C(8 + i)}); /* r0 = eye position */
  uint32_t color0 = k.has_color0 ? Asm::src(R_INPUT, 1) : C(2), color1 = k.has_color1 ? Asm::src(R_INPUT, 2) : C(3);
  /* eye-space normal for lighting and texgen (wined3d: the normal matrix, normalised when NORMALIZENORMALS) */
  if ((light || texgen) && k.has_normal && blend) {
    static const uint32_t kSel[4] = {XXXX, YYYY, ZZZZ, WWWW};
    for (uint32_t i = 0; i < k.blend; i++) {
      uint32_t w = i + 1 < k.blend ? Asm::src(R_INPUT, 13, kSel[i]) : R(10, YYYY);
      for (uint32_t c = 0; c < 3; c++) a.op(OP_DP3, {D(12, 1u << c), Asm::src(R_INPUT, V_NORMAL), C(FFP_BLEND_N + 3 * i + c)});
      if (i == 0) a.op(OP_MUL, {D(1, 0x7), R(12), w}); else a.op(OP_MAD, {D(1, 0x7), R(12), w, R(1)});
    }
    if (k.normalize) a.op(OP_NRM, {D(1, 0x7), R(1)});
  } else if ((light || texgen) && k.has_normal) {
    for (uint32_t i = 0; i < 3; i++) a.op(OP_DP3, {D(1, 1u << i), Asm::src(R_INPUT, V_NORMAL), C(12 + i)});
    if (k.normalize) a.op(OP_NRM, {D(1, 0x7), R(1)});
  } else if (texgen) a.op(OP_MOV, {D(1, 0x7), C(21, XXXX)});
  if (!light) {
    a.op(OP_MOV, {Asm::dst(R_OUTPUT, 1, 0xF, true), color0});
    a.op(OP_MOV, {Asm::dst(R_OUTPUT, 2, 0xF, true), color1});
  } else {
    /* r1 normal, r2 ambient, r3 diffuse, r4 specular, r5 dir, r6 (1, d, d^2), r7 (1/d, att, in range), r8/r9 scratch,
       r11 view vector (towards the viewer) */
    a.op(OP_MOV, {D(2, 0x7), C(20)});
    a.op(OP_MOV, {D(3, 0x7), C(21, XXXX)});
    a.op(OP_MOV, {D(4), C(21, XXXX)});
    if (k.specular) {
      if (k.localviewer) { a.op(OP_NRM, {D(11, 0x7), R(0)}); a.op(OP_MOV, {D(11, 0x7), R(11, XYZW, MOD_NEG)}); }
      else a.op(OP_MOV, {D(11, 0x7), C(21, 0x80) /* (0,0,0) */}), a.op(OP_ADD, {D(11, 0x4), R(11, ZZZZ), C(21, YYYY, MOD_NEG)});
    }
    auto footer = [&](uint32_t L) {
      if (!k.has_normal) return;
      a.op(OP_DP3, {D(8, 0x1), R(5), R(1)});
      a.op(OP_MAX, {D(8, 0x2), R(8, XXXX), C(21, XXXX)});
      a.op(OP_MIN, {D(8, 0x2), R(8, YYYY), C(21, YYYY)});
      a.op(OP_MUL, {D(8, 0x2), R(8, YYYY), R(7, YYYY)});
      a.op(OP_MAD, {D(3, 0x7), C(L + 0), R(8, YYYY), R(3)});
      if (!k.specular) return;
      a.op(OP_ADD, {D(9, 0x7), R(5), R(11)});
      a.op(OP_NRM, {D(9, 0x7), R(9)});
      a.op(OP_DP3, {D(8, 0x4), R(1), R(9)});                       /* t */
      a.op(OP_POW, {D(8, 0x8), R(8, ZZZZ), C(19, XXXX)});           /* t^power */
      a.op(OP_SLT, {D(8, 0x1), C(21, XXXX), R(8, XXXX)});           /* n.l > 0 */
      a.op(OP_SLT, {D(8, 0x4), C(21, XXXX), R(8, ZZZZ)});           /* t > 0 */
      a.op(OP_MUL, {D(8, 0x1), R(8, XXXX), R(8, ZZZZ)});
      a.op(OP_ADD, {D(8, 0x1), R(8, XXXX), C(21, ZZZZ, MOD_NEG)});
      a.op(OP_CMP, {D(8, 0x8), R(8, XXXX), R(8, WWWW), C(21, XXXX)}); /* select, NaN-safe */
      a.op(OP_MUL, {D(8, 0x8), R(8, WWWW), R(7, YYYY)});
      a.op(OP_MAD, {D(4), C(L + 1), R(8, WWWW), R(4)});
    };
    for (uint32_t i = 0; i < k.light_count && i < 8; i++) {
      uint32_t L = FFP_LIGHT_BASE + 7 * i;
      uint32_t type = k.light_type[i];
      if (type == 3) { /* directional */
        a.op(OP_ADD, {D(2, 0x7), R(2), C(L + 2)});
        a.op(OP_MOV, {D(7, 0x2), C(21, YYYY)});
        if (k.has_normal) a.op(OP_NRM, {D(5, 0x7), C(L + 4)});
        footer(L);
        continue;
      }
      a.op(OP_ADD, {D(5, 0x7), C(L + 3), R(0, XYZW, MOD_NEG)});
      a.op(OP_DP3, {D(6, 0x4), R(5), R(5)});
      a.op(OP_RSQ, {D(7, 0x1), R(6, ZZZZ)});
      a.op(OP_MUL, {D(6, 0x2), R(6, ZZZZ), R(7, XXXX)});
      a.op(OP_MOV, {D(6, 0x1), C(21, YYYY)});
      a.op(OP_SGE, {D(7, 0x4), C(L + 5, XXXX), R(6, YYYY)});        /* d <= range */
      a.op(OP_MUL, {D(5, 0x7), R(5), R(7, XXXX)});                   /* normalised */
      a.op(OP_DP3, {D(7, 0x2), R(6), C(L + 5, 0xF9) /* yzw */});
      a.op(OP_RCP, {D(7, 0x2), R(7, YYYY)});
      if (type == 2) { /* spot */
        a.op(OP_NRM, {D(9, 0x7), C(L + 4)});
        a.op(OP_DP3, {D(8, 0x1), R(5, XYZW, MOD_NEG), R(9)});        /* t */
        a.op(OP_ADD, {D(8, 0x2), R(8, XXXX), C(L + 6, YYYY, MOD_NEG)});
        a.op(OP_ADD, {D(8, 0x4), C(L + 6, XXXX), C(L + 6, YYYY, MOD_NEG)});
        a.op(OP_RCP, {D(8, 0x4), R(8, ZZZZ)});
        a.op(OP_MUL, {D(8, 0x2), R(8, YYYY), R(8, ZZZZ)});
        a.op(OP_MAX, {D(8, 0x2), R(8, YYYY), C(21, XXXX)});
        a.op(OP_POW, {D(8, 0x4), R(8, YYYY), C(L + 6, ZZZZ)});
        a.op(OP_ADD, {D(8, 0x8), R(8, XXXX), C(L + 6, XXXX, MOD_NEG)});
        a.op(OP_CMP, {D(8, 0x4), R(8, WWWW, MOD_NEG), R(8, ZZZZ), C(21, YYYY)});  /* t > cos(theta/2): 1 */
        a.op(OP_ADD, {D(8, 0x8), R(8, XXXX), C(L + 6, YYYY, MOD_NEG)});
        a.op(OP_CMP, {D(8, 0x4), R(8, WWWW, MOD_NEG), C(21, XXXX), R(8, ZZZZ)});  /* t <= cos(phi/2): 0 */
        a.op(OP_MUL, {D(7, 0x2), R(7, YYYY), R(8, ZZZZ)});
      }
      a.op(OP_MUL, {D(7, 0x2), R(7, YYYY), R(7, ZZZZ)});
      a.op(OP_MAD, {D(2, 0x7), C(L + 2), R(7, YYYY), R(2)});
      footer(L);
    }
    auto msrc = [&](uint8_t src, uint32_t material) {
      return src == 1 && k.has_color0 ? Asm::src(R_INPUT, 1) : src == 2 && k.has_color1 ? Asm::src(R_INPUT, 2) : C(material);
    };
    uint32_t diff = msrc(k.diffuse_src, 15), amb = msrc(k.ambient_src, 16), spec = msrc(k.specular_src, 17), emis = msrc(k.emissive_src, 18);
    a.op(OP_MUL, {D(10, 0x7), amb, R(2)});
    a.op(OP_MAD, {D(10, 0x7), diff, R(3), R(10)});
    a.op(OP_ADD, {Asm::dst(R_OUTPUT, 1, 0x7, true), R(10), emis});
    a.op(OP_MOV, {Asm::dst(R_OUTPUT, 1, 0x8, true), diff});
    if (k.specular) a.op(OP_MUL, {Asm::dst(R_OUTPUT, 2, 0xF, true), spec, R(4)});
    else a.op(OP_MOV, {Asm::dst(R_OUTPUT, 2, 0xF, true), color1});
  }
  /* texture coordinates: stage i -> output slot i (D3D9 FFP), source by TEXCOORDINDEX (input or texgen), then the
     stage's texture matrix (prepared on the CPU as wined3d compute_texture_matrix) */
  for (uint32_t i = 0; i < 8; i++) {
    uint8_t src = k.tc_src[i];
    if (src == 0xFF) continue;
    uint32_t gen = k.pretransformed ? 0 : src >> 4, idx = src & 7;
    uint32_t v;
    if (gen == 0) v = idx < k.texcoords ? Asm::src(R_INPUT, 3 + idx) : C(21, XXXX);
    else {
      /* r12 = generated coordinate */
      if (gen == 1) { a.op(OP_MOV, {D(12, 0x7), R(1)}); a.op(OP_MOV, {D(12, 0x8), C(21, YYYY)}); }
      else if (gen == 2) { a.op(OP_MOV, {D(12, 0x7), R(0)}); a.op(OP_MOV, {D(12, 0x8), C(21, YYYY)}); }
      else {
        /* reflect(normalize(eye), n) = I - 2 (n.I) n */
        a.op(OP_NRM, {D(13, 0x7), R(0)});
        a.op(OP_DP3, {D(14, 0x1), R(1), R(13)});
        a.op(OP_ADD, {D(14, 0x1), R(14, XXXX), R(14, XXXX)});
        a.op(OP_MAD, {D(12, 0x7), R(1), R(14, XXXX, MOD_NEG), R(13)});
        if (gen == 3) a.op(OP_MOV, {D(12, 0x8), C(21, YYYY)});
        else {
          /* sphere map: m = 2 |(r.x, r.y, r.z + 1)|, (r.x / m + 0.5, r.y / m + 0.5, 0, 1) */
          a.op(OP_MOV, {D(13, 0x3), R(12)});
          a.op(OP_ADD, {D(13, 0x4), R(12, ZZZZ), C(21, YYYY)});
          a.op(OP_DP3, {D(14, 0x1), R(13), R(13)});
          a.op(OP_RSQ, {D(14, 0x1), R(14, XXXX)});
          a.op(OP_MUL, {D(14, 0x1), R(14, XXXX), C(21, ZZZZ)});            /* 1/m = rsq / 2 */
          a.op(OP_MAD, {D(12, 0x3), R(12), R(14, XXXX), C(21, ZZZZ)});
          a.op(OP_MOV, {D(12, 0x4), C(21, XXXX)});
          a.op(OP_MOV, {D(12, 0x8), C(21, YYYY)});
        }
      }
      v = R(12);
    }
    if (k.tc_xform[i] && !k.pretransformed)
      for (uint32_t c = 0; c < 4; c++) a.op(OP_DP4, {Asm::dst(R_OUTPUT, 3 + i, 1u << c), v, C(FFP_TEXMAT_BASE + 4 * i + c)});
    else a.op(OP_MOV, {Asm::dst(R_OUTPUT, 3 + i), v});
  }
  /* (DX9FULL 13.11.2026) point size (D3D9 FFP): Si = vertex PSIZE or D3DRS_POINTSIZE; with POINTSCALEENABLE (not for XYZRHW)
     Ss = Vh * Si * sqrt(1 / (A + B De + C De^2)), De = |eye position|; the clamp to [MIN, MAX] is done by dxso's point epilogue */
  if (k.point & 1) {
    uint32_t si = (k.point & 2) ? Asm::src(R_INPUT, 12, XXXX) : C(FFP_VS_POINT, XXXX);
    if ((k.point & 4) && !k.pretransformed) {
      a.op(OP_DP3, {D(13, 0x1), R(0), R(0)});                                   /* De^2 */
      a.op(OP_RSQ, {D(13, 0x2), R(13, XXXX)});
      a.op(OP_RCP, {D(13, 0x2), R(13, YYYY)});                                  /* De (0 for De^2 = 0) */
      a.op(OP_MUL, {D(13, 0x4), R(13, XXXX), C(FFP_VS_POINT, WWWW)});           /* C De^2 */
      a.op(OP_MAD, {D(13, 0x4), R(13, YYYY), C(FFP_VS_POINT, ZZZZ), R(13, ZZZZ)}); /* + B De */
      a.op(OP_ADD, {D(13, 0x4), R(13, ZZZZ), C(FFP_VS_POINT, YYYY)});           /* + A */
      a.op(OP_RSQ, {D(13, 0x8), R(13, ZZZZ)});
      a.op(OP_MUL, {D(13, 0x8), R(13, WWWW), C(FFP_VS_POINT + 1, XXXX)});
      a.op(OP_MUL, {Asm::dst(R_OUTPUT, 12, 0x1), R(13, WWWW), si});
    } else a.op(OP_MOV, {Asm::dst(R_OUTPUT, 12, 0x1), si});
  }
  switch (k.fog_coord) {
  case FFP_FOG_EYE_Z: a.op(OP_MAX, {Asm::dst(R_OUTPUT, 11, 0x1), R(0, ZZZZ), R(0, ZZZZ, MOD_NEG)}); break;
  case FFP_FOG_RAW_Z: a.op(OP_MOV, {Asm::dst(R_OUTPUT, 11, 0x1), Asm::src(R_INPUT, 0, ZZZZ)}); break;
  case FFP_FOG_RANGE:
    a.op(OP_DP3, {D(9, 0x1), R(0), R(0)}); a.op(OP_RSQ, {D(9, 0x2), R(9, XXXX)});
    a.op(OP_MUL, {Asm::dst(R_OUTPUT, 11, 0x1), R(9, XXXX), R(9, YYYY)}); break;
  case FFP_FOG_SPECULAR_W: a.op(OP_MOV, {Asm::dst(R_OUTPUT, 11, 0x1), k.has_color1 ? Asm::src(R_INPUT, 2, WWWW) : C(21, YYYY)}); break;
  default: break;
  }
  a.t.push_back(0x0000FFFF);
  return a.t;
}

/* FFP PS constants: c0 texture factor, c1 (1,1,1,1), c2 (0.5,0.5,0.5,0.5). Temps: r0 current, r1 scratch, r2 result,
 * r3 TEMP, r4+i texture of stage i.  Inputs: v0 diffuse, v1 specular, v2+n texcoord n. */
std::vector<uint32_t> ffp_ps_tokens(const FfpPsKey &k) {
  enum : uint32_t { TA_DIFFUSE = 0, TA_CURRENT = 1, TA_TEXTURE = 2, TA_TFACTOR = 3, TA_SPECULAR = 4, TA_TEMP = 5, TA_CONSTANT = 6,
                    TA_COMPLEMENT = 0x10, TA_ALPHAREPLICATE = 0x20 };
  Asm a;
  a.t.push_back(0xFFFF0300);
  a.dcl(DU_COLOR, 0, Asm::dst(R_INPUT, 0));
  a.dcl(DU_COLOR, 1, Asm::dst(R_INPUT, 1));
  uint32_t tc_done = 0;
  auto uses_tex = [&k](const FfpStage &s) {
    return (s.colorarg1 & 0xF) == TA_TEXTURE || (s.colorarg2 & 0xF) == TA_TEXTURE || (s.alphaarg1 & 0xF) == TA_TEXTURE ||
           (s.alphaarg2 & 0xF) == TA_TEXTURE || s.colorop == 13 /* BLENDTEXTUREALPHA */ || s.alphaop == 13 || s.colorop == 15 || s.alphaop == 15 ||
           (k.ext && ((s.colorarg0 & 0xF) == TA_TEXTURE || (s.alphaarg0 & 0xF) == TA_TEXTURE ||
                      s.colorop == 22 || s.colorop == 23 /* BUMPENVMAP(LUMINANCE): the bump map */));
  };
  for (uint32_t i = 0; i < k.stages; i++) {
    const auto &s = k.stage[i];
    if (!uses_tex(s) || !s.has_texture) continue;
    if (!(tc_done >> s.texcoord & 1) && s.texcoord < 8) { a.dcl(DU_TEXCOORD, s.texcoord, Asm::dst(R_INPUT, 2 + s.texcoord)); tc_done |= 1u << s.texcoord; }
    (void)0;
    a.dcl_2d(i, s.has_texture == 2 ? 3 : s.has_texture == 3 ? 4 : 2);
  }
  a.op(OP_MOV, {Asm::dst(R_TEMP, 0), Asm::src(R_INPUT, 0)});
  a.op(OP_MOV, {Asm::dst(R_TEMP, 3), Asm::src(R_CONST, 1, XYZW)});
  for (uint32_t i = 0; i < k.stages; i++) {
    const auto &s = k.stage[i];
    if (s.colorop == 1 /* DISABLE */) break;
    uint32_t tex = 4 + i;
    /* (DX9FULL) the stage after a BUMPENVMAP(LUMINANCE) stage samples at its coordinate + bump matrix x (du, dv) (D3D9 /
       DXVK DoBumpmapCoords: u += M00 du + M10 dv, v += M01 du + M11 dv) */
    bool bumped = k.ext && i > 0 && (k.stage[i - 1].colorop == 22 || k.stage[i - 1].colorop == 23) && k.stage[i - 1].has_texture;
    if (uses_tex(s)) {
      if (s.has_texture && s.texcoord < 8 && bumped) {
        uint32_t bt = 4 + (i - 1), bm = FFP_PS_BUMPMAT_BASE + (i - 1);
        a.op(OP_MOV, {Asm::dst(R_TEMP, 12), Asm::src(R_INPUT, 2 + s.texcoord)});
        a.op(OP_MAD, {Asm::dst(R_TEMP, 12, 0x1), Asm::src(R_TEMP, bt, XXXX), Asm::src(R_CONST, bm, XXXX), Asm::src(R_TEMP, 12, XXXX)});
        a.op(OP_MAD, {Asm::dst(R_TEMP, 12, 0x1), Asm::src(R_TEMP, bt, YYYY), Asm::src(R_CONST, bm, YYYY), Asm::src(R_TEMP, 12, XXXX)});
        a.op(OP_MAD, {Asm::dst(R_TEMP, 12, 0x2), Asm::src(R_TEMP, bt, XXXX), Asm::src(R_CONST, bm, ZZZZ), Asm::src(R_TEMP, 12, YYYY)});
        a.op(OP_MAD, {Asm::dst(R_TEMP, 12, 0x2), Asm::src(R_TEMP, bt, YYYY), Asm::src(R_CONST, bm, WWWW), Asm::src(R_TEMP, 12, YYYY)});
        a.op(OP_TEX | (s.projected ? (1u << 16) : 0), {Asm::dst(R_TEMP, tex), Asm::src(R_TEMP, 12), Asm::src(R_SAMPLER, i)});
        if (k.stage[i - 1].colorop == 23) {   /* luminance: the bump map's third channel x scale + offset, saturated */
          uint32_t bl = FFP_PS_BUMPL_BASE + (i - 1);
          a.op(OP_MAD, {Asm::dst(R_TEMP, 1, 0x1, true), Asm::src(R_TEMP, bt, ZZZZ), Asm::src(R_CONST, bl, XXXX), Asm::src(R_CONST, bl, YYYY)});
          a.op(OP_MUL, {Asm::dst(R_TEMP, tex), Asm::src(R_TEMP, tex), Asm::src(R_TEMP, 1, XXXX)});
        }
      } else if (s.has_texture && s.texcoord < 8)
        a.op(OP_TEX | (s.projected ? (1u << 16) /* texldp */ : 0), {Asm::dst(R_TEMP, tex), Asm::src(R_INPUT, 2 + s.texcoord), Asm::src(R_SAMPLER, i)});
      else a.op(OP_MOV, {Asm::dst(R_TEMP, tex), Asm::src(R_CONST, 1)});
    }
    auto arg = [&](uint8_t v, uint32_t swz_alpha) {
      uint32_t type = R_TEMP, n = 0;
      switch (v & 0xF) {
      case TA_DIFFUSE: type = R_INPUT; n = 0; break;
      case TA_CURRENT: type = R_TEMP; n = 0; break;
      case TA_TEXTURE: type = R_TEMP; n = tex; break;
      case TA_TFACTOR: type = R_CONST; n = 0; break;
      case TA_SPECULAR: type = R_INPUT; n = 1; break;
      case TA_TEMP: type = R_TEMP; n = 3; break;
      case TA_CONSTANT: type = R_CONST; n = k.ext ? FFP_PS_CONST_BASE + i : 0; break;   /* (DX9FULL) D3DTSS_CONSTANT of the stage */
      default: type = R_CONST; n = 0; break;
      }
      uint32_t swz = (v & TA_ALPHAREPLICATE) ? WWWW : swz_alpha;
      return Asm::src(type, n, swz, (v & TA_COMPLEMENT) ? MOD_COMP : 0);
    };
    auto emit = [&](uint8_t op, uint8_t a1v, uint8_t a2v, uint8_t a0v, uint32_t mask, uint32_t swz) {
      uint32_t d = Asm::dst(R_TEMP, 2, mask, true), dn = Asm::dst(R_TEMP, 2, mask, false);
      uint32_t a1 = arg(a1v, swz), a2 = arg(a2v, swz), r2 = Asm::src(R_TEMP, 2, swz);
      if (k.ext) switch (op) {   /* (DX9FULL) the ops DXVK implements that s8j fell back on SELECTARG1 for */
      case 20:   /* MODULATEINVALPHA_ADDCOLOR: (1 - a1.a) a2 + a1 */
        a.op(OP_MOV, {Asm::dst(R_TEMP, 1, mask), (a1 & ~(0xFFu << 16)) | (WWWW << 16)});
        a.op(OP_ADD, {Asm::dst(R_TEMP, 1, mask), Asm::src(R_CONST, 1), Asm::src(R_TEMP, 1, XYZW, MOD_NEG)});
        a.op(OP_MAD, {d, Asm::src(R_TEMP, 1, swz), a2, a1}); return;
      case 21:   /* MODULATEINVCOLOR_ADDALPHA: (1 - a1) a2 + a1.a */
        a.op(OP_MOV, {Asm::dst(R_TEMP, 1, mask), a1});
        a.op(OP_ADD, {Asm::dst(R_TEMP, 1, mask), Asm::src(R_CONST, 1), Asm::src(R_TEMP, 1, XYZW, MOD_NEG)});
        a.op(OP_MAD, {d, Asm::src(R_TEMP, 1, swz), a2, (a1 & ~(0xFFu << 16)) | (WWWW << 16)}); return;
      case 22: case 23:   /* BUMPENVMAP(LUMINANCE): the stage perturbs the next one; current passes through */
        a.op(OP_MOV, {d, Asm::src(R_TEMP, 0, swz)}); return;
      case 25: a.op(OP_MAD, {d, a1, a2, arg(a0v, swz)}); return;       /* MULTIPLYADD: a0 + a1 a2 */
      case 26: a.op(OP_LRP, {d, arg(a0v, swz), a1, a2}); return;       /* LERP: a0 a1 + (1 - a0) a2 */
      default: break;
      }
      switch (op) {
      case 2: a.op(OP_MOV, {d, a1}); break;
      case 3: a.op(OP_MOV, {d, a2}); break;
      case 4: a.op(OP_MUL, {d, a1, a2}); break;
      case 5: a.op(OP_MUL, {dn, a1, a2}); a.op(OP_ADD, {d, r2, r2}); break;
      case 6: a.op(OP_MUL, {dn, a1, a2}); a.op(OP_ADD, {dn, r2, r2}); a.op(OP_ADD, {d, r2, r2}); break;
      case 7: a.op(OP_ADD, {d, a1, a2}); break;
      case 8: a.op(OP_ADD, {dn, a1, a2}); a.op(OP_ADD, {d, r2, Asm::src(R_CONST, 2, XYZW, MOD_NEG)}); break;
      case 9: a.op(OP_ADD, {dn, a1, a2}); a.op(OP_ADD, {dn, r2, Asm::src(R_CONST, 2, XYZW, MOD_NEG)}); a.op(OP_ADD, {d, r2, r2}); break;
      case 10: a.op(OP_ADD, {d, a1, a2 ^ (MOD_NEG << 24)}); break;
      case 11: a.op(OP_ADD, {Asm::dst(R_TEMP, 1, mask), Asm::src(R_CONST, 1), a1 ^ (MOD_NEG << 24)});
               a.op(OP_MAD, {d, Asm::src(R_TEMP, 1, swz), a2, a1}); break;
      case 12: a.op(OP_LRP, {d, Asm::src(R_INPUT, 0, WWWW), a1, a2}); break;
      case 13: a.op(OP_LRP, {d, Asm::src(R_TEMP, tex, WWWW), a1, a2}); break;
      case 14: a.op(OP_LRP, {d, Asm::src(R_CONST, 0, WWWW), a1, a2}); break;
      case 15: a.op(OP_ADD, {Asm::dst(R_TEMP, 1, mask), Asm::src(R_CONST, 1), Asm::src(R_TEMP, tex, WWWW, MOD_NEG)});
               a.op(OP_MAD, {d, a2, Asm::src(R_TEMP, 1, swz), a1}); break;
      case 16: a.op(OP_LRP, {d, Asm::src(R_TEMP, 0, WWWW), a1, a2}); break;
      case 18: a.op(OP_MAD, {d, (a1 & ~(0xFFu << 16)) | (WWWW << 16), a2, a1}); break;
      case 19: a.op(OP_MAD, {d, a1, a2, (a1 & ~(0xFFu << 16)) | (WWWW << 16)}); break;
      case 24: a.op(OP_DP3, {Asm::dst(R_TEMP, 2, 0xF, true), (a1 & ~(0xFu << 24)) | (MOD_SIGN << 24), (a2 & ~(0xFu << 24)) | (MOD_SIGN << 24)}); break;
      default: a.op(OP_MOV, {d, a1}); break;
      }
    };
    a.op(OP_MOV, {Asm::dst(R_TEMP, 2), Asm::src(R_TEMP, 0)});
    emit(s.colorop, s.colorarg1, s.colorarg2, s.colorarg0, 0x7, XYZW);
    if (s.alphaop != 1 && s.colorop != 24) emit(s.alphaop, s.alphaarg1, s.alphaarg2, s.alphaarg0, 0x8, XYZW);
    /* (DX9FULL) D3DTSS_RESULTARG = D3DTA_TEMP: the stage writes the temp register and leaves current */
    a.op(OP_MOV, {Asm::dst(R_TEMP, k.ext && s.resultarg == TA_TEMP ? 3 : 0), Asm::src(R_TEMP, 2)});
  }
  if (k.specular_add) a.op(OP_ADD, {Asm::dst(R_TEMP, 0, 0x7, true), Asm::src(R_TEMP, 0), Asm::src(R_INPUT, 1)});
  a.op(OP_MOV, {Asm::dst(R_COLOROUT, 0), Asm::src(R_TEMP, 0)});
  a.t.push_back(0x0000FFFF);
  return a.t;
}

} // namespace hb
