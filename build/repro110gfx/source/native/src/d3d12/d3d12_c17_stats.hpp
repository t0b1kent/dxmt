// SPDX-License-Identifier: LGPL-2.1-or-later
// MacRunner 2026 (TESS-GS c17): keys on top of ENCODERS and the reasons they record. All keys default to 0; with every
// key 0 the layer encodes exactly what it encoded before. Diagnostics print to stderr (WriteFile on STD_ERROR_HANDLE),
// never through Wine debug channels, and only as totals: one line per non-zero reason every N presents.
//
//   MACRUNNER_DX12_DEFER_MARKERS=1        WriteBufferImmediate MARKER_OUT floats with the ENCODERS copy work (needs
//                                         DEFER_COPIES or BLIT_CHAIN to have an open place to float in): it no longer ends
//                                         the open pass, lands at the next pass boundary in recording order, at the latest
//                                         at Close(). A marker write to the same 4 bytes as an earlier floating marker,
//                                         with nothing in between touching those bytes, replaces it (only the last value
//                                         is observable: nothing can read the destination before the flush).
//   MACRUNNER_DX12_DEFER_MARKERS=2        the same for MARKER_IN.
//   MACRUNNER_DX12_MERGE_DEPENDENCIES=1   (implies MERGE_SAFE) passes with the same targets merge across a barrier unless it
//                                         concerns an attachment of the chain (any subresource), names no object, or is an
//                                         aliasing barrier with an unnamed side; UAV-state transitions, UAV barriers and
//                                         aliasing of other objects are crossed when no draw of the chain or of the next
//                                         pass uses a pipeline that can write memory (UAV declared in any stage).
//   MACRUNNER_DX12_CLEAR_SUBRESOURCE=1    (with DEFER_CLEARS) a barrier about another mip level of a pending clear's texture
//                                         does not encode the clear; two pending clears of one texture covering different
//                                         aspects (depth / stencil) or disjoint mip / array ranges may be reordered, so the
//                                         earlier one can still fold when the later one cannot.
//   MACRUNNER_DX12_QUERY_NO_COLOR=1       a pipeline without a pixel shader (and without alpha-to-coverage) gets a twin
//                                         without colour attachments; a recorded render pass whose draws all use such
//                                         pipelines and that has a depth or stencil attachment drops its colour
//                                         attachments at Close() (no load/store of colours; Elden Ring's occlusion query
//                                         draws). Pending clears of the dropped colours are then encoded on their own.
//   MACRUNNER_DX12_MERGE_REDUNDANT_TRANSITIONS=1  two passes with the same targets, separated only by barrier passes
//                                         whose transitions form closed per-subresource cycles RT|DEPTH_WRITE -> SRV ->
//                                         back, stay in one Metal encoder (no store/load); descriptor bindings veto.
//   MACRUNNER_DX12_MERGE_REDUNDANT_TRANSITIONS=2  the same, bindings recorded before the head pass do not veto
//                                         (empty_binding_tolerated counts these); rejects are split by cause.
//                                         With C17_STATS and any value of this key, every cross-list pair is also judged
//                                         by the mode-2 rules without acting on it: xl_e2_* counters (observation only).
//   MACRUNNER_DX12_C17_STATS=<N>          every N presents: "dx12_c17_reason frames=F name=<reason> count=<total>"
//                                         lines (cumulative). Also counts, at ExecuteCommandLists with EARLY_ENCODE,
//                                         adjacent lists whose boundary passes have the same targets (cross-list chains).
//   MACRUNNER_DX12_C17_STATS_EXECUTE=1    the same lines after every ExecuteCommandLists (probes without a swapchain).
//   MACRUNNER_DX12_XL_BARRIER_AUDIT=1    bounded boundary evidence, including states/subresources/flags, at Execute;
//                                         retains EARLY_ENCODE. Does not authorize merges or read GPU data.
//   MACRUNNER_DX12_XL_BARRIER_AUDIT_FRAME=<N> only frame N (0 = first submits of a no-present probe); max 4096 lines.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace dxmt::c17 {

