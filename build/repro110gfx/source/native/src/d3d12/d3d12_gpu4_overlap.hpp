// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// GPU4 02.11.2026: let the long vertex stage of a big render pass overlap the fragment stage of the pass before it.
// Elden Ring at the church (c20, PASS_TIME run PTI1): shadows 4096x2048 V 1.59 ms + F 0.54 ms, then 4 colour clears 1280x720
// (own encoders), then the G-buffer V 3.89 ms + F 1.30 ms. The G-buffer V starts 91 us after the shadow F ends: nothing else
// runs on the GPU during the shadow F. Native stand (stand/vov.mm, M1 Pro): the shadow F and the G-buffer V overlap only if
// BOTH (a) the G-buffer waits the pre-raster fence (split wait) and (b) no render encoder with a fragment-stage fence wait sits
// between them (a blocked fragment wait holds back the vertex stage of every later render encoder): 4.55 -> 3.76 ms per frame
// on the stand shape; either alone gives nothing.
//
// MACRUNNER_DX12_TYPED_PRECISE=1 (default 0; only with RENDER_FENCE=3): a pass whose pre-raster stages read TYPED SRVs (a
// texture: the reason mode 3 gives it the full wait) gets the split wait when none of the textures those stages can read is
// an attachment written by a render or clear encoder encoded since the last pre-raster fence update in this command buffer.
// Read set: the descriptor table ranges visible to VS/HS/DS/GS intersected with the SRV registers those stages declare (DXIL
// PSV0 / DXBC RDEF), recorded per draw-state change, resolved at encoding (static descriptors: RS 1.1 default; descriptors are
// read at encoding as the hazard observer does). Not free: unreadable declarations / tables, indirect root mutation, UAV or heap
// indexing before rasterization (pre_raster_hard), no pre-raster fence update yet in this command buffer (the textures of the
// command buffers before are covered only after one), an aliasing barrier, or a compute / blit encoder that skipped the
// pre-raster update under PRERASTER_PRECISE since the last one (its texture writes are not covered). 9 = NEGATIVE CONTROL for
// probes only: every pass with recorded reads is free (the race the rule prevents).
// MACRUNNER_DX12_CLEAR_SINK=1 (default 0; needs DEFER_CLEARS + FOLD_CLEARS): a deferred full colour clear that the next barrier
// moves from RENDER_TARGET to shader-read states only (named transition, all subresources, no flags, no aliasing) keeps being
// carried ("sunk") past render passes whose read set (all stages, SRV registers, recorded for passes with >= 4 colour targets
// or depth-only >= 2048 wide) provably does not contain its texture; it is encoded before the first pass that may read it,
// before any non-render pass, at a barrier that touches it otherwise, or at the end. A barrier back to RENDER_TARGET (from
// read states only) returns it to an ordinary pending clear (it may then fold into the next pass's load action).
// 9 = NEGATIVE CONTROL: no read check (sinks past every render pass).
// Counters: FENCE_STATS window line `dx12_gpu4` (per frame); MACRUNNER_DX12_GPU4_TRACE=1: cumulative after every
// ExecuteCommandLists (probes).
// ON THE FLY (curator 02.11 15:16: one game launch, A B B A world segments): both keys are entries of the shared hot-key table
// (src/dxmt/dxmt_hotkeys.hpp, MACRUNNER_DX12_HOTKEYS_FILE=<unix path>, lines `NAME=0|1`, polled at most once a second at a present;
// every change prints `dxmt_hotkeys applied NAME=V old=O presents=N tick_ms=T`). 9 only from the environment. Both are safe to switch: each
// ExecuteCommandLists encoding reads them once, a pass recorded partly with the key off counts as unknown (full wait / clear
// encoded in place), and the pipelines' declarations are read at creation whenever the file is set (the keys may start at 0).
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "com/com_pointer.hpp"
#include "d3d12_render_fence.hpp"
#include "dxmt_hotkeys.hpp"   // shared hot-key table (SHADERS-GPU 02.11)

namespace dxmt {
class Texture;
}

