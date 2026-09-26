// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include "d3d12_frame_trace.hpp"

namespace dxmt {

enum class CommandFailureOperation : uint32_t {
  AllocatorCreate, AllocatorReset, ListCreate, ListCreate1, ListInitialize, ListClose, ListReset, RecordAfterClose, Count
};

// Failure-only counters are independent of successful creation/frame sampling.
// Keep original HRESULT and LastError; this observer never recovers or skips work.
inline HRESULT
TraceCommandFailure(CommandFailureOperation operation, const char *api, const void *self,
                    HRESULT hr, const char *reason, const void *related = nullptr, uint64_t detail = 0) {
  if (SUCCEEDED(hr) || !FrameTraceEnabled())
    return hr;
  static std::atomic<uint32_t> counters[uint32_t(CommandFailureOperation::Count)]{};
  const auto index = uint32_t(operation);
  if (index >= uint32_t(CommandFailureOperation::Count))
    return hr;
  const DWORD saved_error = GetLastError();
  LARGE_INTEGER tick{};
  QueryPerformanceCounter(&tick);
  TraceFrame(counters[index], api, self,
             "hr=%08x reason=%s related=%p detail=%llu tid=%lu qpc=%llu",
             unsigned(hr), reason, related, static_cast<unsigned long long>(detail),
             static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(tick.QuadPart));
  SetLastError(saved_error);
  return hr;
}

} // namespace dxmt
