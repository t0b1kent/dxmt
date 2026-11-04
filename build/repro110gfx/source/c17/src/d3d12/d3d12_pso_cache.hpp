#pragma once
// MacRunner pso-cache 17.10.2026 — stage 1 for 1.1.0: pipeline manifest (user), replay of the manifest before the first
// frame at low priority, MTLBinaryArchive in the layer cache directory, chain version in the directory name, and the
// translation cache the curator added to the stage (12:58): ready pipeline objects in process keyed by the full description,
// translated stages (AIR metallib) on disk keyed by bytecode + translation arguments.
// Design: artifacts/pso-cache-20261017/PLAN-PSO-CACHE.md; numbers: shader-prewarm-20261017/RESULT.md §2.10, §3.2, §6.
//
// One directory under $DARWIN_USER_CACHE_DIR (the gpuarchiver service writes only there), reached from the PE side through
// two winemetal calls (WMTCacheFileRead/Write): dxmt/<exe>/pso/<key>/{archives.txt, archive-*.metallib, manifest.bin, blobs/, air/},
// key = <chain version>-<os>-<gpu>.
// Environment: MACRUNNER_DX12_PSO_CACHE=0 (off), MACRUNNER_DX12_PSO_ARCHIVE=0, MACRUNNER_DX12_PSO_REPLAY=0, MACRUNNER_DX12_PSO_AIR=0,
// MACRUNNER_DX12_PSO_OBJECTS=0, MACRUNNER_DX12_PSO_REPLAY_THREADS=N (1..4, default 2), MACRUNNER_DX12_PSO_COMPACT_AT=N (archive files
// before the replay compacts them into one, default 8), MACRUNNER_DX12_PSO_MAX_MB=N (ceiling for current+previous version, 2048),
// MACRUNNER_DX12_PSO_CACHE_DIR=<unix path; relative = under the user cache dir>.
// key = <airconv chain>-<8 hex of sha1(unix winemetal fingerprint + every MACRUNNER_AIRCONV_*/AIRCONV_* switch + the shader-path
// switches of the layer)>-<os>-<gpu>: a flipped emission switch or a winemetal from another tree is a new directory.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include "Metal.hpp"
#include "sha1/sha1_util.hpp"
#include "airconv_public.h"

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA;

namespace dxmt {
class MTLD3D12Device;

namespace pso_cache {

// Full description of one public pipeline creation: stage/root digests and the serialized fixed-function part.
struct Key {
  Sha1Digest id{};            // sha1 of body — the manifest record id and the in-process object key
  Sha1Digest stages[5]{};     // VS, HS, DS, GS, PS (graphics) or CS in [0] (compute); zero = absent
  Sha1Digest root{};
  std::vector<uint8_t> body;  // record body (kind + digests + fixed function)
  bool compute = false;
};

// Phases of one creation that the cache itself does not run but whose cost decides what is left on an all-hit
// creation (MACRUNNER_DX12_PSO_TIMING=1 reports them per creation; see TimingReport).
enum class TimingSlot : unsigned { Prelude = 0, Validate, AirKey, InputLayout, Function, PixelName, DepthStencil, Store, Count };
struct TimingScope {
  explicit TimingScope(TimingSlot slot);
  ~TimingScope() { End(); }
  void End();
  LONGLONG begin;
  TimingSlot slot;
  bool done = false;
};

// One parameter of a shader's input signature in the form the pack index keeps it: exactly the fields the input
// layout needs (ExtractMTLInputLayoutElementsFrom), so a hit builds the layout without parsing the container.
struct InputParameter {
  char name[44];
  uint8_t semantic_index;
  uint8_t reg;
  bool system_value;
};
constexpr unsigned kInputSignatureBytes = 256;   // inline block per reflection entry; 0xFF count = not kept

class PipelineCache {
public:
  // nullptr when disabled or when the directory cannot be used.
  static std::unique_ptr<PipelineCache> Open(MTLD3D12Device *device, WMT::Device metal);
  ~PipelineCache();

  // Keys. false = the description is outside the cache (stream output, root without blob, no shader).
  bool BuildKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, Key &key) const;
  bool BuildKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, Key &key) const;
  // In-process ready objects: a hit returns the kept pipeline (borrowed; the cache holds its reference until the device lets
  // go). The device clones a fresh COM object over it — no translation, no Metal.
  ID3D12PipelineState *Lookup(const Key &key);
  // After a successful public creation: keeps the object (one reference) and, off the replay threads, appends the manifest
  // record and stores the bytecode/root blobs once by hash.
  void Store(const Key &key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, IUnknown *created);
  void Store(const Key &key, const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, IUnknown *created);

