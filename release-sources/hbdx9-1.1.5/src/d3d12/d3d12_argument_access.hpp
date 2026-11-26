// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dxmt::root_argument {

enum Kind : uint32_t { Table, Constants, CBV, SRV, UAV };
struct Parameter {
  Kind kind = Table;
  uint32_t qword = 0, count = 0, heap_type = 0;
};
struct Layout {
  std::array<Parameter, 64> parameters{};
  uint32_t count = 0, qwords = 0;
};

// Global arguments use the existing DXMT qword packing, not local shader-record packing.
template <typename Desc>
bool Build(const Desc &desc, Layout &out) {
  out = {};
  if ((uint32_t(desc.Flags) & 0x80) || desc.NumParameters > 64 ||
      (desc.NumParameters && !desc.pParameters)) return false;
  Layout next;
  uint32_t cost = 0;
  for (uint32_t i = 0; i < desc.NumParameters; ++i) {
    const auto &p = desc.pParameters[i];
    if (uint32_t(p.ParameterType) > UAV) return false;
    auto &entry = next.parameters[i];
    entry.kind = Kind(p.ParameterType);
    entry.qword = next.qwords;
    uint32_t add = 2;
    if (entry.kind == Constants) {
      entry.count = p.Constants.Num32BitValues;
      if (entry.count > 64) return false;
      add = entry.count;
      next.qwords += (entry.count + 1) / 2;
    } else {
      ++next.qwords;
      if (entry.kind == Table) {
        add = 1;
        if (p.DescriptorTable.NumDescriptorRanges && !p.DescriptorTable.pDescriptorRanges) return false;
        uint32_t type = 2;
        for (uint32_t j = 0; j < p.DescriptorTable.NumDescriptorRanges; ++j) {
          const auto range = uint32_t(p.DescriptorTable.pDescriptorRanges[j].RangeType);
          if (range > 3) return false;
          const uint32_t heap = range == 3 ? 1 : 0;
          if (type != 2 && type != heap) return false;
          type = heap;
        }
        entry.heap_type = type;
      }
    }
    if (add > 64 - cost) return false;
    cost += add;
  }
  next.count = desc.NumParameters;
  out = next;
  return true;
}

struct State {
  std::array<uint64_t, 64> words{};
  std::array<uint64_t, 64> initialized{};

  bool WriteValue(const Layout &layout, uint32_t index, Kind kind, uint64_t value) {
    if (index >= layout.count || index >= 64 || kind == Constants) return false;
    const auto &p = layout.parameters[index];
    if (p.kind != kind || p.qword >= words.size()) return false;
    words[p.qword] = value;
    initialized[index] = 1;
    return true;
  }

  bool WriteConstants(const Layout &layout, uint32_t index, uint32_t count, const void *data, uint32_t offset) {
    if (index >= layout.count || index >= 64) return false;
    const auto &p = layout.parameters[index];
    if (p.kind != Constants || p.count > 64 || offset > p.count || count > p.count - offset ||
        p.qword > words.size() || p.count > (words.size() - p.qword) * 2 || (count && !data)) return false;
    if (!count) return true;
    std::memcpy(reinterpret_cast<unsigned char *>(words.data() + p.qword) + size_t(offset) * 4, data, size_t(count) * 4);
    const uint64_t mask = count == 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
    initialized[index] |= mask << offset;
    return true;
  }

  bool ReadValue(const Layout &layout, uint32_t index, Kind kind, uint64_t &value) const {
    value = 0;
    if (index >= layout.count || index >= 64 || kind == Constants) return false;
    const auto &p = layout.parameters[index];
    if (p.kind != kind || p.qword >= words.size() || initialized[index] != 1) return false;
    value = words[p.qword];
    return true;
  }

  bool ConstantsReady(const Layout &layout, uint32_t index) const {
    if (index >= layout.count || index >= 64) return false;
    const auto &p = layout.parameters[index];
    if (p.kind != Constants || !p.count || p.count > 64) return false;
    const uint64_t mask = p.count == 64 ? ~uint64_t(0) : (uint64_t(1) << p.count) - 1;
    return initialized[index] == mask;
  }

  void InvalidateTables(const Layout &layout) {
    for (uint32_t i = 0; i < layout.count && i < 64; ++i)
      if (layout.parameters[i].kind == Table) {
        initialized[i] = 0;
        if (layout.parameters[i].qword < words.size()) words[layout.parameters[i].qword] = 0;
      }
  }
};

struct HeapRange {
  uint64_t base = 0;
  uint32_t count = 0, stride = 0, type = 0;
  bool Valid() const {
    return base && count && stride && type <= 1 &&
           uint64_t(count) * stride - 1 <= std::numeric_limits<uint64_t>::max() - base;
  }
};
struct TableLocation {
  uint32_t index = 0;
  uint64_t address = 0;
};

inline bool Locate(const HeapRange &heap, uint64_t table, uint32_t offset, uint32_t count, TableLocation &out) {
  out = {};
  if (!heap.Valid() || table < heap.base || !count) return false;
  const uint64_t delta = table - heap.base;
  if (delta % heap.stride) return false;
  const uint64_t first = delta / heap.stride;
  if (first >= heap.count || offset >= heap.count - first) return false;
  const uint64_t index = first + offset;
  if (count > heap.count - index) return false;
  out.index = uint32_t(index);
  out.address = heap.base + index * heap.stride;
  return true;
}

inline bool ReadTable(const Layout &layout, const State &state, uint32_t parameter,
                      const HeapRange &heap, uint32_t offset, uint32_t count, TableLocation &out) {
  out = {};
  uint64_t table = 0;
  return state.ReadValue(layout, parameter, Table, table) &&
         layout.parameters[parameter].heap_type == heap.type && Locate(heap, table, offset, count, out);
}

// Caller holds the registry lock through retain(): ownership must precede unlocking.
template <typename Map, typename Retain>
auto AcquireByVA(const Map &intervals, uint64_t address, uint64_t length, uint64_t &offset, Retain retain)
    -> decltype(retain(intervals.begin()->second.allocation)) {
  offset = 0;
  auto it = intervals.upper_bound(address);
  if (it == intervals.begin()) return {};
  --it;
  const uint64_t delta = address - it->first;
  if (delta > it->second.logical_length || length > it->second.logical_length - delta) return {};
  auto result = retain(it->second.allocation);
  if (!result) return {};
  offset = delta;
  return result;
}

} // namespace dxmt::root_argument