namespace dxmt::gpu4 {

inline unsigned ReadDigit(const char *name) {
  const DWORD error = GetLastError();
  char value[4] = {};
  unsigned v = 0;
  if (GetEnvironmentVariableA(name, value, sizeof(value)) == 1 && value[0] >= '0' && value[0] <= '9') v = unsigned(value[0] - '0');
  SetLastError(error);
  return v;
}
// The keys live in the shared hot-key table (dxmt_hotkeys.hpp, SHADERS-GPU 02.11; MACRUNNER_DX12_HOTKEYS_FILE, polled at presents):
// initial value = the environment variable, a file line `NAME=0|1` switches it at a frame boundary. 9 (negative control) is honoured
// only when the environment set it at start; any other value reads as 0.
struct Keys {
  unsigned typed_env = 0, sink_env = 0, late_env = 0;
  bool armed = false;   // the keys were on at start or may be switched on later: read declarations, advance binding epochs
  Keys() {
    typed_env = ReadDigit("MACRUNNER_DX12_TYPED_PRECISE"); sink_env = ReadDigit("MACRUNNER_DX12_CLEAR_SINK");
    late_env = ReadDigit("MACRUNNER_DX12_SHADOW_LATE");
    armed = typed_env == 1 || typed_env == 9 || sink_env == 1 || sink_env == 9 || late_env == 1 || late_env == 9 || hotkeys::Enabled();
  }
};
inline Keys &K() { static Keys k; return k; }
inline unsigned KeyValue(hotkeys::Key key, unsigned env) {
  const int v = hotkeys::Value(key);
  return v == 1 ? 1u : (v == 9 && env == 9) ? 9u : 0u;
}
// 0 off, 1 on, 9 negative control
inline unsigned TypedPrecise() { return render_fence::Mode() == 3 ? KeyValue(hotkeys::TypedPrecise, K().typed_env) : 0u; }
inline unsigned ClearSink() { return KeyValue(hotkeys::ClearSink, K().sink_env); }
// MACRUNNER_DX12_TS_NEIGHBOR=1 (default 0; with TS_NODRAIN=1, not in RENDER_FENCE=2): an EndQuery(TIMESTAMP) pass whose next pass in the
// list is a plain copy pass gets no blit encoder of its own: the copy's encoder samples its START (it waits the common fence first, as
// the timestamp encoder did). Elden Ring c20 PTI1: T_begin sits between two copies, 52-60 us of GPU idle per frame around it, of which
// ~15 us is the copy->copy step anyway. Anything else between them -> the timestamp is encoded as before (flush).
// 02.11 16:5x (G4S1 in the world: ts_moved 0): also when the next pass is a ResolveQueryData of OTHER queries (Elden Ring's form:
// T_begin is followed by the resolve of earlier queries); never when that resolve reads T's own sample.
inline unsigned TsNeighbor() { return hotkeys::Value(hotkeys::TsNeighbor) == 1 ? 1u : 0u; }
// MACRUNNER_DX12_VSERIAL=1 (default 0; RENDER_FENCE=3; meant with TYPED_PRECISE + CLEAR_SINK). G4S1 in the world: the G-buffer vertex stage
// started ~0.64 ms BEFORE the shadow fragments, i.e. on top of the shadow VERTEX stage, and both stretched (1.5 ms of stretch for 1.28 ms
// of overlap: both vertex phases are bound by index submission, GBUF-VS 25.10). With the key, a big depth-only render encoder (>= 2048
// wide: the shadow pass) also updates a vertex-done fence after its VERTEX stage, and the next split-waiting render encoder of the
// command buffer waits it before its pre-raster stages: its vertex work may overlap the shadow fragments only. Ordering only.
inline unsigned VSerial() { return hotkeys::Value(hotkeys::VSerial) == 1 ? 1u : 0u; }
// MACRUNNER_DX12_SHADOW_LATE=1 (default 0; RENDER_FENCE=3; meant with TYPED_PRECISE + CLEAR_SINK). Order swap without a new pass: a big
// depth-only render pass S (>= 2048 wide, no colour: the shadow map; plain: no queries, no tessellation / geometry / mesh emulation, no
// UAV writes, read set known) is not encoded where it was recorded; it (with the passes right after it on the same targets) waits and
// is encoded before the first later pass that needs it: any pass that is not a render pass, clear or barrier; a render pass that is not
// big with a known read set, uses S's attachments, reads them (all-stage SRV set), writes memory, or writes an attachment S reads; a
// clear of S's attachment or of a texture S reads; an aliasing / UAV / unnamed barrier; the end of the ExecuteCommandLists call. In
// Elden Ring the shadow pass (V 1.59-1.79 ms, index bound) then goes after the G-buffer (F 1.22-1.41 ms): its vertex stage overlaps
// the G-buffer fragments (different units), its fragments follow. Every reordered pass is independent of the passes it moves past by
// the rule above (D3D12 orders them only by their data). 9 = NEGATIVE CONTROL for probes: no read / attachment check against later
// render passes (S flushes only at a non-render pass or the end).
inline unsigned ShadowLate() { return render_fence::Mode() == 3 ? KeyValue(hotkeys::ShadowLate, K().late_env) : 0u; }
inline bool Trace() { static const bool v = ReadDigit("MACRUNNER_DX12_GPU4_TRACE") == 1; return v; }
inline bool Any() { return TypedPrecise() || ClearSink() || ShadowLate(); }   // the read-set keys (TS_NEIGHBOR needs no recording)
inline bool Armed() { return K().armed; }

// One descriptor span a draw state can read: CBV/SRV/UAV heap (kept alive by Reads::heaps), absolute heap indices.
struct Span {
  const void *heap = nullptr;
  uint32_t start = 0, count = 0;
};
struct Reads {
  std::vector<Span> pre;   // SRV registers of the pre-raster stages (TYPED_PRECISE; only draws of typed SRV-only pipelines)
  std::vector<Span> all;   // SRV registers of every stage (CLEAR_SINK; only big passes)
  std::vector<Com<ID3D12DescriptorHeap>> heaps;
  bool pre_unknown = false, all_unknown = false;
  bool want_all = false;   // the pass is big enough to record the all-stage set
  uint64_t last_epoch = UINT64_MAX;
  const void *last_pso = nullptr, *last_root = nullptr;
};
constexpr uint32_t kSpanCap = 8192;   // a span longer than this is not resolved (the set counts as unknown)

struct Stats {
  std::atomic<uint64_t> typed_free{}, typed_w{}, typed_skip{}, typed_start{}, typed_unknown{}, typed_alias{};
  std::atomic<uint64_t> sink_marked{}, sink_passed{}, sink_unsunk{}, stop_read{}, stop_unknown{}, stop_attach{}, stop_other{},
      stop_barrier{}, stop_end{}, ts_moved{}, ts_flushed{}, vserial_marked{};
  // SHADOW_LATE: chains deferred; passes they went past (render / clear / barrier); why the chain was encoded (flush_*)
  std::atomic<uint64_t> late_deferred{}, late_past_render{}, late_past_clear{}, late_past_barrier{}, late_flush_attach{},
      late_flush_read{}, late_flush_unknown{}, late_flush_barrier{}, late_flush_other{}, late_flush_end{}, late_refused{};
  // (в) geometry census of the big passes (counting only; GPU4_STATS): s = depth-only >= 2048 wide, g = >= 4 colours.
  // ix = indices x instances, dr = indexed draws, by index format; vx = non-indexed vertices x instances; inst = draws with
  // InstanceCount > 1; ind = indirect draws (counts unknown at record time).
  std::atomic<uint64_t> ix_s16{}, ix_s32{}, ix_g16{}, ix_g32{}, dr_s16{}, dr_s32{}, dr_g16{}, dr_g32{}, vx_s{}, vx_g{}, inst_s{},
      inst_g{}, ind_s{}, ind_g{};
};
inline Stats &Get() { static Stats s; return s; }
// MACRUNNER_DX12_GPU4_STATS=N: a `dx12_gpu4` line every N presents (counting only); default = FENCE_STATS' period.
inline unsigned StatsPeriod() {
  static const unsigned n = [] {
    const DWORD error = GetLastError();
    char value[12] = {};
    unsigned parsed = 0;
    const DWORD len = GetEnvironmentVariableA("MACRUNNER_DX12_GPU4_STATS", value, sizeof(value));
    if (len && len < 8) for (DWORD i = 0; i < len && value[i] >= '0' && value[i] <= '9'; ++i) parsed = parsed * 10 + unsigned(value[i] - '0');
    SetLastError(error);
    return parsed ? parsed : render_fence::StatsPeriod();
  }();
  return n;
}
inline void Note(std::atomic<uint64_t> &a, uint64_t n = 1) {
  if (StatsPeriod() || Trace()) a.fetch_add(n, std::memory_order_relaxed);
}
inline void ExecuteTrace() {
  if (!Trace()) return;
  static std::atomic<uint64_t> calls{0};
  hotkeys::Poll();   // probes have no presents: poll the hot-key file per submission (at most once a second)
  auto &s = Get();
  std::fprintf(stderr, "dx12_gpu4_trace execute=%llu typed_precise=%u clear_sink=%u typed_free=%llu typed_w=%llu typed_skip=%llu typed_start=%llu"
               " typed_unknown=%llu typed_alias=%llu sink_marked=%llu sink_passed=%llu sink_unsunk=%llu stop_read=%llu stop_unknown=%llu"
               " stop_attach=%llu stop_other=%llu stop_barrier=%llu stop_end=%llu ts_moved=%llu ts_flushed=%llu vserial=%llu"
               " late_deferred=%llu late_past_render=%llu late_past_clear=%llu late_past_barrier=%llu late_flush_attach=%llu late_flush_read=%llu"
               " late_flush_unknown=%llu late_flush_barrier=%llu late_flush_other=%llu late_flush_end=%llu late_refused=%llu\n",
               (unsigned long long)(calls.fetch_add(1, std::memory_order_relaxed) + 1), TypedPrecise(), ClearSink(),
               (unsigned long long)s.typed_free.load(), (unsigned long long)s.typed_w.load(), (unsigned long long)s.typed_skip.load(),
               (unsigned long long)s.typed_start.load(), (unsigned long long)s.typed_unknown.load(), (unsigned long long)s.typed_alias.load(),
               (unsigned long long)s.sink_marked.load(), (unsigned long long)s.sink_passed.load(), (unsigned long long)s.sink_unsunk.load(),
               (unsigned long long)s.stop_read.load(), (unsigned long long)s.stop_unknown.load(), (unsigned long long)s.stop_attach.load(),
               (unsigned long long)s.stop_other.load(), (unsigned long long)s.stop_barrier.load(), (unsigned long long)s.stop_end.load(),
               (unsigned long long)s.ts_moved.load(), (unsigned long long)s.ts_flushed.load(), (unsigned long long)s.vserial_marked.load(),
               (unsigned long long)s.late_deferred.load(), (unsigned long long)s.late_past_render.load(), (unsigned long long)s.late_past_clear.load(),
               (unsigned long long)s.late_past_barrier.load(), (unsigned long long)s.late_flush_attach.load(), (unsigned long long)s.late_flush_read.load(),
               (unsigned long long)s.late_flush_unknown.load(), (unsigned long long)s.late_flush_barrier.load(), (unsigned long long)s.late_flush_other.load(),
               (unsigned long long)s.late_flush_end.load(), (unsigned long long)s.late_refused.load());
}
inline void Presented() {
  static std::atomic<uint64_t> presents{0};
  const uint64_t p = presents.fetch_add(1, std::memory_order_relaxed) + 1;
  const unsigned n = StatsPeriod();
  if (!n || !Armed() || Trace()) return;
  if (p % n) return;
  auto &s = Get();
  const auto take = [&](std::atomic<uint64_t> &a) { return double(a.exchange(0, std::memory_order_relaxed)) / double(n); };
  std::fprintf(stderr, "dx12_gpu4 presents=%llu window=%u typed_precise=%u clear_sink=%u typed_free=%.2f typed_w=%.2f typed_skip=%.2f"
               " typed_start=%.2f typed_unknown=%.2f typed_alias=%.2f sink_marked=%.2f sink_passed=%.2f sink_unsunk=%.2f stop_read=%.2f"
               " stop_unknown=%.2f stop_attach=%.2f stop_other=%.2f stop_barrier=%.2f stop_end=%.2f ts_moved=%.2f ts_flushed=%.2f vserial=%.2f\n",
               (unsigned long long)p, n, TypedPrecise(), ClearSink(), take(s.typed_free), take(s.typed_w), take(s.typed_skip),
               take(s.typed_start), take(s.typed_unknown), take(s.typed_alias), take(s.sink_marked), take(s.sink_passed),
               take(s.sink_unsunk), take(s.stop_read), take(s.stop_unknown), take(s.stop_attach), take(s.stop_other),
               take(s.stop_barrier), take(s.stop_end), take(s.ts_moved), take(s.ts_flushed), take(s.vserial_marked));
  std::fprintf(stderr, "dx12_gpu4_late presents=%llu window=%u shadow_late=%u deferred=%.2f past_render=%.2f past_clear=%.2f past_barrier=%.2f"
               " flush_attach=%.2f flush_read=%.2f flush_unknown=%.2f flush_barrier=%.2f flush_other=%.2f flush_end=%.2f refused=%.2f\n",
               (unsigned long long)p, n, ShadowLate(), take(s.late_deferred), take(s.late_past_render), take(s.late_past_clear),
               take(s.late_past_barrier), take(s.late_flush_attach), take(s.late_flush_read), take(s.late_flush_unknown),
               take(s.late_flush_barrier), take(s.late_flush_other), take(s.late_flush_end), take(s.late_refused));
  std::fprintf(stderr, "dx12_gpu4_geom presents=%llu window=%u ix_s16=%.0f ix_s32=%.0f ix_g16=%.0f ix_g32=%.0f dr_s16=%.1f dr_s32=%.1f"
               " dr_g16=%.1f dr_g32=%.1f vx_s=%.0f vx_g=%.0f inst_s=%.1f inst_g=%.1f ind_s=%.1f ind_g=%.1f\n",
               (unsigned long long)p, n, take(s.ix_s16), take(s.ix_s32), take(s.ix_g16), take(s.ix_g32), take(s.dr_s16), take(s.dr_s32),
               take(s.dr_g16), take(s.dr_g32), take(s.vx_s), take(s.vx_g), take(s.inst_s), take(s.inst_g), take(s.ind_s), take(s.ind_g));
}

} // namespace dxmt::gpu4
