#pragma once
// GPU-PASS 25.10.2026: MACRUNNER_DX12_TESS_PREPASS=1 (default 0) -- tessellation without mesh draws (v2).
// A tessellation draw of the native airconv path is recorded as a compute pre-pass in front of its render pass plus two
// ordinary draws inside it, instead of one drawMeshThreadgroups:
//   hull kernel   = the VS+HS object function as a kernel (same body, same threadgroup shape); its payload is one device
//                   slot per object threadgroup with the control points / patch constants and, per emitted workload, a
//                   4-byte record (patch, vertex and triangle count) in the mesh path's emission order;
//   scan          = one threadgroup scans the groups in grid order (flat workload / vertex / index bases) and writes the
//                   arguments of the three indirect calls; when the workload table or the index buffer is too small it
//                   switches to the fallback (the slots are bounded by the payload capacity and never overflow);
//   materialize   = one threadgroup per group, lane = patch: regenerates the workloads in program order into the table at
//                   their emission positions and writes (((flat workload << 8) | local vertex) << 1) | corner role;
//   draw          = drawIndexedPrimitives (indirect) whose vertex function reads its workload from the index value (no
//                   search) and evaluates domain point + DS per native first/other-corner identity; same pixel function;
//   fallback      = drawPrimitives (indirect, 0 vertices unless overflowed) whose vertex function finds group / record /
//                   corner and regenerates the workload from the slot (no table).
// Why: any mesh draw that reaches the GPU costs 58-110 us per render pass on the M1 Pro (probe tessbuf zeromesh, 25.10; the
// floor of the tess gaps at the church). v1 (DS in a kernel writing 160-byte vertex records) was byte-exact but not faster
// and held 64 MB of slots per 21k-patch draw; v2 keeps no vertex records and ~10 MB of slots for the same draw.
// The need of every pipeline (max workloads / indices seen) comes back through a small shared buffer; the capacity of the
// next draws grows from it (no CPU wait; an overflowing draw renders through the fallback, exactly).
// GetEnvironmentVariableA/GetLastError come from the includer.
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include "thread.hpp"

