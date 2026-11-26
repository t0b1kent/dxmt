/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 (lane HBDX9, 07.11.2026): D3D9 shader tokens -> DXBC SM 5.0 container for airconv.
 * Decoding: DXVK dxso decoder (this directory).  Binding model and linkage: ../airconv_dxso.h.
 *
 * Covered: vs/ps 2.0-3.0 arithmetic, texld/texldp/texldb/texldl/texldd, texkill, cmp/cnd, m4x4..m3x2, sincos, lrp,
 * nrm, crs, pow, dp2add, sgn, dst, lit, mova/a0 relative constants, loop/rep (aL), if/ifc/else/break/breakc, call/callnz
 * (inlined), setp + predicated instructions, vPos/vFace, oDepth, vs 1.1 (same register model).
 * Not yet: ps 1.x (texture-stage era instructions), relative input/output addressing in SM3, int vertex formats
 * other than via dxso_variant.int_inputs. */
#include "../airconv_dxso.h"
#include "dxso_decoder.h"
#include "../../../libs/DXBCParser/d3d12tokenizedprogramformat.hpp"
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace dxvk;
using namespace microsoft;

#define DXSO_VS_POINT_SHIFT 16   /* (DX9FULL) VS variant vs_flags bit 16: point expansion, bit 17: point sprite texcoords */
namespace {

/* ------------------------------------------------------------------ DXBC operands ---------------------------------- */
struct Opnd {
  uint32_t type = D3D10_SB_OPERAND_TYPE_TEMP;
  uint32_t ncomp = 2;            /* 0: 0-comp, 1: 1-comp, 2: 4-comp */
  uint32_t selmode = 1;          /* 0 mask, 1 swizzle, 2 select1 */
  uint32_t sel = 0xE4;           /* mask (4 bits) / swizzle (8 bits) / component */
  uint32_t dims = 1;
  uint32_t idx[2] = {0, 0};
  bool rel = false;              /* relative addressing on the last index */
  uint32_t rel_temp = 0, rel_comp = 0;
  uint32_t mod = 0;              /* 1 neg, 2 abs, 3 abs+neg */
  uint32_t immn = 0;             /* immediate: 1 or 4 values */
  uint32_t imm[4] = {0, 0, 0, 0};

