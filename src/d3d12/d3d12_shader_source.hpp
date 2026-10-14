// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

// Shader-source seam. A pipeline can take ready Metal code for its stages from
// a provider instead of translating SM5.0 bytecode through airconv (the legacy
// path, which stays the default).
//
//   MACRUNNER_DX12_SHADER_PATH=<provider>   read once per process; unset = legacy path only
//
// Every provider answers one request (shader container, serialized root
// signature, stage) with one response (Metal library, entry name, what the
// function needs from the layer). A bridge that translates on demand is one
// more row in the provider table; pipelines do not change.
//
// The choice is made per pipeline. When a provider has no translation of a
// stage, the whole pipeline is built by the legacy path and one stderr line
// names the missing shader. A translation that exists but cannot be used fails
// the pipeline: an error of our own shader is never hidden behind the legacy
// path.
//
// A translated function follows the binding convention of the legacy path, so
// the draw side does not know which path built the pipeline: root arguments in
// buffer slot 0, static samplers in slot 1, the vertex buffer table in slot 2,
// 32-byte descriptors in the tables. Vertex functions pull their inputs
// themselves; which table entry, offset, format and step an input uses comes
// from the pipeline's input layout through four Metal function constants per
// input location (4 * location + 0..3: entry, byte offset, MTLAttributeFormat,
// step rate with 0xffffffff for per-vertex data).
//
// Pipeline state reaches the stages the way the legacy path passes it to its
// translator. The vertex stage gets the input layout (above). The pixel stage
// gets what SM50_SHADER_PSO_PIXEL_SHADER carries:
//   sample mask           a mask other than all ones selects the variant "sm":
//                         the function has a coverage output and takes the mask
//                         as function constant 128
//   dual-source blending  selects the variant "ds": SV_Target1 is the second
//                         source of colour attachment 0
//   target formats        the legacy translator retypes an output to the class
//                         of its target format; a ready function cannot be
//                         retyped, so a class other than the declared one
//                         (description key `colors`) fails the pipeline
// Whether alpha-to-coverage survives (the function writes coverage itself)
// comes from the description key `coverage_output`.
//
// Providers:
//   own   ready libraries of our DXIL -> Metal translator in
//         MACRUNNER_DX12_OWN_SHADER_DIR, found by the SHA-1 of the shader
//         container and, when the function reads root arguments, the SHA-1 of
//         the RTS0 part of the pipeline's root signature:
//           <shader>.<root>.<ps|vs|gs|hs|ds|cs>[.<variant>].metallib   reads root arguments
//           <shader>.<stage>[.<variant>].metallib                      reads none
//           the same name with .desc                       text, see ParseDescription
//         MACRUNNER_DX12_OWN_SHADER_REQUEST_DIR (optional): every missing
//         translation leaves its inputs there for the offline translator:
//           <shader>.<stage>.dxbc, <root>.rts0, and a line in requests.txt
//           (stage, shader, root and, for a pixel variant, variant); one request
//           per process for the same stage, shader, root and variant
//
// Kind of the answer (MACRUNNER_DX12_OWN_SHADER_KIND, read once per process):
//   library   (default, also when unset) the ready Metal library files above, as always
//   source    MSL text: <shader>.<root>.<stage>[.<variant>].msl (or without root) next to the same
//             .desc, which then carries
//                 kind=source          the answer is text; a .desc without it is a broken translation
//                 language=<M>.<m>     MSL language version the text is built with (3.1)
//             The layer builds the library itself with that language version and every other compile
//             option at its default (the translator's text is written for it; the system default moves with
//             the OS), once per process for the same text and language. A build error fails the stage with
//             the Metal error text on stderr (dx12_shader_source_compile_failed).
//
// Two opt-in keys for running an application (d3d12_shader_source_stats.hpp):
//   MACRUNNER_DX12_OWN_FALLBACK=1        a provider failure rebuilds the pipeline by the
//                                        legacy path, with one reason line, instead of failing it
//   MACRUNNER_DX12_OWN_STATS=<seconds>   cumulative counters by path and reason

#include "d3d12_device.hpp"
#include "d3d12_shader_source_stats.hpp"
#include "DXBCParser/DXBCUtils.h"
#include "sha1/sha1_util.hpp"
#include "thread.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt::shader_source {

