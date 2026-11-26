#include "dxil_converter_base.hpp"

#include "../airconv_error.hpp"
#include "../dxbc_converter.hpp"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <bit>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;
using namespace llvm::air;

namespace dxmt::dxil {

llvm::Error
unsupported(llvm::StringRef what) {
  return llvm::make_error<dxmt::UnsupportedFeature>(what);
}

std::optional<uint32_t>
asU32(Value *value) {
  if (auto *ci = dyn_cast<ConstantInt>(value))
    return (uint32_t)ci->getZExtValue();
  return std::nullopt;
}

int32_t
tryGetI32(Value *value) {
  if (auto *ci = dyn_cast<ConstantInt>(value))
    return (int32_t)ci->getSExtValue();
  return (int32_t)0;
}

Converter::Converter(EntryPoint const &entry, dxmt::dxbc::context &ctx) :
    ctx(ctx),
    ir(ctx.builder),
    air(ctx.air),
    entry(entry) {}

llvm::Expected<llvm::BasicBlock *>
Converter::Run(llvm::BasicBlock *ReturnBB) {
  if (!entry.function)
    return unsupported("dxil: entry point is missing");

  {
    for (auto &BB : *entry.function) {
      auto *NewBB = llvm::BasicBlock::Create(air.getContext(), "", ctx.function);
      block_map_[&BB] = NewBB;
      ir.SetInsertPoint(NewBB);
      for (auto &Inst : BB) {
        auto *PHI = dyn_cast<PHINode>(&Inst);
        if (!PHI)
          break;
        value_map_[PHI] = ir.CreatePHI(MapType(PHI->getType()), 0);
      }
    }

    // FIXME: remove const_cast
    for (auto &BB : *const_cast<llvm::Function *>(entry.function)) {
      ir.SetInsertPoint(block_map_[&BB]);
      for (auto &Inst : BB) {
        if (isa<PHINode>(&Inst))
          continue;
        if (Inst.isTerminator()) {
          if (auto err = TranslateTerminator(&Inst, ReturnBB))
            return err;
          break;
        }
        auto Result = TranslateInstruction(&Inst);
        if (!Result)
          return Result.takeError();
        if (*Result && !(*Result)->getType()->isVoidTy())
          value_map_[&Inst] = *Result;
      }
    }
  }

  return block_map_[&entry.function->getEntryBlock()];
}

llvm::Type *
Converter::MapType(llvm::Type *Type) {
  auto Found = type_map_.find(Type);
  if (Found != type_map_.end())
    return Found->second;

  llvm::Type *TyMapped = nullptr;
  if (Type->isVoidTy()) {
    TyMapped = llvm::Type::getVoidTy(air.getContext());
  } else if (Type->isIntegerTy()) {
    TyMapped = llvm::IntegerType::get(air.getContext(), Type->getIntegerBitWidth());
  } else if (Type->isHalfTy()) {
    TyMapped = llvm::Type::getHalfTy(air.getContext());
  } else if (Type->isFloatTy()) {
    TyMapped = llvm::Type::getFloatTy(air.getContext());
  } else if (Type->isDoubleTy()) {
    TyMapped = llvm::Type::getDoubleTy(air.getContext());
  } else if (auto *pointer = llvm::dyn_cast<llvm::PointerType>(Type)) {
    TyMapped = MapType(pointer->getNonOpaquePointerElementType())->getPointerTo(pointer->getAddressSpace());
  } else if (auto *array = llvm::dyn_cast<llvm::ArrayType>(Type)) {
    TyMapped = llvm::ArrayType::get(MapType(array->getElementType()), array->getNumElements());
  } else if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(Type)) {
    TyMapped = llvm::FixedVectorType::get(MapType(vector->getElementType()), vector->getNumElements());
  } else if (auto *function = llvm::dyn_cast<llvm::FunctionType>(Type)) {
    llvm::SmallVector<llvm::Type *, 8> Params;
    for (auto *param : function->params())
      Params.push_back(MapType(param));
    TyMapped = llvm::FunctionType::get(MapType(function->getReturnType()), Params, function->isVarArg());
  } else if (auto *TyStruct = llvm::dyn_cast<llvm::StructType>(Type)) {
    if (TyStruct->isOpaque()) {
      TyMapped = llvm::StructType::create(air.getContext(), TyStruct->getName());
    } else if (TyStruct->hasName()) {
      auto *TyNamed = llvm::StructType::create(air.getContext(), TyStruct->getName());
      type_map_[Type] = TyNamed;
      llvm::SmallVector<llvm::Type *, 8> Elements;
      for (auto *TyElement : TyStruct->elements())
        Elements.push_back(MapType(TyElement));
      TyNamed->setBody(Elements);
      TyMapped = TyNamed;
    } else {
      llvm::SmallVector<llvm::Type *, 8> Elements;
      for (auto *TyElement : TyStruct->elements())
        Elements.push_back(MapType(TyElement));
      TyMapped = llvm::StructType::get(air.getContext(), Elements);
    }
  } else {
    TyMapped = Type;
  }

  type_map_[Type] = TyMapped;
  return TyMapped;
}

llvm::Expected<llvm::Constant *>
Converter::MapConstantExpr(llvm::ConstantExpr *Expr) {
  switch (Expr->getOpcode()) {
  case llvm::Instruction::GetElementPtr: {
    llvm::SmallVector<llvm::Constant *, 8> Indices;
    for (unsigned i = 1; i < Expr->getNumOperands(); i++) {
      auto Index = MapConstant(llvm::cast<llvm::Constant>(Expr->getOperand(i)));
      if (!Index)
        return Index.takeError();
      Indices.push_back(*Index);
    }
    auto Ptr = MapConstant(llvm::cast<llvm::Constant>(Expr->getOperand(0)));
    if (!Ptr)
      return Ptr.takeError();
    auto *TySrc = MapType(Expr->getOperand(0)->getType()->getNonOpaquePointerElementType());
    return llvm::ConstantExpr::getGetElementPtr(TySrc, *Ptr, Indices);
  }
  case llvm::Instruction::BitCast:
  case llvm::Instruction::Trunc:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::SIToFP:
  case llvm::Instruction::FPToUI:
  case llvm::Instruction::FPToSI:
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::AddrSpaceCast: {
    auto Operand = MapConstant(llvm::cast<llvm::Constant>(Expr->getOperand(0)));
    if (!Operand)
      return Operand.takeError();
    return llvm::ConstantExpr::getCast(Expr->getOpcode(), *Operand, MapType(Expr->getType()));
  }
  default:
    return unsupported(
        std::string("dxil: unsupported constant expression: ") + Instruction::getOpcodeName(Expr->getOpcode())
    );
  }
}

llvm::Expected<llvm::Constant *>
Converter::MapConstant(llvm::Constant *Constant) {
  auto Found = constant_map_.find(Constant);
  if (Found != constant_map_.end())
    return Found->second;

  llvm::Constant *Mapped = nullptr;
  if (auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Constant)) {
    Mapped = llvm::ConstantInt::get(MapType(Integer->getType()), Integer->getValue());
  } else if (auto *FP = llvm::dyn_cast<llvm::ConstantFP>(Constant)) {
    Mapped = llvm::ConstantFP::get(air.getContext(), FP->getValueAPF());
  } else if (llvm::isa<llvm::ConstantPointerNull>(Constant)) {
    Mapped = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(MapType(Constant->getType())));
  } else if (llvm::isa<llvm::UndefValue>(Constant)) {
    Mapped = llvm::UndefValue::get(MapType(Constant->getType()));
  } else if (llvm::isa<llvm::PoisonValue>(Constant)) {
    Mapped = llvm::PoisonValue::get(MapType(Constant->getType()));
  } else if (llvm::isa<llvm::ConstantAggregateZero>(Constant)) {
    Mapped = llvm::ConstantAggregateZero::get(MapType(Constant->getType()));
  } else if (auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Constant)) {
    auto MappedGlobal = MapGlobal(Global);
    if (!MappedGlobal)
      return MappedGlobal.takeError();
    Mapped = *MappedGlobal;
  } else if (auto *Sequence = llvm::dyn_cast<llvm::ConstantDataSequential>(Constant)) {
    llvm::SmallVector<llvm::Constant *, 16> Elements;
    for (unsigned i = 0; i < Sequence->getNumElements(); i++) {
      auto Element = MapConstant(Sequence->getElementAsConstant(i));
      if (!Element)
        return Element.takeError();
      Elements.push_back(*Element);
    }
    if (Sequence->getType()->isVectorTy())
      Mapped = llvm::ConstantVector::get(Elements);
    else
      Mapped = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(MapType(Sequence->getType())), Elements);
  } else if (auto *Vector = llvm::dyn_cast<llvm::ConstantVector>(Constant)) {
    llvm::SmallVector<llvm::Constant *, 16> Elements;
    for (auto &Operand : Vector->operands()) {
      auto Element = MapConstant(llvm::cast<llvm::Constant>(Operand));
      if (!Element)
        return Element.takeError();
      Elements.push_back(*Element);
    }
    Mapped = llvm::ConstantVector::get(Elements);
  } else if (auto *Array = llvm::dyn_cast<llvm::ConstantArray>(Constant)) {
    llvm::SmallVector<llvm::Constant *, 16> Elements;
    for (auto &Operand : Array->operands()) {
      auto Element = MapConstant(llvm::cast<llvm::Constant>(Operand));
      if (!Element)
        return Element.takeError();
      Elements.push_back(*Element);
    }
    Mapped = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(MapType(Array->getType())), Elements);
  } else if (auto *Structure = llvm::dyn_cast<llvm::ConstantStruct>(Constant)) {
    llvm::SmallVector<llvm::Constant *, 16> Elements;
    for (auto &Operand : Structure->operands()) {
      auto Element = MapConstant(llvm::cast<llvm::Constant>(Operand));
      if (!Element)
        return Element.takeError();
      Elements.push_back(*Element);
    }
    Mapped = llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(MapType(Structure->getType())), Elements);
  } else if (auto *Expr = llvm::dyn_cast<llvm::ConstantExpr>(Constant)) {
    auto MappedExpr = MapConstantExpr(Expr);
    if (!MappedExpr)
      return MappedExpr.takeError();
    Mapped = *MappedExpr;
  } else {
    return unsupported("dxil: unsupported Constant");
  }

  constant_map_[Constant] = Mapped;
  return Mapped;
}

llvm::Expected<llvm::GlobalVariable *>
Converter::MapGlobal(llvm::GlobalVariable *Global) {
  auto Found = global_map_.find(Global);
  if (Found != global_map_.end())
    return Found->second;

  auto *Type = MapType(Global->getValueType());
  llvm::GlobalVariable *Mapped = nullptr;
  if (Global->getAddressSpace() == 3) {
    if (Global->hasInitializer() && !isa<llvm::UndefValue>(Global->getInitializer()))
      return unsupported("dxil: unsupported tgsm initializer");
    Mapped = new llvm::GlobalVariable(
        *air.getModule(), Type, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(Type),
        Global->getName(), nullptr, Global->getThreadLocalMode(), 3
    );
  } else if (Global->hasLocalLinkage() && Global->isConstant() && Global->hasInitializer()) {
    auto initializer = MapConstant(Global->getInitializer());
    if (!initializer)
      return initializer.takeError();
    Mapped = new llvm::GlobalVariable(
        *air.getModule(), Type, true, Global->getLinkage(), *initializer, Global->getName(), nullptr,
        Global->getThreadLocalMode(), 2
    );
  } else {
    return unsupported("dxil: unsupported Global variable " + Global->getName().str());
  }
  Mapped->setAlignment(Global->getAlign());
  Mapped->setUnnamedAddr(Global->getUnnamedAddr());
  global_map_[Global] = Mapped;
  return Mapped;
}

llvm::Expected<llvm::Value *>
Converter::Get(llvm::Value *Value) {
  auto Found = value_map_.find(Value);
  if (Found != value_map_.end())
    return Found->second;
  if (auto *Constant = llvm::dyn_cast<llvm::Constant>(Value)) {
    auto Mapped = MapConstant(Constant);
    if (!Mapped)
      return Mapped.takeError();
    return *Mapped;
  }
  return unsupported("dxil: reference to untranslated Value");
}