namespace dxmt::tess_prepass {

// bytes of the pre-pass blocks alive in this process (all command allocators) and their peak: the memory the key adds
inline std::atomic<uint64_t> &LiveBytes() { static std::atomic<uint64_t> v{0}; return v; }
inline std::atomic<uint64_t> &PeakBytes() { static std::atomic<uint64_t> v{0}; return v; }
// block allocator of the command allocators' pre-pass ring: GPU-private buffers, counted
class CountedBlockAllocator {
public:
  CountedBlockAllocator(WMT::Device device, WMTResourceOptions options) : device_(device), options_(options) {}
  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address = 0;
    uint64_t size = 0;
    Block() = default;
    Block(const Block &) = delete;
    Block(Block &&o) noexcept : buffer(std::move(o.buffer)), gpu_address(o.gpu_address), size(o.size) { o.size = 0; }
    Block &operator=(Block &&o) noexcept {
      if (this != &o) { release(); buffer = std::move(o.buffer); gpu_address = o.gpu_address; size = o.size; o.size = 0; }
      return *this;
    }
    ~Block() { release(); }
  private:
    void release() { if (size) { LiveBytes().fetch_sub(size, std::memory_order_relaxed); size = 0; } }
  };
  Block
  allocate(size_t block_size) {
    WMTBufferInfo info{};
    info.memory.set(nullptr);
    info.options = options_;
    info.length = block_size;
    Block block;
    block.buffer = device_.newBuffer(info);
    block.gpu_address = info.gpu_address;
    block.size = block_size;
    const uint64_t live = LiveBytes().fetch_add(block_size, std::memory_order_relaxed) + block_size;
    uint64_t peak = PeakBytes().load(std::memory_order_relaxed);
    while (live > peak && !PeakBytes().compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
    return block;
  }
private:
  WMT::Device device_;
  WMTResourceOptions options_;
};

// GAPS 01.11 MACRUNNER_DX12_TESS_SCRATCH_POOL=<MB> (default 0): one process-wide pool of finished pre-pass blocks instead of
// per-allocator keeping (TESS_SCRATCH_KEEP, which held up to the cap in EVERY allocator: +0.73 GB in Elden Ring at 32 MB). At Reset an
// allocator gives its blocks to the pool (the GPU has finished them: Reset requires it) while the pool holds less than <MB>; the rest
// is released. A new block is taken from the pool (smallest pooled block >= the request, at most 2x) or created. Sizes are rounded
// to 4 classes per octave (>= 64 KB) so that blocks are interchangeable between allocators. Memory = blocks in use + at most <MB>.
inline std::atomic<uint64_t> &PoolBytes() { static std::atomic<uint64_t> v{0}; return v; }
inline size_t PoolCapBytes() {
  static const size_t cap = [] {
    const DWORD saved = GetLastError();
    char text[16] = {};
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_TESS_SCRATCH_POOL", text, sizeof(text));
    SetLastError(saved);
    size_t mb = 0;
    for (DWORD i = 0; i < n && i < sizeof(text) && text[i] >= '0' && text[i] <= '9'; ++i) mb = mb * 10 + size_t(text[i] - '0');
    if (n == 0 || n >= sizeof(text) || mb > 16384) mb = 0;
    return mb << 20;
  }();
  return cap;
}
inline size_t PoolClass(size_t size) {
  size_t s = size < 65536 ? 65536 : size;
  unsigned bit = 63 - unsigned(__builtin_clzll(s));
  const size_t step = (size_t(1) << bit) >> 2;
  return (s + step - 1) / step * step;
}
struct BlockPool {
  dxmt::mutex lock;   // GAPS 14:xx: dxmt::mutex, so MACRUNNER_DX12_LOCK_STATS counts its contention by site (std::mutex is invisible there)
  std::multimap<uint64_t, CountedBlockAllocator::Block> free;
  uint64_t takes = 0, hits = 0, gives = 0, drops = 0;
};
inline BlockPool &Pool() { static BlockPool *p = new BlockPool; return *p; }   // never destroyed: allocators may reset during process exit

// A command allocator may be reset only after its GPU work has completed.
// Buffers remain owned until allocator Reset (GPU completion is required). Do not retain
// a finished worker's largest allocation: ER rotates heavy draws between many worker allocators.
class ScratchBuffers {
public:
  ScratchBuffers(CountedBlockAllocator &&allocator) : allocator_(std::move(allocator)) {}
  void reset() {
    if (const size_t pool_cap = PoolCapBytes()) {   // GAPS TESS_SCRATCH_POOL: hand the finished blocks to the process pool
      auto &pool = Pool();
      {
        std::lock_guard<dxmt::mutex> guard(pool.lock);
        for (auto &b : blocks_) {
          if (!b.size) continue;
          const uint64_t size = b.size;
          if (PoolBytes().load(std::memory_order_relaxed) + size <= pool_cap) {
            pool.free.emplace(size, std::move(b));
            PoolBytes().fetch_add(size, std::memory_order_relaxed);
            ++pool.gives;
          } else ++pool.drops;   // b is released when blocks_ is cleared
        }
      }
      blocks_.clear();   // the dropped blocks are released outside the pool lock
      cursor_ = 0;
      return;
    }
    // GAPS 01.11 MACRUNNER_DX12_TESS_SCRATCH_KEEP=<MB> (default 0 = release all, as before): keep this allocator's blocks for
    // its next recording up to <MB> in total (the GPU has finished them: Reset requires it), release the rest.
    const size_t cap = KeepCapBytes();
    if (!cap) {
      blocks_.clear(); // All prior references belong to recordings invalidated by this allocator Reset.
      cursor_ = 0;
      return;
    }
    size_t kept = 0, n = 0;
    while (n < blocks_.size() && kept + blocks_[n].size <= cap) kept += blocks_[n++].size;
    blocks_.resize(n);
    cursor_ = 0;
  }
  static size_t KeepCapBytes() {
    static const size_t cap = [] {
      const DWORD saved = GetLastError();
      char text[16] = {};
      const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_TESS_SCRATCH_KEEP", text, sizeof(text));
      SetLastError(saved);
      size_t mb = 0;
      for (DWORD i = 0; i < n && i < sizeof(text) && text[i] >= '0' && text[i] <= '9'; ++i) mb = mb * 10 + size_t(text[i] - '0');
      if (n == 0 || n >= sizeof(text) || mb > 4096) mb = 0;
      return mb << 20;
    }();
    return cap;
  }
  CountedBlockAllocator::Block &allocate(size_t size) {
    size = (size + 4095) & ~size_t(4095);
    if (PoolCapBytes()) {   // GAPS TESS_SCRATCH_POOL: blocks_ only grows between Resets in this mode
      const size_t want = PoolClass(size);
      CountedBlockAllocator::Block block;
      {
        auto &pool = Pool();
        std::lock_guard<dxmt::mutex> guard(pool.lock);
        ++pool.takes;
        auto it = pool.free.lower_bound(want);
        if (it != pool.free.end() && it->first <= 2 * want) {
          block = std::move(it->second);
          PoolBytes().fetch_sub(it->first, std::memory_order_relaxed);
          pool.free.erase(it);
          ++pool.hits;
        }
      }
      if (!block.size) block = allocator_.allocate(want);
      blocks_.push_back(std::move(block));
      ++cursor_;
      return blocks_.back();
    }
    if (cursor_ == blocks_.size()) blocks_.push_back(allocator_.allocate(size));
    else if (blocks_[cursor_].size < size) {
      // This slot is from a completed allocator recording. Release it before growing, so the old and new capacities
      // are not both resident at the peak. Buffers referenced by the current recording always occupy earlier slots.
      blocks_[cursor_] = CountedBlockAllocator::Block{};
      blocks_[cursor_] = allocator_.allocate(size);
    }
    return blocks_[cursor_++];
  }
private:
  CountedBlockAllocator allocator_;
  std::vector<CountedBlockAllocator::Block> blocks_;
  size_t cursor_ = 0;
};

inline unsigned
Mode() {
  static const unsigned on = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const unsigned parsed = GetEnvironmentVariableA("MACRUNNER_DX12_TESS_PREPASS", value, sizeof(value)) == 1 &&
        (value[0] == '1' || value[0] == '3') ? unsigned(value[0] - '0') : 0u;
    SetLastError(error);
    return parsed;
  }();
  return on;
}
inline bool Enabled() { return Mode() != 0; }
inline bool Verify() { return Mode() == 3; }
// Diagnostic oracle controls: 1=M/M, 2=D/D; identical pre-draw attachments.
inline unsigned VerifyControl() {
  static const unsigned mode=[] { const DWORD saved=GetLastError(); char value[4]={};
    const DWORD n=GetEnvironmentVariableA("MACRUNNER_DX12_TESS_VERIFY_MESH_CONTROL",value,sizeof(value));
    SetLastError(saved); return n==1 && value[0]>='1' && value[0]<='2' ? unsigned(value[0]-'0') : 0u; }();
  return Verify() ? mode : 0u;
}
inline bool VerifyMeshControl() { return VerifyControl() == 1; }
inline bool VerifyPullControl() { return VerifyControl() == 2; }
inline bool VerifyRecords() {
  static const bool on=[] { const DWORD saved=GetLastError(); char value[4]={};
    const DWORD n=GetEnvironmentVariableA("MACRUNNER_DX12_TESS_VERIFY_RECORDS",value,sizeof(value));
    SetLastError(saved); return n==1 && (value[0]=='1' || value[0]=='2'); }();
  return Verify() && on;
}
inline bool RecordNegative() {
  static const bool on=[] { const DWORD saved=GetLastError(); char value[4]={};
    const DWORD n=GetEnvironmentVariableA("MACRUNNER_DX12_TESS_VERIFY_RECORDS",value,sizeof(value));
    SetLastError(saved); return n==1 && value[0]=='2'; }();
  return VerifyRecords() && on;
}
// Diagnostic only: keep materialization and the encoded index stream, but expand
// index references in the vertex function. Separates table data from indexed IA.
inline unsigned NoIndexMode() {
  static const unsigned mode=[] { const DWORD saved=GetLastError(); char value[4]={};
    const DWORD n=GetEnvironmentVariableA("MACRUNNER_DX12_TESS_PREPASS_NOINDEX",value,sizeof(value));
    SetLastError(saved); return n==1 && value[0]>='1' && value[0]<='3' ? unsigned(value[0]-'0') : 0u; }();
  return Verify() && !VerifyRecords() ? mode : 0u;
}
inline bool NoIndex() { return NoIndexMode() != 0; }
// Restrict the existing one-draw/32-MiB localization capture to a census PSO. Zero preserves the first-draw control.
inline bool RecordPso(uint64_t pso, uint32_t patches) {
  static const uint64_t selected=[] {
    const DWORD saved=GetLastError(); char value[24]={};
    const DWORD n=GetEnvironmentVariableA("MACRUNNER_DX12_TESS_VERIFY_RECORDS_PSO",value,sizeof(value));
    SetLastError(saved);
    uint64_t result=0;
    if (!n || n>=sizeof(value)) return uint64_t(0);
    for (DWORD i=0;i<n;++i) {
      if (value[i]<'0' || value[i]>'9' || result>(UINT64_MAX-9)/10) return uint64_t(0);
      result=result*10+uint64_t(value[i]-'0');
    }
    return result;
  }();
  static const uint32_t minimum=[] {
    const DWORD saved=GetLastError(); char value[12]={};
    const DWORD n=GetEnvironmentVariableA("MACRUNNER_DX12_TESS_VERIFY_RECORDS_MIN_PATCHES",value,sizeof(value));
    SetLastError(saved);
    uint32_t result=0;
    if (!n || n>=sizeof(value)) return uint32_t(0);
    for (DWORD i=0;i<n;++i) {
      if (value[i]<'0' || value[i]>'9' || result>(UINT32_MAX-9)/10) return uint32_t(0);
      result=result*10+uint32_t(value[i]-'0');
    }
    return result;
  }();
  return (!selected || selected==pso) && patches>=minimum;
}

