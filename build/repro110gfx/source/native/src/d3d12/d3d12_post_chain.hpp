#pragma once
// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// POSTCS 03.11.2026: MACRUNNER_DX12_POST_CHAIN (default 0; hot key) — a LONG chain of dependent one-draw post-process render
// passes encoded as ONE compute encoder (one dispatch per pass, memory barriers between), byte-exact, with no new work on the
// application's recording threads.
//
// Why (Elden Ring at the church, c24 LRS4 PASS_TIME 02.11, 200 frames): the 20-pass R11G11B10F blur chain 640x360..40x23..640x360
// spans 852 us of GPU time for 167 us of fragment work: 13 of 19 passes start their vertex stage only after the previous pass's
// fragment stage ended (the M1 submission of small render passes, GAPS 01.11 §2.1: no fence causes it) and pay F->F 35 us each.
// The same chain as compute in one encoder: 926 -> 471 us in the world (GAPS 01.11 series P, POST-CS v3). POST-CS v3 converted
// AT RECORDING (Draw on the game's GXWorker threads) and turned the lists of converted passes into early-encoded lists (Close:
// +0.19 ms per GXWorker thread, ~1.4 ms CPU per frame - the series Q frame rate fell although busy fell) and converted short runs
// and singletons that lose (series P: busy +0.11). This key:
//   * records every pass as before (the render pass is always there); a candidate draw only leaves a small POD side record (the
//     pointers of its already emitted setbuffer commands, its viewport/scissor, the VAs of the vertex/constant words its vertex
//     stage reads) in the list's command arena - no key, no hash, no lock, no allocation on the recording thread;
//   * decides AT ENCODING (ExecuteCommandLists, the submission thread): a run of >= MIN_RUN consecutive candidate passes (only
//     plain transition barriers between them, across the lists of the call) of <= MAX_PX pixels each becomes one compute encoder;
//     shorter runs stay render passes. Early (Close-time) encodings never convert (they stay as recorded).
//   * the kernel = the pixel shader's own body (airconv SM50_SHADER_PS_AS_COMPUTE, POST-CS v3, unchanged); interpolated inputs from
//     the uv-map (the rasterizer's own values, rendered once per content key: ENC-CENSUS case 30/31 - a formula is NOT byte-exact),
//     keyed at encoding by the bytes the vertex stage reads (vertex bytes of the keyed slots, the 16-byte constant rows of the VS
//     key query), witnessed on the GPU (thread 0,0 compares the same words; a difference turns the key off for the process).
// Candidate pipeline (creation, armed only): POST-CS v3 rules (one float target R11G11B10F/RGBA16F/RGB10A2, no depth, 1 sample, no
// blend/logic/alpha-to-coverage, full write mask, triangles, solid, VS+PS only, per-vertex input; kernel and uv-map compile; VS key
// query ok). Candidate draw (recording): DrawInstanced, 1 instance, list/strip, the pass's first and only draw, 1 RTV (2D view,
// ShaderWrite usage), no DSV, 1 viewport, no queries, root signature with <= 64 qwords. Candidate pass (encoding): its draw is the
// pass's only draw, no visibility / LDC probe / tessellation / mesh work, pixels <= MAX_PX.
// Run members may be separated only by null passes and barrier-only passes whose records are TRANSITIONs (flags 0, named object)
// among RENDER_TARGET / UNORDERED_ACCESS / (NON_)PIXEL_SHADER_RESOURCE, or UAV barriers: a memory barrier between dispatches covers
// them. Pending clears that are sunk, or whose texture is a run target or named by a run barrier, are encoded before the run.
// The compute encoder waits for / updates the common fence like any compute pass (kernels write only textures + the witness word:
// with PRERASTER_PRECISE the pre-raster fence is left alone, as for any texture-only compute pass).
//   MACRUNNER_DX12_POST_CHAIN=1 | 2 | 9   1: runs >= MIN_RUN (default 16); 2: runs >= MIN_RUN2 (default 8, for experiments);
//                                         9: NEGATIVE CONTROL (environment only): as 1, but the last pixel column of every converted
//                                         pass is not written - the byte comparison of the probe must FAIL
//   MACRUNNER_DX12_POST_CHAIN_MIN_RUN / _MIN_RUN2 / _MAX_PX (230400 = 640x360)
//   MACRUNNER_DX12_POST_CHAIN_SW_MAXW     widest render target that gets ShaderWrite usage at creation when armed (default 640)
//   MACRUNNER_DX12_POST_CHAIN_CACHE_MB    uv-map memory limit (default 64)
//   MACRUNNER_DX12_POST_CHAIN_STATS=N     counters every N presents (default LIGHTRN_STATS / GPU4_STATS / FENCE_STATS)
//   MACRUNNER_DX12_POST_CHAIN_TRACE=1     cumulative counters after every ExecuteCommandLists (probes without a swapchain)
// Armed = the key is 1/2/9 in the environment or MACRUNNER_DX12_HOTKEYS_FILE is set: then candidate pipelines get their compute
// variant, render targets get ShaderWrite and draws leave side records (both arms of a hot-key A/B are armed). Not armed: nothing.
// Hot key: entry 7 of src/dxmt/dxmt_hotkeys.hpp, read once per ExecuteCommandLists encoding (a run is all or nothing).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "dxmt_hotkeys.hpp"
#include "d3d12_post_chain_env.hpp"

