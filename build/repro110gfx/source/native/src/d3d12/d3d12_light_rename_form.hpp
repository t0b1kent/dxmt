// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// LIGHTRN 02.11.2026: the "full-overwrite form" of a compute shader, decided from its DXIL at pipeline creation (no hash).
// Pure C++17 (no Windows / Metal dependency) so it is unit-tested natively against real containers.
//
// What it proves (per UAV range of the shader): a typed store whose coordinates are exactly (SV_DispatchThreadID.x, .y, .z)
// with all four components written, reached on a straight path from the entry block (every block on the path has one
// predecessor), together with the conditions of the branches on that path. Those conditions are kept as expression trees over
// SV_DispatchThreadID, constant-buffer words and literals; at dispatch time they are evaluated with the CB words of that
// dispatch and the thread ID as an interval (the box of the bound view): a condition that holds for EVERY thread of the box
// means every texel of the view is written by this dispatch. The shader may not access a UAV in any other way (load, atomic,
// counter, buffer ops: the whole form is refused), may not index resources dynamically or through the heap (SM 6.6).
// Anything the evaluator does not model (floats, PHIs, memory loads, other intrinsics) is unknown => not proven => fallback.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dxmt::light_rename::form {

// ---- LLVM bitstream (the subset DXIL 1.x uses) ------------------------------------------------------------------------------
struct Bits {
  const uint8_t *data = nullptr;
  size_t end = 0, pos = 0;   // in bits
  bool bad = false;
  uint64_t Fixed(unsigned n) {
    if (!n) return 0;
    if (n > 64 || pos + n > end) { bad = true; pos = end; return 0; }
    uint64_t v = 0;
    for (unsigned i = 0; i < n;) {
      const size_t byte = pos >> 3; const unsigned bit = unsigned(pos & 7);
      const unsigned take = std::min(8u - bit, n - i);
      v |= uint64_t((data[byte] >> bit) & ((1u << take) - 1u)) << i;
      i += take; pos += take;
    }
    return v;
  }
  uint64_t VBR(unsigned n) {
    if (n < 2 || n > 32) { bad = true; return 0; }
    uint64_t v = 0; unsigned shift = 0; const uint64_t hi = uint64_t(1) << (n - 1);
    for (;;) {
      const uint64_t piece = Fixed(n);
      if (bad) return 0;
      if (shift < 64) v |= (piece & (hi - 1)) << shift;
      if (!(piece & hi)) return v;
      shift += n - 1;
      if (shift > 70) { bad = true; return 0; }
    }
  }
  void Align32() { pos = (pos + 31) & ~size_t(31); if (pos > end) { bad = true; pos = end; } }
};

struct AbbrevOp { uint8_t enc = 0; uint64_t val = 0; };   // enc: 0 literal, 1 fixed, 2 vbr, 3 array, 4 char6, 5 blob
using Abbrev = std::vector<AbbrevOp>;

struct Record { unsigned code = 0; std::vector<uint64_t> ops; };

// ---- what the analysis keeps ----------------------------------------------------------------------------------------------
enum Op : uint8_t {
  OpUnknown, OpConst, OpTid, OpCb, OpAdd, OpSub, OpMul, OpShl, OpLshr, OpAshr, OpAnd, OpOr, OpXor, OpUdiv, OpUrem,
  OpIcmp, OpSelect, OpZext, OpSext, OpTrunc,
};
struct Node {
  Op op = OpUnknown;
  uint8_t width = 32;       // integer width of the value (1 for booleans)
  uint8_t pred = 0;         // OpIcmp: LLVM predicate (32 eq .. 41 sle)
  uint32_t a = 0, b = 0, c = 0;   // operand nodes; OpTid: a = component; OpCb: a = CBV space, b = register, c = 32-bit word index
  uint64_t k = 0;           // OpConst value (zero-extended to width)
};
struct Cond { uint32_t node = 0; bool want = true; };
struct Store { uint32_t uav = 0; std::vector<Cond> path; };   // uav = index into Form::uavs
struct Resource { uint32_t space = 0, lower = 0, count = 1, kind = 0; };
struct Form {
  bool ok = false;
  std::string why;                 // refusal reason (short token) when !ok
  std::vector<Node> nodes;
  std::vector<Resource> uavs, srvs, cbvs;
  std::vector<Store> stores;       // identity stores (coordinates = thread ID, mask 0xF) with straight-path conditions
  std::vector<uint32_t> uav_stores;   // per UAV: number of identity stores found (diagnostics)
  uint32_t store_total = 0;        // every textureStore seen
  // LIGHTRN 03.11 (copy-in): the set of texels a dispatch writes is a function of (grid, bound views, the CB words below) only —
  // every textureStore is one of `stores` and the conditions on their paths reach only thread IDs, CB words and literals through
  // modelled operations (no unknown node). Then two dispatches with the same grid, views and words write the same texels.
  bool pure = false;
  struct CbWord { uint32_t space = 0, reg = 0, word = 0; };
  std::vector<CbWord> cb_words;    // every CB word those conditions read (pure forms only)
};

// ---- DXBC container helpers --------------------------------------------------------------------------------------------------
inline bool FindPart(const uint8_t *c, size_t n, const char cc[4], const uint8_t *&p, uint32_t &size) {
  if (n < 32 || memcmp(c, "DXBC", 4)) return false;
  uint32_t parts = 0; memcpy(&parts, c + 28, 4);
  if (parts > 64 || 32ull + 4ull * parts > n) return false;
  for (uint32_t i = 0; i < parts; ++i) {
    uint32_t o = 0; memcpy(&o, c + 32 + 4 * i, 4);
    if (uint64_t(o) + 8 > n) return false;
    uint32_t s = 0; memcpy(&s, c + o + 4, 4);
    if (uint64_t(o) + 8 + s > n) return false;
    if (!memcmp(c + o, cc, 4)) { p = c + o + 8; size = s; return true; }
  }
  return false;
}

class Analyzer {
public:
  explicit Analyzer(Form &f) : f_(f) {}

