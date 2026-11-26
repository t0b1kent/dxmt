// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "../winemetal/winemetal.h"
#include <span>
namespace dxmt::compute_commands {
// All nodes are preallocated and owned by the submission/encoding scope.
inline void Link(WMTMemoryPointer &first, std::span<wmtcmd_compute_useresource> uses,
    std::span<wmtcmd_compute_setbuffer> buffers, std::span<wmtcmd_compute_settexture> textures,
    wmtcmd_compute_dispatch &launch,
    std::span<wmtcmd_compute_setaccelerationstructure> scenes = {}) {
  WMTMemoryPointer *tail = &first;
  for (auto &node : uses) { tail->set(&node); tail = &node.next; }
  for (auto &node : buffers) { tail->set(&node); tail = &node.next; }
  for (auto &node : textures) { tail->set(&node); tail = &node.next; }
  for (auto &node : scenes) { tail->set(&node); tail = &node.next; }
  tail->set(&launch);
  launch.next.set(nullptr);
}
}
