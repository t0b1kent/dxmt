#pragma once
/*
 * MacRunner HKGPU 03.11.2026: PixelFormatView policy for D3D11 textures (keys default 0).
 *
 *   MACRUNNER_DXMT_D11_PFV=1        a TYPELESS texture bound as render target / depth-stencil (DEFAULT usage, no UAV, not
 *                                   shared) is created WITHOUT MTLTextureUsagePixelFormatView, so the GPU may keep it
 *                                   losslessly compressed (light-tile 24.10, M1 Pro, Metal validation: PFV is what turns
 *                                   compression off; without PFV a view of the same format with any swizzle and an
 *                                   sRGB <-> linear view are legal, UNORM -> UINT and X32_Stencil8 are not).
 *                                   Every later view that would need PFV is reported (detector below). Measurement key:
 *                                   no fallback re-allocation yet, a reported view is a correctness defect of the run.
 *   MACRUNNER_DXMT_D11_PFV_TRACE=1  also print each texture created without PFV (first 64) and run the detector with
 *                                   the key at 0 (views of typed textures created internally with another format).
 *
 * Hollow Knight menu tape (record12, 300 frames): 3 texture views in the whole run, all depth-stencil views of their own
 * format with a swizzle; 20 attachments carry PFV (usage 21) only because Unity creates them TYPELESS.
 */
#include <cstdint>

namespace dxmt::pfv {

/* MTLPixelFormat values: the linear twin of an sRGB format (identity otherwise). */
constexpr uint32_t
srgb_base(uint32_t format) {
  switch (format) {
  case 11:  return 10;  // R8Unorm_sRGB
  case 31:  return 30;  // RG8Unorm_sRGB
  case 71:  return 70;  // RGBA8Unorm_sRGB
  case 81:  return 80;  // BGRA8Unorm_sRGB
  case 131: return 130; // BC1_RGBA_sRGB
  case 133: return 132; // BC2_RGBA_sRGB
  case 135: return 134; // BC3_RGBA_sRGB
  case 153: return 152; // BC7_RGBAUnorm_sRGB
  case 553: return 552; // BGRA10_XR_sRGB
  case 555: return 554; // BGR10_XR_sRGB
  default:  return format;
  }
}

/* A view of texture_format reinterpreted as view_format needs PixelFormatView (same format or sRGB twin: no). */
constexpr bool
view_needs_pfv(uint32_t texture_format, uint32_t view_format) {
  return srgb_base(texture_format) != srgb_base(view_format);
}

static_assert(!view_needs_pfv(70, 70) && !view_needs_pfv(70, 71) && !view_needs_pfv(71, 70) && !view_needs_pfv(80, 81));
static_assert(!view_needs_pfv(260, 260) && !view_needs_pfv(252, 252));
static_assert(view_needs_pfv(70, 73) && view_needs_pfv(260, 261) && view_needs_pfv(90, 91) && view_needs_pfv(70, 80));

} // namespace dxmt::pfv
