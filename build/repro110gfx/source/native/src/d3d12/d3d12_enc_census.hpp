// SPDX-License-Identifier: LGPL-2.1-or-later
// ENC-CENSUS (23.10.2026): census of the Metal encoder boundaries of a frame. Observation only: nothing is merged,
// skipped, reordered or re-encoded because of this file. Default off; with the key 0 the layer encodes and records
// exactly what it did before (the encode-time annotations below are two bytes per recorded pass, written always,
// read only by the census).
//
//   MACRUNNER_DX12_ENC_CENSUS=P          census on; window report lines every P presents (cumulative + per-frame window).
//   MACRUNNER_DX12_ENC_CENSUS_EVERY=K    evaluate only every K-th frame (default 1); annotations are cheap and always on.
//   MACRUNNER_DX12_ENC_CENSUS_DETAIL=F   one row per encoder of frame F (at most 4096 rows), plus its boundary.
//   MACRUNNER_DX12_ENC_CENSUS_READS=1    for every recorded render pass, the texture subresources its draws can read
//                                        (declared SRV/UAV descriptors, read at ExecuteCommandLists on the caller thread
//                                        like MACRUNNER_DX12_XL_HAZARD, whose per-draw binding record it needs:
//                                        set MACRUNNER_DX12_XL_HAZARD=1, e.g. with _EVERY=100000 so it never judges).
//
// For each boundary between two consecutive Metal encoders of one queue (in submission order, across lists, command
// buffers and ExecuteCommandLists of the frame): the kinds of both sides; the attachments of both render sides with
// format, size and the FINAL load / store actions (after clear folding and dead-store dropping); the bytes the left side
// stores, the right side loads, and the "round trip" = subresources stored by the left side and loaded back by the right
// one; why the layer opened a new encoder (its merge / fold decision); and whether a real data dependency exists:
//   dep_srv      the right side's draws can read (declared) a subresource the left side wrote          (needs READS)
//   dep_copy     the right side copies from what the left side wrote (blit source)
//   dep_barrier  a barrier between them transitions / UAV-orders what the left side wrote; the right side's reads are
//                unknown (compute, argument, resolve, render without READS) so it may be the reader
//   dep_war      a barrier between them moves a resource the left side may read into a write state
//   dep_ecl      different ExecuteCommandLists and nothing above: D3D12 orders them implicitly (counted as needed)
//   dep_unknown  the left side's writes are unknown and a UAV-state barrier / UAV barrier / aliasing is in between
//   dep_later    a barrier names what the left side wrote, but the right side provably does not read it (later reader)
//   dep_attach   the only flow is the attachment itself (right side loads what the left side stored): merge removes it
//   dep_none     nothing: under D3D12 rules the right side cannot observe the left side's writes
// Also: clears encoded on their own (why, the barrier that forced them, the next use of the texture) and near-empty
// render-type encoders (0-1 draws: origin, size, bytes; draw histogram of all render-type encoders).
// Lines (stderr, WriteFile): dx12_enc_census (head), dx12_enc_census_extra, dx12_enc_census_draws,
// dx12_enc_census_empty, dx12_enc_census_clear, dx12_enc_census_class, dx12_enc_census_enc (detail).
#pragma once
#include "d3d12_c17_stats.hpp"
#include <cstdint>

