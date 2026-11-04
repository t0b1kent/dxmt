#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "llvm/Support/raw_ostream.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/IR/PassTimingInfo.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO/DeadArgumentElimination.h"
#include "llvm/Transforms/IPO/GlobalOpt.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/IPO/Annotation2Metadata.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/InferFunctionAttrs.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include "airconv_context.hpp"
#include <cstring>
#include <cstdlib>
#include "llvm/IR/Instructions.h"

#include "air_msad.h"
#include "air_samplepos.h"
#include "air_tessellation.h"

#include "transforms/lower_16bit_texread.hpp"
#include "transforms/simdgroup_implicit_membarrier.hpp"
#include "transforms/uniform_div.hpp"

using namespace llvm;

namespace dxmt {

void initializeModule(llvm::Module &M) {
  M.setSourceFileName("airconv_generated.metal");
  M.setTargetTriple("air64-apple-macosx14.0.0");
  M.setDataLayout(
    "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:"
    "64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-"
    "v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:"
    "16:32"
  );
  M.setSDKVersion(VersionTuple(15, 0));
  M.addModuleFlag(Module::ModFlagBehavior::Error, "wchar_size", 4);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "frame-pointer", 2);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_device_buffers", 31);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_constant_buffers", 31);
  M.addModuleFlag(
    Module::ModFlagBehavior::Max, "air.max_threadgroup_buffers", 31
  );
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_textures", 128);
  M.addModuleFlag(
    Module::ModFlagBehavior::Max, "air.max_read_write_textures", 8
  );
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_samplers", 16);

};

static std::atomic_flag llvm_overwrite = false;