// Separate switch only for the scheduling A/B; the master switch still defaults to zero.
inline bool Schedule() {
  static const bool on = [] {
    const DWORD error = GetLastError(); char value[4] = {};
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_TESS_PREPASS_SCHEDULE", value, sizeof(value));
    SetLastError(error); return !n || (n == 1 && value[0] == '1');
  }();
  return Enabled() && on;
}

// GAPS 01.11.2026: MACRUNNER_DX12_TESS_PREPASS_RENAME=1 (default 0). The pre-pass scratch (slots, counts, offsets, table, indices,
// args of one draw) is taken from a per-recording pool whose cursor restarts at every render pass, so consecutive tess passes
// reuse the same buffers - a WAR on the layer's own memory that stops SCHEDULE at the previous consumer (Elden Ring at the
// church: 5 of 12 pre-passes a frame sit between two tess consumers, F->F across each 85 us, 0.46 ms/frame). With the key the
// cursor runs over the whole recording: every tess draw of a list has its own scratch (memory: one scratch per draw of a list
// instead of per draw of the largest pass), the scheduler sees no shared scratch and moves such pre-passes up together (one
// compute encoder per start point). =9 is a NEGATIVE CONTROL for probes only: buffers still shared, but the scheduler ignores the
// shared-scratch conflict (pre-passes overwrite a scratch the previous consumer has not read yet: pixels must differ).
inline unsigned
RenameMode() {
  static const unsigned m = [] {
    const DWORD error = GetLastError(); char value[4] = {};
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_TESS_PREPASS_RENAME", value, sizeof(value));
    SetLastError(error); return n == 1 && (value[0] == '1' || value[0] == '9') ? unsigned(value[0] - '0') : 0u;
  }();
  return m;
}