  Opnd &mask(uint32_t m) { selmode = 0; sel = m & 0xF; return *this; }
  Opnd &swz(uint32_t s) { selmode = 1; sel = s & 0xFF; return *this; }
  Opnd &sel1(uint32_t c) { selmode = 2; sel = c & 3; return *this; }
  Opnd &neg() { mod ^= 1; return *this; }
  Opnd &abs() { mod = (mod | 2) & ~1u; return *this; }
};

constexpr uint32_t SWZ(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | (y << 2) | (z << 4) | (w << 6); }
constexpr uint32_t XYZW = SWZ(0, 1, 2, 3);
constexpr uint32_t XXXX = SWZ(0, 0, 0, 0);
static uint32_t swz_rep(uint32_t c) { return SWZ(c, c, c, c); }
static uint32_t swz_comp(uint32_t s, uint32_t i) { return (s >> (2 * i)) & 3; }
/* result[i] = s[inner[i]] */
static uint32_t swz_compose(uint32_t s, uint32_t inner) {
  return SWZ(swz_comp(s, swz_comp(inner, 0)), swz_comp(s, swz_comp(inner, 1)), swz_comp(s, swz_comp(inner, 2)),
             swz_comp(s, swz_comp(inner, 3)));
}

static Opnd T(uint32_t n) { Opnd o; o.type = D3D10_SB_OPERAND_TYPE_TEMP; o.idx[0] = n; return o; }
static Opnd V(uint32_t n) { Opnd o; o.type = D3D10_SB_OPERAND_TYPE_INPUT; o.idx[0] = n; return o; }
static Opnd O(uint32_t n) { Opnd o; o.type = D3D10_SB_OPERAND_TYPE_OUTPUT; o.idx[0] = n; return o; }
static Opnd CB(uint32_t slot, uint32_t el) {
  Opnd o; o.type = D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER; o.dims = 2; o.idx[0] = slot; o.idx[1] = el; return o;
}
static Opnd RES(uint32_t n) { Opnd o; o.type = D3D10_SB_OPERAND_TYPE_RESOURCE; o.idx[0] = n; return o; }
static Opnd SMP(uint32_t n) { Opnd o; o.type = D3D10_SB_OPERAND_TYPE_SAMPLER; o.ncomp = 0; o.idx[0] = n; return o; }
static Opnd NUL() { Opnd o; o.type = D3D10_SB_OPERAND_TYPE_NULL; o.ncomp = 0; o.dims = 0; return o; }
static Opnd ODEPTH() { Opnd o; o.type = D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH; o.ncomp = 1; o.dims = 0; return o; }
static Opnd IMMU(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  Opnd o; o.type = D3D10_SB_OPERAND_TYPE_IMMEDIATE32; o.dims = 0; o.immn = 4;
  o.imm[0] = a; o.imm[1] = b; o.imm[2] = c; o.imm[3] = d; return o;
}
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static Opnd IMMF(float a, float b, float c, float d) { return IMMU(fbits(a), fbits(b), fbits(c), fbits(d)); }
static Opnd IMMF1(float a) { return IMMF(a, a, a, a); }
static Opnd IMMU1(uint32_t a) { return IMMU(a, a, a, a); }

static void encode(std::vector<uint32_t> &out, const Opnd &o) {
  uint32_t t = 0;
  if (o.immn) { /* immediates: component count only, no selection bits (as fxc encodes them) */
    out.push_back((o.immn == 1 ? 1u : 2u) | ((o.type & 0xFF) << 12));
    for (uint32_t i = 0; i < o.immn; i++) out.push_back(o.imm[i]);
    return;
  }
  t |= o.ncomp & 3;
  if (o.ncomp == 2) {
    t |= (o.selmode & 3) << 2;
    if (o.selmode == 0) t |= (o.sel & 0xF) << 4;
    else if (o.selmode == 1) t |= (o.sel & 0xFF) << 4;
    else t |= (o.sel & 3) << 4;
  }
  t |= (o.type & 0xFF) << 12;
  t |= (o.dims & 3) << 20;
  for (uint32_t d = 0; d < o.dims; d++) {
    uint32_t rep = (o.rel && d == o.dims - 1) ? D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE : D3D10_SB_OPERAND_INDEX_IMMEDIATE32;
    t |= rep << (22 + 3 * d);
  }
  if (o.mod) t |= 0x80000000u;
  out.push_back(t);
  if (o.mod) out.push_back(1u /* modifier */ | (o.mod << 6));
  if (o.immn) {
    for (uint32_t i = 0; i < o.immn; i++) out.push_back(o.imm[i]);
    return;
  }
  for (uint32_t d = 0; d < o.dims; d++) {
    out.push_back(o.idx[d]);
    if (o.rel && d == o.dims - 1) {
      Opnd r = T(o.rel_temp); r.sel1(o.rel_comp);
      encode(out, r);
    }
  }
}

/* ------------------------------------------------------------------ signatures and container ---------------------- */
struct SigElem {
  std::string name; uint32_t index, sv, reg; uint8_t mask, rw; uint32_t ctype = 3;   /* (DX9FULL) 1 UINT32 (SV_VertexID), 3 FLOAT32 */
};
static std::vector<uint8_t> build_signature(std::vector<SigElem> e) {
  std::stable_sort(e.begin(), e.end(), [](const SigElem &a, const SigElem &b) { return a.reg < b.reg; });
  std::vector<uint8_t> out(8 + 24 * e.size());
  auto put = [&](size_t at, uint32_t v) { memcpy(out.data() + at, &v, 4); };
  put(0, (uint32_t)e.size()); put(4, 8);
  for (size_t i = 0; i < e.size(); i++) {
    size_t at = 8 + 24 * i;
    put(at + 0, (uint32_t)out.size());
    out.insert(out.end(), e[i].name.begin(), e[i].name.end()); out.push_back(0);
    put(at + 4, e[i].index); put(at + 8, e[i].sv); put(at + 12, e[i].ctype); put(at + 16, e[i].reg);
    out[at + 20] = e[i].mask; out[at + 21] = e[i].rw; out[at + 22] = 0; out[at + 23] = 0;
  }
  while (out.size() & 3) out.push_back(0);
  return out;
}
static std::vector<uint8_t> build_container(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>> &chunks) {
  std::vector<uint8_t> out(32 + 4 * chunks.size());
  auto put = [&](size_t at, uint32_t v) { memcpy(out.data() + at, &v, 4); };
  memcpy(out.data(), "DXBC", 4);
  put(20, 1);
  put(28, (uint32_t)chunks.size());
  for (size_t i = 0; i < chunks.size(); i++) {
    put(32 + 4 * i, (uint32_t)out.size());
    uint32_t hdr[2] = {chunks[i].first, (uint32_t)chunks[i].second.size()};
    out.insert(out.end(), (uint8_t *)hdr, (uint8_t *)hdr + 8);
    out.insert(out.end(), chunks[i].second.begin(), chunks[i].second.end());
  }
  put(24, (uint32_t)out.size());
  return out;
}
constexpr uint32_t FOURCC(char a, char b, char c, char d) {
  return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

/* ------------------------------------------------------------------ interpolant slots --------------------------- */
std::mutex g_slot_mutex;
uint32_t g_slot_count = 11;
DxsoSemantic g_slots[DXSO_MAX_SLOTS];

uint32_t slot_of(DxsoUsage usage, uint32_t index) {
  /* fixed: texcoord 0-7 -> 0-7, color 0-1 -> 8-9, fog -> 10; anything else first come first served */
  if (usage == DxsoUsage::Texcoord && index < 8) return index;
  if (usage == DxsoUsage::Color && index < 2) return 8 + index;
  if (usage == DxsoUsage::Fog && index == 0) return 10;
  std::lock_guard<std::mutex> lock(g_slot_mutex);
  for (uint32_t i = 11; i < g_slot_count; i++)
    if (g_slots[i].usage == usage && g_slots[i].usageIndex == index) return i;
  if (g_slot_count >= DXSO_MAX_SLOTS) return ~0u;
  g_slots[g_slot_count] = DxsoSemantic{usage, index};
  return g_slot_count++;
}

const char *usage_name(DxsoUsage u) {
  switch (u) {
  case DxsoUsage::Position: return "POSITION";
  case DxsoUsage::BlendWeight: return "BLENDWEIGHT";
  case DxsoUsage::BlendIndices: return "BLENDINDICES";
  case DxsoUsage::Normal: return "NORMAL";
  case DxsoUsage::PointSize: return "PSIZE";
  case DxsoUsage::Texcoord: return "TEXCOORD";
  case DxsoUsage::Tangent: return "TANGENT";
  case DxsoUsage::Binormal: return "BINORMAL";
  case DxsoUsage::TessFactor: return "TESSFACTOR";
  case DxsoUsage::PositionT: return "POSITIONT";
  case DxsoUsage::Color: return "COLOR";
  case DxsoUsage::Fog: return "FOG";
  case DxsoUsage::Depth: return "DEPTH";
  case DxsoUsage::Sample: return "SAMPLE";
  }
  return "UNKNOWN";
}

/* ps 1.x: ops that sample the texture of the stage named by the destination register */
static bool ps1_samples(DxsoOpcode op) {
  switch (op) {
  case DxsoOpcode::Tex: case DxsoOpcode::TexBem: case DxsoOpcode::TexBemL: case DxsoOpcode::TexReg2Ar:
  case DxsoOpcode::TexReg2Gb: case DxsoOpcode::TexReg2Rgb: case DxsoOpcode::TexM3x2Tex: case DxsoOpcode::TexM3x3Tex:
  case DxsoOpcode::TexM3x3Spec: case DxsoOpcode::TexM3x3VSpec: case DxsoOpcode::TexDp3Tex:
    return true;
  default: return false;
  }
}
/* ps 1.0-1.3: ops that read the texture coordinate set named by the destination register */
static bool ps1_texop(DxsoOpcode op) {
  switch (op) {
  case DxsoOpcode::Tex: case DxsoOpcode::TexCoord: case DxsoOpcode::TexKill: case DxsoOpcode::TexBem:
  case DxsoOpcode::TexBemL: case DxsoOpcode::TexM3x2Pad: case DxsoOpcode::TexM3x2Tex: case DxsoOpcode::TexM3x3Pad:
  case DxsoOpcode::TexM3x3Tex: case DxsoOpcode::TexM3x3Spec: case DxsoOpcode::TexM3x3VSpec: case DxsoOpcode::TexDp3Tex:
  case DxsoOpcode::TexDp3: case DxsoOpcode::TexM3x3: case DxsoOpcode::TexM3x2Depth:
    return true;
  default: return false;
  }
}

/* ------------------------------------------------------------------ translator --------------------------------------- */
struct Translator {
  const uint32_t *tokens; size_t max_dwords;
  dxso_variant var{};
  DxsoProgramInfo prog;
  bool vs = true, ps1 = false, ps14 = false;
  std::string err;
  std::vector<DxsoInstructionContext> insts;
  std::map<uint32_t, std::vector<DxsoInstructionContext>> labels;  /* label# -> body (inlined at call sites) */
  dxso_info info{};
  std::vector<dxso_def> defs;

  /* collected */
  uint32_t max_user_temp = 0;
  std::map<uint32_t, DxsoSemantic> out_sem;  /* VS3 o# */
  std::map<uint32_t, DxsoSemantic> in_sem;   /* VS v#, PS3 v# */
  uint32_t ps_input_mask[DXSO_MAX_SLOTS] = {};
  bool uses_vpos = false, uses_vface = false, uses_cb1 = false, uses_cb2 = false;
  uint32_t loop_depth_max = 0;

  /* temps */
  uint32_t next_temp = 0;
  uint32_t t_pos = ~0u, t_slot[DXSO_MAX_SLOTS], t_psize = ~0u, t_oc[4], t_depth = ~0u;
  uint32_t t_psz = ~0u; bool psize_written = false;   /* (DX9FULL) the real point size output (oPts / PSIZE); t_psize sinks the rest */
  uint32_t t_addr = ~0u, t_pred = ~0u, t_vpos = ~0u, t_vface = ~0u, t_loop[8], t_scratch = 0, t_in[16];
  uint32_t t_tex[8];                 /* ps 1.0-1.3: texture registers t# */
  uint32_t t_m3 = ~0u;               /* ps 1.x texm3x2/3x3 pad results (x, y, z) and eye vector for vspec (w unused) */
  uint32_t t_m3eye = ~0u;
  uint32_t m3_row = 0;               /* rows of the current texm3x* sequence written so far */
  bool uses_bump = false;
  uint32_t scratch_used = 0;
  static constexpr uint32_t kScratch = 8;
  uint32_t loop_depth = 0;
  struct LoopInfo { bool is_loop; uint32_t ireg; };
  std::vector<LoopInfo> loops;

  std::vector<uint32_t> decl, code;

  bool fail(const char *fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (err.empty()) err = b;
    return false;
  }

  /* ---------- emission ---------- */
  void ins(uint32_t opcode, std::initializer_list<Opnd> ops, uint32_t controls = 0, bool sat = false) {
    std::vector<uint32_t> body;
    for (auto &o : ops) encode(body, o);
    code.push_back((opcode & 0x7FF) | controls | (sat ? D3D10_SB_INSTRUCTION_SATURATE_MASK : 0) |
                   ((uint32_t)(body.size() + 1) << 24));
    code.insert(code.end(), body.begin(), body.end());
  }
  void dcl(uint32_t opcode, std::initializer_list<Opnd> ops, uint32_t controls, std::initializer_list<uint32_t> extra = {}) {
    std::vector<uint32_t> body;
    for (auto &o : ops) encode(body, o);
    body.insert(body.end(), extra.begin(), extra.end());
    decl.push_back((opcode & 0x7FF) | controls | ((uint32_t)(body.size() + 1) << 24));
    decl.insert(decl.end(), body.begin(), body.end());
  }
  static constexpr uint32_t NZ = 1u << D3D10_SB_INSTRUCTION_TEST_BOOLEAN_SHIFT;

  uint32_t scratch() {
    if (scratch_used >= kScratch) { fail("out of scratch temps"); return t_scratch; }
    return t_scratch + scratch_used++;
  }

  /* ---------- pass 1: decode and collect ---------- */
  bool decode() {
    uint32_t version = tokens[0];
    uint32_t type = version >> 16;
    if (type != 0xFFFE && type != 0xFFFF) return fail("bad version token %08x", version);
    vs = type == 0xFFFE;
    prog = DxsoProgramInfo(vs ? DxsoProgramTypes::VertexShader : DxsoProgramTypes::PixelShader, version & 0xFF, (version >> 8) & 0xFF);
    if (prog.majorVersion() > 3 || prog.majorVersion() < 1) return fail("unsupported version %08x", version);
    ps1 = !vs && prog.majorVersion() == 1;
    ps14 = ps1 && prog.minorVersion() >= 4;
    DxsoDecodeContext ctx(prog);
    DxsoCodeIter it(tokens + 1);
    std::vector<DxsoInstructionContext> *cur = &insts;
    for (;;) {
      if ((size_t)(it.ptrAt(0) - tokens) >= max_dwords) return fail("no END token within bounds");
      if (!ctx.decodeInstruction(it)) break;
      if (ctx.failed()) return fail("decode error");
      const auto &c = ctx.getInstructionContext();
      if (c.instruction.opcode == DxsoOpcode::Comment || c.instruction.opcode == DxsoOpcode::Nop ||
          c.instruction.opcode == DxsoOpcode::Phase)
        continue;
      if (c.instruction.opcode == DxsoOpcode::Label) { cur = &labels[c.dst.id.num]; continue; } /* label operand decodes as dst */
      cur->push_back(c);
    }
    info.token_count = (uint32_t)(it.ptrAt(0) - tokens);
    return true;
  }

  void note_reg(const DxsoRegister &r, bool is_dst) {
    switch (r.id.type) {
    case DxsoRegisterType::Temp: max_user_temp = std::max(max_user_temp, r.id.num + 1); break;
    case DxsoRegisterType::Const: case DxsoRegisterType::Const2: case DxsoRegisterType::Const3: case DxsoRegisterType::Const4:
      info.max_const_f = r.hasRelative ? 256 : std::max(info.max_const_f, r.id.num + 1); break;
    case DxsoRegisterType::ConstInt: uses_cb1 = true; break;
    case DxsoRegisterType::ConstBool: uses_cb2 = true; break;
    case DxsoRegisterType::MiscType:
      if (!vs && r.id.num == MiscTypePosition) uses_vpos = true;
      if (!vs && r.id.num == MiscTypeFace) uses_vface = true;
      break;
    case DxsoRegisterType::Input:
      /* ps 1.x reads v0/v1 without declarations */
      if (ps1 && !is_dst && r.id.num < 2) {
        if (!in_sem.count(r.id.num)) in_sem[r.id.num] = DxsoSemantic{DxsoUsage::Color, r.id.num};
        info.slots_read |= 1u << slot_of(DxsoUsage::Color, r.id.num);
      }
      break;
    case DxsoRegisterType::Texture:
      /* ps 1.4: t# are texture coordinate inputs (read only, no declarations) */
      if (ps14 && !is_dst && r.id.num < 8) info.slots_read |= 1u << slot_of(DxsoUsage::Texcoord, r.id.num);
      break;
    default: break;
    }
    if (r.hasRelative && r.relative.id.type == DxsoRegisterType::Loop) uses_cb1 = true;
    (void)is_dst;
  }

  bool collect(const std::vector<DxsoInstructionContext> &list) {
    for (const auto &c : list) {
      auto op = c.instruction.opcode;
      if (op == DxsoOpcode::Dcl) {
        const auto &d = c.dst;
        if (d.id.type == DxsoRegisterType::Sampler) {
          if (d.id.num < 16) { info.sampler_type[d.id.num] = (uint8_t)c.dcl.textureType; }
        } else if (vs && d.id.type == DxsoRegisterType::Input) {
          in_sem[d.id.num] = c.dcl.semantic;
          if (info.input_count < 16)
            info.inputs[info.input_count++] = {(uint8_t)c.dcl.semantic.usage, (uint8_t)c.dcl.semantic.usageIndex,
                                               (uint8_t)d.id.num, (uint8_t)0xF};
        } else if (vs && d.id.type == DxsoRegisterType::Output) {
          out_sem[d.id.num] = c.dcl.semantic;
        } else if (!vs && d.id.type == DxsoRegisterType::Input) {
          if (prog.majorVersion() >= 3) in_sem[d.id.num] = c.dcl.semantic;
          else in_sem[d.id.num] = DxsoSemantic{DxsoUsage::Color, d.id.num};
          uint32_t s = slot_of(in_sem[d.id.num].usage, in_sem[d.id.num].usageIndex);
          if (s == ~0u) return fail("out of interpolant slots");
          info.slots_read |= 1u << s;
        } else if (!vs && d.id.type == DxsoRegisterType::Texture) {
          uint32_t s = slot_of(DxsoUsage::Texcoord, d.id.num);
          info.slots_read |= 1u << s;
        } else if (!vs && d.id.type == DxsoRegisterType::MiscType) {
          if (d.id.num == MiscTypePosition) uses_vpos = true; else uses_vface = true;
        }
        continue;
      }
      if (op == DxsoOpcode::Def || op == DxsoOpcode::DefI || op == DxsoOpcode::DefB) {
        dxso_def d{};
        d.kind = op == DxsoOpcode::Def ? DXSO_DEF_FLOAT : op == DxsoOpcode::DefI ? DXSO_DEF_INT : DXSO_DEF_BOOL;
        d.reg = (uint16_t)c.dst.id.num;
        memcpy(d.value, c.def.uint32, 16);
        defs.push_back(d);
        if (op == DxsoOpcode::DefI) uses_cb1 = true;
        if (op == DxsoOpcode::DefB) uses_cb2 = true;
        continue;
      }
      if (op == DxsoOpcode::Loop || op == DxsoOpcode::Rep) { loop_depth++; loop_depth_max = std::max(loop_depth_max, loop_depth); uses_cb1 = true; }
      if (op == DxsoOpcode::EndLoop || op == DxsoOpcode::EndRep) loop_depth--;
      bool has_dst = !(op == DxsoOpcode::If || op == DxsoOpcode::Ifc || op == DxsoOpcode::Rep || op == DxsoOpcode::Loop ||
                       op == DxsoOpcode::BreakC || op == DxsoOpcode::BreakP || op == DxsoOpcode::Break ||
                       op == DxsoOpcode::Else || op == DxsoOpcode::EndIf || op == DxsoOpcode::EndLoop ||
                       op == DxsoOpcode::EndRep || op == DxsoOpcode::Ret || op == DxsoOpcode::Call ||
                       op == DxsoOpcode::CallNz);
      if (has_dst) {
        note_reg(c.dst, true);
        if (vs) {
          uint32_t s = vs_out_slot(c.dst);
          if (s < DXSO_MAX_SLOTS) info.slots_written |= 1u << s;
        } else {
          if (c.dst.id.type == DxsoRegisterType::ColorOut) info.rt_mask |= 1u << c.dst.id.num;
          if (c.dst.id.type == DxsoRegisterType::DepthOut) info.writes_depth = 1;
        }
      }
      uint32_t nsrc = 0;
      switch (op) {
      case DxsoOpcode::If: case DxsoOpcode::Rep: case DxsoOpcode::BreakP: nsrc = 1; break;
      case DxsoOpcode::Ifc: case DxsoOpcode::Loop: case DxsoOpcode::BreakC: nsrc = 2; break;
      default: nsrc = has_dst ? 4 : 0; break;
      }
      for (uint32_t i = 0; i < nsrc; i++) note_reg(c.src[i], false);
      if (ps1) {
        /* ps 1.x texture ops: the sampler (and, for 1.0-1.3, the texcoord) is the destination register number */
        uint32_t n = c.dst.id.num;
        if (ps1_samples(op) && n < 8) {
          info.sampler_mask |= 1u << n;
          uint32_t t = (var.flags >> (DXSO_PS1_SAMPLER_SHIFT + 2 * n)) & 3;
          info.sampler_type[n] = t == 1 ? 3 : t == 2 ? 4 : 2;
        }
        if (!ps14 && ps1_texop(op) && n < 8) info.slots_read |= 1u << slot_of(DxsoUsage::Texcoord, n);
        if (op == DxsoOpcode::TexBem || op == DxsoOpcode::TexBemL || op == DxsoOpcode::Bem) uses_bump = true;
      } else if (op == DxsoOpcode::Tex || op == DxsoOpcode::TexLdl || op == DxsoOpcode::TexLdd) {
        uint32_t s = c.src[1].id.num;
        if (s < 16) { info.sampler_mask |= 1u << s; if (!info.sampler_type[s]) info.sampler_type[s] = 2; }
      }
    }
    return err.empty();
  }

  /* VS output register -> interpolant slot (DXSO_MAX_SLOTS = position, +1 = psize/ignored) */
  uint32_t vs_out_slot(const DxsoRegister &r) {
    const uint32_t kPos = DXSO_MAX_SLOTS, kIgnore = DXSO_MAX_SLOTS + 1, kPsz = DXSO_MAX_SLOTS + 2;
    switch (r.id.type) {
    case DxsoRegisterType::RasterizerOut:
      if (r.id.num == RasterOutPosition) return kPos;
      if (r.id.num == RasterOutFog) return slot_of(DxsoUsage::Fog, 0);
      if (r.id.num == RasterOutPointSize) return kPsz;
      return kIgnore;
    case DxsoRegisterType::AttributeOut: return slot_of(DxsoUsage::Color, r.id.num);
    case DxsoRegisterType::Output: /* == TexcoordOut */
      if (prog.majorVersion() < 3) return slot_of(DxsoUsage::Texcoord, r.id.num);
      {
        auto it = out_sem.find(r.id.num);
        if (it == out_sem.end()) return kIgnore;
        if (it->second.usage == DxsoUsage::Position || it->second.usage == DxsoUsage::PositionT) return kPos;
        if (it->second.usage == DxsoUsage::PointSize) return kPsz;
        uint32_t s = slot_of(it->second.usage, it->second.usageIndex);
        return s == ~0u ? kIgnore : s;
      }
    default: return ~0u;
    }
  }

  /* ---------- operands ---------- */
  Opnd dst_of(const DxsoRegister &r, bool &ok) {
    ok = true;
    uint32_t m = 0;
    for (uint32_t i = 0; i < 4; i++) if (r.mask[i]) m |= 1u << i;
    switch (r.id.type) {
    case DxsoRegisterType::Temp: return T(r.id.num).mask(m);
    case DxsoRegisterType::Addr: /* == Texture */
      if (vs) return T(t_addr).mask(m);
      if (ps1 && !ps14 && r.id.num < 8) return T(t_tex[r.id.num]).mask(m);
      break;
    case DxsoRegisterType::Predicate: return T(t_pred).mask(m);
    case DxsoRegisterType::RasterizerOut: case DxsoRegisterType::AttributeOut: case DxsoRegisterType::Output: {
      if (!vs) break;
      uint32_t s = vs_out_slot(r);
      if (s == DXSO_MAX_SLOTS) return T(t_pos).mask(m);
      if (s < DXSO_MAX_SLOTS) return T(t_slot[s]).mask(m);
      if (s == DXSO_MAX_SLOTS + 2) { psize_written = true; return T(t_psz).mask(m); }
      return T(t_psize).mask(m);
    }
    case DxsoRegisterType::ColorOut: if (!vs && r.id.num < 4) return T(t_oc[r.id.num]).mask(m); break;
    case DxsoRegisterType::DepthOut: if (!vs) return T(t_depth).mask(1); break;
    default: break;
    }
    ok = false; fail("unsupported destination register type %u", (unsigned)r.id.type);
    return T(0);
  }

  void apply_relative(Opnd &o, const DxsoRegister &r) {
    if (!r.hasRelative) return;
    o.rel = true;
    if (r.relative.id.type == DxsoRegisterType::Loop) { o.rel_temp = t_loop[loop_depth ? loop_depth - 1 : 0]; o.rel_comp = 0; }
    else { o.rel_temp = t_addr; o.rel_comp = r.relative.swizzle[0]; }
  }

  /* raw source (register file + swizzle + neg/abs), no complex modifiers */
  Opnd src_raw(const DxsoRegister &r) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < 4; i++) s |= r.swizzle[i] << (2 * i);
    Opnd o;
    switch (r.id.type) {
    case DxsoRegisterType::Temp: o = T(r.id.num); break;
    case DxsoRegisterType::Input:
      if (vs) { o = (var.int_inputs >> r.id.num & 1) ? T(t_in[r.id.num]) : V(r.id.num); }
      else {
        auto it = in_sem.find(r.id.num);
        DxsoSemantic sem = it != in_sem.end() ? it->second : DxsoSemantic{DxsoUsage::Color, r.id.num};
        uint32_t slot = slot_of(sem.usage, sem.usageIndex);
        if (r.hasRelative) fail("relative input addressing not supported");
        o = V(slot + 1);
      }
      break;
    case DxsoRegisterType::Const: case DxsoRegisterType::Const2: case DxsoRegisterType::Const3: case DxsoRegisterType::Const4: {
      uint32_t base = r.id.type == DxsoRegisterType::Const2 ? 2048 : r.id.type == DxsoRegisterType::Const3 ? 4096
                    : r.id.type == DxsoRegisterType::Const4 ? 6144 : 0;
      o = CB(0, base + r.id.num); apply_relative(o, r); break;
    }
    case DxsoRegisterType::Texture: /* == Addr */
      if (vs) o = T(t_addr);
      else if (ps1 && !ps14) o = T(t_tex[r.id.num & 7]);
      else o = V(slot_of(DxsoUsage::Texcoord, r.id.num) + 1);
      break;
    case DxsoRegisterType::ConstInt: o = CB(1, r.id.num); break;
    case DxsoRegisterType::ConstBool: o = CB(2, r.id.num); s = XXXX; break;
    case DxsoRegisterType::Loop: o = T(t_loop[loop_depth ? loop_depth - 1 : 0]); s = XXXX; break;
    case DxsoRegisterType::Predicate: o = T(t_pred); break;
    case DxsoRegisterType::MiscType:
      o = T(r.id.num == MiscTypePosition ? t_vpos : t_vface); break;
    case DxsoRegisterType::Sampler: o = RES(r.id.num); break;
    default: fail("unsupported source register type %u", (unsigned)r.id.type); o = T(0); break;
    }
    o.swz(s);
    return o;
  }