void applyPostMeta(llvm::Module &M);
void
runOptimizationPasses(llvm::Module &M) {

  if (!llvm_overwrite.test_and_set()) {
    auto Map = cl::getRegisteredOptions();
    auto InfiniteLoopThreshold = Map["instcombine-infinite-loop-threshold"];
    if (InfiniteLoopThreshold) {
      reinterpret_cast<cl::opt<unsigned> *>(InfiniteLoopThreshold)->setValue(1000);
    }
  }

  // Following optimization passes are picked from default LLVM pipelines
  // An unoptimized shader can make PSO compilation take a very long time
  // But a full optimization pipeline is also too heavy
  // So we choose some passes that really matter to run

  // Create the analysis managers.
  // These must be declared in this order so that they are destroyed in the
  // correct order due to inter-analysis-manager references.
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Create the new pass manager builder.
  // Take a look at the PassBuilder constructor parameters for more
  // customization, e.g. specifying a TargetMachine or various debugging
  // options.
  llvm::PassInstrumentationCallbacks PIC;
  // MacRunner 17.10 (профиль перевода): MACRUNNER_AIRCONV_TIME_PASSES=1 — время по проходам LLVM (отчёт в stderr при разрушении)
  static const bool TimePasses = getenv("MACRUNNER_AIRCONV_TIME_PASSES") != nullptr;
  std::unique_ptr<llvm::TimePassesHandler> TPH;
  if (TimePasses) { TPH = std::make_unique<llvm::TimePassesHandler>(true); TPH->setOutStream(llvm::errs()); TPH->registerCallbacks(PIC); }
  // MACRUNNER_AIRCONV_PASS_CHANGE_TRACE=1 (профиль, уровень 4): после каждого прохода — изменил ли он IR (хеш текста модуля до/после;
  // дорого, только для переписи холостых проходов). Строки stderr: pass-change <имя> <0|1>.
  static const bool ChangeTrace = getenv("MACRUNNER_AIRCONV_PASS_CHANGE_TRACE") != nullptr;
  std::string ChangeBefore;
  auto ModuleText = [&M]() { std::string S; llvm::raw_string_ostream OS(S); M.print(OS, nullptr); return S; };
  if (ChangeTrace) {
    PIC.registerBeforeNonSkippedPassCallback([&](StringRef, llvm::Any) { ChangeBefore = ModuleText(); });
    PIC.registerAfterPassCallback([&](StringRef Name, llvm::Any, const PreservedAnalyses &) {
      bool changed = ModuleText() != ChangeBefore;
      llvm::errs() << "pass-change " << Name << ' ' << (changed ? 1 : 0) << '\n';
    });
  }
  PassBuilder PB(nullptr, PipelineTuningOptions(), {}, &PIC);

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // llvm::StandardInstrumentations SI(true);
  // SI.registerCallbacks(PIC, &FAM);

  ModulePassManager MPM;

  // Convert @llvm.global.annotations to !annotation metadata.
  MPM.addPass(Annotation2MetadataPass());

  // Force any function attributes we want the rest of the pipeline to observe.
  MPM.addPass(ForceFunctionAttrsPass());
  // Do basic inference of function attributes from known properties of system
  // libraries and other oracles.
  MPM.addPass(InferFunctionAttrsPass());

  {
    // Create an early function pass manager to cleanup the output of the
    // frontend.
    FunctionPassManager EarlyFPM;
    // Lower llvm.expect to metadata before attempting transforms.
    // Compare/branch metadata may alter the behavior of passes like SimplifyCFG.
    EarlyFPM.addPass(LowerExpectIntrinsicPass());
    EarlyFPM.addPass(SimplifyCFGPass());
    EarlyFPM.addPass(SROAPass());
    EarlyFPM.addPass(EarlyCSEPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(EarlyFPM), true /* ? */));
  }

  // MacRunner 17.10 (профиль перевода, §5.44): MACRUNNER_AIRCONV_PIPELINE_DROP=<список через запятую> — снять проход, который
  // на наших шейдерах IR не меняет (перепись PASS_CHANGE_TRACE): ic3 — последний InstCombine в LatePM (0/60), ipsccp — IPSCCP
  // (2/60 при 0,48 мс/контейнер). По умолчанию ничего не снимается.
  static const std::string DropList = getenv("MACRUNNER_AIRCONV_PIPELINE_DROP") ? getenv("MACRUNNER_AIRCONV_PIPELINE_DROP") : "";
  auto Drop = [](const char *name) { return DropList.find(name) != std::string::npos; };
  MPM.addPass(AlwaysInlinerPass());
  if (!Drop("ipsccp")) MPM.addPass(IPSCCPPass());
  MPM.addPass(GlobalOptPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
  MPM.addPass(DeadArgumentEliminationPass());

  {
    // Create a small function pass pipeline to cleanup after all the global
    // optimizations.
    FunctionPassManager GlobalCleanupPM;
    GlobalCleanupPM.addPass(InstCombinePass());

    GlobalCleanupPM.addPass(SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));

    GlobalCleanupPM.addPass(air::Lower16BitTexReadPass());
    GlobalCleanupPM.addPass(air::SimdgroupImplicitMemBarrierPass());

    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(GlobalCleanupPM), true /* ? */));
  }

  {
    // MacRunner shader-prewarm 17.10: вынос инвариантов из циклов и GVN. Без них дескрипторы/константы перечитывались из
    // argument buffer на каждой итерации (EarlyCSE сбрасывает поколение памяти на блоке с несколькими предшественниками —
    // заголовок цикла): LensSimu2_BlurCoC — 11 из 20 load внутри 8-кратного цикла, own/air 0,67. После барьеров/lower-проходов,
    // чтобы не переносить загрузки через вставленные барьеры.
    FunctionPassManager LatePM;
    LatePM.addPass(createFunctionToLoopPassAdaptor(
        LICMPass(PipelineTuningOptions().LicmMssaOptCap, PipelineTuningOptions().LicmMssaNoAccForPromotionCap, /*AllowSpeculation=*/true),
        /*UseMemorySSA=*/true, /*UseBlockFrequencyInfo=*/false
    ));
    LatePM.addPass(GVNPass());
    // MacRunner 17.10 (класс «цикл», RESULT.md §5.31): в нашем конвейере не было разворота счётных циклов — на PS ER с циклом
    // по константе 8 рука air была 1,42–1,54 от own, после полного разворота (LLVM 15 loop-unroll, порог 2000) — 0,89–0,97.
    // MACRUNNER_AIRCONV_UNROLL=0 выключает; =<порог> задаёт порог полного разворота (по умолчанию 2000); частичный/runtime — нет.
    {
      static const char *UnrollEnv = getenv("MACRUNNER_AIRCONV_UNROLL");
      // Умолчание выпуска (владелец, 17.10 15:26): разворот ВКЛЮЧЁН. MACRUNNER_AIRCONV_UNROLL: не задано или 1 — порог 2000;
      // 0 — выключить (прежний путь, побайтно тот же); N > 1 — явный порог. Предел итераций — MACRUNNER_AIRCONV_UNROLL_MAXCOUNT (13).
      static const int UnrollThreshold = !UnrollEnv ? 2000 : (atoi(UnrollEnv) == 1 ? 2000 : atoi(UnrollEnv));
      if (UnrollThreshold > 0) {
        static bool ThresholdSet = false;
        if (!ThresholdSet) {
          ThresholdSet = true;
          auto &Opts = llvm::cl::getRegisteredOptions();
          if (auto It = Opts.find("unroll-threshold"); It != Opts.end()) It->second->addOccurrence(0, "unroll-threshold", std::to_string(UnrollThreshold));
          if (auto It = Opts.find("unroll-max-count"); It != Opts.end()) It->second->addOccurrence(0, "unroll-max-count", "64");
        }
        // Предел числа итераций полного разворота (RESULT.md §5.31.8–§5.31.9): в наборе ER границы 4/7/8/13 — выигрыш или ноль,
        // 16 — ±, 24 — проигрыш 2,1×; по умолчанию разворачиваются циклы с границей ≤ 13 (MACRUNNER_AIRCONV_UNROLL_MAXCOUNT=<N>).
        static const char *MaxEnv = getenv("MACRUNNER_AIRCONV_UNROLL_MAXCOUNT");
        static const unsigned UnrollMaxCount = MaxEnv ? (unsigned)atoi(MaxEnv) : 13u;
        LatePM.addPass(LoopUnrollPass(LoopUnrollOptions(3).setPartial(false).setRuntime(false).setPeeling(false).setUpperBound(true).setFullUnrollMaxCount(UnrollMaxCount)));
        // Опыт (куратор 15:06): частичный/runtime-разворот циклов ПО ДАННЫМ ×N с остатком — MACRUNNER_AIRCONV_UNROLL_RUNTIME=<N> (0/не задано — нет).
        static const char *RtEnv = getenv("MACRUNNER_AIRCONV_UNROLL_RUNTIME");
        static const int RtCount = RtEnv ? atoi(RtEnv) : 0;
        if (RtCount > 1) {
          static bool RtSet = false;
          if (!RtSet) {
            RtSet = true;
            auto &Opts = llvm::cl::getRegisteredOptions();
            if (auto It = Opts.find("unroll-count"); It != Opts.end()) It->second->addOccurrence(0, "unroll-count", std::to_string(RtCount));
            if (auto It = Opts.find("unroll-runtime"); It != Opts.end()) It->second->addOccurrence(0, "unroll-runtime", "true");
            if (auto It = Opts.find("unroll-allow-partial"); It != Opts.end()) It->second->addOccurrence(0, "unroll-allow-partial", "true");
          }
          LatePM.addPass(LoopUnrollPass(LoopUnrollOptions(3).setPartial(true).setRuntime(true).setPeeling(false).setUpperBound(false)));
          LatePM.addPass(InstCombinePass());
          LatePM.addPass(SimplifyCFGPass());
        }
        LatePM.addPass(InstCombinePass());
      }
    }
    if (!Drop("ic3")) LatePM.addPass(InstCombinePass());
    LatePM.addPass(SimplifyCFGPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(LatePM), true));
  }

  // MPM.addPass(VerifierPass());

  // Optimize the IR!
  uniform_div::prepareConstants(M, true);
  MPM.run(M, MAM);
  uniform_div::run(M);
  applyPostMeta(M);
}