// MACRUNNER_DX12_TESS_PREPASS_STATS=<N>: every N draws one stderr line with the pipelines' need and overflow counters.
inline unsigned
StatsPeriod() {
  static const unsigned period = [] {
    const DWORD error = GetLastError();
    char value[16] = {};
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_TESS_PREPASS_STATS", value, sizeof(value));
    SetLastError(error);
    return n && n < sizeof(value) ? unsigned(strtoul(value, nullptr, 10)) : 0u;
  }();
  return period;
}

// MACRUNNER_DX12_TESS_PREPASS_TEST (stand only): 1 = capacity 1 (every draw overflows: the fallback must be byte-exact),
// 2 = negative control (capacity 1 and no fallback draw: the triangles are missing, the comparison must see it).
inline unsigned
TestMode() {
  static const unsigned mode = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_TESS_PREPASS_TEST", value, sizeof(value));
    SetLastError(error);
    return n == 1 && (value[0] == '1' || value[0] == '2') ? unsigned(value[0] - '0') : 0u;
  }();
  return mode;
}

// binding indices of the pre-pass (airconv: SM50_TESS_PREPASS_BINDING_*; 23-25 are the scan's own)
enum : uint8_t {
  kSlots = 16, kGroups = 17, kOffsets = 19, kTable = 20, kIndices = 21, kConstants = 22,
  kScanCaps = 23, kScanArgs = 24, kScanNeed = 25,
};
// byte offsets inside the 64-byte argument block written by the scan
enum : uint32_t { kArgsDispatch = 0, kArgsIndexed = 16, kArgsPull = 48, kArgsBytes = 64 };
constexpr uint32_t kEntryBytes = 64;          // table entry: workload (56 bytes) + group + 0
constexpr uint32_t kMaxTableEntries = 1u << 23; // index32: 23 workload bits + 8 local-vertex bits + 1 corner-role bit

