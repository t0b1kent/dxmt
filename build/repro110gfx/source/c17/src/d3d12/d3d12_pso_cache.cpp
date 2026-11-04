#include "d3d12_pso_work.hpp"
// MacRunner pso-cache 17.10.2026 — stage 1: manifest, replay before the first frame, MTLBinaryArchive, chain version,
// in-process ready pipeline objects, translated stages (AIR) on disk.
#include "d3d12_pso_cache.hpp"
#include "d3d12_device.hpp"
#include "d3d12_shader_source.hpp"
#include "airconv_chain_version.h"
#include "airconv_public.h"
#include "util_env.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dxmt::pso_cache {

namespace {

thread_local bool g_replaying = false;
struct ReplayStart { PipelineCache *cache; MTLD3D12Device *device; HMODULE module; };

// Phase timing of the creation running on this thread (QueryPerformanceCounter ticks).
struct Timing {
  bool active = false;
  LONGLONG start = 0, key = 0, refl = 0, air = 0, lookup = 0, create = 0;
  unsigned refl_hits = 0, refl_misses = 0, air_hits = 0, air_misses = 0, archive_hits = 0, archive_misses = 0;
  LONGLONG extra[unsigned(TimingSlot::Count)] = {};
};
thread_local Timing g_timing;

// Bytecode digests of the creation running on this thread (from its Key), so a stage is hashed once per creation.
struct CurrentDigests {
  const void *ptr[5] = {};
  size_t size[5] = {};
  Sha1Digest digest[5] = {};
  unsigned count = 0;
};
thread_local CurrentDigests g_digests;

// Identity of a shader container: the 16-byte checksum the compiler writes into the DXBC header (the D3D12 runtime validates
// it, so a mismatch never reaches us), widened with the container size; zero checksum (validator off) falls back to SHA-1.
Sha1Digest
ContainerDigest(const void *data, size_t size) {
  const uint8_t *b = static_cast<const uint8_t *>(data);
  if (size >= 32 && std::memcmp(b, "DXBC", 4) == 0) {
    uint32_t total;
    std::memcpy(&total, b + 24, 4);
    bool nonzero = false;
    for (unsigned i = 4; i < 20; ++i) nonzero |= b[i] != 0;
    if (nonzero && total == size) {
      Sha1Digest d{};
      std::memcpy(d.data, b + 4, 16);
      std::memcpy(d.data + 16, &total, 4);
      return d;
    }
  }
  return Sha1HashState::compute(data, size);
}

Sha1Digest
DigestOf(const void *data, size_t size) {
  for (unsigned i = 0; i < g_digests.count; ++i)
    if (g_digests.ptr[i] == data && g_digests.size[i] == size) return g_digests.digest[i];
  return ContainerDigest(data, size);
}

LONGLONG
Now() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

double
Micros(LONGLONG ticks) {
  static LONGLONG hz = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart ? f.QuadPart : 1; }();
  return double(ticks) * 1e6 / double(hz);
}

struct Phase {
  LONGLONG *slot;
  LONGLONG begin;
  explicit Phase(LONGLONG *s) : slot(s), begin(g_timing.active ? Now() : 0) {}
  ~Phase() { if (g_timing.active) *slot += Now() - begin; }
};

} // namespace

TimingScope::TimingScope(TimingSlot s) : begin(g_timing.active ? Now() : 0), slot(s) {}

void
TimingScope::End() {
  if (done) return;
  done = true;
  if (g_timing.active) g_timing.extra[unsigned(slot)] += Now() - begin;
}

namespace {

constexpr uint8_t kMagic[8] = {'M', 'R', 'P', 'S', 'O', '1', 0, 0};
constexpr uint32_t kFormatVersion = 1;
constexpr uint8_t kKindGraphics = 1, kKindCompute = 2;
constexpr DWORD kMaintenancePollMs = 1000;
constexpr ULONGLONG kSerializeIdleMs = 5000;      // after the last new pipeline
constexpr ULONGLONG kSerializeMaxDelayMs = 120000; // while pipelines keep coming
constexpr uint64_t kMaxFileBytes = 1ull << 28;

std::string
Sanitize(const std::string &s, size_t max) {
  std::string out;
  for (char c : s) {
    if (out.size() >= max) break;
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '_';
    out.push_back(ok ? c : '_');
  }
  return out.empty() ? std::string("unknown") : out;
}

struct Writer {
  std::vector<uint8_t> out;
  void u8(uint8_t v) { out.push_back(v); }
  void u32(uint32_t v) { const uint8_t *p = reinterpret_cast<const uint8_t *>(&v); out.insert(out.end(), p, p + 4); }
  void u64(uint64_t v) { const uint8_t *p = reinterpret_cast<const uint8_t *>(&v); out.insert(out.end(), p, p + 8); }
  void bytes(const void *p, size_t n) { const uint8_t *b = static_cast<const uint8_t *>(p); out.insert(out.end(), b, b + n); }
  void digest(const Sha1Digest &d) { bytes(d.data, sizeof(d.data)); }
  void str(const char *s) { const uint32_t n = uint32_t(std::strlen(s)); u32(n); bytes(s, n); }
};

struct Reader {
  const uint8_t *p, *end;
  bool ok = true;
  Reader(const uint8_t *data, size_t size) : p(data), end(data + size) {}
  bool need(size_t n) { if (size_t(end - p) < n) { ok = false; return false; } return true; }
  uint8_t u8() { if (!need(1)) return 0; return *p++; }
  uint32_t u32() { if (!need(4)) return 0; uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; }
  void bytes(void *dst, size_t n) { if (!need(n)) return; std::memcpy(dst, p, n); p += n; }
  Sha1Digest digest() { Sha1Digest d{}; bytes(d.data, sizeof(d.data)); return d; }
  template <class T> T pod() { T v{}; bytes(&v, sizeof(T)); return v; }
};

bool
IsZero(const Sha1Digest &d) {
  for (uint8_t b : d.data) if (b) return false;
  return true;
}

Sha1Digest
HashOrZero(const D3D12_SHADER_BYTECODE &code) {
  if (!code.pShaderBytecode || !code.BytecodeLength) return Sha1Digest{};
  return DigestOf(code.pShaderBytecode, code.BytecodeLength);
}

void
RememberDigests(const D3D12_SHADER_BYTECODE *const *stages, const Sha1Digest *digests, unsigned count) {
  g_digests = CurrentDigests{};
  for (unsigned i = 0; i < count && i < 5; ++i) {
    if (!stages[i]->pShaderBytecode || !stages[i]->BytecodeLength) continue;
    g_digests.ptr[g_digests.count] = stages[i]->pShaderBytecode;
    g_digests.size[g_digests.count] = stages[i]->BytecodeLength;
    g_digests.digest[g_digests.count] = digests[i];
    ++g_digests.count;
  }
}

bool
RootBlob(ID3D12RootSignature *root, const void *&blob, UINT &size) {
  blob = nullptr; size = 0;
  if (!root) return false;
  size = static_cast<MTLD3D12RootSignature *>(root)->GetBlob(&blob);
  return blob && size;
}

// Typed serialization of the translator argument chain. Unknown node types make the stage uncacheable.
bool
SerializeArguments(const SM50_SHADER_COMPILATION_ARGUMENT_DATA *node, Writer &w) {
  for (unsigned guard = 0; node; node = static_cast<const SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(node->next), ++guard) {
    if (guard > 32) return false;
    w.u32(uint32_t(node->type));
    switch (node->type) {
    case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: {
      const auto *d = reinterpret_cast<const SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *>(node);
      w.u32(d->num_output_slots); w.u32(d->num_elements);
      for (unsigned i = 0; i < 4; ++i) w.u32(d->strides[i]);
      if (d->num_elements > 1024) return false;
      for (unsigned i = 0; i < d->num_elements; ++i) w.bytes(&d->elements[i], sizeof(SM50_STREAM_OUTPUT_ELEMENT));
      break;
    }
    case SM50_SHADER_COMMON: {
      const auto *d = reinterpret_cast<const SM50_SHADER_COMMON_DATA *>(node);
      w.u32(uint32_t(d->metal_version)); w.u32(uint32_t(d->flags));
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      const auto *d = reinterpret_cast<const SM50_SHADER_PSO_PIXEL_SHADER_DATA *>(node);
      w.u32(d->sample_mask); w.u8(d->dual_source_blending); w.u8(d->disable_depth_output); w.u32(d->unorm_output_reg_mask);
      for (unsigned i = 0; i < 8; ++i) w.u32(d->pixel_formats[i]);
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      const auto *d = reinterpret_cast<const SM50_SHADER_IA_INPUT_LAYOUT_DATA *>(node);
      w.u32(uint32_t(d->index_buffer_format)); w.u32(d->slot_mask); w.u32(d->num_elements);
      if (d->num_elements > 64) return false;
      for (unsigned i = 0; i < d->num_elements; ++i) w.bytes(&d->elements[i], sizeof(SM50_IA_INPUT_ELEMENT));
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      const auto *d = reinterpret_cast<const SM50_SHADER_GS_PASS_THROUGH_DATA *>(node);
      w.u32(d->DataEncoded); w.u8(d->RasterizationDisabled);
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      const auto *d = reinterpret_cast<const SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *>(node);
      w.u8(d->strip_topology);
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      const auto *d = reinterpret_cast<const SM50_SHADER_PSO_TESSELLATOR_DATA *>(node);
      w.u32(d->max_potential_tess_factor);
      break;
    }
    case SM50_SHADER_ROOT_SIGNATURE:
    case SM50_SHADER_ROOT_SIGNATURE2: {
      const auto *d = reinterpret_cast<const SM50_SHADER_ROOT_SIGNATURE_DATA *>(node);
      if (!d->bytecode || !d->bytecode_length) return false;
      w.digest(DigestOf(d->bytecode, d->bytecode_length));   // a serialized root signature is a DXBC container as well
      break;
    }
    case SM50_SHADER_PS_INPUT_LINKAGE: {
      const auto *d = reinterpret_cast<const SM50_SHADER_PS_INPUT_LINKAGE_DATA *>(node);
      if (d->count > 32) return false;
      w.u32(d->count);
      for (unsigned i = 0; i < d->count; ++i) w.bytes(&d->entries[i], sizeof(SM50_SHADER_PS_INPUT_LINK));
      break;
    }
    case SM50_SHADER_TESS_SPLIT_WORKLOAD:
    case SM50_SHADER_TESS_VERTEX_LINKAGE:
      break;   // acknowledgement-only nodes: the type is the whole content
    default:
      return false;
    }
  }
  return true;
}

} // namespace

