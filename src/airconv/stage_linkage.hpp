// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string_view>

namespace dxmt::stage_linkage {
struct Row {
  std::string_view semantic;
  uint32_t index, system, type, reg, mask, usage, stream, precision;
};
struct Lane { uint32_t source_register, source_component, target_register, target_component; };
struct VertexHullPlan {
  std::array<Lane, 128> lanes{};
  uint32_t count = 0, registers = 0;
  bool identity = true;
};
inline bool same_name(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
  for (size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
  return true;
}
inline bool valid_rows(std::span<const Row> rows) {
  if (rows.empty() || rows.size() > 32) return false;
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto &r = rows[i];
    if (r.semantic.empty() || r.reg >= 32 || !r.mask || (r.mask & ~15u) ||
        (r.usage & ~15u) || r.stream || r.precision || r.type < 1 || r.type > 3) return false;
    for (size_t j = 0; j < i; ++j)
      if ((rows[j].reg == r.reg && (rows[j].mask & r.mask)) ||
          (rows[j].index == r.index && same_name(rows[j].semantic, r.semantic))) return false;
  }
  return true;
}
// Move scalar bits by semantic/component order, never by coincidental register number.
inline bool vertex_hull(std::span<const Row> output, std::span<const Row> input, VertexHullPlan &plan) {
  plan = {};
  if (!valid_rows(output) || !valid_rows(input)) return false;
  for (const auto &o : output) plan.registers = std::max(plan.registers, o.reg + 1);
  for (const auto &p : input) {
    if (p.usage & ~p.mask) return false;
    plan.registers = std::max(plan.registers, p.reg + 1);
    const Row *match = nullptr;
    for (const auto &o : output) {
      if (o.index != p.index || !same_name(o.semantic, p.semantic)) continue;
      if (match || o.system != p.system || o.type != p.type) return false;
      match = &o;
    }
    if (!match) return false;
    const auto &o = *match;
    const bool same_lanes = !(p.mask & ~o.mask);
    if (!same_lanes && std::popcount(p.mask) != std::popcount(o.mask)) return false;
    uint32_t remaining = o.mask;
    for (uint32_t target = 0; target < 4; ++target) {
      if (!(p.mask & (1u << target))) continue;
      const uint32_t source = same_lanes ? target : std::countr_zero(remaining);
      remaining &= ~(1u << source);
      if (o.usage & (1u << source)) return false;
      plan.lanes[plan.count++] = {o.reg, source, p.reg, target};
      plan.identity &= o.reg == p.reg && source == target;
    }
  }
  return true;
}
} // namespace dxmt::stage_linkage