  bool Run(const void *container, size_t length) {
    const auto *c = static_cast<const uint8_t *>(container);
    const uint8_t *part = nullptr; uint32_t size = 0;
    if (!c || length < 32 || length > 64u * 1024 * 1024) return Fail("container");
    {
      const uint8_t *sfi = nullptr; uint32_t ss = 0;
      if (FindPart(c, length, "SFI0", sfi, ss) && ss >= 8) {
        uint64_t features = 0; memcpy(&features, sfi, 8);
        if (features & 0x06000000ull) return Fail("heap_indexing");
      }
    }
    if (!FindPart(c, length, "DXIL", part, size)) return Fail("not_dxil");
    if (size < 24) return Fail("dxil_header");
    uint32_t prog_version = 0, magic = 0, bc_offset = 0, bc_size = 0;
    memcpy(&prog_version, part, 4); memcpy(&magic, part + 8, 4); memcpy(&bc_offset, part + 16, 4); memcpy(&bc_size, part + 20, 4);
    if (memcmp(part + 8, "DXIL", 4)) return Fail("dxil_magic");
    if (((prog_version >> 16) & 0xffff) != 5) return Fail("not_cs");   // program kind 5 = compute
    if (uint64_t(8) + bc_offset + bc_size > size || bc_size < 8) return Fail("dxil_size");
    const uint8_t *bc = part + 8 + bc_offset;
    if (bc[0] != 'B' || bc[1] != 'C' || bc[2] != 0xC0 || bc[3] != 0xDE) return Fail("bc_magic");
    in_.data = bc; in_.end = size_t(bc_size) * 8; in_.pos = 32;
    // top level: abbrev width 2
    while (!in_.bad && in_.pos + 32 <= in_.end) {
      const uint64_t id = in_.Fixed(2);
      if (in_.bad) break;
      if (id != 1) return Fail("bc_top");
      const unsigned block = unsigned(in_.VBR(8)); const unsigned width = unsigned(in_.VBR(4));
      in_.Align32(); const uint64_t words = in_.Fixed(32);
      if (block == 8) { if (!Block(8, width, nullptr)) return Fail(why_.empty() ? "bc_module" : why_); break; }
      in_.pos += words * 32;
      if (in_.pos > in_.end) return Fail("bc_eof");
    }
    if (in_.bad) return Fail("bc_bad");
    if (!have_function_) return Fail("no_function");
    return Analyze();
  }

private:
  Form &f_;
  Bits in_;
  std::string why_;
  bool Fail(const std::string &w) { f_.ok = false; if (f_.why.empty()) f_.why = w; return false; }
  bool Err(const char *w) { if (why_.empty()) why_ = w; return false; }
  void Purity();   // Form::pure / cb_words (after a successful analysis)

  // ---- types / values ----
  struct Type { uint8_t kind = 0; uint32_t width = 0; uint32_t ret = 0; std::vector<uint32_t> elems; };
  // kind: 0 other, 1 void, 2 int, 3 float(any fp), 4 pointer, 5 struct, 6 array/vector, 7 function, 8 label, 9 metadata
  std::vector<Type> types_;
  struct Value {
    uint8_t kind = 0;          // 0 unknown, 1 const int, 2 undef/other const, 3 inst result, 4 function, 5 global, 6 arg
    uint32_t type = ~0u;
    uint64_t ival = 0;
    uint32_t inst = ~0u;       // kind 3
    uint32_t fn_type = ~0u;    // kind 4
    bool is_dxop = false;      // kind 4: never used (dx.op is recognised by the opcode argument)
  };
  std::vector<Value> module_values_, values_;
  std::vector<uint32_t> fn_defs_;   // function value ids with bodies, in order
  size_t fn_def_seen_ = 0;
  bool have_function_ = false;
  std::map<unsigned, std::vector<Abbrev>> blockinfo_;
  // metadata (module level)
  struct Md { uint8_t kind = 0; std::vector<uint64_t> ops; uint32_t value = ~0u; uint32_t type = ~0u; };   // 1 string 2 value 3 node
  std::vector<Md> md_;
  std::map<std::string, std::vector<uint64_t>> named_md_;
  std::string pending_name_;

  // instructions of the (first) function body
  struct Inst {
    uint16_t code = 0; uint32_t result = ~0u; uint32_t type = ~0u; uint32_t bb = 0;
    std::vector<uint32_t> v;     // value operands (absolute ids)
    std::vector<uint64_t> imm;   // immediates (opcode, predicate, indices, bb ids)
  };
  std::vector<Inst> insts_;
  uint32_t declared_blocks_ = 0;

  uint32_t TypeOf(uint32_t value) const { return value < values_.size() ? values_[value].type : ~0u; }
  bool IsInt(uint32_t t) const { return t < types_.size() && types_[t].kind == 2; }
  uint32_t IntWidth(uint32_t t) const { return IsInt(t) ? types_[t].width : 0; }