llvm::Error
Converter::TranslateTerminator(llvm::Instruction *Inst, llvm::BasicBlock *ReturnBB) {
  if (auto *Return = dyn_cast<ReturnInst>(Inst)) {
    if (Return->getNumOperands())
      return unsupported("dxil: non-void return in a compute shader");
    ir.CreateBr(ReturnBB);
  } else if (auto *Branch = dyn_cast<BranchInst>(Inst)) {
    if (Branch->isConditional()) {
      auto Cond = Get(Branch->getCondition());
      if (!Cond)
        return Cond.takeError();
      ir.CreateCondBr(*Cond, block_map_[Branch->getSuccessor(0)], block_map_[Branch->getSuccessor(1)]);
    } else {
      ir.CreateBr(block_map_[Branch->getSuccessor(0)]);
    }
  } else if (llvm::isa<llvm::UnreachableInst>(Inst)) {
    ir.CreateUnreachable();
  } else if (auto *Switch = dyn_cast<SwitchInst>(Inst)) {
    auto Cond = Get(Switch->getCondition());
    if (!Cond)
      return Cond.takeError();
    auto *NewSwitch = ir.CreateSwitch(*Cond, block_map_[Switch->getDefaultDest()], Switch->getNumCases());
    for (auto &Case : Switch->cases()) {
      auto CaseValue = MapConstant(Case.getCaseValue());
      if (!CaseValue)
        return CaseValue.takeError();
      NewSwitch->addCase(llvm::cast<llvm::ConstantInt>(*CaseValue), block_map_[Case.getCaseSuccessor()]);
    }
  } else {
    return unsupported("dxil: unsupported terminator");
  }

  auto *NewBB = ir.GetInsertBlock();
  SmallPtrSet<BasicBlock *, 4> Seen;
  for (auto *Succ : successors(Inst->getParent())) {
    auto Found = block_map_.find(Succ);
    if (Found == block_map_.end() || !Seen.insert(Succ).second)
      continue;
    for (auto &InstInSucc : *Succ) {
      auto *PHI = dyn_cast<PHINode>(&InstInSucc);
      if (!PHI)
        break;
      auto MapFound = value_map_.find(PHI);
      if (MapFound == value_map_.end())
        return unsupported("dxil: phi without translation");
      auto Incoming = Get(PHI->getIncomingValueForBlock(Inst->getParent()));
      if (!Incoming)
        return Incoming.takeError();
      cast<PHINode>(MapFound->second)->addIncoming(*Incoming, NewBB);
    }
  }
  return llvm::Error::success();
}

llvm::Expected<llvm::Value *>
Converter::TranslateInstruction(llvm::Instruction *Inst) {
  auto operand = [&](unsigned index) { return Get(Inst->getOperand(index)); };

  switch (Inst->getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::Mul:
  case llvm::Instruction::UDiv:
  case llvm::Instruction::SDiv:
  case llvm::Instruction::URem:
  case llvm::Instruction::SRem:
  case llvm::Instruction::Shl:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::FSub:
  case llvm::Instruction::FMul:
  case llvm::Instruction::FDiv:
  case llvm::Instruction::FRem: {
    auto LHS = operand(0);
    if (!LHS)
      return LHS.takeError();
    auto RHS = operand(1);
    if (!RHS)
      return RHS.takeError();
    auto *BinOp = llvm::BinaryOperator::Create((llvm::Instruction::BinaryOps)Inst->getOpcode(), *LHS, *RHS);
    if (llvm::isa<llvm::FPMathOperator>(BinOp))
      BinOp->copyFastMathFlags(Inst);
    ir.Insert(BinOp);
    return BinOp;
  }
  case llvm::Instruction::FNeg: {
    auto Value = operand(0);
    if (!Value)
      return Value.takeError();
    auto *UnOp = llvm::UnaryOperator::Create((llvm::Instruction::UnaryOps)Inst->getOpcode(), *Value);
    if (llvm::isa<llvm::FPMathOperator>(UnOp))
      UnOp->copyFastMathFlags(Inst);
    ir.Insert(UnOp);
    return UnOp;
  }
  case llvm::Instruction::ICmp:
  case llvm::Instruction::FCmp: {
    auto LHS = operand(0);
    if (!LHS)
      return LHS.takeError();
    auto RHS = operand(1);
    if (!RHS)
      return RHS.takeError();
    auto *Cmp = llvm::CmpInst::Create(
        (llvm::Instruction::OtherOps)Inst->getOpcode(), llvm::cast<llvm::CmpInst>(Inst)->getPredicate(), *LHS, *RHS
    );
    if (llvm::isa<llvm::FPMathOperator>(Cmp))
      Cmp->copyFastMathFlags(Inst);
    ir.Insert(Cmp);
    return Cmp;
  }
  case llvm::Instruction::Select: {
    auto Cond = operand(0);
    if (!Cond)
      return Cond.takeError();
    auto True = operand(1);
    if (!True)
      return True.takeError();
    auto False = operand(2);
    if (!False)
      return False.takeError();
    return ir.CreateSelect(*Cond, *True, *False);
  }
  case llvm::Instruction::Trunc:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
  case llvm::Instruction::FPToUI:
  case llvm::Instruction::FPToSI:
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::SIToFP:
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::BitCast:
  case llvm::Instruction::AddrSpaceCast: {
    auto Value = operand(0);
    if (!Value)
      return Value.takeError();
    auto *Cast =
        llvm::CastInst::Create((llvm::Instruction::CastOps)Inst->getOpcode(), *Value, MapType(Inst->getType()));
    ir.Insert(Cast);
    return Cast;
  }
  case llvm::Instruction::GetElementPtr: {
    auto *OldGEP = llvm::cast<llvm::GetElementPtrInst>(Inst);
    auto Ptr = operand(0);
    if (!Ptr)
      return Ptr.takeError();
    llvm::SmallVector<llvm::Value *, 8> Indices;
    for (unsigned i = 1; i < Inst->getNumOperands(); i++) {
      auto Index = operand(i);
      if (!Index)
        return Index.takeError();
      Indices.push_back(*Index);
    }
    auto *GEP = llvm::GetElementPtrInst::Create(MapType(OldGEP->getSourceElementType()), *Ptr, Indices);
    GEP->setIsInBounds(OldGEP->isInBounds());
    ir.Insert(GEP);
    return GEP;
  }
  case llvm::Instruction::Load: {
    auto *OldLoad = llvm::cast<llvm::LoadInst>(Inst);
    auto Ptr = operand(0);
    if (!Ptr)
      return Ptr.takeError();
    auto *Load = new llvm::LoadInst(MapType(Inst->getType()), *Ptr, "", OldLoad->isVolatile(), OldLoad->getAlign());
    if (OldLoad->isAtomic())
      Load->setOrdering(OldLoad->getOrdering());
    ir.Insert(Load);
    return Load;
  }
  case llvm::Instruction::Store: {
    auto *OldStore = llvm::cast<llvm::StoreInst>(Inst);
    auto Value = operand(0);
    if (!Value)
      return Value.takeError();
    auto Ptr = operand(1);
    if (!Ptr)
      return Ptr.takeError();
    auto *Store = new llvm::StoreInst(*Value, *Ptr, OldStore->isVolatile(), OldStore->getAlign());
    if (OldStore->isAtomic())
      Store->setOrdering(OldStore->getOrdering());
    ir.Insert(Store);
    return (llvm::Value *)nullptr;
  }
  case llvm::Instruction::AtomicRMW: {   // MacRunner shader-prewarm 17.10: атомики на groupshared (address space 3)
    auto *RMW = llvm::cast<llvm::AtomicRMWInst>(Inst);
    auto Ptr = operand(0);
    if (!Ptr)
      return Ptr.takeError();
    auto Val = operand(1);
    if (!Val)
      return Val.takeError();
    return air.CreateAtomicRMW(RMW->getOperation(), *Ptr, *Val);
  }
  case llvm::Instruction::Alloca: {
    auto *OldAlloca = llvm::cast<llvm::AllocaInst>(Inst);
    llvm::Value *Size = nullptr;
    if (OldAlloca->isArrayAllocation()) {
      auto MappedSize = operand(0);
      if (!MappedSize)
        return MappedSize.takeError();
      Size = *MappedSize;
    }
    auto *Alloca = new llvm::AllocaInst(
        MapType(OldAlloca->getAllocatedType()), OldAlloca->getAddressSpace(), Size, OldAlloca->getAlign()
    );
    ir.Insert(Alloca);
    return Alloca;
  }
  case llvm::Instruction::ExtractElement: {
    auto Vectpr = operand(0);
    if (!Vectpr)
      return Vectpr.takeError();
    auto Index = operand(1);
    if (!Index)
      return Index.takeError();
    auto *ExtractElement = llvm::ExtractElementInst::Create(*Vectpr, *Index);
    ir.Insert(ExtractElement);
    return ExtractElement;
  }
  case llvm::Instruction::InsertElement: {
    auto Vector = operand(0);
    if (!Vector)
      return Vector.takeError();
    auto Element = operand(1);
    if (!Element)
      return Element.takeError();
    auto Index = operand(2);
    if (!Index)
      return Index.takeError();
    auto *InsertElement = llvm::InsertElementInst::Create(*Vector, *Element, *Index);
    ir.Insert(InsertElement);
    return InsertElement;
  }
  case llvm::Instruction::ShuffleVector: {
    auto First = operand(0);
    if (!First)
      return First.takeError();
    auto Second = operand(1);
    if (!Second)
      return Second.takeError();
    auto Mask = operand(2);
    if (!Mask)
      return Mask.takeError();
    return ir.CreateShuffleVector(*First, *Second, *Mask);
  }
  case llvm::Instruction::ExtractValue: {
    auto *OldExtract = llvm::cast<llvm::ExtractValueInst>(Inst);
    auto Aggregate = operand(0);
    if (!Aggregate)
      return Aggregate.takeError();
    auto *ExtractValue = llvm::ExtractValueInst::Create(*Aggregate, OldExtract->getIndices());
    ir.Insert(ExtractValue);
    return ExtractValue;
  }
  case llvm::Instruction::InsertValue: {
    auto *OldInsert = llvm::cast<llvm::InsertValueInst>(Inst);
    auto Aggregate = operand(0);
    if (!Aggregate)
      return Aggregate.takeError();
    auto Value = operand(1);
    if (!Value)
      return Value.takeError();
    auto *InsertValue = llvm::InsertValueInst::Create(*Aggregate, *Value, OldInsert->getIndices());
    ir.Insert(InsertValue);
    return InsertValue;
  }
  case llvm::Instruction::Freeze: {
    auto Value = operand(0);
    if (!Value)
      return Value.takeError();
    return ir.CreateFreeze(*Value);
  }
  case llvm::Instruction::Call:
    return TranslateCall(*llvm::cast<llvm::CallInst>(Inst));
  default:
    return unsupported(std::string("dxil: unsupported instruction: ") + Inst->getOpcodeName());
  }
}

llvm::Expected<HandleInfo>
Converter::ResolveHandleOperand(llvm::Value *Operand) {
  auto Found = handles_.find(Operand);
  if (Found == handles_.end())
    return unsupported("dxil: unresolvable resource handle");
  return Found->second;
}

llvm::Optional<dxmt::dxbc::TextureDescirptor>
Converter::GetTextureDescriptor(const HandleInfo &Handle) {
  return Handle.resource_class == ResourceClass::SRV ? ctx.binding.GetSRVTexture(air, Handle.range_id, Handle.index)
                                                     : ctx.binding.GetUAVTexture(air, Handle.range_id, Handle.index);
}

llvm::Optional<dxmt::dxbc::BufferDescriptor>
Converter::GetBufferDescriptor(const HandleInfo &Handle) {
  return Handle.resource_class == ResourceClass::SRV ? ctx.binding.GetSRVBuffer(air, Handle.range_id, Handle.index)
                                                     : ctx.binding.GetUAVBuffer(air, Handle.range_id, Handle.index);
}