bool
PipelineCache::Replaying() {
  return g_replaying;
}

std::unique_ptr<PipelineCache>
PipelineCache::Open(MTLD3D12Device *device, WMT::Device metal) {
  if (env::getEnvVar("MACRUNNER_DX12_PSO_CACHE") != "1") return nullptr;
  std::unique_ptr<PipelineCache> cache(new PipelineCache());
  cache->device_ = device;
  uint64_t os_major = 0, os_minor = 0, os_patch = 0;
  WMTGetOSVersion(&os_major, &os_minor, &os_patch);
  const std::string gpu = Sanitize(metal.name().getUTF8String(), 24);
  // Everything that changes emitted code or which pipelines get built: the translator sources (build time, both halves),
  // the loaded unix winemetal's fingerprint, and the emission/provider switches present in the environment.
  char fingerprint[96] = {};
  WMTGetBuildFingerprint(fingerprint, sizeof(fingerprint));
  std::string switches;
  if (wchar_t *block = GetEnvironmentStringsW()) {
    std::vector<std::wstring> vars;
    for (wchar_t *entry = block; *entry; entry += wcslen(entry) + 1) {
      std::wstring e(entry);
      // Emission switches by family (future ones included); pure trace switches and the verifier change no code.
      const size_t eq = e.find(L'=');
      const std::wstring name = e.substr(0, eq == std::wstring::npos ? e.size() : eq);
      const bool trace_only = name.size() > 6 && name.compare(name.size() - 6, 6, L"_TRACE") == 0;
      const bool family = !trace_only && name != L"AIRCONV_VERIFY" &&
                          (e.rfind(L"MACRUNNER_AIRCONV_", 0) == 0 || e.rfind(L"AIRCONV_", 0) == 0);
      // Layer switches whose value reaches a pipeline: which provider/library builds it, substituted DXIL, native CS.
      // Audited 17.10 15:40 over every environment read (HANDOFF-PSO-CACHE.md, «Переключатели среды»).
      const bool named = e.rfind(L"MACRUNNER_DX12_SHADER_PATH=", 0) == 0 || e.rfind(L"MACRUNNER_DX12_OWN_SHADER_DIR=", 0) == 0 ||
                         e.rfind(L"MACRUNNER_DX12_OWN_SHADER_KIND=", 0) == 0 || e.rfind(L"MACRUNNER_DX12_DXIL_ARTIFACT_DIR=", 0) == 0 ||
                         e.rfind(L"MACRUNNER_DX12_NATIVE_WAVE=", 0) == 0 || e.rfind(L"MACRUNNER_DX12_OWN_FALLBACK=", 0) == 0;
      if (family || named) vars.push_back(e);
    }
    FreeEnvironmentStringsW(block);
    std::sort(vars.begin(), vars.end());
    for (const auto &v : vars) {
      std::string narrow(v.size(), '?');
      for (size_t i = 0; i < v.size(); ++i) narrow[i] = v[i] < 128 ? char(v[i]) : '?';
      switches += narrow + "\n";
    }
  }
  // The layer itself (function names, pipeline descriptions, pack formats) is part of the key: a rebuilt d3d12.dll that
  // names functions differently must not inherit the archives of the previous one.
  const std::string mixed = std::string(fingerprint) + "\n" + switches + "\n" + D3D12_LAYER_CHAIN_VERSION;
  const std::string mix = Sha1HashState::compute(mixed.data(), mixed.size()).string().substr(0, 8);
  const std::string key = str::format(AIRCONV_CHAIN_VERSION, "-", mix, "-", os_major, ".", os_minor, ".", os_patch, "-", gpu);
  cache->directory_ = cache->OpenDirectory(key);
  if (!cache->directory_) return nullptr;
  {
    std::string one_line = switches;
    for (char &c : one_line) if (c == '\n') c = ' ';
    shader_source::Trace("dx12_pso_cache version airconv=%s winemetal=%s layer=%s switches=[%s] mix=%s\n", AIRCONV_CHAIN_VERSION,
                         fingerprint, D3D12_LAYER_CHAIN_VERSION, one_line.c_str(), mix.c_str());
  }
  const std::string root = cache->dir_.substr(0, cache->dir_.size() - key.size() - 1);
  cache->CleanupVersions(root, key);
  if (const std::string n = env::getEnvVar("MACRUNNER_DX12_PSO_COMPACT_AT"); !n.empty()) {
    const int v = std::atoi(n.c_str());
    cache->compact_at_ = unsigned(v < 2 ? 2 : v > 32 ? 32 : v);
  }
  const bool archive = env::getEnvVar("MACRUNNER_DX12_PSO_ARCHIVE") == "1" && cache->OpenArchives(metal);
  cache->replay_enabled_ = cache->directory_ && env::getEnvVar("MACRUNNER_DX12_PSO_REPLAY") == "1";
  cache->air_enabled_ = cache->directory_ && env::getEnvVar("MACRUNNER_DX12_PSO_AIR") == "1";
  cache->refl_enabled_ = cache->directory_ && env::getEnvVar("MACRUNNER_DX12_PSO_REFL") == "1";
  cache->objects_enabled_ = env::getEnvVar("MACRUNNER_DX12_PSO_OBJECTS") == "1";
  cache->timing_enabled_ = env::getEnvVar("MACRUNNER_DX12_PSO_TIMING") == "1";
  if (const std::string threads = env::getEnvVar("MACRUNNER_DX12_PSO_REPLAY_THREADS"); !threads.empty()) {
    const int n = std::atoi(threads.c_str());
    cache->threads_count_ = unsigned(n < 1 ? 1 : n > 4 ? 4 : n);
  }
  if (cache->directory_) { cache->LoadManifest(); cache->LoadPackIndex(); }
  cache->wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (cache->wake_)
    cache->maintenance_ = CreateThread(nullptr, 0, &PipelineCache::MaintenanceThreadMain, cache.get(), 0, nullptr);
  if (cache->compact_ && !cache->replay_enabled_) cache->compact_ = false;   // compaction rides on the replay
  shader_source::Trace("dx12_pso_cache open key=%s records=%u archives=%u compact=%u serial=%s replay=%u air=%u objects=%u threads=%u dir=%s\n",
                       key.c_str(), unsigned(cache->entries_.size()), unsigned(cache->lookups_.size()), unsigned(cache->compact_),
                       archive ? cache->serial_file_.c_str() : "none", unsigned(cache->replay_enabled_),
                       unsigned(cache->air_enabled_), unsigned(cache->objects_enabled_), cache->threads_count_, cache->dir_.c_str());
  return cache;
}

