// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project. Diagnostic only, default off.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace dxmt::pipeline_timing {

// Pointer identities require lifecycle records; a fence value is NOT a frame number.
enum Kind : uint32_t {
  Swapchain = 1, LatencySet, LatencyHandle, Present, QueuePresent,
  Execute, ExecuteList, Commit, Retire, SemaphoreRelease,
  QueueSignal, QueueWait, FenceCreate, FenceDestroy, FenceSignal,
  FenceCompleted, FenceCompletion, FenceBranch, ListReset, ListClose,
  ListDestroy, QueueCapacity, QueueIdentity, GpuTimes, NativeFence, AllocatorReset,
};
enum Phase : uint32_t { Point = 0, Enter = 1, Leave = 2 };
struct Header {
  char magic[8];
  uint32_t version, record_size, capacity, pid;
  uint64_t frequency, anchor_qpc, anchor_filetime, anchor_span;
  uint64_t window_begin, window_end;
  uint32_t count, overflow, dropped, clock_failed;
  uint64_t reserved[5];
};
struct Record {
  uint64_t qpc, object, id, a, b, c, d;
  uint32_t thread, kind, phase, status, committed, padding;
};
static_assert(sizeof(Header) == 128 && sizeof(Record) == 80);
struct PreserveError {
  DWORD value = GetLastError();
  ~PreserveError() { SetLastError(value); }
};
inline uint64_t Pointer(const void *p) { return reinterpret_cast<uintptr_t>(p); }

// DIAGNOSTIC_ONLY: two or three initial credits, one replenishment per retired
// Present. Never add an early release as well: that would create extra credits.
// The selected depth is observable through GetMaximumFrameLatency and the trace.
inline uint32_t LatencyOverride() {
  static const uint32_t depth = [] {
    PreserveError error;
    char value[4]{};
    const auto n = GetEnvironmentVariableA("MACRUNNER_DX12_LATENCY_DEPTH", value, sizeof(value));
    return n == 1 && (value[0] == '2' || value[0] == '3') ? uint32_t(value[0] - '0') : 0u;
  }();
  return depth;
}

// Same append/commit mapped-file transport as MRPT001. No per-event output,
// object inspection, callback, new thread, or GPU wait. Allocation is bounded.
class Recorder {
  static constexpr uint32_t capacity = 131072;
  HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
  Header *header_ = nullptr;
  WCHAR arm_path_[4096]{};
  uint64_t delay_ = 0, duration_ = 0;
  std::atomic<uint64_t> begin_{0}, poll_after_{0};

  static uint32_t Decimal(const char *name, uint32_t fallback, uint32_t maximum) {
    char value[32]{};
    DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    if (!n || n >= sizeof(value)) return fallback;
    uint64_t result = 0;
    for (DWORD i = 0; i < n; ++i) {
      if (value[i] < '0' || value[i] > '9') return fallback;
      result = result * 10 + value[i] - '0';
      if (result > maximum) return fallback;
    }
    return uint32_t(result);
  }
  static void Increment(uint32_t &value) {
    std::atomic_ref<uint32_t> v(value);
    auto old = v.load(std::memory_order_relaxed);
    while (old != UINT32_MAX && !v.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) {}
  }
  uint64_t Clock() {
    LARGE_INTEGER q{};
    if (QueryPerformanceCounter(&q) && q.QuadPart > 0) return uint64_t(q.QuadPart);
    Increment(header_->clock_failed);
    return 0;
  }
  bool Active(uint64_t now) {
    uint64_t start = begin_.load(std::memory_order_acquire);
    if (!start && arm_path_[0]) {
      auto next = poll_after_.load(std::memory_order_relaxed);
      if (now >= next && poll_after_.compare_exchange_strong(next, now + header_->frequency / 4)) {
        const DWORD attrs = GetFileAttributesW(arm_path_);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
          start = now + delay_;
          header_->window_begin = start;
          header_->window_end = start + duration_;
          begin_.store(start, std::memory_order_release);
        }
      }
    }
    return start && now >= start && now - start < duration_;
  }

