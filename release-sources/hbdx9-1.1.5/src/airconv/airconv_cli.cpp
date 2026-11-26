// Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "airconv_context.hpp"
#include "airconv_public.h"
#include "DXBCParser/BlobContainer.h"
#include "dxbc_root_signature.hpp"
#include "llvm/Support/JSON.h"
#include "metallib_writer.hpp"
#include <time.h>
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include <cstdlib>
#include <memory>
#include <vector>
#include <algorithm>
#include <chrono>
#include <system_error>

#ifdef __WIN32
#include "d3dcompiler.h"
#endif
#include "dxbc_converter.hpp"

using namespace llvm;

static cl::opt<std::string>
  InputFilename(cl::Positional, cl::desc("<input dxbc>"), cl::init("-"));

static cl::opt<std::string> OutputFilename(
  "o", cl::desc("Override output filename"), cl::value_desc("filename")
);

static cl::opt<std::string> RootSignatureFilename(
  "root-signature", cl::desc("DXBC container containing an RTS0 root signature"), cl::value_desc("filename")
);
// MacRunner dxil-tess 17.10.2026: корень второй ступени пары (VS в object-функции VS+HS / VS+GS), как SM50_SHADER_ROOT_SIGNATURE2 у слоя
// MacRunner 17.10 (стенд VS): раскладка входа IA как JSON — {"slot_mask":1,"index_format":1,"elements":[{"reg":0,"slot":0,"offset":0,"format":30,"step_function":0,"step_rate":0},…]}
static cl::opt<std::string> IALayoutFilename("ia-layout", cl::desc("JSON с раскладкой входа (SM50_SHADER_IA_INPUT_LAYOUT) для VS"), cl::value_desc("filename"));
static cl::opt<std::string> RootSignature2Filename(
  "root-signature-vs", cl::desc("DXBC container with the RTS0 root signature of the vertex shader of a pair"), cl::value_desc("filename")
);
static cl::opt<SM50_SHADER_METAL_VERSION> MetalVersion(
  "metal-version", cl::desc("Target Metal version (default: 320)"), cl::init(SM50_SHADER_METAL_320),
  cl::values(clEnumValN(SM50_SHADER_METAL_310, "310", "Metal 3.1"),
             clEnumValN(SM50_SHADER_METAL_320, "320", "Metal 3.2"))
);

static cl::opt<std::string>
  HullBeforeDomain("hull-before-domain", cl::desc("Compile domain shader with supplied hull shader"));

static cl::opt<std::string>
  VertexBeforeHull("vertex-before-hull", cl::desc("Compile hull shader with supplied vertex shader"));

static cl::opt<std::string>
  HullAfterVertex("hull-after-vertex", cl::desc("Compile vertex shader with supplied hull shader"));

static cl::opt<std::string>
  VertexBeforeGeometry("vertex-before-geometry", cl::desc("Compile geometry shader with supplied vertex shader"));

static cl::opt<std::string>
  GeometryAfterVertex("geometry-after-vertex", cl::desc("Compile vertex shader with supplied geometry shader"));

// MacRunner dxil-tess 17.10.2026: --repeat N — повторить перевод+оптимизацию N раз (свежий Module каждый раз),
// напечатать в stderr медиану/мин/p90 мс по повторам со 2-го (первый — прогрев). Цена выпуска AIR без старта процесса.
static cl::opt<unsigned> Repeat("repeat", cl::init(1), cl::desc("Repeat conversion N times and report timing"));
static cl::opt<bool>
  EmitLLVM("S", cl::init(false), cl::desc("Write output as LLVM assembly"));

static cl::opt<bool>
  EmitMetallib("A", cl::init(false), cl::desc("Write output as .metallib"));

static cl::opt<bool> DisassembleDXBC(
  "disas-dxbc", cl::init(false), cl::desc("Disassemble dxbc shader")
);

