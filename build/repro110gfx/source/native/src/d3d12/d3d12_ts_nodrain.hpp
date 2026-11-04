#pragma once
// GPU-PASS 25.10.2026: MACRUNNER_DX12_TS_NODRAIN=1 (default 0). EndQuery(TIMESTAMP) is a blit pass that carries the end-of-
// encoder sample. Key 0 wraps that pass in a queue-wide drain BEFORE and AFTER it (shared-event signal + wait on the same
// command buffer); measured in Elden Ring at the church (run G1, c18-pre2+LDC): 2 timestamp passes per frame, 258 us of GPU
// idle each (0.514 ms/frame). Key 1: no drain; the pass keeps waiting the common fence (every encoder before it on this queue
// updates it after its last stage, so the end-of-encoder sample still follows all prior fenced work) and additionally waits
// the deferred-OUT fence when an OUT write is pending (the drain used to cover that too). It still updates the common fence,
// so later work stays ordered after it. MARKER_IN (wait_for_prior_work) passes keep the drain under both keys.
// The fence argument needs fragment stages that complete in order (RENDER_FENCE 0, 1, 3: each render pass waits the common
// fence before its fragment stage at the latest). Diagnostic RENDER_FENCE=2 (no waits) keeps the drain: probe fv0-m2 with
// the key showed t1-t0 = 0.67 ms for 1.47 ms of work in between (the last fence update does not imply the earlier ones).
// Probe ts1 (25.10, fv-probe 16 passes, mode 3, alternated): frame GPU busy 2.05/2.51 -> 1.175/1.178 ms, timestamps
// inverted 0 / non-monotonic 0, t1-t0 1.08-1.12 ms >= 1.03 ms of sampled work between them, pixels byte-equal.
// Read once per process.
// GetEnvironmentVariableA/GetLastError come from the includer (as in d3d12_render_fence.hpp).

namespace dxmt::ts_nodrain {
inline bool
Enabled() {
  static const bool on = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool parsed = GetEnvironmentVariableA("MACRUNNER_DX12_TS_NODRAIN", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return parsed;
  }();
  return on;
}
} // namespace dxmt::ts_nodrain
