// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

namespace dxmt::indirect_mesh {
template <class Command, class Stages>
constexpr void SetProducerConsumerDependency(Command &cmd, Stages producers, Stages consumers) {
  // WMT forwards these fields literally to Metal: after producers, before consumers.
  cmd.stages_after = producers;
  cmd.stages_before = consumers;
}
}