  /* full source with D3D9 modifiers applied (may emit instructions into a scratch temp) */
  Opnd src(const DxsoRegister &r) {
    Opnd o = src_raw(r);
    if (ps1 && (r.id.type == DxsoRegisterType::Const)) {
      /* ps 1.x clamps float constants to [-1, 1] (as DXVK) */
      uint32_t t = scratch();
      ins(D3D10_SB_OPCODE_MAX, {T(t).mask(15), o, IMMF1(-1.0f)});
      ins(D3D10_SB_OPCODE_MIN, {T(t).mask(15), Opnd(T(t)), IMMF1(1.0f)});
      o = T(t);
    }
    switch (r.modifier) {
    case DxsoRegModifier::None: return o;
    case DxsoRegModifier::Neg: return o.neg();
    case DxsoRegModifier::Abs: return o.abs();
    case DxsoRegModifier::AbsNeg: o.abs(); return o.neg();
    case DxsoRegModifier::Bias: { uint32_t t = scratch(); ins(D3D10_SB_OPCODE_ADD, {T(t).mask(15), o, IMMF1(-0.5f)}); return T(t); }
    case DxsoRegModifier::BiasNeg: { uint32_t t = scratch(); ins(D3D10_SB_OPCODE_ADD, {T(t).mask(15), o.neg(), IMMF1(0.5f)}); return T(t); }
    case DxsoRegModifier::Sign: { uint32_t t = scratch(); ins(D3D10_SB_OPCODE_MAD, {T(t).mask(15), o, IMMF1(2.0f), IMMF1(-1.0f)}); return T(t); }
    case DxsoRegModifier::SignNeg: { uint32_t t = scratch(); ins(D3D10_SB_OPCODE_MAD, {T(t).mask(15), o, IMMF1(-2.0f), IMMF1(1.0f)}); return T(t); }
    case DxsoRegModifier::Comp: { uint32_t t = scratch(); ins(D3D10_SB_OPCODE_ADD, {T(t).mask(15), o.neg(), IMMF1(1.0f)}); return T(t); }
    case DxsoRegModifier::X2: { uint32_t t = scratch(); ins(D3D10_SB_OPCODE_ADD, {T(t).mask(15), o, o}); return T(t); }
    case DxsoRegModifier::X2Neg: { uint32_t t = scratch(); ins(D3D10_SB_OPCODE_MUL, {T(t).mask(15), o, IMMF1(-2.0f)}); return T(t); }
    case DxsoRegModifier::Dz: case DxsoRegModifier::Dw: {
      uint32_t t = scratch(); Opnd d = o; d.swz(swz_rep(swz_comp(o.sel, r.modifier == DxsoRegModifier::Dz ? 2 : 3)));
      ins(D3D10_SB_OPCODE_DIV, {T(t).mask(15), o, d}); return T(t);
    }
    case DxsoRegModifier::Not: { uint32_t t = scratch(); ins(D3D10_SB_OPCODE_NOT, {T(t).mask(15), o}); return T(t); }
    }
    return o;
  }

  /* ---------- per-instruction ---------- */
  uint32_t mask_of(const DxsoRegister &r) { uint32_t m = 0; for (uint32_t i = 0; i < 4; i++) if (r.mask[i]) m |= 1u << i; return m; }

  Opnd cmp_args(DxsoComparison c, Opnd a, Opnd b, uint32_t &opcode) {
    switch (c) {
    case DxsoComparison::GreaterThan: opcode = D3D10_SB_OPCODE_LT; return b; /* swapped below */
    case DxsoComparison::Equal: opcode = D3D10_SB_OPCODE_EQ; break;
    case DxsoComparison::GreaterEqual: opcode = D3D10_SB_OPCODE_GE; break;
    case DxsoComparison::LessThan: opcode = D3D10_SB_OPCODE_LT; break;
    case DxsoComparison::NotEqual: opcode = D3D10_SB_OPCODE_NE; break;
    case DxsoComparison::LessEqual: opcode = D3D10_SB_OPCODE_GE; return b;
    default: opcode = 0; break;
    }
    return a;
  }
  /* writes compare(a, b) into t.mask */
  void emit_compare(DxsoComparison c, Opnd t, Opnd a, Opnd b) {
    switch (c) {
    case DxsoComparison::GreaterThan: ins(D3D10_SB_OPCODE_LT, {t, b, a}); break;
    case DxsoComparison::Equal: ins(D3D10_SB_OPCODE_EQ, {t, a, b}); break;
    case DxsoComparison::GreaterEqual: ins(D3D10_SB_OPCODE_GE, {t, a, b}); break;
    case DxsoComparison::LessThan: ins(D3D10_SB_OPCODE_LT, {t, a, b}); break;
    case DxsoComparison::NotEqual: ins(D3D10_SB_OPCODE_NE, {t, a, b}); break;
    case DxsoComparison::LessEqual: ins(D3D10_SB_OPCODE_GE, {t, b, a}); break;
    case DxsoComparison::Never: ins(D3D10_SB_OPCODE_MOV, {t, IMMU1(0)}); break;
    case DxsoComparison::Always: ins(D3D10_SB_OPCODE_MOV, {t, IMMU1(~0u)}); break;
    }
  }