  // ---- bitstream blocks ----
  bool ReadAbbrev(Abbrev &a) {
    const unsigned n = unsigned(in_.VBR(5));
    if (n > 64) return Err("abbrev_ops");
    for (unsigned i = 0; i < n && !in_.bad; ++i) {
      AbbrevOp op;
      if (in_.Fixed(1)) { op.enc = 0; op.val = in_.VBR(8); }
      else {
        op.enc = uint8_t(in_.Fixed(3));
        if (op.enc == 1 || op.enc == 2) { op.val = in_.VBR(5); if (!op.val) op.enc = 0; }   // width 0 = literal 0
        else if (op.enc < 3 || op.enc > 5) return Err("abbrev_enc");
      }
      a.push_back(op);
    }
    return !in_.bad;
  }
  uint64_t Scalar(const AbbrevOp &op) {
    switch (op.enc) {
    case 0: return op.val;
    case 1: return in_.Fixed(unsigned(op.val));
    case 2: return in_.VBR(unsigned(op.val));
    case 4: {
      const unsigned v = unsigned(in_.Fixed(6));
      if (v < 26) return 'a' + v;
      if (v < 52) return 'A' + v - 26;
      if (v < 62) return '0' + v - 52;
      return v == 62 ? '.' : '_';
    }
    default: in_.bad = true; return 0;
    }
  }
  bool ReadRecord(unsigned id, const std::vector<Abbrev> &abbrevs, Record &r) {
    r.ops.clear();
    if (id == 3) {
      r.code = unsigned(in_.VBR(6));
      const uint64_t n = in_.VBR(6);
      if (n > (1u << 20)) return Err("record_len");
      r.ops.reserve(size_t(n));
      for (uint64_t i = 0; i < n && !in_.bad; ++i) r.ops.push_back(in_.VBR(6));
      return !in_.bad;
    }
    if (id - 4 >= abbrevs.size()) return Err("abbrev_id");
    const Abbrev &a = abbrevs[id - 4];
    std::vector<uint64_t> vals;
    for (size_t i = 0; i < a.size() && !in_.bad; ++i) {
      const auto &op = a[i];
      if (op.enc == 3) {
        if (i + 1 >= a.size()) return Err("abbrev_array");
        const uint64_t n = in_.VBR(6);
        if (n > (1u << 20)) return Err("array_len");
        const auto &elt = a[++i];
        for (uint64_t k = 0; k < n && !in_.bad; ++k) vals.push_back(Scalar(elt));
      } else if (op.enc == 5) {
        const uint64_t n = in_.VBR(6);
        in_.Align32();
        if (in_.pos + n * 8 > in_.end) return Err("blob");
        for (uint64_t k = 0; k < n; ++k) vals.push_back(in_.Fixed(8));
        in_.Align32();
      } else vals.push_back(Scalar(op));
    }
    if (vals.empty() || in_.bad) return Err("record_empty");
    r.code = unsigned(vals[0]);
    r.ops.assign(vals.begin() + 1, vals.end());
    return true;
  }
  bool BlockInfo(unsigned width) {
    std::vector<Abbrev> none;
    int cur = -1;
    for (;;) {
      const uint64_t id = in_.Fixed(width);
      if (in_.bad) return Err("blockinfo_eof");
      if (id == 0) { in_.Align32(); return true; }
      if (id == 1) { in_.VBR(8); in_.VBR(4); in_.Align32(); const uint64_t w = in_.Fixed(32); in_.pos += w * 32; continue; }
      if (id == 2) {
        Abbrev a; if (!ReadAbbrev(a)) return false;
        if (cur < 0) return Err("blockinfo_nobid");
        blockinfo_[unsigned(cur)].push_back(std::move(a));
        continue;
      }
      Record r; if (!ReadRecord(unsigned(id), none, r)) return false;
      if (r.code == 1 && !r.ops.empty()) cur = int(r.ops[0]);   // SETBID
    }
  }
  // Generic block walk; `parent` unused (kept for symmetry). Dispatches records by block id.
  bool Block(unsigned block, unsigned width, void *) {
    std::vector<Abbrev> abbrevs;
    if (auto it = blockinfo_.find(block); it != blockinfo_.end()) abbrevs = it->second;
    Record r;
    const bool function = block == 12;
    if (function) {
      if (fn_def_seen_ >= fn_defs_.size()) return Err("fn_body_extra");
      const uint32_t fid = fn_defs_[fn_def_seen_++];
      if (have_function_) { // only the first body is analysed (DXIL compute = one entry)
        return Err("fn_multiple");
      }
      values_ = module_values_;
      const uint32_t fty = module_values_[fid].fn_type;
      if (fty >= types_.size() || types_[fty].kind != 7) return Err("fn_type");
      for (uint32_t p : types_[fty].elems) { Value v; v.kind = 6; v.type = p; values_.push_back(v); }
      cur_bb_ = 0; function_ctx_ = true;
    }
    for (;;) {
      const uint64_t id = in_.Fixed(width);
      if (in_.bad) return Err("block_eof");
      if (id == 0) { in_.Align32(); break; }
      if (id == 1) {
        const unsigned sub = unsigned(in_.VBR(8)); const unsigned w = unsigned(in_.VBR(4));
        in_.Align32(); const uint64_t words = in_.Fixed(32);
        if (in_.bad) return Err("sub_eof");
        const size_t after = in_.pos + words * 32;
        bool handled = true;
        if (sub == 0) { if (!BlockInfo(w)) return false; }
        else if (sub == 17 || sub == 11 || (sub == 15 && block == 8) || (sub == 12 && block == 8)) { if (!Block(sub, w, nullptr)) return false; }
        else handled = false;
        if (!handled) in_.pos = after;
        if (in_.pos != after) return Err("sub_size");
        continue;
      }
      if (id == 2) { Abbrev a; if (!ReadAbbrev(a)) return false; abbrevs.push_back(std::move(a)); continue; }
      if (!ReadRecord(unsigned(id), abbrevs, r)) return false;
      bool ok = true;
      switch (block) {
      case 8: ok = ModuleRecord(r); break;
      case 17: ok = TypeRecord(r); break;
      case 11: ok = ConstRecord(r, function_ctx_); break;
      case 15: ok = MdRecord(r); break;
      case 12: ok = InstRecord(r); break;
      default: break;
      }
      if (!ok) return false;
    }
    if (function) { have_function_ = true; function_ctx_ = false; }
    return true;
  }
  bool function_ctx_ = false;
  uint32_t cur_type_ = ~0u;   // CONSTANTS SETTYPE
  uint32_t cur_bb_ = 0;

