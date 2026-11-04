// SPDX-License-Identifier: LGPL-2.1-or-later
// MacRunner 2026 (ENCODERS): why each Metal encoder exists, and occlusion queries without pass splits.
// Diagnostics print to stderr (WriteFile on STD_ERROR_HANDLE), never through Wine debug channels.
//
//   MACRUNNER_DX12_ENCODER_STATS=<N>        every N presents: one line "dx12_encoder_stats" with cumulative counts of
//                                           Metal encoders by (recorded pass type, reason the pass ended).
//   MACRUNNER_DX12_ENCODER_STATS_EXECUTE=1  also print after every ExecuteCommandLists (probes without a swapchain).
//   MACRUNNER_DX12_ENCODER_SEQ_FRAME=<F>    one line "dx12_encoder_seq" with the encoder sequence of frame F
//                                           (presents counted from 1; F=0 = before the first present).
//   MACRUNNER_DX12_QUERY_INPASS=1           occlusion queries count straight into the query heap inside the open
//                                           render pass (no split at Begin/End, no scratch blit, no reduction
//                                           compute); occlusion resolves are deferred to the next pass boundary.
//   MACRUNNER_DX12_DEFER_COPIES=1           step 2: inside an open render pass, copies / plain immediate writes / barriers
//                                           among copy states do not end it; the copy work is packed at the pass end.
//   MACRUNNER_DX12_DEFER_CLEARS=1           step 4 (implies FOLD_CLEARS): a full clear waits for the first pass that uses
//                                           its texture (folds there), across passes that do not use it.
//   MACRUNNER_DX12_COMPUTE_CHAIN=1          step 5: UAV / shader-state barriers between dispatches become a memory barrier
//                                           inside the open compute encoder.
//   MACRUNNER_DX12_BLIT_CHAIN=1             step 6: the copy-work deferral of step 2 also outside render passes.
//   MACRUNNER_DX12_MERGE_SAFE=1             step 7 (implies MERGE_PASSES>=1): passes with the same targets merge across
//                                           barriers that touch none of their attachments and no UAV / stream-out state.
//   MACRUNNER_DX12_EARLY_ENCODE=1           level 4.1: a command list is encoded into its own Metal command buffer at Close()
//                                           on the recording thread; ExecuteCommandLists only commits in order (fallback:
//                                           unknown / several queues, executed again, queue state needed, diagnostics on).
//   MACRUNNER_DX12_TILE_MEMORY=1            level 4.2: DiscardResource -> DontCare load of the next pass; a store that the
//                                           next event (in the same submission) fully clears is DontCare.
//   MACRUNNER_DX12_DIFF_STATE=1             level 4.3: within a recorded render pass, a state equal to the one already
//                                           emitted is not emitted again.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace dxmt::encoder_stats {

// Reason a recorded pass ended (stored in EncoderData::close_reason). Letters are used by the sequence dump.
enum Reason : uint8_t {
  ReasonUnknown = 0,
  ReasonSwitchRender,      // r: a draw needed a render pass
  ReasonSwitchCompute,     // k: a dispatch needed a compute pass
  ReasonSwitchBlit,        // b: a copy (or other blit work) needed a blit pass
  ReasonBarrier,           // B: ResourceBarrier
  ReasonRenderTargets,     // O: OMSetRenderTargets
  ReasonClear,             // C: ClearRenderTargetView / ClearDepthStencilView (full)
  ReasonClearHelper,       // U: ClearUnorderedAccessView* / rect clears
  ReasonBeginQuery,        // Q: BeginQuery (occlusion)
  ReasonEndQuery,          // q: EndQuery (occlusion)
  ReasonTimestamp,         // T: timestamp EndQuery / ResolveQueryData
  ReasonResolveQuery,      // Y: ResolveQueryData (occlusion / binary)
  ReasonResolveSubresource,// S: ResolveSubresource(Region)
  ReasonIndirect,          // X: ExecuteIndirect
  ReasonWriteImmediate,    // W: WriteBufferImmediate
  ReasonArgument,          // A: argument / acceleration dispatch
  ReasonClearState,        // Z: ClearState / Reset
  ReasonClose,             // E: end of the command list (Close)
  ReasonFlush,             // F: deferred query resolves flushed
  ReasonOther,             // M: anything else
  ReasonCount
};
inline constexpr char kReasonLetter[ReasonCount + 1] = "?rkbBOCUQqTYSXWAZEFM";
inline constexpr const char *kReasonName[ReasonCount] = {
    "unknown", "switch_render", "switch_compute", "switch_blit", "barrier", "render_targets", "clear", "clear_helper",
    "begin_query", "end_query", "timestamp", "resolve_query", "resolve_subresource", "indirect", "write_immediate",
    "argument", "clear_state", "close", "flush", "other"};