  void emit_texld(const DxsoInstructionContext &c, Opnd d, bool sat) {
    auto op = c.instruction.opcode;
    uint32_t s = c.src[1].id.num;
    uint32_t ttype = s < 16 ? info.sampler_type[s] : 2;
    Opnd coord = src(c.src[0]);
    Opnd res = RES(s); res.swz(src_raw(c.src[1]).sel);
    Opnd smp = SMP(s);
    (void)ttype;
    bool shadow = s < 16 && (var.depth_samplers >> s & 1), raw_depth = s < 16 && (var.depth_samplers >> (16 + s) & 1);
    /* (DX9FULL 13.11.2026) both bits: a vendor depth texture DF16/DF24 - raw depth, no compare, read as (d, 0, 0, 1) (DXVK
       d3d9_format.cpp: swizzle R,0,0,1; d3d9_common_texture.cpp: DF16/DF24/INTZ are never "shadow") */
    bool df = shadow && raw_depth; if (df) shadow = false;
    /* (DX9FULL) Fetch4 (ATI, D3DSAMP_MIPMAPLODBIAS = 'GET4'): PS sm2/3 variant flag bits 16-31; the core sets a bit only for a
       single-channel 2D texture with point MAGFILTER (DXVK UpdateActiveFetch4) */
    bool fetch4 = !vs && !ps1 && s < 16 && !shadow && (var.flags >> (16 + s) & 1) && (ttype == 2 || ttype == 0);
    uint32_t sswz = res.sel;                  /* the instruction's sampler swizzle, applied to the fetched value */
    if (shadow || raw_depth) res.swz(XXXX);   /* one depth/compare value, replicated (wined3d: INTZ "XXXX") */
    if (op == DxsoOpcode::Tex && c.instruction.specificData.texld == DxsoTexLdMode::Project) {
      uint32_t t = scratch(); Opnd w = coord; w.swz(swz_rep(swz_comp(coord.sel, 3)));
      ins(D3D10_SB_OPCODE_DIV, {T(t).mask(15), coord, w});
      coord = T(t);
    }
    if (fetch4) {
      /* DXVK dxso_compiler.cpp: gather of the red channel at coord + (1 - 1/256) / (2 * size) (the D3D9 Fetch4 footprint
         starts at the point-sampled texel), returned in the D3D9 order (gather.z, gather.x, gather.y, gather.w), then the
         instruction's sampler swizzle */
      uint32_t ta = scratch();
      Opnd lvl = IMMU1(0); lvl.immn = 1;
      ins(D3D10_SB_OPCODE_RESINFO, {T(ta).mask(3), lvl, Opnd(RES(s)).swz(XYZW)},
          ENCODE_D3D10_SB_RESINFO_INSTRUCTION_RETURN_TYPE(D3D10_SB_RESINFO_INSTRUCTION_RETURN_RCPFLOAT));
      Opnd cxy = coord;
      ins(D3D10_SB_OPCODE_MAD, {T(ta).mask(3), Opnd(T(ta)).swz(XYZW), IMMF1(0.5f * (1.0f - 1.0f / 256.0f)), cxy});
      Opnd r4 = RES(s); r4.swz(swz_compose(SWZ(2, 0, 1, 3), sswz));
      Opnd s4 = SMP(s); s4.ncomp = 2; s4.sel1((var.fill_slots >> s & 1) ? 3 : 0);   /* (DX9FULL v13) PS fill_slots: A8, gather alpha */
      ins(D3D10_1_SB_OPCODE_GATHER4, {d, Opnd(T(ta)).swz(XYZW), r4, s4}, 0, sat);
      return;
    }
    Opnd d_final = d; bool sat_final = sat;
    uint32_t t_df = 0;
    if (df) { t_df = scratch(); d = T(t_df).mask(15); sat = false; }
    if (shadow) {
      /* depth compare against the (projected) coordinate's z, as wined3d's shadow2D / shadow2DProj */
      Opnd ref = coord; ref.sel1(swz_comp(coord.sel, 2));
      if (vs || op == DxsoOpcode::TexLdl) ins(D3D10_SB_OPCODE_SAMPLE_C_LZ, {d, coord, res, smp, ref}, 0, sat);
      else ins(D3D10_SB_OPCODE_SAMPLE_C, {d, coord, res, smp, ref}, 0, sat);
      return;
    }
    if (op == DxsoOpcode::Tex && c.instruction.specificData.texld == DxsoTexLdMode::Bias) {
      Opnd b = coord; b.sel1(swz_comp(coord.sel, 3));
      ins(D3D10_SB_OPCODE_SAMPLE_B, {d, coord, res, smp, b}, 0, sat);
    } else if (op == DxsoOpcode::TexLdl) {
      Opnd l = coord; l.sel1(swz_comp(coord.sel, 3));
      ins(D3D10_SB_OPCODE_SAMPLE_L, {d, coord, res, smp, l}, 0, sat);
    } else if (op == DxsoOpcode::TexLdd) {
      ins(D3D10_SB_OPCODE_SAMPLE_D, {d, coord, res, smp, src(c.src[2]), src(c.src[3])}, 0, sat);
    } else if (vs) {
      Opnd l = IMMF1(0.0f); l.immn = 1;
      ins(D3D10_SB_OPCODE_SAMPLE_L, {d, coord, res, smp, l}, 0, sat);
    } else {
      ins(D3D10_SB_OPCODE_SAMPLE, {d, coord, res, smp}, 0, sat);
    }
    if (df) {
      ins(D3D10_SB_OPCODE_MOV, {T(t_df).mask(0xE), IMMF(0.0f, 0.0f, 0.0f, 1.0f)});
      ins(D3D10_SB_OPCODE_MOV, {d_final, Opnd(T(t_df)).swz(sswz)}, 0, sat_final);
    }
  }

  bool emit_list(const std::vector<DxsoInstructionContext> &list, uint32_t inline_depth) {
    for (const auto &c : list) {
      if (!emit_one(c, inline_depth)) return false;
      if (!err.empty()) return false;
    }
    return true;
  }

  void epilogue() {
    if (vs) {
      /* fog coordinate (wined3d model: the PS evaluates the fog equation on the interpolated coordinate) */
      uint32_t fs = DXSO_SLOT_FOG;
      if (t_slot[fs] != ~0u) {
        uint32_t src = var.vs_flags & 3;
        if (src == 1) ins(D3D10_SB_OPCODE_MOV, {T(t_slot[fs]).mask(1), Opnd(T(t_pos)).swz(swz_rep(2))});
        else if (src == 2) ins(D3D10_SB_OPCODE_MOV, {T(t_slot[fs]).mask(1), Opnd(T(t_pos)).swz(swz_rep(3))});
        else if (!(info.slots_written >> fs & 1) && (info.slots_written >> 9 & 1) && prog.majorVersion() < 3)
          ins(D3D10_SB_OPCODE_MOV, {T(t_slot[fs]).mask(1), Opnd(T(t_slot[9])).swz(swz_rep(3))});
      }
      if (var.vs_flags >> DXSO_VS_POINT_SHIFT & 1) {
        /* (DX9FULL 13.11.2026) D3D9 point size / point sprites: the core draws every point as an instanced 4-vertex strip
           (instance = the point's vertex), the corner comes from vertex_id; size = oPts or D3DRS_POINTSIZE, clamped to
           [POINTSIZE_MIN, POINTSIZE_MAX]; offset = corner * size / viewport in clip space (x w); POINTSPRITEENABLE (bit 17):
           texture coordinates 0-7 become (u, v, 0, 1) across the point, as DXVK / D3D9 */
        scratch_used = 0;
        uint32_t ts = scratch(), tc = scratch();
        if (psize_written) ins(D3D10_SB_OPCODE_MOV, {T(ts).mask(1), Opnd(T(t_psz)).swz(XXXX)});
        else ins(D3D10_SB_OPCODE_MOV, {T(ts).mask(1), Opnd(CB(3, 6)).swz(XXXX)});
        ins(D3D10_SB_OPCODE_MAX, {T(ts).mask(1), Opnd(T(ts)).swz(XXXX), Opnd(CB(3, 6)).swz(swz_rep(1))});
        ins(D3D10_SB_OPCODE_MIN, {T(ts).mask(1), Opnd(T(ts)).swz(XXXX), Opnd(CB(3, 6)).swz(swz_rep(2))});
        ins(D3D10_SB_OPCODE_AND, {T(tc).mask(1), Opnd(V(16)).swz(XXXX), IMMU1(1)});
        ins(D3D10_SB_OPCODE_USHR, {T(tc).mask(2), Opnd(V(16)).swz(XXXX), IMMU1(1)});
        ins(D3D10_SB_OPCODE_UTOF, {T(tc).mask(3), Opnd(T(tc)).swz(XYZW)});
        ins(D3D10_SB_OPCODE_MAD, {T(tc).mask(0xC), Opnd(T(tc)).swz(SWZ(0, 0, 0, 1)), IMMF(0, 0, 2.0f, -2.0f), IMMF(0, 0, -1.0f, 1.0f)});
        ins(D3D10_SB_OPCODE_MUL, {T(ts).mask(6), Opnd(T(tc)).swz(SWZ(0, 2, 3, 0)), Opnd(T(ts)).swz(XXXX)});
        ins(D3D10_SB_OPCODE_MUL, {T(ts).mask(6), Opnd(T(ts)).swz(XYZW), Opnd(CB(3, 7)).swz(SWZ(0, 0, 1, 0))});
        ins(D3D10_SB_OPCODE_MAD, {T(t_pos).mask(3), Opnd(T(ts)).swz(SWZ(1, 2, 0, 0)), Opnd(T(t_pos)).swz(swz_rep(3)), Opnd(T(t_pos)).swz(XYZW)});
        if (var.vs_flags >> (DXSO_VS_POINT_SHIFT + 1) & 1)
          for (uint32_t s = 0; s < 8; s++) {
            uint32_t sl = slot_of(DxsoUsage::Texcoord, s);
            if (sl >= DXSO_MAX_SLOTS || t_slot[sl] == ~0u) continue;
            ins(D3D10_SB_OPCODE_MOV, {T(t_slot[sl]).mask(3), Opnd(T(tc)).swz(XYZW)});
            ins(D3D10_SB_OPCODE_MOV, {T(t_slot[sl]).mask(0xC), IMMF(0, 0, 0, 1.0f)});
          }
      }
      ins(D3D10_SB_OPCODE_MOV, {O(0).mask(15), T(t_pos)});
      for (uint32_t s = 0; s < DXSO_MAX_SLOTS; s++) {
        if (t_slot[s] == ~0u) continue;
        bool clamp = prog.majorVersion() < 3 && (s == 8 || s == 9);
        ins(D3D10_SB_OPCODE_MOV, {O(s + 1).mask(15), T(t_slot[s])}, 0, clamp);
      }
      uint32_t clip = (var.vs_flags >> DXSO_VS_CLIP_SHIFT) & 0x3F;
      if (clip) {
        /* user clip planes (clip space): distance = dot(plane, position); disabled planes do not clip */
        for (uint32_t i = 0; i < 6; i++) {
          Opnd d = O(17 + i / 4); d.mask(1u << (i & 3));
          if (clip >> i & 1) ins(D3D10_SB_OPCODE_DP4, {d, T(t_pos), CB(3, i)});
          else ins(D3D10_SB_OPCODE_MOV, {d, IMMF1(1.0f)});
        }
      }
    } else {
      if (ps1) ins(D3D10_SB_OPCODE_MOV, {T(t_oc[0]).mask(15), T(0)});
      uint32_t fog = (var.flags >> DXSO_PS_FOG_SHIFT) & 3;
      if (fog && t_oc[0] != ~0u) {
        /* fixed-function fog after the shader: f = sat(equation(coord)); colour.rgb = lerp(fog colour, colour.rgb, f) */
        uint32_t t = t_scratch;
        Opnd coord = V(DXSO_SLOT_FOG + 1); coord.swz(XXXX);
        Opnd prm = CB(3, 2);
        if (fog == 1) {
          ins(D3D10_SB_OPCODE_ADD, {T(t).mask(1), Opnd(prm).swz(XXXX), Opnd(coord).neg()});
          ins(D3D10_SB_OPCODE_MUL, {T(t).mask(1), Opnd(T(t)).swz(XXXX), Opnd(prm).swz(swz_rep(1))}, 0, true);
        } else {
          ins(D3D10_SB_OPCODE_MUL, {T(t).mask(1), coord, Opnd(prm).swz(swz_rep(2))});
          if (fog == 3) ins(D3D10_SB_OPCODE_MUL, {T(t).mask(1), Opnd(T(t)).swz(XXXX), Opnd(T(t)).swz(XXXX)});
          ins(D3D10_SB_OPCODE_MUL, {T(t).mask(1), Opnd(T(t)).swz(XXXX), IMMF1(-1.44269504f)});
          ins(D3D10_SB_OPCODE_EXP, {T(t).mask(1), Opnd(T(t)).swz(XXXX)}, 0, true);
        }
        Opnd fc = CB(3, 1);
        ins(D3D10_SB_OPCODE_ADD, {T(t).mask(14), Opnd(T(t_oc[0])).swz(SWZ(0, 0, 1, 2)), Opnd(fc).swz(SWZ(0, 0, 1, 2)).neg()});
        ins(D3D10_SB_OPCODE_MAD, {T(t_oc[0]).mask(7), Opnd(T(t)).swz(SWZ(1, 2, 3, 3)), Opnd(T(t)).swz(XXXX), fc});
      }
      uint32_t af = var.flags & 0xF;
      if (af && af != 8 /* ALWAYS */ && t_oc[0] != ~0u) {
        /* D3D9 alpha test on oC0.a in the precision of render target 0 (upstream DXVK DoFixedFunctionAlphaTest):
           fixed point (8-bit targets: round_ne(alpha * 255) against the integer reference) or float; the reference
           in cb3[0].x is already in that unit (the core converts it) */
        uint32_t t = t_scratch;
        uint32_t prec = (var.flags >> DXSO_PS_ALPHA_PREC_SHIFT) & 0xF;
        if (prec > 8) {
          ins(D3D10_SB_OPCODE_MOV, {T(t).mask(1), Opnd(T(t_oc[0])).swz(swz_rep(3))});
        } else {
          ins(D3D10_SB_OPCODE_MOV, {T(t).mask(1), Opnd(T(t_oc[0])).swz(swz_rep(3))}, 0, true);
          ins(D3D10_SB_OPCODE_MUL, {T(t).mask(1), Opnd(T(t)).swz(XXXX), IMMF1((float)((256u << prec) - 1))});
          ins(D3D10_SB_OPCODE_ROUND_NE, {T(t).mask(1), Opnd(T(t)).swz(XXXX)});
        }
        static const DxsoComparison map[8] = {DxsoComparison::Never, DxsoComparison::LessThan, DxsoComparison::Equal,
            DxsoComparison::LessEqual, DxsoComparison::GreaterThan, DxsoComparison::NotEqual, DxsoComparison::GreaterEqual,
            DxsoComparison::Always};
        emit_compare(map[(af - 1) & 7], T(t).mask(2), Opnd(T(t)).swz(XXXX), Opnd(CB(3, 0)).swz(XXXX));
        Opnd pass = T(t); pass.sel1(1);
        ins(D3D10_SB_OPCODE_DISCARD, {pass}, 0 /* discard when the test fails */);
      }
      for (uint32_t i = 0; i < 4; i++)
        if (info.rt_mask >> i & 1) ins(D3D10_SB_OPCODE_MOV, {O(i).mask(15), T(t_oc[i])});
      if (info.writes_depth) { Opnd s = T(t_depth); s.sel1(0); ins(D3D10_SB_OPCODE_MOV, {ODEPTH(), s}); }
    }
  }

