// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project; see README-MACRUNNER.md.
#pragma once
#include <cstdint>
#include <vector>

namespace dxmt {

// Register namespaces and shader visibility determine conflicts. Physical heap
// slots may alias, including resource types: they are not shader registers.
template<typename Desc>
bool ValidateRootBindingLayout(const Desc &desc) {
  constexpr uint64_t end32 = uint64_t(1) << 32;
  struct Binding { uint32_t type, space, first, visibility; uint64_t end; };
  std::vector<Binding> bindings;
  auto add = [&](uint32_t type, uint32_t space, uint32_t first, uint64_t end, uint32_t visibility) {
    if (type > 3 || visibility > 7 || end > end32 || uint64_t(first) >= end) return false;
    for (const auto &old : bindings)
      if (type == old.type && space == old.space &&
          (!visibility || !old.visibility || visibility == old.visibility) &&
          uint64_t(first) < old.end && uint64_t(old.first) < end) return false;
    bindings.push_back({type, space, first, visibility, end});
    return true;
  };
  if ((desc.NumParameters && !desc.pParameters) || (desc.NumStaticSamplers && !desc.pStaticSamplers)) return false;
  for (uint32_t i = 0; i < desc.NumParameters; ++i) {
    const auto &p = desc.pParameters[i];
    const uint32_t kind = uint32_t(p.ParameterType), visibility = uint32_t(p.ShaderVisibility);
    if (kind > 4 || visibility > 7) return false;
    if (kind == 0) {
      const auto &table = p.DescriptorTable;
      if (!table.NumDescriptorRanges || !table.pDescriptorRanges) return false;
      uint64_t next = 0;
      uint32_t heap_type = 2;
      for (uint32_t j = 0; j < table.NumDescriptorRanges; ++j) {
        const auto &r = table.pDescriptorRanges[j];
        const uint32_t type = uint32_t(r.RangeType), heap = type == 3 ? 1 : 0;
        if (type > 3 || !r.NumDescriptors || (heap_type != 2 && heap_type != heap)) return false;
        heap_type = heap;
        const uint64_t offset = r.OffsetInDescriptorsFromTableStart == UINT32_MAX ? next : r.OffsetInDescriptorsFromTableStart;
        const bool unbounded = r.NumDescriptors == UINT32_MAX;
        const uint64_t end = unbounded ? end32 : uint64_t(r.BaseShaderRegister) + r.NumDescriptors;
        if (offset >= end32 || (!unbounded && offset + r.NumDescriptors > end32)) return false;
        if (!add(type, r.RegisterSpace, r.BaseShaderRegister, end, visibility)) return false;
        next = unbounded ? end32 : offset + r.NumDescriptors;
      }
    } else {
      const bool constants = kind == 1;
      if (constants && !p.Constants.Num32BitValues) return false;
      const uint32_t reg = constants ? p.Constants.ShaderRegister : p.Descriptor.ShaderRegister;
      const uint32_t space = constants ? p.Constants.RegisterSpace : p.Descriptor.RegisterSpace;
      if (!add(kind <= 2 ? 2u : kind == 3 ? 0u : 1u, space, reg, uint64_t(reg) + 1, visibility)) return false;
    }
  }
  for (uint32_t i = 0; i < desc.NumStaticSamplers; ++i) {
    const auto &s = desc.pStaticSamplers[i];
    if (!add(3, s.RegisterSpace, s.ShaderRegister, uint64_t(s.ShaderRegister) + 1, uint32_t(s.ShaderVisibility))) return false;
  }
  return true;
}

} // namespace dxmt
