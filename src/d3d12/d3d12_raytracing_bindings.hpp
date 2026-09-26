// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_raytracing_library.hpp"
#include "d3d12_raytracing_state_plan.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace dxmt::ray_binding {
enum class Status { Ready, Invalid, Unsupported };
enum class Source { Table, Descriptor, Constants, StaticSampler };

enum class DataPolicy { None, Volatile, WhileExecute, Static };
struct DescriptorPolicy {
  bool descriptors_volatile = false, keep_buffer_bounds = false;
  DataPolicy data = DataPolicy::None;
};
inline Status DecodePolicy(uint32_t type, uint32_t flags, bool table, DescriptorPolicy &out) {
  out = {};
  if (type > 3 || (!table && type == 3)) return Status::Invalid;
  if (flags & ~uint32_t(0x1000f)) return Status::Unsupported;
  if (!table && (flags & 0x10001)) return Status::Invalid;
  const uint32_t data = flags & 14;
  if (data && (data & (data - 1))) return Status::Invalid;
  if (type == 3 && data) return Status::Invalid;
  if (type == 3 && (flags & 0x10000)) return Status::Unsupported;
  if ((flags & 1) && (flags & 0x10008)) return Status::Invalid;
  out.descriptors_volatile = (flags & 1) != 0;
  out.keep_buffer_bounds = (flags & 0x10000) != 0;
  out.data = type == 3 ? DataPolicy::None :
      data == 2 ? DataPolicy::Volatile : data == 4 ? DataPolicy::WhileExecute :
      data == 8 ? DataPolicy::Static : type == 1 ? DataPolicy::Volatile : DataPolicy::WhileExecute;
  return Status::Ready;
}

constexpr uint64_t RegisterEnd = uint64_t(1) << 32;
constexpr uint32_t None = UINT32_MAX;
struct Span {
  uint32_t type, space, first;
  uint64_t end;
  Source source;
  uint32_t parameter, range, offset, constants;
  uint32_t flags = 0;
};
struct Sampler {
  uint32_t space, reg;
  std::array<uint32_t, 10> state;
};
struct RootLayout {
  bool local = false;
  std::vector<Span> spans;
  std::vector<Sampler> samplers;
};
inline auto Key(const Span &s) { return std::pair{s.type, s.space}; }
inline bool Overlap(const Span &a, const Span &b) {
  return Key(a) == Key(b) && uint64_t(a.first) < b.end && uint64_t(b.first) < a.end;
}