  /* ps 1.x texture-stage instructions (DXVK dxso_compiler semantics). Sets handled = false for everything else. */
  bool emit_ps1(const DxsoInstructionContext &c, bool &handled) {
    auto op = c.instruction.opcode;
    uint32_t n = c.dst.id.num & 7;
    handled = true;
    Opnd tc = V(slot_of(DxsoUsage::Texcoord, n) + 1);
    Opnd dt = T(t_tex[n]);
    auto sample = [&](Opnd d, Opnd coord) { ins(D3D10_SB_OPCODE_SAMPLE, {d, coord, RES(n), SMP(n)}, 0, false); };
    auto bump = [&](uint32_t comp) { Opnd b = CB(3, 4 + n); return b.swz(swz_rep(comp)); };
    auto rep = [](Opnd o, uint32_t comp) { Opnd x = o; x.swz(swz_rep(swz_comp(o.sel, comp))); return x; };
    switch (op) {
    case DxsoOpcode::Tex:
      if (ps14) {
        bool ok; Opnd d = dst_of(c.dst, ok); if (!ok) return false;
        ins(D3D10_SB_OPCODE_SAMPLE, {d, src(c.src[0]), RES(n), SMP(n)}, 0, c.dst.saturate);
      } else sample(Opnd(dt).mask(15), tc);
      return true;
    case DxsoOpcode::TexCoord:
      if (ps14) {
        bool ok; Opnd d = dst_of(c.dst, ok); if (!ok) return false;
        ins(D3D10_SB_OPCODE_MOV, {d, src(c.src[0])}, 0, c.dst.saturate);
      } else {
        ins(D3D10_SB_OPCODE_MOV, {Opnd(dt).mask(7), tc}, 0, true);
        ins(D3D10_SB_OPCODE_MOV, {Opnd(dt).mask(8), IMMF1(1.0f)});
      }
      return true;
    case DxsoOpcode::TexBem: case DxsoOpcode::TexBemL: {
      Opnd m = src(c.src[0]);
      uint32_t t = scratch();
      ins(D3D10_SB_OPCODE_MAD, {T(t).mask(1), rep(m, 0), bump(0), rep(tc, 0)});
      ins(D3D10_SB_OPCODE_MAD, {T(t).mask(1), rep(m, 1), bump(2), Opnd(T(t)).swz(XXXX)});
      ins(D3D10_SB_OPCODE_MAD, {T(t).mask(2), rep(m, 0), bump(1), rep(tc, 1)});
      ins(D3D10_SB_OPCODE_MAD, {T(t).mask(2), rep(m, 1), bump(3), Opnd(T(t)).swz(swz_rep(1))});
      sample(Opnd(dt).mask(15), Opnd(T(t)).swz(SWZ(0, 1, 1, 1)));
      if (op == DxsoOpcode::TexBemL) {
        Opnd l = CB(3, 12 + n);
        ins(D3D10_SB_OPCODE_MAD, {T(t).mask(4), rep(m, 2), Opnd(l).swz(XXXX), Opnd(l).swz(swz_rep(1))}, 0, true);
        ins(D3D10_SB_OPCODE_MUL, {Opnd(dt).mask(7), Opnd(dt), Opnd(T(t)).swz(swz_rep(2))});
      }
      return true;
    }
    case DxsoOpcode::TexReg2Ar: { Opnd m = src(c.src[0]); sample(Opnd(dt).mask(15), Opnd(m).swz(swz_compose(m.sel, SWZ(3, 0, 0, 0)))); return true; }
    case DxsoOpcode::TexReg2Gb: { Opnd m = src(c.src[0]); sample(Opnd(dt).mask(15), Opnd(m).swz(swz_compose(m.sel, SWZ(1, 2, 2, 2)))); return true; }
    case DxsoOpcode::TexReg2Rgb: { Opnd m = src(c.src[0]); sample(Opnd(dt).mask(15), Opnd(m).swz(swz_compose(m.sel, SWZ(0, 1, 2, 2)))); return true; }
    case DxsoOpcode::TexM3x2Pad: case DxsoOpcode::TexM3x3Pad: {
      uint32_t row = m3_row++ & 3;
      ins(D3D10_SB_OPCODE_DP3, {T(t_m3).mask(1u << row), tc, src(c.src[0])});
      ins(D3D10_SB_OPCODE_MOV, {T(t_m3eye).mask(1u << row), rep(tc, 3)});
      return true;
    }
    case DxsoOpcode::TexM3x2Tex: case DxsoOpcode::TexM3x3Tex: case DxsoOpcode::TexM3x3: case DxsoOpcode::TexM3x3Spec:
    case DxsoOpcode::TexM3x3VSpec: {
      uint32_t row = op == DxsoOpcode::TexM3x2Tex ? 1 : 2;
      m3_row = 0;
      ins(D3D10_SB_OPCODE_DP3, {T(t_m3).mask(1u << row), tc, src(c.src[0])});
      if (op == DxsoOpcode::TexM3x2Tex) { sample(Opnd(dt).mask(15), Opnd(T(t_m3)).swz(SWZ(0, 1, 1, 1))); return true; }
      if (op == DxsoOpcode::TexM3x3Tex) { sample(Opnd(dt).mask(15), Opnd(T(t_m3)).swz(SWZ(0, 1, 2, 2))); return true; }
      if (op == DxsoOpcode::TexM3x3) {
        ins(D3D10_SB_OPCODE_MOV, {Opnd(dt).mask(7), T(t_m3)});
        ins(D3D10_SB_OPCODE_MOV, {Opnd(dt).mask(8), IMMF1(1.0f)});
        return true;
      }
      /* reflection of the eye vector E about the normal N = t_m3.xyz: R = 2 N (N.E) / (N.N) - E */
      Opnd e;
      if (op == DxsoOpcode::TexM3x3Spec) e = src(c.src[1]);
      else { ins(D3D10_SB_OPCODE_MOV, {T(t_m3eye).mask(4), rep(tc, 3)}); e = T(t_m3eye); }
      uint32_t t = scratch(), r = scratch();
      Opnd N = Opnd(T(t_m3)).swz(SWZ(0, 1, 2, 2));
      ins(D3D10_SB_OPCODE_DP3, {T(t).mask(1), N, e});
      ins(D3D10_SB_OPCODE_DP3, {T(t).mask(2), N, N});
      ins(D3D10_SB_OPCODE_DIV, {T(t).mask(1), Opnd(T(t)).swz(XXXX), Opnd(T(t)).swz(swz_rep(1))});
      ins(D3D10_SB_OPCODE_ADD, {T(t).mask(1), Opnd(T(t)).swz(XXXX), Opnd(T(t)).swz(XXXX)});
      ins(D3D10_SB_OPCODE_MAD, {T(r).mask(7), N, Opnd(T(t)).swz(XXXX), Opnd(e).neg()});
      sample(Opnd(dt).mask(15), Opnd(T(r)).swz(SWZ(0, 1, 2, 2)));
      return true;
    }
    case DxsoOpcode::TexDp3Tex: {
      uint32_t t = scratch();
      ins(D3D10_SB_OPCODE_DP3, {T(t).mask(1), tc, src(c.src[0])});
      ins(D3D10_SB_OPCODE_MOV, {T(t).mask(14), IMMF1(0.0f)});
      sample(Opnd(dt).mask(15), T(t));
      return true;
    }
    case DxsoOpcode::TexDp3: ins(D3D10_SB_OPCODE_DP3, {Opnd(dt).mask(15), tc, src(c.src[0])}); return true;
    case DxsoOpcode::Bem: {
      bool ok; Opnd d = dst_of(c.dst, ok); if (!ok) return false;
      Opnd a = src(c.src[0]), b = src(c.src[1]);
      uint32_t t = scratch();
      ins(D3D10_SB_OPCODE_MAD, {T(t).mask(1), rep(b, 0), bump(0), rep(a, 0)});
      ins(D3D10_SB_OPCODE_MAD, {T(t).mask(1), rep(b, 1), bump(2), Opnd(T(t)).swz(XXXX)});
      ins(D3D10_SB_OPCODE_MAD, {T(t).mask(2), rep(b, 0), bump(1), rep(a, 1)});
      ins(D3D10_SB_OPCODE_MAD, {T(t).mask(2), rep(b, 1), bump(3), Opnd(T(t)).swz(swz_rep(1))});
      Opnd dd = d; dd.sel &= 3;
      ins(D3D10_SB_OPCODE_MOV, {dd, T(t)});
      return true;
    }
    case DxsoOpcode::TexDepth: case DxsoOpcode::TexM3x2Depth:
      return fail("ps_1_x %s not supported", op == DxsoOpcode::TexDepth ? "texdepth" : "texm3x2depth");
    default:
      handled = false;
      return true;
    }
  }