struct Request {
  D3D12_SHADER_BYTECODE shader; // container exactly as the application passed it
  const void *root;             // RTS0 part of the pipeline's root signature; null without one
  size_t root_size;
  uint32_t stage;               // D3D10_SB_TOKENIZED_PROGRAM_TYPE: ps 0, vs 1, gs 2, hs 3, ds 4, cs 5
  char key[41];                 // SHA-1 of the container, lower-case hex
  char root_key[41];            // SHA-1 of the RTS0 part; empty without a root signature
  char variant[8];              // pixel stage: "", "sm", "ds" or "ds.sm" (PixelVariant); empty for other stages
};

// Function constant of the pixel variant "sm": the pipeline's sample mask.
constexpr uint16_t kSampleMaskConstant = 128;

// The variant of a pixel function the pipeline state asks for.
inline void
PixelVariant(bool dual_source_blending, uint32_t sample_mask, char variant[8]) {
  std::snprintf(variant, 8, "%s%s%s", dual_source_blending ? "ds" : "",
                dual_source_blending && sample_mask != 0xffffffffu ? "." : "", sample_mask != 0xffffffffu ? "sm" : "");
}

// One input of a vertex function: what the input layout must feed, by semantic.
struct VertexInput {
  uint32_t location = 0;       // function constants 4 * location + 0..3
  uint32_t semantic_index = 0;
  char semantic[64] = {};
};

// What the provider's `library` bytes are (MACRUNNER_DX12_OWN_SHADER_KIND).
enum class Kind : uint32_t { Library = 0, Source = 1 };

// MTLLanguageVersion3_1: the language the translator's text is written for.
constexpr uint32_t kLanguage31 = (3u << 16) | 1u;

struct Response {
  Kind kind = Kind::Library;    // library: Metal library, "MTLB"; source: MSL text the layer builds a library from
  uint32_t language = 0;        // source: MTLLanguageVersion (major << 16 | minor)
  std::vector<uint8_t> library; // Metal library, "MTLB" (kind library) or MSL text (kind source)
  char entry[128] = {};         // function name inside the library
  uint32_t format = 0;          // description format: 1 binds nothing, 2 and 3 follow the legacy convention
  uint32_t resources = 0;       // resources the function reads through the root arguments
  uint32_t vertex_inputs = 0;   // inputs the function pulls from the vertex buffer table
  char root[41] = {};           // format 2: SHA-1 of the RTS0 part the function was translated for; empty = none
  uint32_t root_qwords = 0;     // format 2: 64-bit words of the root argument buffer the function assumes
  std::vector<VertexInput> inputs;
  uint32_t threads[3] = {};     // format 2, compute: thread group size
  char variant[8] = {};         // format 3: the variant the function was translated as
  uint32_t coverage_output = 0; // format 3, pixel: the shader itself writes coverage (SV_Coverage)
  char colors[9] = {};          // format 3, pixel: per SV_Target 'f', 'i', 'u' (declared type) or '-' (not written)
};

struct Provider {
  const char *name; // value of MACRUNNER_DX12_SHADER_PATH
  // S_OK: response filled. S_FALSE: no translation of this shader; `missing`
  // names what was looked for. Failure: a translation exists but is unusable.
  HRESULT (*fetch)(const Request &request, Response &response, std::string &missing);
};

// One stage of one pipeline as its provider answered it.
struct Shader {
  Request request;
  Response response;
  const char *reason = nullptr; // why the stage is not served (a name of stats::kReasons); null when it is
};

// True when the container carries a DXIL part. An SM5.0 container has nothing to translate.
inline bool
HasDXIL(D3D12_SHADER_BYTECODE bytecode) {
  const auto *bytes = static_cast<const uint8_t *>(bytecode.pShaderBytecode);
  const size_t size = bytecode.BytecodeLength;
  if (!bytes || size < 32 || std::memcmp(bytes, "DXBC", 4)) return false;
  uint32_t count;
  std::memcpy(&count, bytes + 28, 4);
  if (count > 64 || 32 + size_t(count) * 4 > size) return false;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t offset;
    std::memcpy(&offset, bytes + 32 + 4 * i, 4);
    if (size_t(offset) + 8 > size) return false;
    if (!std::memcmp(bytes + offset, "DXIL", 4)) return true;
  }
  return false;
}

