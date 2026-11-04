#pragma once
// Texture usage / storage policy (default off). Base: Astra-2 product-caps-texture 6c274ff (21.10.2026);
// LIGHT-TILE 24.10.2026: level-4 rules measured on M1 Pro with the Metal validation layer (stand/pfv_rules.mm):
// a view of the SAME format with any swizzle and an sRGB <-> linear view need no PixelFormatView; UNORM -> UINT and
// depth/stencil -> X32_Stencil8 do. GPU-only heaps (DEFAULT, or CUSTOM with CPU page NOT_AVAILABLE, which Elden Ring
// uses for its render targets) may hold Private textures.
//   MACRUNNER_DX12_TEXTURE_COMPRESSION=1  no PixelFormatView where no view of another format can be created: typed FLOAT
//                                         and BGRA families (Astra-2), and depth without a stencil plane (the layer
//                                         views D16/D32 only with their own format + swizzle)
//   MACRUNNER_DX12_TEXTURE_COMPRESSION=2  also typed (non-typeless) integer/norm formats, with the device reporting
//                                         CastingFullyTypedFormatSupported = FALSE (a typed resource then has views
//                                         of its own format only, by the D3D12 rules)
//   MACRUNNER_DX12_PRIVATE_TEXTURES=1     Private storage for textures of GPU-only heaps (committed, or placed in a
//                                         texture-only heap); Write/ReadFromSubresource on them are refused
//   MACRUNNER_TEXTURE_POLICY_TRACE=1      allocations (first 128) and every view that needs PixelFormatView on a
//                                         texture created without it (dx12_texture_policy_violation, first 64)
#include "d3d12.h"
#include "util_env.hpp"
#include <cstdlib>

namespace dxmt {
inline unsigned D3D12TextureCompressionMode() {
  static const unsigned mode = [] {
    const auto v = env::getEnvVar("MACRUNNER_DX12_TEXTURE_COMPRESSION");
    return v == "1" ? 1u : v == "2" ? 2u : 0u;
  }();
  return mode;
}
inline bool D3D12TextureCompressionEnabled() { return D3D12TextureCompressionMode() != 0; }
inline bool D3D12PrivateTexturesEnabled() {
  static const bool enabled = env::getEnvVar("MACRUNNER_DX12_PRIVATE_TEXTURES") == "1";
  return enabled;
}
inline bool D3D12GpuOnlyHeap(const D3D12_HEAP_PROPERTIES &props) {
  return props.Type == D3D12_HEAP_TYPE_DEFAULT ||
         (props.Type == D3D12_HEAP_TYPE_CUSTOM && props.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
}
inline bool D3D12PrivateTextureHeap(const D3D12_HEAP_PROPERTIES &props, D3D12_HEAP_FLAGS flags) {
  // Mixed heaps also allocate buffers. Keep their storage mode consistent with
  // the existing buffer allocator; CPU-visible/custom/shared heaps stay shared.
  return D3D12PrivateTexturesEnabled() && D3D12GpuOnlyHeap(props) &&
         (flags & D3D12_HEAP_FLAG_DENY_BUFFERS) &&
         !(flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER));
}
inline bool D3D12PrivateCommittedTexture(const D3D12_HEAP_PROPERTIES &props, D3D12_HEAP_FLAGS flags) {
  return D3D12PrivateTexturesEnabled() && D3D12GpuOnlyHeap(props) &&
         !(flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER));
}
}
