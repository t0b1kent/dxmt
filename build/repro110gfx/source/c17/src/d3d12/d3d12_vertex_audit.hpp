// SPDX-License-Identifier: LGPL-2.1-or-later
// MacRunner: bounded first-use IA evidence; no GPU reads or encoding changes.
#pragma once
#include "d3d12.h"
#include "d3d12_c17_stats.hpp"
#include <array>
#include <cstdarg>
#include <string>

namespace dxmt::vertex_audit {
inline bool Enabled() {
  static const bool v = c17::ReadKey("MACRUNNER_DX12_VERTEX_INPUT_AUDIT", 1) == 1;
  return v;
}
inline unsigned Limit() {
  static const unsigned v = c17::ReadKey("MACRUNNER_DX12_VERTEX_INPUT_AUDIT_LIMIT", 4096);
  return v ? v : 2048;
}
inline uint64_t NextId() {
  static std::atomic<uint64_t> ids{0};
  return ids.fetch_add(1, std::memory_order_relaxed) + 1;
}
struct Element {
  uint32_t format, raw_offset, offset, slot, classification, step;
};
struct State {
  const uint64_t id = NextId();
  std::atomic<bool> emitted{false};
  std::array<Element, 32> elements{};
  uint32_t count = 0, slots = 0;
  bool complete = true;
  std::string vs;
  State() = default;
  // Cache clones are separate D3D12 PSOs, even when Metal pipelines are shared.
  State(const State &s) : elements(s.elements), count(s.count), slots(s.slots), complete(s.complete), vs(s.vs) {}
};
inline void Emit(State &s, const void *owner, uint64_t native, uint32_t used_slots,
                 const D3D12_VERTEX_BUFFER_VIEW *vb, unsigned ib_format, const char *kind,
                 unsigned indirect_ia, unsigned mesh) {
  if (s.emitted.exchange(true, std::memory_order_relaxed)) return;
  static std::atomic<uint64_t> rows{0};
  const auto row = rows.fetch_add(1, std::memory_order_relaxed);
  if (row > Limit()) return;
  const DWORD error = GetLastError();
  char line[8192];
  size_t length = 0;
  bool complete = true;
  auto add = [&](const char *format, ...) {
    if (!complete) return;
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(line + length, sizeof(line) - length, format, args);
    va_end(args);
    if (n < 0 || size_t(n) >= sizeof(line) - length) { complete = false; return; }
    length += size_t(n);
  };
  if (row == Limit()) {
    add("dx12_vertex_input op=limit cap=%u incomplete=1\n", Limit());
  } else {
    add("dx12_vertex_input op=pso row=%llu id=%llu owner=%p native=%llx vs=%s "
        "snapshot=first_recorded_draw kind=%s indirect_ia=%u mesh=%u used_slots=%08x ib_dxgi=%u elements=%u layout=",
        (unsigned long long)row, (unsigned long long)s.id, owner, (unsigned long long)native,
        s.vs.c_str(), kind, indirect_ia, mesh, used_slots, ib_format, s.count);
    if (!s.count) add("none");
    for (unsigned i = 0; i < s.count; ++i) {
      const auto &e = s.elements[i];
      add("%s%u:%u:%u:%u:%u:%u", i ? "," : "", e.format, e.raw_offset, e.offset,
          e.slot, e.classification, e.step);
    }
    add(" vb=");
    bool first = true;
    for (unsigned slot = 0; slot < 32; ++slot) {
      if (!(s.slots & (1u << slot))) continue;
      add("%s%u:%u:%u:%u", first ? "" : ",", slot, vb[slot].StrideInBytes,
          vb[slot].SizeInBytes, unsigned(vb[slot].BufferLocation != 0));
      first = false;
    }
    if (first) add("none");
    add(" incomplete=%u\n", unsigned(indirect_ia != 0 || !s.complete));
  }
  // Never publish a partial record as if it described a complete layout.
  if (!complete) length = std::snprintf(line, sizeof(line),
      "dx12_vertex_input op=overflow id=%llu incomplete=1\n", (unsigned long long)s.id);
  DWORD written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
  SetLastError(error);
}
} // namespace dxmt::vertex_audit