PipelineCache::~PipelineCache() {
  Shutdown();
}

bool
PipelineCache::OpenDirectory(const std::string &key) {
  std::string base = env::getEnvVar("MACRUNNER_DX12_PSO_CACHE_DIR");
  if (base.empty()) base = str::format("dxmt/", env::getExeName(), "/pso");
  if (base.back() != '/') base += '/';
  dir_ = base + key + "/";
  {
    const std::string stamp = str::format(static_cast<unsigned long long>(GetTickCount64()), "\n");
    WMTCacheFileWrite((dir_ + "used").c_str(), stamp.data(), stamp.size(), 0);   // mtime of the version = last use
  }
  // Probe the directory by writing the manifest header when the file does not exist yet.
  if (!WMTCacheFileRead((dir_ + "manifest.bin").c_str(), nullptr, 0)) {
    Writer header;
    header.bytes(kMagic, sizeof(kMagic));
    header.u32(kFormatVersion);
    header.u32(0);
    if (!WMTCacheFileWrite((dir_ + "manifest.bin").c_str(), header.out.data(), header.out.size(), 0)) {
      shader_source::Trace("dx12_pso_cache directory=failed dir=%s\n", dir_.c_str());
      dir_.clear();
      return false;
    }
  }
  return true;
}

void
PipelineCache::CleanupVersions(const std::string &root, const std::string &key) {
  std::vector<char> listing(64 * 1024);
  const uint64_t len = WMTCacheDirList(root.c_str(), listing.data(), listing.size());
  if (!len) return;
  struct Version { std::string name; uint64_t bytes; long long mtime; };
  std::vector<Version> others;
  uint64_t current_bytes = 0;
  std::string line;
  for (size_t i = 0; i < len && i < listing.size() - 1; ++i) {
    if (listing[i] != '\n') { line.push_back(listing[i]); continue; }
    const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 == std::string::npos ? 0 : t1 + 1);
    if (t1 != std::string::npos && t2 != std::string::npos) {
      Version v{line.substr(0, t1), std::strtoull(line.c_str() + t1 + 1, nullptr, 10), std::strtoll(line.c_str() + t2 + 1, nullptr, 10)};
      if (v.name == key) current_bytes = v.bytes; else others.push_back(v);
    }
    line.clear();
  }
  std::sort(others.begin(), others.end(), [](const Version &a, const Version &b) { return a.mtime > b.mtime; });
  uint64_t ceiling_mb = 2048;
  if (const std::string n = env::getEnvVar("MACRUNNER_DX12_PSO_MAX_MB"); !n.empty()) ceiling_mb = std::strtoull(n.c_str(), nullptr, 10);
  unsigned removed = 0;
  uint64_t removed_bytes = 0, kept_bytes = 0;
  std::string kept;
  for (size_t i = 0; i < others.size(); ++i) {
    const bool keep = i == 0 && (current_bytes + others[i].bytes) <= ceiling_mb * 1024 * 1024;
    if (keep) { kept = others[i].name; kept_bytes = others[i].bytes; continue; }
    if (WMTCacheRemoveTree((root + others[i].name).c_str())) { ++removed; removed_bytes += others[i].bytes; }
  }
  if (!others.empty())
    shader_source::Trace("dx12_pso_cache cleanup current_mb=%.1f kept=%s kept_mb=%.1f removed=%u removed_mb=%.1f ceiling_mb=%llu\n",
                         current_bytes / 1048576.0, kept.empty() ? "none" : kept.c_str(), kept_bytes / 1048576.0, removed,
                         removed_bytes / 1048576.0, static_cast<unsigned long long>(ceiling_mb));
}

bool
PipelineCache::OpenArchives(WMT::Device metal) {
  std::vector<uint8_t> index;
  std::vector<std::string> names;
  if (ReadFileBytes(dir_ + "archives.txt", index)) {
    std::string line;
    for (uint8_t c : index) {
      if (c == '\n') { if (!line.empty()) names.push_back(line); line.clear(); }
      else if (c != '\r') line.push_back(char(c));
    }
    if (!line.empty()) names.push_back(line);
  }
  if (names.size() > 32) names.erase(names.begin(), names.end() - 32);   // newest last; bound the lookup array
  unsigned rejected = 0;
  for (const std::string &name : names) {
    WMT::Reference<WMT::Error> err;
    auto archive = metal.newBinaryArchive((dir_ + name).c_str(), err);
    if (!archive || err) { ++rejected; continue; }   // stale (another OS/GPU), damaged or gone: dropped at the next index rewrite
    lookup_handles_.push_back(archive.handle);
    lookups_.push_back(std::move(archive));
    archive_files_.push_back(name);
  }
  {
    WMT::Reference<WMT::Error> err;
    serial_ = metal.newBinaryArchive(nullptr, err);
    if (!serial_) {
      shader_source::Trace("dx12_pso_cache archive=failed error=%.200s\n", err ? err.description().getUTF8String().c_str() : "none");
      return !lookups_.empty();
    }
  }
  serial_file_ = str::format("archive-", static_cast<unsigned long long>(GetTickCount64()), "-", GetCurrentProcessId(), ".metallib");
  compact_ = lookups_.size() >= compact_at_;
  if (rejected || names.size() != archive_files_.size())
    shader_source::Trace("dx12_pso_cache archives listed=%u loaded=%u rejected=%u\n", unsigned(names.size()),
                         unsigned(lookups_.size()), rejected);
  return true;
}