inline const char *
StageName(uint32_t stage) {
  static constexpr const char *names[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
  return stage < 6 ? names[stage] : "unknown";
}

inline void
Sha1Hex(const void *data, size_t size, char hex[41]) {
  const auto digest = Sha1HashState::compute(data, size);
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 20; ++i) {
    hex[2 * i] = digits[digest.data[i] >> 4];
    hex[2 * i + 1] = digits[digest.data[i] & 15];
  }
  hex[40] = 0;
}

template <typename... Args>
inline void
Trace(const char *format, Args... args) {
  const DWORD error = GetLastError();
  char line[768];
  const int length = std::snprintf(line, sizeof(line), format, args...);
  DWORD written;
  if (length > 0 && size_t(length) < sizeof(line))
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
  SetLastError(error);
}

// Says why the legacy path builds this pipeline and returns S_FALSE.
inline HRESULT
Legacy(const Provider &provider, const char *pipeline, const void *owner, const char *reason, const char *detail = "") {
  Trace("dx12_shader_source pipeline=%s owner=%p source=legacy requested=%s reason=%s%s%s\n", pipeline, owner,
        provider.name, reason, *detail ? " " : "", detail);
  return S_FALSE;
}

namespace own {

inline std::string
Environment(const char *name) {
  char value[1024];
  const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
  return length && length < sizeof(value) ? std::string(value, length) : std::string();
}

// 0: read. 1: no such file. 2: unreadable, empty or larger than `limit`.
inline int
Read(const std::string &path, size_t limit, std::vector<uint8_t> &bytes) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 1 : 2;
  }
  LARGE_INTEGER size;
  DWORD read = 0;
  bool complete = GetFileSizeEx(file, &size) && size.QuadPart > 0 && uint64_t(size.QuadPart) <= limit;
  if (complete) {
    bytes.resize(size_t(size.QuadPart));
    complete = ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size();
  }
  CloseHandle(file);
  return complete ? 0 : 2;
}