llvm::Expected<dxmt::dxbc::SamplerDescriptor>
Converter::GetSamplerDescriptor(const HandleInfo &Handle) {
  auto descriptor = ctx.binding.GetSampler(air, Handle.range_id, Handle.index);
  if (!descriptor)
    return unsupported("dxil: no sampler descriptor for resource");
  return *descriptor;
}

bool
Converter::SupportsMemoryCoherency() const {
  return ctx.metal_version >= SM50_SHADER_METAL_320;
}

bool
Converter::SupportsNonExecutionBarrier() const {
  return ctx.metal_version >= SM50_SHADER_METAL_320;
}

llvm::air::Texture
Converter::MakeTexture(const dxmt::dxbc::TextureDescirptor &Descriptor) {
  return llvm::air::Texture{
      .kind = Descriptor.ResourceKind,
      .sample_type = Descriptor.SampleType,
      .memory_access = Descriptor.MemoryAccess,
  };
}

Value *
Converter::BitcastIfNeeded(Value *Value, Type *Type) {
  if (Value->getType() == Type)
    return Value;
  return ir.CreateBitCast(Value, Type);
}

Value *
Converter::BuildResRet(const CallInst &Call, Value *VecOrScalar, Value *Residency) {
  auto *TyStruct = cast<StructType>(MapType(Call.getType()));
  Type *Element = TyStruct->getElementType(0);
  Value *Result = PoisonValue::get(TyStruct);
  if (auto *Vector = dyn_cast<FixedVectorType>(VecOrScalar->getType())) {
    for (unsigned i = 0; i < 4 && i < Vector->getNumElements(); i++)
      Result = ir.CreateInsertValue(Result, ir.CreateExtractElement(VecOrScalar, (uint64_t)i), i);
  } else {
    Result = ir.CreateInsertValue(Result, VecOrScalar, 0);
    for (unsigned i = 1; i < 4; i++)
      Result = ir.CreateInsertValue(Result, Constant::getNullValue(Element), i);
  }
  Value *Status = Residency ? ir.CreateZExt(Residency, ir.getInt32Ty()) : ir.getInt32(0);
  Result = ir.CreateInsertValue(Result, Status, 4);
  return Result;
}

Value *
Converter::MakeVec(ArrayRef<Value *> Components) {
  Type *element = Components.front()->getType();
  Value *vec = PoisonValue::get(FixedVectorType::get(element, Components.size()));
  for (unsigned i = 0; i < Components.size(); i++)
    vec = ir.CreateInsertElement(vec, Components[i], (uint64_t)i);
  return vec;
}

Value *
Converter::AddOffsetIfNeeded(Value *Coord, int32_t Offset) {
  if (Offset == 0)
    return Coord;
  return ir.CreateAdd(Coord, ir.getInt32(Offset));
}

Type *
Converter::GetTextureSampleType(Texture::SampleType SampleType) {
  switch (SampleType) {
  case Texture::sample_float:
    return ir.getFloatTy();
  case Texture::sample_int:
  case Texture::sample_uint:
    return ir.getInt32Ty();
  case Texture::sample_half:
    return ir.getHalfTy();
  case Texture::sample_short:
  case Texture::sample_ushort:
    return ir.getInt16Ty();
  }
}

llvm::Expected<std::tuple<Value *, Value *, Value *>>
Converter::BuildRwAddress(
    Texture::ResourceKind LogicalKind, Value *C0, Value *C1, Value *C2, Value *MipOrSampleIndex, int32_t Off0,
    int32_t Off1, int32_t Off2
) {
  Value *Position = nullptr;
  Value *ArraySlice = nullptr;
  Value *SampleIndex = nullptr;
  switch (LogicalKind) {
  case Texture::texture1d:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), ir.getInt32(0)});
    break;
  case Texture::texture1d_array:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), ir.getInt32(0)});
    ArraySlice = C1;
    break;
  case Texture::texture2d:
  case Texture::depth2d:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1)});
    break;
  case Texture::texture2d_array:
  case Texture::depth2d_array:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1)});
    ArraySlice = C2;
    break;
  case Texture::texture3d:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1), AddOffsetIfNeeded(C2, Off2)});
    break;
  case Texture::texture2d_ms:
  case Texture::depth_2d_ms:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1)});
    SampleIndex = MipOrSampleIndex;
    break;
  case Texture::texture2d_ms_array:
  case Texture::depth_2d_ms_array:
    Position = MakeVec({AddOffsetIfNeeded(C0, Off0), AddOffsetIfNeeded(C1, Off1)});
    ArraySlice = C2;
    SampleIndex = MipOrSampleIndex;
    break;
  case Texture::texture_buffer:
    Position = C0;
    break;
  default:
    return unsupported(
        "dxil: unsupported texture kind for indexing: " + std::to_string(static_cast<uint32_t>(LogicalKind))
    );
  }
  return std::make_tuple(Position, ArraySlice, SampleIndex);
}

llvm::Expected<llvm::Value *>
Converter::TranslateCall(llvm::CallInst &Call) {
  auto *Callee = Call.getCalledFunction();
  if (!Callee)
    return unsupported("dxil: indirect call");
  if (Callee->getName().startswith("dx.op."))
    return EmitDxilOp(Call);
  return unsupported("dxil: unsupported call to " + Callee->getName().str());
}

llvm::Expected<llvm::Value *>
Converter::EmitDxilOp(CallInst &Call) {
  auto *callee = Call.getCalledFunction();
  StringRef op = callee->getName().drop_front(strlen("dx.op."));
  auto dot = op.rfind('.');
  if (dot != StringRef::npos)
    op = op.take_front(dot);

  if (op == "createHandle")
    return EmitCreateHandle(Call);
  if (op == "cbufferLoadLegacy")
    return EmitCbufferLoadLegacy(Call);
  if (op == "threadId")
    return EmitThreadId(Call, ThreadBuiltin::kGlobalId);
  if (op == "groupId")
    return EmitThreadId(Call, ThreadBuiltin::kGroupId);
  if (op == "threadIdInGroup")
    return EmitThreadId(Call, ThreadBuiltin::kLocalId);
  if (op == "flattenedThreadIdInGroup")
    return EmitThreadId(Call, ThreadBuiltin::kLocalLinearId);
  if (op == "bufferStore")
    return EmitBufferStore(Call);
  if (op == "bufferLoad")
    return EmitBufferLoad(Call);
  if (op == "textureStore")
    return EmitTextureStore(Call);
  if (op == "textureLoad")
    return EmitTextureLoad(Call);
  if (op == "bufferUpdateCounter")
    return EmitBufferUpdateCounter(Call);
  if (op == "getDimensions")
    return EmitGetDimensions(Call);
  if (op == "rawBufferStore")
    return EmitRawBufferStore(Call);
  if (op == "rawBufferLoad")
    return EmitRawBufferLoad(Call);
  if (op == "legacyF16ToF32")
    return EmitLegacyF16ToF32(Call);
  if (op == "legacyF32ToF16")
    return EmitLegacyF32ToF16(Call);
  if (op == "unaryBits")
    return EmitUnaryBits(Call);
  if (op == "barrier")
    return EmitBarrier(Call);
  if (op == "loadInput")   // MacRunner dxil-tess 17.10: у HS/DS/GS вход — [вершина][регистр]
    return (ctx.shader_type == microsoft::D3D11_SB_HULL_SHADER || ctx.shader_type == microsoft::D3D11_SB_DOMAIN_SHADER ||
            ctx.shader_type == microsoft::D3D10_SB_GEOMETRY_SHADER)
               ? EmitLoadInputIndexed(Call)
               : EmitLoadInput(Call);
  if (op == "storeOutput")
    return ctx.shader_type == microsoft::D3D11_SB_HULL_SHADER ? EmitStoreOutputHull(Call) : EmitStoreOutput(Call);
  if (op == "sample" || op == "sampleLevel" || op == "sampleBias" || op == "sampleCmp" || op == "sampleCmpLevelZero" || op == "sampleGrad")
    return EmitSample(Call, op);
  if (op == "textureGather" || op == "textureGatherCmp")
    return EmitGather(Call, op);
  // MacRunner shader-prewarm 17.10.2026: семейства из переписи ER
  if (op == "unary")
    return EmitUnary(Call);
  if (op == "binary")
    return EmitBinary(Call);
  if (op == "tertiary")
    return EmitTertiary(Call);
  if (op == "dot2")
    return EmitDot(Call, 2);
  if (op == "dot3")
    return EmitDot(Call, 3);
  if (op == "dot4")
    return EmitDot(Call, 4);
  if (op == "isSpecialFloat")
    return EmitIsSpecialFloat(Call);
  if (op == "discard")
    return EmitDiscard(Call);
  if (op == "atomicBinOp")
    return EmitAtomicBinOp(Call);

  return EmitStageOp(Call);   // MacRunner dxil-tess 17.10: dx.op ступеней HS/DS/GS; остальное — отказ там же
}

// ---- MacRunner shader-prewarm 17.10.2026: dx.op семейства unary/binary/tertiary/dot/isSpecialFloat/discard ----
// Номера — DXIL OpCode (dxil-spirv dxil.hpp): FAbs 6, Saturate 7, Cos 12, Sin 13, Tan 14, Acos 15, Asin 16, Atan 17, Hcos 18,
// Hsin 19, Htan 20, Exp 21 (2^x), Frc 22, Log 23 (log2), Sqrt 24, Rsqrt 25, Round_ne 26, Round_ni 27, Round_pi 28, Round_z 29,
// FMax 35, FMin 36, IMax 37, IMin 38, UMax 39, UMin 40, IMul 41, UMul 42, FMad 46, Fma 47, IMad 48, UMad 49, Dot2 54, Dot3 55,
// Dot4 56, IsNaN 8, IsInf 9, IsFinite 10, Discard 82, DerivCoarseX 83, DerivCoarseY 84, DerivFineX 85, DerivFineY 86.
// Быстрая математика: у ER все арифметические инструкции DXIL несут `fast`, !dx.precise нет — интринсики берём в варианте fast_.

llvm::Value *
Converter::CallAirFloatIntrinsic(StringRef Name, ArrayRef<Value *> Args, bool Fast) {
  auto *Ty = Args[0]->getType();
  std::string FnName = std::string(Fast ? "air.fast_" : "air.") + Name.str();
  std::string Suffix = Ty->getScalarType()->isHalfTy() ? "f16" : "f32";
  if (auto *VT = llvm::dyn_cast<llvm::FixedVectorType>(Ty))
    FnName += ".v" + std::to_string(VT->getNumElements()) + Suffix;
  else
    FnName += "." + Suffix;
  SmallVector<Type *> Tys;
  for (auto *A : Args)
    Tys.push_back(A->getType());
  auto &Context = ir.getContext();
  auto Attrs = AttributeList::get(
      Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)}}
  );
  auto Fn = ir.GetInsertBlock()->getModule()->getOrInsertFunction(FnName, FunctionType::get(Ty, Tys, false), Attrs);
  return ir.CreateCall(Fn, Args);
}

