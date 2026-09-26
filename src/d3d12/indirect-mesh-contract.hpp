// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dxmt::indirect_mesh {
constexpr uint64_t kRecordsOffset = 64;
constexpr uint64_t kRecordBytes = 48;
constexpr uint64_t kDrawOffset = 16;
struct Parameters {
  uint64_t arguments;
  uint64_t count_buffer;
  uint64_t output;
  uint32_t max_count;
  uint32_t stride;
  uint32_t vertices_per_primitive;
  uint32_t primitives_per_group;
  uint32_t indexed;
  uint32_t reserved;
};
static_assert(sizeof(Parameters) == 48);
static_assert(offsetof(Parameters, max_count) == 24);
static_assert(offsetof(Parameters, indexed) == 40);
constexpr uint64_t StorageBytes(uint32_t count) {
  return kRecordsOffset + uint64_t(count) * kRecordBytes;
}
constexpr bool RangeFits(uint64_t size, uint64_t offset, uint32_t count,
                         uint32_t stride, uint32_t draw_bytes) {
  if (offset % 4 || stride % 4 || stride < draw_bytes || offset > size) return false;
  if (!count) return true;
  const uint64_t tail = uint64_t(count - 1) * stride + draw_bytes;
  return tail <= size - offset;
}
constexpr bool CountRangeFits(uint64_t size, uint64_t offset) {
  return offset % 4 == 0 && offset <= size && size - offset >= 4;
}
struct Grid { uint32_t groups, instances, depth, complete_vertices; };
constexpr Grid Resolve(uint32_t vertices, uint32_t instances, uint32_t per_primitive,
                       uint32_t per_group, bool enabled) {
  if (!enabled || !per_primitive || !per_group || !instances) return {};
  const uint32_t primitives = vertices / per_primitive;
  if (!primitives) return {};
  return {1 + (primitives - 1) / per_group, instances, 1, primitives * per_primitive};
}
}