  bool ModuleRecord(const Record &r) {
    switch (r.code) {
    case 1: if (r.ops.empty() || r.ops[0] != 1) return Err("bc_version"); return true;   // relative value ids (LLVM 3.7 DXIL)
    case 7: {   // GLOBALVAR: [ptrty|valty, isconst|explicit<<1, ...]
      Value v; v.kind = 5; v.type = r.ops.empty() ? ~0u : uint32_t(r.ops[0]); module_values_.push_back(v); return true;
    }
    case 8: {   // FUNCTION: [type, cc, isproto, linkage, ...]
      if (r.ops.size() < 3) return Err("fn_record");
      Value v; v.kind = 4; v.fn_type = uint32_t(r.ops[0]); v.type = v.fn_type;
      if (!r.ops[2]) fn_defs_.push_back(uint32_t(module_values_.size()));
      module_values_.push_back(v);
      return true;
    }
    case 9: case 14: { Value v; v.kind = 5; module_values_.push_back(v); return true; }   // ALIAS
    default: return true;
    }
  }
  bool TypeRecord(const Record &r) {
    Type t;
    switch (r.code) {
    case 1: return true;                                        // NUMENTRY
    case 2: t.kind = 1; break;                                  // VOID
    case 3: case 4: case 10: case 13: case 14: case 15: t.kind = 3; break;   // fp kinds
    case 5: t.kind = 8; break;                                  // LABEL
    case 6: t.kind = 0; break;                                  // OPAQUE
    case 7: t.kind = 2; t.width = r.ops.empty() ? 0 : uint32_t(r.ops[0]); break;
    case 8: t.kind = 4; if (!r.ops.empty()) t.elems.push_back(uint32_t(r.ops[0])); break;
    case 11: case 12: t.kind = 6; if (r.ops.size() >= 2) t.elems.push_back(uint32_t(r.ops[1])); break;
    case 16: t.kind = 9; break;
    case 18: case 20: t.kind = 5; for (size_t i = 1; i < r.ops.size(); ++i) t.elems.push_back(uint32_t(r.ops[i])); break;
    case 19: return true;                                       // STRUCT_NAME (applies to the next STRUCT_NAMED)
    case 21: t.kind = 7; if (r.ops.size() >= 2) { t.ret = uint32_t(r.ops[1]); for (size_t i = 2; i < r.ops.size(); ++i) t.elems.push_back(uint32_t(r.ops[i])); } break;
    case 9: t.kind = 7; if (r.ops.size() >= 3) { t.ret = uint32_t(r.ops[2]); for (size_t i = 3; i < r.ops.size(); ++i) t.elems.push_back(uint32_t(r.ops[i])); } break;
    default: t.kind = 0; break;
    }
    types_.push_back(std::move(t));
    return true;
  }
  static int64_t DecodeSigned(uint64_t v) { return (v & 1) ? -int64_t(v >> 1) : int64_t(v >> 1); }
  bool ConstRecord(const Record &r, bool) {
    auto &list = function_ctx_ ? values_ : module_values_;
    if (r.code == 1) { if (r.ops.empty()) return Err("settype"); cur_type_ = uint32_t(r.ops[0]); return true; }
    Value v; v.type = cur_type_; v.kind = 2;
    if (r.code == 2 && IsInt(cur_type_)) { v.kind = 1; v.ival = 0; }            // NULL
    else if (r.code == 4 && !r.ops.empty() && IsInt(cur_type_)) {               // INTEGER
      v.kind = 1;
      const unsigned w = IntWidth(cur_type_);
      uint64_t x = uint64_t(DecodeSigned(r.ops[0]));
      if (w < 64) x &= (uint64_t(1) << w) - 1;
      v.ival = x;
    }
    list.push_back(v);
    return true;
  }
  bool MdRecord(const Record &r) {
    switch (r.code) {
    case 1: case 3: case 2: case 5: case 7: case 8: case 9: case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19:
    case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 33: {
      Md m;
      if (r.code == 1) m.kind = 1;
      else if (r.code == 2) { m.kind = 2; if (r.ops.size() >= 2) { m.type = uint32_t(r.ops[0]); m.value = uint32_t(r.ops[1]); } }
      else if (r.code == 3 || r.code == 5) { m.kind = 3; m.ops = r.ops; }
      md_.push_back(std::move(m));
      return true;
    }
    case 4: pending_name_.assign(r.ops.begin(), r.ops.end()); return true;   // NAME
    case 10: named_md_[pending_name_] = r.ops; pending_name_.clear(); return true;   // NAMED_NODE
    default: return true;   // KIND and others: no metadata id
    }
  }
  // relative operand decoding (LLVM 3.7, bitcode version 1)
  bool Pair(const Record &r, size_t &slot, uint32_t inst_no, uint32_t &out) {
    if (slot >= r.ops.size()) return false;
    const uint32_t rel = uint32_t(r.ops[slot++]);
    out = inst_no - rel;
    if (out < inst_no) return true;
    if (slot >= r.ops.size()) return false;
    ++slot;   // forward reference: explicit type follows
    return true;
  }
  bool Plain(const Record &r, size_t &slot, uint32_t inst_no, uint32_t &out) {
    if (slot >= r.ops.size()) return false;
    out = inst_no - uint32_t(r.ops[slot++]);
    return true;
  }
  void Result(Inst &in, uint32_t type) {
    in.result = uint32_t(values_.size()); in.type = type;
    Value v; v.kind = 3; v.type = type; v.inst = uint32_t(insts_.size());
    values_.push_back(v);
  }
  bool InstRecord(const Record &r) {
    const uint32_t no = uint32_t(values_.size());
    Inst in; in.code = uint16_t(r.code); in.bb = cur_bb_;
    size_t s = 0; uint32_t a = 0, b = 0, c = 0;
    switch (r.code) {
    case 1: declared_blocks_ = r.ops.empty() ? 0 : uint32_t(r.ops[0]); return true;      // DECLAREBLOCKS
    case 33: case 35: return true;                                                         // DEBUG_LOC(_AGAIN)
    case 2:   // BINOP [opval, opval, opcode(, flags)]
      if (!Pair(r, s, no, a) || !Plain(r, s, no, b) || s >= r.ops.size()) return Err("binop");
      in.v = {a, b}; in.imm = {r.ops[s]}; Result(in, TypeOf(a)); break;
    case 3:   // CAST [opval, destty, castopc]
      if (!Pair(r, s, no, a) || s + 1 >= r.ops.size()) return Err("cast");
      in.v = {a}; in.imm = {r.ops[s + 1]}; Result(in, uint32_t(r.ops[s])); break;
    case 28: case 9:   // CMP2 / CMP [opval, opval, pred]
      if (!Pair(r, s, no, a) || !Plain(r, s, no, b) || s >= r.ops.size()) return Err("cmp");
      in.v = {a, b}; in.imm = {r.ops[s]}; Result(in, ~0u); in.type = ~1u; values_.back().type = ~1u; break;   // ~1u = i1
    case 29:  // VSELECT [opval(true), opval(false), pred(pair)]
      if (!Pair(r, s, no, a) || !Plain(r, s, no, b) || !Pair(r, s, no, c)) return Err("vselect");
      in.v = {c, a, b}; Result(in, TypeOf(a)); break;
    case 5:   // SELECT (old) [opval, opval, opval]
      if (!Pair(r, s, no, a) || !Plain(r, s, no, b) || !Plain(r, s, no, c)) return Err("select");
      in.v = {c, a, b}; Result(in, TypeOf(a)); break;
    case 26: {  // EXTRACTVAL [opval, idx...]
      if (!Pair(r, s, no, a)) return Err("extractval");
      in.v = {a};
      uint32_t t = TypeOf(a);
      for (; s < r.ops.size(); ++s) {
        in.imm.push_back(r.ops[s]);
        if (t < types_.size() && (types_[t].kind == 5) && r.ops[s] < types_[t].elems.size()) t = types_[t].elems[size_t(r.ops[s])];
        else if (t < types_.size() && types_[t].kind == 6 && !types_[t].elems.empty()) t = types_[t].elems[0];
        else t = ~0u;
      }
      Result(in, t); break;
    }
    case 34: {  // CALL [paramattrs, cc, (fnty), fnid, args...]
      if (r.ops.size() < 3) return Err("call");
      s = 2;
      uint32_t fnty = ~0u;
      if ((r.ops[1] >> 15) & 1) fnty = uint32_t(r.ops[s++]);
      if (!Pair(r, s, no, a)) return Err("call_fn");
      if (fnty == ~0u && a < values_.size()) fnty = values_[a].fn_type;
      if (fnty >= types_.size() || types_[fnty].kind != 7) return Err("call_type");
      if (types_[fnty].elems.size() != r.ops.size() - s) return Err("call_args");   // varargs not in DXIL
      in.v.push_back(a);
      for (; s < r.ops.size();) { if (!Plain(r, s, no, b)) return Err("call_arg"); in.v.push_back(b); }
      const uint32_t ret = types_[fnty].ret;
      if (ret < types_.size() && types_[ret].kind != 1) Result(in, ret);
      break;
    }
    case 10:  // RET
      if (!r.ops.empty()) { if (!Pair(r, s, no, a)) return Err("ret"); in.v = {a}; }
      insts_.push_back(std::move(in)); ++cur_bb_; return true;
    case 11:  // BR [bb, bb, cond] / [bb]
      if (r.ops.size() == 1) in.imm = {r.ops[0]};
      else if (r.ops.size() == 3) { in.imm = {r.ops[0], r.ops[1]}; s = 2; if (!Plain(r, s, no, a)) return Err("br"); in.v = {a}; }
      else return Err("br_ops");
      insts_.push_back(std::move(in)); ++cur_bb_; return true;
    case 12: case 31: case 15: case 39:   // SWITCH / INDIRECTBR / UNREACHABLE / RESUME: terminators (not modelled)
      in.imm = r.ops; insts_.push_back(std::move(in)); ++cur_bb_; return true;
    case 16: {  // PHI [ty, val0, bb0, ...] (signed relative values)
      if (r.ops.empty()) return Err("phi");
      Result(in, uint32_t(r.ops[0])); break;
    }
    case 44: case 45: case 24: case 42: case 36: return true;   // STORE / STOREATOMIC / FENCE: no value
    case 19:  // ALLOCA [instty, opty, op, align]
      if (r.ops.empty()) return Err("alloca");
      Result(in, ~0u); break;
    case 20: case 41: {  // LOAD [opval, (ty), align, vol]
      if (!Pair(r, s, no, a)) return Err("load");
      uint32_t t = ~0u;
      if (s + 3 == r.ops.size()) t = uint32_t(r.ops[s]);
      Result(in, t); break;
    }
    case 43: case 4: case 30: case 6: case 7: case 8: case 27: case 23: case 37: case 38: case 46: case 40: case 47: case 13:
      Result(in, ~0u); break;   // GEP, element / aggregate ops, atomics, landingpad, invoke: a value we do not model
    default:
      return Err("inst_code");
    }
    insts_.push_back(std::move(in));
    return true;
  }