// Retain register metadata at root creation. This does not alter graphics root
// acceptance: the caller records this separate raytracing-compatibility status.
template<typename Desc>
Status Build(const Desc &desc, RootLayout &out) {
  out = {};
  RootLayout result;
  const uint32_t flags = uint32_t(desc.Flags);
  result.local = (flags & 0x80) != 0;
  if ((result.local && flags != 0x80) || (!result.local && (flags & ~uint32_t(0x37f))))
    return Status::Unsupported; // Directly indexed heaps need separate mapping.
  if ((desc.NumParameters && !desc.pParameters) ||
      (desc.NumStaticSamplers && !desc.pStaticSamplers) || desc.NumStaticSamplers > 2032)
    return Status::Invalid;
  uint64_t cost = 0, local_bytes = 0;
  for (uint32_t i = 0; i < desc.NumParameters; ++i) {
    const auto &p = desc.pParameters[i];
    const uint32_t kind = uint32_t(p.ParameterType), visibility = uint32_t(p.ShaderVisibility);
    if (kind > 4 || visibility > 7 || (result.local && visibility)) return Status::Invalid;
    cost += kind == 0 ? 1 : kind == 1 ? p.Constants.Num32BitValues : 2;
    local_bytes = kind == 1 ? local_bytes + uint64_t(p.Constants.Num32BitValues) * 4
                           : ((local_bytes + 7) & ~uint64_t(7)) + 8;
    if ((!result.local && cost > 64) || (result.local && local_bytes > 4064)) return Status::Invalid;
    if (kind == 1 && !p.Constants.Num32BitValues) return Status::Invalid;
    // Only ALL is visible to compute/raytracing; keep graphics-only arguments
    // out of this namespace while retaining their cost and original indices.
    if (visibility) continue;
    if (kind == 0) {
      const auto &table = p.DescriptorTable;
      if (!table.NumDescriptorRanges || !table.pDescriptorRanges) return Status::Invalid;
      uint64_t next = 0;
      bool sampler = false, resource = false;
      std::vector<std::tuple<uint64_t, uint64_t, uint32_t>> slots;
      for (uint32_t j = 0; j < table.NumDescriptorRanges; ++j) {
        const auto &r = table.pDescriptorRanges[j];
        const uint32_t type = uint32_t(r.RangeType);
        if (type > 3 || !r.NumDescriptors) return Status::Invalid;
        DescriptorPolicy policy;
        const auto validity = DecodePolicy(type, uint32_t(r.Flags), true, policy);
        if (validity != Status::Ready) return validity;
        sampler |= type == 3; resource |= type != 3;
        if (sampler && resource) return Status::Invalid;
        const uint64_t end = r.NumDescriptors == UINT32_MAX ? RegisterEnd
            : uint64_t(r.BaseShaderRegister) + r.NumDescriptors;
        const uint64_t offset = r.OffsetInDescriptorsFromTableStart == UINT32_MAX ? next
            : r.OffsetInDescriptorsFromTableStart;
        if (end > RegisterEnd || offset >= RegisterEnd) return Status::Invalid;
        const uint64_t slot_end = r.NumDescriptors == UINT32_MAX ? UINT64_MAX : offset + r.NumDescriptors;
        if (r.NumDescriptors != UINT32_MAX && slot_end > RegisterEnd) return Status::Invalid;
        slots.emplace_back(offset, slot_end, type);
        result.spans.push_back({type, r.RegisterSpace, r.BaseShaderRegister, end,
            Source::Table, i, j, uint32_t(offset), 0, uint32_t(r.Flags)});
        next = slot_end;
      }
      // Physical descriptor aliasing is allowed only within the same type.
      std::sort(slots.begin(), slots.end());
      std::array<uint64_t, 4> ends{};
      for (const auto &[first, end, type] : slots) {
        for (uint32_t t = 0; t < 4; ++t)
          if (t != type && first < ends[t]) return Status::Invalid;
        ends[type] = std::max(ends[type], end);
      }
    } else {
      const bool constants = kind == 1;
      const uint32_t data_flags = constants ? 0 : uint32_t(p.Descriptor.Flags);
      if (!constants) {
        DescriptorPolicy policy;
        const auto validity = DecodePolicy(kind == 2 ? 2u : kind == 3 ? 0u : 1u, data_flags, false, policy);
        if (validity != Status::Ready) return validity;
      }
      const uint32_t reg = constants ? p.Constants.ShaderRegister : p.Descriptor.ShaderRegister;
      const uint32_t space = constants ? p.Constants.RegisterSpace : p.Descriptor.RegisterSpace;
      result.spans.push_back({kind <= 2 ? 2u : kind == 3 ? 0u : 1u, space, reg, uint64_t(reg) + 1,
          constants ? Source::Constants : Source::Descriptor, i, 0, 0,
          constants ? p.Constants.Num32BitValues : 0, data_flags});
    }
  }
  for (uint32_t i = 0; i < desc.NumStaticSamplers; ++i) {
    const auto &s = desc.pStaticSamplers[i];
    const uint32_t visibility = uint32_t(s.ShaderVisibility);
    if (visibility > 7 || (result.local && visibility)) return Status::Invalid;
    if (visibility) continue;
    if (std::isnan(s.MipLODBias) || std::isnan(s.MinLOD) || std::isnan(s.MaxLOD)) return Status::Invalid;
    const auto bits = [](float x) { return std::bit_cast<uint32_t>(x == 0 ? 0.0f : x); };
    result.samplers.push_back({s.RegisterSpace, s.ShaderRegister,
        {uint32_t(s.Filter), uint32_t(s.AddressU), uint32_t(s.AddressV), uint32_t(s.AddressW),
         bits(s.MipLODBias), s.MaxAnisotropy, uint32_t(s.ComparisonFunc), uint32_t(s.BorderColor),
         bits(s.MinLOD), bits(s.MaxLOD)}});
    result.spans.push_back({3, s.RegisterSpace, s.ShaderRegister, uint64_t(s.ShaderRegister) + 1,
        Source::StaticSampler, i, 0, 0, 0});
  }
  std::sort(result.spans.begin(), result.spans.end(), [](const Span &a, const Span &b) {
    return std::tie(a.type, a.space, a.first) < std::tie(b.type, b.space, b.first);
  });
  for (size_t i = 1; i < result.spans.size(); ++i)
    if (Overlap(result.spans[i - 1], result.spans[i])) return Status::Invalid;
  out = std::move(result);
  return Status::Ready;
}