// Encoder kinds counted at encode time.
enum Kind : uint8_t {
  KindClear = 0, KindRender, KindBlit, KindCompute, KindResolve, KindArgumentCompute, KindAcceleration,
  KindServiceOcclusionBlit, KindServiceOcclusionCompute, KindServiceOutJoin, KindServicePresent, KindServiceReadback,
  KindCount
};
inline constexpr char kKindLetter[KindCount + 1] = "CRBKSAHovjpd";
inline constexpr const char *kKindName[KindCount] = {
    "clear", "render", "blit", "compute", "resolve", "argument_compute", "acceleration",
    "svc_occlusion_blit", "svc_occlusion_compute", "svc_out_join", "svc_present", "svc_readback"};

inline unsigned
ReadUnsigned(const char *name, unsigned fallback, unsigned limit) {
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
  return valid ? value : fallback;
}
inline unsigned Period() { static const unsigned v = ReadUnsigned("MACRUNNER_DX12_ENCODER_STATS", 0, 1000000); return v; }
inline bool EveryExecute() { static const bool v = ReadUnsigned("MACRUNNER_DX12_ENCODER_STATS_EXECUTE", 0, 1) == 1; return v; }
inline int64_t SeqFrame() {
  static const int64_t v = [] {
    const unsigned x = ReadUnsigned("MACRUNNER_DX12_ENCODER_SEQ_FRAME", 0xffffffffu, 100000000u);
    return x == 0xffffffffu ? int64_t(-1) : int64_t(x);
  }();
  return v;
}
inline bool QueryInPass() { static const bool v = ReadUnsigned("MACRUNNER_DX12_QUERY_INPASS", 0, 1) == 1; return v; }
inline bool DeferCopies() { static const bool v = ReadUnsigned("MACRUNNER_DX12_DEFER_COPIES", 0, 1) == 1; return v; }
inline bool DeferClears() { static const bool v = ReadUnsigned("MACRUNNER_DX12_DEFER_CLEARS", 0, 1) == 1; return v; }
inline bool ComputeChain() { static const bool v = ReadUnsigned("MACRUNNER_DX12_COMPUTE_CHAIN", 0, 1) == 1; return v; }
inline bool BlitChain() { static const bool v = ReadUnsigned("MACRUNNER_DX12_BLIT_CHAIN", 0, 1) == 1; return v; }
inline bool MergeSafe() { static const bool v = ReadUnsigned("MACRUNNER_DX12_MERGE_SAFE", 0, 1) == 1; return v; }
inline bool EarlyEncode() { static const bool v = ReadUnsigned("MACRUNNER_DX12_EARLY_ENCODE", 0, 1) == 1; return v; }
inline bool TileMemory() { static const bool v = ReadUnsigned("MACRUNNER_DX12_TILE_MEMORY", 0, 1) == 1; return v; }
inline bool DiffState() { static const bool v = ReadUnsigned("MACRUNNER_DX12_DIFF_STATE", 0, 1) == 1; return v; }
inline bool Enabled() { static const bool v = Period() != 0 || EveryExecute() || SeqFrame() >= 0; return v; }
// Barrier objects are recorded for the encode-time decisions of steps 4 and 7 and for the load/store audit.
inline bool BarrierRecords() { static const bool v = MergeSafe() || DeferClears() || TileMemory() || Enabled(); return v; }