  // ---- metadata: dx.resources ----
  bool MdConstInt(uint64_t mdref_plus1, uint64_t &out) const {   // NODE operand (md id + 1) -> integer constant
    if (!mdref_plus1 || mdref_plus1 - 1 >= md_.size()) return false;
    const Md &m = md_[size_t(mdref_plus1 - 1)];
    if (m.kind != 2 || m.value >= module_values_.size()) return false;
    const Value &v = module_values_[m.value];
    if (v.kind != 1) return false;
    out = v.ival; return true;
  }
  bool ResourceList(uint64_t list_plus1, unsigned klass, std::vector<Resource> &out) {
    if (!list_plus1) return true;   // null = none
    if (list_plus1 - 1 >= md_.size() || md_[size_t(list_plus1 - 1)].kind != 3) return false;
    for (uint64_t e : md_[size_t(list_plus1 - 1)].ops) {
      if (!e || e - 1 >= md_.size() || md_[size_t(e - 1)].kind != 3) return false;
      const auto &ops = md_[size_t(e - 1)].ops;
      if (ops.size() < 6) return false;
      uint64_t id = 0, space = 0, lower = 0, count = 0, kind = 0;
      if (!MdConstInt(ops[0], id) || !MdConstInt(ops[3], space) || !MdConstInt(ops[4], lower) || !MdConstInt(ops[5], count))
        return false;
      if ((klass == 0 || klass == 1) && (ops.size() < 7 || !MdConstInt(ops[6], kind))) return false;
      if (id != out.size()) return false;   // IDs are dense and in order
      Resource res; res.space = uint32_t(space); res.lower = uint32_t(lower); res.count = uint32_t(count); res.kind = uint32_t(kind);
      out.push_back(res);
    }
    return true;
  }

  // ---- analysis ----
  std::vector<int32_t> node_of_;   // value id -> node (or -1 not built yet)
  struct Handle { int klass = -1; uint32_t range = 0; uint32_t index = 0; };
  std::vector<Handle> handle_of_;  // value id -> resource handle (createHandle)
  struct CbRet { bool valid = false; uint32_t space = 0, reg = 0, row = 0; bool is_float = false; };
  std::vector<CbRet> cbret_of_;
  std::vector<int> tid_of_;        // value id -> threadId component or -1

  uint32_t Add(const Node &n) { f_.nodes.push_back(n); return uint32_t(f_.nodes.size() - 1); }
  uint32_t Unknown() { Node n; n.op = OpUnknown; return Add(n); }
  bool ConstOf(uint32_t v, uint64_t &out) const {
    if (v >= values_.size() || values_[v].kind != 1) return false;
    out = values_[v].ival; return true;
  }
  uint8_t WidthOf(uint32_t v) const {
    const uint32_t t = TypeOf(v);
    if (t == ~1u) return 1;
    const uint32_t w = IntWidth(t);
    return uint8_t(w ? std::min<uint32_t>(w, 64) : 0);
  }
  uint32_t NodeOf(uint32_t v, unsigned depth = 0) {
    if (v >= values_.size()) return Unknown();
    if (node_of_[v] >= 0) return uint32_t(node_of_[v]);
    uint32_t id;
    if (depth > 200) id = Unknown();
    else id = Build(v, depth);
    node_of_[v] = int32_t(id);
    return id;
  }
  uint32_t Build(uint32_t v, unsigned depth) {
    const Value &val = values_[v];
    const uint8_t w = WidthOf(v);
    if (val.kind == 1) { Node n; n.op = OpConst; n.k = val.ival; n.width = w ? w : 32; return Add(n); }
    if (val.kind != 3 || val.inst >= insts_.size() || !w) return Unknown();
    const Inst &in = insts_[val.inst];
    Node n; n.width = w;
    switch (in.code) {
    case 2: {
      static const Op map[] = {OpAdd, OpSub, OpMul, OpUdiv, OpUnknown, OpUrem, OpUnknown, OpShl, OpLshr, OpAshr, OpAnd, OpOr, OpXor};
      const uint64_t opc = in.imm[0];
      if (opc > 12 || map[opc] == OpUnknown || !WidthOf(in.v[0])) return Unknown();
      n.op = map[opc]; n.a = NodeOf(in.v[0], depth + 1); n.b = NodeOf(in.v[1], depth + 1); return Add(n);
    }
    case 3: {
      const uint64_t opc = in.imm[0];
      if (!WidthOf(in.v[0])) return Unknown();
      if (opc == 0) n.op = OpTrunc; else if (opc == 1) n.op = OpZext; else if (opc == 2) n.op = OpSext;
      else if (opc == 11 && WidthOf(in.v[0]) == w) return NodeOf(in.v[0], depth + 1);
      else return Unknown();
      n.a = NodeOf(in.v[0], depth + 1); return Add(n);
    }
    case 28: case 9: {
      const uint64_t pred = in.imm[0];
      if (pred < 32 || pred > 41 || !WidthOf(in.v[0])) return Unknown();
      n.op = OpIcmp; n.pred = uint8_t(pred); n.width = 1;
      n.a = NodeOf(in.v[0], depth + 1); n.b = NodeOf(in.v[1], depth + 1); return Add(n);
    }
    case 29: case 5:
      n.op = OpSelect; n.a = NodeOf(in.v[0], depth + 1); n.b = NodeOf(in.v[1], depth + 1); n.c = NodeOf(in.v[2], depth + 1);
      return Add(n);
    case 26: {
      const uint32_t agg = in.v[0];
      if (agg < cbret_of_.size() && cbret_of_[agg].valid && in.imm.size() == 1 && in.imm[0] < 4 && !cbret_of_[agg].is_float) {
        const auto &cb = cbret_of_[agg];
        n.op = OpCb; n.a = cb.space; n.b = cb.reg; n.c = cb.row * 4 + uint32_t(in.imm[0]); return Add(n);
      }
      return Unknown();
    }
    case 34: {
      if (v < tid_of_.size() && tid_of_[v] >= 0) { n.op = OpTid; n.a = uint32_t(tid_of_[v]); return Add(n); }
      return Unknown();
    }
    default: return Unknown();
    }
  }