template <class Info, class Fn>
auto
PipelineCache::CreateThroughArchives(Info &info, WMT::Error &err, Fn &&create) {
  // The replay in compaction mode compiles everything anew so that serial_ ends up holding the whole manifest.
  const bool try_lookup = !lookup_handles_.empty() && !(compact_ && g_replaying);
  if (try_lookup) {
    info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
    info.binary_archives_for_lookup.set(lookup_handles_.data());
    info.num_binary_archives_for_lookup = uint8_t(lookup_handles_.size());
    info.fail_on_binary_archive_miss = true;
    WMT::Reference<WMT::Error> probe;   // the miss error is expected and dropped
    LONGLONG begin = g_timing.active ? Now() : 0;
    auto pso = create(info, static_cast<WMT::Error &>(probe));
    if (g_timing.active) g_timing.lookup += Now() - begin;
    // The miss error is ours to release: the unix side hands errors over retained (err_out ownership, winemetal.h).
    if (pso) { ++archive_hits_; ++g_timing.archive_hits; return pso; }
    ++archive_misses_; ++g_timing.archive_misses;
  }
  info.binary_archive_for_serialization = serial_ ? serial_.handle : NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
  LONGLONG begin = g_timing.active ? Now() : 0;
  auto pso = create(info, err);
  if (g_timing.active) g_timing.create += Now() - begin;
  if (pso && serial_) { ++archive_added_; dirty_ = true; last_store_ms_ = GetTickCount64(); }
  return pso;
}

WMT::Reference<WMT::RenderPipelineState>
PipelineCache::CreateRender(WMT::Device metal, WMTRenderPipelineInfo &info, WMT::Error &err) {
  return CreateThroughArchives(info, err, [&](WMTRenderPipelineInfo &i, WMT::Error &e) { return PSO_BACKEND(metal.newRenderPipelineState(i, e)); });
}

WMT::Reference<WMT::RenderPipelineState>
PipelineCache::CreateMesh(WMT::Device metal, WMTMeshRenderPipelineInfo &info, WMT::Error &err) {
  return CreateThroughArchives(info, err, [&](WMTMeshRenderPipelineInfo &i, WMT::Error &e) { return PSO_BACKEND(metal.newRenderPipelineState(i, e)); });
}

WMT::Reference<WMT::ComputePipelineState>
PipelineCache::CreateCompute(WMT::Device metal, WMTComputePipelineInfo &info, WMT::Error &err) {
  return CreateThroughArchives(info, err, [&](WMTComputePipelineInfo &i, WMT::Error &e) { return PSO_BACKEND(metal.newComputePipelineState(i, e)); });
}

bool
PipelineCache::ReadFileBytes(const std::string &path, std::vector<uint8_t> &out) const {
  // One call for the common small file (reflection, AIR of a stage); a second only when the file is larger than the guess.
  constexpr uint64_t kFirstGuess = 96 * 1024;
  out.resize(size_t(kFirstGuess));
  const uint64_t size = WMTCacheFileRead(path.c_str(), out.data(), kFirstGuess);
  if (!size || size > kMaxFileBytes) { out.clear(); return false; }
  if (size <= kFirstGuess) { out.resize(size_t(size)); return true; }
  out.resize(size_t(size));
  const uint64_t now = WMTCacheFileRead(path.c_str(), out.data(), size);
  if (now < size) { out.clear(); return false; }   // shrank or short read; a grown file still filled our buffer
  return true;
}

void
PipelineCache::LoadManifest() {
  std::vector<uint8_t> data;
  if (!ReadFileBytes(dir_ + "manifest.bin", data)) return;
  Reader r(data.data(), data.size());
  uint8_t magic[8] = {};
  r.bytes(magic, sizeof(magic));
  const uint32_t version = r.u32();
  r.u32();
  if (!r.ok || std::memcmp(magic, kMagic, sizeof(magic)) || version != kFormatVersion) {
    shader_source::Trace("dx12_pso_cache manifest=rejected bytes=%zu version=%u\n", data.size(), version);
    return;
  }
  unsigned truncated = 0;
  while (r.ok && r.p < r.end) {
    const uint32_t length = r.u32();
    const Sha1Digest id = r.digest();
    if (!r.ok || !r.need(length) || length < 1) { truncated = 1; break; }
    Entry e;
    e.id = id;
    e.body.assign(r.p, r.p + length);
    r.p += length;
    if (Sha1HashState::compute(e.body.data(), e.body.size()) != id) { truncated = 1; break; }
    if (known_.insert(id).second) entries_.push_back(std::move(e));
  }
  if (truncated)
    shader_source::Trace("dx12_pso_cache manifest=truncated_tail records=%u\n", unsigned(entries_.size()));
}

bool
PipelineCache::WriteBlob(const Sha1Digest &id, const void *data, size_t size, unsigned &written) {
  const std::string path = dir_ + "blobs/" + id.string() + ".bin";
  if (WMTCacheFileRead(path.c_str(), nullptr, 0)) return true;
  if (!WMTCacheFileWrite(path.c_str(), data, size, 0)) return false;
  ++written;
  return true;
}

bool
PipelineCache::BuildKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, Key &key) const {
  Phase phase(&g_timing.key);
  if (desc.StreamOutput.NumEntries || desc.StreamOutput.NumStrides || !desc.pRootSignature) return false;
  const void *root_blob; UINT root_size;
  if (!RootBlob(desc.pRootSignature, root_blob, root_size)) return false;
  if (!desc.VS.pShaderBytecode || !desc.VS.BytecodeLength) return false;
  const D3D12_SHADER_BYTECODE *stages[5] = {&desc.VS, &desc.HS, &desc.DS, &desc.GS, &desc.PS};
  key.compute = false;
  Writer w;
  w.u8(kKindGraphics);
  g_digests = CurrentDigests{};
  for (unsigned i = 0; i < 5; ++i) { key.stages[i] = HashOrZero(*stages[i]); w.digest(key.stages[i]); }
  RememberDigests(stages, key.stages, 5);
  key.root = ContainerDigest(root_blob, root_size);
  w.digest(key.root);
  w.bytes(&desc.BlendState, sizeof(desc.BlendState));
  w.u32(desc.SampleMask);
  w.bytes(&desc.RasterizerState, sizeof(desc.RasterizerState));
  w.bytes(&desc.DepthStencilState, sizeof(desc.DepthStencilState));
  w.u32(uint32_t(desc.IBStripCutValue));
  w.u32(uint32_t(desc.PrimitiveTopologyType));
  w.u32(desc.NumRenderTargets);
  for (unsigned i = 0; i < 8; ++i) w.u32(uint32_t(desc.RTVFormats[i]));
  w.u32(uint32_t(desc.DSVFormat));
  w.u32(desc.SampleDesc.Count);
  w.u32(desc.SampleDesc.Quality);
  w.u32(desc.NodeMask);
  w.u32(uint32_t(desc.Flags));
  const uint32_t elements = desc.InputLayout.pInputElementDescs ? desc.InputLayout.NumElements : 0;
  if (elements > 32) return false;
  w.u32(elements);
  for (uint32_t i = 0; i < elements; ++i) {
    const auto &e = desc.InputLayout.pInputElementDescs[i];
    w.str(e.SemanticName ? e.SemanticName : "");
    w.u32(e.SemanticIndex);
    w.u32(uint32_t(e.Format));
    w.u32(e.InputSlot);
    w.u32(e.AlignedByteOffset);
    w.u32(uint32_t(e.InputSlotClass));
    w.u32(e.InstanceDataStepRate);
  }
  key.body = std::move(w.out);
  key.id = Sha1HashState::compute(key.body.data(), key.body.size());
  return true;
}

