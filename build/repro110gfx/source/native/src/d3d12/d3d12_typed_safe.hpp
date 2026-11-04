#pragma once
// GPU-PASS 25.10.2026: MACRUNNER_DX12_TYPED_SAFE=1 (default 0; only with RENDER_FENCE=3). Mode 3 lets a render pass whose
// pre-raster stages (VS/HS/DS/GS) read only buffers wait the pre-raster fence before them (its vertex work overlaps the
// fragment work of the passes before it); a pass with a typed pre-raster read waits the common fence there. In Elden Ring at
// the church (run G1) six tessellation passes per frame wait that way and leave 95-427 us of GPU idle each (1.4 ms/frame),
// while the same kind of pass judged safe hides that work under its neighbour's fragments.
// Why typed reads are excluded: the pre-raster fence is updated by compute, blit and memory-writing render passes only, so
// the attachments written by the other render passes since the last update are not covered by it - and only a texture
// (an attachment) can be read typed there after being written that way. D3D12 requires a transition before an attachment
// can be read by a non-pixel stage; it is the only way such a texture becomes readable there.
// Key 1: a pass whose typed pre-raster reads are all SRVs (no UAV, no descriptor-heap indexing, no stream output; read from
// the application's bytecode) also gets the split wait when, since the last pre-raster fence update in this command buffer,
// no transition GAINED NON_PIXEL_SHADER_RESOURCE (and no aliasing or unrecorded barrier was seen). Conservative at the start
// of every command buffer (implicit promotion of simultaneous-access textures happens there) until the first update.
// FENCE_STATS counts, in either key, the typed passes that would get the split wait (`<c>.typed_free`) and those that are
// SRV-only but blocked by such a transition (`<c>.typed_exposed`). Read once per process.
// GetEnvironmentVariableA/GetLastError and the D3D12 barrier enums come from the includer (as in d3d12_render_fence.hpp).
#include <cstdint>

namespace dxmt::typed_safe {
inline bool
Enabled() {
  static const bool on = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool parsed = GetEnvironmentVariableA("MACRUNNER_DX12_TYPED_SAFE", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return parsed;
  }();
  return on;
}
// Does this recorded barrier make something readable by non-pixel stages that a render pass may have written as an
// attachment? Aliasing and unnamed records count as yes.
inline bool
Exposes(uint8_t type, obj_handle_t object, uint32_t before, uint32_t after) {
  constexpr uint32_t kNonPixel = 0x40;   // D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
  if (type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING || !object) return true;
  if (type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) return false;   // UAV barriers: UAV writers update the pre-raster fence
  return (after & kNonPixel) && !(before & kNonPixel);
}
} // namespace dxmt::typed_safe
