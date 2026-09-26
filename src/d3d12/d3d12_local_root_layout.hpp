// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>

namespace dxmt::local_root {
enum Kind : uint32_t { Table, Constants, CBV, SRV, UAV };
enum RangeKind : uint32_t { RangeSRV, RangeUAV, RangeCBV, RangeSampler };
struct Range {
  uint32_t kind, count, shader_register, space, descriptor_offset;
};
struct Parameter {
  uint32_t kind, byte_offset, byte_size, shader_register, space;
  std::vector<Range> ranges;
};
struct Layout {
  uint32_t bytes = 0;
  std::vector<Parameter> parameters;
};

// Local arguments follow the 32-byte shader identifier: DWORD arrays pack at
// four-byte alignment; descriptors/table handles at eight. This is NOT the
// private qword upload layout used for global compute/graphics root arguments.
template<typename Desc>
bool Build(const Desc &desc, Layout &out) {
  out = {};
  if (uint32_t(desc.Flags) != 0x80 || desc.NumParameters > 1016 ||
      (desc.NumParameters && !desc.pParameters) ||
      (desc.NumStaticSamplers && !desc.pStaticSamplers))
    return false;
  for (uint32_t i = 0; i < desc.NumStaticSamplers; ++i)
    if (uint32_t(desc.pStaticSamplers[i].ShaderVisibility) != 0)
      return false;
  Layout result;
  result.parameters.reserve(desc.NumParameters);
  for (uint32_t i = 0; i < desc.NumParameters; ++i) {
    const auto &p = desc.pParameters[i];
    const uint32_t kind = uint32_t(p.ParameterType);
    if (kind > UAV || uint32_t(p.ShaderVisibility) != 0)
      return false;
    Parameter item{kind, result.bytes, 0, 0, 0, {}};
    if (kind == Constants) {
      const uint32_t n = p.Constants.Num32BitValues;
      if (!n || n > 1016)
        return false;
      item.byte_size = n * 4;
      item.shader_register = p.Constants.ShaderRegister;
      item.space = p.Constants.RegisterSpace;
    } else {
      item.byte_offset = (result.bytes + 7) & ~uint32_t(7);
      item.byte_size = 8;
      if (kind != Table) {
        item.shader_register = p.Descriptor.ShaderRegister;
        item.space = p.Descriptor.RegisterSpace;
      } else {
        const auto &table = p.DescriptorTable;
        if (table.NumDescriptorRanges && !table.pDescriptorRanges)
          return false;
        uint64_t next = 0;
        bool sampler = false, resource = false;
        for (uint32_t j = 0; j < table.NumDescriptorRanges; ++j) {
          const auto &range = table.pDescriptorRanges[j];
          const uint32_t type = uint32_t(range.RangeType);
          if (type > RangeSampler)
            return false;
          sampler |= type == RangeSampler;
          resource |= type != RangeSampler;
          const uint64_t offset = range.OffsetInDescriptorsFromTableStart == UINT32_MAX
              ? next : range.OffsetInDescriptorsFromTableStart;
          if ((sampler && resource) || offset > UINT32_MAX)
            return false;
          item.ranges.push_back({type, range.NumDescriptors, range.BaseShaderRegister,
                                 range.RegisterSpace, uint32_t(offset)});
          // APPEND after an unbounded range cannot name a finite next slot.
          next = range.NumDescriptors == UINT32_MAX ? UINT64_MAX : offset + range.NumDescriptors;
        }
      }
    }
    if (item.byte_offset > 4064 || item.byte_size > 4064 - item.byte_offset)
      return false;
    result.bytes = item.byte_offset + item.byte_size;
    result.parameters.push_back(std::move(item));
  }
  out = std::move(result);
  return true;
}

enum class Match { Found, Missing, Ambiguous, Invalid };
struct Binding {
  uint32_t parameter = 0;
  uint64_t descriptor_index = 0;
  bool table = false;
};
// Returns an offset within a table, not a Metal resource or a fabricated GPU VA.
inline Match Find(const Layout &layout, uint32_t type, uint32_t reg, uint32_t space, Binding &out) {
  out = {};
  if (type > RangeSampler)
    return Match::Invalid;
  bool found = false;
  Binding selected;
  for (uint32_t i = 0; i < layout.parameters.size(); ++i) {
    const auto &p = layout.parameters[i];
    if (p.kind == Table) {
      for (const auto &r : p.ranges) {
        if (r.kind != type || r.space != space || reg < r.shader_register)
          continue;
        const uint64_t delta = uint64_t(reg) - r.shader_register;
        if (r.count != UINT32_MAX && delta >= r.count)
          continue;
        if (found)
          return Match::Ambiguous;
        selected = {i, uint64_t(r.descriptor_offset) + delta, true};
        found = true;
      }
    } else {
      const uint32_t t = p.kind == Constants || p.kind == CBV ? RangeCBV : p.kind == SRV ? RangeSRV : RangeUAV;
      if (t == type && p.shader_register == reg && p.space == space) {
        if (found)
          return Match::Ambiguous;
        selected = {i, 0, false};
        found = true;
      }
    }
  }
  if (!found)
    return Match::Missing;
  out = selected;
  return Match::Found;
}

inline bool Read(const Layout &layout, uint32_t parameter, const void *arguments,
                 size_t bytes, uint32_t constant_word, uint64_t &out) {
  out = 0;
  if (!arguments || parameter >= layout.parameters.size() || bytes < layout.bytes)
    return false;
  const auto &p = layout.parameters[parameter];
  const bool constant = p.kind == Constants;
  if (p.kind > UAV || (constant ? constant_word >= p.byte_size / 4 : constant_word != 0))
    return false;
  const uint64_t offset = uint64_t(p.byte_offset) + (constant ? uint64_t(constant_word) * 4 : 0);
  const uint32_t size = constant ? 4 : 8;
  if (offset > layout.bytes || size > layout.bytes - offset || offset > bytes || size > bytes - offset)
    return false;
  const auto *data = static_cast<const uint8_t *>(arguments) + offset;
  for (uint32_t i = 0; i < size; ++i)
    out |= uint64_t(data[i]) << (i * 8);
  return true;
}
} // namespace dxmt::local_root
