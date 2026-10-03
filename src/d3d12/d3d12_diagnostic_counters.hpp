// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project; see README-MACRUNNER.md.
// Optional process-wide measurements for bounded synthetic D3D12 probes.
#pragma once

#include "d3d12.h"
#include "thread.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace dxmt::diagnostic {

inline bool Enabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool active = GetEnvironmentVariableA("MACRUNNER_DX12_COUNTERS", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return active;
  }();
  return enabled;
}

inline uint64_t Tick() {
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return uint64_t(value.QuadPart);
}

enum class LockPath : unsigned { RegisterVA, UnregisterVA, RegisterSource, UnregisterSource, Identity, CaptureVA, BufferVA, Count };
enum class Counter : unsigned { RootUploadCalls, RootUploadBytes, StaticSamplerBytes, RootStateZeroCalls, RootStateZeroBytes,
                               CompileCS, CompileVS, CompileHS, CompileDS, CompileGS, CompilePS,
                               ResidencyRetain, ResidencyRelease, ResidencyAdd, ResidencyRemove, ResidencyCommit, Count };
struct LockCounters {
  std::atomic<uint64_t> calls{0}, contended{0}, wait_ticks{0}, held_ticks{0};
};
struct Counters {
  uint64_t start = Tick();
  std::array<LockCounters, unsigned(LockPath::Count)> locks;
  std::array<std::atomic<uint64_t>, unsigned(Counter::Count)> values{};
};
inline Counters &Data() { static Counters value; return value; }
inline void Add(Counter counter, uint64_t value = 1) {
  if (Enabled()) Data().values[unsigned(counter)].fetch_add(value, std::memory_order_relaxed);
}

// With diagnostics disabled this has the original unique_lock semantics.
// A failed try_lock is the explicit contention observation; elapsed acquisition
// and hold ticks include instrumentation and are not production timings.
class ResidencyLock {
  std::unique_lock<dxmt::mutex> lock_;
  LockCounters *counter_ = nullptr;
  uint64_t acquired_ = 0;
public:
  ResidencyLock(dxmt::mutex &mutex, LockPath path) : lock_(mutex, std::defer_lock) {
    if (!Enabled()) { lock_.lock(); return; }
    counter_ = &Data().locks[unsigned(path)];
    counter_->calls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t start = Tick();
    if (!lock_.try_lock()) {
      counter_->contended.fetch_add(1, std::memory_order_relaxed);
      lock_.lock();
    }
    acquired_ = Tick();
    counter_->wait_ticks.fetch_add(acquired_ - start, std::memory_order_relaxed);
  }
  ~ResidencyLock() {
    if (counter_) counter_->held_ticks.fetch_add(Tick() - acquired_, std::memory_order_relaxed);
  }
};

inline void Dump(const void *device) {
  if (!Enabled()) return;
  const DWORD error = GetLastError();
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  auto &data = Data();
  char line[512];
  auto write = [&](int length) {
    DWORD written;
    if (length > 0 && size_t(length) < sizeof(line))
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
  };
  write(std::snprintf(line, sizeof(line), "dx12_counters scope=process device=%p start_qpc=%llu end_qpc=%llu frequency=%llu\n",
      device, (unsigned long long)data.start, (unsigned long long)Tick(), (unsigned long long)frequency.QuadPart));
  constexpr const char *paths[] = {"register_va", "unregister_va", "register_source", "unregister_source", "identity", "capture_va", "buffer_va"};
  for (unsigned i = 0; i < unsigned(LockPath::Count); ++i) {
    auto &c = data.locks[i];
    write(std::snprintf(line, sizeof(line), "dx12_counters lock=%s calls=%llu contended=%llu wait_ticks=%llu held_ticks=%llu\n",
        paths[i], (unsigned long long)c.calls.load(), (unsigned long long)c.contended.load(),
        (unsigned long long)c.wait_ticks.load(), (unsigned long long)c.held_ticks.load()));
  }
  constexpr const char *names[] = {"root_upload_calls", "root_upload_bytes", "static_sampler_bytes", "root_state_zero_calls",
      "root_state_zero_bytes", "compile_cs", "compile_vs", "compile_hs", "compile_ds", "compile_gs", "compile_ps",
      "residency_retain", "residency_release", "residency_add", "residency_remove", "residency_commit"};
  for (unsigned i = 0; i < unsigned(Counter::Count); ++i)
    write(std::snprintf(line, sizeof(line), "dx12_counters counter=%s value=%llu\n", names[i],
        (unsigned long long)data.values[i].load()));
  SetLastError(error);
}

} // namespace dxmt::diagnostic