inline void
Write(const std::string &path, const void *data, size_t size, bool append) {
  HANDLE file = CreateFileA(path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written;
  WriteFile(file, data, DWORD(size), &written, nullptr);
  CloseHandle(file);
}

// Leaves what the offline translator needs to produce the missing translation.
inline void
LeaveRequest(const Request &request) {
  static const std::string directory = Environment("MACRUNNER_DX12_OWN_SHADER_REQUEST_DIR");
  if (directory.empty()) return;
  const std::string stage = StageName(request.stage);
  {
    // An application creates the same pipeline many times: one request per process is enough.
    static dxmt::mutex mutex;
    static std::unordered_set<std::string> left;
    std::lock_guard<dxmt::mutex> lock(mutex);
    if (!left.insert(stage + "." + request.key + "." + (request.root ? request.root_key : "none") + "." + request.variant)
             .second)
      return;
  }
  stats::CountRequest();
  Write(directory + "\\" + request.key + "." + stage + ".dxbc", request.shader.pShaderBytecode,
        request.shader.BytecodeLength, false);
  if (request.root) Write(directory + "\\" + request.root_key + ".rts0", request.root, request.root_size, false);
  const std::string line = "stage=" + stage + " shader=" + request.key + " root=" +
                           (request.root ? request.root_key : "none") +
                           (request.variant[0] ? std::string(" variant=") + request.variant : std::string()) + "\r\n";
  Write(directory + "\\requests.txt", line.data(), line.size(), true);
}

// format=1 (first milestone, binds nothing):
//   entry=<name>  resources=<n>  vertex_inputs=<n>
// format=2 (legacy binding convention):
//   entry=<name>
//   root=<sha1 of the RTS0 part | none>   root_qwords=<n>   resources=<n>
//   vertex_inputs=<n>   then n lines   vin=<location> <semantic index> <semantic>
//   threads=<x> <y> <z>                   compute only
// format=3: format 2 and
//   variant=<none|sm|ds|ds.sm>            what the function was translated as
//   coverage_output=<0|1>                 pixel only
//   colors=<8 characters f i u ->         pixel only
// kind=<library|source>, language=<M.m>  (any format) what the answer is and, for text, the language version;
//                                        read only when MACRUNNER_DX12_OWN_SHADER_KIND=source asks for text
inline bool
ParseDescription(const std::vector<uint8_t> &bytes, Response &response) {
  const std::string text(bytes.begin(), bytes.end());
  unsigned seen = 0;
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string name = line.substr(0, equals), value = line.substr(equals + 1);
    if (name == "format") {
      response.format = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    } else if (name == "entry") {
      if (value.empty() || value.size() >= sizeof(response.entry)) return false;
      std::memcpy(response.entry, value.c_str(), value.size() + 1);
      seen |= 1;
    } else if (name == "resources") {
      response.resources = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
      seen |= 2;
    } else if (name == "vertex_inputs") {
      response.vertex_inputs = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
      seen |= 4;
    } else if (name == "root") {
      if (value != "none") {
        if (value.size() != 40) return false;
        std::memcpy(response.root, value.c_str(), 41);
      }
      seen |= 8;
    } else if (name == "root_qwords") {
      response.root_qwords = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    } else if (name == "vin") {
      VertexInput input;
      char semantic[64] = {};
      if (std::sscanf(value.c_str(), "%u %u %63s", &input.location, &input.semantic_index, semantic) != 3 ||
          input.location >= 32)
        return false;
      std::memcpy(input.semantic, semantic, sizeof(semantic));
      response.inputs.push_back(input);
    } else if (name == "threads") {
      if (std::sscanf(value.c_str(), "%u %u %u", &response.threads[0], &response.threads[1], &response.threads[2]) != 3)
        return false;
    } else if (name == "variant") {
      if (value != "none") {
        if (value.size() >= sizeof(response.variant)) return false;
        std::memcpy(response.variant, value.c_str(), value.size() + 1);
      }
      seen |= 16;
    } else if (name == "kind") {
      if (value == "source") response.kind = Kind::Source;
      else if (value == "library") response.kind = Kind::Library;
      else return false;
    } else if (name == "language") {
      unsigned major = 0, minor = 0;
      if (std::sscanf(value.c_str(), "%u.%u", &major, &minor) != 2 || !major || major > 255 || minor > 255) return false;
      response.language = (major << 16) | minor;
    } else if (name == "coverage_output") {
      response.coverage_output = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    } else if (name == "colors") {
      if (value.size() != 8 || value.find_first_not_of("fiu-") != std::string::npos) return false;
      std::memcpy(response.colors, value.c_str(), 9);
    }
  }
  if (response.format == 1) return seen == 7;
  return (response.format == 2 ? seen == 15 : response.format == 3 && seen == 31) &&
         response.inputs.size() == response.vertex_inputs && (response.root[0] || !response.resources);
}

// The kind of answer asked for, read once: "source" = MSL text; "library", unset or anything else = ready libraries.
inline Kind
SelectedKind() {
  static const Kind kind = [] {
    const std::string value = Environment("MACRUNNER_DX12_OWN_SHADER_KIND");
    if (value == "source") return Kind::Source;
    if (!value.empty() && value != "library")
      Trace("dx12_shader_source kind=library requested=%.32s reason=unknown_kind\n", value.c_str());
    return Kind::Library;
  }();
  return kind;
}