llvm::Expected<llvm::Value *>
Converter::EmitUnary(CallInst &Call) {
  auto Op = asU32(Call.getArgOperand(0));
  if (!Op)
    return unsupported("dxil: non-constant unary op");
  auto X = Get(Call.getArgOperand(1));
  if (!X)
    return X.takeError();
  switch (*Op) {
  case 6: return air.CreateFPUnOp(AIRBuilder::fabs, *X);
  case 7: return air.CreateFPUnOp(AIRBuilder::saturate, *X);
  case 12: return air.CreateFPUnOp(AIRBuilder::cos, *X);
  case 13: return air.CreateFPUnOp(AIRBuilder::sin, *X);
  case 14: return CallAirFloatIntrinsic("tan", {*X}, true);
  case 15: return CallAirFloatIntrinsic("acos", {*X}, true);
  case 16: return CallAirFloatIntrinsic("asin", {*X}, true);
  case 17: return CallAirFloatIntrinsic("atan", {*X}, true);
  case 18: return CallAirFloatIntrinsic("cosh", {*X}, true);
  case 19: return CallAirFloatIntrinsic("sinh", {*X}, true);
  case 20: return CallAirFloatIntrinsic("tanh", {*X}, true);
  case 21: return air.CreateFPUnOp(AIRBuilder::exp2, *X);
  case 22: return air.CreateFPUnOp(AIRBuilder::fract, *X);
  case 23: return air.CreateFPUnOp(AIRBuilder::log2, *X);
  case 24: return air.CreateFPUnOp(AIRBuilder::sqrt, *X);
  case 25: return air.CreateFPUnOp(AIRBuilder::rsqrt, *X);
  case 26: return air.CreateFPUnOp(AIRBuilder::rint, *X);
  case 27: return air.CreateFPUnOp(AIRBuilder::floor, *X);
  case 28: return air.CreateFPUnOp(AIRBuilder::ceil, *X);
  case 29: return air.CreateFPUnOp(AIRBuilder::trunc, *X);
  case 83: case 85: return air.CreateDerivative(*X, false);
  case 84: case 86: return air.CreateDerivative(*X, true);
  default:
    return unsupported("dxil: unsupported unary op " + std::to_string(*Op));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitBinary(CallInst &Call) {
  auto Op = asU32(Call.getArgOperand(0));
  if (!Op)
    return unsupported("dxil: non-constant binary op");
  auto A = Get(Call.getArgOperand(1));
  if (!A)
    return A.takeError();
  auto B = Get(Call.getArgOperand(2));
  if (!B)
    return B.takeError();
  switch (*Op) {
  case 35: return air.CreateFPBinOp(AIRBuilder::fmax, *A, *B);
  case 36: return air.CreateFPBinOp(AIRBuilder::fmin, *A, *B);
  case 37: return air.CreateIntBinOp(AIRBuilder::max, *A, *B, true);
  case 38: return air.CreateIntBinOp(AIRBuilder::min, *A, *B, true);
  case 39: return air.CreateIntBinOp(AIRBuilder::max, *A, *B, false);
  case 40: return air.CreateIntBinOp(AIRBuilder::min, *A, *B, false);
  case 41: return air.CreateIntBinOp(AIRBuilder::mul_hi, *A, *B, true);
  case 42: return air.CreateIntBinOp(AIRBuilder::mul_hi, *A, *B, false);
  default:
    return unsupported("dxil: unsupported binary op " + std::to_string(*Op));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitTertiary(CallInst &Call) {
  auto Op = asU32(Call.getArgOperand(0));
  if (!Op)
    return unsupported("dxil: non-constant tertiary op");
  auto A = Get(Call.getArgOperand(1));
  if (!A)
    return A.takeError();
  auto B = Get(Call.getArgOperand(2));
  if (!B)
    return B.takeError();
  auto C = Get(Call.getArgOperand(3));
  if (!C)
    return C.takeError();
  switch (*Op) {
  case 46: case 47: return air.CreateFMA(*A, *B, *C);
  case 48: case 49: return ir.CreateAdd(ir.CreateMul(*A, *B), *C);
  default:
    return unsupported("dxil: unsupported tertiary op " + std::to_string(*Op));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitDot(CallInst &Call, unsigned Dim) {
  SmallVector<Value *, 4> A, B;
  for (unsigned i = 0; i < Dim; i++) {
    auto a = Get(Call.getArgOperand(1 + i));
    if (!a)
      return a.takeError();
    auto b = Get(Call.getArgOperand(1 + Dim + i));
    if (!b)
      return b.takeError();
    A.push_back(*a);
    B.push_back(*b);
  }
  return air.CreateDotProduct(MakeVec(A), MakeVec(B));
}

llvm::Expected<llvm::Value *>
Converter::EmitIsSpecialFloat(CallInst &Call) {
  auto Op = asU32(Call.getArgOperand(0));
  if (!Op)
    return unsupported("dxil: non-constant isSpecialFloat op");
  auto X = Get(Call.getArgOperand(1));
  if (!X)
    return X.takeError();
  switch (*Op) {
  case 8: return air.CreateIsNaN(*X);
  case 9: return ir.CreateFCmpOEQ(air.CreateFPUnOp(AIRBuilder::fabs, *X), llvm::ConstantFP::getInfinity((*X)->getType()));
  case 10: return ir.CreateFCmpOLT(air.CreateFPUnOp(AIRBuilder::fabs, *X), llvm::ConstantFP::getInfinity((*X)->getType()));
  default:
    return unsupported("dxil: unsupported isSpecialFloat op " + std::to_string(*Op));
  }
}

// dx.op.atomicBinOp(i32 78, handle, i32 atomicOp, i32 c0, i32 c1, i32 c2, newValue) → старое значение.
// Коды DXIL AtomicBinOpCode: Add 0, And 1, Or 2, Xor 3, IMin 4, IMax 5, UMin 6, UMax 7, Exchange 8.
llvm::Expected<llvm::Value *>
Converter::EmitAtomicBinOp(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Code = asU32(Call.getArgOperand(2));
  if (!Code)
    return unsupported("dxil: non-constant atomicBinOp code");
  AtomicRMWInst::BinOp Op;
  switch (*Code) {
  case 0: Op = AtomicRMWInst::Add; break;
  case 1: Op = AtomicRMWInst::And; break;
  case 2: Op = AtomicRMWInst::Or; break;
  case 3: Op = AtomicRMWInst::Xor; break;
  case 4: Op = AtomicRMWInst::Min; break;
  case 5: Op = AtomicRMWInst::Max; break;
  case 6: Op = AtomicRMWInst::UMin; break;
  case 7: Op = AtomicRMWInst::UMax; break;
  case 8: Op = AtomicRMWInst::Xchg; break;
  default:
    return unsupported("dxil: unsupported atomicBinOp code " + std::to_string(*Code));
  }
  auto C0 = Get(Call.getArgOperand(3));
  if (!C0)
    return C0.takeError();
  auto NewValue = Get(Call.getArgOperand(6));
  if (!NewValue)
    return NewValue.takeError();
  if (auto Buffer = GetBufferDescriptor(*Handle)) {
    Value *ByteOffset = *C0;
    if (Buffer->StructureStride != 0) {
      auto C1 = Get(Call.getArgOperand(4));
      if (!C1)
        return C1.takeError();
      ByteOffset = ir.CreateAdd(ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride)), *C1);
    }
    auto AS = Buffer->Pointer->getType()->getPointerAddressSpace();
    auto *Ptr = ir.CreatePointerCast(
        ir.CreateGEP(ir.getInt8Ty(), ir.CreatePointerCast(Buffer->Pointer, ir.getInt8PtrTy(AS)), {ByteOffset}),
        ir.getInt32Ty()->getPointerTo(AS)
    );
    return air.CreateAtomicRMW(Op, Ptr, BitcastIfNeeded(*NewValue, ir.getInt32Ty()));
  }
  if (auto Texture = GetTextureDescriptor(*Handle)) {
    auto C1 = Get(Call.getArgOperand(4));
    if (!C1)
      return C1.takeError();
    auto C2 = Get(Call.getArgOperand(5));
    if (!C2)
      return C2.takeError();
    auto Address = BuildRwAddress(Texture->ResourceKindLogical, *C0, *C1, *C2, nullptr, 0, 0, 0);
    if (!Address)
      return Address.takeError();
    auto [Pos, Array, Mip] = *Address;
    auto *Vec4 = MakeVec({BitcastIfNeeded(*NewValue, ir.getInt32Ty()), ir.getInt32(0), ir.getInt32(0), ir.getInt32(0)});
    return air.CreateAtomicRMW(MakeTexture(*Texture), Texture->ResourceHandle, Op, Pos, Vec4, Array);
  }
  return unsupported("dxil: atomicBinOp on a resource without buffer/texture descriptor");
}

llvm::Expected<llvm::Value *>
Converter::EmitDiscard(CallInst &Call) {
  // dx.op.discard(i32 82, i1 cond): немедленный air.discard_fragment под условием (как InstPixelDiscard у DXBC) — семантика demote.
  auto Cond = Get(Call.getArgOperand(1));
  if (!Cond)
    return Cond.takeError();
  auto *Fn = ir.GetInsertBlock()->getParent();
  auto *DiscardBB = llvm::BasicBlock::Create(ir.getContext(), "dxil_discard", Fn);
  auto *ContBB = llvm::BasicBlock::Create(ir.getContext(), "dxil_discard_cont", Fn);
  ir.CreateCondBr(*Cond, DiscardBB, ContBB);
  ir.SetInsertPoint(DiscardBB);
  air.CreateDiscard();
  ir.CreateBr(ContBB);
  ir.SetInsertPoint(ContBB);
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitCreateHandle(CallInst &Call) {
  auto Class = asU32(Call.getArgOperand(1));
  auto Range = asU32(Call.getArgOperand(2));
  if (!Class || !Range)
    return unsupported("dxil: non-constant createHandle arguments");
  auto Index = Get(Call.getArgOperand(3));
  if (!Index)
    return Index.takeError();
  handles_[&Call] = HandleInfo{(ResourceClass)*Class, *Range, *Index};
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitCbufferLoadLegacy(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = ctx.binding.GetConstantBuffer(air, Handle->range_id, Handle->index);
  if (!Descriptor)
    return unsupported("dxil: no constant buffer descriptor for resource");
  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();

  auto *TyResult = MapType(Call.getType());
  auto *TyElement = TyResult->getStructElementType(0);
  auto *PackedType = FixedVectorType::get(TyElement, TyResult->getStructNumElements());
  auto *PackedPtr = ir.CreateBitCast(
      ir.CreateGEP(air.getIntTy(4), Descriptor->Pointer, {*Index}),
      PackedType->getPointerTo(Descriptor->Pointer->getType()->getPointerAddressSpace())
  );
  auto *PackedLoad = ir.CreateLoad(PackedType, PackedPtr);
  llvm::Value *Result = PoisonValue::get(TyResult);
  for (unsigned i = 0; i < PackedType->getNumElements(); i++)
    Result = ir.CreateInsertValue(Result, ir.CreateExtractElement(PackedLoad, (uint64_t)i), i);
  return Result;
}

llvm::Expected<llvm::Value *>
Converter::EmitThreadId(CallInst &Call, ThreadBuiltin Kind) {
  if (Kind == ThreadBuiltin::kLocalLinearId)
    return air.CreateGetLocalLinearID();
  auto component = asU32(Call.getArgOperand(1));
  if (!component || *component > 2)
    return unsupported("dxil: invalid thread id component");
  switch (Kind) {
  case ThreadBuiltin::kGlobalId:
    return air.CreateGetGlobalID(*component);
  case ThreadBuiltin::kGroupId:
    return air.CreateGetGroupID(*component);
  default:
    return air.CreateGetLocalID(*component);
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitBufferStore(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();
  auto Mask = asU32(Call.getArgOperand(8));
  if (!Mask)
    return unsupported("dxil: non-constant bufferStore mask");

  auto Texture = GetTextureDescriptor(*Handle);
  if (Texture) {
    auto *TyElement = GetTextureSampleType(Texture->SampleType);
    auto *VecType = FixedVectorType::get(TyElement, 4);
    Value *Value = PoisonValue::get(VecType);
    for (unsigned i = 0; i < 4; i++) {
      if (!(*Mask & (1u << i)))
        continue;
      auto component = Get(Call.getArgOperand(4 + i));
      if (!component)
        return component.takeError();
      Value = ir.CreateInsertElement(Value, BitcastIfNeeded(*component, TyElement), (uint64_t)i);
    }
    bool Coherent = Texture->GlobalCoherent && SupportsMemoryCoherency();
    air.CreateWrite(
        MakeTexture(*Texture), Texture->ResourceHandle, *Index, nullptr, nullptr, air.getInt(0), Value, Coherent
    );
    return (llvm::Value *)nullptr;
  }

  auto Buffer = GetBufferDescriptor(*Handle);
  if (!Buffer)
    return unsupported("dxil: no buffer descriptor for resource");

  Value *ByteOffset = *Index;
  if (Buffer->StructureStride != 0) {
    auto ElementOffset = Get(Call.getArgOperand(3));
    if (!ElementOffset)
      return ElementOffset.takeError();
    ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
    ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
  }
  auto Base = ir.CreateLShr(ByteOffset, ir.getInt32(2));
  bool Coherent = Buffer->GlobalCoherent && SupportsMemoryCoherency();

  for (unsigned i = 0; i < 4; i++) {
    if (!(*Mask & (1u << i)))
      continue;
    auto Component = Get(Call.getArgOperand(4 + i));
    if (!Component)
      return Component.takeError();
    Value *Word = BitcastIfNeeded(*Component, ir.getInt32Ty());
    auto *Ptr = ir.CreateGEP(ir.getInt32Ty(), Buffer->Pointer, {ir.CreateAdd(Base, ir.getInt32(i))});
    if (Coherent)
      air.CreateDeviceCoherentStore(Word, Ptr);
    else
      ir.CreateStore(Word, Ptr);
  }
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitBufferLoad(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();

  auto Texture = GetTextureDescriptor(*Handle);
  if (Texture) {
    bool Coherent = Texture->GlobalCoherent && SupportsMemoryCoherency();
    auto [Value, Residency] = air.CreateRead(
        MakeTexture(*Texture), Texture->ResourceHandle, *Index, nullptr, nullptr, air.getInt(0), Coherent
    );
    return BuildResRet(Call, Value, Residency);
  }

  auto Buffer = GetBufferDescriptor(*Handle);
  if (!Buffer)
    return unsupported("dxil: no buffer descriptor for resource");

  Value *ByteOffset = *Index;
  if (Buffer->StructureStride != 0) {
    auto ElementOffset = Get(Call.getArgOperand(3));
    if (!ElementOffset)
      return ElementOffset.takeError();
    ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
    ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
  }
  auto Base = ir.CreateLShr(ByteOffset, ir.getInt32(2));
  bool Coherent = Buffer->GlobalCoherent && SupportsMemoryCoherency();

  // MacRunner shader-prewarm 17.10: грузить только слова, которые шейдер извлекает (extractvalue с константным индексом),
  // одной векторной загрузкой; прежде — всегда 4 скалярных слова, даже для .x.
  unsigned Needed = 0;
  bool Known = true;
  for (User *U : Call.users()) {
    auto *EV = dyn_cast<ExtractValueInst>(U);
    if (!EV || EV->getNumIndices() != 1 || EV->getIndices()[0] > 3) { Known = false; break; }
    Needed = std::max(Needed, EV->getIndices()[0] + 1);
  }
  if (!Known || Needed == 0)
    Needed = 4;
  Value *Words = Constant::getNullValue(FixedVectorType::get(ir.getInt32Ty(), 4));
  // MacRunner 17.10 (куратор 14:07, три руки A/B на TBL): MACRUNNER_AIRCONV_BUFLOAD = apple | units; не задано — как прежде.
  // Общее условие обеих рук: align в загрузке не выше настоящего выравнивания смещения: единица 16 Б выровнена на 16 только при
  // stride % 16 == 0 и константном смещении в элементе; <2> на 8 при stride % 8 == 0 и смещении % 8 == 0; иначе — прежний путь (gcd).
  static const char *BufEnv = getenv("MACRUNNER_AIRCONV_BUFLOAD");
  const bool ArmApple = BufEnv && strcmp(BufEnv, "apple") == 0, ArmUnits = BufEnv && strcmp(BufEnv, "units") == 0;
  std::optional<unsigned> ConstOff;
  if (Buffer->StructureStride != 0)
    if (auto C = asU32(Call.getArgOperand(3))) ConstOff = *C;
  const unsigned S = Buffer->StructureStride;
  if ((ArmApple || ArmUnits) && !Coherent && ConstOff && S % 16 == 0 && *ConstOff % 4 == 0) {
    const unsigned O = *ConstOff, AS = Buffer->Pointer->getType()->getPointerAddressSpace();
    auto *I32 = ir.getInt32Ty(); auto *V4 = FixedVectorType::get(I32, 4); auto *V2 = FixedVectorType::get(I32, 2);
    auto UnitPtr = [&](unsigned Unit) {   // &buf[index].unit16[Unit] как GEP по <4 x i32>: index·(S/16) + Unit, без байтов и lshr
      Value *Idx = ir.CreateMul(*Index, ir.getInt32(S / 16));
      if (Unit) Idx = ir.CreateAdd(Idx, ir.getInt32(Unit));
      return ir.CreateGEP(V4, ir.CreatePointerCast(Buffer->Pointer, V4->getPointerTo(AS)), {ir.CreateZExt(Idx, ir.getInt64Ty())});
    };
    auto WordPtr = [&](unsigned Word) {   // &buf[index].word[Word] как GEP по i32: index·(S/4) + Word
      Value *Idx = ir.CreateMul(*Index, ir.getInt32(S / 4));
      if (Word) Idx = ir.CreateAdd(Idx, ir.getInt32(Word));
      return ir.CreateGEP(I32, ir.CreatePointerCast(Buffer->Pointer, I32->getPointerTo(AS)), {ir.CreateZExt(Idx, ir.getInt64Ty())});
    };
    bool Done = false;
    if (ArmUnits) {   // (в) свой минимум: ровно те 16-байтные единицы, которые читаются; извлечь нужные слова; без shufflevector
      const unsigned W0 = O / 4, U0 = W0 / 4, U1 = (W0 + Needed - 1) / 4;
      Value *Units[4] = {nullptr, nullptr, nullptr, nullptr};
      for (unsigned U = U0; U <= U1 && U - U0 < 4; U++) Units[U - U0] = ir.CreateAlignedLoad(V4, UnitPtr(U), Align(16));
      if (Needed == 4 && W0 % 4 == 0) Words = Units[0];
      else
        for (unsigned i = 0; i < Needed; i++) {
          const unsigned W = W0 + i;
          Words = ir.CreateInsertElement(Words, ir.CreateExtractElement(Units[W / 4 - U0], (uint64_t)(W % 4)), (uint64_t)i);
        }
      Done = true;
    } else if (ArmApple) {   // (б) как Apple: полные векторы 4 (единица 16 Б) и 2 (align 8), остальное скалярами
      if (Needed == 4 && O % 16 == 0) { Words = ir.CreateAlignedLoad(V4, UnitPtr(O / 16), Align(16)); Done = true; }
      else if (Needed == 2 && O % 8 == 0 && S % 8 == 0) {
        auto *P2 = ir.CreatePointerCast(WordPtr(O / 4), V2->getPointerTo(AS));
        Value *L2 = ir.CreateAlignedLoad(V2, P2, Align(8));
        for (unsigned i = 0; i < 2; i++) Words = ir.CreateInsertElement(Words, ir.CreateExtractElement(L2, (uint64_t)i), (uint64_t)i);
        Done = true;
      } else {
        for (unsigned i = 0; i < Needed; i++) Words = ir.CreateInsertElement(Words, ir.CreateAlignedLoad(I32, WordPtr(O / 4 + i), Align(4)), (uint64_t)i);
        Done = true;
      }
    }
    if (Done) {
      auto *TyStruct0 = cast<StructType>(MapType(Call.getType()));
      Type *TyElement0 = TyStruct0->getElementType(0);
      if (TyElement0->isFloatingPointTy()) Words = ir.CreateBitCast(Words, FixedVectorType::get(TyElement0, 4));
      return BuildResRet(Call, Words, nullptr);
    }
  }
  if (!Coherent && Needed >= 2) {
    unsigned Alignment = 4;   // как у rawBufferLoad: gcd(16, stride, константное смещение)
    {
      auto Gcd = [](unsigned a, unsigned b) { while (b) { unsigned t = a % b; a = b; b = t; } return a; };
      unsigned Inferred = 16;
      if (Buffer->StructureStride) {
        Inferred = Gcd(Inferred, Buffer->StructureStride);
        if (auto C = asU32(Call.getArgOperand(3))) Inferred = Gcd(Inferred, *C); else Inferred = Gcd(Inferred, 4u);
      } else if (auto C = asU32(Call.getArgOperand(2))) Inferred = Gcd(Inferred, *C);
      else Inferred = 4;
      Alignment = std::max(Alignment, Inferred);
    }
    auto *TyVec = FixedVectorType::get(ir.getInt32Ty(), Needed);
    auto *VecPtr = ir.CreatePointerCast(ir.CreateGEP(ir.getInt32Ty(), Buffer->Pointer, {Base}),
                                        TyVec->getPointerTo(Buffer->Pointer->getType()->getPointerAddressSpace()));
    Value *Loaded = ir.CreateAlignedLoad(TyVec, VecPtr, Align(Alignment));
    if (Needed == 4)
      Words = Loaded;
    else
      for (unsigned i = 0; i < Needed; i++)
        Words = ir.CreateInsertElement(Words, ir.CreateExtractElement(Loaded, (uint64_t)i), (uint64_t)i);
  } else {
    for (unsigned i = 0; i < Needed; i++) {
      auto *Ptr = ir.CreateGEP(ir.getInt32Ty(), Buffer->Pointer, {ir.CreateAdd(Base, ir.getInt32(i))});
      Value *Word = Coherent ? (Value *)air.CreateDeviceCoherentLoad(ir.getInt32Ty(), Ptr)
                             : (Value *)ir.CreateLoad(ir.getInt32Ty(), Ptr);
      Words = ir.CreateInsertElement(Words, Word, (uint64_t)i);
    }
  }

  auto *TyStruct = cast<StructType>(MapType(Call.getType()));
  Type *TyElement = TyStruct->getElementType(0);
  if (TyElement->isFloatingPointTy())
    Words = ir.CreateBitCast(Words, FixedVectorType::get(TyElement, 4));
  return BuildResRet(Call, Words, nullptr);
}

llvm::Expected<llvm::Value *>
Converter::EmitTextureStore(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = GetTextureDescriptor(*Handle);
  if (!Descriptor)
    return unsupported("dxil: no texture descriptor for resource");

  auto C0 = Get(Call.getArgOperand(2));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(3));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(4));
  if (!C2)
    return C2.takeError();

  auto Mask = asU32(Call.getArgOperand(9));
  if (!Mask)
    return unsupported("dxil: non-constant textureStore mask");

  auto Address = BuildRwAddress(Descriptor->ResourceKindLogical, *C0, *C1, *C2, nullptr, 0, 0, 0);
  if (!Address)
    return Address.takeError();

  auto [Position, Slice, Unused] = *Address;
  auto Texture = MakeTexture(*Descriptor);
  auto *TyElement = GetTextureSampleType(Texture.sample_type);
  auto *TyVec = FixedVectorType::get(TyElement, 4);
  Value *Value = PoisonValue::get(TyVec);
  for (unsigned i = 0; i < 4; i++) {
    if (!(*Mask & (1u << i)))
      continue;
    auto Component = Get(Call.getArgOperand(5 + i));
    if (!Component)
      return Component.takeError();
    Value = ir.CreateInsertElement(Value, BitcastIfNeeded(*Component, TyElement), (uint64_t)i);
  }

  bool Coherent = Descriptor->GlobalCoherent && SupportsMemoryCoherency();
  air.CreateWrite(Texture, Descriptor->ResourceHandle, Position, Slice, nullptr, air.getInt(0), Value, Coherent);
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitTextureLoad(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = GetTextureDescriptor(*Handle);
  if (!Descriptor)
    return unsupported("dxil: no texture descriptor for resource");

  auto MipOrSampleIndex = Get(Call.getArgOperand(2));
  if (!MipOrSampleIndex)
    return MipOrSampleIndex.takeError();
  auto C0 = Get(Call.getArgOperand(3));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(4));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(5));
  if (!C2)
    return C2.takeError();

  int32_t O0 = tryGetI32(Call.getArgOperand(6));
  int32_t O1 = tryGetI32(Call.getArgOperand(7));
  int32_t O2 = tryGetI32(Call.getArgOperand(8));

  auto Address = BuildRwAddress(Descriptor->ResourceKindLogical, *C0, *C1, *C2, *MipOrSampleIndex, O0, O1, O2);
  if (!Address)
    return Address.takeError();

  auto [Position, Slice, SampleIndex] = *Address;
  auto Texture = MakeTexture(*Descriptor);
  Value *Level = SampleIndex ? air.getInt(0) : *MipOrSampleIndex;
  bool Coherent = Descriptor->GlobalCoherent && SupportsMemoryCoherency();
  auto [Value, Residency] =
      air.CreateRead(Texture, Descriptor->ResourceHandle, Position, Slice, SampleIndex, Level, Coherent);
  return BuildResRet(Call, Value, Residency);
}

llvm::Expected<llvm::Value *>
Converter::EmitBufferUpdateCounter(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();
  auto Descriptor = ctx.binding.GetUAVCounter(air, Handle->range_id, Handle->index);
  if (!Descriptor)
    return unsupported("dxil: no counter descriptor for resource");
  if (auto Increment = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(2))) {
    auto Amount = Increment->getSExtValue();
    auto *OldValue = air.CreateAtomicRMW(AtomicRMWInst::Add, Descriptor->Pointer, ir.getInt32(Amount));
    if (!OldValue)
      return unsupported("dxil: buffer counter update failed");
    if (Amount < 0)
      return ir.CreateAdd(OldValue, ir.getInt32(Amount));
    return OldValue;
  }
  return unsupported("dxil: non-constant buffer counter increment");
}

llvm::Expected<llvm::Value *>
Converter::EmitGetDimensions(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Level = Get(Call.getArgOperand(2));
  if (!Level)
    return Level.takeError();

  Value *C1 = llvm::UndefValue::get(ir.getInt32Ty());
  Value *C2 = llvm::UndefValue::get(ir.getInt32Ty());
  Value *C3 = llvm::UndefValue::get(ir.getInt32Ty());
  Value *C4 = llvm::UndefValue::get(ir.getInt32Ty());

  if (auto Descriptor = GetTextureDescriptor(*Handle)) {
    auto Texture = MakeTexture(*Descriptor);
    switch (Descriptor->ResourceKindLogical) {
    case Texture::texture_buffer:
      C1 = ir.CreateTrunc(ir.CreateLShr(Descriptor->Metadata, 32uLL), air.getIntTy());
      break;
    case Texture::texture1d:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture1d_array:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::array_length, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture2d:
    case Texture::depth2d:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture2d_array:
    case Texture::depth2d_array:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C3 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::array_length, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture3d:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C3 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::depth, *Level);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texturecube:
    case Texture::depthcube:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texturecube_array:
    case Texture::depthcube_array:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, *Level);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, *Level);
      C3 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::array_length, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_mip_levels, nullptr);
      break;
    case Texture::texture2d_ms:
    case Texture::depth_2d_ms:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, nullptr);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_samples, nullptr);
      break;
    case Texture::texture2d_ms_array:
    case Texture::depth_2d_ms_array:
      C1 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::width, nullptr);
      C2 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::height, nullptr);
      C3 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::array_length, nullptr);
      C4 = air.CreateTextureQuery(Texture, Descriptor->ResourceHandle, Texture::num_samples, nullptr);
      break;
    }
  } else if (auto Buffer = GetBufferDescriptor(*Handle)) {
    C1 = ir.CreateTrunc(Buffer->Metadata, air.getIntTy());
    if (Buffer->StructureStride) {
      C1 = ir.CreateUDiv(C1, ir.getInt32(Buffer->StructureStride));
    }
  } else
    return unsupported("dxil: no buffer descriptor for resource");

  auto *TyStruct = cast<StructType>(MapType(Call.getType()));
  Value *Ret = llvm::PoisonValue::get(TyStruct);
  Ret = ir.CreateInsertValue(Ret, C1, {0u});
  Ret = ir.CreateInsertValue(Ret, C2, {1u});
  Ret = ir.CreateInsertValue(Ret, C3, {2u});
  Ret = ir.CreateInsertValue(Ret, C4, {3u});
  return Ret;
}

