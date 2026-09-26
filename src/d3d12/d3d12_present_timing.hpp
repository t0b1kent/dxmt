// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxmt::present_timing {

struct Header {
  char magic[8];
  uint32_t version, record_size, capacity, pid;
  uint64_t frequency, anchor_qpc, anchor_filetime, anchor_span;
  uint32_t overflow, reserved_count;
};
struct Record {
  uint64_t begin, end, swapchain;
  uint32_t flags, sync, width, height, result, thread;
  uint64_t reserved;
  uint32_t committed, padding;
};
static_assert(sizeof(Header) == 64 && sizeof(Record) == 64);
static_assert(offsetof(Header, reserved_count) == 60 && offsetof(Record, committed) == 56);

// Opt-in diagnostic only: append to a bounded mapped file, not a log line per frame.
// Measures CPU Present calls, never GPU execution or actual display scanout.
class Recorder {
  static constexpr uint32_t capacity = 262144;
  HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
  Header *header_ = nullptr;

public:
  Recorder() {
    WCHAR path[4096];
    const DWORD length = GetEnvironmentVariableW(L"MACRUNNER_DX12_PRESENT_TIMING_PATH", path, 4096);
    if (!length || length >= 4096) return;
    LARGE_INTEGER frequency{}, before{}, after{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return;
    FILETIME wall{};
    if (!QueryPerformanceCounter(&before) || before.QuadPart <= 0) return;
    GetSystemTimeAsFileTime(&wall);
    if (!QueryPerformanceCounter(&after) || after.QuadPart < before.QuadPart) return;
    file_ = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return;
    constexpr DWORD bytes = sizeof(Header) + capacity * sizeof(Record);
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, bytes, nullptr);
    if (!mapping_) return;
    header_ = static_cast<Header *>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, bytes));
    if (!header_) return;
    Header initial{};
    std::memcpy(initial.magic, "MRPT001", 8);
    initial.version = 1; initial.record_size = sizeof(Record); initial.capacity = capacity;
    initial.pid = GetCurrentProcessId(); initial.frequency = frequency.QuadPart;
    initial.anchor_qpc = before.QuadPart + (after.QuadPart - before.QuadPart) / 2;
    initial.anchor_filetime = (uint64_t(wall.dwHighDateTime) << 32) | wall.dwLowDateTime;
    initial.anchor_span = after.QuadPart - before.QuadPart;
    std::memcpy(header_, &initial, sizeof(initial));
  }
  ~Recorder() {
    if (header_) { FlushViewOfFile(header_, 0); UnmapViewOfFile(header_); }
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  }
  uint64_t Begin() const {
    if (!header_) return 0;
    LARGE_INTEGER value;
    return QueryPerformanceCounter(&value) ? uint64_t(value.QuadPart) : 0;
  }
  void End(uint64_t begin, const void *swapchain, UINT flags, UINT sync, UINT width, UINT height, HRESULT result) {
    if (!begin || !header_) return;
    LARGE_INTEGER end;
    if (!QueryPerformanceCounter(&end)) return;
    auto count = reinterpret_cast<volatile LONG *>(&header_->reserved_count);
    LONG index = InterlockedCompareExchange(count, 0, 0);
    for (;;) {
      if (uint32_t(index) >= capacity) {
        InterlockedExchange(reinterpret_cast<volatile LONG *>(&header_->overflow), 1);
        return;
      }
      const LONG observed = InterlockedCompareExchange(count, index + 1, index);
      if (observed == index) break;
      index = observed;
    }
    auto &record = reinterpret_cast<Record *>(header_ + 1)[index];
    record.begin = begin; record.end = end.QuadPart;
    record.swapchain = reinterpret_cast<uintptr_t>(swapchain);
    record.flags = flags; record.sync = sync; record.width = width; record.height = height;
    record.result = uint32_t(result); record.thread = GetCurrentThreadId(); record.reserved = 0;
    record.padding = 0;
    // Readers ignore uncommitted slots. Slots are never reused, including after overflow.
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&record.committed), index + 1);
  }
};

inline Recorder &Get() { static Recorder recorder; return recorder; }

} // namespace dxmt::present_timing