struct State {
  std::atomic<uint64_t> counts[KindCount][ReasonCount]{};
  std::atomic<uint64_t> merged_render{0}, skipped_barrier_blits{0}, executes{0}, frames{0};
  std::atomic<uint64_t> direct_queries{0}, legacy_queries{0}, deferred_resolves{0}, resolve_flushes{0};
  // steps 2-7
  std::atomic<uint64_t> deferred_blits{0}, deferred_phase_splits{0}, absorbed_barriers{0}, compute_chain_barriers{0};
  std::atomic<uint64_t> merged_safe{0}, clears_carried{0}, clears_folded_deferred{0}, discard_calls{0};
  // level-4 audit: submit-thread wall time, attachment traffic, render commands
  std::atomic<uint64_t> execute_ticks{0}, present_ticks{0}, qpc_hz{0};
  std::atomic<uint64_t> att_load{0}, att_clear{0}, att_dontcare{0}, att_store{0}, att_store_dead{0};
  std::atomic<uint64_t> draws{0}, state_cmds{0}, state_redundant{0}, other_render_cmds{0};
  // level 4.1-4.3
  std::atomic<uint64_t> early_encoded{0}, early_committed{0}, early_ineligible{0}, early_limit{0}, early_fallback_queue{0};
  std::atomic<uint64_t> early_fallback_out_pending{0}, early_fallback_reexecute{0}, early_fallback_lists{0};
  std::atomic<uint64_t> early_ticks{0}, execute_total_ticks{0}, att_load_avoided{0}, att_store_avoided{0}, state_skipped{0};
  std::mutex seq_lock;
  std::string seq;
  bool seq_done = false;
};
inline State &Get() { static State state; return state; }

inline bool SeqActive() {
  const auto f = SeqFrame();
  if (f < 0) return false;
  auto &s = Get();
  return !s.seq_done && s.frames.load(std::memory_order_relaxed) == uint64_t(f);
}
inline void SeqAppend(const char *text) {
  if (!SeqActive()) return;
  auto &s = Get();
  std::lock_guard<std::mutex> lock(s.seq_lock);
  if (s.seq.size() < 60000) s.seq += text;
}
inline void Count(Kind kind, uint8_t reason) {
  if (!Enabled()) return;
  if (reason >= ReasonCount) reason = ReasonOther;
  Get().counts[kind][reason].fetch_add(1, std::memory_order_relaxed);
  if (SeqActive()) { const char token[3] = {kKindLetter[kind], kReasonLetter[reason], 0}; SeqAppend(token); }
}
inline void Note(std::atomic<uint64_t> &counter, uint64_t n = 1) {
  if (Enabled()) counter.fetch_add(n, std::memory_order_relaxed);
}

