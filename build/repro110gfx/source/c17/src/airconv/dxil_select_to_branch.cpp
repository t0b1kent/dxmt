// MacRunner shader-prewarm 17.10.2026 — см. dxil_select_to_branch.hpp.
#include "dxil_select_to_branch.hpp"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <cstdlib>

namespace dxmt::dxil {
using namespace llvm;

bool
SelectToBranchEnabled() {
  static const bool enabled = [] {
    const char *v = std::getenv("MACRUNNER_AIRCONV_SELECT_TO_BRANCH");
    return !(v && v[0] == '0');
  }();
  return enabled;
}

namespace {

// "dx.op.sample.f32" → "sample"; не dx.op → пусто.
StringRef
DxOpName(const CallInst &Call) {
  auto *Callee = Call.getCalledFunction();
  if (!Callee)
    return {};
  StringRef Name = Callee->getName();
  if (!Name.startswith("dx.op."))
    return {};
  Name = Name.drop_front(6);
  size_t Dot = Name.find('.');
  return Dot == StringRef::npos ? Name : Name.take_front(Dot);
}

// Дорогие операции без побочных эффектов: ради них и ставится ветвь.
bool
IsExpensiveLoad(StringRef Op) {
  return Op == "sample" || Op == "sampleLevel" || Op == "sampleBias" || Op == "sampleGrad" || Op == "sampleCmp" ||
         Op == "sampleCmpLevelZero" || Op == "textureLoad" || Op == "bufferLoad" || Op == "rawBufferLoad" ||
         Op == "textureGather" || Op == "textureGatherCmp" || Op == "calculateLOD";
}

// Неявный LOD и явные производные (dx.op.unary 83–86: DerivCoarseX/Y, DerivFineX/Y): в неоднородной ветви не определены
// (D3D и Metal) — переносятся только при условии, однородном в кваде.
bool
IsImplicitLod(const CallInst &Call, StringRef Op) {
  if (Op == "sample" || Op == "sampleBias" || Op == "sampleCmp" || Op == "calculateLOD")
    return true;
  if (Op == "unary")
    if (auto *C = dyn_cast<ConstantInt>(Call.getArgOperand(0)))
      return C->getZExtValue() >= 83 && C->getZExtValue() <= 86;
  return false;
}

// Чистые dx.op, которые можно перенести в ветвь (ни записи памяти, ни побочных эффектов, ни зависимости от положения).
bool
IsMovableDxOp(StringRef Op) {
  if (IsExpensiveLoad(Op))
    return true;
  return Op == "cbufferLoadLegacy" || Op == "cbufferLoad" || Op == "createHandle" || Op == "annotateHandle" ||
         Op == "createHandleFromBinding" || Op == "unary" || Op == "binary" || Op == "tertiary" || Op == "quaternary" ||
         Op == "dot2" || Op == "dot3" || Op == "dot4" || Op == "isSpecialFloat" || Op == "getDimensions" || Op == "loadInput" ||
         Op == "unaryBits" || Op == "legacyF32ToF16" || Op == "legacyF16ToF32" || Op == "bitcastI32toF32" ||
         Op == "bitcastF32toI32" || Op == "bitcastI16toF16" || Op == "bitcastF16toI16";
}

bool
IsMovable(const Instruction &I) {
  if (isa<PHINode>(I) || I.isTerminator() || isa<AllocaInst>(I) || isa<LoadInst>(I) || isa<StoreInst>(I))
    return false;
  if (auto *Call = dyn_cast<CallInst>(&I)) {
    StringRef Op = DxOpName(*Call);
    return !Op.empty() && IsMovableDxOp(Op) && !Call->mayWriteToMemory();
  }
  return !I.mayReadOrWriteMemory() && !I.mayHaveSideEffects();
}

// Однородно ли значение в кваде: только константы, cbuffer и чистая арифметика над ними. PHI, входы, выборки — нет.
bool
IsQuadUniform(Value *V, SmallPtrSetImpl<Value *> &Visited, unsigned Depth) {
  if (isa<Constant>(V))
    return true;
  auto *I = dyn_cast<Instruction>(V);
  if (!I || Depth > 64)
    return false;
  if (!Visited.insert(V).second)
    return true;
  if (isa<PHINode>(I) || (I->mayReadOrWriteMemory() && !isa<CallInst>(I)))
    return false;
  if (auto *Call = dyn_cast<CallInst>(I)) {
    StringRef Op = DxOpName(*Call);
    const bool pure = Op == "cbufferLoadLegacy" || Op == "cbufferLoad" || Op == "createHandle" || Op == "annotateHandle" ||
                      Op == "createHandleFromBinding" || Op == "getDimensions" || Op == "unary" || Op == "binary" ||
                      Op == "tertiary" || Op == "quaternary" || Op == "dot2" || Op == "dot3" || Op == "dot4" ||
                      Op == "isSpecialFloat" || Op == "unaryBits" || Op == "legacyF32ToF16" || Op == "legacyF16ToF32";
    if (!pure)
      return false;
    for (auto &A : Call->args())
      if (!IsQuadUniform(A.get(), Visited, Depth + 1))
        return false;
    return true;
  }
  for (auto &Op : I->operands())
    if (!IsQuadUniform(Op.get(), Visited, Depth + 1))
      return false;
  return true;
}

// Группа select одного блока с одним условием (dxc раскладывает float4 на 4 select с общим условием; выборка
// %dx.types.ResRet имеет 4 пользователя-extractvalue — по одному на select). Сторона переносится целиком: инструкция
// входит в замыкание стороны, если ВСЕ её пользователи — либо уже в замыкании, либо select группы именно этой стороной.
struct Group {
  Value *Cond = nullptr;
  SelectInst *Split = nullptr;                 // точка разреза — один из select группы
  SmallVector<SelectInst *, 4> Sels;
  SmallVector<Instruction *, 32> Side[2];      // true, false — в порядке программы
  bool Expensive[2] = {false, false}, ImplicitLod[2] = {false, false};
};


bool
DefinedBefore(Value *V, Instruction *Point) {
  if (isa<Constant>(V) || isa<Argument>(V))
    return true;
  auto *I = dyn_cast<Instruction>(V);
  return I && (I->getParent() != Point->getParent() || I->comesBefore(Point));
}

void
CollectGroupClosure(Group &G, bool &Unmovable) {
  // Точка разреза — ПОСЛЕДНИЙ select группы (куратор 17.10 09:59): операнды, определённые между select группы, тоже уходят в
  // ветви; сами select группы в замыкание не входят (они станут phi в хвосте).
  SelectInst *Split = G.Split;
  SmallPtrSet<Instruction *, 32> Set[2];
  SmallPtrSet<SelectInst *, 8> InGroup(G.Sels.begin(), G.Sels.end());
  for (Instruction *I = Split->getPrevNode(); I; I = I->getPrevNode()) {
    if (isa<PHINode>(I))
      continue;
    if (auto *AsSel = dyn_cast<SelectInst>(I); AsSel && InGroup.count(AsSel))
      continue;
    for (int side = 0; side < 2; side++) {
      if (I->use_empty())
        break;
      bool all = true;
      for (User *U : I->users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI) { all = false; break; }
        if (Set[side].count(UI))
          continue;
        auto *Sel = dyn_cast<SelectInst>(UI);
        Value *mine = side == 0 ? (Sel ? Sel->getTrueValue() : nullptr) : (Sel ? Sel->getFalseValue() : nullptr);
        Value *other = side == 0 ? (Sel ? Sel->getFalseValue() : nullptr) : (Sel ? Sel->getTrueValue() : nullptr);
        if (Sel && InGroup.count(Sel) && mine == I && other != I && Sel->getCondition() != I)
          continue;
        all = false; break;
      }
      if (!all)
        continue;
      if (!IsMovable(*I)) { Unmovable = true; return; }   // дорогая сторона упёрлась в непереносимое — группу не трогаем
      Set[side].insert(I);
      if (auto *Call = dyn_cast<CallInst>(I)) {
        StringRef Op = DxOpName(*Call);
        G.Expensive[side] |= IsExpensiveLoad(Op);
        G.ImplicitLod[side] |= IsImplicitLod(*Call, Op);
      }
      break;   // инструкция принадлежит одной стороне
    }
  }
  for (auto &I : *Split->getParent()) {
    if (&I == Split) break;
    for (int side = 0; side < 2; side++)
      if (Set[side].count(&I)) G.Side[side].push_back(&I);
  }
}


// Подъём производных (куратор 17.10 11:00): выборка с неявным LOD внутри стороны при неоднородном условии → ddx/ddy координат
// считаются ДО разреза (однородный поток, dx.op.unary 83/84 = DerivCoarseX/Y), сама выборка становится sampleGrad. Координаты
// получают пользователя вне стороны и остаются в голове; замыкание пересчитывается вызывающим. Только dx.op.sample (60):
// sampleBias потребовал бы масштаба градиента 2^bias (не побитово), sampleCmp — формы сравнения с градиентом (SM 6.8 op 254),
// которой у конвертера нет; calculateLOD — по смыслу неразделим с неявным LOD.
unsigned
GradientComponents(const CallInst &Sample, const EntryPoint *Entry) {
  if (!Entry) return 0;
  auto *HandleCall = dyn_cast<CallInst>(Sample.getArgOperand(1));
  if (!HandleCall || DxOpName(*HandleCall) != "createHandle") return 0;
  auto *Class = dyn_cast<ConstantInt>(HandleCall->getArgOperand(1));
  auto *Range = dyn_cast<ConstantInt>(HandleCall->getArgOperand(2));
  if (!Class || !Range || Class->getZExtValue() != (uint64_t)ResourceClass::SRV) return 0;
  for (const auto &R : Entry->resources) {
    if (R.resource_class != ResourceClass::SRV || R.id != Range->getZExtValue()) continue;
    switch (R.kind) {
    case ResourceKind::Texture1D: case ResourceKind::Texture1DArray: return 1;
    case ResourceKind::Texture2D: case ResourceKind::Texture2DArray: return 2;
    case ResourceKind::Texture3D: case ResourceKind::TextureCube: case ResourceKind::TextureCubeArray: return 3;
    default: return 0;
    }
  }
  return 0;
}

bool
RewriteSampleToGrad(CallInst *Sample, Instruction *InsertBefore, const EntryPoint *Entry) {
  const unsigned Comps = GradientComponents(*Sample, Entry);
  if (!Comps || Sample->arg_size() < 11) return false;
  Module *M = Sample->getModule(); LLVMContext &C = M->getContext();
  StringRef Callee = Sample->getCalledFunction()->getName();          // dx.op.sample.f32 → суффикс типа
  StringRef Suffix = Callee.substr(Callee.rfind('.'));                // ".f32" / ".f16"
  Type *FloatTy = Sample->getArgOperand(3)->getType();
  Type *I32 = Type::getInt32Ty(C);
  // dx.op.unary<suffix>(i32 opcode, T) — производные
  FunctionType *UnaryTy = FunctionType::get(FloatTy, {I32, FloatTy}, false);
  FunctionCallee Unary = M->getOrInsertFunction(("dx.op.unary" + Suffix).str(), UnaryTy);
  if (auto *UF = dyn_cast<Function>(Unary.getCallee()); UF && UF->getAttributes().isEmpty()) {
    UF->addFnAttr(Attribute::NoUnwind); UF->addFnAttr(Attribute::ReadNone);
  }
  // dx.op.sampleGrad<suffix>(i32, handle, handle, c0..c3, o0..o2, ddx0..2, ddy0..2, clamp)
  Type *HandleTy = Sample->getArgOperand(1)->getType();
  SmallVector<Type *, 17> Params{I32, HandleTy, HandleTy, FloatTy, FloatTy, FloatTy, FloatTy, I32, I32, I32,
                                 FloatTy, FloatTy, FloatTy, FloatTy, FloatTy, FloatTy, FloatTy};
  FunctionType *GradTy = FunctionType::get(Sample->getType(), Params, false);
  FunctionCallee Grad = M->getOrInsertFunction(("dx.op.sampleGrad" + Suffix).str(), GradTy);
  if (auto *GF = dyn_cast<Function>(Grad.getCallee()); GF && GF->getAttributes().isEmpty())
    GF->setAttributes(Sample->getCalledFunction()->getAttributes());
  IRBuilder<> B(InsertBefore);
  Value *DX[3], *DY[3];
  for (unsigned i = 0; i < 3; i++) {
    if (i < Comps) {
      Value *Coord = Sample->getArgOperand(3 + i);
      // Грубые производные на квад (83/84 DerivCoarseX/Y). Тонкие (85/86) в AIR неотличимы: конвертер переводит оба в один
      // air dfdx/dfdy (у Metal одна форма производной) — проверено 17.10 12:01: библиотеки побайтно равны; выбор «грубые/тонкие» пуст.
      DX[i] = B.CreateCall(Unary, {ConstantInt::get(I32, 83), Coord});
      DY[i] = B.CreateCall(Unary, {ConstantInt::get(I32, 84), Coord});
    } else {
      DX[i] = UndefValue::get(FloatTy); DY[i] = UndefValue::get(FloatTy);
    }
  }
  IRBuilder<> At(Sample);
  SmallVector<Value *, 17> Args{ConstantInt::get(I32, 63), Sample->getArgOperand(1), Sample->getArgOperand(2)};
  for (unsigned i = 3; i <= 9; i++) Args.push_back(Sample->getArgOperand(i));
  for (unsigned i = 0; i < 3; i++) Args.push_back(DX[i]);
  for (unsigned i = 0; i < 3; i++) Args.push_back(DY[i]);
  Args.push_back(Sample->getArgOperand(10));                           // clamp
  CallInst *NewCall = At.CreateCall(Grad, Args);
  NewCall->setAttributes(Sample->getAttributes());
  Sample->replaceAllUsesWith(NewCall);
  Sample->eraseFromParent();
  return true;
}

} // namespace

