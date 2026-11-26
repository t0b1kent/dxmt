// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <cstdint>

namespace dxmt {
// The outer object grid is distinct from each object's shader-declared mesh grid.
// Preserve the donor's conservative policy for experimental non-Apple GPUs.
constexpr uint64_t ObjectGridLimit(bool apple7) {
  return apple7 ? UINT64_MAX : uint64_t(1024);
}
constexpr bool ObjectGridFits(uint32_t groups, uint32_t instances, uint64_t limit) {
  return uint64_t(groups) * uint64_t(instances) <= limit;
}
}