llvm::Expected<llvm::Value *>
Converter::EmitRawBufferStore(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();
  auto Mask = asU32(Call.getArgOperand(8));
  if (!Mask)
    return unsupported("dxil: non-constant rawBufferStore mask");

  auto Component0 = Get(Call.getArgOperand(4));
  if (!Component0)
    return Component0.takeError();

  auto Buffer = GetBufferDescriptor(*Handle);
  if (!Buffer)
    return unsupported("dxil: no buffer descriptor for resource");

  Value *ByteOffset = *Index;
  if (Buffer->StructureStride != 0) {
    auto ElementOffset = Get(Call.getArgOperand(3));
    if (!ElementOffset)
      return ElementOffset.takeError();
    ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
    ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
  }
  bool Coherent = Buffer->GlobalCoherent && SupportsMemoryCoherency();
  auto BufferAddrSpace = Buffer->Pointer->getType()->getPointerAddressSpace();
  auto *TyElement = (*Component0)->getType();
  auto Base = ir.CreatePointerCast(
      ir.CreateGEP(
          ir.getInt8Ty(), ir.CreatePointerCast(Buffer->Pointer, ir.getInt8PtrTy(BufferAddrSpace)), {ByteOffset}
      ),
      TyElement->getPointerTo(BufferAddrSpace)
  );

  for (unsigned i = 0; i < 4; i++) {
    if (!(*Mask & (1u << i)))
      continue;
    auto Component = Get(Call.getArgOperand(4 + i));
    if (!Component)
      return Component.takeError();
    auto *Ptr = ir.CreateConstGEP1_32(TyElement, Base, i);
    if (Coherent)
      air.CreateDeviceCoherentStore(*Component, Ptr);
    else
      ir.CreateStore(*Component, Ptr);
  }
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitRawBufferLoad(CallInst &Call) {
  auto Handle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!Handle)
    return Handle.takeError();

  auto Index = Get(Call.getArgOperand(2));
  if (!Index)
    return Index.takeError();

  auto Buffer = GetBufferDescriptor(*Handle);
  if (!Buffer)
    return unsupported("dxil: no buffer descriptor for resource");
  auto Mask = asU32(Call.getArgOperand(4));
  if (!Mask)
    return unsupported("dxil: non-constant rawBufferLoad mask");

  Value *ByteOffset = *Index;
  if (Buffer->StructureStride != 0) {
    auto ElementOffset = Get(Call.getArgOperand(3));
    if (!ElementOffset)
      return ElementOffset.takeError();
    ByteOffset = ir.CreateMul(ByteOffset, ir.getInt32(Buffer->StructureStride));
    ByteOffset = ir.CreateAdd(ByteOffset, *ElementOffset);
  }
  bool Coherent = Buffer->GlobalCoherent && SupportsMemoryCoherency();
  auto *TyStruct = cast<StructType>(MapType(Call.getType()));
  Type *TyElement = TyStruct->getElementType(0);
  auto BufferAddrSpace = Buffer->Pointer->getType()->getPointerAddressSpace();
  // MacRunner 17.10 (гибриды TBL, куратор 14:00): MACRUNNER_AIRCONV_RAWLOAD = gep16 | scalar3 | gep16,scalar3 — опыты формы обращений.
  // gep16: адрес структурного буфера считать в ЕДИНИЦАХ доступа (слово/вектор 16 Б) от индекса элемента, как фронтенд Apple
  // (mul stride/16 → GEP <4 x i32>*), а не байтами с последующим lshr exact 2; scalar3: сплошную маску из 3 компонент грузить
  // скалярами (как Apple), вектор только для 4 и 2. По умолчанию (не задано) — как прежде.
  static const char *RawEnv = getenv("MACRUNNER_AIRCONV_RAWLOAD");
  const bool Gep16 = RawEnv && strstr(RawEnv, "gep16"), Scalar3 = RawEnv && strstr(RawEnv, "scalar3");
  const unsigned ElementBytes0 = std::max(1u, TyElement->getScalarSizeInBits() / 8);
  std::optional<unsigned> ConstElemOff;
  if (Buffer->StructureStride != 0)
    if (auto C = asU32(Call.getArgOperand(3))) ConstElemOff = *C;
  const bool WordAddr = Gep16 && Buffer->StructureStride != 0 && ElementBytes0 == 4 && Buffer->StructureStride % 4 == 0 && ConstElemOff && *ConstElemOff % 4 == 0;
  Value *Base = nullptr;
  if (WordAddr) {   // база = GEP по словам: index·(stride/4) + off/4 — без байтов и lshr
    Value *WordIdx = ir.CreateMul(*Index, ir.getInt32(Buffer->StructureStride / 4));
    if (*ConstElemOff) WordIdx = ir.CreateAdd(WordIdx, ir.getInt32(*ConstElemOff / 4));
    Base = ir.CreateGEP(TyElement, ir.CreatePointerCast(Buffer->Pointer, TyElement->getPointerTo(BufferAddrSpace)), {ir.CreateZExt(WordIdx, ir.getInt64Ty())});
  } else {
    Base = ir.CreatePointerCast(
        ir.CreateGEP(
            ir.getInt8Ty(), ir.CreatePointerCast(Buffer->Pointer, ir.getInt8PtrTy(BufferAddrSpace)), {ByteOffset}
        ),
        TyElement->getPointerTo(BufferAddrSpace)
    );
  }

  Value *Vector = Constant::getNullValue(FixedVectorType::get(TyElement, 4));
  // MacRunner shader-prewarm 17.10: сплошная маска → ОДНА векторная загрузка (как у нашего MSL-пути: load <4 x float>), а не
  // 4 скалярных (TileBasedLighting: 42 скалярных device-load против 8 векторных → own/air 0,67). Выравнивание — из DXIL (операнд 5).
  const unsigned Lo = (unsigned)std::countr_zero(*Mask & 0xFu), N = (unsigned)std::popcount(*Mask & 0xFu);
  const bool Contiguous = N >= 2 && ((*Mask & 0xFu) >> Lo) == ((1u << N) - 1u);
  if (Contiguous && !Coherent && !(Scalar3 && N == 3)) {
    unsigned Alignment = 4;
    if (WordAddr && N == 4 && Lo == 0 && Buffer->StructureStride % 16 == 0 && *ConstElemOff % 16 == 0) {   // gep16: полный вектор по единице 16 Б
      auto *TyVec4 = FixedVectorType::get(TyElement, 4);
      Value *Idx16 = ir.CreateMul(*Index, ir.getInt32(Buffer->StructureStride / 16));
      if (*ConstElemOff) Idx16 = ir.CreateAdd(Idx16, ir.getInt32(*ConstElemOff / 16));
      auto *Ptr16 = ir.CreateGEP(TyVec4, ir.CreatePointerCast(Buffer->Pointer, TyVec4->getPointerTo(BufferAddrSpace)), {ir.CreateZExt(Idx16, ir.getInt64Ty())});
      return BuildResRet(Call, ir.CreateAlignedLoad(TyVec4, Ptr16, Align(16)), nullptr);
    }
    if (Call.arg_size() > 5)
      if (auto A = asU32(Call.getArgOperand(5)); A && *A >= 4 && *A <= 16 && (*A & (*A - 1)) == 0)
        Alignment = *A;
    // 17.10 (TileBasedLighting, own/air 0,68): dxc пишет минимальное выравнивание 4, и `load <4 x float> align 4` бэкенд Metal
    // разбивает; MSL-путь грузит те же float4 с align 16. Начало raw/структурного вида выровнено на 16
    // (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT), смещение = index·stride + константа → выравнивание = gcd(16, stride, константа, Lo·размер).
    {
      auto Gcd = [](unsigned a, unsigned b) { while (b) { unsigned t = a % b; a = b; b = t; } return a; };
      unsigned Inferred = 16;
      if (Buffer->StructureStride) {
        Inferred = Gcd(Inferred, Buffer->StructureStride);
        if (auto C = asU32(Call.getArgOperand(3))) Inferred = Gcd(Inferred, *C);
        else Inferred = Gcd(Inferred, 4u);
      } else if (auto C = asU32(Call.getArgOperand(2))) {
        Inferred = Gcd(Inferred, *C);
      } else {
        Inferred = 4;
      }
      const unsigned ElementBytes = std::max(1u, TyElement->getScalarSizeInBits() / 8);
      if (Lo) Inferred = Gcd(Inferred, Lo * ElementBytes);
      Alignment = std::max(Alignment, std::max(Inferred, ElementBytes));
    }
    auto *TyVec = FixedVectorType::get(TyElement, N);
    auto *VecPtr = ir.CreatePointerCast(ir.CreateConstGEP1_32(TyElement, Base, Lo), TyVec->getPointerTo(BufferAddrSpace));
    Value *Loaded = ir.CreateAlignedLoad(TyVec, VecPtr, Align(Alignment));
    if (Lo == 0 && N == 4)
      return BuildResRet(Call, Loaded, nullptr);
    for (unsigned i = 0; i < N; i++)
      Vector = ir.CreateInsertElement(Vector, ir.CreateExtractElement(Loaded, (uint64_t)i), (uint64_t)(Lo + i));
    return BuildResRet(Call, Vector, nullptr);
  }
  for (unsigned i = 0; i < 4; i++) {
    if (!(*Mask & (1u << i)))
      continue;
    auto *Ptr = ir.CreateConstGEP1_32(TyElement, Base, i);
    Value *Component =
        Coherent ? (Value *)air.CreateDeviceCoherentLoad(TyElement, Ptr) : (Value *)ir.CreateLoad(TyElement, Ptr);
    Vector = ir.CreateInsertElement(Vector, Component, (uint64_t)i);
  }
  return BuildResRet(Call, Vector, nullptr);
}