  bool Analyze() {
    // resources from !dx.resources
    auto it = named_md_.find("dx.resources");
    if (it != named_md_.end() && !it->second.empty()) {
      const uint64_t top = it->second[0];
      if (top >= md_.size() || md_[size_t(top)].kind != 3 || md_[size_t(top)].ops.size() < 4) return Fail("md_resources");
      const auto &lists = md_[size_t(top)].ops;
      if (!ResourceList(lists[0], 0, f_.srvs) || !ResourceList(lists[1], 1, f_.uavs) || !ResourceList(lists[2], 2, f_.cbvs))
        return Fail("md_resource_list");
    }
    f_.uav_stores.assign(f_.uavs.size(), 0);
    node_of_.assign(values_.size(), -1);
    handle_of_.assign(values_.size(), Handle{});
    cbret_of_.assign(values_.size(), CbRet{});
    tid_of_.assign(values_.size(), -1);
    // pass 1: handles, thread IDs, CB loads; refuse any UAV use other than typed stores
    struct StoreCall { size_t inst; };
    std::vector<size_t> store_insts;
    for (size_t i = 0; i < insts_.size(); ++i) {
      const Inst &in = insts_[i];
      if (in.code == 38 || in.code == 37 || in.code == 46) return Fail("memory_atomic");
      if (in.code != 34) continue;
      if (in.v.size() < 2) return Fail("call_noargs");
      uint64_t opc = 0;
      if (!ConstOf(in.v[1], opc)) return Fail("call_not_dxop");
      const auto arg = [&](size_t k) { return k + 1 < in.v.size() ? in.v[k + 1] : ~0u; };   // k-th dx.op argument after the opcode
      switch (opc) {
      case 57: {   // createHandle(class, rangeID, index, nonUniform)
        uint64_t klass = 0, range = 0, index = 0;
        if (!ConstOf(arg(1), klass) || !ConstOf(arg(2), range)) return Fail("handle_const");
        if (!ConstOf(arg(3), index)) return Fail("dynamic_index");
        if (in.result < handle_of_.size()) handle_of_[in.result] = Handle{int(klass), uint32_t(range), uint32_t(index)};
        break;
      }
      case 160: case 216: case 217: case 218: return Fail("handle_kind");   // createHandleForLib / SM 6.6 handles
      case 93: {   // threadId(component)
        uint64_t comp = 0;
        if (!ConstOf(arg(1), comp) || comp > 2) return Fail("tid_const");
        if (in.result < tid_of_.size()) tid_of_[in.result] = int(comp);
        break;
      }
      case 59: {   // cbufferLoadLegacy(handle, row)
        const uint32_t h = arg(1); uint64_t row = 0;
        if (h < handle_of_.size() && handle_of_[h].klass == 2 && ConstOf(arg(2), row) && in.result < cbret_of_.size() &&
            handle_of_[h].range < f_.cbvs.size()) {
          CbRet cb; cb.valid = true; cb.space = f_.cbvs[handle_of_[h].range].space; cb.reg = handle_of_[h].index; cb.row = uint32_t(row);
          const uint32_t t = in.type;
          cb.is_float = t < types_.size() && types_[t].kind == 5 && !types_[t].elems.empty() &&
                        types_[t].elems[0] < types_.size() && types_[types_[t].elems[0]].kind == 3;
          cbret_of_[in.result] = cb;
        }
        break;
      }
      case 67: store_insts.push_back(i); break;   // textureStore
      default:
        for (size_t k = 2; k < in.v.size(); ++k)
          if (in.v[k] < handle_of_.size() && handle_of_[in.v[k]].klass == 1) return Fail("uav_other_access");
        break;
      }
    }
    // CFG: predecessors per block (BR terminators); a block with an unmodelled terminator taints its successors
    const uint32_t nbb = declared_blocks_;
    if (!nbb) return Fail("no_blocks");
    std::vector<std::vector<uint32_t>> preds(nbb);
    std::vector<int32_t> term(nbb, -1);
    for (size_t i = 0; i < insts_.size(); ++i) {
      const Inst &in = insts_[i];
      if (in.code == 11 || in.code == 10 || in.code == 12 || in.code == 31 || in.code == 15 || in.code == 39) {
        if (in.bb >= nbb) return Fail("bb_range");
        term[in.bb] = int32_t(i);
        if (in.code == 11) {
          for (uint64_t t : in.imm) {
            if (t >= nbb) return Fail("br_target");
            if (std::find(preds[size_t(t)].begin(), preds[size_t(t)].end(), in.bb) == preds[size_t(t)].end()) preds[size_t(t)].push_back(in.bb);
          }
        } else if (in.code == 12) {
          return Fail("switch");   // conservative: a switch anywhere -> no form
        }
      }
    }
    for (size_t si : store_insts) {
      const Inst &in = insts_[si];
      ++f_.store_total;
      // textureStore(handle, c0, c1, c2, v0, v1, v2, v3, mask)
      if (in.v.size() != 11) return Fail("store_args");
      const uint32_t h = in.v[2];
      if (h >= handle_of_.size() || handle_of_[h].klass != 1) return Fail("store_handle");
      const uint32_t range = handle_of_[h].range;
      if (range >= f_.uavs.size()) return Fail("store_range");
      uint64_t mask = 0;
      if (!ConstOf(in.v[10], mask)) continue;
      const auto tid = [&](uint32_t v) { return v < tid_of_.size() ? tid_of_[v] : -1; };
      const uint32_t kind = f_.uavs[range].kind;
      bool identity = (mask & 0xF) == 0xF && tid(in.v[3]) == 0 && tid(in.v[4]) == 1;
      if (kind == 7) identity = identity && tid(in.v[5]) == 2;          // Texture2DArray: slice = thread ID z
      else if (kind != 2) identity = false;                              // only 2D and 2D arrays are modelled
      if (!identity) continue;
      Store st; st.uav = range;
      // straight path from the entry: every block on it has exactly one predecessor
      uint32_t bb = in.bb; bool straight = true; unsigned guard = 0;
      while (bb != 0) {
        if (preds[bb].size() != 1 || ++guard > nbb) { straight = false; break; }
        const uint32_t p = preds[bb][0];
        if (term[p] < 0) { straight = false; break; }
        const Inst &br = insts_[size_t(term[p])];
        if (br.code != 11) { straight = false; break; }
        if (br.imm.size() == 2 && br.imm[0] != br.imm[1]) {
          Cond c; c.node = NodeOf(br.v[0]); c.want = br.imm[0] == bb;
          st.path.push_back(c);
        }
        bb = p;
      }
      if (!straight) continue;
      std::reverse(st.path.begin(), st.path.end());
      f_.stores.push_back(std::move(st));
      ++f_.uav_stores[range];
    }
    if (f_.uavs.empty()) return Fail("no_uav");
    for (uint32_t i = 0; i < f_.uavs.size(); ++i)
      if (!f_.uav_stores[i]) return Fail("uav_without_identity_store");
    f_.ok = true;
    Purity();
    return true;
  }
};