bool
PipelineCache::BuildKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, Key &key) const {
  Phase phase(&g_timing.key);
  if (!desc.pRootSignature || !desc.CS.pShaderBytecode || !desc.CS.BytecodeLength) return false;
  const void *root_blob; UINT root_size;
  if (!RootBlob(desc.pRootSignature, root_blob, root_size)) return false;
  key.compute = true;
  Writer w;
  w.u8(kKindCompute);
  g_digests = CurrentDigests{};
  key.stages[0] = HashOrZero(desc.CS);
  for (unsigned i = 1; i < 5; ++i) key.stages[i] = Sha1Digest{};
  const D3D12_SHADER_BYTECODE *cs_stage[1] = {&desc.CS};
  RememberDigests(cs_stage, key.stages, 1);
  key.root = ContainerDigest(root_blob, root_size);
  w.digest(key.stages[0]);
  w.digest(key.root);
  w.u32(desc.NodeMask);
  w.u32(uint32_t(desc.Flags));
  key.body = std::move(w.out);
  key.id = Sha1HashState::compute(key.body.data(), key.body.size());
  return true;
}

ID3D12PipelineState *
PipelineCache::Lookup(const Key &key) {
  if (!objects_enabled_) return nullptr;
  ID3D12PipelineState *object = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = objects_.find(key.id);
    if (it == objects_.end()) return nullptr;
    object = it->second;
    ++hits_;
  }
  shader_source::Trace("dx12_pso_cache hit kind=%s id=%s replaying=%u\n", key.compute ? "compute" : "graphics",
                       key.id.string().c_str(), unsigned(g_replaying));
  return object;
}

void
PipelineCache::KeepObject(const Key &key, IUnknown *created) {
  if (!objects_enabled_ || !created) return;
  ID3D12PipelineState *object = nullptr;
  if (FAILED(created->QueryInterface(__uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&object))) || !object) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!objects_.emplace(key.id, object).second) object->Release();   // a parallel creation won; keep the first
}

bool
PipelineCache::HoldsOnly(uint32_t device_refs) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !objects_.empty() && objects_.size() == device_refs;
}

void
PipelineCache::DropObjects() {
  // The last Release below may destroy the device and with it this cache: nothing of `this` is touched after the loop.
  std::vector<ID3D12PipelineState *> objects;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    objects.reserve(objects_.size());
    for (auto &object : objects_) objects.push_back(object.second);
    objects_.clear();
  }
  shader_source::Trace("dx12_pso_cache drop_objects count=%u\n", unsigned(objects.size()));
  for (ID3D12PipelineState *object : objects) object->Release();
}

void
PipelineCache::Append(const Key &key, const char *kind, unsigned new_blobs) {
  Writer w;
  w.u32(uint32_t(key.body.size()));
  w.digest(key.id);
  w.bytes(key.body.data(), key.body.size());
  const bool ok = WMTCacheFileWrite((dir_ + "manifest.bin").c_str(), w.out.data(), w.out.size(), 1);
  ++recorded_new_;
  shader_source::Trace("dx12_pso_cache record kind=%s id=%s blobs_written=%u total_new=%u ok=%u\n", kind,
                       key.id.string().c_str(), new_blobs, recorded_new_, unsigned(ok));
}