inline Status Pair(const RootLayout *global, const RootLayout *local) {
  if ((global && global->local) || (local && !local->local)) return Status::Invalid;
  if (!global || !local) return Status::Ready;
  size_t i = 0, j = 0;
  while (i < global->spans.size() && j < local->spans.size()) {
    const auto &a = global->spans[i], &b = local->spans[j];
    if (Overlap(a, b)) return Status::Invalid;
    if (std::tie(a.type, a.space, a.first) < std::tie(b.type, b.space, b.first)) ++i;
    else ++j;
  }
  return Status::Ready;
}

struct Binding {
  uint32_t resource = None;
  bool local = false;
  Source source = Source::Table;
  uint32_t parameter = None, range = None;
  uint64_t descriptor_offset = 0;
  uint32_t flags = 0;
};


inline Status SelectTable(const RootLayout &root, const Binding &binding, const Span *&out) {
  out = nullptr;
  if (root.local != binding.local || binding.source != Source::Table) return Status::Invalid;
  for (const auto &span : root.spans) {
    if (span.source != Source::Table || span.parameter != binding.parameter || span.range != binding.range) continue;
    if (span.type > 3 || span.end <= span.first || span.end > RegisterEnd ||
        binding.flags != span.flags || binding.descriptor_offset < span.offset ||
        binding.descriptor_offset - span.offset >= span.end - span.first) return Status::Invalid;
    out = &span;
    return Status::Ready;
  }
  return Status::Invalid;
}

inline Status Find(const RootLayout *global, const RootLayout *local,
                   const ray_library::Resource &r, Binding &out) {
  out = {};
  if (Pair(global, local) != Status::Ready) return Status::Invalid;
  if (r.type > 3 || r.lower > r.upper || r.kind == 0) return Status::Invalid;
  if (r.type != 1 && (r.flags & 0x27)) return Status::Invalid;
  if (r.flags & ~uint32_t(0x3f) || r.kind > 16 || (r.flags & 0x34)) return Status::Unsupported;
  if ((r.type == 2 && r.kind != 13) || (r.type == 3 && r.kind != 14) ||
      (r.type < 2 && (r.kind == 13 || r.kind == 14)) || (r.type == 1 && r.kind == 16))
    return Status::Invalid;
  const uint64_t end = uint64_t(r.upper) + 1;
  std::vector<const Span *> partial;
  for (const auto *root : {global, local}) {
    if (!root) continue;
    for (const auto &s : root->spans) {
      if (s.type != r.type || s.space != r.space || s.end <= r.lower || uint64_t(s.first) >= end) continue;
      partial.push_back(&s);
      if (s.first > r.lower || s.end < end) continue;
      // RDAT's UINT_MAX upper bound denotes an unbounded array, even when
      // the lower bound happens to be the same numeric value.
      if (s.source != Source::Table && r.upper == UINT32_MAX) return Status::Invalid;
      if (s.source == Source::Descriptor) {
        if (r.lower != r.upper || (r.flags & 2)) return Status::Invalid;
        if (r.flags & 8) return Status::Unsupported;
        if (r.type != 2 && r.kind != 11 && r.kind != 12 && !(r.type == 0 && r.kind == 16))
          return Status::Invalid;
      }
      // Base RDAT does not describe constant-buffer byte size or indexing.
      // Do not certify a constants-backed cbuffer from register coverage alone.
      if (s.source == Source::Constants) return Status::Unsupported;
      out = {None, root->local, s.source, s.parameter, s.range,
          s.source == Source::Table ? uint64_t(s.offset) + r.lower - s.first : 0, s.flags};
      return Status::Ready;
    }
  }
  std::sort(partial.begin(), partial.end(), [](const Span *a, const Span *b) { return a->first < b->first; });
  uint64_t covered = r.lower;
  for (auto *s : partial) {
    if (s->first > covered) break;
    covered = std::max(covered, s->end);
  }
  // Full coverage split over multiple arguments/ranges needs a separate
  // array-index lowering contract; a gap is unambiguously invalid.
  return covered >= end ? Status::Unsupported : Status::Invalid;
}

