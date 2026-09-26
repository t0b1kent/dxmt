// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
namespace dxmt { inline constexpr auto kMeshResolver = R"MESH(#include <metal_stdlib>
using namespace metal;

struct MeshParameters {
  device const uchar *arguments;
  device const uint *count_buffer;
  device uint *output;
  uint max_count;
  uint stride;
  uint vertices_per_primitive;
  uint primitives_per_group;
  uint indexed;
  uint reserved;
};
static_assert(sizeof(MeshParameters) == 48, "CPU/Metal parameter ABI");

vertex void resolve_mesh_draws(constant MeshParameters &p [[buffer(30)]],
                              uint i [[vertex_id]]) {
  if (i >= p.max_count) return;
  device uint *out = p.output + ulong(i) * 12;
  for (uint j = 0; j < 12; ++j) out[j] = 0;
  const uint active = p.count_buffer ? min(*p.count_buffer, p.max_count) : p.max_count;
  // Inactive records must not read the GPU argument buffer.
  if (i >= active || !p.vertices_per_primitive || !p.primitives_per_group) return;
  device const uint *arg = reinterpret_cast<device const uint *>(p.arguments + ulong(i) * p.stride);
  const uint primitives = arg[0] / p.vertices_per_primitive;
  if (!primitives || !arg[1]) return;
  out[0] = 1 + (primitives - 1) / p.primitives_per_group;
  out[1] = arg[1];
  out[2] = 1;
  // Preserve start vertex/index, signed base vertex bits and base instance.
  for (uint j = 0; j < (p.indexed ? 5u : 4u); ++j) out[4 + j] = arg[j];
  out[4] = primitives * p.vertices_per_primitive;
}
)MESH"; }