SelectToBranchStats
SelectToBranch(Function &F, const EntryPoint *Entry) {
  SelectToBranchStats S;
  if (!SelectToBranchEnabled())
    return S;
  // Группы собираются один раз по исходным блокам; после разрезания select группы остаются вместе в хвосте (порядок сохранён).
  SmallVector<Group, 16> Groups;
  for (auto &BB : F) {
    MapVector<Value *, unsigned> Index;
    for (auto &I : BB)
      if (auto *Sel = dyn_cast<SelectInst>(&I))
        if (Sel->getCondition()->getType()->isIntegerTy(1)) {
          auto It = Index.find(Sel->getCondition());
          if (It == Index.end()) { Index[Sel->getCondition()] = Groups.size(); Groups.push_back(Group{Sel->getCondition(), nullptr, {Sel}}); }
          else Groups[It->second].Sels.push_back(Sel);
        }
  }
  for (Group &G : Groups) {
    S.candidates += G.Sels.size();
    // Точка разреза выбирается среди select группы так, чтобы перевести как можно больше: select S годится при разрезе P, если его
    // операнды определены до P (иначе ветви их не видят) и все его пользователи в блоке стоят после P (иначе phi в хвосте их не
    // доминирует); select, которым пользуется другой select группы, не годится (phi не доминирует ветви). Куратор 17.10 09:59.
    SmallPtrSet<SelectInst *, 8> InGroup(G.Sels.begin(), G.Sels.end());
    SmallVector<SelectInst *, 4> Best; SelectInst *BestSplit = nullptr;
    for (SelectInst *P : G.Sels) {
      SmallVector<SelectInst *, 4> Kept;
      for (SelectInst *Sel : G.Sels) {
        if (Sel->getParent() != P->getParent() || !DefinedBefore(Sel->getTrueValue(), P) || !DefinedBefore(Sel->getFalseValue(), P)) continue;
        bool ok = true;
        for (User *U : Sel->users()) {
          auto *UI = dyn_cast<Instruction>(U);
          if (!UI || (isa<SelectInst>(UI) && InGroup.count(cast<SelectInst>(UI))) ||
              (UI->getParent() == P->getParent() && UI != Sel && !P->comesBefore(UI))) { ok = false; break; }
          if (UI == Sel) { ok = false; break; }
        }
        if (ok) Kept.push_back(Sel);
      }
      if (Kept.size() > Best.size()) { Best = Kept; BestSplit = P; }
    }
    S.skipped_order += G.Sels.size() - Best.size();
    G.Sels = Best; G.Split = BestSplit;
    if (G.Sels.empty()) continue;
    bool Unmovable = false;
    CollectGroupClosure(G, Unmovable);
    if (Unmovable) { S.skipped_unmovable++; continue; }
    if (!G.Expensive[0] && !G.Expensive[1]) { S.skipped_cheap += G.Sels.size(); continue; }
    bool ViaGrad = false;
    if (G.ImplicitLod[0] || G.ImplicitLod[1]) {
      SmallPtrSet<Value *, 32> Visited;
      if (!IsQuadUniform(G.Cond, Visited, 0)) {
        // Подъём производных: все неявные выборки сторон должны быть dx.op.sample с известным видом текстуры.
        SmallVector<CallInst *, 8> Implicit;
        bool Rewritable = true;
        for (int side = 0; side < 2; side++)
          for (Instruction *I : G.Side[side])
            if (auto *Call = dyn_cast<CallInst>(I); Call && IsImplicitLod(*Call, DxOpName(*Call))) {
              if (DxOpName(*Call) != "sample" || !GradientComponents(*Call, Entry)) Rewritable = false;
              Implicit.push_back(Call);
            }
        if (!Rewritable) { S.skipped_implicit_lod += G.Sels.size(); continue; }
        for (CallInst *Call : Implicit)
          if (RewriteSampleToGrad(Call, G.Split, Entry)) S.grad_rewrites++;
        G.Side[0].clear(); G.Side[1].clear(); G.Expensive[0] = G.Expensive[1] = G.ImplicitLod[0] = G.ImplicitLod[1] = false;
        CollectGroupClosure(G, Unmovable);                              // координаты теперь с пользователем в голове — остаются
        if (Unmovable || (!G.Expensive[0] && !G.Expensive[1]) || G.ImplicitLod[0] || G.ImplicitLod[1]) { S.skipped_implicit_lod += G.Sels.size(); continue; }
        ViaGrad = true;
      }
    }
    Instruction *ThenTerm = nullptr, *ElseTerm = nullptr;
    SelectInst *Split = G.Split;
    SplitBlockAndInsertIfThenElse(G.Cond, Split, &ThenTerm, &ElseTerm);
    for (Instruction *I : G.Side[0]) I->moveBefore(ThenTerm);
    for (Instruction *I : G.Side[1]) I->moveBefore(ElseTerm);
    BasicBlock *Tail = Split->getParent();
    for (SelectInst *Sel : G.Sels) {
      PHINode *Phi = PHINode::Create(Sel->getType(), 2, "", &Tail->front());
      Phi->addIncoming(Sel->getTrueValue(), ThenTerm->getParent());
      Phi->addIncoming(Sel->getFalseValue(), ElseTerm->getParent());
      Sel->replaceAllUsesWith(Phi);
      Sel->eraseFromParent();
      S.converted++;
      if (ViaGrad) S.converted_via_grad++;
    }
    S.moved_instructions += G.Side[0].size() + G.Side[1].size();
  }
  if (const char *t = std::getenv("MACRUNNER_AIRCONV_SELECT_TO_BRANCH_TRACE"); t && t[0] == '1')
    errs() << "select_to_branch function=" << F.getName() << " candidates=" << S.candidates << " converted=" << S.converted
           << " moved=" << S.moved_instructions << " skipped_implicit_lod=" << S.skipped_implicit_lod
           << " skipped_cheap=" << S.skipped_cheap << " skipped_unmovable=" << S.skipped_unmovable << " skipped_order=" << S.skipped_order << " grad_rewrites=" << S.grad_rewrites << " converted_via_grad=" << S.converted_via_grad << "\n";
  return S;
}

} // namespace dxmt::dxil