namespace dxmt::post_chain {

#define DXMT_POST_CHAIN_COUNTERS(X)                                                                                          \
  X(pso_candidates) X(pso_ready) X(pso_refused) X(draws) X(draw_multi) X(encodings) X(early) X(cand_passes)                \
  X(runs) X(runs_short) X(converted) X(barriers) X(maps_rendered) X(maps_reused) X(maps_created)                           \
  X(len_8) X(len_16) X(len_32) X(len_more)                                                                                 \
  X(stop_render) X(stop_barrier) X(stop_clear) X(stop_other) X(stop_end) X(stop_size) X(stop_tess) X(stop_draws)            \
  X(r_vb) X(r_cb) X(r_witness) X(r_coverage) X(r_cache) X(r_churn) X(r_usage) X(r_disabled) X(r_state) X(r_root)            \
  X(clears_flushed) X(cpu_us)                                                                                              \
  X(stop_copy) X(stop_marker) X(stop_query) X(bx_passes) X(bx_strict) X(bx_invalid) X(bx_flags) X(bx_alias) X(bx_type)      \
  X(bx_noobj) X(bx_common) X(bx_states) X(bx_alias_join) X(bx_clears_all)                                                  \
  X(sr_pso_blend) X(sr_pso_kernel) X(sr_pso_format) X(sr_pso_other) X(sr_no_record) X(sr_record) X(clears_hoisted) X(clears_inrun)                                                                    \
  X(cpu_scan_us) X(cpu_prep_us) X(cpu_enc_us) X(prep_lookup_ns) X(prep_key_ns) X(prep_find_ns) X(prep_lookups) X(qpc_calls)

struct State {
#define DXMT_POST_CHAIN_FIELD(name) std::atomic<uint64_t> name{0};
  DXMT_POST_CHAIN_COUNTERS(DXMT_POST_CHAIN_FIELD)
#undef DXMT_POST_CHAIN_FIELD
  std::atomic<uint64_t> presents{0};
  std::atomic<bool> disabled{false};
  std::atomic<uint32_t> refusals_printed{0};
  std::atomic<uint32_t> barriers_printed{0};
  std::atomic<uint32_t> stops_printed{0};
  bool clear_tried = false;                                  // PCS2: the clear kernel (under witness_lock)
  WMT::Reference<WMT::ComputePipelineState> clear_kernel;
  std::mutex witness_lock;
  WMT::Reference<WMT::Buffer> witness;
  volatile uint32_t *witness_words = nullptr;
  uint32_t witness_seen = 0;
  uint64_t last[96] = {};   // values at the previous counters line (per-frame averages over the window)
};
inline State &Get() { static State s; return s; }
inline void Note(std::atomic<uint64_t> &counter, uint64_t n = 1) { counter.fetch_add(n, std::memory_order_relaxed); }
inline bool Disabled() { return Get().disabled.load(std::memory_order_relaxed); }

inline void
Line(const char *text, int length) {
  if (length <= 0) return;
  DWORD written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, DWORD(length), &written, nullptr);
}