  // Translated stages on disk (AIR metallib). The key covers the function name, every bytecode the compiler consumes,
  // the argument chain (typed, including root-signature blobs by hash) and the Metal version carried in the chain.
  bool AirKey(const char *function, const D3D12_SHADER_BYTECODE *shaders, unsigned count,
              const SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, Sha1Digest &key) const;
  // A hit returns the Metal library of the stage (kept per process; the first use reads the pack); null = miss.
  WMT::Reference<WMT::Library> LoadLibrary(const Sha1Digest &key, WMT::Device metal);
  // After a compile: the AIR goes to the pack and the library built from it is kept.
  void StoreAir(const Sha1Digest &key, const void *data, uint64_t size, WMT::Library library);

  // Parse result of a shader (MTL_SHADER_REFLECTION) on disk by bytecode digest, so a second launch never parses the
  // container or the DXIL bitcode; the shader object itself is created lazily, only when a stage has to be compiled.
  bool LoadReflection(const D3D12_SHADER_BYTECODE &code, MTL_SHADER_REFLECTION &out);
  // inputs: the parsed input signature to keep with the reflection (nullptr = not available for this container).
  void StoreReflection(const D3D12_SHADER_BYTECODE &code, const MTL_SHADER_REFLECTION &refl,
                       const std::vector<InputParameter> *inputs = nullptr);
  bool LoadInputSignature(const D3D12_SHADER_BYTECODE &code, std::vector<InputParameter> &out);

  // Per-creation phase timing on the calling thread (MACRUNNER_DX12_PSO_TIMING=1): key, reflection, AIR files, archive
  // lookup/creation, total. Begin at the public entry, Report after it.
  // Identity of a container (DXBC checksum + size; SHA-1 only when the checksum is zero), from the digests of the
  // creation in flight on this thread when BuildKey already computed them.
  static Sha1Digest ShaderId(const void *data, size_t size);
  void TimingBegin();
  void TimingReport(const char *kind, bool object_hit, HRESULT hr);
  bool TimingEnabled() const { return timing_enabled_; }

  // Pipeline creation through the archives. Metal cannot serialize an archive loaded from a file once extended ("cannot pack"
  // from its per-process scratch; measured 17.10 on 27.0.1), so every process serializes only what it compiled into a file of
  // its own, and lookups span every file listed in archives.txt. A hit is tried first with fail-on-miss and never touches the
  // serialization archive; a miss compiles and is added to it.
  WMT::Reference<WMT::RenderPipelineState> CreateRender(WMT::Device metal, WMTRenderPipelineInfo &info, WMT::Error &err);
  WMT::Reference<WMT::RenderPipelineState> CreateMesh(WMT::Device metal, WMTMeshRenderPipelineInfo &info, WMT::Error &err);
  WMT::Reference<WMT::ComputePipelineState> CreateCompute(WMT::Device metal, WMTComputePipelineInfo &info, WMT::Error &err);

  // Replays the manifest known at open, once, on background threads (first command queue creation).
  void StartReplay();
  // Stops the threads, releases replay objects, serializes a dirty archive. Called by the device destructor first.
  void Shutdown();
  // Serializes the archive when pipelines were added since the last serialization.
  void Serialize(const char *reason);

  static bool Replaying();

  // Ready objects hold references to the device. When the application's last reference is gone, only they keep the device
  // alive; the device's Release() asks and the cache lets go so that the device destructs and the archive is written.
  bool HoldsOnly(uint32_t device_refs) const;
  void DropObjects();

private:
  struct Entry {
    Sha1Digest id;
    std::vector<uint8_t> body;
  };
  PipelineCache() = default;
  bool OpenDirectory(const std::string &key);
  bool OpenArchives(WMT::Device metal);
  // Keeps the current version directory and the most recently used previous one; removes the rest and, over the ceiling
  // (MACRUNNER_DX12_PSO_MAX_MB, default 2048), the previous one too.
  void CleanupVersions(const std::string &root, const std::string &key);
  template <class Info, class Fn> auto CreateThroughArchives(Info &info, WMT::Error &err, Fn &&create);
  void FinishCompaction();
  void LoadManifest();
  bool ReadFileBytes(const std::string &path, std::vector<uint8_t> &out) const;
  bool WriteBlob(const Sha1Digest &id, const void *data, size_t size, unsigned &written);
  void Append(const Key &key, const char *kind, unsigned new_blobs);
  void KeepObject(const Key &key, IUnknown *created);
  ID3D12RootSignature *ReplayRoot(const Sha1Digest &id);
  const std::vector<uint8_t> *ReplayBlob(const Sha1Digest &id);
  bool ReplayGraphics(const uint8_t *body, size_t size);
  bool ReplayCompute(const uint8_t *body, size_t size);
  void ReplayWorker();
  void Maintenance();
  static DWORD WINAPI ReplayThreadMain(void *self);
  static DWORD WINAPI MaintenanceThreadMain(void *self);