  bool emit_one(const DxsoInstructionContext &c, uint32_t inline_depth) {
    scratch_used = 0;
    auto op = c.instruction.opcode;
    bool ok = true;
    if (ps1) {
      bool handled = false;
      if (!emit_ps1(c, handled)) return false;
      if (handled) return err.empty();
    }
    switch (op) {
    case DxsoOpcode::Dcl: case DxsoOpcode::Def: case DxsoOpcode::DefI: case DxsoOpcode::DefB:
    case DxsoOpcode::TexM3x2Pad: case DxsoOpcode::TexM3x3Pad:
      return true;
    case DxsoOpcode::End: return true;
    case DxsoOpcode::Ret:
      if (inline_depth == 0) { epilogue(); ins(D3D10_SB_OPCODE_RET, {}); }
      return true;
    case DxsoOpcode::Call: case DxsoOpcode::CallNz: {
      if (inline_depth > 8) return fail("call depth");
      auto it = labels.find(c.dst.id.num); /* call l#: the label decodes as dst, callnz's condition as src0 */
      if (it == labels.end()) return fail("call to missing label %u", c.dst.id.num);
      if (op == DxsoOpcode::CallNz) {
        Opnd cond = src_raw(c.src[0]); cond.sel1(swz_comp(cond.sel, 0));
        bool negate = c.src[0].modifier == DxsoRegModifier::Not;
        ins(D3D10_SB_OPCODE_IF, {cond}, negate ? 0 : NZ);
        bool r = emit_list(it->second, inline_depth + 1);
        ins(D3D10_SB_OPCODE_ENDIF, {});
        return r;
      }
      return emit_list(it->second, inline_depth + 1);
    }
    case DxsoOpcode::If: {
      Opnd cond = src_raw(c.src[0]); cond.sel1(swz_comp(cond.sel, 0));
      bool negate = c.src[0].modifier == DxsoRegModifier::Not;
      ins(D3D10_SB_OPCODE_IF, {cond}, negate ? 0 : NZ);
      return true;
    }
    case DxsoOpcode::Ifc: {
      uint32_t t = scratch();
      Opnd a = src(c.src[0]), b = src(c.src[1]);
      a.swz(swz_rep(swz_comp(a.sel, 0))); b.swz(swz_rep(swz_comp(b.sel, 0)));
      emit_compare(c.instruction.specificData.comparison, T(t).mask(1), a, b);
      Opnd cond = T(t); cond.sel1(0);
      ins(D3D10_SB_OPCODE_IF, {cond}, NZ);
      return true;
    }
    case DxsoOpcode::Else: ins(D3D10_SB_OPCODE_ELSE, {}); return true;
    case DxsoOpcode::EndIf: ins(D3D10_SB_OPCODE_ENDIF, {}); return true;
    case DxsoOpcode::Break: ins(D3D10_SB_OPCODE_BREAK, {}); return true;
    case DxsoOpcode::BreakC: {
      uint32_t t = scratch();
      Opnd a = src(c.src[0]), b = src(c.src[1]);
      a.swz(swz_rep(swz_comp(a.sel, 0))); b.swz(swz_rep(swz_comp(b.sel, 0)));
      emit_compare(c.instruction.specificData.comparison, T(t).mask(1), a, b);
      Opnd cond = T(t); cond.sel1(0);
      ins(D3D10_SB_OPCODE_BREAKC, {cond}, NZ);
      return true;
    }
    case DxsoOpcode::BreakP: {
      Opnd cond = src_raw(c.src[0]); cond.sel1(swz_comp(cond.sel, 0));
      ins(D3D10_SB_OPCODE_BREAKC, {cond}, c.src[0].modifier == DxsoRegModifier::Not ? 0 : NZ);
      return true;
    }
    case DxsoOpcode::Loop: case DxsoOpcode::Rep: {
      bool is_loop = op == DxsoOpcode::Loop;
      const DxsoRegister &ireg = is_loop ? c.src[1] : c.src[0];
      if (loop_depth >= 8) return fail("loop depth");
      uint32_t lt = t_loop[loop_depth];
      loops.push_back({is_loop, ireg.id.num});
      Opnd ib = CB(1, ireg.id.num);
      ins(D3D10_SB_OPCODE_MOV, {T(lt).mask(2), Opnd(ib).swz(XXXX)});           /* .y = iteration count */
      if (is_loop) ins(D3D10_SB_OPCODE_MOV, {T(lt).mask(1), Opnd(ib).swz(swz_rep(1))}); /* .x = aL start */
      loop_depth++;
      ins(D3D10_SB_OPCODE_LOOP, {});
      Opnd cnt = T(lt); cnt.sel1(1);
      ins(D3D10_SB_OPCODE_BREAKC, {cnt}, 0 /* break when zero */);
      return true;
    }
    case DxsoOpcode::EndLoop: case DxsoOpcode::EndRep: {
      if (loops.empty() || !loop_depth) return fail("endloop without loop");
      LoopInfo l = loops.back(); loops.pop_back();
      uint32_t lt = t_loop[loop_depth - 1];
      if (l.is_loop) ins(D3D10_SB_OPCODE_IADD, {T(lt).mask(1), Opnd(T(lt)).swz(XXXX), Opnd(CB(1, l.ireg)).swz(swz_rep(2))});
      ins(D3D10_SB_OPCODE_IADD, {T(lt).mask(2), Opnd(T(lt)).swz(swz_rep(1)), IMMU1(~0u)});
      ins(D3D10_SB_OPCODE_ENDLOOP, {});
      loop_depth--;
      return true;
    }
    case DxsoOpcode::TexKill: {
      uint32_t t = scratch();
      DxsoRegister r = c.dst; r.swizzle = IdentitySwizzle; r.modifier = DxsoRegModifier::None;
      Opnd v = (ps1 && !ps14) ? V(slot_of(DxsoUsage::Texcoord, c.dst.id.num) + 1) : src_raw(r);
      uint32_t m = prog.majorVersion() >= 2 ? mask_of(c.dst) : 7;
      if (!m) m = 15;
      ins(D3D10_SB_OPCODE_LT, {T(t).mask(m), v, IMMF1(0.0f)});
      uint32_t first = __builtin_ctz(m);
      for (uint32_t i = first + 1; i < 4; i++)
        if (m >> i & 1) ins(D3D10_SB_OPCODE_OR, {T(t).mask(1u << first), Opnd(T(t)).swz(swz_rep(first)), Opnd(T(t)).swz(swz_rep(i))});
      Opnd cond = T(t); cond.sel1(first);
      ins(D3D10_SB_OPCODE_DISCARD, {cond}, NZ);
      return true;
    }
    default: break;
    }

    /* arithmetic with a destination */
    Opnd d = dst_of(c.dst, ok);
    if (!ok) return false;
    bool sat = c.dst.saturate && !c.dst.shift; /* D3D9: shift first, then saturate */
    bool predicated = c.instruction.predicated;
    Opnd final_dst = d;
    uint32_t pred_tmp = ~0u;
    if (predicated || c.dst.shift) { pred_tmp = scratch(); d = T(pred_tmp).mask(final_dst.sel); }
    uint32_t m = final_dst.sel;

    auto S = [&](int i) { return src(c.src[i]); };
    switch (op) {
    case DxsoOpcode::Mov: case DxsoOpcode::Mova:
      if (vs && c.dst.id.type == DxsoRegisterType::Addr) {
        uint32_t t = scratch();
        ins(op == DxsoOpcode::Mova || prog.majorVersion() >= 2 ? D3D10_SB_OPCODE_ROUND_NE : D3D10_SB_OPCODE_ROUND_NI,
            {T(t).mask(m), S(0)});
        ins(D3D10_SB_OPCODE_FTOI, {d, T(t)});
      } else ins(D3D10_SB_OPCODE_MOV, {d, S(0)}, 0, sat);
      break;
    case DxsoOpcode::Add: ins(D3D10_SB_OPCODE_ADD, {d, S(0), S(1)}, 0, sat); break;
    case DxsoOpcode::Sub: ins(D3D10_SB_OPCODE_ADD, {d, S(0), S(1).neg()}, 0, sat); break;
    case DxsoOpcode::Mul: ins(D3D10_SB_OPCODE_MUL, {d, S(0), S(1)}, 0, sat); break;
    case DxsoOpcode::Mad: ins(D3D10_SB_OPCODE_MAD, {d, S(0), S(1), S(2)}, 0, sat); break;
    case DxsoOpcode::Min: ins(D3D10_SB_OPCODE_MIN, {d, S(0), S(1)}, 0, sat); break;
    case DxsoOpcode::Max: ins(D3D10_SB_OPCODE_MAX, {d, S(0), S(1)}, 0, sat); break;
    case DxsoOpcode::Frc: ins(D3D10_SB_OPCODE_FRC, {d, S(0)}, 0, sat); break;
    case DxsoOpcode::Abs: ins(D3D10_SB_OPCODE_MOV, {d, S(0).abs()}, 0, sat); break;
    case DxsoOpcode::Rcp: ins(D3D10_SB_OPCODE_DIV, {d, IMMF1(1.0f), S(0)}, 0, sat); break;
    case DxsoOpcode::Rsq: ins(D3D10_SB_OPCODE_RSQ, {d, S(0).abs()}, 0, sat); break;
    case DxsoOpcode::Exp: case DxsoOpcode::ExpP: ins(D3D10_SB_OPCODE_EXP, {d, S(0)}, 0, sat); break;
    case DxsoOpcode::Log: case DxsoOpcode::LogP: ins(D3D10_SB_OPCODE_LOG, {d, S(0).abs()}, 0, sat); break;
    case DxsoOpcode::Dp3: ins(D3D10_SB_OPCODE_DP3, {d, S(0), S(1)}, 0, sat); break;
    case DxsoOpcode::Dp4: ins(D3D10_SB_OPCODE_DP4, {d, S(0), S(1)}, 0, sat); break;
    case DxsoOpcode::Slt: case DxsoOpcode::Sge: {
      uint32_t t = scratch();
      ins(op == DxsoOpcode::Slt ? D3D10_SB_OPCODE_LT : D3D10_SB_OPCODE_GE, {T(t).mask(m), S(0), S(1)});
      ins(D3D10_SB_OPCODE_AND, {d, T(t), IMMU1(fbits(1.0f))});
      break;
    }
    case DxsoOpcode::Lrp: {
      uint32_t t = scratch(); Opnd c2 = S(2);
      ins(D3D10_SB_OPCODE_ADD, {T(t).mask(m), S(1), Opnd(c2).neg()});
      ins(D3D10_SB_OPCODE_MAD, {d, S(0), T(t), c2}, 0, sat);
      break;
    }
    case DxsoOpcode::Pow: {
      uint32_t t = scratch(); Opnd a = S(0), b = S(1);
      a.swz(swz_rep(swz_comp(a.sel, 0))); b.swz(swz_rep(swz_comp(b.sel, 0)));
      ins(D3D10_SB_OPCODE_LOG, {T(t).mask(1), a.abs()});
      ins(D3D10_SB_OPCODE_MUL, {T(t).mask(1), Opnd(T(t)).swz(XXXX), b});
      ins(D3D10_SB_OPCODE_EXP, {d, Opnd(T(t)).swz(XXXX)}, 0, sat);
      break;
    }
    case DxsoOpcode::Nrm: {
      uint32_t t = scratch(); Opnd a = S(0);
      ins(D3D10_SB_OPCODE_DP3, {T(t).mask(1), a, a});
      ins(D3D10_SB_OPCODE_RSQ, {T(t).mask(1), Opnd(T(t)).swz(XXXX)});
      ins(D3D10_SB_OPCODE_MUL, {d, a, Opnd(T(t)).swz(XXXX)}, 0, sat);
      break;
    }
    case DxsoOpcode::Crs: {
      uint32_t t = scratch(); Opnd a = S(0), b = S(1);
      Opnd a1 = a, b1 = b, a2 = a, b2 = b;
      a1.swz(swz_compose(a.sel, SWZ(2, 0, 1, 3))); b1.swz(swz_compose(b.sel, SWZ(1, 2, 0, 3)));
      a2.swz(swz_compose(a.sel, SWZ(1, 2, 0, 3))); b2.swz(swz_compose(b.sel, SWZ(2, 0, 1, 3)));
      ins(D3D10_SB_OPCODE_MUL, {T(t).mask(7), a1, b1});
      Opnd dd = d; dd.sel &= 7;
      ins(D3D10_SB_OPCODE_MAD, {dd, a2, b2, Opnd(T(t)).neg()}, 0, sat);
      break;
    }
    case DxsoOpcode::Sgn: {
      uint32_t t1 = scratch(), t2 = scratch(); Opnd a = S(0);
      ins(D3D10_SB_OPCODE_LT, {T(t1).mask(m), IMMF1(0.0f), a});
      ins(D3D10_SB_OPCODE_LT, {T(t2).mask(m), a, IMMF1(0.0f)});
      ins(D3D10_SB_OPCODE_IADD, {T(t1).mask(m), T(t2), Opnd(T(t1)).neg()});
      ins(D3D10_SB_OPCODE_ITOF, {d, T(t1)}, 0, sat);
      break;
    }
    case DxsoOpcode::SinCos: {
      uint32_t t = scratch(); Opnd a = S(0); a.swz(swz_rep(swz_comp(a.sel, 0))); /* replicate swizzle required */
      ins(D3D10_SB_OPCODE_SINCOS, {T(t).mask(2), T(t).mask(1), a});
      Opnd dd = d; dd.sel &= 3;
      if (dd.sel) ins(D3D10_SB_OPCODE_MOV, {dd, T(t)}, 0, sat);
      break;
    }
    case DxsoOpcode::Dp2Add: {
      uint32_t t = scratch(); Opnd c2 = S(2); c2.swz(swz_rep(swz_comp(c2.sel, 0)));
      ins(D3D10_SB_OPCODE_DP2, {T(t).mask(1), S(0), S(1)});
      ins(D3D10_SB_OPCODE_ADD, {d, Opnd(T(t)).swz(XXXX), c2}, 0, sat);
      break;
    }
    case DxsoOpcode::Cmp: {
      uint32_t t = scratch();
      ins(D3D10_SB_OPCODE_GE, {T(t).mask(m), S(0), IMMF1(0.0f)});
      ins(D3D10_SB_OPCODE_MOVC, {d, T(t), S(1), S(2)}, 0, sat);
      break;
    }
    case DxsoOpcode::Cnd: {
      uint32_t t = scratch();
      ins(D3D10_SB_OPCODE_LT, {T(t).mask(m), IMMF1(0.5f), S(0)});
      ins(D3D10_SB_OPCODE_MOVC, {d, T(t), S(1), S(2)}, 0, sat);
      break;
    }
    case DxsoOpcode::M4x4: case DxsoOpcode::M4x3: case DxsoOpcode::M3x4: case DxsoOpcode::M3x3: case DxsoOpcode::M3x2: {
      uint32_t rows = (op == DxsoOpcode::M4x4 || op == DxsoOpcode::M3x4) ? 4 : (op == DxsoOpcode::M3x2) ? 2 : 3;
      bool dp4 = op == DxsoOpcode::M4x4 || op == DxsoOpcode::M4x3;
      Opnd a = S(0);
      uint32_t r = scratch(); /* destination may alias the source: build the result first */
      for (uint32_t i = 0; i < rows; i++) {
        if (!(m >> i & 1)) continue;
        DxsoRegister row = c.src[1]; row.id.num += i;
        ins(dp4 ? D3D10_SB_OPCODE_DP4 : D3D10_SB_OPCODE_DP3, {T(r).mask(1u << i), a, src(row)});
      }
      ins(D3D10_SB_OPCODE_MOV, {d, T(r)}, 0, sat);
      break;
    }
    case DxsoOpcode::Dst: {
      Opnd a = S(0), b = S(1); uint32_t r = scratch();
      ins(D3D10_SB_OPCODE_MOV, {T(r).mask(1), IMMF1(1.0f)});
      ins(D3D10_SB_OPCODE_MUL, {T(r).mask(2), a, b});
      ins(D3D10_SB_OPCODE_MOV, {T(r).mask(4), a});
      ins(D3D10_SB_OPCODE_MOV, {T(r).mask(8), b});
      ins(D3D10_SB_OPCODE_MOV, {d, T(r)}, 0, sat);
      break;
    }
    case DxsoOpcode::Lit: {
      uint32_t t = scratch(), k = scratch(); Opnd a = S(0);
      auto comp = [&](uint32_t i) { Opnd x = a; x.swz(swz_rep(swz_comp(a.sel, i))); return x; };
      ins(D3D10_SB_OPCODE_MAX, {T(t).mask(3), Opnd(comp(0)).swz(swz_compose(a.sel, SWZ(0, 1, 0, 0))), IMMF1(0.0f)});
      ins(D3D10_SB_OPCODE_MAX, {T(t).mask(4), comp(3), IMMF1(-127.9961f)});
      ins(D3D10_SB_OPCODE_MIN, {T(t).mask(4), Opnd(T(t)).swz(swz_rep(2)), IMMF1(127.9961f)});
      ins(D3D10_SB_OPCODE_LOG, {T(k).mask(1), Opnd(T(t)).swz(swz_rep(1))});
      ins(D3D10_SB_OPCODE_MUL, {T(k).mask(1), Opnd(T(k)).swz(XXXX), Opnd(T(t)).swz(swz_rep(2))});
      ins(D3D10_SB_OPCODE_EXP, {T(k).mask(1), Opnd(T(k)).swz(XXXX)});
      ins(D3D10_SB_OPCODE_LT, {T(k).mask(2), IMMF1(0.0f), comp(0)});
      ins(D3D10_SB_OPCODE_AND, {T(t).mask(4), Opnd(T(k)).swz(XXXX), Opnd(T(k)).swz(swz_rep(1))});
      ins(D3D10_SB_OPCODE_MOV, {T(t).mask(9), IMMF1(1.0f)});
      ins(D3D10_SB_OPCODE_MOV, {d, T(t)}, 0, sat);
      break;
    }
    case DxsoOpcode::Tex: case DxsoOpcode::TexLdl: case DxsoOpcode::TexLdd:
      emit_texld(c, d, sat);
      break;
    case DxsoOpcode::DsX: ins(D3D10_SB_OPCODE_DERIV_RTX, {d, S(0)}, 0, sat); break;
    case DxsoOpcode::DsY: ins(D3D10_SB_OPCODE_DERIV_RTY, {d, S(0)}, 0, sat); break;
    case DxsoOpcode::SetP: {
      emit_compare(c.instruction.specificData.comparison, d, S(0), S(1));
      break;
    }
    default:
      return fail("unsupported opcode %u", (unsigned)op);
    }

    if (c.dst.shift) {
      float f = c.dst.shift > 0 ? (float)(1 << c.dst.shift) : 1.0f / (float)(1 << -c.dst.shift);
      Opnd tmp = T(pred_tmp);
      ins(D3D10_SB_OPCODE_MUL, {T(pred_tmp).mask(m), tmp, IMMF1(f)}, 0, c.dst.saturate);
    }
    if (predicated) {
      Opnd p = src_raw(c.pred);
      bool negate = c.pred.modifier == DxsoRegModifier::Not;
      Opnd cur = final_dst; cur.swz(XYZW);
      if (negate) ins(D3D10_SB_OPCODE_MOVC, {final_dst, p, cur, T(pred_tmp)});
      else ins(D3D10_SB_OPCODE_MOVC, {final_dst, p, T(pred_tmp), cur});
    } else if (c.dst.shift) {
      ins(D3D10_SB_OPCODE_MOV, {final_dst, T(pred_tmp)});
    }
    return err.empty();
  }

