// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_frame_trace.hpp"
#include <cstring>

namespace dxmt {

struct CommandRecordingState {
  bool open = false;
  void Begin(bool succeeded) { open = succeeded; }
  void End() { open = false; }
  bool Allows(bool allocator_recording) const { return open && allocator_recording; }
};

// Preserve the first failure independently of later HRESULT overwrites. No
// dynamic allocation, descriptor reads, or resource ownership changes.
struct FirstRecordingFailure {
  HRESULT hr = 0;
  const char *api = nullptr;
  const char *reason = nullptr;
  const char *caller = nullptr;
  uint64_t invocation = 0;
  unsigned line = 0, topology = 0;
  const void *pso = nullptr;
  bool detailed = false;
  char detail[192] = {};

  void Clear() { *this = {}; }
  void Capture(HRESULT value, const char *operation, unsigned source_line, unsigned topo, const void *pipeline, uint64_t call) {
    if (FAILED(hr) || SUCCEEDED(value)) return;
    hr = value; api = operation; line = source_line; topology = topo; pso = pipeline; invocation = call;
  }
  template<typename... Args>
  void Detail(uint64_t call, const char *operation, const char *why, const char *format, Args... args) {
    if (!FAILED(hr) || detailed || !api || call != invocation) return;
    const DWORD saved = GetLastError();
    reason = why; caller = operation;
    const int n = std::snprintf(detail, sizeof(detail), format, args...);
    if (n < 0) detail[0] = 0;
    if (n >= int(sizeof(detail))) {
      constexpr char marker[] = " [truncated]";
      std::memcpy(detail + sizeof(detail) - sizeof(marker), marker, sizeof(marker));
    }
    detailed = true;
    SetLastError(saved);
  }
  void Trace(const void *list) const {
    if (!FAILED(hr)) return;
    static std::atomic<uint32_t> counter{0};
    TraceFrame(counter, "list.first_recording_failure", list,
               "hr=%08x api=%s line=%u caller=%s call=%llu topology=%u pso=%p reason=%s detail=%s",
               unsigned(hr), api ? api : "unknown", line, caller ? caller : "unknown",
               static_cast<unsigned long long>(invocation), topology, pso, reason ? reason : "source_line", detail);
  }
};

} // namespace dxmt