inline HRESULT
Fetch(const Request &request, Response &response, std::string &missing) {
  static const std::string directory = Environment("MACRUNNER_DX12_OWN_SHADER_DIR");
  if (directory.empty()) {
    missing = "MACRUNNER_DX12_OWN_SHADER_DIR";
    return S_FALSE;
  }
  const bool text = SelectedKind() == Kind::Source;
  const char *const extension = text ? ".msl" : ".metallib";
  // The translation made for this root signature first, then the one that reads no root arguments.
  const std::string stage =
      std::string(StageName(request.stage)) + (request.variant[0] ? std::string(".") + request.variant : std::string());
  std::string name = std::string(request.key) + "." + request.root_key + "." + stage;
  int read = request.root ? Read(directory + "\\" + name + extension, 64u << 20, response.library) : 1;
  if (read == 1) {
    missing = request.root ? name + extension : std::string();
    name = std::string(request.key) + "." + stage;
    read = Read(directory + "\\" + name + extension, 64u << 20, response.library);
    if (read == 1) {
      missing = missing.empty() ? name + extension : missing + "|" + name + extension;
      LeaveRequest(request);
      return S_FALSE;
    }
  }
  if (read) return E_FAIL;
  // A library without its description is a broken translation, not a missing one.
  std::vector<uint8_t> description;
  if (Read(directory + "\\" + name + ".desc", 16384, description) || !ParseDescription(description, response))
    return E_FAIL;
  if (text) {
    // The text needs its description to say so, and the language it is built with.
    if (response.kind != Kind::Source || !response.language) return E_FAIL;
  } else {
    response.kind = Kind::Library; // kind and language of a description are for the text answer only
    response.language = 0;
  }
  return S_OK;
}

} // namespace own

// The provider named by MACRUNNER_DX12_SHADER_PATH; null selects the legacy path for every pipeline.
inline const Provider *
Selected() {
  static const Provider *const selected = []() -> const Provider * {
    static constexpr Provider providers[] = {
        {"own", own::Fetch},
    };
    const DWORD error = GetLastError();
    char value[16] = {};
    const DWORD length = GetEnvironmentVariableA("MACRUNNER_DX12_SHADER_PATH", value, sizeof(value));
    SetLastError(error);
    if (!length) return nullptr;
    if (length < sizeof(value))
      for (const auto &provider : providers)
        if (!std::strcmp(value, provider.name)) {
          if (stats::Period()) stats::Arm();
          return &provider;
        }
    Trace("dx12_shader_source source=legacy requested=%s reason=unknown_provider\n",
          length < sizeof(value) ? value : "(too long)");
    return nullptr;
  }();
  return selected;
}

// One stage through the provider. S_OK: `shader` holds a translation this
// pipeline can use. S_FALSE: the legacy path builds the pipeline (reason on
// stderr). Failure: the translation exists and is broken.
inline HRESULT
Fetch(const Provider &provider, const char *pipeline, const void *owner, D3D12_SHADER_BYTECODE bytecode,
      uint32_t stage, MTLD3D12RootSignature *root, Shader &shader, const char *variant = "") {
  struct PreserveLastError {
    DWORD value = GetLastError();
    ~PreserveLastError() { SetLastError(value); }
  } preserve_last_error;
  if (!bytecode.pShaderBytecode || !bytecode.BytecodeLength) return E_INVALIDARG;
  shader.request = {bytecode, nullptr, 0, stage, {}, {}, {}};
  std::snprintf(shader.request.variant, sizeof(shader.request.variant), "%s", variant);
  if (root) {
    const void *blob = nullptr;
    UINT size = 0;
    if (root->GetBlob(&blob) && SUCCEEDED(microsoft::DXBCGetRootSignature(blob, &shader.request.root, &size)) &&
        shader.request.root && size) {
      shader.request.root_size = size;
      Sha1Hex(shader.request.root, size, shader.request.root_key);
    } else {
      shader.request.root = nullptr;
    }
  }
  Sha1Hex(bytecode.pShaderBytecode, bytecode.BytecodeLength, shader.request.key);
  char detail[320];
  // Running an application (fallback key): an SM5.0 container is the legacy translator's, and no request is left for it.
  if (FallbackEnabled() && !HasDXIL(bytecode)) {
    std::snprintf(detail, sizeof(detail), "stage=%s shader=%s", StageName(stage), shader.request.key);
    shader.reason = "not_dxil";
    return Legacy(provider, pipeline, owner, "not_dxil", detail);
  }
  std::string missing;
  const HRESULT hr = provider.fetch(shader.request, shader.response, missing);
  if (hr == S_FALSE) {
    std::snprintf(detail, sizeof(detail), "stage=%s shader=%s root=%s variant=%s missing=%s", StageName(stage),
                  shader.request.key, shader.request.root ? shader.request.root_key : "none",
                  *variant ? variant : "none", missing.c_str());
    shader.reason = *variant ? "no_translation_variant" : "no_translation";
    return Legacy(provider, pipeline, owner, "no_translation", detail);
  }
  const auto &response = shader.response;
  // A function translated as one variant under the name of another is a broken translation.
  const bool text = response.kind == Kind::Source;
  if (FAILED(hr) || response.library.size() < 16 || (!text && std::memcmp(response.library.data(), "MTLB", 4)) ||
      (text && !response.language) || !response.entry[0] || !std::memchr(response.entry, 0, sizeof(response.entry)) ||
      std::strcmp(response.variant, variant) || (*variant && response.format != 3)) {
    Trace("dx12_shader_source pipeline=%s owner=%p source=%s failed=unusable_translation stage=%s shader=%s hr=%08x\n",
          pipeline, owner, provider.name, StageName(stage), shader.request.key, unsigned(hr));
    shader.reason = "unusable_translation";
    return FAILED(hr) ? hr : E_FAIL;
  }
  std::snprintf(detail, sizeof(detail), "stage=%s shader=%s format=%u resources=%u vertex_inputs=%u root=%s",
                StageName(stage), shader.request.key, response.format, response.resources, response.vertex_inputs,
                response.root[0] ? response.root : "none");
  // Format 1 knows no binding convention: it is admitted only when it binds nothing.
  if (response.format == 1 && (response.resources || response.vertex_inputs)) {
    shader.reason = "translation_not_admitted";
    return Legacy(provider, pipeline, owner, "translation_not_admitted", detail);
  }
  // A function translated for one root signature reads garbage under another.
  if (response.root[0] && (!shader.request.root || std::strcmp(response.root, shader.request.root_key) ||
                           !root || response.root_qwords != root->UploadQwords)) {
    Trace("dx12_shader_source pipeline=%s owner=%p source=%s failed=root_signature_mismatch %s pipeline_root=%s "
          "pipeline_qwords=%u translation_qwords=%u\n",
          pipeline, owner, provider.name, detail, shader.request.root ? shader.request.root_key : "none",
          root ? unsigned(root->UploadQwords) : 0u, response.root_qwords);
    shader.reason = "root_signature_mismatch";
    return E_FAIL;
  }
  if (response.vertex_inputs && stage != 1) {
    shader.reason = "translation_not_admitted";
    return Legacy(provider, pipeline, owner, "translation_not_admitted", detail);
  }
  return S_OK;
}

