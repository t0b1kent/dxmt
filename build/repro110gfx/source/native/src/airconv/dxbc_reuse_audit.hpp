// SPDX-License-Identifier: LGPL-2.1-or-later
// Read-only, bounded dependency inventory. Does not run the optimizer or emit AIR.
#pragma once
#include "dxbc_converter.hpp"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include <set>
#include <tuple>
#include <type_traits>

namespace dxmt::dxbc::reuse_audit {
struct Query {
  SM50ShaderInternal &shader;
  SM50_SHADER_REUSE_AUDIT_DATA &out;
  uint64_t visits = 0;
  using Key = std::tuple<uint32_t,uint32_t,uint32_t>;
  std::map<Key,unsigned> rows;
  bool Step() { if (++visits <= 200000) return true; ++out.unknown; return false; }
  unsigned Row(uint32_t space, uint32_t binding, uint32_t reg, uint32_t bound) {
    Key key{space,binding,reg};
    if (auto i=rows.find(key); i!=rows.end()) return i->second;
    if (out.count==SM50_REUSE_AUDIT_ROWS) { ++out.overflow; return UINT32_MAX; }
    unsigned id=out.count++; rows[key]=id;
    out.rows[id]={uint32_t(shader.shader_type),space,binding,reg,bound,1}; return id;
  }
  void Mark(unsigned id,unsigned flags) { if (id<out.count) out.rows[id].flags|=flags; }
  static std::optional<uint32_t> UInt(const llvm::Value *v) {
    if (auto c=llvm::dyn_cast<llvm::ConstantInt>(v); c && c->getValue().getActiveBits()<=32) return uint32_t(c->getZExtValue());
    return {};
  }
  unsigned DXILLoad(const llvm::CallInst &load, const dxil::EntryPoint &entry) {
    if (load.arg_size()<3) { ++out.unknown; return UINT32_MAX; }
    const auto handle=llvm::dyn_cast<llvm::CallInst>(load.getArgOperand(1));
    if (!handle || !handle->getCalledFunction() || handle->getCalledFunction()->getName()!="dx.op.createHandle" || handle->arg_size()<4) {
      ++out.unknown; return UINT32_MAX;
    }
    auto kind=UInt(handle->getArgOperand(1)), id=UInt(handle->getArgOperand(2));
    auto binding=UInt(handle->getArgOperand(3)), reg=UInt(load.getArgOperand(2));
    if (!kind || *kind!=2 || !id || !binding || !reg) { ++out.unknown; return UINT32_MAX; }
    for (const auto &r:entry.resources) if (r.id==*id && uint32_t(r.resource_class)==2) {
      if (*binding<r.lower_bound || *binding-r.lower_bound>=r.range_size || uint64_t(*reg)*16>=r.cbuffer_size) {
        ++out.unknown; return UINT32_MAX;
      }
      return Row(r.space,*binding,*reg,(r.cbuffer_size+15)/16);
    }
    ++out.unknown; return UINT32_MAX;
  }
  void DXIL(const dxil::EntryPoint &entry) {
    if (!entry.function) return;
    std::map<const llvm::Value *,unsigned> loads;
    for (const auto &bb:*entry.function) for (const auto &inst:bb) {
      if (!Step()) return;
      auto call=llvm::dyn_cast<llvm::CallInst>(&inst);
      if (call && call->getCalledFunction() && call->getCalledFunction()->getName().startswith("dx.op.cbufferLoadLegacy"))
        loads[call]=DXILLoad(*call,entry);
    }
    auto walk=[&](const llvm::Value *root,unsigned flags) {
      std::vector<const llvm::Value *> todo{root}; std::set<const llvm::Value *> seen;
      while (!todo.empty()) {
        auto v=todo.back();todo.pop_back();
        if (!seen.insert(v).second) continue;
        if (!Step()) return;
        if (auto i=loads.find(v);i!=loads.end()) { Mark(i->second,flags);continue; }
        if (auto i=llvm::dyn_cast<llvm::Instruction>(v)) {
          if (llvm::isa<llvm::LoadInst>(i)) ++out.unknown; // memory-mediated dependencies need alias analysis
          if (auto c=llvm::dyn_cast<llvm::CallInst>(i); c && (!c->getCalledFunction() || !c->getCalledFunction()->getName().startswith("dx.op."))) ++out.unknown;
          for (const auto &op:i->operands()) todo.push_back(op.get());
        }
      }
    };
    llvm::DominatorTree dom(*const_cast<llvm::Function *>(entry.function)); llvm::LoopInfo loops(dom);
    for (const auto &bb:*entry.function) for (const auto &inst:bb) {
      const llvm::Value *condition=nullptr;
      if (auto b=llvm::dyn_cast<llvm::BranchInst>(&inst); b && b->isConditional()) condition=b->getCondition();
      if (auto s=llvm::dyn_cast<llvm::SwitchInst>(&inst)) condition=s->getCondition();
      if (auto s=llvm::dyn_cast<llvm::SelectInst>(&inst)) condition=s->getCondition();
      if (!condition) continue;
      unsigned flags=2;
      if (inst.isTerminator()) for (auto l=loops.getLoopFor(&bb);l;l=l->getParentLoop())
        if (l->isLoopExiting(&bb)) flags|=4;
      walk(condition,flags);
    }
  }

