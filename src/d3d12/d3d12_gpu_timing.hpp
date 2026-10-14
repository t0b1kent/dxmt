// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project; see README-MACRUNNER.md.
#pragma once

#include "d3d12.h"
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace dxmt {

// Diagnostic only: timestamps from already completed command buffers. No new
// GPU waits, encoders, handlers or queries are inserted. CPU and Metal timestamps
// are separate clock domains; only differences within one domain are meaningful.
struct GpuTimingRecord {
  uint64_t ticket = 0, commit_begin = 0, commit_end = 0;
};

class GpuTimingTrace {
  bool enabled_ = false;
  bool heartbeat_enabled_ = false;
  uint32_t limit_ = 4096;
  uint64_t first_qpc_ = 0, frequency_ = 0, delay_ticks_ = 0;
  std::atomic<uint32_t> reserved_{0};
  std::atomic<bool> exhausted_{false};
  std::atomic<uint64_t> completed_presents_{0}, failed_presents_{0};
  std::atomic<uint64_t> last_present_qpc_{0}, next_heartbeat_qpc_{0};
  std::atomic<bool> heartbeat_clock_failed_{false};

  static uint32_t DecimalEnv(const char *name, uint32_t fallback, uint32_t maximum) {
    char value[24] = {};
    const auto length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (!length || length >= sizeof(value)) return fallback;
    uint64_t number = 0;
    for (DWORD i = 0; i < length; ++i) {
      if (value[i] < '0' || value[i] > '9') return fallback;
      number = number * 10 + uint64_t(value[i] - '0');
      if (number > maximum) return fallback;
    }
    return uint32_t(number);
  }

  template<typename... Args> static void Emit(const char *format, Args... args) {
    char line[768];
    const int length = std::snprintf(line, sizeof(line), format, args...);
    DWORD written = 0;
    if (length > 0 && size_t(length) < sizeof(line))
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
    // Full bounded stderr and its collector's dropped_bytes are required evidence.
    // A missing/partial line is not a zero GPU duration.
  }

public:
  GpuTimingTrace() {
    const DWORD error = GetLastError();
    char value[4] = {};
    enabled_ = GetEnvironmentVariableA("MACRUNNER_DX12_GPU_TIMING", value, sizeof(value)) == 1 && value[0] == '1';
    heartbeat_enabled_ = GetEnvironmentVariableA("MACRUNNER_DX12_PRESENT_HEARTBEAT", value, sizeof(value)) == 1 && value[0] == '1';
    if (enabled_ || heartbeat_enabled_) {
      LARGE_INTEGER frequency{}, first{}, last{};
      FILETIME utc;
      QueryPerformanceFrequency(&frequency);
      QueryPerformanceCounter(&first);
      GetSystemTimeAsFileTime(&utc);
      QueryPerformanceCounter(&last);
      frequency_ = uint64_t(frequency.QuadPart);
      first_qpc_ = uint64_t(first.QuadPart);
      if (heartbeat_enabled_) {
        if (frequency.QuadPart <= 0 || first.QuadPart <= 0 || last.QuadPart < first.QuadPart) {
          heartbeat_clock_failed_.store(true, std::memory_order_relaxed);
          Emit("%s", "dx12_present state=FAILED reason=clock_init\n");
        } else {
          next_heartbeat_qpc_.store(first_qpc_, std::memory_order_relaxed);
          Emit("dx12_present state=READY version=1 frequency=%llu qpc=%llu interval_ms=1000 mode=completion_progress\n",
               (unsigned long long)frequency_, (unsigned long long)first_qpc_);
        }
      }
      if (enabled_) {
        limit_ = DecimalEnv("MACRUNNER_DX12_GPU_TIMING_LIMIT", 4096, 65536);
        if (!limit_) limit_ = 4096;
        const uint32_t delay = DecimalEnv("MACRUNNER_DX12_GPU_TIMING_DELAY_MS", 0, 3600000);
        delay_ticks_ = frequency_ * delay / 1000;
        Emit("dx12_gpu state=READY version=1 frequency=%llu qpc=%llu utc_filetime=%llu anchor_span=%llu delay_ms=%u limit=%u\n",
             (unsigned long long)frequency_, (unsigned long long)first_qpc_,
             (unsigned long long)((uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime),
             (unsigned long long)(last.QuadPart - first.QuadPart), delay, limit_);
      }
    }
    SetLastError(error);
  }

  static uint64_t Clock() {
    const DWORD error = GetLastError();
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    SetLastError(error);
    return uint64_t(qpc.QuadPart);
  }

  // Progress telemetry from the existing retirement path, independent of GPU
  // timestamp sampling/delay/cap. The external monitor owns its periodic timer.
  // No completion means no line: silence after READY must not refresh its deadline.
  void Heartbeat(bool present, uint32_t status) {
    if (!heartbeat_enabled_ || heartbeat_clock_failed_.load(std::memory_order_relaxed)) return;
    const DWORD error = GetLastError();
    const uint64_t now = Clock();
    if (now < first_qpc_) {
      if (!heartbeat_clock_failed_.exchange(true, std::memory_order_relaxed))
        Emit("%s", "dx12_present state=FAILED reason=clock_read\n");
      SetLastError(error);
      return;
    }
    if (present) {
      if (status == 4) {
        completed_presents_.fetch_add(1, std::memory_order_relaxed);
        auto prior = last_present_qpc_.load(std::memory_order_relaxed);
        while (prior < now && !last_present_qpc_.compare_exchange_weak(prior, now, std::memory_order_relaxed)) {}
      } else {
        failed_presents_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    auto next = next_heartbeat_qpc_.load(std::memory_order_relaxed);
    if (now >= next && next_heartbeat_qpc_.compare_exchange_strong(next, now + frequency_, std::memory_order_relaxed))
      Emit("dx12_present state=PROGRESS completed=%llu failed=%llu last_completed_qpc=%llu qpc=%llu\n",
           (unsigned long long)completed_presents_.load(std::memory_order_relaxed),
           (unsigned long long)failed_presents_.load(std::memory_order_relaxed),
           (unsigned long long)last_present_qpc_.load(std::memory_order_relaxed),
           (unsigned long long)now);
    SetLastError(error);
  }

  GpuTimingRecord Begin() {
    if (!enabled_ || exhausted_.load(std::memory_order_relaxed)) return {};
    const DWORD error = GetLastError();
    const uint64_t now = Clock();
    GpuTimingRecord record{};
    if (now - first_qpc_ >= delay_ticks_) {
      // Saturate the global reservation count across queues. No wraparound.
      uint32_t slot = reserved_.load(std::memory_order_relaxed);
      while (slot < limit_ && !reserved_.compare_exchange_weak(slot, slot + 1, std::memory_order_relaxed)) {}
      if (slot < limit_) record = {uint64_t(slot) + 1, now, 0};
      else if (!exhausted_.exchange(true, std::memory_order_relaxed))
        Emit("dx12_gpu state=DROPPED reason=record_limit limit=%u further_command_buffers=NOT_RECORDED\n", limit_);
    }
    SetLastError(error);
    return record;
  }

  void Complete(const GpuTimingRecord &record, const void *queue, uint64_t seq,
                bool present, uint32_t status, uint64_t wait_begin, uint64_t wait_end,
                uint64_t gpu_begin, uint64_t gpu_end) {
    const DWORD error = GetLastError();
    // Completed=4; error/incomplete and zero/reversed timestamps remain explicit.
    const char *state = status == 4 && gpu_begin && gpu_end >= gpu_begin ? "PRESENT" : "INVALID";
    Emit("dx12_gpu state=%s ticket=%llu queue=%p seq=%llu present=%u tid=%lu status=%u commit_begin=%llu commit_end=%llu wait_begin=%llu wait_end=%llu gpu_begin_ns=%llu gpu_end_ns=%llu\n",
         state, (unsigned long long)record.ticket, queue, (unsigned long long)seq, unsigned(present),
         (unsigned long)GetCurrentThreadId(), status, (unsigned long long)record.commit_begin,
         (unsigned long long)record.commit_end, (unsigned long long)wait_begin, (unsigned long long)wait_end,
         (unsigned long long)gpu_begin, (unsigned long long)gpu_end);
    SetLastError(error);
  }
};

inline GpuTimingTrace &GpuTiming() {
  static GpuTimingTrace trace;
  return trace;
}

} // namespace dxmt