// A pipeline that does not qualify, and why (first 64 per process, with stats or trace only).
inline void
Refused(const char *ps, const char *why) {
  Note(Get().pso_refused);
  if ((!E().stats && !E().trace) || Get().refusals_printed.fetch_add(1, std::memory_order_relaxed) >= 64) return;
  char line[256];
  const DWORD error = GetLastError();
  Line(line, std::snprintf(line, sizeof(line), "dx12_post_chain_refused ps=%s why=%s\n", ps, why));
  SetLastError(error);
}

// The compute variant of one graphics pipeline (built at creation when armed; immutable afterwards).
struct Variant {
  uint64_t id = 0;
  char ps[17] = {};
  uint32_t target_dxgi = 0;
  static constexpr uint32_t kMaxMaps = 48;   // uv-map contents per pipeline before it stays on the render path (r_churn)
  std::atomic<uint32_t> maps_created{0};
  WMT::Reference<WMT::ComputePipelineState> kernel;
  WMT::Reference<WMT::RenderPipelineState> uvmap;
  obj_handle_t dss = 0;          // depth/stencil state without depth for the uv-map pass (owned by the pipeline)
  WMTPixelFormat target_format = WMTPixelFormatInvalid;
  uint32_t map_count = 0, map_reg[SM50_POST_CS_MAX_MAPS] = {}, map_mask[SM50_POST_CS_MAX_MAPS] = {}, cov_comp = 0;
  uint32_t row_count = 0;
  SM50_POST_CS_VS_ROW rows[SM50_POST_CS_VS_MAX_ROWS] = {};
  uint32_t coverage = 1, map_format = 4;
  uint32_t pos_kind[4] = {}, pos_a[4] = {}, pos_b[4] = {};
  float pos_value[4] = {};
  uint32_t pos_slot = 0, pos_offset = 0, pos_components = 0;
  uint32_t slot_mask = 0;        // vertex buffer slots the input layout uses
  uint32_t cull = 1, front_ccw = 0, depth_clip = 1;   // D3D12_CULL_MODE_NONE = 1
  // rasterizer state of the uv-map pass (the pipeline's own)
  WMTCullMode r_cull = WMTCullModeNone; WMTDepthClipMode r_depth_clip = WMTDepthClipModeClip; WMTTriangleFillMode r_fill = WMTTriangleFillModeFill;
  float r_bias = 0, r_bias_clamp = 0, r_slope = 0; WMTWinding r_winding = WMTWindingClockwise;
};
// PCS2 03.11: why a render pass is not a run member (pipeline: 1..31, draw record: 32..). Names for the stop lines.
enum Why : uint8_t {
  WhyNone = 0, WhyRenderTargets = 2, WhyTargetFormat, WhyDepth, WhySamples, WhyA2C, WhyBlend, WhyWriteMask, WhyTopologyType,
  WhyRasterizer, WhyStages, WhyStreamOut, WhyPerInstance, WhyRootSig, WhyShaderInit, WhyVsKey, WhyKernel, WhyUvmap, WhyPipeOther,
  WhyDraws = 32, WhyInstances, WhyTopology, WhyBinding, WhyVisibility, WhyRootSize, WhyRootCmds, WhySamplerCmds, WhyVbCmd, WhyTarget,
  WhyRect, WhyCb, WhyNoVariant,
};
inline const char *WhyName(unsigned w) {
  switch (w) {
  case WhyNone: return "no_drawinstanced"; case WhyRenderTargets: return "pso_render_targets"; case WhyTargetFormat: return "pso_target_format";
  case WhyDepth: return "pso_depth"; case WhySamples: return "pso_samples"; case WhyA2C: return "pso_a2c"; case WhyBlend: return "pso_blend";
  case WhyWriteMask: return "pso_write_mask"; case WhyTopologyType: return "pso_topology"; case WhyRasterizer: return "pso_rasterizer";
  case WhyStages: return "pso_stages"; case WhyStreamOut: return "pso_stream_out"; case WhyPerInstance: return "pso_per_instance";
  case WhyRootSig: return "pso_root_sig"; case WhyShaderInit: return "pso_shader_init"; case WhyVsKey: return "pso_vs_key";
  case WhyKernel: return "pso_kernel"; case WhyUvmap: return "pso_uvmap"; case WhyPipeOther: return "pso_other";
  case WhyDraws: return "draws"; case WhyInstances: return "instances"; case WhyTopology: return "topology"; case WhyBinding: return "rtv_dsv_viewport_query";
  case WhyVisibility: return "visibility"; case WhyRootSize: return "root_size"; case WhyRootCmds: return "root_cmds";
  case WhySamplerCmds: return "sampler_cmds"; case WhyVbCmd: return "vb_cmd"; case WhyTarget: return "target"; case WhyRect: return "rect";
  case WhyCb: return "cb"; case WhyNoVariant: return "no_variant";
  case 99: return "pass_state";
  default: return "?";
  }
}
inline uint8_t WhyOfPipeline(const char *why) {
  if (!why) return WhyNone;
  static const struct { const char *s; uint8_t w; } map[] = {
    {"render_targets", WhyRenderTargets}, {"target_format", WhyTargetFormat}, {"depth_target", WhyDepth}, {"samples", WhySamples},
    {"alpha_to_coverage", WhyA2C}, {"blend", WhyBlend}, {"write_mask", WhyWriteMask}, {"topology", WhyTopologyType},
    {"rasterizer", WhyRasterizer}, {"stages", WhyStages}, {"stream_output", WhyStreamOut}, {"per_instance_input", WhyPerInstance},
    {"root_signature", WhyRootSig}, {"shader_init", WhyShaderInit}, {"vs_key", WhyVsKey}, {"kernel", WhyKernel}, {"uvmap", WhyUvmap}};
  for (const auto &m : map) if (!std::strncmp(why, m.s, std::strlen(m.s))) return m.w;
  return WhyPipeOther;
}
// The first 24 run stops at a render pass, with the pass and why (stats or trace only).
inline void
StopSeen(unsigned why, unsigned width, unsigned height, unsigned format, uint64_t ps, unsigned draws, unsigned run_len) {
  if ((!E().stats && !E().trace) || Get().stops_printed.fetch_add(1, std::memory_order_relaxed) >= 24) return;
  char line[256];
  const DWORD error = GetLastError();
  Line(line, std::snprintf(line, sizeof(line), "dx12_post_chain_stop why=%s size=%ux%u format=%u ps=%016llx draws=%u run_before=%u\n",
                           WhyName(why), width, height, format, (unsigned long long)ps, draws, run_len));
  SetLastError(error);
}