  /* ---------- pass 2 ---------- */
  bool build(std::vector<uint8_t> &out) {
    for (auto &x : t_slot) x = ~0u;
    for (auto &x : t_oc) x = ~0u;
    for (auto &x : t_in) x = ~0u;
    for (auto &x : t_tex) x = ~0u;
    if (!collect(insts)) return false;
    for (auto &l : labels) if (!collect(l.second)) return false;
    info.stage = vs ? DXSO_STAGE_VERTEX : DXSO_STAGE_PIXEL;
    info.major = prog.majorVersion(); info.minor = prog.minorVersion();
    for (uint32_t s = 0; s < 16; s++) if (info.sampler_type[s]) info.sampler_mask |= 1u << s;

    /* temp layout */
    next_temp = max_user_temp;
    if (vs) {
      t_pos = next_temp++;
      uint32_t outs = info.slots_written | var.fill_slots;
      for (uint32_t s = 0; s < DXSO_MAX_SLOTS; s++) if (outs >> s & 1) t_slot[s] = next_temp++;
      t_psize = next_temp++;
      t_psz = next_temp++;
      t_addr = next_temp++;
      for (uint32_t i = 0; i < 16; i++) if (var.int_inputs >> i & 1) t_in[i] = next_temp++;
    } else {
      if (prog.majorVersion() < 3 && !info.rt_mask) info.rt_mask = 1;
      if (ps1) {
        /* r0 is the colour output; registers r0-r5 exist even when a shader only reads them */
        max_user_temp = std::max<uint32_t>(max_user_temp, ps14 ? 6 : 2);
        next_temp = max_user_temp;
        for (uint32_t i = 0; i < 8; i++) t_tex[i] = ps14 ? ~0u : next_temp++;
        t_m3 = next_temp++; t_m3eye = next_temp++;
      }
      if ((var.flags >> DXSO_PS_FOG_SHIFT) & 3) info.slots_read |= 1u << DXSO_SLOT_FOG;
      for (uint32_t i = 0; i < 4; i++) if (info.rt_mask >> i & 1) t_oc[i] = next_temp++;
      if (info.writes_depth) t_depth = next_temp++;
      if (uses_vpos) t_vpos = next_temp++;
      if (uses_vface) t_vface = next_temp++;
    }
    t_pred = next_temp++;
    for (uint32_t i = 0; i < 8; i++) t_loop[i] = next_temp + i;
    next_temp += std::max<uint32_t>(loop_depth_max, 1);
    t_scratch = next_temp; next_temp += kScratch;

    /* declarations */
    dcl(D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS, {}, D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED);
    { Opnd cb = CB(0, 256); cb.swz(XYZW);
      dcl(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER, {cb}, ENCODE_D3D10_SB_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(D3D10_SB_CONSTANT_BUFFER_DYNAMIC_INDEXED)); }
    if (uses_cb1) { Opnd cb = CB(1, 16); cb.swz(XYZW); dcl(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER, {cb}, 0); }
    if (uses_cb2) { Opnd cb = CB(2, 16); cb.swz(XYZW); dcl(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER, {cb}, 0); }
    uint32_t clip_mask = vs ? (var.vs_flags >> DXSO_VS_CLIP_SHIFT) & 0x3F : 0;
    if (!vs && ((var.flags & 0xF) || ((var.flags >> DXSO_PS_FOG_SHIFT) & 3) || uses_bump)) {
      Opnd cb = CB(3, 20); cb.swz(XYZW); dcl(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER, {cb}, 0);
    }
    /* (DX9FULL) point expansion (VS flag bit 16): cb3[6] = (size, min, max, 0), cb3[7] = (1 / viewport width, 1 / height, 0, 0) */
    bool points = vs && (var.vs_flags >> DXSO_VS_POINT_SHIFT & 1);
    if (clip_mask || points) { Opnd cb = CB(3, points ? 8 : 6); cb.swz(XYZW); dcl(D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER, {cb}, 0); }
    for (uint32_t s = 0; s < 16; s++) {
      if (!(info.sampler_mask >> s & 1)) continue;
      dcl(D3D10_SB_OPCODE_DCL_SAMPLER, {SMP(s)}, ENCODE_D3D10_SB_SAMPLER_MODE(
          ((var.depth_samplers >> s & 1) && !(var.depth_samplers >> (16 + s) & 1)) ? D3D10_SB_SAMPLER_MODE_COMPARISON
                                                                                    : D3D10_SB_SAMPLER_MODE_DEFAULT));
      uint32_t dim = info.sampler_type[s] == 3 ? D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE
                   : info.sampler_type[s] == 4 ? D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D : D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D;
      Opnd r = RES(s); r.ncomp = 0;
      dcl(D3D10_SB_OPCODE_DCL_RESOURCE, {r}, ENCODE_D3D10_SB_RESOURCE_DIMENSION(dim), {0x5555u /* float x4 */});
    }
    std::vector<SigElem> isgn, osgn;
    if (vs) {
      for (uint32_t i = 0; i < info.input_count; i++) {
        auto &e = info.inputs[i];
        isgn.push_back({usage_name((DxsoUsage)e.usage), e.usage_index, 0, e.reg, 0xF, 0xF});
        Opnd v = V(e.reg); v.mask(15);
        dcl(D3D10_SB_OPCODE_DCL_INPUT, {v}, 0);
      }
      if (points) {   /* (DX9FULL) the quad corner: vertex_id 0..3 of the instanced strip */
        SigElem vid{"SV_VertexID", 0, 6 /* VERTEX_ID */, 16, 0x1, 0x1}; vid.ctype = 1; isgn.push_back(vid);
        Opnd v = V(16); v.mask(1);
        dcl(D3D10_SB_OPCODE_DCL_INPUT_SGV, {v}, 0, {D3D10_SB_NAME_VERTEX_ID});
      }
      osgn.push_back({"SV_Position", 0, 1 /* POSITION */, 0, 0xF, 0x0});
      { Opnd o = O(0); o.mask(15); dcl(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, {o}, 0, {D3D10_SB_NAME_POSITION}); }
      for (uint32_t s = 0; s < DXSO_MAX_SLOTS; s++) {
        if (t_slot[s] == ~0u) continue;
        osgn.push_back({"HBSLOT", s, 0, s + 1, 0xF, 0x0});
        Opnd o = O(s + 1); o.mask(15); dcl(D3D10_SB_OPCODE_DCL_OUTPUT, {o}, 0);
      }
      if (clip_mask) {
        osgn.push_back({"SV_ClipDistance", 0, 2 /* CLIP_DISTANCE */, 17, 0xF, 0x0});
        osgn.push_back({"SV_ClipDistance", 1, 2, 18, 0x3, 0x0});
        { Opnd o = O(17); o.mask(15); dcl(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, {o}, 0, {D3D10_SB_NAME_CLIP_DISTANCE}); }
        { Opnd o = O(18); o.mask(3); dcl(D3D10_SB_OPCODE_DCL_OUTPUT_SIV, {o}, 0, {D3D10_SB_NAME_CLIP_DISTANCE}); }
      }
    } else {
      if (uses_vpos) {
        isgn.push_back({"SV_Position", 0, 1, 0, 0xF, 0x3});
        Opnd v = V(0); v.mask(15);
        dcl(D3D10_SB_OPCODE_DCL_INPUT_PS_SIV, {v}, ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE),
            {D3D10_SB_NAME_POSITION});
      }
      /* (DX9FULL 13.11.2026) D3DRS_SHADEMODE = D3DSHADE_FLAT (PS variant flag bit 10): the two colour interpolants are constant
         across the primitive (DXVK: Flat decoration on COLOR inputs); texture coordinates stay interpolated */
      bool flat = !vs && (var.flags >> 10 & 1);
      uint32_t c0 = flat ? slot_of(DxsoUsage::Color, 0) : ~0u, c1 = flat ? slot_of(DxsoUsage::Color, 1) : ~0u;
      for (uint32_t s = 0; s < DXSO_MAX_SLOTS; s++) {
        if (!(info.slots_read >> s & 1)) continue;
        isgn.push_back({"HBSLOT", s, 0, s + 1, 0xF, 0xF});
        Opnd v = V(s + 1); v.mask(15);
        dcl(D3D10_SB_OPCODE_DCL_INPUT_PS, {v}, ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(
            (s == c0 || s == c1) ? D3D10_SB_INTERPOLATION_CONSTANT : D3D10_SB_INTERPOLATION_LINEAR));
      }
      if (uses_vface) {
        isgn.push_back({"SV_IsFrontFace", 0, 9, DXSO_MAX_SLOTS + 1, 0x1, 0x1});
        Opnd v = V(DXSO_MAX_SLOTS + 1); v.mask(1);
        dcl(D3D10_SB_OPCODE_DCL_INPUT_PS_SGV, {v}, ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(D3D10_SB_INTERPOLATION_CONSTANT),
            {D3D10_SB_NAME_IS_FRONT_FACE});
      }
      for (uint32_t i = 0; i < 4; i++) {
        if (!(info.rt_mask >> i & 1)) continue;
        osgn.push_back({"SV_Target", i, 0, i, 0xF, 0x0});
        Opnd o = O(i); o.mask(15); dcl(D3D10_SB_OPCODE_DCL_OUTPUT, {o}, 0);
      }
      if (info.writes_depth) {
        osgn.push_back({"SV_Depth", 0, 0, 0xFFFFFFFFu, 0x1, 0x0});
        dcl(D3D10_SB_OPCODE_DCL_OUTPUT, {ODEPTH()}, 0);
      }
    }
    dcl(D3D10_SB_OPCODE_DCL_TEMPS, {}, 0, {next_temp});

