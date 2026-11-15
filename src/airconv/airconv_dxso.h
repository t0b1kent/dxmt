/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 (lane HBDX9, 07.11.2026): Direct3D 9 shader input for airconv.
 *
 * D3D9 SM1-3 token streams are decoded with DXVK's dxso decoder (src/airconv/dxso, zlib licence) and re-emitted as a
 * DXBC SM 5.0 container; the container goes through the unchanged airconv path (SM50Initialize -> SM50Compile ->
 * AIR/metallib), so the D3D9 path shares airconv's code generation, optimisation and caches with D3D11/D3D12.
 *
 * Fixed binding model of the emitted DXBC (the native D3D9 core binds accordingly):
 *   cb0[256] float4  c#  (def constants are NOT inlined: the core writes them into its copy of the bank, so relative
 *                         addressing sees them too)
 *   cb1[16]  int4    i#   (defi likewise)
 *   cb2[16]  uint4   b#.x (0 or 1; defb likewise)
 *   t#/s#            sampler s# -> texture t# + sampler s#, same index
 *   VS inputs        v# keep their register numbers; matched to the vertex declaration by (usage, usage index)
 *   VS->PS linkage   o0/v0 = SV_Position; interpolant slot k -> register k+1 (float4), slots from dxso_slot_of()
 *   PS outputs       oC# -> o#, oDepth -> oDepth
 *   PS cb3[20]       fixed-function state the D3D9 pipeline applies around a PS: [0].x alpha reference (0..255),
 *                    [1] fog colour, [2] (fog end, fog scale = 1/(end-start), fog density, 0),
 *                    [4+n] bump-env matrix of stage n (00, 01, 10, 11), [12+n] (luminance scale, offset, 0, 0)
 *   VS cb3[6]        user clip planes in clip space (the core transforms fixed-function world-space planes)
 *   VS clip output   o17 (planes 0-3), o18.xy (planes 4-5) as SV_ClipDistance; disabled planes write +1
 *   ps_1_x           r0 is the colour; t# are texture registers (1.0-1.3) or texcoord inputs (1.4); float constants
 *                    clamp to [-1, 1]; sampler n of a texture op is the destination register number (as DXVK)
 */
#ifndef AIRCONV_DXSO_H
#define AIRCONV_DXSO_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DXSO_MAX_SLOTS 16

enum dxso_stage { DXSO_STAGE_VERTEX = 0, DXSO_STAGE_PIXEL = 1 };
enum dxso_def_kind { DXSO_DEF_FLOAT = 0, DXSO_DEF_INT = 1, DXSO_DEF_BOOL = 2 };

struct dxso_io_elem {
  uint8_t usage;       /* D3DDECLUSAGE */
  uint8_t usage_index;
  uint8_t reg;         /* v# */
  uint8_t mask;        /* declared components */
};

struct dxso_def {
  uint16_t kind;       /* enum dxso_def_kind */
  uint16_t reg;
  uint32_t value[4];   /* raw bits */
};

struct dxso_info {
  uint32_t stage, major, minor;
  uint32_t input_count;
  struct dxso_io_elem inputs[16];   /* VS: vertex inputs */
  uint32_t slots_written;           /* VS: interpolant slots written (bit k = slot k) */
  uint32_t slots_read;              /* PS: interpolant slots read */
  uint32_t sampler_mask;            /* samplers used */
  uint8_t sampler_type[16];         /* D3DSAMPLER_TEXTURE_TYPE >> 27: 2 = 2D, 3 = cube, 4 = volume; 0 = undeclared */
  uint32_t rt_mask;                 /* PS: oC# written */
  uint32_t writes_depth;
  uint32_t max_const_f;             /* c# referenced + 1 (256 when relatively addressed) */
  uint32_t def_count;
  const struct dxso_def *defs;      /* valid while the result lives */
  uint32_t token_count;             /* dwords including the END token */
};

struct dxso_variant {
  uint32_t fill_slots;              /* VS: slots the pixel shader reads that this VS must write (defaults written) */
  uint32_t int_inputs;              /* VS: v# whose vertex format is integer (UBYTE4/SHORTn): converted with itof */
  uint32_t flags;                   /* PS: bits 0-3 alpha test D3DCMPFUNC (0 = off); reference = cb3[0].x in 0..255
                                       bits 4-5 fog: 0 off, 1 linear, 2 exp, 3 exp2 on the fog coordinate (slot 10 .x)
                                       bits 6-9 alpha test precision (upstream DXVK GetAlphaTestPrecision, by RT0 format):
                                       p <= 8: round_ne(sat(alpha) * ((256 << p) - 1)) against the integer reference
                                       (ref << p | ref >> (8 - p)), p = 0 for 8-bit targets; 0xF: float alpha against ref/255
                                       bits 16-31 ps_1_x sampler types, 2 bits per stage: 0 2D, 1 cube, 2 volume */
  uint32_t vs_flags;                /* VS: bits 0-1 fog coordinate source: 0 oFog (else oD1.w, else 0), 1 oPos.z,
                                       2 oPos.w; bits 8-13 user clip planes enabled */
  uint32_t depth_samplers;          /* PS (sm2/3): bits 0-15 sampler s is a depth texture compared against coord.z
                                       (D24S8/D16 "hardware shadow": sample_c, result replicated); bits 16-31 sampler
                                       s-16 is a raw depth fetch (INTZ: depth replicated to all components) */
};
#define DXSO_PS_FOG_SHIFT 4
#define DXSO_PS_ALPHA_PREC_SHIFT 6
#define DXSO_PS1_SAMPLER_SHIFT 16
#define DXSO_VS_CLIP_SHIFT 8
#define DXSO_SLOT_FOG 10

typedef struct dxso_result dxso_result;

/* Returns 0 on success. tokens: the D3D9 shader function; max_dwords bounds the scan. */
int dxso_translate(const uint32_t *tokens, size_t max_dwords, const struct dxso_variant *variant, dxso_result **out,
                   char *error, size_t error_size);
const void *dxso_result_dxbc(const dxso_result *r, size_t *size);
const struct dxso_info *dxso_result_info(const dxso_result *r);
void dxso_result_free(dxso_result *r);

/* Length of the token stream in dwords including END; 0 when malformed. */
size_t dxso_token_count(const uint32_t *tokens, size_t max_dwords);
/* Interpolant slot of a semantic (usage = D3DDECLUSAGE); ~0u when none is left. Stable for the process. */
uint32_t dxso_slot_of(uint32_t usage, uint32_t usage_index);

#ifdef __cplusplus
}
#endif
#endif