inline uint64_t NextVariantId() { static std::atomic<uint64_t> next{1}; return next.fetch_add(1, std::memory_order_relaxed); }

// What a candidate draw leaves in its pass (command arena of the list: POD, lives as long as the recording).
struct Draw {
  Variant *variant = nullptr;    // the application keeps the pipeline alive while a list using it may execute
  uint32_t vertex_count = 0, start_vertex = 0;
  WMTPrimitiveType primitive = WMTPrimitiveTypeTriangle;
  uint32_t topology = 0;
  // the setbuffer commands the draw's PreDraw emitted in this pass (read at encoding: LIGHTRN may patch them for one encoding)
  const wmtcmd_render_setbuffer *fs_root = nullptr, *fs_samplers = nullptr, *vs_root = nullptr, *vs_samplers = nullptr, *vs_vb = nullptr;
  D3D12_VIEWPORT viewport{};
  D3D12_RECT scissor{};          // clamped as the render path clamps it
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // pixels the pass can touch (target ∩ scissor ∩ viewport)
  uint32_t width = 0, height = 0;          // render target
  obj_handle_t target = 0;                  // the view's Metal texture (ShaderWrite checked at recording)
  // vertex stage inputs: vertex bytes of the keyed slots, constant rows (VA, or bytes of root constants)
  uint32_t slot_mask = 0;
  uint64_t vb_va[32] = {};
  uint32_t vb_stride[32] = {}, vb_size[32] = {};
  uint64_t row_va[SM50_POST_CS_VS_MAX_ROWS] = {};
  uint8_t row_bytes[SM50_POST_CS_VS_MAX_ROWS][16] = {};
  uint32_t row_const_mask = 0;              // bit r: row r is root constants (bytes above, nothing to witness)
};