public:
  Recorder() {
    PreserveError guard;
    WCHAR path[4096]{};
    DWORD n = GetEnvironmentVariableW(L"MACRUNNER_DX12_PIPELINE_PATH", path, 4096);
    if (!n || n >= 4096) return;
    LARGE_INTEGER freq{}, before{}, after{};
    FILETIME wall{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 || freq.QuadPart > 1000000000) return;
    if (!QueryPerformanceCounter(&before) || before.QuadPart <= 0) return;
    GetSystemTimeAsFileTime(&wall);
    if (!QueryPerformanceCounter(&after) || after.QuadPart < before.QuadPart) return;
    file_ = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return;
    constexpr DWORD bytes = sizeof(Header) + capacity * sizeof(Record);
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, bytes, nullptr);
    if (!mapping_) return;
    header_ = static_cast<Header *>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, bytes));
    if (!header_) return;
    Header h{};
    std::memcpy(h.magic, "MRPL001", 8);
    h.version = 1; h.record_size = sizeof(Record); h.capacity = capacity;
    h.pid = GetCurrentProcessId(); h.frequency = uint64_t(freq.QuadPart);
    h.anchor_qpc = uint64_t(before.QuadPart + (after.QuadPart - before.QuadPart) / 2);
    h.anchor_filetime = (uint64_t(wall.dwHighDateTime) << 32) | wall.dwLowDateTime;
    h.anchor_span = uint64_t(after.QuadPart - before.QuadPart);
    delay_ = uint64_t(Decimal("MACRUNNER_DX12_PIPELINE_DELAY_MS", 0, 3600000)) * h.frequency / 1000;
    duration_ = uint64_t(Decimal("MACRUNNER_DX12_PIPELINE_DURATION_MS", 10000, 60000)) * h.frequency / 1000;
    n = GetEnvironmentVariableW(L"MACRUNNER_DX12_PIPELINE_ARM_PATH", arm_path_, 4096);
    if (n >= 4096) { UnmapViewOfFile(header_); header_ = nullptr; return; }
    if (!n) {
      arm_path_[0] = 0;
      h.window_begin = h.anchor_qpc + delay_;
      h.window_end = h.window_begin + duration_;
      begin_.store(h.window_begin, std::memory_order_relaxed);
    }
    std::memcpy(header_, &h, sizeof(h));
  }
  ~Recorder() {
    PreserveError guard;
    if (header_) { FlushViewOfFile(header_, 0); UnmapViewOfFile(header_); }
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  }
  bool IsActive() {
    if (!header_) return false;
    PreserveError guard;
    const auto now = Clock();
    return now && Active(now);
  }
  uint64_t Emit(Kind kind, Phase phase, const void *object, uint64_t id = 0,
                uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0,
                uint32_t status = 0, bool force = false) {
    if (!header_) return 0;
    PreserveError guard;
    const auto now = Clock();
    if (!now || (!force && !Active(now))) return 0;
    std::atomic_ref<uint32_t> count(header_->count);
    auto index = count.load(std::memory_order_relaxed);
    for (;;) {
      if (index >= capacity) {
        std::atomic_ref<uint32_t>(header_->overflow).store(1, std::memory_order_relaxed);
        Increment(header_->dropped);
        return 0;
      }
      if (count.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) break;
    }
    auto &r = reinterpret_cast<Record *>(header_ + 1)[index];
    r.qpc = now; r.object = Pointer(object); r.id = id;
    r.a = a; r.b = b; r.c = c; r.d = d; r.thread = GetCurrentThreadId();
    r.kind = kind; r.phase = phase; r.status = status; r.padding = 0;
    std::atomic_ref<uint32_t>(r.committed).store(index + 1, std::memory_order_release);
    return index + 1;
  }
};
inline Recorder &Get() { static Recorder recorder; return recorder; }

// Leave.id is the ENTER record ticket, not the caller's id. End records stay
// admitted past window end, so an in-flight operation does not silently vanish.
class Scope {
  Kind kind_;
  const void *object_;
  uint64_t ticket_;
  uint32_t status_ = UINT32_MAX; // Result unknown unless Result() was called.
public:
  Scope(Kind kind, const void *object, uint64_t id = 0, uint64_t a = 0, uint64_t b = 0) :
    kind_(kind), object_(object), ticket_(Get().Emit(kind, Enter, object, id, a, b)) {}
  ~Scope() { if (ticket_) Get().Emit(kind_, Leave, object_, ticket_, 0, 0, 0, 0, status_, true); }
  HRESULT Result(HRESULT hr) { status_ = uint32_t(hr); return hr; }
  uint64_t Ticket() const { return ticket_; }
};
} // namespace dxmt::pipeline_timing