llvm::Expected<llvm::Value *>
Converter::EmitLegacyF16ToF32(CallInst &Call) {
  auto HalfInI32 = Get(Call.getArgOperand(1));
  if (!HalfInI32)
    return HalfInI32.takeError();

  return air.CreateConvertToFloat(ir.CreateBitCast(ir.CreateTrunc(*HalfInI32, ir.getInt16Ty()), ir.getHalfTy()));
}

llvm::Expected<llvm::Value *>
Converter::EmitLegacyF32ToF16(CallInst &Call) {
  auto Float = Get(Call.getArgOperand(1));
  if (!Float)
    return Float.takeError();

  return ir.CreateZExt(ir.CreateBitCast(air.CreateConvertToHalf(*Float), ir.getInt16Ty()), ir.getInt32Ty());
}

llvm::Expected<llvm::Value *>
Converter::EmitUnaryBits(CallInst &Call) {
  auto Bits = Get(Call.getArgOperand(1));
  if (!Bits)
    return Bits.takeError();

  auto Op = asU32(Call.getArgOperand(0));
  if (!Op)
    return unsupported("dxil: non-constant unaryBits op");

  auto NormalizeOverflow = [this](llvm::Value *Value) {
    llvm::Value *Max = llvm::ConstantInt::getAllOnesValue(Value->getType());
    auto BitWidth = Max->getType()->getIntegerBitWidth();
    return ir.CreateSExtOrTrunc(
        ir.CreateSelect(ir.CreateIsNull(ir.CreateAnd(Value, ~(BitWidth - 1))), Value, Max), ir.getInt32Ty()
    );
  };

  switch (*Op) {
  case 31: // countbits
    return ir.CreateSExtOrTrunc(air.CreateIntUnOp(AIRBuilder::popcount, *Bits), ir.getInt32Ty());
  case 32: // firstbitlow
    return NormalizeOverflow(air.CreateCountZero(*Bits, true));
  case 33: // firstbithigh
    return NormalizeOverflow(air.CreateCountZero(*Bits, false));
  case 34: // firstbithigh_shi
    return NormalizeOverflow(ir.CreateSelect(
        ir.CreateIsNotNeg(*Bits), air.CreateCountZero(*Bits, false),
        ir.CreateAdd(
            air.CreateCountZero(ir.CreateShl(ir.CreateNot(*Bits), 1ull), false),
            llvm::ConstantInt::get((*Bits)->getType(), 1)
        )
    ));
  default:
    return unsupported("dxil: unsupported unaryBits op " + std::to_string(*Op));
  }
}