  struct Dependencies { std::set<unsigned> cb; std::set<uint64_t> temps; };
  static void Join(Dependencies &a,const Dependencies &b) { a.cb.insert(b.cb.begin(),b.cb.end());a.temps.insert(b.temps.begin(),b.temps.end()); }
  std::map<uint64_t,Dependencies> definitions;
  Dependencies Source(const SrcOperand &operand) {
    Dependencies d;
    std::visit([&](const auto &v) {
      using T=std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T,SrcOperandConstantBuffer>) {
        auto b=std::get_if<uint32_t>(&v.rangeindex),r=std::get_if<uint32_t>(&v.regindex);
        auto decl=shader.shader_info.cbufferMap.find(v.rangeid);
        if (!b || !r || decl==shader.shader_info.cbufferMap.end()) { ++out.unknown; return; }
        const auto &info=decl->second;
        if (*b<info.range.lower_bound || *b-info.range.lower_bound>=info.range.size || *r>=info.size_in_vec4) { ++out.unknown; return; }
        d.cb.insert(Row(info.range.space,*b,*r,info.size_in_vec4));
      } else if constexpr (std::is_same_v<T,SrcOperandTemp>) d.temps.insert((uint64_t(v.phase)<<32)|v.regid);
      else if constexpr (std::is_same_v<T,SrcOperandIndexableTemp>) ++out.unknown;
    },operand);
    return d;
  }
  void Destination(const DstOperand &operand,const Dependencies &d) {
    std::visit([&](const auto &v) {
      using T=std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T,DstOperandTemp>) Join(definitions[(uint64_t(v.phase)<<32)|v.regid],d);
      else if constexpr (std::is_same_v<T,DstOperandIndexableTemp>) ++out.unknown;
    },operand);
  }
  void MarkDependency(Dependencies d,unsigned flags) {
    std::set<uint64_t> seen;
    while (!d.temps.empty()) {
      auto t=*d.temps.begin();d.temps.erase(d.temps.begin());
      if (!seen.insert(t).second) continue;
      if (!Step()) return;
      if (auto i=definitions.find(t);i!=definitions.end()) Join(d,i->second);
      else ++out.unknown;
    }
    for (auto id:d.cb) Mark(id,flags);
  }
  std::vector<BasicBlock *> Successors(const BasicBlock &bb) {
    std::vector<BasicBlock *> out;
    std::visit([&](const auto &t) {
      using T=std::decay_t<decltype(t)>;
      if constexpr (std::is_same_v<T,BasicBlockConditionalBranch>) out={t.true_branch,t.false_branch};
      else if constexpr (std::is_same_v<T,BasicBlockUnconditionalBranch>) out={t.target};
      else if constexpr (std::is_same_v<T,BasicBlockSwitch>) { out.push_back(t.case_default);for (auto [_,b]:t.cases) out.push_back(b); }
      else if constexpr (std::is_same_v<T,BasicBlockInstanceBarrier>) out={t.active,t.sync};
      else if constexpr (std::is_same_v<T,BasicBlockHullShaderWriteOutput>) out={t.epilogue};
      else if constexpr (std::is_same_v<T,BasicBlockCall>) { ++this->out.unknown;out={t.return_point}; }
    },bb.target);
    return out;
  }
  bool Reaches(const BasicBlock *from,const BasicBlock *target) {
    std::vector<const BasicBlock *> todo{from};std::set<const BasicBlock *> seen;
    while (!todo.empty()) {
      auto b=todo.back();todo.pop_back();if (!b) continue;if (b==target) return true;
      if (!seen.insert(b).second) continue;if (!Step()) return false;
      for (auto next:Successors(*b)) todo.push_back(next);
    }
    return false;
  }
  void DXBC() {
    // Union of all definitions deliberately over-approximates reaching definitions.
    // Report flags as MAY depend, never an exact optimized branch/register count.
    std::vector<Dependencies> selects;
    for (const auto &bb:shader.bbs) bb->instructions.for_each([&](const auto &i) {
      if (!Step()) return;
      Dependencies d;
#define REUSE_SRC(field) if constexpr (requires { i.field; }) { if constexpr (std::is_same_v<std::decay_t<decltype(i.field)>,SrcOperand>) Join(d,Source(i.field)); }
      REUSE_SRC(src) REUSE_SRC(src0) REUSE_SRC(src1) REUSE_SRC(src2) REUSE_SRC(src3)
      REUSE_SRC(src_cond) REUSE_SRC(src_address) REUSE_SRC(src_byte_offset) REUSE_SRC(src_bias)
      REUSE_SRC(src_lod) REUSE_SRC(src_mip_level) REUSE_SRC(src_reference) REUSE_SRC(src_sample_index)
      REUSE_SRC(src_x_derivative) REUSE_SRC(src_y_derivative) REUSE_SRC(dst_address) REUSE_SRC(dst_byte_offset)
      REUSE_SRC(offset) REUSE_SRC(sample_index)
#undef REUSE_SRC
#define REUSE_DST(field) if constexpr (requires { i.field; }) { if constexpr (std::is_same_v<std::decay_t<decltype(i.field)>,DstOperand>) Destination(i.field,d); }
      REUSE_DST(dst) REUSE_DST(dst0) REUSE_DST(dst1) REUSE_DST(dst_carry) REUSE_DST(dst_cos)
      REUSE_DST(dst_hi) REUSE_DST(dst_low) REUSE_DST(dst_original) REUSE_DST(dst_quot) REUSE_DST(dst_rem) REUSE_DST(dst_sin)
#undef REUSE_DST
      if constexpr (requires { i.src_cond; }) selects.push_back(Source(i.src_cond));
    });
    for (auto &d:selects) MarkDependency(d,2);
    for (const auto &bb:shader.bbs) {
      bool back=false,exit=false;for (auto next:Successors(*bb)) (Reaches(next,bb.get())?back:exit)=true;
      const unsigned flags=2|((back&&exit)?4:0);
      std::visit([&](const auto &t) {
        using T=std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T,BasicBlockConditionalBranch>) MarkDependency(Source(t.cond.operand),flags);
        else if constexpr (std::is_same_v<T,BasicBlockSwitch>) MarkDependency(Source(t.value),flags);
      },bb->target);
    }
  }
};
inline void Run(SM50ShaderInternal &shader, SM50_SHADER_REUSE_AUDIT_DATA &out) {
  if (out.size != sizeof(out)) return;
  out.applied=out.count=out.unknown=out.overflow=0;
  Query q{shader,out};
  if (shader.dxil) { q.DXIL(shader.dxil_entry);q.DXIL(shader.dxil_patch_constant_entry); }
  else q.DXBC();
  out.applied=SM50_REUSE_AUDIT_APPLIED;
}
} // namespace dxmt::dxbc::reuse_audit