namespace dxmt::enc_census {

inline unsigned Period() { static const unsigned v = c17::ReadKey("MACRUNNER_DX12_ENC_CENSUS", 1000000); return v; }
inline bool Enabled() { return Period() != 0; }
inline unsigned Every() {
  static const unsigned v = [] { const unsigned e = c17::ReadKey("MACRUNNER_DX12_ENC_CENSUS_EVERY", 100000); return e ? e : 1u; }();
  return v;
}
inline bool Reads() { static const bool v = Enabled() && c17::ReadKey("MACRUNNER_DX12_ENC_CENSUS_READS", 1) == 1; return v; }
inline unsigned DetailFrame() {
  static const unsigned v = [] {
    char text[16]{};
    const DWORD error = GetLastError();
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_ENC_CENSUS_DETAIL", text, sizeof(text));
    SetLastError(error);
    return n ? c17::ReadKey("MACRUNNER_DX12_ENC_CENSUS_DETAIL", 100000000) : 0xffffffffu;
  }();
  return v;
}

// EncoderData::census_begin
enum Begin : uint8_t { BeginNo = 0, BeginYes = 1, BeginUnset = 0xff };

// EncoderData::census_why: why this recorded pass opened a new encoder (census_begin == 1), or what happened to it.
enum Why : uint8_t {
  WhyUnset = 0,
  // render pass opened a new encoder
  WhyScopeStart,      // first encoder of its Metal command buffer (early-encoded list or start of a run)
  WhyAfterOther,      // previous encoder of the buffer is not a render pass (clear, blit, compute, resolve, service)
  WhyDiffTargets,     // previous encoder is a render pass with other attachments
  WhyNotPlain,        // legacy visibility queries on this pass
  WhyPendingClear,    // a pending clear of this pass's texture could not wait
  WhyVisibility,      // direct-query visibility buffer / slot of the open encoder does not fit
  WhyDeadStore,       // the open encoder drops a store (TILE_MEMORY): nothing may continue it
  WhyBarrierAttach,   // a barrier about an attachment of the chain
  WhyBarrierOther,    // another barrier verdict (unnamed object, invalid records, unnamed aliasing, merge mode 1)
  WhyWriter,          // relaxed barrier crossed and a draw can write memory
  WhyNotOpen,         // same targets, no veto, but the previous encoder was already closed (prev pass not plain)
  // clear encoders (ClearWhy of the encode loop + 1)
  WhyClearDirect, WhyClearBarrier, WhyClearResolve, WhyClearUnfoldable, WhyClearOrder, WhyClearBreak,
  WhyClearScopeEnd, WhyClearPrefix,
  // other encoders
  WhyBlitPlain,       // copy work
  WhyBlitTimestamp,   // timestamp sample / counter resolve
  WhyBlitOutJoin,     // barrier pass joining an immediate OUT write
  WhyBlitImmediate,   // WriteBufferImmediate OUT
  WhyCompute, WhyArgument, WhyAcceleration, WhyResolve,
  // passes that did not open an encoder
  WhyMerged,          // render pass continued the open encoder
  WhyFolded,          // clear became the load action of a later pass
  WhySkippedBarrier,  // barrier-only pass, nothing encoded
  WhyClearBatched,    // ENC-FIX CLEAR_BATCH: clear encoded in the same encoder as the previous clear
  WhyBlitMerged,      // ENC-FIX BLIT_RUN: copy pass encoded into the open blit encoder
  WhyCount
};
inline constexpr const char *kWhyName[WhyCount] = {
    "unset", "scope_start", "after_other", "diff_targets", "not_plain", "pending_clear", "visibility", "dead_store",
    "barrier_attach", "barrier_other", "writer", "not_open", "clear_direct", "clear_barrier", "clear_resolve",
    "clear_unfoldable", "clear_order", "clear_break", "clear_scope_end", "clear_prefix", "blit_plain", "blit_timestamp",
    "blit_out_join", "blit_immediate", "compute", "argument", "acceleration", "resolve", "merged", "folded",
    "skipped_barrier", "clear_batched", "blit_merged"};

// ENC-CENSUS fixes (23.10.2026), both default 0 (with 0 the encode path is exactly the one before):
//   MACRUNNER_DX12_CLEAR_BATCH=1   full clears that are encoded on their own at the same point (one barrier, resolve,
//                                  break or end of the call) and have the same size / layers / samples share ONE render
//                                  encoder: up to 8 colour textures and one depth/stencil texture, each texture once,
//                                  recorded order kept between encoders (clears of different textures commute).
//   MACRUNNER_DX12_CLEAR_CYCLE=1   (with DEFER_CLEARS) a pending clear is not encoded because of a barrier when that
//                                  barrier opens a closed cycle of its texture: the run of barrier-only passes that starts
//                                  with it moves every named subresource out of RENDER_TARGET / DEPTH_WRITE and back,
//                                  with no other pass in between (so nothing can observe the texture); the clear keeps
//                                  waiting and can still fold into the load action of the next pass that uses it.
//                                  With EARLY_ENCODE, a list whose first or last non-barrier pass is a full clear is then
//                                  encoded at ExecuteCommandLists (with its neighbours), so the cycle may span two lists.
inline bool ClearBatch() { static const bool v = c17::ReadKey("MACRUNNER_DX12_CLEAR_BATCH", 1) == 1; return v; }
//   MACRUNNER_DX12_BLIT_RUN=1      consecutive plain copy passes (copies / fills / mip generation, no timestamps, resolves or
//                                  OUT writes) share ONE blit encoder while each new pass neither writes what the open
//                                  encoder read or wrote nor reads what it wrote; barrier passes between them may only be
//                                  plain transitions of named resources (aliasing / UAV / split barriers end the run).
inline bool BlitRun() { static const bool v = c17::ReadKey("MACRUNNER_DX12_BLIT_RUN", 1) == 1; return v; }
//   MACRUNNER_DX12_LOAD_DONTCARE=1 a colour (or depth) attachment that the FIRST draw of a pass overwrites at every pixel
//                                  starts with DontCare instead of Load. Rule (record time, d3d12_pipeline_graphics.cpp
//                                  LdcAnalyze + command list LoadDontCareFirstDraw): viewport 0 and scissor 0 cover the view,
//                                  one slice; non-indexed Draw of 3 (list) / 4 (strip) vertices, one instance; pipeline: VS
//                                  only (no HS/DS/GS/SO), empty input layout, VS without resources and without clip / cull /
//                                  viewport / RT-array outputs, solid fill, no culling, no forced sample count; blend and logic
//                                  op off, write mask and PS SV_Target mask cover the format's channels, no alpha-to-coverage,
//                                  full sample mask, no coverage / stencil-ref output, PS without discard (DXBC opcode; DXIL:
//                                  name dx.op.discard / dx.op.demote in the bitstream), depth test off or ALWAYS, stencil off.
//                                  Coverage is PROVEN once per (pipeline, viewport, start vertex / instance, render area): the
//                                  first such draw counts its samples (visibility counting, its own result slot); DontCare only
//                                  after that count was read back equal to width x height x samples; any other count = Load for
//                                  good. The positions are constant for the key (VS of SV_VertexID / SV_InstanceID only, no
//                                  resources), one triangle cannot cover a sample twice, nothing rejects a sample (no discard,
//                                  depth test off / ALWAYS, stencil off, full sample mask): count = all samples <=> every sample
//                                  written. Draw of 3 vertices (triangle list), 1 instance, all attachments of the render area size.
//   MACRUNNER_DX12_SIGNAL_TAIL=1   asynchronous submission: a Signal enqueued right after an ExecuteCommandLists that the submission
//                                  thread has not committed yet is encoded into that call's LAST command buffer (event signal +
//                                  fence retirement of that buffer) instead of a command buffer of its own; one Signal per call.
inline bool SignalTail() { static const bool v = c17::ReadKey("MACRUNNER_DX12_SIGNAL_TAIL", 1) == 1; return v; }
inline bool LoadDontCare() { static const bool v = c17::ReadKey("MACRUNNER_DX12_LOAD_DONTCARE", 1) == 1; return v; }
inline bool ClearCycle() { static const bool v = c17::ReadKey("MACRUNNER_DX12_CLEAR_CYCLE", 1) == 1; return v; }

} // namespace dxmt::enc_census