// MacRunner 17.10 (куратор 13:03/13:12, V6/V7 для TBL): опыты ПОСЛЕ оптимизации, по MACRUNNER_AIRCONV_MODULE_META
// (тот же список, что в setup_module_meta): convert — uitofp/fptoui/sitofp/fptosi → air.convert.* (как фронтенд Apple);
// invariant — !invariant.load на загрузках из addrspace(2) (константы/аргументный буфер неизменны в шейдере);
// bufattrs — атрибуты параметра аргументного буфера как у Apple: nocapture noundef readonly align 8 "air-buffer-no-alias".
void applyPostMeta(llvm::Module &M) {
  // HKGPU 04.11.2026: MODULE_META safe/safeall — setup_module_meta marked a module whose DXBC does not allow refactoring:
  // arcp+contract+afn on its floating-point instructions (no nnan/ninf/nsz/reassoc), after optimisation, as on the tape.
  if (auto *marker = M.getNamedMetadata("macrunner.safe_fp_flags")) {
    M.eraseNamedMetadata(marker);
    static const bool trace = getenv("MACRUNNER_AIRCONV_MODULE_META_TRACE") != nullptr;
    unsigned flagged = 0;
    for (auto &F : M)
      for (auto &BB : F)
        for (auto &I : BB) {
          const unsigned op = I.getOpcode();
          if (op == Instruction::FAdd || op == Instruction::FSub || op == Instruction::FMul || op == Instruction::FDiv ||
              op == Instruction::FRem || op == Instruction::FNeg || op == Instruction::FCmp) {
            I.setHasAllowReciprocal(true); I.setHasAllowContract(true); I.setHasApproxFunc(true); flagged++;
          }
        }
    if (trace) fprintf(stderr, "module_meta safe_fp_flags instructions=%u\n", flagged);
  }
  static const char *env = getenv("MACRUNNER_AIRCONV_MODULE_META");
  if (!env || !*env || strcmp(env, "off") == 0 || strcmp(env, "auto") == 0) return;
  std::string e(env);
  auto has = [&](const char *t) { return (("," + e + ",").find(std::string(",") + t + ",") != std::string::npos); };
  bool convert = has("convert"), invariant = has("invariant"), bufattrs = has("bufattrs");
  if (!convert && !invariant && !bufattrs) return;
  auto &C = M.getContext();
  auto tyName = [](Type *T) -> std::string {
    if (auto *VT = dyn_cast<FixedVectorType>(T)) return "v" + std::to_string(VT->getNumElements()) + (VT->getElementType()->isFloatTy() ? "f32" : VT->getElementType()->isHalfTy() ? "f16" : "i32");
    return T->isFloatTy() ? "f32" : T->isHalfTy() ? "f16" : "i32";
  };
  for (auto &F : M) {
    if (F.isDeclaration()) continue;
    if (bufattrs)
      for (auto &A : F.args())
        if (auto *PT = dyn_cast<PointerType>(A.getType()); PT && PT->getAddressSpace() == 2 && !A.hasAttribute(Attribute::ReadOnly)) {
          A.addAttr(Attribute::NoCapture); A.addAttr(Attribute::NoUndef); A.addAttr(Attribute::ReadOnly);
          A.addAttr(Attribute::getWithAlignment(C, Align(8))); A.addAttr(Attribute::get(C, "air-buffer-no-alias"));
        }
    SmallVector<Instruction *> dead;
    for (auto &BB : F)
      for (auto &I : BB) {
        if (invariant)
          if (auto *L = dyn_cast<LoadInst>(&I); L && L->getPointerAddressSpace() == 2 && !L->getMetadata(LLVMContext::MD_invariant_load))
            L->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(C, {}));
        if (convert && (isa<UIToFPInst>(I) || isa<FPToUIInst>(I) || isa<SIToFPInst>(I) || isa<FPToSIInst>(I))) {
          auto *Src = I.getOperand(0); Type *ST = Src->getType(), *DT = I.getType();
          bool srcInt = ST->getScalarType()->isIntegerTy(32), dstInt = DT->getScalarType()->isIntegerTy(32);
          bool srcF = ST->getScalarType()->isFloatTy(), dstF = DT->getScalarType()->isFloatTy();
          if (!((srcInt && dstF) || (srcF && dstInt))) continue;
          bool toFloat = dstF; bool sgn = isa<SIToFPInst>(I) || isa<FPToSIInst>(I);
          std::string name = std::string("air.convert.") + (toFloat ? "f." : (sgn ? "s." : "u.")) + tyName(DT) + "." + (toFloat ? (sgn ? "s." : "u.") : "f.") + tyName(ST);
          auto Attrs = AttributeList::get(C, {{~0U, Attribute::get(C, Attribute::ReadNone)}, {~0U, Attribute::get(C, Attribute::NoUnwind)}, {~0U, Attribute::get(C, Attribute::WillReturn)}});
          auto Fn = M.getOrInsertFunction(name, FunctionType::get(DT, {ST}, false), Attrs);
          IRBuilder<> B(&I); auto *Call = B.CreateCall(Fn, {Src});
          if (toFloat && isa<FPMathOperator>(Call)) Call->setFast(true);
          I.replaceAllUsesWith(Call); dead.push_back(&I);
        }
      }
    for (auto *I : dead) I->eraseFromParent();
  }
}