// The Metal library of one translated stage. Kind library: made from the ready bytes, every call, as always.
// Kind source: built from the MSL text with the response's language version and every other option at its default,
// once per process for the same text and language; the pipelines after the first take the library from the process.
// A build error leaves the Metal text on stderr; `error` is borrowed, as with newLibrary.
inline WMT::Reference<WMT::Library>
MakeLibrary(WMT::Device device, const Response &response, WMT::Error &error) {
  if (response.kind != Kind::Source) return device.newLibrary(response.library.data(), response.library.size(), error);
  char key[41 + 9];
  Sha1Hex(response.library.data(), response.library.size(), key);
  std::snprintf(key + 40, 9, ":%x", unsigned(response.language));
  static dxmt::mutex mutex;
  static std::unordered_map<std::string, WMT::Reference<WMT::Library>> built;
  {
    std::lock_guard<dxmt::mutex> lock(mutex);
    const auto found = built.find(key);
    if (found != built.end()) {
      stats::CountSourceLibrary(false, 0);
      return found->second;
    }
  }
  LARGE_INTEGER frequency, begin, end;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&begin);
  auto library = device.newLibraryWithSource(
      std::string_view(reinterpret_cast<const char *>(response.library.data()), response.library.size()),
      response.language, error);
  QueryPerformanceCounter(&end);
  if (!library) {
    std::string text = error.handle ? error.description().getUTF8String() : std::string("none");
    for (auto &c : text)
      if (c == '\n' || c == '\r') c = '|';
    Trace("dx12_shader_source_compile_failed key=%.40s language=%x bytes=%llu error=%.600s\n", key,
          unsigned(response.language), (unsigned long long)response.library.size(), text.c_str());
    return library;
  }
  stats::CountSourceLibrary(true, uint64_t((end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart));
  std::lock_guard<dxmt::mutex> lock(mutex);
  built.emplace(key, library);
  return library;
}

} // namespace dxmt::shader_source
