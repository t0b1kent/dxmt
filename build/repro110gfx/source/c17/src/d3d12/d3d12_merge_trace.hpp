// SPDX-License-Identifier: LGPL-2.1-or-later
// MacRunner 2026: diagnostic evidence only; NEVER authorizes a merge.
#pragma once
#include "d3d12.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <tuple>
#include <vector>

namespace dxmt::merge_trace {
inline std::atomic<uint64_t> next_present{1};
inline std::atomic<uint32_t> lines{0};
inline std::atomic<uint32_t> recorded_rows{0};
inline uint64_t Target() {
  static const uint64_t target = [] {
    const auto error = GetLastError();
    char text[16]{};
    const auto n = GetEnvironmentVariableA("MACRUNNER_DX12_MERGE_TRACE_FRAME", text, sizeof(text));
    uint64_t value = 0;
    bool valid = n > 0 && n < sizeof(text);
    for (DWORD i = 0; valid && i < n; ++i) {
      valid = text[i] >= '0' && text[i] <= '9';
      if (valid) value = value * 10 + unsigned(text[i] - '0');
    }
    SetLastError(error);
    return valid && value <= 1000000 ? value : uint64_t(0);
  }();
  return target;
}
inline bool RecordWindow() {
  const auto target = Target();
  if (!target) return false;
  const auto n = next_present.load(std::memory_order_relaxed);
  return n <= target && target - n <= 2;
}
inline bool SubmitWindow() {
  return Target() && next_present.load(std::memory_order_relaxed) == Target();
}

struct Resource {
  uint64_t object = 0, native = 0, address = 0, bytes = 0;
  uint32_t kind = 0; // 1 buffer, 2 texture; zero means unresolved
  auto Key() const { return std::tie(object, native, address, bytes, kind); }
};
struct Use {
  Resource resource;
  uint64_t offset = 0, bytes = 0, view = 0;
  uint32_t role = 0; // 1 RTV, 2 DSV, 3 SRV, 4 UAV, 5 CBV, 6 VB, 7 IB, 8 counter
  uint32_t access = 0; // read=1, write=2; binding capabilities, NOT shader execution
  auto Key() const { return std::tuple{resource.Key(), offset, bytes, view, role, access}; }
  bool operator==(const Use &b) const { return Key() == b.Key(); }
};
struct Barrier {
  Resource a, b;
  uint32_t type = 0, flags = 0, before = 0, after = 0, subresource = 0;
};
template<typename T, size_t Limit> struct Bounded {
  std::vector<T> rows;
  uint64_t dropped = 0, failed = 0;
  void Add(const T &row) noexcept {
    if (rows.size() >= Limit) { ++dropped; return; }
    auto n = recorded_rows.load(std::memory_order_relaxed);
    while (n < 65536 && !recorded_rows.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) {}
    if (n >= 65536) { ++dropped; return; }
    try { rows.push_back(row); } catch (...) { ++failed; }
  }
};
struct Render {
  Bounded<Use, 512> uses;
  uint64_t draws = 0, unrecorded = 0, unknown = 0;
  void Add(const Use &use) noexcept {
    for (const auto &old : uses.rows) if (old == use) return;
    uses.Add(use);
  }
};
struct Barriers {
  Bounded<Barrier, 256> values;
  uint64_t requested = 0, unrecorded = 0;
};

template<typename... Args> inline void Emit(const void *queue, uint64_t seq, const void *list,
    uint64_t pass, const char *op, const char *format, Args... args) noexcept {
  if (!SubmitWindow()) return;
  const DWORD error = GetLastError();
  const auto row = lines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (row > 16385) { SetLastError(error); return; }
  char detail[1280], output[1600];
  int n;
  if constexpr (sizeof...(args) == 0) n = std::snprintf(detail, sizeof(detail), "%s", format);
  else n = std::snprintf(detail, sizeof(detail), format, args...);
  if (n < 0 || size_t(n) >= sizeof(detail)) {
    op = "format_limit";
    std::snprintf(detail, sizeof(detail), "dropped=1");
  }
  if (row == 16385) { op = "limit"; std::snprintf(detail, sizeof(detail), "dropped=1 cap=16384"); }
  const int count = std::snprintf(output, sizeof(output),
      "dx12_merge_trace row=%u frame=%llu queue=%p seq=%llu list=%p pass=%llu op=%s %s\n",
      row, (unsigned long long)Target(), queue, (unsigned long long)seq, list,
      (unsigned long long)pass, op, detail);
  DWORD written;
  if (count > 0 && size_t(count) < sizeof(output))
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), output, DWORD(count), &written, nullptr);
  SetLastError(error);
}
} // namespace dxmt::merge_trace