inline void Write(const char *text, size_t length) {
  DWORD written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, DWORD(length), &written, nullptr);
}
inline void Report(const char *why) {
  auto &s = Get();
  const DWORD error = GetLastError();
  std::string line = "dx12_encoder_stats why=";
  line += why;
  char head[256];
  std::snprintf(head, sizeof(head), " frames=%llu executes=%llu query_inpass=%u merged_render=%llu skipped_barrier_blits=%llu "
                "direct_queries=%llu legacy_queries=%llu deferred_resolves=%llu resolve_flushes=%llu",
                (unsigned long long)s.frames.load(), (unsigned long long)s.executes.load(), unsigned(QueryInPass()),
                (unsigned long long)s.merged_render.load(), (unsigned long long)s.skipped_barrier_blits.load(),
                (unsigned long long)s.direct_queries.load(), (unsigned long long)s.legacy_queries.load(),
                (unsigned long long)s.deferred_resolves.load(), (unsigned long long)s.resolve_flushes.load());
  line += head;
  char more[1024];
  const auto get = [](const std::atomic<uint64_t> &v) { return (unsigned long long)v.load(std::memory_order_relaxed); };
  std::snprintf(more, sizeof(more), " keys=q%u.c%u.d%u.k%u.b%u.m%u deferred_blits=%llu deferred_phase_splits=%llu "
                "absorbed_barriers=%llu compute_chain_barriers=%llu merged_safe=%llu clears_carried=%llu clears_folded_deferred=%llu "
                "discard_calls=%llu execute_ticks=%llu present_ticks=%llu qpc_hz=%llu att_load=%llu att_clear=%llu att_dontcare=%llu "
                "att_store=%llu att_store_dead=%llu draws=%llu state_cmds=%llu state_redundant=%llu other_render_cmds=%llu",
                unsigned(QueryInPass()), unsigned(DeferCopies()), unsigned(DeferClears()), unsigned(ComputeChain()),
                unsigned(BlitChain()), unsigned(MergeSafe()), get(s.deferred_blits), get(s.deferred_phase_splits),
                get(s.absorbed_barriers), get(s.compute_chain_barriers), get(s.merged_safe), get(s.clears_carried),
                get(s.clears_folded_deferred), get(s.discard_calls), get(s.execute_ticks), get(s.present_ticks), get(s.qpc_hz),
                get(s.att_load), get(s.att_clear), get(s.att_dontcare), get(s.att_store), get(s.att_store_dead), get(s.draws),
                get(s.state_cmds), get(s.state_redundant), get(s.other_render_cmds));
  line += more;
  char l4[768];
  std::snprintf(l4, sizeof(l4), " l4keys=e%u.t%u.s%u early_encoded=%llu early_committed=%llu early_ineligible=%llu early_limit=%llu "
                "early_fallback_queue=%llu early_fallback_out_pending=%llu early_fallback_reexecute=%llu early_fallback_lists=%llu "
                "early_ticks=%llu execute_total_ticks=%llu att_load_avoided=%llu att_store_avoided=%llu state_skipped=%llu",
                unsigned(EarlyEncode()), unsigned(TileMemory()), unsigned(DiffState()), get(s.early_encoded), get(s.early_committed),
                get(s.early_ineligible), get(s.early_limit), get(s.early_fallback_queue), get(s.early_fallback_out_pending),
                get(s.early_fallback_reexecute), get(s.early_fallback_lists), get(s.early_ticks), get(s.execute_total_ticks),
                get(s.att_load_avoided), get(s.att_store_avoided), get(s.state_skipped));
  line += l4;
  uint64_t total = 0;
  for (unsigned k = 0; k < KindCount; ++k)
    for (unsigned r = 0; r < ReasonCount; ++r) {
      const auto v = s.counts[k][r].load(std::memory_order_relaxed);
      if (!v) continue;
      total += v;
      char cell[96];
      std::snprintf(cell, sizeof(cell), " %s.%s=%llu", kKindName[k], kReasonName[r], (unsigned long long)v);
      line += cell;
    }
  char tail[64];
  std::snprintf(tail, sizeof(tail), " encoders_total=%llu\n", (unsigned long long)total);
  line += tail;
  Write(line.data(), line.size());
  SetLastError(error);
}
inline void FlushSeq(const char *why) {
  auto &s = Get();
  std::string text;
  {
    std::lock_guard<std::mutex> lock(s.seq_lock);
    if (s.seq_done || s.seq.empty()) return;
    text.swap(s.seq);
    // a frame's sequence is printed once; per-execute dumps (probes, no presents) keep collecting
    if (std::strcmp(why, "present") == 0) s.seq_done = true;
  }
  const DWORD error = GetLastError();
  char head[128];
  std::snprintf(head, sizeof(head), "dx12_encoder_seq why=%s frame=%llu tokens=", why, (unsigned long long)SeqFrame());
  std::string line = head;
  line += text;
  line += "\n";
  Write(line.data(), line.size());
  SetLastError(error);
}
// One ExecuteCommandLists finished encoding.
inline void Executed() {
  if (!Enabled()) return;
  Get().executes.fetch_add(1, std::memory_order_relaxed);
  if (EveryExecute()) { Report("execute"); FlushSeq("execute"); }
}
// One Present encoded: the frame counter moves.
inline void Presented() {
  if (!Enabled()) return;
  auto &s = Get();
  const auto before = s.frames.load(std::memory_order_relaxed);
  if (SeqFrame() >= 0 && before == uint64_t(SeqFrame())) FlushSeq("present");
  const auto n = s.frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (Period() && (n == 1 || n % Period() == 0)) Report("present");
}

} // namespace dxmt::encoder_stats