static cl::opt<bool>
  OptLevelO0("O0", cl::desc("Optimization level 0. Similar to clang -O0. "));

static cl::opt<bool>
  OptLevelO1("O1", cl::desc("Optimization level 1. Similar to clang -O1. "));

static cl::opt<bool>
  OptLevelO2("O2", cl::desc("Optimization level 2. Similar to clang -O2. "));

static cl::opt<bool> PreserveBitcodeUseListOrder(
  "preserve-bc-uselistorder",
  cl::desc("Preserve use-list order when writing LLVM bitcode."),
  cl::init(false), cl::Hidden
);

static cl::opt<bool> PreserveAssemblyUseListOrder(
  "preserve-ll-uselistorder",
  cl::desc("Preserve use-list order when writing LLVM assembly."),
  cl::init(false), cl::Hidden
);

cl::list<std::string> f("f", cl::Prefix, cl::Hidden);

namespace {

struct LLVMDisDiagnosticHandler : public DiagnosticHandler {
  char *Prefix;
  LLVMDisDiagnosticHandler(char *PrefixPtr) : Prefix(PrefixPtr) {}
  bool handleDiagnostics(const DiagnosticInfo &DI) override {
    raw_ostream &OS = errs();
    OS << Prefix << ": ";
    switch (DI.getSeverity()) {
    case DS_Error:
      WithColor::error(OS);
      break;
    case DS_Warning:
      WithColor::warning(OS);
      break;
    case DS_Remark:
      OS << "remark: ";
      break;
    case DS_Note:
      WithColor::note(OS);
      break;
    }

    DiagnosticPrinterRawOStream DP(OS);
    DI.print(DP);
    OS << '\n';

    if (DI.getSeverity() == DS_Error)
      exit(1);
    return true;
  }
};
} // namespace

namespace dxmt::dxbc {
llvm::Error convertDXBC(
  sm50_shader_t pShader, const char *name, llvm::LLVMContext &context,
  llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);
}

static ExitOnError ExitOnErr;