void
PipelineCache::Store(const Key &key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, IUnknown *created) {
  KeepObject(key, created);
  if (g_replaying || !directory_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (known_.count(key.id)) return;
  unsigned new_blobs = 0;
  const D3D12_SHADER_BYTECODE *stages[5] = {&desc.VS, &desc.HS, &desc.DS, &desc.GS, &desc.PS};
  for (unsigned i = 0; i < 5; ++i) {
    if (IsZero(key.stages[i])) continue;
    if (!WriteBlob(key.stages[i], stages[i]->pShaderBytecode, stages[i]->BytecodeLength, new_blobs)) { ++skipped_; return; }
  }
  const void *root_blob; UINT root_size;
  if (!RootBlob(desc.pRootSignature, root_blob, root_size) || !WriteBlob(key.root, root_blob, root_size, new_blobs)) { ++skipped_; return; }
  known_.insert(key.id);
  Append(key, "graphics", new_blobs);
}

void
PipelineCache::Store(const Key &key, const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, IUnknown *created) {
  KeepObject(key, created);
  if (g_replaying || !directory_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (known_.count(key.id)) return;
  unsigned new_blobs = 0;
  const void *root_blob; UINT root_size;
  if (!WriteBlob(key.stages[0], desc.CS.pShaderBytecode, desc.CS.BytecodeLength, new_blobs) ||
      !RootBlob(desc.pRootSignature, root_blob, root_size) || !WriteBlob(key.root, root_blob, root_size, new_blobs)) { ++skipped_; return; }
  known_.insert(key.id);
  Append(key, "compute", new_blobs);
}

bool
PipelineCache::AirKey(const char *function, const D3D12_SHADER_BYTECODE *shaders, unsigned count,
                      const SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, Sha1Digest &key) const {
  if (!air_enabled_ || !function) return false;
  Phase phase(&g_timing.extra[unsigned(TimingSlot::AirKey)]);
  Writer w;
  w.str("air1");
  w.str(function);
  w.u32(count);
  for (unsigned i = 0; i < count; ++i) {
    if (!shaders[i].pShaderBytecode || !shaders[i].BytecodeLength) return false;
    w.digest(DigestOf(shaders[i].pShaderBytecode, shaders[i].BytecodeLength));
  }
  if (!SerializeArguments(args, w)) return false;
  key = Sha1HashState::compute(w.out.data(), w.out.size());
  return true;
}

namespace {
constexpr char kPackIndexMagic[8] = {'M', 'R', 'P', 'K', 'I', 'D', 'X', '6'};
constexpr size_t kPackHeader = 16, kPackSignatureAt = 32 + sizeof(MTL_SHADER_REFLECTION),
                 kPackEntry = kPackSignatureAt + kInputSignatureBytes;

// Block: count (0xFF = none), then per parameter reg, system_value, semantic_index, name length, name bytes.
void
EncodeInputSignature(const std::vector<InputParameter> *params, uint8_t *block) {
  std::memset(block, 0, kInputSignatureBytes);
  block[0] = 0xFF;
  if (!params || params->size() > 32) return;
  size_t at = 1;
  for (const auto &p : *params) {
    const size_t len = strnlen(p.name, sizeof(p.name));
    if (!len || at + 4 + len > kInputSignatureBytes) return;
    block[at++] = p.reg;
    block[at++] = p.system_value ? 1 : 0;
    block[at++] = p.semantic_index;
    block[at++] = uint8_t(len);
    std::memcpy(block + at, p.name, len);
    at += len;
  }
  block[0] = uint8_t(params->size());
}

bool
DecodeInputSignature(const uint8_t *block, std::vector<InputParameter> &out) {
  if (block[0] == 0xFF || block[0] > 32) return false;
  out.clear();
  size_t at = 1;
  for (unsigned i = 0; i < block[0]; ++i) {
    if (at + 4 > kInputSignatureBytes) return false;
    InputParameter p{};
    p.reg = block[at];
    p.system_value = block[at + 1] != 0;
    p.semantic_index = block[at + 2];
    const unsigned len = block[at + 3];
    at += 4;
    if (!len || len >= sizeof(p.name) || at + len > kInputSignatureBytes) return false;
    std::memcpy(p.name, block + at, len);
    at += len;
    out.push_back(p);
  }
  return true;
}
} // namespace

bool
PipelineCache::LoadPackIndex() {
  std::vector<uint8_t> index;
  const bool present = ReadFileBytes(dir_ + "pack.idx", index);
  if (!present) {
    uint8_t header[kPackHeader] = {};
    std::memcpy(header, kPackIndexMagic, 8);
    // Native mode 3 locks the inode, initializes only an empty file and verifies an existing header.
    // This happens before any workers. It is atomic across cache instances and processes as well.
    pack_usable_ = !WMTCacheFileRead((dir_ + "pack.idx").c_str(), nullptr, 0) &&
        WMTCacheFileWrite((dir_ + "pack.idx").c_str(), header, sizeof(header), 3);
    if (!pack_usable_) shader_source::Trace("dx12_pso_cache pack=rejected reason=read_or_init\n");
    return pack_usable_;
  }
  if (index.size() < kPackHeader || std::memcmp(index.data(), kPackIndexMagic, 8) != 0 ||
      (index.size() - kPackHeader) % kPackEntry) {
    // Do not reinterpret a shifted record as reflection/AIR, or unlink a pack another process may still read.
    shader_source::Trace("dx12_pso_cache pack=rejected reason=header_or_alignment bytes=%zu\n", index.size());
    return false;
  }
  const size_t whole = (index.size() - kPackHeader) / kPackEntry;
  for (size_t i = 0; i < whole; ++i) {
    const uint8_t kind = index[kPackHeader + i * kPackEntry];
    if (kind != 1 && kind != 2) {
      shader_source::Trace("dx12_pso_cache pack=rejected reason=record_kind entry=%zu kind=%u\n", i, unsigned(kind));
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(pack_mutex_);
  for (size_t i = 0; i < whole; ++i) {
    const uint8_t *e = index.data() + kPackHeader + i * kPackEntry;
    Sha1Digest id; std::memcpy(id.data, e + 4, 20);
    if (e[0] == 1) {
      MTL_SHADER_REFLECTION refl; std::memcpy(&refl, e + 32, sizeof(refl));
      pack_refl_[id] = refl;
      std::vector<InputParameter> inputs;
      if (DecodeInputSignature(e + kPackSignatureAt, inputs)) pack_sig_[id] = std::move(inputs);
    } else {
      PackEntry entry; std::memcpy(&entry.offset, e + 24, 8); std::memcpy(&entry.length, e + 32, 4);
      pack_air_[id] = entry;
    }
  }
  pack_usable_ = true;
  shader_source::Trace("dx12_pso_cache pack=loaded records=%zu air=%zu reflection=%zu\n", whole, pack_air_.size(), pack_refl_.size());
  return true;
}

bool
PipelineCache::PackAppendIndex(const uint8_t *entry) {
  // Header creation must never be a separate check/append on the worker path.
  return pack_usable_ && WMTCacheFileAppend((dir_ + "pack.idx").c_str(), entry, kPackEntry) != ~0ull;
}

bool
PipelineCache::PackLoadAir(const Sha1Digest &id, std::vector<uint8_t> &out) {
  PackEntry entry;
  {
    std::lock_guard<std::mutex> lock(pack_mutex_);
    auto it = pack_air_.find(id);
    if (it == pack_air_.end()) return false;
    entry = it->second;
  }
  if (!entry.length || entry.length > kMaxFileBytes) return false;
  out.resize(entry.length);
  return WMTCacheFileReadAt((dir_ + "pack.bin").c_str(), entry.offset, out.data(), entry.length) == entry.length;
}

WMT::Reference<WMT::Library>
PipelineCache::LoadLibrary(const Sha1Digest &key, WMT::Device metal) {
  if (!air_enabled_ || !pack_usable_) return {};
  Phase phase(&g_timing.air);
  {
    std::lock_guard<std::mutex> lock(pack_mutex_);
    auto it = libraries_.find(key);
    if (it != libraries_.end()) { ++air_hits_; ++g_timing.air_hits; return it->second; }
  }
  std::vector<uint8_t> air;
  if (!PackLoadAir(key, air)) { ++air_misses_; ++g_timing.air_misses; return {}; }
  WMT::Reference<WMT::Error> err;
  auto library = PSO_BACKEND(metal.newLibrary(WMT::MakeDispatchData(air.data(), air.size()), err));
  if (!library || err) { ++air_misses_; ++g_timing.air_misses; return {}; }
  ++air_hits_; ++g_timing.air_hits;
  std::lock_guard<std::mutex> lock(pack_mutex_);
  return libraries_.emplace(key, library).first->second;
}

void
PipelineCache::StoreAir(const Sha1Digest &key, const void *data, uint64_t size, WMT::Library library) {
  if (!air_enabled_ || !pack_usable_ || !data || !size || size > kMaxFileBytes) return;
  const uint64_t offset = WMTCacheFileAppend((dir_ + "pack.bin").c_str(), data, size);
  if (offset != ~0ull) {
    uint8_t entry[kPackEntry] = {};
    entry[0] = 2;
    std::memcpy(entry + 4, key.data, 20);
    std::memcpy(entry + 24, &offset, 8);
    const uint32_t length = uint32_t(size);
    std::memcpy(entry + 32, &length, 4);   // +32: the reflection payload area, unused for AIR entries
    if (PackAppendIndex(entry)) ++air_stored_;
  }
  std::lock_guard<std::mutex> lock(pack_mutex_);
  if (offset != ~0ull) pack_air_[key] = PackEntry{offset, uint32_t(size)};
  if (library.handle) libraries_.emplace(key, WMT::Reference<WMT::Library>(library));
}

bool
PipelineCache::LoadReflection(const D3D12_SHADER_BYTECODE &code, MTL_SHADER_REFLECTION &out) {
  if (!refl_enabled_ || !pack_usable_ || !code.pShaderBytecode || !code.BytecodeLength) return false;
  Phase phase(&g_timing.refl);
  const Sha1Digest id = DigestOf(code.pShaderBytecode, code.BytecodeLength);
  std::lock_guard<std::mutex> lock(pack_mutex_);
  auto it = pack_refl_.find(id);
  if (it == pack_refl_.end()) { ++refl_misses_; ++g_timing.refl_misses; return false; }
  out = it->second;
  ++refl_hits_; ++g_timing.refl_hits;
  return true;
}

void
PipelineCache::StoreReflection(const D3D12_SHADER_BYTECODE &code, const MTL_SHADER_REFLECTION &refl,
                               const std::vector<InputParameter> *inputs) {
  if (!refl_enabled_ || !pack_usable_ || !code.pShaderBytecode || !code.BytecodeLength) return;
  const Sha1Digest id = DigestOf(code.pShaderBytecode, code.BytecodeLength);
  uint8_t entry[kPackEntry] = {};
  entry[0] = 1;
  std::memcpy(entry + 4, id.data, 20);
  std::memcpy(entry + 32, &refl, sizeof(refl));
  EncodeInputSignature(inputs, entry + kPackSignatureAt);
  PackAppendIndex(entry);
  std::vector<InputParameter> kept;
  const bool keep = DecodeInputSignature(entry + kPackSignatureAt, kept);   // exactly what a later process will read
  std::lock_guard<std::mutex> lock(pack_mutex_);
  pack_refl_[id] = refl;
  if (keep) pack_sig_[id] = std::move(kept);
}

bool
PipelineCache::LoadInputSignature(const D3D12_SHADER_BYTECODE &code, std::vector<InputParameter> &out) {
  if (!refl_enabled_ || !code.pShaderBytecode || !code.BytecodeLength) return false;
  const Sha1Digest id = DigestOf(code.pShaderBytecode, code.BytecodeLength);
  std::lock_guard<std::mutex> lock(pack_mutex_);
  auto it = pack_sig_.find(id);
  if (it == pack_sig_.end()) return false;
  out = it->second;
  return true;
}

Sha1Digest
PipelineCache::ShaderId(const void *data, size_t size) {
  return DigestOf(data, size);
}

void
PipelineCache::TimingBegin() {
  if (!timing_enabled_) return;
  g_timing = Timing{};
  g_timing.active = true;
  g_timing.start = Now();
}

void
PipelineCache::TimingReport(const char *kind, bool object_hit, HRESULT hr) {
  if (!timing_enabled_ || !g_timing.active) return;
  const LONGLONG total = Now() - g_timing.start;
  g_timing.active = false;
  shader_source::Trace("dx12_pso_cache timing kind=%s hr=%08lx object_hit=%u total_us=%.0f key_us=%.0f refl_us=%.0f air_us=%.0f "
                       "lookup_us=%.0f create_us=%.0f refl=%u/%u air=%u/%u archive=%u/%u replaying=%u "
                       "prelude_us=%.0f validate_us=%.0f airkey_us=%.0f ia_us=%.0f fn_us=%.0f psname_us=%.0f dsso_us=%.0f store_us=%.0f\n",
                       kind, static_cast<unsigned long>(hr), unsigned(object_hit), Micros(total), Micros(g_timing.key),
                       Micros(g_timing.refl), Micros(g_timing.air), Micros(g_timing.lookup), Micros(g_timing.create),
                       g_timing.refl_hits, g_timing.refl_hits + g_timing.refl_misses, g_timing.air_hits,
                       g_timing.air_hits + g_timing.air_misses, g_timing.archive_hits, g_timing.archive_hits + g_timing.archive_misses,
                       unsigned(g_replaying), Micros(g_timing.extra[0]), Micros(g_timing.extra[1]), Micros(g_timing.extra[2]),
                       Micros(g_timing.extra[3]), Micros(g_timing.extra[4]), Micros(g_timing.extra[5]), Micros(g_timing.extra[6]),
                       Micros(g_timing.extra[7]));
}

const std::vector<uint8_t> *
PipelineCache::ReplayBlob(const Sha1Digest &id) {
  std::lock_guard<std::mutex> lock(replay_mutex_);
  auto it = blobs_.find(id);
  if (it == blobs_.end()) {
    std::vector<uint8_t> data;
    if (!ReadFileBytes(dir_ + "blobs/" + id.string() + ".bin", data) || ContainerDigest(data.data(), data.size()) != id)
      data.clear();
    it = blobs_.emplace(id, std::move(data)).first;
  }
  return it->second.empty() ? nullptr : &it->second;
}

ID3D12RootSignature *
PipelineCache::ReplayRoot(const Sha1Digest &id) {
  {
    std::lock_guard<std::mutex> lock(replay_mutex_);
    auto it = roots_.find(id);
    if (it != roots_.end()) return it->second;
  }
  const std::vector<uint8_t> *blob = ReplayBlob(id);
  ID3D12RootSignature *root = nullptr;
  if (blob)
    device_->CreateRootSignature(0, blob->data(), blob->size(), __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&root));
  std::lock_guard<std::mutex> lock(replay_mutex_);
  auto inserted = roots_.emplace(id, root);
  if (!inserted.second && root) { root->Release(); return inserted.first->second; }
  return root;
}

bool
PipelineCache::ReplayGraphics(const uint8_t *body, size_t size) {
  Reader r(body, size);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  Sha1Digest stages[5];
  for (auto &s : stages) s = r.digest();
  const Sha1Digest root = r.digest();
  desc.BlendState = r.pod<D3D12_BLEND_DESC>();
  desc.SampleMask = r.u32();
  desc.RasterizerState = r.pod<D3D12_RASTERIZER_DESC>();
  desc.DepthStencilState = r.pod<D3D12_DEPTH_STENCIL_DESC>();
  desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(r.u32());
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(r.u32());
  desc.NumRenderTargets = r.u32();
  for (unsigned i = 0; i < 8; ++i) desc.RTVFormats[i] = DXGI_FORMAT(r.u32());
  desc.DSVFormat = DXGI_FORMAT(r.u32());
  desc.SampleDesc.Count = r.u32();
  desc.SampleDesc.Quality = r.u32();
  desc.NodeMask = r.u32();
  desc.Flags = D3D12_PIPELINE_STATE_FLAGS(r.u32());
  const uint32_t elements = r.u32();
  if (!r.ok || elements > 32) return false;
  std::vector<std::string> names(elements);
  std::vector<D3D12_INPUT_ELEMENT_DESC> layout(elements);
  for (uint32_t i = 0; i < elements; ++i) {
    const uint32_t length = r.u32();
    if (!r.need(length) || length > 256) return false;
    names[i].assign(reinterpret_cast<const char *>(r.p), length);
    r.p += length;
    layout[i].SemanticName = names[i].c_str();
    layout[i].SemanticIndex = r.u32();
    layout[i].Format = DXGI_FORMAT(r.u32());
    layout[i].InputSlot = r.u32();
    layout[i].AlignedByteOffset = r.u32();
    layout[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION(r.u32());
    layout[i].InstanceDataStepRate = r.u32();
  }
  if (!r.ok || r.p != r.end) return false;
  desc.InputLayout = {elements ? layout.data() : nullptr, elements};
  D3D12_SHADER_BYTECODE *targets[5] = {&desc.VS, &desc.HS, &desc.DS, &desc.GS, &desc.PS};
  for (unsigned i = 0; i < 5; ++i) {
    if (IsZero(stages[i])) continue;
    const std::vector<uint8_t> *blob = ReplayBlob(stages[i]);
    if (!blob) return false;
    *targets[i] = {blob->data(), blob->size()};
  }
  desc.pRootSignature = ReplayRoot(root);
  if (!desc.pRootSignature) return false;
  ID3D12PipelineState *pso = nullptr;
  const HRESULT hr = device_->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&pso));
  if (pso) pso->Release();
  return SUCCEEDED(hr);
}

bool
PipelineCache::ReplayCompute(const uint8_t *body, size_t size) {
  Reader r(body, size);
  const Sha1Digest cs = r.digest(), root = r.digest();
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.NodeMask = r.u32();
  desc.Flags = D3D12_PIPELINE_STATE_FLAGS(r.u32());
  if (!r.ok || r.p != r.end) return false;
  const std::vector<uint8_t> *blob = ReplayBlob(cs);
  if (!blob) return false;
  desc.CS = {blob->data(), blob->size()};
  desc.pRootSignature = ReplayRoot(root);
  if (!desc.pRootSignature) return false;
  ID3D12PipelineState *pso = nullptr;
  const HRESULT hr = device_->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&pso));
  if (pso) pso->Release();
  return SUCCEEDED(hr);
}