llvm::Expected<llvm::Value *>
Converter::EmitBarrier(CallInst &Call) {
  auto Mode = asU32(Call.getArgOperand(1));
  if (!Mode)
    return unsupported("dxil: non-constant barrier mode");
  uint32_t Flags = 0;
  if (*Mode & (2 | 4))
    Flags |= (uint32_t)MemFlags::Device | (uint32_t)MemFlags::Texture;
  if (*Mode & 8)
    Flags |= (uint32_t)MemFlags::Threadgroup;
  // UAVFenceGlobal = 2
  // UAVFenceThreadGroup = 4
  // TGSMFence = 8
  // MacRunner 17.10 (куратор 16:54; класс «двойной барьер», §5.40): MACRUNNER_AIRCONV_BARRIER=single — барьер с
  // синхронизацией (бит 1) кодировать ОДНИМ air.wg.barrier(flags, 5, 1), как фронтенд Apple для threadgroup_barrier(mem_flags),
  // без отдельного atomic_fence (у нас было fence + barrier на каждый GroupMemoryBarrierWithGroupSync). Умолчание — как было.
  // Трёхаргументная форма принимается Metal только при air.version >= 2.9 (с 2.7 MTLCompilerService падает:
  // XPC_ERROR_CONNECTION_INTERRUPTED; `xcrun metal -x ir` говорит прямо: «expecting 2.9»). Версию модуля задаёт ключ
  // MACRUNNER_AIRCONV_MODULE_META (ver29); без него ключ барьера бездействует, об этом один раз сообщается в stderr.
  static const bool SingleBarrier = [] {
    const char *E = getenv("MACRUNNER_AIRCONV_BARRIER");
    if (!(E && strcmp(E, "single") == 0))
      return false;
    const char *M = getenv("MACRUNNER_AIRCONV_MODULE_META");
    if (M && strstr(M, "ver29"))
      return true;
    fprintf(stderr, "airconv: MACRUNNER_AIRCONV_BARRIER=single требует MACRUNNER_AIRCONV_MODULE_META с ver29 — ключ барьера выключен\n");
    return false;
  }();
  if (SingleBarrier && (*Mode & 1)) {
    air.CreateBarrierWithMemoryScope((MemFlags)Flags);
    return (llvm::Value *)nullptr;
  }
  if ((*Mode & (2 | 4 | 8)) && SupportsNonExecutionBarrier())
    air.CreateAtomicFence((MemFlags)Flags, (*Mode & 2) ? ThreadScope::Device : ThreadScope::Threadgroup);
  // SyncThreadGroup = 1
  if ((*Mode & 1) || !(SupportsNonExecutionBarrier()))
    air.CreateBarrier((MemFlags)Flags);
  return (llvm::Value *)nullptr;
}

llvm::Expected<llvm::Value *>
Converter::EmitLoadInput(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Row = asU32(Call.getArgOperand(2));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Row || !Component)
    return unsupported("dxil: non-constant loadInput index");
  if (*Element >= entry.inputs.size())
    return unsupported("dxil: loadInput element index out of range");
  const auto &signature = entry.inputs[*Element];
  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: input signature element is not mapped");
  if (!ctx.resource.input.ptr_int4 || !ctx.resource.input.ptr_float4)
    return unsupported("dxil: input register file is not available");
  switch (signature.semantic_kind) {
  case SemanticKind::Position:
  case SemanticKind::VertexID:
  case SemanticKind::InstanceID:
  case SemanticKind::Arbitrary:
  case SemanticKind::IsFrontFace:   // MacRunner shader-prewarm 17.10: системные входы PS лежат в регистрах после InitializeInput
  case SemanticKind::SampleIndex:
  case SemanticKind::PrimitiveID:
    break;
  default:
    return unsupported("dxil: unsupported loadInput semantic kind=" + std::to_string((uint32_t)signature.semantic_kind) + " name=" + signature.name);
  }

  auto *Type = Call.getType();
  uint32_t Reg = (uint32_t)signature.start_row + *Row;
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: loadInput component out of range");

  if (Type->isIntegerTy()) {
    auto *Ptr = ir.CreateGEP(
        ctx.resource.input.ptr_int4->getType()->getNonOpaquePointerElementType(), ctx.resource.input.ptr_int4,
        {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}
    );
    auto *Value = ir.CreateLoad(ir.getInt32Ty(), Ptr);
    if (Type->getIntegerBitWidth() < 32)
      return ir.CreateTrunc(Value, MapType(Type));   // 17.10 (Fork): Type — из контекста модуля DXIL, в целевой модуль только через MapType
    if (Type->getIntegerBitWidth() == 32)
      return Value;
    return unsupported("dxil: unsupported loadInput integer width");
  }
  if (Type->isFloatTy()) {
    auto *Ptr = ir.CreateGEP(
        ctx.resource.input.ptr_float4->getType()->getNonOpaquePointerElementType(), ctx.resource.input.ptr_float4,
        {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}
    );
    return ir.CreateLoad(ir.getFloatTy(), Ptr);
  }
  if (Type->isHalfTy()) {
    if (signature.semantic_kind == SemanticKind::Position) {
      auto *Ptr = ir.CreateGEP(
          ctx.resource.input.ptr_float4->getType()->getNonOpaquePointerElementType(), ctx.resource.input.ptr_float4,
          {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}
      );
      return air.CreateConvertToHalf(ir.CreateLoad(ir.getFloatTy(), Ptr));
    }
    auto *Ptr = ir.CreateGEP(
        ctx.resource.input.ptr_int4->getType()->getNonOpaquePointerElementType(), ctx.resource.input.ptr_int4,
        {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}
    );
    auto *Value = ir.CreateLoad(ir.getInt32Ty(), Ptr);
    return ir.CreateBitCast(ir.CreateTrunc(Value, ir.getInt16Ty()), ir.getHalfTy());
  }
  return unsupported("dxil: unsupported loadInput type");
}