  MTLD3D12Device *device_ = nullptr;
  std::string dir_;                  // unix path for winemetal, trailing slash (relative = under the user cache dir)
  std::vector<WMT::Reference<WMT::BinaryArchive>> lookups_;   // archives of earlier processes, oldest first
  std::vector<obj_handle_t> lookup_handles_;
  std::vector<std::string> archive_files_;                      // names behind lookups_ (archives.txt)
  WMT::Reference<WMT::BinaryArchive> serial_;                   // this process's additions
  std::string serial_file_;
  bool serial_indexed_ = false;
  bool compact_ = false;                                        // replay compiles everything into serial_, then the index shrinks to it
  unsigned compact_at_ = 8;
  std::atomic<uint32_t> archive_hits_{0}, archive_misses_{0}, archive_added_{0};
  bool directory_ = false;
  bool replay_enabled_ = false;
  bool air_enabled_ = false;
  bool refl_enabled_ = false;
  bool objects_enabled_ = false;
  bool timing_enabled_ = false;
  std::atomic<uint32_t> refl_hits_{0}, refl_misses_{0};
  // Pack of reflection and AIR records: pack.bin holds the bytes, pack.idx fixed 36-byte entries (kind, digest, offset, length);
  // one positional read per hit over a descriptor the unix side keeps open instead of open/read/close per file.
  // pack.idx: 16-byte header "MRPKIDX5" + u64 reserved, then entries of 32 + sizeof(MTL_SHADER_REFLECTION) + 256 bytes:
  // kind (1 refl, 2 air), digest, offset, length, and for reflections the MTL_SHADER_REFLECTION itself inline followed by
  // the parsed input signature — a reflection or signature hit is a map lookup, an AIR hit one positional read.
  struct PackEntry { uint64_t offset; uint32_t length; };
  std::mutex pack_mutex_;
  std::unordered_map<Sha1Digest, PackEntry> pack_air_;
  std::unordered_map<Sha1Digest, MTL_SHADER_REFLECTION> pack_refl_;
  std::unordered_map<Sha1Digest, std::vector<InputParameter>> pack_sig_;   // decoded input signatures of kind-1 entries
  std::unordered_map<Sha1Digest, WMT::Reference<WMT::Library>> libraries_;   // Metal libraries by AIR key, this process
  bool LoadPackIndex();
  bool PackLoadAir(const Sha1Digest &id, std::vector<uint8_t> &out);
  bool PackAppendIndex(const uint8_t *entry);
  bool pack_usable_ = false; // immutable after Open: corrupt/partial indexes fall back to fresh compilation
  unsigned threads_count_ = 2;

  mutable std::mutex mutex_;         // known_, objects_, manifest, counters below
  std::unordered_set<Sha1Digest> known_;
  std::unordered_map<Sha1Digest, ID3D12PipelineState *> objects_;
  std::vector<Entry> entries_;       // loaded at open, consumed by the replay
  uint32_t recorded_new_ = 0, skipped_ = 0, hits_ = 0;
  std::atomic<uint32_t> air_hits_{0}, air_misses_{0}, air_stored_{0};

  std::atomic<uint32_t> next_{0};
  std::atomic<uint32_t> replay_created_{0}, replay_failed_{0};
  std::atomic<uint32_t> workers_alive_{0};
  std::atomic<bool> stop_{false}, replay_started_{false}, dirty_{false};
  std::atomic<ULONGLONG> last_store_ms_{0};   // debounce: serialize a few seconds after the last new pipeline
  std::vector<HANDLE> threads_;
  HANDLE maintenance_ = nullptr;
  HANDLE wake_ = nullptr;
  ULONGLONG replay_start_ms_ = 0;
  std::mutex serialize_mutex_;

  std::mutex replay_mutex_;          // blobs_, roots_
  std::unordered_map<Sha1Digest, std::vector<uint8_t>> blobs_;
  std::unordered_map<Sha1Digest, ID3D12RootSignature *> roots_;
};

} // namespace pso_cache
} // namespace dxmt