void
PipelineCache::ReplayWorker() {
  g_replaying = true;
  for (;;) {
    if (stop_) break;
    const uint32_t index = next_.fetch_add(1);
    if (index >= entries_.size()) break;
    const Entry &e = entries_[index];
    const uint8_t kind = e.body.empty() ? 0 : e.body[0];
    const bool ok = kind == kKindGraphics   ? ReplayGraphics(e.body.data() + 1, e.body.size() - 1)
                    : kind == kKindCompute  ? ReplayCompute(e.body.data() + 1, e.body.size() - 1)
                                            : false;
    if (ok) ++replay_created_; else ++replay_failed_;
  }
  if (workers_alive_.fetch_sub(1) == 1) {
    const ULONGLONG ms = GetTickCount64() - replay_start_ms_;
    shader_source::Trace("dx12_pso_cache replay=done records=%u created=%u failed=%u ms=%llu air_hits=%u air_misses=%u "
                         "archive_hits=%u archive_misses=%u compact=%u stopped=%u\n",
                         unsigned(entries_.size()), replay_created_.load(), replay_failed_.load(),
                         static_cast<unsigned long long>(ms), air_hits_.load(), air_misses_.load(), archive_hits_.load(),
                         archive_misses_.load(), unsigned(compact_), unsigned(stop_.load()));
    {
      std::lock_guard<std::mutex> lock(replay_mutex_);
      for (auto &root : roots_) if (root.second) root.second->Release();
      roots_.clear();
      blobs_.clear();
    }
    if (!stop_) Serialize("replay");
  }
  g_replaying = false;
}

