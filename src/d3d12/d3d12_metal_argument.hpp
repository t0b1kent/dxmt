// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_capture_ownership.hpp"
#include "../winemetal/winemetal.h"
#include <vector>
namespace dxmt::metal_argument {
inline bool Acceleration(uint64_t kind) {
  return kind == WMTArgumentKindPrimitiveAccelerationStructure || kind == WMTArgumentKindInstanceAccelerationStructure;
}
struct Member { uint32_t index = 0, source = 0; WMTArgumentKind kind = WMTArgumentKindBuffer; };
struct Block { uint32_t index = 0; std::vector<Member> members; };
struct Plan { std::vector<Block> blocks; std::vector<Member> direct; };
inline capture::Status Validate(const Plan &plan, size_t sources) {
  if ((plan.blocks.empty() && plan.direct.empty()) || plan.blocks.size() > 31 ||
      plan.direct.size() > WMTMaxComputeBindings || sources > 65536) return capture::Status::Invalid;
  uint32_t slots = 0;
  for (const auto &block : plan.blocks) {
    if (block.index > 30 || (slots & (1u << block.index)) ||
        block.members.empty() || block.members.size() > 256) return capture::Status::Invalid;
    slots |= 1u << block.index;
    for (size_t i = 0; i < block.members.size(); ++i) {
      const auto &m = block.members[i];
      if (m.source >= sources || m.kind > WMTArgumentKindSampler) return capture::Status::Invalid;
      for (size_t j = 0; j < i; ++j)
        if (m.index == block.members[j].index) return capture::Status::Invalid;
    }
  }
  for (size_t i = 0; i < plan.direct.size(); ++i) {
    const auto &m = plan.direct[i];
    if (m.source >= sources || (m.kind > WMTArgumentKindTexture && !Acceleration(m.kind))) return capture::Status::Invalid;
    if (m.kind == WMTArgumentKindBuffer || Acceleration(m.kind)) {
      if (m.index > 30 || (slots & (1u << m.index))) return capture::Status::Invalid;
      slots |= 1u << m.index;
    } else {
      if (m.index > 127) return capture::Status::Invalid;
      for (size_t j = 0; j < i; ++j)
        if (plan.direct[j].kind == m.kind && plan.direct[j].index == m.index) return capture::Status::Invalid;
    }
  }
  return capture::Status::Ready;
}
inline capture::Status Match(const Plan &plan, size_t sources, const WMTComputeBindingLayout &layout) {
  if (Validate(plan, sources) != capture::Status::Ready || layout.count > WMTMaxComputeBindings ||
      !layout.max_threads || layout.count != plan.blocks.size() + plan.direct.size())
    return capture::Status::Invalid;
  for (size_t i = 0; i < layout.count; ++i) {
    const auto &r = layout.bindings[i];
    if (r.access > 2 || r.kind > WMTComputeBindingInstanceAccelerationStructure) return capture::Status::Unsupported;
    if ((r.kind == WMTComputeBindingTexture && r.index > 127) ||
        (r.kind != WMTComputeBindingTexture && (r.index > 30 || (!Acceleration(r.kind) && !r.alignment)))) return capture::Status::Invalid;
    if (Acceleration(r.kind) && (r.access || r.alignment || r.minimum_size ||
        r.texture_type || r.texture_data_type || r.depth)) return capture::Status::Invalid;
    for (size_t j = 0; j < i; ++j)
      if ((layout.bindings[j].kind == WMTComputeBindingTexture) == (r.kind == WMTComputeBindingTexture) &&
          layout.bindings[j].index == r.index) return capture::Status::Invalid;
    bool found = false;
    if (r.kind == WMTComputeBindingArgumentBuffer) {
      for (const auto &b : plan.blocks) if (b.index == r.index) found = true;
    } else {
      for (const auto &m : plan.direct)
        if (m.index == r.index && uint64_t(m.kind) == uint64_t(r.kind)) found = true;
    }
    if (!found) return capture::Status::Invalid;
  }
  return capture::Status::Ready;
}
inline bool Threads(WMTSize threads, uint64_t maximum) {
  if (!threads.width || !threads.height || !threads.depth || !maximum ||
      threads.width > maximum / threads.height) return false;
  return threads.width * threads.height <= maximum / threads.depth;
}
} // namespace dxmt::metal_argument