    /* prologue */
    /* (session 6) every r# starts at (0,0,0,0) for all shader models, as DXVK (dxso_compiler.cpp: temps are created with a
       zero initializer); a shader reading a component it never wrote must not read register garbage (07.11: the world tape
       replay differed run to run in isolated pixels) */
    if (!ps1) for (uint32_t i = 0; i < max_user_temp; i++) ins(D3D10_SB_OPCODE_MOV, {T(i).mask(15), IMMF1(0.0f)});
    if (vs) {
      ins(D3D10_SB_OPCODE_MOV, {T(t_pos).mask(15), IMMF(0, 0, 0, 0)});
      for (uint32_t s = 0; s < DXSO_MAX_SLOTS; s++)
        if (t_slot[s] != ~0u) ins(D3D10_SB_OPCODE_MOV, {T(t_slot[s]).mask(15), s == 8 ? IMMF1(1.0f) : IMMF1(0.0f)});
      ins(D3D10_SB_OPCODE_MOV, {T(t_addr).mask(15), IMMU1(0)});
      for (uint32_t i = 0; i < 16; i++)
        if (t_in[i] != ~0u) ins(D3D10_SB_OPCODE_ITOF, {T(t_in[i]).mask(15), V(i)});
    } else {
      for (uint32_t i = 0; i < 4; i++) if (t_oc[i] != ~0u) ins(D3D10_SB_OPCODE_MOV, {T(t_oc[i]).mask(15), IMMF1(0.0f)});
      if (ps1) {
        for (uint32_t i = 0; i < max_user_temp; i++) ins(D3D10_SB_OPCODE_MOV, {T(i).mask(15), IMMF1(0.0f)});
        for (uint32_t i = 0; i < 8; i++) if (t_tex[i] != ~0u) ins(D3D10_SB_OPCODE_MOV, {T(t_tex[i]).mask(15), IMMF1(0.0f)});
      }
      if (uses_vpos) {
        ins(D3D10_SB_OPCODE_ADD, {T(t_vpos).mask(3), V(0), IMMF(-0.5f, -0.5f, 0, 0)});
        ins(D3D10_SB_OPCODE_MOV, {T(t_vpos).mask(12), IMMF(0, 0, 0, 1)});
      }
      if (uses_vface) {
        Opnd f = V(DXSO_MAX_SLOTS + 1); f.sel1(0);
        ins(D3D10_SB_OPCODE_MOVC, {T(t_vface).mask(15), f, IMMF1(1.0f), IMMF1(-1.0f)});
      }
    }
    ins(D3D10_SB_OPCODE_MOV, {T(t_pred).mask(15), IMMU1(0)});

    if (!emit_list(insts, 0)) return false;
    epilogue();
    ins(D3D10_SB_OPCODE_RET, {});
    if (!loops.empty()) return fail("unterminated loop");

    /* container */
    std::vector<uint32_t> shex;
    shex.push_back(ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(vs ? D3D10_SB_VERTEX_SHADER : D3D10_SB_PIXEL_SHADER, 5, 0));
    shex.push_back((uint32_t)(2 + decl.size() + code.size()));
    shex.insert(shex.end(), decl.begin(), decl.end());
    shex.insert(shex.end(), code.begin(), code.end());
    std::vector<uint8_t> shex_bytes((uint8_t *)shex.data(), (uint8_t *)shex.data() + shex.size() * 4);
    out = build_container({{FOURCC('I', 'S', 'G', 'N'), build_signature(isgn)},
                           {FOURCC('O', 'S', 'G', 'N'), build_signature(osgn)},
                           {FOURCC('S', 'H', 'E', 'X'), shex_bytes}});
    return true;
  }
};

} // namespace

struct dxso_result {
  std::vector<uint8_t> dxbc;
  dxso_info info;
  std::vector<dxso_def> defs;
};

extern "C" int dxso_translate(const uint32_t *tokens, size_t max_dwords, const struct dxso_variant *variant, dxso_result **out,
                              char *error, size_t error_size) {
  if (out) *out = nullptr;
  if (!tokens || !out || max_dwords < 2) { if (error && error_size) snprintf(error, error_size, "bad arguments"); return 1; }
  Translator t;
  t.tokens = tokens; t.max_dwords = max_dwords;
  if (variant) t.var = *variant;
  auto r = new dxso_result;
  bool ok = t.decode() && t.build(r->dxbc);
  if (!ok || !t.err.empty()) {
    if (error && error_size) snprintf(error, error_size, "%s", t.err.empty() ? "translation failed" : t.err.c_str());
    delete r; return 1;
  }
  r->defs = std::move(t.defs);
  r->info = t.info;
  r->info.def_count = (uint32_t)r->defs.size();
  r->info.defs = r->defs.data();
  *out = r;
  return 0;
}
extern "C" const void *dxso_result_dxbc(const dxso_result *r, size_t *size) { if (size) *size = r->dxbc.size(); return r->dxbc.data(); }
extern "C" const struct dxso_info *dxso_result_info(const dxso_result *r) { return &r->info; }
extern "C" void dxso_result_free(dxso_result *r) { delete r; }

extern "C" size_t dxso_token_count(const uint32_t *tokens, size_t max_dwords) {
  if (!tokens || max_dwords < 2) return 0;
  uint32_t version = tokens[0], type = version >> 16;
  if (type != 0xFFFE && type != 0xFFFF) return 0;
  DxsoProgramInfo prog(type == 0xFFFE ? DxsoProgramTypes::VertexShader : DxsoProgramTypes::PixelShader, version & 0xFF, (version >> 8) & 0xFF);
  DxsoDecodeContext ctx(prog);
  DxsoCodeIter it(tokens + 1);
  for (;;) {
    if ((size_t)(it.ptrAt(0) - tokens) >= max_dwords) return 0;
    if (!ctx.decodeInstruction(it)) break;
    if (ctx.failed()) return 0;
  }
  return (size_t)(it.ptrAt(0) - tokens);
}

extern "C" uint32_t dxso_slot_of(uint32_t usage, uint32_t usage_index) { return slot_of((DxsoUsage)usage, usage_index); }