static bool isRootSignatureContainer(StringRef Bytes) {
  using namespace microsoft;
  if (Bytes.size() < sizeof(DXBCHeader) || Bytes.size() > UINT32_MAX)
    return false;
  auto Read32 = [&](size_t Offset) { return support::endian::read32le(Bytes.data() + Offset); };
  uint32_t Count = Read32(offsetof(DXBCHeader, BlobCount));
  if (Count > (Bytes.size() - sizeof(DXBCHeader)) / sizeof(uint32_t))
    return false;
  // Bound the index and every chunk before invoking the existing container parser.
  for (size_t I = 0; I < Count; ++I) {
    size_t Offset = Read32(sizeof(DXBCHeader) + I * sizeof(uint32_t));
    if (Offset > Bytes.size() || Bytes.size() - Offset < sizeof(DXBCBlobHeader) ||
        Read32(Offset + offsetof(DXBCBlobHeader, BlobSize)) > Bytes.size() - Offset - sizeof(DXBCBlobHeader))
      return false;
  }
  CDXBCParser Parser;
  if (FAILED(Parser.ReadDXBC(Bytes.data(), static_cast<uint32_t>(Bytes.size()))))
    return false;
  auto Index = Parser.FindNextMatchingBlob(DXBC_RootSignature);
  if (Index == DXBC_BLOB_NOT_FOUND || Parser.GetBlobSize(Index) < sizeof(dxmt::RawRootSignatureDesc))
    return false;
  auto Version = support::endian::read32le(Parser.GetBlob(Index));
  return Version == D3D_ROOT_SIGNATURE_VERSION_1_0 || Version == D3D_ROOT_SIGNATURE_VERSION_1_1;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  ExitOnErr.setBanner(std::string(argv[0]) + ": error: ");

  // MacRunner 17.10 (куратор 17:50): свежий LLVMContext на КАЖДУЮ итерацию --repeat — как в слое (свой контекст на каждый
  // SM50Compile*); иначе повтор в одном контексте переименовывал тип (%argument_buffer_struct → .16) и менял байты выхода.
  auto NewContext = [&]() { auto C = std::make_unique<LLVMContext>();
  C->setDiagnosticHandler(
    std::make_unique<LLVMDisDiagnosticHandler>(argv[0])
  );
  C->setOpaquePointers(false);
    return C; };
  std::unique_ptr<LLVMContext> ContextPtr = NewContext();
  cl::ParseCommandLineOptions(argc, argv, "DXBC to Metal AIR transpiler\n");

  // bool FastMath = true;
  // for (StringRef Flag : f) {
  //   if (Flag == "no-fast-math") {
  //     FastMath = false;
  //   }
  // }

  if (OutputFilename.empty()) { // Unspecified output, infer it.
    if (InputFilename == "-") {
      OutputFilename = "-";
    } else {
      StringRef IFN = InputFilename;
      OutputFilename = (IFN.endswith(".cso")    ? IFN.drop_back(4)
                        : IFN.endswith(".fxc")  ? IFN.drop_back(4)
                        : IFN.endswith(".obj")  ? IFN.drop_back(4)
                        : IFN.endswith(".o")    ? IFN.drop_back(2)
                        : IFN.endswith(".dxbc") ? IFN.drop_back(5)
                                                : IFN)
                         .str();
      OutputFilename += DisassembleDXBC ? ".txt"
                        : EmitMetallib  ? ".metallib"
                        : EmitLLVM      ? ".ll"
                                        : ".air";
    }
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
    MemoryBuffer::getFileOrSTDIN(InputFilename, /*IsText=*/false);
  if (std::error_code EC = FileOrErr.getError()) {
    SMDiagnostic(
      InputFilename, SourceMgr::DK_Error,
      "Could not open input file: " + EC.message()
    )
      .print(argv[0], errs());
    return 1;
  }
  auto MemRef = FileOrErr->get()->getMemBufferRef();

  if (DisassembleDXBC) {
#ifdef __WIN32
    std::error_code EC;
    std::unique_ptr<ToolOutputFile> Out(
      new ToolOutputFile(OutputFilename, EC, sys::fs::OF_TextWithCRLF)
    );
    if (EC) {
      errs() << EC.message() << '\n';
      return 1;
    }

    ID3DBlob *blob;
    D3DDisassemble(
      MemRef.getBufferStart(), MemRef.getBufferSize(), 0, nullptr, &blob
    );

    Out->os().write(
      (const char *)blob->GetBufferPointer(), blob->GetBufferSize() - 1
    );
    // Declare success.
    Out->keep();

    blob->Release();
    return 0;
#else
    errs() << "Disassemble only supported on Windows" << '\n';
    return 1;
#endif
  }

  std::unique_ptr<MemoryBuffer> RootSignatureBuffer;
  if (RootSignatureFilename.getNumOccurrences()) {
    auto RootFileOrErr = MemoryBuffer::getFile(RootSignatureFilename, /*IsText=*/false);
    if (std::error_code EC = RootFileOrErr.getError()) {
      SMDiagnostic(RootSignatureFilename, SourceMgr::DK_Error,
                   "Could not open root-signature file: " + EC.message()).print(argv[0], errs());
      return 1;
    }
    RootSignatureBuffer = std::move(*RootFileOrErr);
    if (!isRootSignatureContainer(RootSignatureBuffer->getBuffer())) {
      SMDiagnostic(RootSignatureFilename, SourceMgr::DK_Error,
                   "Malformed or unsupported root-signature DXBC container (expected RTS0 version 1.0 or 1.1)")
        .print(argv[0], errs());
      return 1;
    }
  }

  std::unique_ptr<MemoryBuffer> RootSignature2Buffer;
  if (RootSignature2Filename.getNumOccurrences()) {
    auto RootFileOrErr = MemoryBuffer::getFile(RootSignature2Filename, /*IsText=*/false);
    if (std::error_code EC = RootFileOrErr.getError()) {
      SMDiagnostic(RootSignature2Filename, SourceMgr::DK_Error,
                   "Could not open root-signature-vs file: " + EC.message()).print(argv[0], errs());
      return 1;
    }
    RootSignature2Buffer = std::move(*RootFileOrErr);
    if (!isRootSignatureContainer(RootSignature2Buffer->getBuffer())) {
      SMDiagnostic(RootSignature2Filename, SourceMgr::DK_Error,
                   "Malformed or unsupported root-signature-vs DXBC container (expected RTS0 version 1.0 or 1.1)")
        .print(argv[0], errs());
      return 1;
    }
  }

  std::unique_ptr<Module> ModulePtr;
  std::vector<double> RepeatMs;
  // MacRunner 17.10 (куратор 17:18: профиль перевода): фазы по процессорному времени ПОТОКА (не зависит от чужой нагрузки):
  // init = SM50Initialize (контейнер, разбор DXIL-битокода, отражение), conv = convertDXBC (построение AIR-модуля),
  // opt = runOptimizationPasses, link = linkMSAD/SamplePos/Tessellation, write = запись выхода. Печать: airconv-phases (медианы).
  auto ThreadCpuMs = [] { struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; };
  std::vector<double> PhInit, PhConv, PhOpt, PhLink;
  for (unsigned Iteration = 0; Iteration < std::max(1u, Repeat.getValue()); ++Iteration) {
  if (Iteration > 0) { ModulePtr.reset(); ContextPtr = NewContext(); }
  LLVMContext &Context = *ContextPtr;
  auto RepeatStart = std::chrono::steady_clock::now();
  double PhT0 = ThreadCpuMs();
  ModulePtr = std::make_unique<Module>("default", Context);
  Module &M = *ModulePtr;
  dxmt::initializeModule(M);

  sm50_shader_t sm50;
  sm50_error_t err;
  if (SM50Initialize(
        MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50, nullptr, &err
      )) {
    errs() << SM50GetErrorMessageString(err) << '\n';
    SM50FreeError(err);
    return 1;
  }

  double PhT1 = ThreadCpuMs(); PhInit.push_back(PhT1 - PhT0);
  SM50_SHADER_COMMON_DATA data;
  data.metal_version = MetalVersion;
  data.flags = {};
  data.next = 0;
  data.type = SM50_SHADER_COMMON;

  SM50_SHADER_ROOT_SIGNATURE_DATA RootSignature{};
  auto *Args = reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&data);
  if (RootSignatureBuffer) {
    RootSignature.next = Args;
    RootSignature.type = SM50_SHADER_ROOT_SIGNATURE;
    RootSignature.bytecode = RootSignatureBuffer->getBufferStart();
    RootSignature.bytecode_length = RootSignatureBuffer->getBufferSize();
    Args = reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&RootSignature);
  }
  SM50_SHADER_IA_INPUT_LAYOUT_DATA IALayout{};
  std::vector<SM50_IA_INPUT_ELEMENT> IAElements;
  if (IALayoutFilename.getNumOccurrences()) {
    auto LayoutOrErr = MemoryBuffer::getFile(IALayoutFilename, /*IsText=*/true);
    if (!LayoutOrErr) { errs() << "ia-layout: cannot read " << IALayoutFilename << '\n'; return 1; }
    auto JsonOrErr = llvm::json::parse((*LayoutOrErr)->getBuffer());
    if (!JsonOrErr) { errs() << "ia-layout: bad JSON\n"; return 1; }
    auto *Obj = JsonOrErr->getAsObject();
    if (!Obj) { errs() << "ia-layout: object expected\n"; return 1; }
    IALayout.next = Args; IALayout.type = SM50_SHADER_IA_INPUT_LAYOUT;
    IALayout.slot_mask = (uint32_t)Obj->getInteger("slot_mask").value_or(1);
    IALayout.index_buffer_format = (SM50_INDEX_BUFFER_FORMAT)Obj->getInteger("index_format").value_or(0);
    if (auto *Arr = Obj->getArray("elements"))
      for (auto &V : *Arr) {
        auto *E = V.getAsObject(); if (!E) continue;
        SM50_IA_INPUT_ELEMENT El{};
        El.reg = (uint32_t)E->getInteger("reg").value_or(0); El.slot = (uint32_t)E->getInteger("slot").value_or(0);
        El.aligned_byte_offset = (uint32_t)E->getInteger("offset").value_or(0); El.format = (uint32_t)E->getInteger("format").value_or(31);
        El.step_function = (uint32_t)E->getInteger("step_function").value_or(0); El.step_rate = (uint32_t)E->getInteger("step_rate").value_or(0);
        IAElements.push_back(El);
      }
    IALayout.num_elements = (uint32_t)IAElements.size(); IALayout.elements = IAElements.data();
    Args = reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&IALayout);
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA RootSignature2{};
  if (RootSignature2Buffer) {
    RootSignature2.next = Args;
    RootSignature2.type = SM50_SHADER_ROOT_SIGNATURE2;
    RootSignature2.bytecode = RootSignature2Buffer->getBufferStart();
    RootSignature2.bytecode_length = RootSignature2Buffer->getBufferSize();
    Args = reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&RootSignature2);
  }

  if (!HullBeforeDomain.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(HullBeforeDomain, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        HullBeforeDomain, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_hull;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_hull, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    if (auto err = dxmt::dxbc::convert_dxbc_tesselator_domain_shader(
          (dxmt::dxbc::SM50ShaderInternal *)sm50, "shader_main",
          (dxmt::dxbc::SM50ShaderInternal *)sm50_hull, Context, M, Args
        )) {
      errs() << err << '\n';
      return 1;
    }
  } else if (!VertexBeforeHull.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(VertexBeforeHull, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        VertexBeforeHull, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_vertex;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_vertex, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    if (auto err = dxmt::dxbc::convert_dxbc_vertex_hull_shader(
          (dxmt::dxbc::SM50ShaderInternal *)sm50_vertex, (dxmt::dxbc::SM50ShaderInternal *)sm50,
          "shader_main", Context, M, Args
        )) {
      errs() << err << '\n';
      return 1;
    }
  } else if (!HullAfterVertex.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(HullAfterVertex, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        HullAfterVertex, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_hull;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_hull, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    if (auto err = dxmt::dxbc::convert_dxbc_vertex_hull_shader(
          (dxmt::dxbc::SM50ShaderInternal *)sm50, (dxmt::dxbc::SM50ShaderInternal *)sm50_hull,
          "shader_main", Context, M, Args
        )) {
      errs() << err << '\n';
      return 1;
    }
  } else if (!VertexBeforeGeometry.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(VertexBeforeGeometry, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        VertexBeforeGeometry, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_vertex;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_vertex, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    if (auto err = dxmt::dxbc::convert_dxbc_geometry_shader(
          (dxmt::dxbc::SM50ShaderInternal *)sm50, "shader_main",
          (dxmt::dxbc::SM50ShaderInternal *)sm50_vertex, Context, M, Args
        )) {
      errs() << err << '\n';
      return 1;
    }
  } else if (!GeometryAfterVertex.getValue().empty()) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
      MemoryBuffer::getFile(GeometryAfterVertex, /*IsText=*/false);
    if (std::error_code EC = FileOrErr.getError()) {
      SMDiagnostic(
        GeometryAfterVertex, SourceMgr::DK_Error,
        "Could not open input file: " + EC.message()
      )
        .print(argv[0], errs());
      return 1;
    }
    auto MemRef = FileOrErr->get()->getMemBufferRef();
    sm50_shader_t sm50_geometry;
    if (SM50Initialize(
          MemRef.getBufferStart(), MemRef.getBufferSize(), &sm50_geometry, nullptr,
          &err
        )) {
      errs() << SM50GetErrorMessageString(err) << '\n';
      SM50FreeError(err);
      return 1;
    }
    if (auto err = dxmt::dxbc::convert_dxbc_vertex_for_geometry_shader(
          (dxmt::dxbc::SM50ShaderInternal *)sm50, "shader_main",
          (dxmt::dxbc::SM50ShaderInternal *)sm50_geometry, Context, M, Args
        )) {
      errs() << err << '\n';
      return 1;
    }
  } else {
    if (auto err =
          dxmt::dxbc::convertDXBC(sm50, "shader_main", Context, M, Args)) {
      errs() << err << '\n';
      return 1;
    }
  }

  double PhT2 = ThreadCpuMs(); PhConv.push_back(PhT2 - PhT1);
  // MacRunner dxil-tess 17.10.2026: AIRCONV_VERIFY=1 — проверка IR до оптимизатора (ошибки построения иначе
  // проявляются зависанием SROA или падением в LLVM); AIRCONV_NO_OPT=1 — без оптимизатора (для чтения IR через -S).
  if (getenv("AIRCONV_VERIFY") && llvm::verifyModule(M, &errs())) {
    errs() << "airconv: IR verification failed\n";
    return 1;
  }
  if (OptLevelO0 || getenv("AIRCONV_NO_OPT")) {
    // do nothing
  } else {
    dxmt::runOptimizationPasses(M);
  }

  double PhT3 = ThreadCpuMs(); PhOpt.push_back(PhT3 - PhT2);
  dxmt::linkMSAD(M);
  dxmt::linkSamplePos(M);
  dxmt::linkTessellation(M);
  PhLink.push_back(ThreadCpuMs() - PhT3);
  RepeatMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - RepeatStart).count());
  }
  Module &M = *ModulePtr;
  if (Repeat.getValue() > 1) {
    std::vector<double> Sorted(RepeatMs.begin() + 1, RepeatMs.end());
    std::sort(Sorted.begin(), Sorted.end());
    errs() << "airconv-repeat: n=" << Sorted.size() << " median_ms=" << Sorted[Sorted.size() / 2] << " min_ms=" << Sorted.front()
           << " p90_ms=" << Sorted[(Sorted.size() * 9) / 10] << " first_ms=" << RepeatMs.front() << '\n';
  }

  std::error_code EC;
  std::unique_ptr<ToolOutputFile> Out(new ToolOutputFile(
    OutputFilename, EC,
    (EmitLLVM || EmitMetallib) ? sys::fs::OF_TextWithCRLF : sys::fs::OF_None
  ));
  if (EC) {
    errs() << EC.message() << '\n';
    return 1;
  }

  double PhWrite0 = ThreadCpuMs();
  if (EmitLLVM) {
    M.print(Out->os(), nullptr, PreserveAssemblyUseListOrder);
  } else if (EmitMetallib) {
    dxmt::metallib::MetallibWriter writer;
    writer.Write(M, Out->os());
  } else {
    WriteBitcodeToFile(
      M, Out->os(), PreserveBitcodeUseListOrder, nullptr, true
    );
  }

  if (getenv("MACRUNNER_AIRCONV_PHASE_TRACE")) {
    auto Med = [](std::vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    errs() << "airconv-phases: n=" << PhInit.size() << " init_ms=" << Med(PhInit) << " conv_ms=" << Med(PhConv) << " opt_ms=" << Med(PhOpt)
           << " link_ms=" << Med(PhLink) << " write_ms=" << (ThreadCpuMs() - PhWrite0) << " first_total_ms=" << (RepeatMs.empty() ? 0.0 : RepeatMs.front()) << '\n';
  }
  // Declare success.
  Out->keep();

  return 0;
}
