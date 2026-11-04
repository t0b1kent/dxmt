// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// LIGHTRN 02.11.2026: MACRUNNER_DX12_LIGHT_RENAME (default 0) — scratch renaming for the light <-> compute ping-pong.
//
// Elden Ring at the church (c20/c23 PASS_TIME): 14 light-volume batches per frame; between EVERY two batches the compute shader
// CS_EnvMapRegion_CopyCubeToCubeLerp (DXIL 8c956fb0) lerps two cube maps into ONE shared scratch cube (8 mips, UAV views u0..u7)
// that the next batch samples in its pixel shader (t25/t26). The shared scratch makes every batch a RAW (compute -> light F)
// plus WAR (light F -> next compute) pair: r->c and c->r encoder switches with fences, 41 us F(L)->F(L') per batch, ~0.77 ms/frame.
//
// With the key, in one ExecuteCommandLists encoding (one Metal command buffer), a RUN C1 X.. C2 X.. .. Cn of candidate compute
// passes Cj that write the same texture S, separated only by render passes and barrier-only passes, is encoded as
//   [C1 -> S_1, C2 -> S_2, .., Cn -> S] in ONE compute encoder at C1's place (concurrent dispatch: independent writes), then the
//   passes between them in their order, with every descriptor of S they can read redirected to the copy S_j of the batch before.
// S_j are pooled private allocations of S's own texture (same format, size, mips, usage); the last batch writes S itself, so S
// ends exactly as without the key. Redirection = a copy of the descriptor table (32-byte GPU records) with S's records pointing to
// S_j and a copy of the root-argument block pointing to that table; the recorded setbuffer commands are patched for this encoding
// only and restored afterwards (a list executed again is planned again). Legal because:
//   * a candidate compute pass has exactly one direct dispatch of a pipeline with the full-overwrite FORM (DXIL, computed at
//     creation: d3d12_light_rename_form.hpp; no hash), every declared UAV resolves to a view of the same texture S, and for
//     every such view the form's straight-path store conditions hold for every thread of the view's box with THIS dispatch's
//     constant-buffer words (read at encoding from an UPLOAD heap: the GPU never writes those; early (Close-time) encodings are
//     re-checked at commit and dropped when the words changed) and grid >= view extent: every texel of every written view is
//     written by this dispatch, so S_j holds exactly what S would hold after Cj;
//   * readers between Cj and Cj+1 only read views of S whose subresources Cj fully wrote (else the run ends at Cj); S_j (j<n)
//     are never read after the run;
//   * no pass in the run writes S or anything a later candidate reads (render passes: attachments, no UAV writes; barrier passes:
//     no aliasing, no record naming a later candidate's input; any other pass ends the run), the candidates do not read S;
//   * descriptors read at encoding are the ones the GPU uses: with static descriptors (RS 1.1 default) from the time the table is set;
//     with volatile ones (RS 1.0 or DESCRIPTORS_VOLATILE: Elden Ring's root signatures, LRS1 03.11) they may change until
//     ExecuteCommandLists, so an encoding AT ExecuteCommandLists reads final descriptors, and an early (Close-time) encoding keeps a hash
//     of every descriptor range it read or copied and is dropped at commit when one changed (early_desc_changed), as for the CB words.
// Anything else -> the current path (every refusal is counted by reason).
// COPY-IN (03.11; Elden Ring LRS2: every 8c956fb0 dispatch refused `partial`, 8.9/frame): a candidate whose views are NOT all proven
// fully written is still taken when its form is PURE (every textureStore is an identity store on a straight path whose conditions
// read only thread IDs, CB words and literals): the texel set W it writes is then a function of (form, grid, views, the values of
// those CB words) = its signature. A run joins only candidates with EQUAL signatures, so every batch writes the same W, and a texel
// outside W keeps its pre-run value through the whole run (no pass of the run writes S). Before the run's compute encoder one blit
// encoder copies S (pre-run) into S_1..S_n-1 on every subresource the batches do not fully write; Cj then writes W into S_j, so
// S_j == S after Cj without the key on EVERY texel, and readers may read any view of S. The last batch writes S itself.
// The first PARTIAL_LOG such candidates (and refused partial ones) print a `dx12_lightrn_partial` line with what the proof saw.
// Negative controls (environment, probes): NOCOPYIN=1 (blits not encoded) and NOSIG=1 (runs across signatures) must FAIL.
// (b) 03.11 (Elden Ring LRS3: runs of 2, cut by light passes `pass_writes` 6.1/frame): a UAV descriptor of S in a table of a render
// pass none of whose stages declares a UAV (may_write_memory = 0, from the application's bytecode) is not a write: a shader reaches
// only the UAVs it declares and this layer has no direct heap indexing. It is neither counted nor redirected. Passes whose stages
// declare a UAV stay refused (pass_writes). The first PASS_LOG refused passes print `dx12_lightrn_pass`. UAVTRUST=1 = negative control.
// 9 = NEGATIVE CONTROL for probes (environment only): no coverage proof (renames partial writers too) -> must FAIL there.
// Hot key (shared table, src/dxmt/dxmt_hotkeys.hpp): read once per ExecuteCommandLists encoding; recording (root blocks of the
// render passes after a candidate dispatch in the same list) and the form are done whenever the key may be used (armed).
// Counters: `dx12_lightrn` line every MACRUNNER_DX12_LIGHTRN_STATS presents (default: GPU4_STATS / FENCE_STATS period);
// MACRUNNER_DX12_LIGHTRN_TRACE=1: cumulative line after every ExecuteCommandLists (probes).
// Pool cap: MACRUNNER_DX12_LIGHTRN_POOL_MB (default 128; copies of the scratch + 256 KB table blocks): copies beyond it are not made
// (run truncated, counted pool_full). Elden Ring: ~13 copies per frame of a ~1 MB cube (estimate), x frames in flight / early lists.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include "com/com_pointer.hpp"
#include "d3d12_argument_access.hpp"
#include "d3d12_light_rename_form.hpp"
#include "dxmt_hotkeys.hpp"
#include "winemetal.h"