// The coverage proof of one draw (variant.coverage == 0): POST-CS v3 Covered, unchanged.
inline bool
Covered(const Variant &v, const uint8_t *const vertex_bytes[3], const uint8_t rowbytes[][16], const D3D12_VIEWPORT &vp,
        int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
  double wx[3], wy[3];
  for (unsigned k = 0; k < 3; ++k) {
    float pos[4];
    for (unsigned c = 0; c < 4; ++c) {
      switch (v.pos_kind[c]) {
      case SM50_POST_CS_POS_INPUT: {
        const uint32_t comp = v.pos_b[c];
        if (comp < v.pos_components) std::memcpy(&pos[c], vertex_bytes[k] + v.pos_offset + 4 * comp, 4);
        else pos[c] = comp == 3 ? 1.0f : 0.0f;
        break;
      }
      case SM50_POST_CS_POS_LITERAL: pos[c] = v.pos_value[c]; break;
      case SM50_POST_CS_POS_CB: std::memcpy(&pos[c], rowbytes[v.pos_a[c]] + 4 * v.pos_b[c], 4); break;
      default: return false;
      }
    }
    if (!(pos[3] > 0.0f) || !std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]) || !std::isfinite(pos[3]))
      return false;
    const double xn = double(pos[0]) / pos[3], yn = double(pos[1]) / pos[3], zn = double(pos[2]) / pos[3];
    if (v.depth_clip && (zn < 0.0 || zn > 1.0)) return false;
    wx[k] = vp.TopLeftX + (xn + 1.0) * 0.5 * vp.Width;
    wy[k] = vp.TopLeftY + (1.0 - yn) * 0.5 * vp.Height;
  }
  const double area = (wx[1] - wx[0]) * (wy[2] - wy[0]) - (wx[2] - wx[0]) * (wy[1] - wy[0]);
  if (std::fabs(area) < 1.0) return false;
  const bool front = (area > 0) != bool(v.front_ccw);
  if ((v.cull == D3D12_CULL_MODE_BACK && !front) || (v.cull == D3D12_CULL_MODE_FRONT && front)) return false;
  const double margin = 1.0 / 16.0;
  if (x0 + 0.5 - margin < vp.TopLeftX || y0 + 0.5 - margin < vp.TopLeftY || x1 - 0.5 + margin > vp.TopLeftX + vp.Width ||
      y1 - 0.5 + margin > vp.TopLeftY + vp.Height)
    return false;
  const double px[4] = {x0 + 0.5, x1 - 0.5, x0 + 0.5, x1 - 0.5}, py[4] = {y0 + 0.5, y0 + 0.5, y1 - 0.5, y1 - 0.5};
  for (unsigned e = 0; e < 3; ++e) {
    const unsigned a = e, b = (e + 1) % 3;
    const double ex = wx[b] - wx[a], ey = wy[b] - wy[a], len = std::sqrt(ex * ex + ey * ey);
    for (unsigned q = 0; q < 4; ++q) {
      const double side = (ex * (py[q] - wy[a]) - ey * (px[q] - wx[a])) * (area > 0 ? 1.0 : -1.0);
      if (side < margin * len) return false;
    }
  }
  return true;
}

// Content key of a uv-map.
struct MapKey {
  std::vector<uint8_t> bytes;
  void put(const void *data, size_t n) { const auto *p = static_cast<const uint8_t *>(data); bytes.insert(bytes.end(), p, p + n); }
  template <typename T> void value(const T &v) { put(&v, sizeof(v)); }
  uint64_t hash() const {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : bytes) { h ^= b; h *= 1099511628211ull; }
    return h;
  }
};

struct MapEntry {
  std::vector<uint8_t> key;
  uint32_t width = 0, height = 0, count = 0;
  Rc<Texture> maps[SM50_POST_CS_MAX_MAPS];
  uint64_t ready_queue = 0;   // queue whose encoding rendered it (later encodings on that queue may read it); under the cache lock
};