llvm::Expected<llvm::Value *>
Converter::EmitStoreOutput(const CallInst &Call) {
  auto Element = asU32(Call.getArgOperand(1));
  auto Row = asU32(Call.getArgOperand(2));
  auto Component = asU32(Call.getArgOperand(3));
  if (!Element || !Row || !Component)
    return unsupported("dxil: non-constant storeOutput index");
  if (*Element >= entry.outputs.size())
    return unsupported("dxil: storeOutput element index out of range");
  auto Value = Get(Call.getArgOperand(4));
  if (!Value)
    return Value.takeError();
  if (!*Value)
    return unsupported("dxil: missing storeOutput value");
  const auto &signature = entry.outputs[*Element];
  auto *Stored = *Value;
  if (signature.semantic_kind == SemanticKind::Depth || signature.semantic_kind == SemanticKind::DepthLessEqual ||
      signature.semantic_kind == SemanticKind::DepthGreaterEqual) {   // MacRunner shader-prewarm 17.10: SV_Depth → регистр глубины (как oDepth у DXBC)
    if (!ctx.resource.depth_output_reg)
      return unsupported("dxil: depth output register is not available");
    ir.CreateStore(BitcastIfNeeded(Stored, ir.getFloatTy()), ir.CreateConstGEP1_32(ir.getFloatTy(), ctx.resource.depth_output_reg, 0));
    return (llvm::Value *)nullptr;
  }

  switch (signature.semantic_kind) {
  case SemanticKind::Position:
  case SemanticKind::Target:
  case SemanticKind::Arbitrary:
  case SemanticKind::ClipDistance:   // MacRunner shader-prewarm 17.10: SV_ClipDistance/SV_CullDistance лежат в выходных регистрах (как у DXBC)
  case SemanticKind::CullDistance:
    break;
  default:
    return unsupported("dxil: unsupported storeOutput semantic kind=" + std::to_string((uint32_t)signature.semantic_kind) + " name=" + signature.name + " element=" + std::to_string(*Element) + " at " + std::to_string(__LINE__));
  }

  if (signature.start_row < 0 || signature.start_col < 0)
    return unsupported("dxil: output signature element is not mapped");
  if (!ctx.resource.output.ptr_int4 || !ctx.resource.output.ptr_float4)
    return unsupported("dxil: output register file is not available");
  uint32_t Reg = (uint32_t)signature.start_row + *Row;
  uint32_t Lane = (uint32_t)signature.start_col + *Component;
  if (Lane > 3)
    return unsupported("dxil: storeOutput component out of range");

  if (Stored->getType()->isFloatTy()) {
    auto *Ptr = ir.CreateGEP(
        ctx.resource.output.ptr_float4->getType()->getNonOpaquePointerElementType(), ctx.resource.output.ptr_float4,
        {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}
    );
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  if (Stored->getType()->isHalfTy()) {
    if (signature.semantic_kind == SemanticKind::Position) {
      auto *Ptr = ir.CreateGEP(
          ctx.resource.output.ptr_float4->getType()->getNonOpaquePointerElementType(), ctx.resource.output.ptr_float4,
          {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}
      );
      ir.CreateStore(air.CreateConvertToFloat(Stored), Ptr);
      return (llvm::Value *)nullptr;
    }
    Stored = ir.CreateZExt(ir.CreateBitCast(Stored, ir.getInt16Ty()), ir.getInt32Ty());
    auto *Ptr = ir.CreateGEP(
        ctx.resource.output.ptr_int4->getType()->getNonOpaquePointerElementType(), ctx.resource.output.ptr_int4,
        {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}
    );
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  if (Stored->getType()->isIntegerTy()) {
    if (Stored->getType()->getIntegerBitWidth() < 32)
      Stored = ir.CreateZExt(Stored, ir.getInt32Ty());
    else if (Stored->getType()->getIntegerBitWidth() > 32)
      return unsupported("dxil: unsupported storeOutput integer width");
    auto *Ptr = ir.CreateGEP(
        ctx.resource.output.ptr_int4->getType()->getNonOpaquePointerElementType(), ctx.resource.output.ptr_int4,
        {ir.getInt32(0), ir.getInt32(Reg), ir.getInt32(Lane)}
    );
    ir.CreateStore(Stored, Ptr);
    return (llvm::Value *)nullptr;
  }
  return unsupported("dxil: unsupported storeOutput type");
}

llvm::Expected<llvm::Value *>
Converter::EmitSample(const CallInst &Call, StringRef Op) {
  auto TextureHandle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!TextureHandle)
    return TextureHandle.takeError();
  auto TextureDescriptor = GetTextureDescriptor(*TextureHandle);
  if (!TextureDescriptor)
    return unsupported("dxil: no texture descriptor for resource");
  auto SamplerHandle = ResolveHandleOperand(Call.getArgOperand(2));
  if (!SamplerHandle)
    return SamplerHandle.takeError();
  auto SamplerDescriptor = GetSamplerDescriptor(*SamplerHandle);
  if (!SamplerDescriptor)
    return SamplerDescriptor.takeError();
  auto Texture = MakeTexture(*TextureDescriptor);
  auto Sampler = SamplerDescriptor->SamplerHandle;

  auto C0 = Get(Call.getArgOperand(3));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(4));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(5));
  if (!C2)
    return C2.takeError();
  auto C3 = Get(Call.getArgOperand(6));
  if (!C3)
    return C3.takeError();

  const int32_t Offset[3] = {
      tryGetI32(Call.getArgOperand(7)), tryGetI32(Call.getArgOperand(8)), tryGetI32(Call.getArgOperand(9))
  };

  Value *Coord = nullptr;
  Value *Array = nullptr;

  auto ConvertArrayIndexFromFloat = [this](Value *Value) {
    return air.CreateConvertToSigned(air.CreateFPUnOp(AIRBuilder::rint, Value));
  };

  switch (TextureDescriptor->ResourceKindLogical) {
  case Texture::texture1d:
    Coord = MakeVec({*C0, air.getFloat(0)});
    break;
  case Texture::texture1d_array:
    Coord = MakeVec({*C0, air.getFloat(0)});
    Array = ConvertArrayIndexFromFloat(*C1);
    break;
  case Texture::texture2d:
  case Texture::depth2d:
    Coord = MakeVec({*C0, *C1});
    break;
  case Texture::texture2d_array:
  case Texture::depth2d_array:
    Coord = MakeVec({*C0, *C1});
    Array = ConvertArrayIndexFromFloat(*C2);
    break;
  case Texture::texture3d:
    Coord = MakeVec({*C0, *C1, *C2});
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = MakeVec({*C0, *C1, *C2});
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = MakeVec({*C0, *C1, *C2});
    Array = ConvertArrayIndexFromFloat(*C3);
    break;
  default:
    return unsupported(
        "dxil: unsupported texture kind for sampling: " +
        std::to_string(static_cast<uint32_t>(TextureDescriptor->ResourceKindLogical))
    );
  }

  llvm::Value *Value = nullptr, *Residency = nullptr;
  if (Op == "sampleGrad") {   // MacRunner shader-prewarm 17.10: dx.op.sampleGrad(…, c0..c3, o0..o2, ddx0..2 (10–12), ddy0..2 (13–15), clamp (16))
    unsigned n = Coord->getType()->isVectorTy() ? llvm::cast<llvm::FixedVectorType>(Coord->getType())->getNumElements() : 1;
    SmallVector<llvm::Value *, 3> DX, DY;
    for (unsigned i = 0; i < n; i++) {
      auto dx = Get(Call.getArgOperand(10 + i));
      if (!dx)
        return dx.takeError();
      auto dy = Get(Call.getArgOperand(13 + i));
      if (!dy)
        return dy.takeError();
      DX.push_back(*dx && !llvm::isa<llvm::UndefValue>(*dx) ? *dx : air.getFloat(0));
      DY.push_back(*dy && !llvm::isa<llvm::UndefValue>(*dy) ? *dy : air.getFloat(0));
    }
    llvm::Value *MinLOD = air.getFloat(0);
    if (!llvm::isa<llvm::UndefValue>(Call.getArgOperand(16))) {
      auto Clamp = Get(Call.getArgOperand(16));
      if (!Clamp)
        return Clamp.takeError();
      if (*Clamp)
        MinLOD = *Clamp;
    }
    std::tie(Value, Residency) = air.CreateSampleGrad(
        Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, n > 1 ? MakeVec(DX) : DX[0],
        n > 1 ? MakeVec(DY) : DY[0], MinLOD, Offset
    );
  } else if (Op == "sampleLevel") {
    auto Level = Get(Call.getArgOperand(10));
    if (!Level)
      return Level.takeError();
    std::tie(Value, Residency) = air.CreateSample(
        Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, Offset, sample_level{*Level}
    );
  } else if (Op == "sampleBias") {
    auto Bias = Get(Call.getArgOperand(10));
    if (!Bias)
      return Bias.takeError();
    std::tie(Value, Residency) =
        air.CreateSample(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, Offset, sample_bias{*Bias});
  } else if (Op == "sampleCmpLevelZero") {
    auto Reference = Get(Call.getArgOperand(10));
    if (!Reference)
      return Reference.takeError();
    std::tie(Value, Residency) = air.CreateSampleCmp(
        Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, *Reference, Offset,
        sample_level{air.getFloat(0)}
    );
  } else if (Op == "sampleCmp") {
    auto Reference = Get(Call.getArgOperand(10));
    if (!Reference)
      return Reference.takeError();
    auto Clamp = Get(Call.getArgOperand(11));
    if (!Clamp)
      return Clamp.takeError();
    std::tie(Value, Residency) = air.CreateSampleCmp(
        Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, *Reference, Offset,
        sample_bias{air.getFloat(0)}, sample_min_lod_clamp{*Clamp}
    );
  } else {
    std::tie(Value, Residency) =
        air.CreateSample(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, Offset);
  }
  if (!Value)
    return unsupported("dxil: sampling operation failed");
  // Metal depth texture must use float type, resolve potential type mismatch here
  if (Call.getType()->getStructElementType(0)->isHalfTy())
    Value = air.CreateConvertToHalf(Value);
  return BuildResRet(Call, Value, Residency);
}

llvm::Expected<llvm::Value *>
Converter::EmitGather(const CallInst &Call, StringRef Op) {
  auto TextureHandle = ResolveHandleOperand(Call.getArgOperand(1));
  if (!TextureHandle)
    return TextureHandle.takeError();
  auto TextureDescriptor = GetTextureDescriptor(*TextureHandle);
  if (!TextureDescriptor)
    return unsupported("dxil: no texture descriptor for resource");
  auto SamplerHandle = ResolveHandleOperand(Call.getArgOperand(2));
  if (!SamplerHandle)
    return SamplerHandle.takeError();
  auto SamplerDescriptor = GetSamplerDescriptor(*SamplerHandle);
  if (!SamplerDescriptor)
    return SamplerDescriptor.takeError();
  auto Texture = MakeTexture(*TextureDescriptor);
  auto Sampler = SamplerDescriptor->SamplerHandle;

  auto C0 = Get(Call.getArgOperand(3));
  if (!C0)
    return C0.takeError();
  auto C1 = Get(Call.getArgOperand(4));
  if (!C1)
    return C1.takeError();
  auto C2 = Get(Call.getArgOperand(5));
  if (!C2)
    return C2.takeError();
  auto C3 = Get(Call.getArgOperand(6));
  if (!C3)
    return C3.takeError();
  auto O0 = Get(Call.getArgOperand(7));
  if (!O0)
    return O0.takeError();
  auto O1 = Get(Call.getArgOperand(8));
  if (!O1)
    return O1.takeError();
  auto Component = Get(Call.getArgOperand(9));
  if (!Component)
    return Component.takeError();

  Value *Offset = nullptr;
  Value *Coord = nullptr;
  Value *Array = nullptr;

  auto ConvertArrayIndexFromFloat = [this](Value *Value) {
    return air.CreateConvertToSigned(air.CreateFPUnOp(AIRBuilder::rint, Value));
  };

  switch (TextureDescriptor->ResourceKindLogical) {
  case Texture::texture2d:
  case Texture::depth2d:
    Coord = MakeVec({*C0, *C1});
    Offset = MakeVec({*O0, *O1});
    break;
  case Texture::texture2d_array:
  case Texture::depth2d_array:
    Coord = MakeVec({*C0, *C1});
    Array = ConvertArrayIndexFromFloat(*C2);
    Offset = MakeVec({*O0, *O1});
    break;
  case Texture::texturecube:
  case Texture::depthcube:
    Coord = MakeVec({*C0, *C1, *C2});
    Offset = air.getInt2(0, 0);
    break;
  case Texture::texturecube_array:
  case Texture::depthcube_array:
    Coord = MakeVec({*C0, *C1, *C2});
    Array = ConvertArrayIndexFromFloat(*C3);
    Offset = air.getInt2(0, 0);
    break;
  default:
    return unsupported(
        "dxil: unsupported texture kind for gathering: " +
        std::to_string(static_cast<uint32_t>(TextureDescriptor->ResourceKindLogical))
    );
  }

  llvm::Value *Value = nullptr, *Residency = nullptr;
  if (Op == "textureGatherCmp") {
    auto Reference = Get(Call.getArgOperand(10));
    if (!Reference)
      return Reference.takeError();
    std::tie(Value, Residency) =
        air.CreateGatherCompare(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, *Reference, Offset);
  } else {
    std::tie(Value, Residency) =
        air.CreateGather(Texture, TextureDescriptor->ResourceHandle, Sampler, Coord, Array, Offset, *Component);
  }
  if (!Value)
    return unsupported("dxil: gather operation failed");
  // Metal depth texture must use float type, resolve potential type mismatch here
  if (Call.getType()->getStructElementType(0)->isHalfTy())
    Value = air.CreateConvertToHalf(Value);
  return BuildResRet(Call, Value, Residency);
}

} // namespace dxmt::dxil
