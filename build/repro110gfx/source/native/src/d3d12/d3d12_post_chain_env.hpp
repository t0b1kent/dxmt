#pragma once
// SPDX-License-Identifier: LGPL-2.1-or-later
// POSTCS 03.11.2026: the environment / hot-key part of MACRUNNER_DX12_POST_CHAIN (d3d12_post_chain.hpp), for files that only
// need to know whether the key is armed.
#include <windows.h>
#include <algorithm>
#include "dxmt_hotkeys.hpp"

namespace dxmt::post_chain {

inline unsigned
ReadNumber(const char *name, unsigned def) {
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
  unsigned key_env = 0, min_run = 8, min_run2 = 12, max_px = 640 * 360, sw_maxw = 640, cache_mb = 64, stats = 0;
  bool armed = false, trace = false;
  bool strict_barriers = false;
  bool hoist_clears = true;
  bool inrun_clears = true;       // 03.11 PCS2: otherwise a clear of a shader-writable 2D colour target runs inside the run (clear kernel)       // 03.11 PCS2: a clear inside a run goes before the run when the run cannot have seen its texture   // 03.11 PCS1: 1 = the first rule (only RT/UAV/shader-state transitions of named resources)
  Env() {
    key_env = ReadNumber("MACRUNNER_DX12_POST_CHAIN", 0);
    // 03.11 v3: 8 (stand, ER ping-pong pyramid, runs of N between 1280x720 render passes, 3 rounds alternating: N=4 +0,6, N=6 +11,
    // N=8 -8,8, N=12 -23,9 us per run; ER series Q 01.11 with v3 kernels, runs >= 8: busy -0,185 ms)
    min_run = std::max(2u, ReadNumber("MACRUNNER_DX12_POST_CHAIN_MIN_RUN", 8));
    min_run2 = std::max(2u, ReadNumber("MACRUNNER_DX12_POST_CHAIN_MIN_RUN2", 12));
    max_px = ReadNumber("MACRUNNER_DX12_POST_CHAIN_MAX_PX", 640 * 360);
    sw_maxw = ReadNumber("MACRUNNER_DX12_POST_CHAIN_SW_MAXW", 640);
    cache_mb = ReadNumber("MACRUNNER_DX12_POST_CHAIN_CACHE_MB", 64);
    trace = ReadNumber("MACRUNNER_DX12_POST_CHAIN_TRACE", 0) == 1;
    strict_barriers = ReadNumber("MACRUNNER_DX12_POST_CHAIN_STRICT_BARRIERS", 0) == 1;
    hoist_clears = ReadNumber("MACRUNNER_DX12_POST_CHAIN_HOIST_CLEARS", 1) == 1;
    inrun_clears = ReadNumber("MACRUNNER_DX12_POST_CHAIN_INRUN_CLEARS", 1) == 1;
    stats = ReadNumber("MACRUNNER_DX12_POST_CHAIN_STATS", 0);
    if (!stats) stats = ReadNumber("MACRUNNER_DX12_LIGHTRN_STATS", 0);
    if (!stats) stats = ReadNumber("MACRUNNER_DX12_GPU4_STATS", 0);
    if (!stats) stats = ReadNumber("MACRUNNER_DX12_FENCE_STATS", 0);
    armed = key_env == 1 || key_env == 2 || key_env == 9 || hotkeys::Enabled();
  }
};
inline Env &E() { static Env e; return e; }
inline bool Armed() { return E().armed; }
// 0 off, 1 / 2 on (MIN_RUN / MIN_RUN2), 9 negative control (environment only)
inline unsigned Key() {
  if (!Armed()) return 0;
  const int v = hotkeys::Value(hotkeys::PostChain);
  return v == 1 ? 1u : v == 2 ? 2u : (v == 9 && E().key_env == 9) ? 9u : 0u;
}
inline unsigned MinRun(unsigned key) { return key == 2 ? E().min_run2 : E().min_run; }

} // namespace dxmt::post_chain