struct InputNeed { uint32_t type, first, last, space, bytes, visibility; };
struct PsoPrepass {
  // Only VS/HS feed the compute pre-pass; DS remains in the render vertex function.
  std::vector<InputNeed> inputs;
  bool inputs_known = false;
  // Mode-3 census only: full original VS/HS/DS bytes and reflected inputs. No reuse is enabled by this observer.
  std::vector<uint8_t> twin_shaders;
  std::vector<InputNeed> twin_inputs;
  bool twin_known = false;
  WMT::Reference<WMT::ComputePipelineState> hull[3];   // object function variants: no index buffer, uint16, uint32
  WMT::Reference<WMT::ComputePipelineState> materialize;
  WMT::Reference<WMT::RenderPipelineState> pull, fallback;
  WMT::Reference<WMT::RenderPipelineState> record_mesh[3];
  WMT::Reference<WMT::Buffer> record_disabled;
  uint32_t record_stride = 0, record_vertices = 0;
  uint32_t slot_stride = 0, meta_offset = 0, workloads_per_group = 0, patches_per_group = 0, threads_per_patch = 0;
  WMT::Reference<WMT::Buffer> need;   // shared uint4 {max workloads, max indices, overflowed draws, draws}
  volatile uint32_t *need_cpu = nullptr;
  std::atomic<uint32_t> cap_workloads{0}, cap_indices{0};

  // Capacity for the next draw: the larger of the current one and the exact need seen so far (the GPU writes the need
  // while the CPU records; a stale read only delays the growth by a frame, the overflowing draw itself is exact)
  std::pair<uint32_t, uint32_t>
  Capacity(uint32_t patches) {
    uint32_t w = cap_workloads.load(std::memory_order_relaxed), i = cap_indices.load(std::memory_order_relaxed);
    // No guessed per-patch reserve: the exact fallback handles the first draw while the GPU reports its need.
    // Metal still needs a nonempty backing allocation when the eventual draw has zero output.
    if (!w) { w = 1; i = 1; }
    if (need_cpu) {
      const uint32_t nw = need_cpu[0], ni = need_cpu[1];
      w = std::max(w, nw);
      i = std::max(i, ni);
    }
    w = std::min(w, kMaxTableEntries);
    cap_workloads.store(w, std::memory_order_relaxed);
    cap_indices.store(i, std::memory_order_relaxed);
    return {w, i};
  }
};

