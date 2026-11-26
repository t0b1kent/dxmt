#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace dxmt::fence_publication {

inline bool Enabled() {
  static const bool enabled = [] {
    const DWORD saved = GetLastError();
    char value[4] = {};
    const bool result = GetEnvironmentVariableA("MACRUNNER_DX12_FENCE_PUBLICATION", value, sizeof(value)) == 1 &&
                        value[0] == '1';
    if (result) {
      static const char marker[] = "dx12_fence_publication enabled=1 source=ordered_retirement\n";
      DWORD written;
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), marker, sizeof(marker) - 1, &written, nullptr);
    }
    SetLastError(saved);
    return result;
  }();
  return enabled;
}

struct Counters {
  std::atomic<uint64_t> cpu_signals{0}, queue_signals{0}, idle_signals{0};
  std::atomic<uint64_t> registrations{0}, events_published{0};
  // These measure D3D12's own null-handle / retirement waits, NOT application
  // WaitForSingleObject calls, which execute outside this DLL.
  std::atomic<uint64_t> wait_calls{0}, wait_pending{0}, wait_ticks{0};
};
inline Counters &Stats() { static Counters counters; return counters; }
inline uint64_t Frequency() {
  static const uint64_t frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return uint64_t(value.QuadPart);
  }();
  return frequency;
}
inline DWORD Wait(HANDLE event) {
  auto &stats = Stats();
  stats.wait_calls.fetch_add(1, std::memory_order_relaxed);
  DWORD result = WaitForSingleObject(event, 0);
  if (result != WAIT_TIMEOUT) return result;
  stats.wait_pending.fetch_add(1, std::memory_order_relaxed);
  LARGE_INTEGER begin{}, end{};
  QueryPerformanceCounter(&begin);
  result = WaitForSingleObject(event, INFINITE);
  const DWORD error = GetLastError();
  QueryPerformanceCounter(&end);
  stats.wait_ticks.fetch_add(uint64_t(end.QuadPart - begin.QuadPart), std::memory_order_relaxed);
  SetLastError(error);
  return result;
}

} // namespace dxmt::fence_publication