namespace dxmt {
class MTLD3D12RootSignature;
}

namespace dxmt::light_rename {

inline unsigned ReadNumber(const char *name, unsigned def) {
  const DWORD error = GetLastError();
  char value[16] = {};
  const DWORD len = GetEnvironmentVariableA(name, value, sizeof(value));
  SetLastError(error);
  if (!len || len >= sizeof(value)) return def;
  unsigned v = 0;
  for (DWORD i = 0; i < len; ++i) { if (value[i] < '0' || value[i] > '9') return def; v = v * 10 + unsigned(value[i] - '0'); }
  return v;
}
struct Env {
  unsigned key_env = 0;
  bool armed = false, trace = false;
  unsigned pool_mb = 128, stats = 0;
  bool snapcheck = true;   // MACRUNNER_DX12_LIGHTRN_SNAPCHECK=0: NEGATIVE CONTROL for probes (Close-time CB words not re-checked)
  // LIGHTRN 03.11 copy-in (partial writers): MACRUNNER_DX12_LIGHTRN_COPYIN=0 refuses them as before (partial);
  // NOCOPYIN=1 / NOSIG=1: NEGATIVE CONTROLS for probes (copy-in blits not encoded / runs joined across different signatures);
  // PARTIAL_LOG = how many `dx12_lightrn_partial` detail lines (refused or copy-in candidates) a process prints (default 16).
  bool copyin = true, nocopyin = false, nosig = false;
  unsigned partial_log = 16;
  // 03.11 (b): PASS_LOG = how many `dx12_lightrn_pass` lines (render passes refused inside a run) a process prints (default 16);
  // UAVTRUST=1: NEGATIVE CONTROL for probes (passes whose stages declare UAVs are treated as non-writers) -> must FAIL there.
  unsigned pass_log = 16;
  bool uavtrust = false;
  Env() {
    key_env = ReadNumber("MACRUNNER_DX12_LIGHT_RENAME", 0);
    trace = ReadNumber("MACRUNNER_DX12_LIGHTRN_TRACE", 0) == 1;
    pool_mb = ReadNumber("MACRUNNER_DX12_LIGHTRN_POOL_MB", 128);
    snapcheck = ReadNumber("MACRUNNER_DX12_LIGHTRN_SNAPCHECK", 1) != 0;
    copyin = ReadNumber("MACRUNNER_DX12_LIGHTRN_COPYIN", 1) != 0;
    nocopyin = ReadNumber("MACRUNNER_DX12_LIGHTRN_NOCOPYIN", 0) == 1;
    nosig = ReadNumber("MACRUNNER_DX12_LIGHTRN_NOSIG", 0) == 1;
    partial_log = ReadNumber("MACRUNNER_DX12_LIGHTRN_PARTIAL_LOG", 16);
    pass_log = ReadNumber("MACRUNNER_DX12_LIGHTRN_PASS_LOG", 16);
    uavtrust = ReadNumber("MACRUNNER_DX12_LIGHTRN_UAVTRUST", 0) == 1;
    stats = ReadNumber("MACRUNNER_DX12_LIGHTRN_STATS", 0);
    if (!stats) stats = ReadNumber("MACRUNNER_DX12_GPU4_STATS", 0);
    if (!stats) stats = ReadNumber("MACRUNNER_DX12_FENCE_STATS", 0);
    armed = key_env == 1 || key_env == 9 || hotkeys::Enabled();
  }
};
inline Env &E() { static Env e; return e; }
inline bool Armed() { return E().armed; }
inline bool Trace() { return E().trace; }
// 0 off, 1 on, 9 negative control (environment only)
inline unsigned Key() {
  if (!Armed()) return 0;
  const int v = hotkeys::Value(hotkeys::LightRename);
  return v == 1 ? 1u : (v == 9 && E().key_env == 9) ? 9u : 0u;
}

// ---- what the command list records (only while armed) ----------------------------------------------------------------------
// One root-argument emission of a render pass recorded after a candidate dispatch in the same list.
struct RootRec {
  wmtcmd_render_setbuffer *cmd[3] = {};   // vertex/object, mesh, fragment setbuffer commands of this block
  uint8_t ncmd = 0;
  const uint64_t *words = nullptr;        // the block (allocator GPU heap, CPU view)
  MTLD3D12RootSignature *root = nullptr;
  root_argument::HeapRange heap{};        // CBV/SRV/UAV heap bound at the emission
  ID3D12DescriptorHeap *heap_object = nullptr;
};
// Raw pointers: the application keeps root signatures and descriptor heaps alive while a list that uses them may execute, and
// every encoding (Close or ExecuteCommandLists) happens while the list is about to execute.
struct PassRoots {
  std::vector<RootRec> roots;
  bool unknown = false;                   // root arguments changed in a way not recorded (ExecuteIndirect bindings, bundles)
};
// The dispatch of a compute pass whose pipeline has a form (only while armed).
struct DispatchRec {
  wmtcmd_compute_setbuffer *root_cmd = nullptr;
  const uint64_t *words = nullptr;
  MTLD3D12RootSignature *root = nullptr;
  root_argument::HeapRange heap{};
  ID3D12DescriptorHeap *heap_object = nullptr;
  uint32_t groups[3] = {}, threads[3] = {};
  std::shared_ptr<const form::Form> form;
};
struct PassDispatch {
  std::vector<DispatchRec> d;
  uint32_t dispatches = 0;                // every dispatch of the pass (direct, indirect, ExecuteIndirect)
  bool unknown = false;
};

// ---- counters -----------------------------------------------------------------------------------------------------------------
enum Reason : unsigned {
  RNoRecord, RMulti, RForm, RVolatile, RUavUnresolved, RUavNotTexture, RUavMixed, RPartial, RCbUnresolved, RCbHeap,
  RSrvUnresolved, RSrvIsS, RInputWritten, RInputBarrier, RSingle, RViewUncovered, RPassUnknown, RPassWrites, RPassAttachS,
  ROtherPass, RPoolFull, RUnbounded, RAlias, RFinalState, RNoMemory, REarlyCbChanged, RTableUnresolved, REarlyDescChanged,
  RViewShape, RSigMismatch, RCount
};
inline const char *ReasonName(unsigned r) {
  static const char *names[RCount] = {"no_record", "multi", "form", "volatile", "uav_unresolved", "uav_not_texture", "uav_mixed",
      "partial", "cb_unresolved", "cb_heap", "srv_unresolved", "srv_is_s", "input_written", "input_barrier", "single",
      "view_uncovered", "pass_unknown", "pass_writes", "pass_attach_s", "other_pass", "pool_full", "unbounded", "alias",
      "final_state", "no_memory", "early_cb_changed", "table_unresolved", "early_desc_changed", "view_shape", "sig_mismatch"};
  return r < RCount ? names[r] : "?";
}
struct Stats {
  std::atomic<uint64_t> encodings{}, candidates{}, runs{}, renamed{}, kept{}, passes_redirected{}, tables_redirected{},
      roots_redirected{}, forms_ok{}, forms_refused{}, bytes_tables{}, early_stash{}, early_committed{}, desc_ranges{};
  // copy-in (03.11): partial candidates accepted (pure form), runs with copy-in, blit commands, bytes copied (format-class estimate)
  std::atomic<uint64_t> copyin_cands{}, copyin_runs{}, copyin_cmds{}, copyin_bytes{};
  // root signatures created while armed: static descriptors (RS 1.1, no DESCRIPTORS_VOLATILE), RS 1.0, RS 1.1 with a volatile range
  std::atomic<uint64_t> rs_static{}, rs_v10{}, rs_volatile{};
  std::atomic<uint64_t> reason[RCount]{};
  std::atomic<uint64_t> pool_bytes{}, pool_peak{}, pool_entries{};
};
inline Stats &Get() { static Stats s; return s; }
inline unsigned StatsPeriod() { return E().stats; }
inline void Note(std::atomic<uint64_t> &a, uint64_t n = 1) { if (StatsPeriod() || Trace()) a.fetch_add(n, std::memory_order_relaxed); }
inline void NoteReason(unsigned r) { if (r < RCount) Note(Get().reason[r]); }
inline void PoolBytes(int64_t delta) {
  auto &s = Get();
  const uint64_t now = s.pool_bytes.fetch_add(uint64_t(delta), std::memory_order_relaxed) + uint64_t(delta);
  uint64_t peak = s.pool_peak.load(std::memory_order_relaxed);
  while (now > peak && !s.pool_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {}
}
inline int Format(char *b, size_t n, bool take, unsigned period) {
  auto &s = Get();
  const auto v = [&](std::atomic<uint64_t> &a) -> double {
    return take ? double(a.exchange(0, std::memory_order_relaxed)) / double(period ? period : 1) : double(a.load(std::memory_order_relaxed));
  };
  int at = snprintf(b, n, "key=%u encodings=%.2f candidates=%.2f runs=%.2f renamed=%.2f kept=%.2f passes_redirected=%.2f"
                    " tables_redirected=%.2f roots_redirected=%.2f early_stash=%.2f early_committed=%.2f desc_ranges=%.2f forms_ok=%.0f"
                    " forms_refused=%.0f rs_static=%.0f rs_v10=%.0f rs_volatile=%.0f table_kb=%.1f copyin_cands=%.2f copyin_runs=%.2f"
                    " copyin_cmds=%.2f copyin_kb=%.1f",
                    Key(), v(s.encodings), v(s.candidates), v(s.runs), v(s.renamed), v(s.kept), v(s.passes_redirected),
                    v(s.tables_redirected), v(s.roots_redirected), v(s.early_stash), v(s.early_committed), v(s.desc_ranges),
                    double(s.forms_ok.load()), double(s.forms_refused.load()), double(s.rs_static.load()), double(s.rs_v10.load()),
                    double(s.rs_volatile.load()), v(s.bytes_tables) / 1024.0, v(s.copyin_cands), v(s.copyin_runs), v(s.copyin_cmds),
                    v(s.copyin_bytes) / 1024.0);
  for (unsigned r = 0; r < RCount && at > 0 && size_t(at) < n; ++r) {
    const double x = v(s.reason[r]);
    if (x != 0) at += snprintf(b + at, n - size_t(at), " %s=%.2f", ReasonName(r), x);
  }
  if (at > 0 && size_t(at) < n)
    at += snprintf(b + at, n - size_t(at), " pool_mb=%.2f peak_mb=%.2f pool_entries=%llu", double(s.pool_bytes.load()) / 1048576.0,
                   double(s.pool_peak.load()) / 1048576.0, (unsigned long long)s.pool_entries.load());
  return at;
}
inline void Line(const char *prefix, bool take, unsigned period, unsigned long long counter) {
  char b[2048];
  int at = snprintf(b, sizeof(b), "%s%llu ", prefix, counter);
  if (at < 0) return;
  const int more = Format(b + at, sizeof(b) - size_t(at) - 2, take, period);
  if (more < 0) return;
  at += more;
  if (size_t(at) > sizeof(b) - 2) at = int(sizeof(b) - 2);
  b[at++] = '\n'; b[at] = 0;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  DWORD w = 0;
  if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, b, DWORD(at), &w, nullptr);
}
inline void ExecuteTrace() {
  if (!Trace() || !Armed()) return;
  static std::atomic<uint64_t> calls{0};
  hotkeys::Poll();   // probes have no presents
  Line("dx12_lightrn_trace execute=", false, 1, (unsigned long long)(calls.fetch_add(1, std::memory_order_relaxed) + 1));
}
inline void Presented() {
  static std::atomic<uint64_t> presents{0};
  const uint64_t p = presents.fetch_add(1, std::memory_order_relaxed) + 1;
  const unsigned n = StatsPeriod();
  if (!n || !Armed() || Trace() || p % n) return;
  char prefix[96];
  snprintf(prefix, sizeof(prefix), "dx12_lightrn window=%u presents=", n);
  Line(prefix, true, n, (unsigned long long)p);
}

} // namespace dxmt::light_rename
