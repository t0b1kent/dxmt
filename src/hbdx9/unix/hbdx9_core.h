/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 native core — shared declarations (lane HBDX9, 07.11.2026).
 * D3D9 enum values are repeated here (the native side cannot include the Windows d3d9.h). */
#pragma once
#include <stdint.h>
#include <vector>
#include "../hbdx9_protocol.h"

namespace hb {

/* D3DFORMAT */
enum : uint32_t {
  FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24, FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26,
  FMT_A8 = 28, FMT_A2B10G10R10 = 31, FMT_A8B8G8R8 = 32, FMT_X8B8G8R8 = 33, FMT_G16R16 = 34, FMT_A2R10G10B10 = 35,
  FMT_A16B16G16R16 = 36, FMT_L8 = 50, FMT_A8L8 = 51, FMT_D32 = 71, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D16 = 80,
  FMT_L16 = 81, FMT_V8U8 = 60, FMT_Q8W8V8U8 = 63, FMT_V16U16 = 64, FMT_D32F_LOCKABLE = 82, FMT_D24FS8 = 83, FMT_VERTEXDATA = 100, FMT_INDEX16 = 101, FMT_INDEX32 = 102,
  FMT_R16F = 111, FMT_G16R16F = 112, FMT_A16B16G16R16F = 113, FMT_R32F = 114, FMT_G32R32F = 115, FMT_A32B32G32R32F = 116,
  FMT_DXT1 = 0x31545844, FMT_DXT2 = 0x32545844, FMT_DXT3 = 0x33545844, FMT_DXT4 = 0x34545844, FMT_DXT5 = 0x35545844,
  FMT_INTZ = 0x5A544E49, FMT_NULL = 0x4C4C554E,
  FMT_ATI1 = 0x31495441, FMT_ATI2 = 0x32495441,   /* (CRYSIS 12.11.2026) 3Dc */
  /* (DX9FULL 13.11.2026) vendor depth textures and the remaining colour formats DXVK maps directly */
  FMT_DF16 = 0x36314644, FMT_DF24 = 0x34324644, FMT_X4R4G4B4 = 30, FMT_Q16W16V16U16 = 110,
  /* (DX9FULL) formats Metal has no direct equivalent for: converted on upload (DXVK d3d9_convert_*.comp) */
  FMT_A4L4 = 52, FMT_L6V5U5 = 61, FMT_X8L8V8U8 = 62, FMT_A2W10V10U10 = 67,
  FMT_UYVY = 0x59565955, FMT_YUY2 = 0x32595559, FMT_R8G8_B8G8 = 0x47424752, FMT_G8R8_G8B8 = 0x42475247,
};
/* (DX9FULL) vendor render-state / sampler-state magic values (DXVK d3d9_device.cpp) */
enum : uint32_t {
  HACK_NVDB = 0x4244564E, HACK_ATOC = 0x434F5441, HACK_GET4 = 0x34544547, HACK_GET1 = 0x31544547,
};
#define DXSO_VS_POINT_SHIFT 16    /* (DX9FULL) VS variant vs_flags: bit 16 point expansion, bit 17 point sprite texcoords (dxso) */
#define DXSO_PS_FETCH4_SHIFT 16   /* (DX9FULL) PS sm2/3 variant flags bits 16-31: Fetch4 per sampler (ps_1_x: sampler types) */

/* D3DRENDERSTATETYPE (subset) */
enum : uint32_t {
  RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19,
  RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_ALPHABLENDENABLE = 27,
  RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37,
  RS_FOGDENSITY = 38, RS_RANGEFOGENABLE = 48, RS_STENCILENABLE = 52, RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54,
  RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_STENCILREF = 57, RS_STENCILMASK = 58, RS_STENCILWRITEMASK = 59,
  RS_TEXTUREFACTOR = 60, RS_CLIPPING = 136, RS_LIGHTING = 137, RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140,
  RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142, RS_NORMALIZENORMALS = 143, RS_DIFFUSEMATERIALSOURCE = 145,
  RS_SPECULARMATERIALSOURCE = 146, RS_AMBIENTMATERIALSOURCE = 147, RS_EMISSIVEMATERIALSOURCE = 148,
  RS_CLIPPLANEENABLE = 152, RS_MULTISAMPLEANTIALIAS = 161, RS_SCISSORTESTENABLE = 174,
  RS_SLOPESCALEDEPTHBIAS = 175, RS_COLORWRITEENABLE = 168, RS_BLENDOP = 171, RS_TWOSIDEDSTENCILMODE = 185,
  RS_CCW_STENCILFAIL = 186, RS_CCW_STENCILZFAIL = 187, RS_CCW_STENCILPASS = 188, RS_CCW_STENCILFUNC = 189,
  RS_COLORWRITEENABLE1 = 190, RS_COLORWRITEENABLE2 = 191, RS_COLORWRITEENABLE3 = 192, RS_BLENDFACTOR = 193,
  RS_SRGBWRITEENABLE = 194, RS_DEPTHBIAS = 195, RS_SEPARATEALPHABLENDENABLE = 206, RS_SRCBLENDALPHA = 207,
  RS_DESTBLENDALPHA = 208, RS_BLENDOPALPHA = 209,
  RS_POINTSIZE = 154, RS_MULTISAMPLEMASK = 162,                       /* (DX9FULL) */
  RS_POINTSIZE_MIN = 155, RS_POINTSPRITEENABLE = 156, RS_POINTSCALEENABLE = 157, RS_POINTSCALE_A = 158, RS_POINTSCALE_B = 159,
  RS_POINTSCALE_C = 160, RS_POINTSIZE_MAX = 166, RS_VERTEXBLEND = 151, RS_INDEXEDVERTEXBLENDENABLE = 167,
  RS_ADAPTIVETESS_X = 180, RS_ADAPTIVETESS_Y = 181, RS_ADAPTIVETESS_Z = 182, RS_ADAPTIVETESS_W = 183,
};

/* D3DSAMPLERSTATETYPE */
enum : uint32_t {
  SAMP_ADDRESSU = 1, SAMP_ADDRESSV = 2, SAMP_ADDRESSW = 3, SAMP_BORDERCOLOR = 4, SAMP_MAGFILTER = 5, SAMP_MINFILTER = 6,
  SAMP_MIPFILTER = 7, SAMP_MIPMAPLODBIAS = 8, SAMP_MAXMIPLEVEL = 9, SAMP_MAXANISOTROPY = 10, SAMP_SRGBTEXTURE = 11,
};

/* D3DTEXTURESTAGESTATETYPE */
enum : uint32_t {
  TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
  TSS_BUMPENVMAT00 = 7, TSS_BUMPENVMAT01 = 8, TSS_BUMPENVMAT10 = 9, TSS_BUMPENVMAT11 = 10,
  TSS_TEXCOORDINDEX = 11, TSS_BUMPENVLSCALE = 22, TSS_BUMPENVLOFFSET = 23, TSS_TEXTURETRANSFORMFLAGS = 24, TSS_COLORARG0 = 26, TSS_ALPHAARG0 = 27, TSS_RESULTARG = 28, TSS_CONSTANT = 32,
};

/* D3DFVF */
enum : uint32_t {
  FVF_POSITION_MASK = 0x400E, FVF_XYZ = 0x002, FVF_XYZRHW = 0x004, FVF_XYZB1 = 0x006, FVF_XYZB2 = 0x008,
  FVF_XYZB3 = 0x00a, FVF_XYZB4 = 0x00c, FVF_XYZB5 = 0x00e, FVF_XYZW = 0x4002, FVF_NORMAL = 0x010, FVF_PSIZE = 0x020,
  FVF_DIFFUSE = 0x040, FVF_SPECULAR = 0x080, FVF_TEXCOUNT_MASK = 0xf00, FVF_TEXCOUNT_SHIFT = 8,
  FVF_LASTBETA_UBYTE4 = 0x1000, FVF_LASTBETA_D3DCOLOR = 0x8000,
};

/* D3DDECLTYPE / D3DDECLUSAGE */
enum : uint8_t {
  DT_FLOAT1 = 0, DT_FLOAT2, DT_FLOAT3, DT_FLOAT4, DT_D3DCOLOR, DT_UBYTE4, DT_SHORT2, DT_SHORT4, DT_UBYTE4N, DT_SHORT2N,
  DT_SHORT4N, DT_USHORT2N, DT_USHORT4N, DT_UDEC3, DT_DEC3N, DT_FLOAT16_2, DT_FLOAT16_4, DT_UNUSED = 17,
};
enum : uint8_t {
  DU_POSITION = 0, DU_BLENDWEIGHT, DU_BLENDINDICES, DU_NORMAL, DU_PSIZE, DU_TEXCOORD, DU_TANGENT, DU_BINORMAL,
  DU_TESSFACTOR, DU_POSITIONT, DU_COLOR, DU_FOG, DU_DEPTH, DU_SAMPLE,
};

/* D3DPRIMITIVETYPE */
enum : uint32_t { PT_POINTLIST = 1, PT_LINELIST = 2, PT_LINESTRIP = 3, PT_TRIANGLELIST = 4, PT_TRIANGLESTRIP = 5, PT_TRIANGLEFAN = 6 };

/* D3DTRANSFORMSTATETYPE */
enum : uint32_t { TS_VIEW = 2, TS_PROJECTION = 3, TS_TEXTURE0 = 16, TS_WORLD = 256, TS_WORLD1 = 257, TS_WORLD3 = 259 };

std::vector<hbdx9_vertex_element> fvf_to_elements(uint32_t fvf);
uint32_t element_size(uint8_t type);

/* Fixed-function shaders as D3D9 token streams (vs_3_0 / ps_3_0), translated by the same dxso path. */
enum : uint8_t { FFP_FOG_NONE = 0, FFP_FOG_EYE_Z = 2, FFP_FOG_RAW_Z = 3, FFP_FOG_RANGE = 4, FFP_FOG_SPECULAR_W = 5 };
struct FfpVsKey {
  uint8_t pretransformed, has_color0, has_color1, texcoords;  /* texcoords: count of TEXCOORD0..n-1 in the vertex */
  uint8_t has_normal, lighting, normalize, specular;          /* lighting as wined3d's GLSL FFP (non-legacy) */
  uint8_t localviewer, fog_coord, diffuse_src, ambient_src;   /* fog_coord: FFP_FOG_*; *_src: D3DMATERIALCOLORSOURCE */
  uint8_t specular_src, emissive_src, light_count, point;   /* point (DX9FULL): bit 0 output PSIZE, bit 1 from the vertex, bit 2 POINTSCALE */
  uint8_t light_type[8];                                      /* D3DLIGHTTYPE of active light i */
  uint8_t tc_src[8];     /* stage i output: low 3 bits input TEXCOORDn, high nibble texgen (TCI >> 16): 1 camera normal,
                            2 camera position, 3 reflection vector, 4 sphere map; 0xFF = no output */
  uint8_t tc_xform[8];   /* stage i coordinate multiplied by its texture matrix (c80 + 4i, columns) */
  uint8_t blend, pad2[3];   /* (DX9FULL) D3DRS_VERTEXBLEND: number of world matrices blended (2-4), 0 = none */
  bool operator==(const FfpVsKey &o) const { return !__builtin_memcmp(this, &o, sizeof(*this)); }
};
struct FfpStage {
  uint8_t colorop, colorarg1, colorarg2, alphaop, alphaarg1, alphaarg2, texcoord, has_texture; /* has_texture: 1 2D, 2 cube, 3 volume */
  uint8_t projected;     /* D3DTTFF_PROJECTED: sample with the coordinate divided by w (texldp) */
  uint8_t colorarg0, alphaarg0, resultarg;   /* (DX9FULL, ext only) ARG0 of MULTIPLYADD / LERP (else 0); RESULTARG 5 = TEMP, else 0 */
};
struct FfpPsKey {
  FfpStage stage[8];
  uint8_t stages, specular_add, ext, pad;   /* ext: (DX9FULL) HBDX9_FMT_EXT - the stage ops below the s8j set; 0 = s8j tokens */
  bool operator==(const FfpPsKey &o) const { return !__builtin_memcmp(this, &o, sizeof(*this)); }
};
/* (DX9FULL) FFP PS constants beyond c0..c2 (only when the key needs them): c3+i D3DTSS_CONSTANT of stage i, c11+i bump
   matrix of stage i as (M00, M10, M01, M11), c19+i (BUMPENVLSCALE, BUMPENVLOFFSET, 0, 0) */
enum : uint32_t { FFP_PS_CONST_BASE = 3, FFP_PS_BUMPMAT_BASE = 11, FFP_PS_BUMPL_BASE = 19, FFP_PS_BANK_EXT = 27 };
/* FFP VS constant layout: c0 = viewport scale/offset for XYZRHW, c1 = depth scale/offset, c2 = default diffuse,
 * c3 = default specular, c4..c7 = (world*view*projection) columns, c8..c11 = (world*view) columns,
 * c12..c14 = rows of inverse(world*view 3x3) (normal matrix columns), c15 material diffuse, c16 ambient, c17 specular,
 * c18 emissive, c19.x power, c20 global ambient, c21 = (0, 1, 0.5, 0) (def), light i at c24 + 7 i: diffuse, specular,
 * ambient, eye position, eye direction (directional: towards the light), (range, att0, att1, att2),
 * (cos(theta/2), cos(phi/2), falloff, 0).  FFP PS: c0 = texture factor. */
enum : uint32_t { FFP_VS_BANK = 112, FFP_LIGHT_BASE = 24, FFP_TEXMAT_BASE = 80 };
/* (DX9FULL) vertex blending (only when the key blends; the bank is then 144 registers): c112 + 4i columns of world_i * view,
   c128 + 3i rows of inverse(world_i * view) 3x3, c140..c143 projection columns */
enum : uint32_t { FFP_VS_BANK_BLEND = 144, FFP_BLEND_WV = 112, FFP_BLEND_N = 128, FFP_BLEND_PROJ = 140 };
/* (DX9FULL) FFP VS point size: c22 = (POINTSIZE, POINTSCALE_A, _B, _C), c23 = (viewport height, 0, 0, 0) */
enum : uint32_t { FFP_VS_POINT = 22 };
std::vector<uint32_t> ffp_vs_tokens(const FfpVsKey &k);
std::vector<uint32_t> ffp_ps_tokens(const FfpPsKey &k);

} // namespace hb