void
removeNamedMetadata(llvm::Module &M, StringRef Name) {
  if (auto MD = M.getNamedMetadata(Name)) {
    M.eraseNamedMetadata(MD);
  }
}

// MacRunner 17.10 (куратор 17:18, профиль перевода: фаза link = 4,1 мс из 7,3 на контейнер — 49 %): MACRUNNER_AIRCONV_LAZY_LINK=1 —
// встроенные библиотеки (msad, sample_pos, тесселяция) разбираются и линкуются ТОЛЬКО если модуль объявляет хотя бы одну их функцию.
// Имена определяемых функций каждой библиотеки снимаются один раз на процесс (разбор во временном контексте). При LinkOnlyNeeded
// линковка без единой нужной функции ничего в модуль не вносит — пропуск даёт тот же байткод (проверка: cmp на 522 + кеш игры).
static const bool LazyLink = getenv("MACRUNNER_AIRCONV_LAZY_LINK") != nullptr;
struct LibraryNames { std::vector<std::string> functions; std::vector<std::string> mdKinds; };
static LibraryNames definedFunctionNames(const unsigned char *bitcode, size_t N) {
  LibraryNames names;
  llvm::LLVMContext tmp;
  auto buffer = MemoryBuffer::getMemBufferCopy(StringRef((const char *)bitcode, N));
  auto modOrErr = parseBitcodeFile(buffer->getMemBufferRef(), tmp);
  if (!modOrErr) { consumeError(modOrErr.takeError()); return names; }
  for (auto &F : **modOrErr)
    if (!F.isDeclaration())
      names.functions.push_back(F.getName().str());
  // разбор библиотеки регистрирует в контексте её виды метаданных (напр. "heapallocsite"); писатель битокода выводит таблицу видов
  // контекста целиком — чтобы выход при пропуске был байт в байт тем же, виды регистрируем и без линковки.
  SmallVector<StringRef, 64> kinds; tmp.getMDKindNames(kinds);
  for (auto k : kinds) names.mdKinds.push_back(k.str());
  return names;
}
static bool moduleNeedsAny(llvm::Module &M, const std::vector<std::string> &names) {
  for (auto &n : names) {
    auto *F = M.getFunction(n);
    if (F && F->isDeclaration())
      return true;
  }
  return false;
}

