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
//   source    MSL text. The description (<shader>.<root>.<stage>[.<variant>].desc, or without root) marks the
//             translation and carries
//                 kind=source          the answer is text; a .desc without it is a broken translation
//                 language=<M>.<m>     MSL language version the text is built with (3.1)
//                 text=<sha1>          (optional) SHA-1 of the text, which is then <sha1>.msl in the same directory:
//                                      one file per different text, however many descriptions name it. The layer
//                                      does not read the text when the library for (text, language) already exists
//                                      in the process, and refuses a text whose SHA-1 differs.
//             Without text= the text is <shader>.<root>.<stage>[.<variant>].msl next to the description (as in the
//             first revision of this answer).
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

extern "C" int puff(unsigned char *dest, unsigned long *destlen, const unsigned char *source, unsigned long *sourcelen);

namespace dxmt::shader_source {

// tess-gs 14.10.2026: a translation of a PAIR of stages. The object function of a tessellation pipeline
// fuses VS and HS (kind 1, "vshs"), the mesh function fuses the tessellator and DS (kind 2, "hsds"); the
// pair is keyed by both container digests.
enum PairKind : uint32_t { kSingleStage = 0, kVertexHull = 1, kHullDomain = 2, kVertexGeometry = 3, kGeometryMesh = 4 };
// tess-gs: geometry pairs (format 5): kVertexGeometry "vsgs" = object function (VS batching), kGeometryMesh "gsm" =
// mesh function (GS replay); both are keyed by VS+GS+root because the payload layout depends on the VS registers.

struct Request {
  D3D12_SHADER_BYTECODE shader; // container exactly as the application passed it
  const void *root;             // RTS0 part of the pipeline's root signature; null without one
  size_t root_size;
  uint32_t stage;               // D3D10_SB_TOKENIZED_PROGRAM_TYPE: ps 0, vs 1, gs 2, hs 3, ds 4, cs 5
  char key[41];                 // SHA-1 of the container, lower-case hex
  char root_key[41];            // SHA-1 of the RTS0 part; empty without a root signature
  char variant[8];              // pixel stage: "", "sm", "ds" or "ds.sm" (PixelVariant); empty for other stages
  uint32_t kind = kSingleStage; // PairKind
  D3D12_SHADER_BYTECODE second = {}; // the second container of a pair (HS for vshs, DS for hsds)
  char second_key[41] = {};     // its SHA-1
};

inline const char *
KindName(uint32_t kind) {
  return kind == kVertexHull ? "vshs" : kind == kHullDomain ? "hsds" : kind == kVertexGeometry ? "vsgs" : kind == kGeometryMesh ? "gsm" : "";
}

// Reflection of a tessellation pair the layer's allocation (ValidateTessellationReflection) consumes.
struct TessellationResponse {
  char kind[8] = {};
  uint32_t domain = 0, partition = 0, output = 0;  // D3D11_SB_TESSELLATOR_* values
  float max_factor = 0;
  uint32_t in_cp = 0, out_cp = 0, threads_per_patch = 0, hs_out_regs = 0, patch_scalars = 0, patch_regs = 0,
           vs_out_regs = 0, payload_bytes = 0, ds_out_regs = 0, max_potential_factor = 0, mesh_vertices = 0,
           mesh_primitives = 0, split = 0, max_split_factor = 0;   // split: раздельный режим mesh (две группы на workload)
};

// tess-gs: reflection of a geometry pair (gs_compose.py description(), format 5).
struct GeometryResponse {
  char kind[8] = {};
  uint32_t primitive = 0, output_topology = 0, max_vertices = 0, vertices_per_group = 0, primitives_per_group = 0,
           vs_out_regs = 0, gs_out_regs = 0, payload_bytes = 0, instances = 0;
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
  std::vector<uint8_t> library; // Metal library, "MTLB" (kind library) or MSL text (kind source; empty when text_ready)
  char text[41] = {};           // source: SHA-1 of the text (description key `text`); empty = the text sits next to the description
  uint64_t text_bytes = 0;      // source: length of the text
  bool text_ready = false;      // source: the library for this text and language exists in the process; `library` is not read
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
  TessellationResponse tess;    // format 4: a tessellation pair (object or mesh function)
  GeometryResponse gs;          // format 5: a geometry pair (object or mesh function)
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

// tess-gs 14.10.2026: canonical root key = SHA-1 of the PARSED layout text (byte-identical to translator/rootcanon.py), not of
// the RTS0 bytes. The same root signature serialized as version 1.0 or 1.1 gives one key (the game serialized 1.0 in the trial,
// the cache was keyed by the 1.1 blob). Fields that exist only in 1.1/1.2 (range and root-descriptor flags, sampler flags) are
// not part of it; APPEND range offsets are resolved to the running offset. A malformed blob falls back to the SHA-1 of its bytes.
// tess-gs 15.10.2026: the RTS0 version of a pipeline's root signature object (the pair requests of a tessellation or geometry
// pipeline carry the root; the single-stage VS request of such a pipeline does not, so the trace takes it from the object).
inline unsigned
PipelineRootVersion(MTLD3D12RootSignature *root) {
  const void *blob = nullptr;
  const void *rts0 = nullptr;
  UINT size = 0;
  if (!root || !root->GetBlob(&blob) || FAILED(microsoft::DXBCGetRootSignature(blob, &rts0, &size)) || !rts0 || size < 4) return 0;
  uint32_t v = 0;
  std::memcpy(&v, rts0, 4);
  return v;
}

// tess-gs 15.10.2026: the serialization version of the RTS0 part (1 = 1.0, 2 = 1.1, 3 = 1.2) for the trace; 0 without a root
inline unsigned
RootVersion(const void *rts0, size_t size) {
  uint32_t v = 0;
  if (rts0 && size >= 4) std::memcpy(&v, rts0, 4);
  return v;
}

inline void
RootKey(const void *rts0, size_t size, char hex[41]) {
  const auto *b = static_cast<const uint8_t *>(rts0);
  auto u32 = [&](size_t off, uint32_t &v) {
    if (off + 4 > size) return false;
    std::memcpy(&v, b + off, 4);
    return true;
  };
  std::string text;
  char line[320];
  uint32_t version = 0, nparam = 0, param_off = 0, nstatic = 0, static_off = 0, flags = 0;
  bool ok = size >= 24 && u32(0, version) && u32(4, nparam) && u32(8, param_off) && u32(12, nstatic) &&
            u32(16, static_off) && u32(20, flags) && version >= 1 && version <= 3;
  if (ok) {
    std::snprintf(line, sizeof(line), "rootsig-canon-1\nflags=%u\nparams=%u\n", flags, nparam);
    text += line;
    for (uint32_t i = 0; ok && i < nparam; ++i) {
      uint32_t kind = 0, vis = 0, off = 0;
      const size_t pp = size_t(param_off) + size_t(12) * i;
      if (!(u32(pp, kind) && u32(pp + 4, vis) && u32(pp + 8, off))) { ok = false; break; }
      if (kind == 0) {
        uint32_t nranges = 0, roff = 0;
        if (!(u32(off, nranges) && u32(off + 4, roff))) { ok = false; break; }
        std::snprintf(line, sizeof(line), "p%u type=%u vis=%u ranges=%u\n", i, kind, vis, nranges);
        text += line;
        uint32_t running = 0;
        const size_t stride = version == 1 ? 20 : 24;
        for (uint32_t j = 0; j < nranges; ++j) {
          uint32_t rtype = 0, num = 0, base = 0, space = 0, offset = 0;
          const size_t r = size_t(roff) + stride * j;
          if (!(u32(r, rtype) && u32(r + 4, num) && u32(r + 8, base) && u32(r + 12, space) &&
                u32(r + (version == 1 ? 16 : 20), offset))) { ok = false; break; }
          if (offset == 0xffffffffu) offset = running;
          running = num == 0xffffffffu ? 0xffffffffu : offset + num;
          std::snprintf(line, sizeof(line), " r%u type=%u num=%u base=%u space=%u offset=%u\n", j, rtype, num, base, space, offset);
          text += line;
        }
      } else if (kind == 1) {
        uint32_t reg = 0, space = 0, count = 0;
        if (!(u32(off, reg) && u32(off + 4, space) && u32(off + 8, count))) { ok = false; break; }
        std::snprintf(line, sizeof(line), "p%u type=%u vis=%u reg=%u space=%u num=%u\n", i, kind, vis, reg, space, count);
        text += line;
      } else if (kind >= 2 && kind <= 4) {
        uint32_t reg = 0, space = 0;
        if (!(u32(off, reg) && u32(off + 4, space))) { ok = false; break; }
        std::snprintf(line, sizeof(line), "p%u type=%u vis=%u reg=%u space=%u\n", i, kind, vis, reg, space);
        text += line;
      } else { ok = false; break; }
    }
    if (ok) {
      std::snprintf(line, sizeof(line), "samplers=%u\n", nstatic);
      text += line;
      const size_t stride = version == 3 ? 56 : 52;
      for (uint32_t i = 0; ok && i < nstatic; ++i) {
        uint32_t f[13] = {};
        for (int k = 0; k < 13 && ok; ++k) ok = u32(size_t(static_off) + stride * i + size_t(4) * k, f[k]);
        if (!ok) break;
        std::snprintf(line, sizeof(line),
                      " s%u filter=%u au=%u av=%u aw=%u lodbias=%u aniso=%u cmp=%u border=%u minlod=%u maxlod=%u reg=%u space=%u vis=%u\n",
                      i, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12]);
        text += line;
      }
    }
  }
  if (!ok) {
    Sha1Hex(rts0, size, hex);
    return;
  }
  Sha1Hex(text.data(), text.size(), hex);
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

// Libraries built from MSL text in this process, by SHA-1 of the text and the language version. Entries are never removed.
namespace source_libraries {
struct Built {
  WMT::Reference<WMT::Library> library;
  uint64_t bytes = 0; // length of the text
};
inline dxmt::mutex &
Mutex() {
  static dxmt::mutex mutex;
  return mutex;
}
inline std::unordered_map<std::string, Built> &
Map() {
  static std::unordered_map<std::string, Built> built;
  return built;
}
inline std::string
Key(const char *sha1, uint32_t language) {
  char key[41 + 9];
  std::snprintf(key, sizeof(key), "%.40s:%x", sha1, unsigned(language));
  return key;
}
inline bool
Find(const std::string &key, Built &found) {
  std::lock_guard<dxmt::mutex> lock(Mutex());
  const auto it = Map().find(key);
  if (it == Map().end()) return false;
  found = it->second;
  return true;
}
} // namespace source_libraries

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

// tess-gs 15.10.2026: text of a source answer — <name>.msl, or, when absent, <name>.msl.z: 4-byte little-endian length of the text
// followed by a zlib stream (level 9, ~8x on MSL). Inflate is zlib's contrib/puff (Mark Adler, zlib license, src/d3d12/puff). A broken
// .msl.z is a clear refusal with a trace line, never a crash: the caller treats it like a missing text (E_FAIL).
// Returns 0 on success, 1 when neither file exists, 2 when the compressed file is malformed (traced).
inline int
ReadTextMaybeCompressed(const std::string &directory, const std::string &name, std::vector<uint8_t> &text) {
  const int rc_plain = Read(directory + "\\" + name + ".msl", 64u << 20, text);
  if (rc_plain == 0) return 0;
  std::vector<uint8_t> packed;
  const int rc = Read(directory + "\\" + name + ".msl.z", 64u << 20, packed);
  if (rc != 0) return 1;
  uint32_t length = 0;
  if (packed.size() < 6 + 2) {
    Trace("dx12_shader_source text=%s failed=text_inflate reason=short bytes=%zu\n", name.c_str(), packed.size());
    return 2;
  }
  std::memcpy(&length, packed.data(), 4);
  const unsigned char *z = packed.data() + 4;
  const size_t zlen = packed.size() - 4;
  if (length > (64u << 20) || (z[0] & 0x0f) != 8 || ((unsigned(z[0]) << 8) | z[1]) % 31 != 0 || (z[1] & 0x20)) {
    Trace("dx12_shader_source text=%s failed=text_inflate reason=header length=%u\n", name.c_str(), length);
    return 2;
  }
  text.assign(length, 0);
  unsigned long destlen = length, sourcelen = static_cast<unsigned long>(zlen - 2);   // deflate stream after the 2-byte zlib header
  const int err = puff(text.data(), &destlen, z + 2, &sourcelen);
  if (err != 0 || destlen != length) {
    Trace("dx12_shader_source text=%s failed=text_inflate reason=puff rc=%d got=%lu want=%u\n", name.c_str(), err, destlen, length);
    text.clear();
    return 2;
  }
  return 0;
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
  if (request.kind != kSingleStage) {
    // A pair: both containers under their own stage names and one line naming the pair.
    const char *first = request.kind == kHullDomain ? "hs" : "vs",
               *second = request.kind == kVertexHull ? "hs" : request.kind == kHullDomain ? "ds" : "gs";
    Write(directory + "\\" + request.key + "." + first + ".dxbc", request.shader.pShaderBytecode,
          request.shader.BytecodeLength, false);
    Write(directory + "\\" + request.second_key + "." + second + ".dxbc", request.second.pShaderBytecode,
          request.second.BytecodeLength, false);
    if (request.root) Write(directory + "\\" + request.root_key + ".rts0", request.root, request.root_size, false);
    const std::string line = std::string("stage=") + KindName(request.kind) + " shader=" + request.key + " second=" +
                             request.second_key + " root=" + (request.root ? request.root_key : "none") + "\r\n";
    Write(directory + "\\requests.txt", line.data(), line.size(), true);
    return;
  }
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
// kind=<library|source>, language=<M.m>, text=<sha1>  (any format) what the answer is and, for text, the language version
//                                        and the SHA-1 of the text; read only when MACRUNNER_DX12_OWN_SHADER_KIND=source asks for text
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
    } else if (name == "answer" || name == "kind") {
      // `answer=<library|source>` is what the answer is; the first spelling of it, `kind=<library|source>`, is still read.
      // `kind=<vshs|hsds|vsgs|gsm>` (tess-gs) names the pair a translation is for: the same key, disjoint values.
      if (value == "source") response.kind = Kind::Source;
      else if (value == "library") response.kind = Kind::Library;
      else if (name == "kind") {
        if (value.empty() || value.size() >= sizeof(response.tess.kind)) return false;
        std::memcpy(response.tess.kind, value.c_str(), value.size() + 1);
        std::memcpy(response.gs.kind, value.c_str(), value.size() + 1);
        seen |= 32;
      } else {
        return false;
      }
    } else if (name == "text") {
      if (value.size() != 40 || value.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
      std::memcpy(response.text, value.c_str(), 41);
    } else if (name == "language") {
      unsigned major = 0, minor = 0;
      if (std::sscanf(value.c_str(), "%u.%u", &major, &minor) != 2 || !major || major > 255 || minor > 255) return false;
      response.language = (major << 16) | minor;
    } else if (name == "coverage_output") {
      response.coverage_output = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    } else if (name == "colors") {
      if (value.size() != 8 || value.find_first_not_of("fiu-") != std::string::npos) return false;
      std::memcpy(response.colors, value.c_str(), 9);
    } else if (name.compare(0, 5, "tess_") == 0) {
      // format=4: the tessellation reflection of a pair (tess_compose.py description())
      auto &t = response.tess;
      const std::string field = name.substr(5);
      const uint32_t number = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
      if (field == "domain") t.domain = value == "tri" ? 2 : value == "quad" ? 3 : value == "isoline" ? 1 : 0;
      else if (field == "partition") t.partition = value == "integer" ? 1 : value == "pow2" ? 2 : value == "odd" ? 3 : value == "even" ? 4 : 0;
      else if (field == "output") t.output = value == "point" ? 1 : value == "line" ? 2 : value == "tri_cw" ? 3 : value == "tri_ccw" ? 4 : 0;
      else if (field == "max_factor") t.max_factor = float(std::strtod(value.c_str(), nullptr));
      else if (field == "in_cp") t.in_cp = number;
      else if (field == "out_cp") t.out_cp = number;
      else if (field == "threads_per_patch") t.threads_per_patch = number;
      else if (field == "hs_out_regs") t.hs_out_regs = number;
      else if (field == "patch_scalars") t.patch_scalars = number;
      else if (field == "patch_regs") t.patch_regs = number;
      else if (field == "vs_out_regs") t.vs_out_regs = number;
      else if (field == "payload_bytes") t.payload_bytes = number;
      else if (field == "ds_out_regs") t.ds_out_regs = number;
      else if (field == "max_potential_factor") t.max_potential_factor = number;
      else if (field == "mesh_vertices") t.mesh_vertices = number;
      else if (field == "mesh_primitives") t.mesh_primitives = number;
      else if (field == "split") t.split = number;
      else if (field == "max_split_factor") t.max_split_factor = number;
    } else if (name.compare(0, 3, "gs_") == 0) {
      // format=5: the geometry reflection of a pair (gs_compose.py description())
      auto &g = response.gs;
      const std::string field = name.substr(3);
      const uint32_t number = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
      if (field == "primitive") g.primitive = number;
      else if (field == "output_topology") g.output_topology = number;
      else if (field == "max_vertices") g.max_vertices = number;
      else if (field == "vertices_per_group") g.vertices_per_group = number;
      else if (field == "primitives_per_group") g.primitives_per_group = number;
      else if (field == "vs_out_regs") g.vs_out_regs = number;
      else if (field == "out_regs") g.gs_out_regs = number;
      else if (field == "payload_bytes") g.payload_bytes = number;
      else if (field == "instances") g.instances = number;
    }
  }
  if (response.format == 1) return seen == 7;
  if (response.format == 5) {
    const auto &g = response.gs;
    return seen == 63 && response.inputs.size() == response.vertex_inputs && (response.root[0] || !response.resources) &&
           g.primitive && g.output_topology && g.max_vertices && g.max_vertices <= 256 && g.vertices_per_group &&
           g.primitives_per_group && g.vs_out_regs && g.payload_bytes && g.payload_bytes <= 16256 && g.instances == 1;
  }
  if (response.format == 4) {
    const auto &t = response.tess;
    return seen == 63 && response.inputs.size() == response.vertex_inputs && (response.root[0] || !response.resources) &&
           t.domain && t.partition && t.output && t.in_cp && t.out_cp && t.threads_per_patch && t.payload_bytes &&
           t.mesh_vertices && t.mesh_primitives;
  }
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

// Kind source. The description marks the translation (the text without it is not looked for); the text is then found by
// the description's `text` key, or next to the description when it has none.
inline HRESULT
FetchText(const Request &request, Response &response, std::string &missing, const std::string &directory) {
  // A pair (tess-gs) is named by both digests: <first>.<second>.<root>.<vshs|hsds|vsgs|gsm>.
  const std::string stage = request.kind != kSingleStage
      ? std::string(KindName(request.kind))
      : std::string(StageName(request.stage)) + (request.variant[0] ? std::string(".") + request.variant : std::string());
  const std::string key = request.kind != kSingleStage ? std::string(request.key) + "." + request.second_key
                                                       : std::string(request.key);
  std::vector<uint8_t> description;
  std::string name = key + "." + request.root_key + "." + stage;
  int read = request.root ? Read(directory + "\\" + name + ".desc", 16384, description) : 1;
  if (read == 1) {
    missing = request.root ? name + ".desc" : std::string();
    name = key + "." + stage;
    read = Read(directory + "\\" + name + ".desc", 16384, description);
    if (read == 1) {
      missing = missing.empty() ? name + ".desc" : missing + "|" + name + ".desc";
      LeaveRequest(request);
      return S_FALSE;
    }
  }
  if (read || !ParseDescription(description, response)) return E_FAIL;
  // The text needs its description to say so, and the language it is built with.
  if (response.kind != Kind::Source || !response.language) return E_FAIL;
  if (!response.text[0]) {
    // The first revision's layout: the text of this translation only, next to its description.
    if (ReadTextMaybeCompressed(directory, name, response.library)) return E_FAIL;
    response.text_bytes = response.library.size();
    return S_OK;
  }
  source_libraries::Built built;
  if (source_libraries::Find(source_libraries::Key(response.text, response.language), built)) {
    response.text_ready = true; // the library exists in this process: the text is not read again
    response.text_bytes = built.bytes;
    return S_OK;
  }
  // A description that names a text which is not there, or is another one, is a broken translation.
  if (ReadTextMaybeCompressed(directory, response.text, response.library)) return E_FAIL;
  char actual[41];
  Sha1Hex(response.library.data(), response.library.size(), actual);
  if (std::strcmp(actual, response.text)) {
    Trace("dx12_shader_source failed=text_mismatch shader=%s description_text=%.40s actual=%.40s\n", request.key,
          response.text, actual);
    return E_FAIL;
  }
  response.text_bytes = response.library.size();
  return S_OK;
}

inline HRESULT
Fetch(const Request &request, Response &response, std::string &missing) {
  static const std::string directory = Environment("MACRUNNER_DX12_OWN_SHADER_DIR");
  if (directory.empty()) {
    missing = "MACRUNNER_DX12_OWN_SHADER_DIR";
    return S_FALSE;
  }
  const bool text = SelectedKind() == Kind::Source;
  if (text) return FetchText(request, response, missing, directory);
  const char *const extension = ".metallib";
  // The translation made for this root signature first, then the one that reads no root arguments.
  // A pair (tess-gs) is named by both digests: <first>.<second>.<root>.<vshs|hsds|vsgs|gsm>.
  const std::string stage = request.kind != kSingleStage
      ? std::string(KindName(request.kind))
      : std::string(StageName(request.stage)) + (request.variant[0] ? std::string(".") + request.variant : std::string());
  const std::string key = request.kind != kSingleStage ? std::string(request.key) + "." + request.second_key
                                                       : std::string(request.key);
  std::string name = key + "." + request.root_key + "." + stage;
  int read = request.root ? Read(directory + "\\" + name + extension, 64u << 20, response.library) : 1;
  if (read == 1) {
    missing = request.root ? name + extension : std::string();
    name = key + "." + stage;
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
  response.kind = Kind::Library; // kind, language and text of a description are for the text answer only
  response.language = 0;
  response.text[0] = 0;
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
      RootKey(shader.request.root, size, shader.request.root_key);
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
    std::snprintf(detail, sizeof(detail), "stage=%s shader=%s root=%s root_version=%u variant=%s missing=%s", StageName(stage),
                  shader.request.key, shader.request.root ? shader.request.root_key : "none", RootVersion(shader.request.root, shader.request.root_size),
                  *variant ? variant : "none", missing.c_str());
    shader.reason = *variant ? "no_translation_variant" : "no_translation";
    return Legacy(provider, pipeline, owner, "no_translation", detail);
  }
  const auto &response = shader.response;
  // A function translated as one variant under the name of another is a broken translation.
  const bool text = response.kind == Kind::Source;
  if (FAILED(hr) || (response.library.size() < 16 && !response.text_ready) || (!text && std::memcmp(response.library.data(), "MTLB", 4)) ||
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

// tess-gs 14.10.2026: a pair of stages through the provider (kVertexHull: VS+HS object function; kHullDomain:
// HS+DS mesh function; kVertexGeometry / kGeometryMesh: the object and mesh functions of a geometry pipeline).
// Same contract as Fetch; a pair is format 4 (tessellation) or 5 (geometry) and carries its reflection. The answer
// is a library or, with MACRUNNER_DX12_OWN_SHADER_KIND=source, MSL text (`answer=source` in the description).
inline HRESULT
FetchPair(const Provider &provider, const char *pipeline, const void *owner, D3D12_SHADER_BYTECODE first,
          D3D12_SHADER_BYTECODE second, uint32_t kind, MTLD3D12RootSignature *root, Shader &shader) {
  struct PreserveLastError {
    DWORD value = GetLastError();
    ~PreserveLastError() { SetLastError(value); }
  } preserve_last_error;
  if (!first.pShaderBytecode || !first.BytecodeLength || !second.pShaderBytecode || !second.BytecodeLength ||
      (kind != kVertexHull && kind != kHullDomain && kind != kVertexGeometry && kind != kGeometryMesh))
    return E_INVALIDARG;
  const bool geometry_pair = kind == kVertexGeometry || kind == kGeometryMesh;
  shader.request = {first, nullptr, 0, kind == kHullDomain ? 3u : 1u, {}, {}, {}};
  shader.request.kind = kind;
  shader.request.second = second;
  if (root) {
    const void *blob = nullptr;
    UINT size = 0;
    if (root->GetBlob(&blob) && SUCCEEDED(microsoft::DXBCGetRootSignature(blob, &shader.request.root, &size)) &&
        shader.request.root && size) {
      shader.request.root_size = size;
      RootKey(shader.request.root, size, shader.request.root_key);
    } else {
      shader.request.root = nullptr;
    }
  }
  Sha1Hex(first.pShaderBytecode, first.BytecodeLength, shader.request.key);
  Sha1Hex(second.pShaderBytecode, second.BytecodeLength, shader.request.second_key);
  std::string missing;
  const HRESULT hr = provider.fetch(shader.request, shader.response, missing);
  char detail[400];
  if (hr == S_FALSE) {
    std::snprintf(detail, sizeof(detail), "stage=%s shader=%s second=%s root=%s root_version=%u missing=%s", KindName(kind),
                  shader.request.key, shader.request.second_key, shader.request.root ? shader.request.root_key : "none", RootVersion(shader.request.root, shader.request.root_size),
                  missing.c_str());
    shader.reason = "no_translation";
    return Legacy(provider, pipeline, owner, "no_translation", detail);
  }
  const auto &response = shader.response;
  const bool text = response.kind == Kind::Source;
  if (FAILED(hr) || (response.library.size() < 16 && !response.text_ready) ||
      (!text && std::memcmp(response.library.data(), "MTLB", 4)) || (text && !response.language) ||
      !response.entry[0] || !std::memchr(response.entry, 0, sizeof(response.entry)) ||
      response.format != (geometry_pair ? 5u : 4u) ||
      std::strcmp(geometry_pair ? response.gs.kind : response.tess.kind, KindName(kind))) {
    Trace("dx12_shader_source pipeline=%s owner=%p source=%s failed=unusable_translation stage=%s shader=%s second=%s hr=%08x\n",
          pipeline, owner, provider.name, KindName(kind), shader.request.key, shader.request.second_key, unsigned(hr));
    shader.reason = "unusable_translation";
    return FAILED(hr) ? hr : E_FAIL;
  }
  std::snprintf(detail, sizeof(detail), "stage=%s shader=%s second=%s format=%u resources=%u vertex_inputs=%u root=%s",
                KindName(kind), shader.request.key, shader.request.second_key, response.format, response.resources,
                response.vertex_inputs, response.root[0] ? response.root : "none");
  if (response.root[0] && (!shader.request.root || std::strcmp(response.root, shader.request.root_key) ||
                           !root || response.root_qwords != root->UploadQwords)) {
    Trace("dx12_shader_source pipeline=%s owner=%p source=%s failed=root_signature_mismatch %s pipeline_root=%s "
          "pipeline_qwords=%u translation_qwords=%u\n",
          pipeline, owner, provider.name, detail, shader.request.root ? shader.request.root_key : "none",
          root ? unsigned(root->UploadQwords) : 0u, response.root_qwords);
    shader.reason = "root_signature_mismatch";
    return E_FAIL;
  }
  if (response.vertex_inputs && kind != kVertexHull && kind != kVertexGeometry) {
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
  char sha1[41];
  if (response.text[0]) std::memcpy(sha1, response.text, 41);
  else Sha1Hex(response.library.data(), response.library.size(), sha1);
  const std::string key = source_libraries::Key(sha1, response.language);
  source_libraries::Built found;
  if (source_libraries::Find(key, found)) {
    stats::CountSourceLibrary(false, 0);
    return found.library;
  }
  if (response.text_ready) { // the library was there when the translation was fetched and entries are never removed
    Trace("dx12_shader_source_compile_failed key=%.40s language=%x bytes=0 error=text_ready_without_library\n", sha1,
          unsigned(response.language));
    return WMT::Reference<WMT::Library>();
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
    Trace("dx12_shader_source_compile_failed key=%.40s language=%x bytes=%llu error=%.600s\n", sha1,
          unsigned(response.language), (unsigned long long)response.library.size(), text.c_str());
    return library;
  }
  stats::CountSourceLibrary(true, uint64_t((end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart));
  std::lock_guard<dxmt::mutex> lock(source_libraries::Mutex());
  source_libraries::Map().emplace(key, source_libraries::Built{library, uint64_t(response.library.size())});
  return library;
}

// What the trace lines name as the response's library: its SHA-1 and length (for text kept by content, the text's own).
inline uint64_t
LibraryId(const Response &response, char sha1[41]) {
  if (response.kind == Kind::Source && response.text[0]) {
    std::memcpy(sha1, response.text, 41);
    return response.text_bytes;
  }
  Sha1Hex(response.library.data(), response.library.size(), sha1);
  return response.library.size();
}

} // namespace dxmt::shader_source
