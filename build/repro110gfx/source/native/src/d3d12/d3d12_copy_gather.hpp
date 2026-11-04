// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
//
// GAPS 31.10.2026: MACRUNNER_DX12_COPY_GATHER=1 (default 0). With DEFER_COPIES / BLIT_CHAIN, floating copy work is emitted as
// blit passes at the first barrier that leaves the copy states - in Elden Ring that is between two render passes, 13 times a
// frame at the church, and every such blit costs a render -> blit -> render round trip (measured: F->F 36-95 us across one
// blit instead of 2.5-5 us; native stand: ~39 us per blit inside a render chain, ~30 us even without any fence).
// With the key, at Close() the deferred copy passes of the list are moved up to the FIRST deferred copy pass of the same
// list (they then sit next to it, and BLIT_RUN can encode them into its encoder). A copy pass B moves up past the passes
// between that point and B only when, under D3D12's state rules, none of them can observe or produce what B touches:
//   - B holds only copy work recorded through the deferral (its objects = the AccessRange objects of its items, the same
//     ResourceObject handles the barrier records use), no occlusion resolve, no immediate / OUT write, no marker;
//   - no barrier record between them names an object of B (transition / UAV object, both sides of aliasing), there is no
//     barrier without valid records, no aliasing barrier, no UAV barrier on a null resource;
//   - no copy pass that stays between them touches an object of B (copy-to-copy order, also through absorbed copy-state
//     barriers), and no other blit pass (timestamps, resolves, OUT writes, non-deferred copies) and no pass kind other than
//     render / clear / compute lies between them;
//   - the join pass that a flush puts before its copies (absorbed copy-state barriers, waits for a pending OUT write) moves
//     with them and keeps its order; no OUT write lies between the gather point and them.
// Why render / clear / compute passes in between cannot conflict: during that interval B's destination is in COPY_DEST and
// its sources in COPY_SOURCE (or a read state) - a shader cannot write a resource in a copy state, and cannot read B's
// destination - unless a barrier changes the state, and every such barrier names the object (checked above). Implicit
// promotion from COMMON (buffers) to a write state cannot follow a promotion to another state without an explicit barrier.
// MACRUNNER_DX12_PRERASTER_PRECISE_TRACE=1 also prints the gather counters.
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dxmt::copy_gather {

inline bool
Enabled() {
  static const bool on = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool v = GetEnvironmentVariableA("MACRUNNER_DX12_COPY_GATHER", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return v;
  }();
  return on;
}

// Every buffer a WriteBufferImmediate ever targeted (process-wide, also pending OUT writes of earlier lists): a copy that
// touches one of them never moves (an OUT write is ordered by its own fence, joined only at barriers / flush joins).
inline std::mutex &ImmediateMutex() { static std::mutex m; return m; }
inline std::vector<uint64_t> &ImmediateTargets() { static std::vector<uint64_t> v; return v; }
inline void
NoteImmediateTarget(uint64_t object) {
  if (!Enabled() || !object) return;
  std::lock_guard<std::mutex> lock(ImmediateMutex());
  auto &v = ImmediateTargets();
  if (std::find(v.begin(), v.end(), object) == v.end()) v.push_back(object);
}
inline bool
TouchesImmediateTarget(const std::vector<uint64_t> &objects) {
  std::lock_guard<std::mutex> lock(ImmediateMutex());
  const auto &v = ImmediateTargets();
  for (auto o : objects) if (std::find(v.begin(), v.end(), o) != v.end()) return true;
  return false;
}

struct Stats {
  std::atomic<uint64_t> lists{}, moved{}, kept_conflict{}, kept_stop{}, kept_out{};
};
inline Stats &
Get() {
  static Stats s;
  return s;
}

} // namespace dxmt::copy_gather