// Revalidate the exact resolver result before selecting an array element.
inline Status SelectResource(const RootLayout &root, const Binding &binding,
                             const ray_library::Resource &resource, uint32_t element,
                             const Span *&out, uint64_t &offset) {
  out = nullptr;
  offset = 0;
  if (root.local != binding.local) return Status::Invalid;
  Binding expected;
  const auto status = Find(root.local ? nullptr : &root, root.local ? &root : nullptr, resource, expected);
  if (status != Status::Ready) return status;
  if (std::tie(expected.local, expected.source, expected.parameter, expected.range, expected.descriptor_offset, expected.flags) !=
      std::tie(binding.local, binding.source, binding.parameter, binding.range, binding.descriptor_offset, binding.flags) ||
      uint64_t(element) > uint64_t(resource.upper) - resource.lower) return Status::Invalid;
  if (binding.source != Source::Table && element) return Status::Invalid;
  if (binding.source == Source::Constants || binding.source == Source::StaticSampler) return Status::Unsupported;
  for (const auto &span : root.spans) {
    if (span.source != binding.source || span.parameter != binding.parameter || span.range != binding.range) continue;
    const uint64_t selected = binding.descriptor_offset + element;
    if (binding.source == Source::Table &&
        (selected < span.offset || selected - span.offset >= span.end - span.first)) return Status::Invalid;
    out = &span;
    offset = selected;
    return Status::Ready;
  }
  return Status::Invalid;
}

template<typename Libraries, typename LookupRoot>
Status Resolve(const state_plan::Plan &plan, const Libraries &libraries, LookupRoot lookup,
               std::vector<std::vector<Binding>> &out) {
  out.clear();
  if (plan.shaders.size() != plan.shader_bindings.size() || plan.hits.size() != plan.hit_bindings.size())
    return Status::Invalid;
  std::map<std::pair<uint32_t, uint32_t>, std::array<uint32_t, 10>> local_samplers;
  std::set<std::tuple<uint32_t, uint32_t, std::array<uint32_t, 10>>> unique_samplers;
  std::set<const RootLayout *> seen_roots;
  const auto roots = [&](const state_plan::Bindings &b, std::array<const RootLayout *, 2> &r) {
    r = {};
    for (uint32_t k = 0; k < 2; ++k) {
      if (b[k] == state_plan::None) continue;
      if (b[k] >= plan.items.size() || plan.items[b[k]].kind != k) return Status::Invalid;
      r[k] = lookup(plan.items[b[k]].key);
      if (!r[k] || r[k]->local != bool(k)) return Status::Invalid;
      if (seen_roots.insert(r[k]).second) {
        for (const auto &s : r[k]->samplers) {
          if (k) {
            const auto [it, inserted] = local_samplers.emplace(std::pair{s.space, s.reg}, s.state);
            if (!inserted && it->second != s.state) return Status::Invalid;
          }
          unique_samplers.emplace(s.space, s.reg, s.state);
          if (unique_samplers.size() > 2032) return Status::Invalid;
        }
      }
    }
    return Pair(r[0], r[1]);
  };
  std::vector<std::vector<Binding>> result;
  for (size_t i = 0; i < plan.shaders.size(); ++i) {
    const auto &s = plan.shaders[i];
    if (s.library >= libraries.size()) return Status::Invalid;
    const auto &library = libraries[s.library].prepared.reflection;
    if (s.function >= library.functions.size()) return Status::Invalid;
    std::array<const RootLayout *, 2> r;
    auto status = roots(plan.shader_bindings[i], r);
    if (status != Status::Ready) return status;
    std::vector<Binding> bindings;
    for (uint32_t index : library.functions[s.function].resources) {
      if (index >= library.resources.size()) return Status::Invalid;
      Binding binding;
      status = Find(r[0], r[1], library.resources[index], binding);
      if (status != Status::Ready) return status;
      binding.resource = index;
      bindings.push_back(binding);
    }
    result.push_back(std::move(bindings));
  }
  for (const auto &b : plan.hit_bindings) {
    std::array<const RootLayout *, 2> r;
    const auto status = roots(b, r);
    if (status != Status::Ready) return status;
  }
  out = std::move(result);
  return Status::Ready;
}
} // namespace dxmt::ray_binding