class Cache {
public:
  static Cache &Instance() { static Cache c; return c; }
  // The entry of this content, created (with its maps) on first use; null when the cache is full. `ready`: it was rendered by an
  // earlier encoding on queue `queue` (else the caller renders it now and calls Rendered).
  std::shared_ptr<MapEntry> Find(const MapKey &key, uint32_t width, uint32_t height, uint32_t count, uint32_t channels,
                                 MTLD3D12Device *device, uint64_t queue, bool &ready, bool &created) {
    ready = created = false;
    const uint64_t h = key.hash();
    std::lock_guard<std::mutex> lock(mutex_);
    auto &bucket = entries_[h];
    for (auto &e : bucket)
      if (e->key == key.bytes) { ready = e->ready_queue == queue; return e; }
    const uint64_t bytes = uint64_t(width) * height * (channels == 2 ? 8 : 16) * count;
    if (total_ >= kMaxEntries || bytes_ + bytes > (uint64_t(E().cache_mb) << 20)) return {};
    auto e = std::make_shared<MapEntry>();
    e->key = key.bytes; e->width = width; e->height = height; e->count = count;
    for (uint32_t i = 0; i < count; ++i) {
      WMTTextureInfo info{};
      info.type = WMTTextureType2D;
      info.pixel_format = channels == 2 ? WMTPixelFormatRG32Float : WMTPixelFormatRGBA32Float;
      info.width = width; info.height = height; info.depth = 1; info.array_length = 1; info.mipmap_level_count = 1;
      info.sample_count = 1;
      info.usage = WMTTextureUsage(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
      info.options = WMTResourceOptions(WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate);
      Rc<Texture> texture = new Texture(info, device->GetMTLDevice());
      Flags<TextureAllocationFlag> flags;
      flags.set(TextureAllocationFlag::CpuInvisible);
      texture->rename(texture->allocate(flags));
      if (!texture->current() || !texture->current()->texture()) return {};
      device->RegisterResidency(texture->current()->texture());
      e->maps[i] = texture;
    }
    bucket.push_back(e);
    ++total_;
    bytes_ += bytes;
    created = true;
    return e;
  }
  void Rendered(const std::shared_ptr<MapEntry> &e, uint64_t queue) { std::lock_guard<std::mutex> lock(mutex_); e->ready_queue = queue; }
  uint64_t Bytes() { std::lock_guard<std::mutex> lock(mutex_); return bytes_; }
  size_t Entries() { std::lock_guard<std::mutex> lock(mutex_); return total_; }
private:
  // Maps are never evicted (a GPU reader may still be in flight): a key that keeps changing stops at these limits (r_cache).
  static constexpr size_t kMaxEntries = 1024;
  uint64_t bytes_ = 0;
  size_t total_ = 0;
  std::mutex mutex_;
  std::unordered_map<uint64_t, std::vector<std::shared_ptr<MapEntry>>> entries_;
};

// The device word pair the kernels count witness differences into (shared storage, read at Present).
inline WMT::Buffer
WitnessBuffer(MTLD3D12Device *device) {
  auto &s = Get();
  std::lock_guard<std::mutex> lock(s.witness_lock);
  if (!s.witness) {
    WMTBufferInfo info{};
    info.length = 16;
    info.options = WMTResourceOptions(WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
    s.witness = device->GetMTLDevice().newBuffer(info);
    s.witness_words = static_cast<volatile uint32_t *>(info.memory.get_accessible_or_null());
    if (s.witness_words) for (unsigned i = 0; i < 4; ++i) s.witness_words[i] = 0;
    if (s.witness) device->RegisterResidency(s.witness);
  }
  return s.witness;
}

// 03.11 PCS2: the layer's own texture-clear kernel (dxmt_command.metal cs_clear_texture2d_float), for a clear inside a run.
inline WMT::ComputePipelineState
ClearKernel(MTLD3D12Device *device) {
  auto &s = Get();
  std::lock_guard<std::mutex> lock(s.witness_lock);
  if (!s.clear_tried) {
    s.clear_tried = true;
    auto func = device->GetLib().getLibrary().newFunction("cs_clear_texture2d_float");
    WMT::Reference<WMT::Error> err;
    if (func) s.clear_kernel = device->GetMTLDevice().newComputePipelineState(func, err);
  }
  return s.clear_kernel;
}

// The counters line: per-frame averages over the window (counts) and the process totals of the cache.
// 03.11 PCS1: a barrier pass beyond the first rule met inside a candidate run (first 32 per process, with stats or trace only) -
// what the game's post barriers carry (records: type/flags/before/after, o0 = no object).
inline void
BarrierSeen(unsigned extra, bool valid, size_t count, const char *records) {
  if ((!E().stats && !E().trace) || Get().barriers_printed.fetch_add(1, std::memory_order_relaxed) >= 32) return;
  char line[768];
  const DWORD error = GetLastError();
  Line(line, std::snprintf(line, sizeof(line), "dx12_post_chain_barrier extra=0x%x valid=%u records=%zu %s\n", extra,
                           unsigned(valid), count, records));
  SetLastError(error);
}

inline void
Report(const char *why, uint64_t window) {
  auto &s = Get();
  const DWORD error = GetLastError();
  char line[3072];
  const double n = window ? double(window) : 1.0;
  int at = std::snprintf(line, sizeof(line), "dx12_post_chain why=%s window=%llu presents=%llu key=%u armed=%u disabled=%u min_run=%u",
                         why, (unsigned long long)window, (unsigned long long)s.presents.load(std::memory_order_relaxed), Key(),
                         unsigned(Armed()), unsigned(Disabled()), MinRun(Key()));
  unsigned k = 0;
#define DXMT_POST_CHAIN_PRINT(name)                                                                                         \
  {                                                                                                                         \
    const uint64_t v = s.name.load(std::memory_order_relaxed);                                                               \
    if (at > 0 && size_t(at) < sizeof(line))                                                                                \
      at += std::snprintf(line + at, sizeof(line) - at, " " #name "=%.2f", double(v - (window ? s.last[k] : 0)) / n);        \
    if (window) s.last[k] = v;                                                                                              \
    ++k;                                                                                                                    \
  }
  DXMT_POST_CHAIN_COUNTERS(DXMT_POST_CHAIN_PRINT)
#undef DXMT_POST_CHAIN_PRINT
  static const double qpc_ns = [] {   // PCX 03.11: the instrument's own clock cost (1000 QueryPerformanceCounter calls, once per process)
    LARGE_INTEGER a{}, b{}, f{}, x{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (int i = 0; i < 1000; ++i) QueryPerformanceCounter(&x);
    QueryPerformanceCounter(&b);
    return f.QuadPart ? double(b.QuadPart - a.QuadPart) * 1e9 / double(f.QuadPart) / 1000.0 : 0.0;
  }();
  if (at > 0 && size_t(at) < sizeof(line)) at += std::snprintf(line + at, sizeof(line) - at, " qpc_ns=%.1f", qpc_ns);
  if (at > 0 && size_t(at) < sizeof(line))
    at += std::snprintf(line + at, sizeof(line) - at, " cache_maps=%zu cache_mb=%.2f\n", Cache::Instance().Entries(),
                        double(Cache::Instance().Bytes()) / 1048576.0);
  if (at > 0 && size_t(at) < sizeof(line)) Line(line, at);
  SetLastError(error);
}

// Every Present: the witness word (a difference turns the key off) and the counters line.
inline void
Presented() {
  if (!Armed()) return;
  auto &s = Get();
  const auto presents = s.presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (s.witness_words) {
    const uint32_t seen = s.witness_words[0];
    if (seen != s.witness_seen) {
      s.witness_seen = seen;
      s.disabled.store(true, std::memory_order_relaxed);
      char line[160];
      const DWORD error = GetLastError();
      Line(line, std::snprintf(line, sizeof(line), "dx12_post_chain_witness differences=%u last_variant=%u presents=%llu key=off\n",
                               seen, s.witness_words[1], (unsigned long long)presents));
      SetLastError(error);
    }
  }
  if (E().stats && presents % E().stats == 0) Report("present", E().stats);
}
inline void Executed() { if (E().trace && Armed()) Report("trace", 0); }

// Texture creation (d3d12_texture.cpp): a render target of a kernel-writable format, not wider than SW_MAXW, gets ShaderWrite
// usage when armed (both arms of a hot-key A/B; ER's targets are Shared storage with PRIVATE_TEXTURES=0: no lossless compression).
inline bool
TargetUsage(DXGI_FORMAT format, bool render_target, unsigned samples, uint64_t width) {
  if (!Armed() || !render_target || samples > 1 || width > E().sw_maxw) return false;
  switch (format) {
  case DXGI_FORMAT_R11G11B10_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    return true;
  default:
    return false;
  }
}
inline bool
KernelFormat(WMTPixelFormat format) {
  return format == WMTPixelFormatRG11B10Float || format == WMTPixelFormatRGBA16Float || format == WMTPixelFormatRGB10A2Unorm;
}

} // namespace dxmt::post_chain