inline void Analyzer::Purity() {
  Form &f = f_;
  f.pure = f.store_total == f.stores.size();
  f.cb_words.clear();
  std::vector<uint8_t> seen(f.nodes.size(), 0);
  std::vector<uint32_t> stack;
  for (const auto &st : f.stores) for (const auto &c : st.path) stack.push_back(c.node);
  while (!stack.empty() && f.pure) {
    const uint32_t n = stack.back(); stack.pop_back();
    if (n >= f.nodes.size()) { f.pure = false; break; }
    if (seen[n]) continue;
    seen[n] = 1;
    const Node &x = f.nodes[n];
    switch (x.op) {
    case OpUnknown: f.pure = false; break;
    case OpConst: case OpTid: break;
    case OpCb: {
      bool have = false;
      for (const auto &w : f.cb_words) if (w.space == x.a && w.reg == x.b && w.word == x.c) { have = true; break; }
      if (!have) f.cb_words.push_back({x.a, x.b, x.c});
      break;
    }
    case OpZext: case OpSext: case OpTrunc: stack.push_back(x.a); break;
    case OpSelect: stack.push_back(x.a); stack.push_back(x.b); stack.push_back(x.c); break;
    default: stack.push_back(x.a); stack.push_back(x.b); break;
    }
  }
  if (!f.pure || f.cb_words.size() > 64) { f.pure = false; f.cb_words.clear(); }
}

inline std::shared_ptr<Form> Analyze(const void *container, size_t length) {
  auto f = std::make_shared<Form>();
  try {
    Analyzer a(*f);
    if (!a.Run(container, length)) { f->ok = false; if (f->why.empty()) f->why = "refused"; }
  } catch (...) {
    f->ok = false; f->why = "exception";
  }
  return f;
}

// ---- evaluation at dispatch time ----------------------------------------------------------------------------------------------
// Value domain: an unsigned interval [lo, hi] of `width` bits (lo == hi: exact), or unknown.
struct Val { bool known = false; uint64_t lo = 0, hi = 0; uint8_t width = 32; };
// Box of the thread IDs to prove a condition over (inclusive upper bounds).
struct Box { uint64_t hi[3] = {0, 0, 0}; };
// CB word lookup: (space, register, 32-bit word index) -> value; false = unresolvable.
using CbFetch = bool (*)(void *ctx, uint32_t space, uint32_t reg, uint32_t word, uint32_t &out);

