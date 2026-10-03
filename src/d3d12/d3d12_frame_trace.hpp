// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include "d3d12.h"
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace dxmt {

inline bool
FrameTraceEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_TRACE", value, sizeof(value)) == 1 &&
                        value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

inline uint32_t
FrameTraceSample(std::atomic<uint32_t> &counter) {
  if (!FrameTraceEnabled())
    return 0;
  // Saturate to prevent wraparound from restarting logging in a long run.
  uint32_t previous = counter.load(std::memory_order_relaxed);
  while (previous < 1048576) {
    if (counter.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) {
      const uint32_t n = previous + 1;
      return n <= 4 || (n & (n - 1)) == 0 ? n : 0;
    }
  }
  return 0;
}

template<typename... Args>
inline void
TraceFrame(std::atomic<uint32_t> &counter, const char *stage, const void *self, const char *format, Args... args) {
  const uint32_t n = FrameTraceSample(counter);
  if (!n)
    return;
  const DWORD error = GetLastError();
  char detail[320];
  char line[640];
  if constexpr (sizeof...(args) == 0)
    std::snprintf(detail, sizeof(detail), "%s", format);
  else
    std::snprintf(detail, sizeof(detail), format, args...);
  LARGE_INTEGER qpc, frequency;
  FILETIME utc;
  QueryPerformanceCounter(&qpc);
  QueryPerformanceFrequency(&frequency);
  GetSystemTimeAsFileTime(&utc);
  const uint64_t utc_ticks = (uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
  const int length = std::snprintf(line, sizeof(line),
      "dx12_frame stage=%s n=%u self=%p tid=%lu qpc=%llu frequency=%llu utc_filetime=%llu %s\n", stage, n, self,
      (unsigned long)GetCurrentThreadId(), (unsigned long long)qpc.QuadPart,
      (unsigned long long)frequency.QuadPart, (unsigned long long)utc_ticks, detail);
  DWORD written;
  if (length > 0 && size_t(length) < sizeof(line))
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
  SetLastError(error);
}

struct FrameTraceResultCounters {
  std::atomic<uint32_t> enter{0};
  std::atomic<uint32_t> result{0};
  std::atomic<uint32_t> failure{0};
};

inline HRESULT
TraceFrameResult(FrameTraceResultCounters &counters, const char *stage, const char *failure_stage,
                 const void *self, HRESULT hr) {
  TraceFrame(counters.result, stage, self, "hr=%08x", unsigned(hr));
  // A rare failure must not be hidden by a successful PSO/Present sample stream.
  if (FAILED(hr))
    TraceFrame(counters.failure, failure_stage, self, "hr=%08x", unsigned(hr));
  return hr;
}

} // namespace dxmt