inline unsigned
ReadKey(const char *name, unsigned limit) {
  const DWORD error = GetLastError();
  char text[16]{};
  const DWORD n = GetEnvironmentVariableA(name, text, sizeof(text));
  unsigned value = 0;
  bool valid = n > 0 && n < sizeof(text);
  for (DWORD i = 0; valid && i < n; ++i) {
    valid = text[i] >= '0' && text[i] <= '9';
    if (valid) value = value * 10 + unsigned(text[i] - '0');
    if (value > limit) valid = false;
  }
  SetLastError(error);
  return valid ? value : 0;
}
inline unsigned DeferMarkers() { static const unsigned v = ReadKey("MACRUNNER_DX12_DEFER_MARKERS", 2); return v; }
inline bool MergeDependencies() { static const bool v = ReadKey("MACRUNNER_DX12_MERGE_DEPENDENCIES", 1) == 1; return v; }
inline bool ClearSubresource() { static const bool v = ReadKey("MACRUNNER_DX12_CLEAR_SUBRESOURCE", 1) == 1; return v; }
inline bool QueryNoColor() { static const bool v = ReadKey("MACRUNNER_DX12_QUERY_NO_COLOR", 1) == 1; return v; }
// Closed transition-only intervals. 1: descriptor table/SRV/UAV bindings veto an interval.
// 2 (XL-MERGE 22.10): bindings do not veto. Every attachment subresource of the chain is in
// RENDER_TARGET/DEPTH_WRITE for every draw of both passes (the proof requires every cycle to
// start and end there and rejects read-only DSVs), so a valid D3D12 draw cannot read chain
// memory through a descriptor; each recorded pass re-emits its complete state and bindings
// at its first draw (new pass = all state dirty), so the merged encoder needs nothing else.
inline unsigned EmptyTransitionsMode() { static const unsigned v = ReadKey("MACRUNNER_DX12_MERGE_REDUNDANT_TRANSITIONS", 2); return v; }
inline bool EmptyTransitions() { return EmptyTransitionsMode() != 0; }
inline unsigned Period() { static const unsigned v = ReadKey("MACRUNNER_DX12_C17_STATS", 1000000); return v; }
inline bool EveryExecute() { static const bool v = ReadKey("MACRUNNER_DX12_C17_STATS_EXECUTE", 1) == 1; return v; }
inline bool BarrierAudit() { static const bool v = ReadKey("MACRUNNER_DX12_XL_BARRIER_AUDIT", 1) == 1; return v; }
inline unsigned BarrierAuditFrame() { static const unsigned v = ReadKey("MACRUNNER_DX12_XL_BARRIER_AUDIT_FRAME", 1000000); return v; }
inline bool Enabled() { static const bool v = Period() != 0 || EveryExecute() || BarrierAudit() || EmptyTransitions(); return v; }

