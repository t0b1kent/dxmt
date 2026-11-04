// MacRunner EMIT-DIV, 2026-10-24 (v6 2026-10-25). Opt-in exact i32 division by a uniform value.
#pragma once
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <cstring>

namespace dxmt::uniform_div {
using namespace llvm;

inline bool enabled(const char *name) {
  const char *s = std::getenv(name);
  return s && std::strcmp(s, "1") == 0;
}

// Constant address space alone does NOT imply a uniform load: its address must
// be uniform too. In particular, per-lane indexing into a CB remains varying.
struct Uniformity {
  DenseMap<Value *, unsigned> state;
  SmallPtrSet<BasicBlock *, 16> visiting;
  bool same(Value *a, Value *b, unsigned depth = 0) {
    if (a == b) return true;
    if (depth > 32 || a->getType() != b->getType()) return false;
    if (auto *p = dyn_cast<PHINode>(a)) {
      for (Value *v : p->incoming_values()) if (!same(v, b, depth + 1)) return false;
      return true;
    }
    if (isa<PHINode>(b)) return same(b, a, depth + 1);
    auto *ia = dyn_cast<Instruction>(a), *ib = dyn_cast<Instruction>(b);
    if (!ia || !ib || !ia->isSameOperationAs(ib)) return false;
    if (auto *la = dyn_cast<LoadInst>(ia)) {
      auto *lb = cast<LoadInst>(ib);
      if (la->isVolatile() || lb->isVolatile() || la->isAtomic() || lb->isAtomic() || la->getPointerAddressSpace() != 2)
        return false;
    } else if (!isa<GetElementPtrInst>(ia) && !isa<CastInst>(ia) &&
               !isa<ExtractElementInst>(ia) && !isa<ExtractValueInst>(ia)) return false;
    for (unsigned n = 0; n < ia->getNumOperands(); ++n)
      if (!same(ia->getOperand(n), ib->getOperand(n), depth + 1)) return false;
    return true;
  }
  static bool pureMath(CallInst *call) {
    auto *fn = call->getCalledFunction();
    if (!fn || !fn->doesNotAccessMemory()) return false;
    StringRef name = fn->getName();
    if (!name.consume_front("air.")) return false;
    name.consume_front("fast_");
    auto op = name.split('.').first;
    // Do not infer uniformity from readnone alone: lane/quad intrinsics can
    // be readnone too. Only scalar/vector mathematical functions qualify.
    for (auto math : {"convert", "dot", "fma", "fabs", "abs", "sqrt", "rsqrt", "sin", "cos", "tan",
                      "asin", "acos", "atan", "atan2", "log", "log2", "exp", "exp2", "pow", "floor",
                      "ceil", "trunc", "rint", "round", "fmin", "fmax", "min", "max", "clamp",
                      "clz", "ctz", "popcount", "reverse_bits"})
      if (op == math) return true;
    return false;
  }
  bool control(BasicBlock *bb) {
    if (bb == &bb->getParent()->getEntryBlock()) return true;
    if (!visiting.insert(bb).second) return false; // conservatively reject cycles
    bool ok = true;
    for (auto *p : predecessors(bb)) {
      auto *t = p->getTerminator();
      if (auto *b = dyn_cast<BranchInst>(t)) {
        ok &= !b->isConditional() || is(b->getCondition());
      } else if (auto *s = dyn_cast<SwitchInst>(t)) {
        ok &= is(s->getCondition());
      } else ok = false;
      if (ok) ok &= control(p);
    }
    visiting.erase(bb);
    return ok;
  }
  bool is(Value *v) {
    if (isa<UndefValue>(v) || isa<PoisonValue>(v)) return false;
    if (isa<Constant>(v)) return true;
    if (auto *a = dyn_cast<Argument>(v))
      return a->getType()->isPointerTy() && a->getType()->getPointerAddressSpace() == 2;
    auto it = state.find(v);
    if (it != state.end()) return it->second == 2;
    state[v] = 1;
    auto *i = dyn_cast<Instruction>(v);
    bool ok = i != nullptr;
    if (auto *l = dyn_cast_or_null<LoadInst>(i))
      ok = !l->isVolatile() && !l->isAtomic() && l->getPointerAddressSpace() == 2 && is(l->getPointerOperand());
    else if (auto *p = dyn_cast_or_null<PHINode>(i)) {
      if (auto *single = p->hasConstantValue()) ok = is(single);
      else {
        // GVN PRE merges separate reads of the SAME immutable CB descriptor
        // across varying control flow. Those values are equal, not varying.
        bool equal = true;
        Value *first = p->getIncomingValue(0);
        for (Value *x : p->incoming_values()) equal &= same(first, x);
        if (equal) ok = is(first);
        else {
          ok = control(p->getParent());
          for (Value *x : p->incoming_values()) if (ok) ok &= is(x);
        }
      }
    } else if (auto *call = dyn_cast_or_null<CallInst>(i)) {
      ok = pureMath(call);
      for (Value *x : call->args()) if (ok) ok &= is(x);
    } else if (i && (isa<BinaryOperator>(i) || isa<CastInst>(i) || isa<FreezeInst>(i) || isa<CmpInst>(i) ||
                   isa<SelectInst>(i) || isa<GetElementPtrInst>(i) || isa<ExtractElementInst>(i) ||
                   isa<InsertElementInst>(i) || isa<ShuffleVectorInst>(i) || isa<ExtractValueInst>(i) ||
                   isa<InsertValueInst>(i))) {
      for (Value *x : i->operands()) if (ok) ok &= is(x);
    } else ok = false;
    state[v] = ok ? 2 : 3;
    return ok;
  }
};


inline Argument *positionArgument(Module &m, Function &f) {
  auto *entries = m.getNamedMetadata("air.fragment");
  if (!entries) return nullptr;
  for (MDNode *entry : entries->operands()) {
    if (entry->getNumOperands() < 3) continue;
    auto *fn = dyn_cast_or_null<ValueAsMetadata>(entry->getOperand(0));
    if (!fn || fn->getValue() != &f) continue;
    auto *args = dyn_cast_or_null<MDNode>(entry->getOperand(2));
    if (!args) continue;
    for (auto &op : args->operands()) {
      auto *arg = dyn_cast_or_null<MDNode>(op);
      if (!arg || arg->getNumOperands() < 2) continue;
      bool position = false;
      for (auto &field : arg->operands())
        if (auto *s = dyn_cast_or_null<MDString>(field)) position |= s->getString() == "air.position";
      auto *idx = mdconst::dyn_extract<ConstantInt>(arg->getOperand(0));
      if (position && idx && idx->getZExtValue() < f.arg_size()) return f.getArg(idx->getZExtValue());
    }
  }
  return nullptr;
}

inline bool shaderEntry(Module &m, Function &f) {
  for (auto name : {"air.fragment", "air.vertex", "air.kernel"}) {
    auto *entries = m.getNamedMetadata(name);
    if (!entries) continue;
    for (MDNode *entry : entries->operands()) {
      if (!entry->getNumOperands()) continue;
      auto *fn = dyn_cast_or_null<ValueAsMetadata>(entry->getOperand(0));
      if (fn && fn->getValue() == &f) return true;
    }
  }
  return false;
}

// O2's DivRemPairs freezes a numerator shared by a div and a rem. For any
// non-poison value freeze is the identity, so the bound of its operand holds;
// a poison numerator has no defined quotient in the input DXIL either (the
// freeze is LLVM's own refinement), so no defined result changes.
inline Value *thaw(Value *v) {
  while (auto *f = dyn_cast<FreezeInst>(v)) v = f->getOperand(0);
  return v;
}

inline uint64_t maxNumerator(Value *x, bool sign, Argument *pos, const DataLayout &dl) {
  x = thaw(x);
  // D3D12_VIEWPORT_BOUNDS_MAX=32767; only direct x/y, never z/w or derived
  // coordinates. Rasterized positions are nonnegative and inside the target.
  // No assumption is made about an arbitrary FPToUI or a CB value.
  if (auto *cast = dyn_cast<CastInst>(x); cast && (isa<FPToUIInst>(cast) || isa<FPToSIInst>(cast))) {
    if (auto *e = dyn_cast<ExtractElementInst>(cast->getOperand(0))) {
      auto *index = dyn_cast<ConstantInt>(e->getIndexOperand());
      if (pos && e->getVectorOperand() == pos && index && index->getZExtValue() < 2)
        return 32767;
    }
  }
  auto bits = computeKnownBits(x, dl);
  if (!sign || bits.isNonNegative()) return bits.getMaxValue().getZExtValue();
  return uint64_t(1) << 31; // magnitude of any signed i32, including INT_MIN
}

// The ONLY divisions this pass rewrites: i32, dynamic divisor that is uniform
// in a shader entry, numerator provably <= 2^19 (one estimate is then exact).
// Measured 25.10 (v2-v5): for unbounded numerators every exact replacement
// (uniform fallback with udiv, division-free fallback, two-step exact path)
// was at best equal to the release udiv and up to 6 % slower in some PS, so
// those divisions keep the release code byte for byte.
constexpr uint64_t kBoundedNumerator = uint64_t(1) << 19;
inline bool candidate(Instruction &i, Uniformity &u, Argument *pos, const DataLayout &dl, uint64_t *bound = nullptr) {
  unsigned op = i.getOpcode();
  if (op != Instruction::UDiv && op != Instruction::URem && op != Instruction::SDiv && op != Instruction::SRem) return false;
  if (!i.getType()->isIntegerTy(32) || isa<Constant>(i.getOperand(1))) return false;
  bool sign = op == Instruction::SDiv || op == Instruction::SRem;
  uint64_t n = maxNumerator(i.getOperand(0), sign, pos, dl);
  if (bound) *bound = n;
  return u.is(i.getOperand(1)) && n <= kBoundedNumerator;
}

// LLVM treats zero division and INT_MIN/-1 as poison. Preserve the D3D domain
// of the divisions this pass rewrites before optimization can delete derived
// exceptional values (cb-cb). The late call resolves literal cases only. Ordinary nonzero literals stay with
// LLVM's normal shifts/multiply strength reduction.
inline void prepareConstants(Module &m, bool beforeOptimization = false) {
  if (!enabled("MACRUNNER_AIRCONV_UNIFORM_DIV")) return;
  SmallVector<BinaryOperator *, 8> work;
  for (Function &f : m) {
    if (f.isDeclaration()) continue;
    Uniformity u;
    Argument *pos = positionArgument(m, f);
    bool entry = shaderEntry(m, f);
    for (Instruction &i : instructions(f)) {
      unsigned op = i.getOpcode();
      if (op != Instruction::UDiv && op != Instruction::URem &&
          op != Instruction::SDiv && op != Instruction::SRem) continue;
      if (!i.getType()->isIntegerTy(32)) continue;
      auto *d = dyn_cast<ConstantInt>(i.getOperand(1));
      bool lower = !d && beforeOptimization && entry && candidate(i, u, pos, m.getDataLayout());
      if (lower || (d && (d->isZero() || ((op == Instruction::SDiv || op == Instruction::SRem) && d->isMinusOne()))))
        work.push_back(cast<BinaryOperator>(&i));
    }
  }
  for (auto *i : work) {
    IRBuilder<> b(i);
    auto *literal = dyn_cast<ConstantInt>(i->getOperand(1));
    if (!literal) {
      // Preserve the D3D domain BEFORE LLVM may fold a derived zero (cb-cb)
      // or exploit division's undefined cases to remove surrounding code.
      // Only for divisions the late pass will rewrite (candidate()); every
      // other division keeps the release code byte for byte.
      Value *d = i->getOperand(1), *x = i->getOperand(0);
      bool sign = i->getOpcode() == Instruction::SDiv || i->getOpcode() == Instruction::SRem;
      auto *zero = b.CreateICmpEQ(d, b.getInt32(0), "div.domain.zero");
      Value *minusOne = sign ? b.CreateICmpEQ(d, b.getInt32(-1), "div.domain.minusone") : nullptr;
      Value *exception = sign ? b.CreateOr(zero, minusOne) : zero;
      i->setOperand(1, b.CreateSelect(exception, b.getInt32(1), d, "div.domain.safe"));
      b.SetInsertPoint(i->getNextNode());
      // Build the replacement using a placeholder, so replaceAllUsesWith
      // cannot create a self-reference in the newly inserted selects.
      Value *placeholder = PoisonValue::get(i->getType());
      Value *v = placeholder;
      if (sign) v = b.CreateSelect(minusOne, i->getOpcode() == Instruction::SDiv ? b.CreateNeg(x) : b.getInt32(0), v);
      auto *result = cast<Instruction>(b.CreateSelect(zero, b.getInt32(0xffffffffu), v, "div.domain.result"));
      i->replaceAllUsesWith(result);
      if (sign) cast<SelectInst>(v)->setFalseValue(i);
      else cast<SelectInst>(result)->setFalseValue(i);
      continue;
    }
    Value *v = b.getInt32(0xffffffffu);
    if (!literal->isZero())
      v = i->getOpcode() == Instruction::SDiv ? b.CreateNeg(i->getOperand(0)) : b.getInt32(0);
    i->replaceAllUsesWith(v); i->eraseFromParent();
  }
}

struct Reciprocal { Value *d, *zero, *r; };
inline Reciprocal reciprocal(Value *divisor, bool sign, Instruction *use) {
  Instruction *where = use;
  if (auto *i = dyn_cast<Instruction>(divisor)) {
    where = isa<PHINode>(i) ? &*i->getParent()->getFirstInsertionPt() : i->getNextNode();
  } else if (isa<Argument>(divisor)) where = &*use->getFunction()->getEntryBlock().getFirstInsertionPt();
  IRBuilder<> b(where);
  Value *d = divisor;
  if (sign) d = b.CreateSelect(b.CreateICmpSLT(d, b.getInt32(0)), b.CreateNeg(d), d, "udiv.absd");
  auto *zero = b.CreateICmpEQ(d, b.getInt32(0), "udiv.zero");
  d = b.CreateSelect(zero, b.getInt32(1), d, "udiv.safe.d");
  auto *df = b.CreateUIToFP(d, b.getFloatTy(), "udiv.df");
  // One reciprocal per uniform denominator, shared by div/rem and numerators.
  auto *r = b.CreateFDiv(ConstantFP::get(b.getFloatTy(), 1.0), df, "udiv.rcp");
  if (auto *i = dyn_cast<Instruction>(r)) {
    FastMathFlags flags; flags.setAllowReciprocal(); i->setFastMathFlags(flags);
  }
  return {d, zero, r};
}

// Bounded numerators x <= 2^19 (e.g. SV_Position x/y, N = 32767): the total
// error (uint->float, rounded d, reciprocal <= 3 ulp, multiply) is < 1, so q0 is
// q-1, q or q+1. q0 >= 1 needs d <= x(1+9u), hence q0*d <= x + d <= 2^20: the
// product never wraps and one correction on each side is exact.
inline std::pair<Value *, Value *> estimate(IRBuilder<> &b, Value *x, Reciprocal &v, bool broken) {
  auto *q = b.CreateFPToUI(b.CreateFMul(b.CreateUIToFP(x, b.getFloatTy()), v.r), b.getInt32Ty(), "udiv.q0");
  Value *p = b.CreateMul(q, v.d, "udiv.product");
  if (broken) return {q, b.CreateSub(x, p)};
  Value *over = b.CreateICmpUGT(p, x);
  q = b.CreateSelect(over, b.CreateSub(q, b.getInt32(1)), q, "udiv.q1");
  p = b.CreateSelect(over, b.CreateSub(p, v.d), p);
  auto *r = b.CreateSub(x, p, "udiv.r1");
  auto *under = b.CreateICmpUGE(r, v.d);
  return {b.CreateAdd(q, b.CreateZExt(under, b.getInt32Ty()), "udiv.q"),
          b.CreateSelect(under, b.CreateSub(r, v.d), r, "udiv.r")};
}

// negativeControl deliberately omits the +-1 corrections; only the standalone
// probe (tools/transform.cpp) sets it. airconv calls run(M).
inline void run(Module &m, bool negativeControl = false) {
  const bool active = enabled("MACRUNNER_AIRCONV_UNIFORM_DIV");
  const bool audit = enabled("MACRUNNER_AIRCONV_UNIFORM_DIV_AUDIT");
  if (!active && !audit) return; // default path: no metadata, IR or pass changes
  prepareConstants(m); // also covers standalone transform tests
  for (Function &f : m) {
    // A helper's constant-space pointer argument may be supplied with a
    // varying address. Only actual shader-entry buffer arguments are uniform.
    if (f.isDeclaration() || !shaderEntry(m, f)) continue;
    Uniformity u;
    auto *pos = positionArgument(m, f);
    struct Work { BinaryOperator *op; uint64_t bound; };
    SmallVector<Work, 16> work;
    for (Instruction &i : instructions(f)) {
      unsigned op = i.getOpcode();
      if (op != Instruction::UDiv && op != Instruction::URem && op != Instruction::SDiv && op != Instruction::SRem) continue;
      uint64_t bound = 0;
      bool rewrite = candidate(i, u, pos, m.getDataLayout(), &bound);
      if (audit)
        errs() << "uniform-div op=" << i.getOpcodeName() << " uniform=" << u.is(i.getOperand(1))
               << " i32=" << i.getType()->isIntegerTy(32) << " literal=" << isa<Constant>(i.getOperand(1))
               << " max=" << bound << " rewrite=" << rewrite << " ir=" << i << '\n';
      // Literals stay with LLVM/Apple strength reduction (shift, multiply).
      if (active && rewrite) work.push_back({cast<BinaryOperator>(&i), bound});
    }
    DenseMap<Value *, Reciprocal> unsignedR, signedR;
    for (auto w : work) {
      auto *i = w.op;
      unsigned op = i->getOpcode();
      bool sign = op == Instruction::SDiv || op == Instruction::SRem;
      bool rem = op == Instruction::URem || op == Instruction::SRem;
      Value *den = i->getOperand(1), *num = i->getOperand(0);
      auto &cache = sign ? signedR : unsignedR;
      auto it = cache.find(den);
      if (it == cache.end()) it = cache.insert({den, reciprocal(den, sign, i)}).first;
      Reciprocal &v = it->second;
      IRBuilder<> b(i);
      Value *x = num;
      // Magnitudes: INT_MIN is the unsigned 0x80000000, inside the exact range.
      if (sign) x = b.CreateSelect(b.CreateICmpSLT(x, b.getInt32(0)), b.CreateNeg(x), x, "udiv.absx");
      // q <= 2^19 is provable: one estimate (error < 1) and +-1 corrections.
      auto qr = estimate(b, x, v, negativeControl);
      Value *result = rem ? qr.second : qr.first;
      if (sign) {
        Value *negative = b.CreateICmpSLT(num, b.getInt32(0));
        if (!rem) negative = b.CreateXor(negative, b.CreateICmpSLT(den, b.getInt32(0)));
        result = b.CreateSelect(negative, b.CreateNeg(result), result);
      }
      // D3D unsigned division/remainder by zero return all ones. Define the
      // same deterministic result for otherwise undefined signed zero cases.
      result = b.CreateSelect(v.zero, b.getInt32(0xffffffffu), result, "uniform.div.result");
      i->replaceAllUsesWith(result); i->eraseFromParent();
    }
  }
}
} // namespace dxmt::uniform_div