template<size_t N>
void linkShader(llvm::Module &M, const unsigned char (&bitcode)[N]) {
  if (LazyLink) {
    // таблица имён на библиотеку: ключ — адрес массива (три библиотеки, три статических записи)
    static std::mutex NamesMutex;
    static std::map<const unsigned char *, LibraryNames> Names;
    const LibraryNames *names = nullptr;
    {
      std::lock_guard<std::mutex> lock(NamesMutex);
      auto it = Names.find(bitcode);
      if (it == Names.end())
        it = Names.emplace(bitcode, definedFunctionNames(bitcode, N)).first;
      names = &it->second;
    }
    if (!moduleNeedsAny(M, names->functions)) {
      for (auto &k : names->mdKinds) M.getContext().getMDKindID(k);
      return;
    }
  }
  auto buffer = MemoryBuffer::getMemBufferCopy(StringRef((const char *)bitcode, N));
  Expected<std::unique_ptr<Module>> modOrErr = parseBitcodeFile(buffer->getMemBufferRef(), M.getContext());

  if (!modOrErr) {
    // not expected to see this unless something really bad happened in compile time
    errs() << "Failed to parse air bitcode\n";
    return;
  }

  auto module = std::move(modOrErr.get());

  removeNamedMetadata(*module, "air.compile_options");
  removeNamedMetadata(*module, "air.version");
  removeNamedMetadata(*module, "air.language_version");
  removeNamedMetadata(*module, "air.source_file_name");
  removeNamedMetadata(*module, "llvm.ident");
  removeNamedMetadata(*module, "llvm.module.flags");

  llvm::Linker::linkModules(M, std::move(module), Linker::LinkOnlyNeeded);
};

void
linkMSAD(llvm::Module &M) {
  linkShader(M, air_msad);
}

void
linkSamplePos(llvm::Module &M) {
  linkShader(M, air_samplepos);
}

void
linkTessellation(llvm::Module &M) {
  linkShader(M, air_tessellation);
}

} // namespace dxmt