// the scan (shader-independent): groups in grid order -> bases, arguments of the three indirect calls, need
inline constexpr char kScanSource[] = R"(
#include <metal_stdlib>
using namespace metal;
struct ScanCaps { uint workloads, indices, groups, pad; };
kernel void tess_prepass_scan(device const uint4 *gcount [[buffer(17)]], device uint4 *offsets [[buffer(19)]],
                              constant ScanCaps &caps [[buffer(23)]], device uint *args [[buffer(24)]],
                              device atomic_uint *need [[buffer(25)]], uint t [[thread_index_in_threadgroup]],
                              uint lane [[thread_index_in_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]]) {
  const uint groups = caps.groups;
  const uint per = (groups + 1023) / 1024, g0 = min(t * per, groups), g1 = min(g0 + per, groups);
  uint w = 0, v = 0, i = 0;
  for (uint g = g0; g < g1; g++) { const uint4 c = gcount[g]; w += c.x; v += c.y; i += c.z; }
  threadgroup uint sw[32], sv[32], si[32], tot[3];
  const uint pw = simd_prefix_exclusive_sum(w), pv = simd_prefix_exclusive_sum(v), pi = simd_prefix_exclusive_sum(i);
  if (lane == 31) { sw[sg] = pw + w; sv[sg] = pv + v; si[sg] = pi + i; }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (sg == 0) {
    const uint a = sw[lane], b = sv[lane], c = si[lane];
    const uint ea = simd_prefix_exclusive_sum(a), eb = simd_prefix_exclusive_sum(b), ec = simd_prefix_exclusive_sum(c);
    sw[lane] = ea; sv[lane] = eb; si[lane] = ec;
    if (lane == 31) { tot[0] = ea + a; tot[1] = eb + b; tot[2] = ec + c; }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  uint bw = sw[sg] + pw, bv = sv[sg] + pv, bi = si[sg] + pi;
  for (uint g = g0; g < g1; g++) { const uint4 c = gcount[g]; offsets[g] = uint4(bw, bv, bi, 0); bw += c.x; bv += c.y; bi += c.z; }
  if (t == 0) {
    const uint tw = tot[0], ti = tot[2];
    // Eight local-vertex bits plus one native corner-role bit leave 23 bits
    // for the workload ordinal. Route excess through the existing exact fallback.
    const bool over = tw > caps.workloads || ti > caps.indices || tw > 0x800000u;
    args[0] = over ? 0u : groups; args[1] = 1; args[2] = 1; args[3] = 0;
    args[4] = over ? 0u : ti; args[5] = 1; args[6] = 0; args[7] = 0; args[8] = 0;
    args[12] = over ? ti : 0u; args[13] = 1; args[14] = 0; args[15] = 0;
    atomic_fetch_max_explicit(&need[0], tw, memory_order_relaxed);
    atomic_fetch_max_explicit(&need[1], ti, memory_order_relaxed);
    if (over) atomic_fetch_add_explicit(&need[2], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&need[3], 1u, memory_order_relaxed);
  }
}
)";

inline WMT::ComputePipelineState
ScanPso(WMT::Device device) {
  static std::mutex mutex;
  static WMT::Reference<WMT::ComputePipelineState> pso;
  static obj_handle_t owner = 0;
  std::lock_guard lock(mutex);
  if (pso && owner == device.handle) return pso;
  WMT::Reference<WMT::Error> error;
  auto library = device.newLibraryWithSource(kScanSource, error);
  if (!library) return {};
  WMTComputePipelineInfo info;
  WMT::InitializeComputePipelineInfo(info);
  auto function = library.newFunction("tess_prepass_scan");
  info.compute_function = function;
  pso = device.newComputePipelineState(info, error);
  owner = device.handle;
  return pso;
}

} // namespace dxmt::tess_prepass
