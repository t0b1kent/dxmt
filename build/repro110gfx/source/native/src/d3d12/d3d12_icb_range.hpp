#pragma once
// GPU-PASS 24.10.2026: MACRUNNER_DX12_ICB_RANGE=1 (default 0). The ExecuteIndirect resolver runs ONE THREAD PER SLOT (a vertex
// per slot in the render path, a grid position per slot in the compute path) instead of one thread looping over all
// MaxCommandCount slots (reset + encode each). Execution keeps the CPU range [0, MaxCommandCount). Key 0: exactly as before.
// (First version used a GPU-written execution range; probe icb1 showed the execution did not see the range written in the
// same encoder — wrong pixels — so that path is not used.) Read once per process.
#include <cstdint>
// GetEnvironmentVariableA/GetLastError come from the includer (as in d3d12_render_fence.hpp).

namespace dxmt::icb_range {
// executeCommandsInBuffer:indirectBuffer: — larger ICBs keep the CPU range (conservative bound for the indirect range length).
constexpr uint64_t kMaxIndirectRange = 16384;
inline bool
Enabled() {
  static const bool on = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool parsed = GetEnvironmentVariableA("MACRUNNER_DX12_ICB_RANGE", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return parsed;
  }();
  return on;
}
} // namespace dxmt::icb_range