#define DXMT_C17_REASONS(X)                                                                                            \
  X(empty_candidates) X(empty_approved) X(empty_merged) X(empty_defer_lists)                                          \
  X(empty_reject_binding) X(empty_reject_state) X(empty_reject_effect) X(empty_reject_work)                            \
  X(empty_binding_tolerated) X(empty_effect_write) X(empty_effect_query) X(empty_effect_loadstore)                    \
  X(empty_effect_mesh) X(empty_effect_nodraw) X(empty_effect_indirect) X(empty_effect_readonly)                        \
  X(empty_state_records) X(empty_state_value) X(empty_state_subresource) X(empty_state_open)                           \
  X(xl_e2_approved) X(xl_e2_tolerated) X(xl_e2_binding) X(xl_e2_effect) X(xl_e2_effect_write) X(xl_e2_effect_mesh)     \
  X(xl_e2_effect_query) X(xl_e2_effect_nodraw) X(xl_e2_effect_loadstore) X(xl_e2_effect_indirect)                     \
  X(xl_e2_effect_readonly) X(xl_e2_state) X(xl_e2_state_records) X(xl_e2_state_value) X(xl_e2_state_subresource)      \
  X(xl_e2_state_open)                                                                                                  \
  X(markers_deferred) X(markers_dropped) X(markers_inline)                                                            \
  X(merge_pairs) X(merge_merged) X(merge_merged_relaxed)                                                               \
  X(cross_plain) X(cross_uav_state) X(cross_uav_barrier) X(cross_aliasing)                                             \
  X(reject_barrier_mode1) X(reject_attachment) X(reject_unknown_object) X(reject_records_invalid)                      \
  X(reject_aliasing_unnamed) X(reject_writer) X(reject_pending_clear) X(reject_visibility) X(reject_dead_store)        \
  X(reject_not_plain) X(reject_not_open)                                                                               \
  X(xl_pairs) X(xl_mergeable) X(xl_reject_attachment) X(xl_reject_unknown_object) X(xl_reject_records_invalid)         \
  X(xl_reject_aliasing_unnamed) X(xl_reject_writer) X(xl_reject_not_plain) X(xl_runs) X(xl_run_lists)                  \
  X(xl_relaxed) X(xl_run_early_ticks)                                                                                 \
  X(clear_enc_direct) X(clear_enc_barrier) X(clear_enc_resolve) X(clear_enc_unfoldable) X(clear_enc_order)              \
  X(clear_enc_break) X(clear_enc_scope_end) X(clear_enc_prefix)                                                       \
  X(clear_batched) X(clear_batch_groups) X(clear_cycle_kept) X(clear_cycle_defer_lists) X(blit_run_merged) \
  X(ldc_first_draws) X(ldc_att_load) X(ldc_att_dontcare) X(ldc_att_flagged) X(ldc_merged_tail) X(ldc_probe_nop) X(ldc_depth_dontcare) X(ldc_att_rt_fail) X(ldc_fail_stage)          \
  X(ldc_fail_layout) X(ldc_fail_topo) X(ldc_fail_raster) X(ldc_fail_sample) X(ldc_fail_ds) X(ldc_fail_vs) X(ldc_fail_ps)     \
  X(ldc_fail_bytecode) X(ldc_fail_rt) X(ldc_fail_draw) X(ldc_fail_viewport) X(ldc_fail_scissor) X(ldc_fail_array) X(ldc_fail_size) X(ldc_fail_query) X(ldc_probe_started) X(ldc_probe_pending) X(ldc_proven) X(ldc_disproven) X(ldc_fail_coverage) X(signal_tail_merged) X(signal_tail_late)                              \
  X(noc_twins) X(noc_converted) X(noc_converted_query) X(noc_reject_pipeline) X(noc_reject_no_depth)                   \
  X(noc_reject_resolve)

struct State {
#define DXMT_C17_FIELD(name) std::atomic<uint64_t> name{0};
  DXMT_C17_REASONS(DXMT_C17_FIELD)
#undef DXMT_C17_FIELD
  std::atomic<uint64_t> frames{0};
};
inline State &Get() { static State s; return s; }
inline void Note(std::atomic<uint64_t> &counter, uint64_t n = 1) {
  if (Enabled()) counter.fetch_add(n, std::memory_order_relaxed);
}

inline void Report(const char *why) {
  auto &s = Get();
  const DWORD error = GetLastError();
  const auto frames = (unsigned long long)s.frames.load(std::memory_order_relaxed);
  char line[160];
  DWORD written;
  const int head = std::snprintf(line, sizeof(line), "dx12_c17_reason why=%s frames=%llu name=keys count=m%u.d%u.c%u.n%u.e%u\n", why,
                                 frames, DeferMarkers(), unsigned(MergeDependencies()), unsigned(ClearSubresource()),
                                 unsigned(QueryNoColor()), EmptyTransitionsMode());
  if (head > 0 && size_t(head) < sizeof(line))
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(head), &written, nullptr);
#define DXMT_C17_PRINT(name)                                                                                           \
  if (const auto v = s.name.load(std::memory_order_relaxed)) {                                                         \
    const int n = std::snprintf(line, sizeof(line), "dx12_c17_reason why=%s frames=%llu name=" #name " count=%llu\n",   \
                                why, frames, (unsigned long long)v);                                                   \
    if (n > 0 && size_t(n) < sizeof(line))                                                                             \
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);                                    \
  }
  DXMT_C17_REASONS(DXMT_C17_PRINT)
#undef DXMT_C17_PRINT
  SetLastError(error);
}
inline void Executed() {
  if (EveryExecute()) Report("execute");
}
inline void Presented() {
  if (!Enabled()) return;
  const auto n = Get().frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (Period() && n % Period() == 0) Report("present");
}

} // namespace dxmt::c17