class Evaluator {
public:
  Evaluator(const Form &f, const Box &box, CbFetch fetch, void *ctx) : f_(f), box_(box), fetch_(fetch), ctx_(ctx),
      memo_(f.nodes.size()), done_(f.nodes.size(), 0) {}
  Val Eval(uint32_t n, unsigned depth = 0) {
    if (n >= f_.nodes.size() || depth > 400) return {};
    if (done_[n]) return memo_[n];
    Val v = Compute(f_.nodes[n], depth);
    memo_[n] = v; done_[n] = 1;
    return v;
  }
  // true: the condition is `want` for every thread of the box
  bool Holds(const Cond &c) {
    const Val v = Eval(c.node);
    if (!v.known || v.width != 1 || v.lo != v.hi) return false;
    return (v.lo != 0) == c.want;
  }
private:
  const Form &f_; Box box_; CbFetch fetch_; void *ctx_;
  std::vector<Val> memo_; std::vector<uint8_t> done_;
  static uint64_t Mask(uint8_t w) { return w >= 64 ? ~uint64_t(0) : (uint64_t(1) << w) - 1; }
  static Val Exact(uint64_t x, uint8_t w) { Val v; v.known = true; v.width = w; v.lo = v.hi = x & Mask(w); return v; }
  static Val Range(uint64_t lo, uint64_t hi, uint8_t w) { Val v; v.known = true; v.width = w; v.lo = lo; v.hi = hi; return v; }
  static Val Full(uint8_t w) { return Range(0, Mask(w), w); }
  static bool Ex(const Val &v) { return v.known && v.lo == v.hi; }
  static int64_t Signed(uint64_t x, uint8_t w) { return w >= 64 ? int64_t(x) : int64_t(x << (64 - w)) >> (64 - w); }
  Val Compute(const Node &n, unsigned depth) {
    const uint8_t w = n.width;
    switch (n.op) {
    case OpConst: return Exact(n.k, w);
    case OpTid: return n.a < 3 ? Range(0, box_.hi[n.a], w) : Val{};
    case OpCb: { uint32_t x = 0; if (!fetch_ || !fetch_(ctx_, n.a, n.b, n.c, x)) return {}; return Exact(x, w); }
    case OpZext: { Val a = Eval(n.a, depth + 1); if (!a.known) return {}; return Range(a.lo, a.hi, w); }
    case OpTrunc: { Val a = Eval(n.a, depth + 1); if (!a.known) return {}; if (a.hi <= Mask(w)) return Range(a.lo, a.hi, w); return Ex(a) ? Exact(a.lo, w) : Full(w); }
    case OpSext: {
      Val a = Eval(n.a, depth + 1); if (!a.known) return {};
      if (a.hi < (uint64_t(1) << (a.width - 1))) return Range(a.lo, a.hi, w);   // non-negative: same as zext
      return Ex(a) ? Exact(uint64_t(Signed(a.lo, a.width)), w) : Full(w);
    }
    case OpSelect: {
      Val c = Eval(n.a, depth + 1);
      if (Ex(c)) return Eval(c.lo ? n.b : n.c, depth + 1);
      Val x = Eval(n.b, depth + 1), y = Eval(n.c, depth + 1);
      if (!c.known || !x.known || !y.known) return {};
      return Range(std::min(x.lo, y.lo), std::max(x.hi, y.hi), w);
    }
    case OpIcmp: {
      Val a = Eval(n.a, depth + 1), b = Eval(n.b, depth + 1);
      if (!a.known || !b.known) return {};
      const uint8_t aw = a.width;
      const bool sign = n.pred >= 38;
      uint64_t alo = a.lo, ahi = a.hi, blo = b.lo, bhi = b.hi;
      if (sign) {   // signed compare: only when both intervals lie in the non-negative half
        const uint64_t half = uint64_t(1) << (aw - 1);
        if (ahi >= half || bhi >= half) return {};
      }
      int t = -1;   // -1 unknown, 0 false, 1 true
      switch (n.pred) {
      case 32: if (Ex(a) && Ex(b)) t = alo == blo; else if (ahi < blo || bhi < alo) t = 0; break;   // eq
      case 33: if (Ex(a) && Ex(b)) t = alo != blo; else if (ahi < blo || bhi < alo) t = 1; break;   // ne
      case 34: case 38: if (alo > bhi) t = 1; else if (ahi <= blo) t = 0; break;                   // gt
      case 35: case 39: if (alo >= bhi) t = 1; else if (ahi < blo) t = 0; break;                   // ge
      case 36: case 40: if (ahi < blo) t = 1; else if (alo >= bhi) t = 0; break;                   // lt
      case 37: case 41: if (ahi <= blo) t = 1; else if (alo > bhi) t = 0; break;                   // le
      default: break;
      }
      if (t < 0) return Range(0, 1, 1);
      return Exact(uint64_t(t), 1);
    }
    default: break;
    }
    Val a = Eval(n.a, depth + 1), b = Eval(n.b, depth + 1);
    if (!a.known || !b.known) return {};
    const uint64_t m = Mask(w);
    if (w == 1) {   // boolean logic, three-valued
      const int x = Ex(a) ? int(a.lo) : -1, y = Ex(b) ? int(b.lo) : -1;
      switch (n.op) {
      case OpAnd: if (x == 0 || y == 0) return Exact(0, 1); if (x == 1 && y == 1) return Exact(1, 1); return Range(0, 1, 1);
      case OpOr: if (x == 1 || y == 1) return Exact(1, 1); if (x == 0 && y == 0) return Exact(0, 1); return Range(0, 1, 1);
      case OpXor: if (x >= 0 && y >= 0) return Exact(uint64_t(x ^ y), 1); return Range(0, 1, 1);
      default: return Range(0, 1, 1);
      }
    }
    if (Ex(a) && Ex(b)) {
      const uint64_t x = a.lo, y = b.lo;
      switch (n.op) {
      case OpAdd: return Exact(x + y, w);
      case OpSub: return Exact(x - y, w);
      case OpMul: return Exact(x * y, w);
      case OpShl: return y < w ? Exact(x << y, w) : Val{};
      case OpLshr: return y < w ? Exact(x >> y, w) : Val{};
      case OpAshr: return y < w ? Exact(uint64_t(Signed(x, w) >> y), w) : Val{};
      case OpAnd: return Exact(x & y, w);
      case OpOr: return Exact(x | y, w);
      case OpXor: return Exact(x ^ y, w);
      case OpUdiv: return y ? Exact(x / y, w) : Val{};
      case OpUrem: return y ? Exact(x % y, w) : Val{};
      default: return {};
      }
    }
    switch (n.op) {   // monotone cases with one interval operand
    case OpAdd: if (a.hi + b.hi <= m && a.hi + b.hi >= a.hi) return Range(a.lo + b.lo, a.hi + b.hi, w); return Full(w);
    case OpSub: if (a.lo >= b.hi) return Range(a.lo - b.hi, a.hi - b.lo, w); return Full(w);
    case OpMul: if (Ex(b) && (!b.lo || a.hi <= m / b.lo)) return Range(a.lo * b.lo, a.hi * b.lo, w);
                if (Ex(a) && (!a.lo || b.hi <= m / a.lo)) return Range(b.lo * a.lo, b.hi * a.lo, w); return Full(w);
    case OpLshr: if (Ex(b) && b.lo < w) return Range(a.lo >> b.lo, a.hi >> b.lo, w); return Full(w);
    case OpShl: if (Ex(b) && b.lo < w && (a.hi << b.lo) >> b.lo == a.hi && (a.hi << b.lo) <= m) return Range(a.lo << b.lo, a.hi << b.lo, w); return Full(w);
    case OpUdiv: if (Ex(b) && b.lo) return Range(a.lo / b.lo, a.hi / b.lo, w); return Full(w);
    case OpAnd: if (Ex(b)) return Range(0, std::min(a.hi, b.lo), w); if (Ex(a)) return Range(0, std::min(b.hi, a.lo), w); return Full(w);
    default: return Full(w);
    }
  }
};

// Is every texel of a view of extent (width, height, layers) written by this dispatch? `grid` = threads per axis of the
// dispatch (groups x numthreads). Requires an identity store of `uav` whose straight-path conditions all hold over the box.
inline bool Covers(const Form &f, uint32_t uav, uint32_t width, uint32_t height, uint32_t layers, const uint64_t grid[3],
                   CbFetch fetch, void *ctx) {
  if (!f.ok || uav >= f.uavs.size() || !width || !height || !layers) return false;
  if (grid[0] < width || grid[1] < height || grid[2] < (f.uavs[uav].kind == 7 ? layers : 1)) return false;
  Box box; box.hi[0] = width - 1; box.hi[1] = height - 1; box.hi[2] = f.uavs[uav].kind == 7 ? layers - 1 : 0;
  Evaluator ev(f, box, fetch, ctx);
  for (const auto &st : f.stores) {
    if (st.uav != uav) continue;
    bool all = true;
    for (const auto &c : st.path) if (!ev.Holds(c)) { all = false; break; }
    if (all) return true;
  }
  return false;
}

} // namespace dxmt::light_rename::form
