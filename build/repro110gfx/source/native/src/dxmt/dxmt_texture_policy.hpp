#pragma once
#include "dxmt_format.hpp"
#include "dxgiformat.h"
#include "util_env.hpp"
#include <atomic>
#include <cstdio>

namespace dxmt {
// View formats are not declared at resource creation. Be conservative for
// families allowing integer/norm casts: M1 Metal validation requires the flag
// even for RGBA8Unorm -> RGBA8Uint. Never replace a live texture on a late view:
// recorded commands and descriptors may already refer to its allocation.
inline bool TextureNeedsLayoutViews(unsigned format, unsigned format_flags, bool depth, bool uav) {
  if (depth || uav || (format_flags & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER |
                                     MTL_DXGI_FORMAT_STENCIL_PLANER)))
    return true;
  // BGRA/X families only have UNORM/sRGB views (and swizzles). Typed FLOAT
  // resources cannot be cast to non-FLOAT under the advertised RS2 rules.
  switch (format) {
  case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    return false;
  default:
    return true;
  }
}

// LIGHT-TILE (24.10.2026), level-4 rules (stand/pfv_rules.mm, M1 Pro, Metal validation layer): a view needs
// PixelFormatView only when its format differs from the texture's other than by sRGB-ness; a swizzle alone does not.
//  mode 1: the conservative list above, and depth WITHOUT a stencil plane (D16 / D32: the D3D12 layer gives every
//          SRV and DSV of them the texture's own format with a swizzle), unless UAV / BC.
//  mode 2: additionally every TYPED (non-typeless) integer/norm colour format: the device then reports
//          CastingFullyTypedFormatSupported = FALSE, so a typed resource has views of its own format (or none) only.
inline bool TextureNeedsLayoutViews2(unsigned format, unsigned format_flags, unsigned metal_format, bool depth, bool uav,
                                     unsigned mode) {
  if (uav || (format_flags & MTL_DXGI_FORMAT_BC))
    return true;
  const bool stencil_plane = metal_format == WMTPixelFormatDepth32Float_Stencil8 || metal_format == WMTPixelFormatDepth24Unorm_Stencil8 ||
                             metal_format == WMTPixelFormatStencil8 || metal_format == WMTPixelFormatX32_Stencil8 ||
                             metal_format == WMTPixelFormatX24_Stencil8;
  if (depth || (format_flags & (MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER)))
    return stencil_plane || !(metal_format == WMTPixelFormatDepth16Unorm || metal_format == WMTPixelFormatDepth32Float);
  if (!TextureNeedsLayoutViews(format, format_flags, false, false))
    return false;
  if (mode >= 2 && !(format_flags & MTL_DXGI_FORMAT_TYPELESS))
    return false;
  return true;
}

// Does a view of `view_format` on a texture of `texture_format` need PixelFormatView? (custom swizzle bits ignored)
inline bool ViewNeedsPixelFormatView(unsigned texture_format, unsigned view_format) {
  const unsigned a = ORIGINAL_FORMAT(texture_format), b = ORIGINAL_FORMAT(view_format);
  if (a == b) return false;
  const auto linear = [](unsigned f) -> unsigned {
    switch (f) {
    case WMTPixelFormatRGBA8Unorm_sRGB: return WMTPixelFormatRGBA8Unorm;
    case WMTPixelFormatBGRA8Unorm_sRGB: return WMTPixelFormatBGRA8Unorm;
    case WMTPixelFormatR8Unorm_sRGB: return WMTPixelFormatR8Unorm;
    case WMTPixelFormatRG8Unorm_sRGB: return WMTPixelFormatRG8Unorm;
    case WMTPixelFormatBC1_RGBA_sRGB: return WMTPixelFormatBC1_RGBA;
    case WMTPixelFormatBC2_RGBA_sRGB: return WMTPixelFormatBC2_RGBA;
    case WMTPixelFormatBC3_RGBA_sRGB: return WMTPixelFormatBC3_RGBA;
    case WMTPixelFormatBC7_RGBAUnorm_sRGB: return WMTPixelFormatBC7_RGBAUnorm;
    default: return f;
    }
  };
  return linear(a) != linear(b);
}

inline void TraceTexturePolicy(const char *api, unsigned format, const WMTTextureInfo &info, bool placed) {
  static const bool trace = env::getEnvVar("MACRUNNER_TEXTURE_POLICY_TRACE") == "1";
  static std::atomic_uint count{0};
  if (trace && count.fetch_add(1, std::memory_order_relaxed) < 128)
    fprintf(stderr, "texture_policy api=%s format=%u usage=%u options=%u placed=%u\n",
            api, format, unsigned(info.usage), unsigned(info.options), unsigned(placed));
}
}