DWORD WINAPI
PipelineCache::ReplayThreadMain(void *self) {
  auto start = std::unique_ptr<ReplayStart>(static_cast<ReplayStart *>(self));
  auto *cache = start->cache;
  auto *device = start->device;
  const auto module = start->module;
  start.reset();
  cache->ReplayWorker();
  // No access to cache after releasing this pin: the last worker may destroy it here.
  device->Release();
  FreeLibraryAndExitThread(module, 0);
  return 0;
}

void
PipelineCache::StartReplay() {
  if (replay_started_.exchange(true)) return;
  if (!replay_enabled_ || entries_.empty()) {
    shader_source::Trace("dx12_pso_cache replay=skipped records=%u enabled=%u\n", unsigned(entries_.size()),
                         unsigned(replay_enabled_));
    return;
  }
  const unsigned count = unsigned(entries_.size()) < threads_count_ ? unsigned(entries_.size()) : threads_count_;
  replay_start_ms_ = GetTickCount64();
  workers_alive_ = count;
  for (unsigned i = 0; i < count; ++i) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&PipelineCache::ReplayThreadMain), &module)) {
      workers_alive_.fetch_sub(1); continue;
    }
    auto start = std::make_unique<ReplayStart>(ReplayStart{this, device_, module});
    device_->AddRef();
    HANDLE thread = CreateThread(nullptr, 0, &PipelineCache::ReplayThreadMain, start.get(), CREATE_SUSPENDED, nullptr);
    if (!thread) { workers_alive_.fetch_sub(1); device_->Release(); FreeLibrary(module); continue; }
    start.release();
    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    threads_.push_back(thread);
  }
  // Start together after the actual worker count is known. A failed final CreateThread must not strand the
  // replay completion/serialization path after all earlier workers have already consumed their records.
  workers_alive_ = unsigned(threads_.size());
  shader_source::Trace("dx12_pso_cache replay=start records=%u threads=%u\n", unsigned(entries_.size()),
                       unsigned(threads_.size()));
  for (HANDLE thread : threads_) ResumeThread(thread);
}

void
PipelineCache::Serialize(const char *reason) {
  if (!serial_ || !dirty_) return;
  std::lock_guard<std::mutex> lock(serialize_mutex_);
  if (!dirty_.exchange(false)) return;
  const ULONGLONG start = GetTickCount64();
  WMT::Reference<WMT::Error> err;
  const std::string path = dir_ + serial_file_;
  serial_.serialize(path.c_str(), err);
  const uint64_t bytes = err ? 0 : WMTCacheFileRead(path.c_str(), nullptr, 0);
  shader_source::Trace("dx12_pso_cache serialize reason=%s ok=%u ms=%llu added=%u bytes=%llu file=%s error=%.200s\n", reason,
                       unsigned(!err), static_cast<unsigned long long>(GetTickCount64() - start), archive_added_.load(),
                       static_cast<unsigned long long>(bytes), serial_file_.c_str(),
                       err ? err.description().getUTF8String().c_str() : "none");
  if (err) { dirty_ = true; return; }
  if (!serial_indexed_) {
    const std::string line = serial_file_ + "\n";
    serial_indexed_ = WMTCacheFileWrite((dir_ + "archives.txt").c_str(), line.data(), line.size(), 1);
  }
  if (compact_ && replay_started_ && workers_alive_ == 0) FinishCompaction();
}

void
PipelineCache::FinishCompaction() {
  // serial_ now holds every pipeline the replay created: the index shrinks to it and the old files go. Their mappings stay
  // valid for this process, so lookups_ keeps working until exit.
  const std::string line = serial_file_ + "\n";
  if (!WMTCacheFileWrite((dir_ + "archives.txt").c_str(), line.data(), line.size(), 0)) return;
  unsigned removed = 0;
  for (const std::string &name : archive_files_)
    if (name != serial_file_ && WMTCacheFileWrite((dir_ + name).c_str(), nullptr, 0, 2)) ++removed;
  shader_source::Trace("dx12_pso_cache compact=done kept=%s removed=%u\n", serial_file_.c_str(), removed);
  compact_ = false;
}

void
PipelineCache::Maintenance() {
  ULONGLONG last_serialized = GetTickCount64();
  for (;;) {
    WaitForSingleObject(wake_, kMaintenancePollMs);
    if (stop_) break;
    if (!dirty_) continue;
    const ULONGLONG now = GetTickCount64();
    if (now - last_store_ms_ >= kSerializeIdleMs || now - last_serialized >= kSerializeMaxDelayMs) {
      Serialize(now - last_store_ms_ >= kSerializeIdleMs ? "idle" : "timer");
      last_serialized = now;
    }
  }
}

DWORD WINAPI
PipelineCache::MaintenanceThreadMain(void *self) {
  static_cast<PipelineCache *>(self)->Maintenance();
  return 0;
}

void
PipelineCache::Shutdown() {
  if (stop_.exchange(true)) return;
  if (wake_) SetEvent(wake_);
  for (HANDLE thread : threads_) {
    // Destruction on the final replay worker occurs only after ReplayWorker has returned.
    if (GetThreadId(thread) != GetCurrentThreadId()) WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
  }
  threads_.clear();
  if (maintenance_) { WaitForSingleObject(maintenance_, INFINITE); CloseHandle(maintenance_); maintenance_ = nullptr; }
  if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
  {
    std::lock_guard<std::mutex> lock(replay_mutex_);
    for (auto &root : roots_) if (root.second) root.second->Release();
    roots_.clear();
    blobs_.clear();
  }
  Serialize("exit");
  shader_source::Trace("dx12_pso_cache close recorded_new=%u skipped=%u hits=%u objects=%u replay_created=%u replay_failed=%u "
                       "air_hits=%u air_misses=%u air_stored=%u refl_hits=%u refl_misses=%u archive_hits=%u archive_misses=%u archive_added=%u\n",
                       recorded_new_, skipped_, hits_, unsigned(objects_.size()), replay_created_.load(), replay_failed_.load(),
                       air_hits_.load(), air_misses_.load(), air_stored_.load(), refl_hits_.load(), refl_misses_.load(),
                       archive_hits_.load(), archive_misses_.load(), archive_added_.load());
  // The ready objects are released after the device's own teardown starts; they are plain pipeline states of this device.
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &object : objects_) object.second->Release();
  objects_.clear();
}

} // namespace dxmt::pso_cache
