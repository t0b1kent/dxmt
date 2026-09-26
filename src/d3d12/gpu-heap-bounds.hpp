// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <cstddef>

namespace dxmt {
struct GPUHeapExhausted {
  size_t used, requested, alignment, capacity;
};
struct GPUHeapPlan { bool valid; size_t offset, end; };
constexpr GPUHeapPlan PlanGPUHeap(size_t used, size_t length, size_t alignment, size_t capacity) {
  if (!length) return {true, 0, used};
  if (!alignment || (alignment & (alignment - 1)) || used > capacity) return {};
  const size_t adjustment = (alignment - (used & (alignment - 1))) & (alignment - 1);
  if (adjustment > capacity - used) return {};
  const size_t offset = used + adjustment;
  if (length > capacity - offset) return {};
  return {true, offset, offset + length};
}
inline size_t ReserveGPUHeap(size_t &used, size_t length, size_t alignment, size_t capacity) {
  const auto plan = PlanGPUHeap(used, length, alignment, capacity);
  if (!plan.valid) throw GPUHeapExhausted{used, length, alignment, capacity};
  used = plan.end;
  return plan.offset;
}
}
