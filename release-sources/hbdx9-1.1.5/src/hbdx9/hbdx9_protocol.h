/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 (HBDX9) — boundary protocol between the thin guest d3d9.dll shim (i386/x86_64 PE, translated)
 * and the native ARM64 core (hbdx9.so, Wine unix library).  07.11.2026, lane HBDX9.  Version 3 (07.11 session 3: gaps for Gothic-class games).
 *
 * Rules: every structure has the same layout for an i386 guest and the 64-bit host (fixed-size fields, explicit
 * padding, guest pointers carried as uint64_t), so __wine_unix_call_funcs and __wine_unix_call_wow64_funcs are the
 * same functions.  State changes, uploads and draws are NOT unix calls: the shim appends packets to a buffer in guest
 * memory and hands the whole buffer over with one HBDX9_CALL_SUBMIT (at Present, before a synchronous call, or when
 * full).  Object creation is synchronous (it must be able to fail and returns the native handle). */
#ifndef HBDX9_PROTOCOL_H
#define HBDX9_PROTOCOL_H
#include <stdint.h>

#define HBDX9_PROTOCOL_VERSION 3

enum hbdx9_call {
  HBDX9_CALL_INIT = 0,           /* create the native device; adapter identity out */
  HBDX9_CALL_SWAPCHAIN_CREATE,   /* bind a CAMetalLayer to the HWND, create the back buffer (+ auto depth) */
  HBDX9_CALL_SUBMIT,             /* execute a packet buffer in order */
  HBDX9_CALL_READBACK,           /* synchronous: copy a native surface into guest memory */
  HBDX9_CALL_RELEASE,            /* release a native swapchain/device (objects go through HBDX9_PKT_RELEASE) */
  HBDX9_CALL_CREATE_RESOURCE,    /* buffer / texture / surface */
  HBDX9_CALL_CREATE_SHADER,      /* copy + validate a D3D9 token stream */
  HBDX9_CALL_STATS,              /* counters (cache hits/misses, draws) for probes */
  HBDX9_CALL_FEATURES,           /* (DX9FULL, HBDX9_CAP_FEATURES) measured GPU features, e.g. whether depth bounds work */
  HBDX9_CALL_COUNT
};

struct hbdx9_init_params {
  uint32_t version;       /* in: HBDX9_PROTOCOL_VERSION */
  uint32_t status;        /* out: 0 = ok */
  uint64_t device;        /* out: native device handle */
  uint64_t vram_bytes;    /* out */
  uint32_t vendor_id;     /* out */
  uint32_t device_id;     /* out */
  char name[128];         /* out: adapter description, UTF-8 */
  char exe_name[64];      /* in: guest executable file name (shader cache location, as DXMT: dxmt/<exe>/) */
  uint64_t fence_page;    /* in: guest address of 4096 bytes the core writes fence values into (completion handler) */
  uint32_t max_samples;   /* out: highest supported MSAA sample count */
  uint32_t caps;          /* out: HBDX9_CAP_* (0 from a core before INT32G patch 0004) */
};

struct hbdx9_swapchain_params {
  uint64_t device;        /* in */
  uint64_t hwnd;          /* in: guest HWND value; 0 = offscreen (native stand) */
  uint32_t width, height; /* in: back buffer size */
  uint32_t format;        /* in: D3DFORMAT of the back buffer */
  uint32_t depth_format;  /* in: D3DFORMAT of the automatic depth buffer, 0 = none */
  uint64_t swapchain;     /* out: native swapchain handle */
  uint64_t backbuffer;    /* out: native resource handle of back buffer 0 */
  uint64_t depth;         /* out: native resource handle of the automatic depth buffer, 0 = none */
  uint32_t status;        /* out */
  uint32_t samples;       /* in: MSAA sample count of the back buffer (0/1 = none) */
  uint32_t interval;      /* in: D3DPRESENT_INTERVAL_* (IMMEDIATE = no display sync) */
  uint32_t pace_mhz;      /* in (DX9FULL, was pad = 0): frame limiter, frames per second x 1000, 0 = none.  Bit 31 set = always
                             (HBDX9_MAX_FRAME_RATE); clear = only while faster than the display (a full-screen mode's refresh rate
                             under vsync, which the Mac display does not switch to) */
};

struct hbdx9_submit_params {
  uint64_t device;        /* in */
  uint64_t data;          /* in: guest address of the packet buffer */
  uint32_t size;          /* in: bytes */
  uint32_t status;        /* out: 0 = ok, else index+1 of the first bad packet; in: HBDX9_SUBMIT_* flags (0 = none) */
};

/* (INT32G patch 0004) what the core can do beyond protocol 3, in hbdx9_init_params.caps */
#define HBDX9_CAP_SUBMIT_NOCOPY   0x1u         /* understands HBDX9_SUBMIT_NOCOPY and HBDX9_SUBMIT_HAS_PRESENTS */
#define HBDX9_CAP_RING            0x2u         /* understands HBDX9_SUBMIT_RING_* */
#define HBDX9_CAP_TIMESTAMP       0x4u         /* (DX9FULL) understands HBDX9_PKT_TIMESTAMP */
#define HBDX9_CAP_FEATURES        0x8u         /* (DX9FULL) has HBDX9_CALL_FEATURES */
#define HBDX9_CAP_TEXTURE_LOD     0x10u        /* (DX9FULL v12) understands HBDX9_PKT_TEXTURE_LOD and the PRESENT force-immediate bit */
/* flags of hbdx9_submit_params.status on entry; only if the core announced the capability */
#define HBDX9_SUBMIT_NOCOPY       0x80000000u  /* the guest leaves the packet buffer alone until this SUBMIT's sequence is published */
#define HBDX9_SUBMIT_HAS_PRESENTS 0x40000000u  /* bits 0..7 = number of PRESENT packets in the buffer */
#define HBDX9_SUBMIT_RING_ATTACH  0x20000000u  /* not a submit: data = guest address of struct hbdx9_ring (0 = detach), size = its size */
#define HBDX9_SUBMIT_RING_WAKE    0x10000000u  /* not a submit: the ring has work and the worker said it sleeps */
#define HBDX9_SUBMIT_RING_WAIT    0x08000000u  /* not a submit: block; bits 0..7: 1 until the ring has a free slot, 2 until at most two
                                                  presented frames are ahead of the worker (size = presents published), 3 until idle */
/* the chunk ring: one producer (the device's calling thread, under the shim's device lock), one consumer (the core's worker).
   A slot is valid from the moment `head` moves past it until `tail` moves past it; the buffer it names is not touched by the
   guest in between.  `head` and `tail` count chunks and wrap as 32-bit numbers. */
#define HBDX9_RING_MAGIC 0x52444248u           /* "HBDR" */
#define HBDX9_RING_SLOTS 1024u
struct hbdx9_ring_slot { uint64_t data; uint32_t size; uint32_t presents; };
struct hbdx9_ring {
  uint32_t magic, slots;                       /* set by the shim before the attach */
  uint32_t pad0[14];
  volatile uint32_t head;                      /* producer: chunks published */
  uint32_t pad1[15];
  volatile uint32_t tail;                      /* consumer: chunks run, their sequence published */
  volatile uint32_t sleeping;                  /* consumer: 1 while it parks or is about to */
  volatile uint32_t presents_done;             /* consumer: PRESENT packets of ring chunks run */
  uint32_t pad2[13];
  struct hbdx9_ring_slot slot[HBDX9_RING_SLOTS];
};

struct hbdx9_readback_params {
  uint64_t device;        /* in */
  uint64_t surface;       /* in: native resource handle */
  uint64_t dst;           /* in: guest address */
  uint32_t pitch;         /* in: guest row pitch in bytes */
  uint32_t width, height; /* in */
  uint32_t level;         /* in: mip level of a texture */
  uint32_t status;        /* out */
  uint32_t pad;
};

struct hbdx9_release_params {
  uint64_t object;        /* in */
  uint32_t kind;          /* in: 1 swapchain, 2 device */
  uint32_t status;        /* out */
};

enum hbdx9_resource_kind {
  HBDX9_RES_BUFFER = 1,       /* vertex or index buffer */
  HBDX9_RES_TEXTURE_2D = 2,
  HBDX9_RES_TEXTURE_CUBE = 3,
  HBDX9_RES_TEXTURE_3D = 4,
  HBDX9_RES_SURFACE_RT = 5,   /* CreateRenderTarget */
  HBDX9_RES_SURFACE_DS = 6,   /* CreateDepthStencilSurface */
};

struct hbdx9_create_resource_params {
  uint64_t device;        /* in */
  uint32_t kind;          /* in: hbdx9_resource_kind */
  uint32_t format;        /* in: D3DFORMAT (buffers: D3DFMT_INDEX16/32 or D3DFMT_VERTEXDATA) */
  uint32_t width, height, depth, levels; /* in (levels 0 = full chain); buffers: width = bytes */
  uint32_t usage;         /* in: D3DUSAGE_* */
  uint32_t pool;          /* in: D3DPOOL */
  uint64_t resource;      /* out */
  uint32_t out_levels;    /* out */
  uint32_t status;        /* out: 0 ok, 1 unsupported format, 2 out of memory */
  uint32_t samples;       /* in: MSAA sample count for render targets / depth surfaces (0/1 = none) */
  uint32_t pad;
};

struct hbdx9_stats_params {
  uint64_t device;        /* in */
  uint32_t variants_compiled, variants_cached, psos_created, draws; /* out */
  uint32_t cache_rows_written, cache_ready, pad[2];
};

/* (DX9FULL) HBDX9_CALL_FEATURES: query 1 = the GPU honours -[MTLRenderCommandEncoder setDepthTestMinBound:maxBound:]
   (measured once per device by a 4x1 offscreen draw: the API exists from macOS 26, an Apple7 GPU ignores it) */
struct hbdx9_features_params {
  uint64_t device;        /* in */
  uint32_t query;         /* in */
  uint32_t result;        /* out: 1 yes, 0 no */
};

struct hbdx9_create_shader_params {
  uint64_t device;        /* in */
  uint64_t tokens;        /* in: guest address of the token stream */
  uint32_t max_dwords;    /* in: scan bound */
  uint32_t stage;         /* in: 0 vertex, 1 pixel (checked against the version token) */
  uint64_t shader;        /* out */
  uint32_t token_count;   /* out: dwords including END */
  uint32_t status;        /* out: 0 ok */
};

/* ---- packets inside a SUBMIT buffer (each starts with this header; size includes it, multiple of 4) ---- */
enum hbdx9_packet_type {
  HBDX9_PKT_CLEAR = 1,
  HBDX9_PKT_PRESENT = 2,
  HBDX9_PKT_RENDER_STATE = 3,
  HBDX9_PKT_SAMPLER_STATE = 4,
  HBDX9_PKT_TEXTURE_STAGE_STATE = 5,
  HBDX9_PKT_TEXTURE = 6,
  HBDX9_PKT_STREAM_SOURCE = 7,
  HBDX9_PKT_INDICES = 8,
  HBDX9_PKT_SHADER = 9,
  HBDX9_PKT_CONSTANTS = 10,
  HBDX9_PKT_VIEWPORT = 11,
  HBDX9_PKT_SCISSOR = 12,
  HBDX9_PKT_RENDER_TARGET = 13,
  HBDX9_PKT_DEPTH_STENCIL = 14,
  HBDX9_PKT_FVF = 15,
  HBDX9_PKT_VERTEX_DECL = 16,
  HBDX9_PKT_DRAW = 17,
  HBDX9_PKT_DRAW_INDEXED = 18,
  HBDX9_PKT_DRAW_UP = 19,
  HBDX9_PKT_DRAW_INDEXED_UP = 20,
  HBDX9_PKT_UPDATE_BUFFER = 21,
  HBDX9_PKT_UPDATE_TEXTURE = 22,
  HBDX9_PKT_RELEASE_OBJECT = 23,
  HBDX9_PKT_TRANSFORM = 24,
  HBDX9_PKT_STREAM_FREQ = 25,
  HBDX9_PKT_LIGHTS = 26,        /* the active light set (<= 8), replaces the previous one */
  HBDX9_PKT_MATERIAL = 27,
  HBDX9_PKT_CLIP_PLANE = 28,
  HBDX9_PKT_BLIT = 29,          /* StretchRect */
  HBDX9_PKT_COLOR_FILL = 30,
  HBDX9_PKT_FENCE = 31,         /* write value into fence_page slot when the GPU work recorded so far completes */
  HBDX9_PKT_FLUSH = 32,         /* commit the open command buffer without waiting */
  HBDX9_PKT_QUERY = 33,         /* occlusion query begin/end */
  HBDX9_PKT_GENERATE_MIPS = 34,
  HBDX9_PKT_RESET_STATE = 35,   /* IDirect3DDevice9::Reset: default states, nothing bound except the new back buffer */
  HBDX9_PKT_TIMESTAMP = 36,     /* (DX9FULL, HBDX9_CAP_TIMESTAMP) hbdx9_pkt_fence layout: ends the command buffer here; when it
                                   completes, its GPU end time in ns goes to fence_page[slot] (low) and [slot + 1] (high), then
                                   value to [slot + 2] */
  HBDX9_PKT_TEXTURE_LOD = 37,   /* (DX9FULL v12, HBDX9_CAP_TEXTURE_LOD) hbdx9_pkt_object: object = texture, index = IDirect3DBaseTexture9::SetLOD
                                   level (the most detailed level sampled; MANAGED textures) */
};
/* (DX9FULL v12) HBDX9_PKT_PRESENT: hdr.pad bit 0 = D3DPRESENT_FORCEIMMEDIATE (this frame without display sync) */
#define HBDX9_PRESENT_FORCE_IMMEDIATE 0x1u

struct hbdx9_packet {
  uint16_t type;
  uint16_t pad;
  uint32_t size;
};

struct hbdx9_rect { int32_t x1, y1, x2, y2; };

struct hbdx9_pkt_clear {
  struct hbdx9_packet hdr;
  uint32_t flags;         /* D3DCLEAR_* */
  uint32_t color;         /* D3DCOLOR (ARGB) */
  float z;
  uint32_t stencil;
  uint32_t rect_count;    /* rects follow (already clipped to the viewport by the shim) */
  uint32_t pad;
  /* struct hbdx9_rect rects[rect_count]; */
};

struct hbdx9_pkt_present {
  struct hbdx9_packet hdr;
  uint64_t swapchain;
};

struct hbdx9_pkt_state {      /* RENDER_STATE (index unused), SAMPLER_STATE, TEXTURE_STAGE_STATE, STREAM_FREQ */
  struct hbdx9_packet hdr;
  uint32_t index;             /* sampler (0-15, 16-19 = D3DVERTEXTEXTURESAMPLER0-3) / stage / stream */
  uint32_t state;
  uint32_t value;
  uint32_t pad;
};

struct hbdx9_pkt_object {     /* TEXTURE, INDICES, SHADER, DEPTH_STENCIL, RENDER_TARGET, RELEASE_OBJECT */
  struct hbdx9_packet hdr;
  uint32_t index;             /* texture sampler / shader stage (0 vs, 1 ps) / rt index */
  uint32_t level;             /* surface level (RENDER_TARGET / DEPTH_STENCIL) */
  uint64_t object;            /* native handle, 0 = unbind / fixed function */
};

struct hbdx9_pkt_stream_source {
  struct hbdx9_packet hdr;
  uint32_t stream, offset, stride, pad;
  uint64_t buffer;
};

enum hbdx9_constant_kind { HBDX9_CONST_VS_F = 0, HBDX9_CONST_PS_F, HBDX9_CONST_VS_I, HBDX9_CONST_PS_I, HBDX9_CONST_VS_B, HBDX9_CONST_PS_B };

struct hbdx9_pkt_constants {
  struct hbdx9_packet hdr;
  uint32_t kind, start, count, pad;
  /* float/int: 16 bytes per register; bool: 4 bytes per register */
};

struct hbdx9_pkt_viewport {
  struct hbdx9_packet hdr;
  uint32_t x, y, width, height;
  float min_z, max_z;
};

struct hbdx9_pkt_scissor {
  struct hbdx9_packet hdr;
  struct hbdx9_rect rect;
};

struct hbdx9_pkt_fvf {
  struct hbdx9_packet hdr;
  uint32_t fvf, pad;
};

struct hbdx9_vertex_element {  /* D3DVERTEXELEMENT9 */
  uint16_t stream, offset;
  uint8_t type, method, usage, usage_index;
};

struct hbdx9_pkt_vertex_decl {
  struct hbdx9_packet hdr;
  uint32_t count, pad;
  /* struct hbdx9_vertex_element elements[count]; (without D3DDECL_END) */
};

struct hbdx9_pkt_draw {
  struct hbdx9_packet hdr;
  uint32_t primitive;         /* D3DPRIMITIVETYPE */
  uint32_t start_vertex;
  uint32_t primitive_count;
  uint32_t pad;
};

struct hbdx9_pkt_draw_indexed {
  struct hbdx9_packet hdr;
  uint32_t primitive;
  int32_t base_vertex;
  uint32_t min_index, num_vertices, start_index, primitive_count;
};

struct hbdx9_pkt_draw_up {
  struct hbdx9_packet hdr;
  uint32_t primitive, primitive_count, stride, vertex_bytes;
  /* vertex data follows */
};

struct hbdx9_pkt_draw_indexed_up {
  struct hbdx9_packet hdr;
  uint32_t primitive, min_index, num_vertices, primitive_count;
  uint32_t index_format;      /* D3DFMT_INDEX16 / D3DFMT_INDEX32 */
  uint32_t stride, index_bytes, vertex_bytes;
  /* index data (padded to 4) then vertex data follow */
};

struct hbdx9_pkt_update_buffer {
  struct hbdx9_packet hdr;
  uint64_t buffer;
  uint64_t guest_data;        /* 0: data follows inline; else a guest address (the shim flushed before) */
  uint32_t offset, size;
  uint32_t flags;             /* D3DLOCK_* */
  uint32_t pad;
};

struct hbdx9_pkt_update_texture {
  struct hbdx9_packet hdr;
  uint64_t texture;
  uint64_t guest_data;        /* as above */
  uint32_t level, face;       /* face: cube face or 3D slice */
  uint32_t x, y, width, height; /* region in pixels (block aligned for compressed formats) */
  uint32_t pitch;             /* source row pitch (rows of blocks for compressed) */
  uint32_t size;              /* bytes */
};

struct hbdx9_pkt_transform {
  struct hbdx9_packet hdr;
  uint32_t state, pad;        /* D3DTRANSFORMSTATETYPE */
  float m[16];
};

struct hbdx9_light {          /* D3DLIGHT9 */
  uint32_t type, pad;
  float diffuse[4], specular[4], ambient[4];
  float position[3], direction[3];
  float range, falloff, attenuation0, attenuation1, attenuation2, theta, phi, pad2;
};
struct hbdx9_pkt_lights {
  struct hbdx9_packet hdr;
  uint32_t count, pad;
  /* struct hbdx9_light lights[count]; */
};

struct hbdx9_pkt_material {   /* D3DMATERIAL9 */
  struct hbdx9_packet hdr;
  float diffuse[4], ambient[4], specular[4], emissive[4];
  float power, pad[3];
};

struct hbdx9_pkt_clip_plane {
  struct hbdx9_packet hdr;
  uint32_t index, pad;
  float plane[4];
};

struct hbdx9_pkt_blit {       /* StretchRect: src/dst native resources, level/face, rects, D3DTEXTUREFILTERTYPE */
  struct hbdx9_packet hdr;
  uint64_t src, dst;
  uint32_t src_level, src_face, dst_level, dst_face;
  struct hbdx9_rect src_rect, dst_rect;
  uint32_t filter, pad;
};

struct hbdx9_pkt_color_fill {
  struct hbdx9_packet hdr;
  uint64_t dst;
  uint32_t level, face;
  struct hbdx9_rect rect;
  uint32_t color, pad;
};

struct hbdx9_pkt_fence {
  struct hbdx9_packet hdr;
  uint32_t slot;              /* index of a uint32 in fence_page */
  uint32_t value;
};

struct hbdx9_pkt_query {
  struct hbdx9_packet hdr;
  uint32_t slot;              /* occlusion: result slot (uint32 pair in fence_page: [count, done-value]) */
  uint32_t begin;             /* 1 begin, 0 end */
  uint32_t value;             /* done-value written with the result at end */
  uint32_t pad;
};

#if defined(__cplusplus)
#define HBDX9_ASSERT(x, n) static_assert(sizeof(x) == n, "layout " #x)
#else
#define HBDX9_ASSERT(x, n) _Static_assert(sizeof(x) == n, "layout " #x)
#endif
HBDX9_ASSERT(struct hbdx9_init_params, 240);
HBDX9_ASSERT(struct hbdx9_swapchain_params, 72);
HBDX9_ASSERT(struct hbdx9_submit_params, 24);
HBDX9_ASSERT(struct hbdx9_ring_slot, 16);
HBDX9_ASSERT(struct hbdx9_ring, 192 + 16 * 1024);
HBDX9_ASSERT(struct hbdx9_readback_params, 48);
HBDX9_ASSERT(struct hbdx9_release_params, 16);
HBDX9_ASSERT(struct hbdx9_create_resource_params, 64);
HBDX9_ASSERT(struct hbdx9_stats_params, 40);
HBDX9_ASSERT(struct hbdx9_create_shader_params, 40);
HBDX9_ASSERT(struct hbdx9_features_params, 16);
HBDX9_ASSERT(struct hbdx9_packet, 8);
HBDX9_ASSERT(struct hbdx9_pkt_clear, 32);
HBDX9_ASSERT(struct hbdx9_pkt_present, 16);
HBDX9_ASSERT(struct hbdx9_pkt_state, 24);
HBDX9_ASSERT(struct hbdx9_pkt_object, 24);
HBDX9_ASSERT(struct hbdx9_pkt_stream_source, 32);
HBDX9_ASSERT(struct hbdx9_pkt_constants, 24);
HBDX9_ASSERT(struct hbdx9_pkt_viewport, 32);
HBDX9_ASSERT(struct hbdx9_pkt_scissor, 24);
HBDX9_ASSERT(struct hbdx9_pkt_fvf, 16);
HBDX9_ASSERT(struct hbdx9_vertex_element, 8);
HBDX9_ASSERT(struct hbdx9_pkt_vertex_decl, 16);
HBDX9_ASSERT(struct hbdx9_pkt_draw, 24);
HBDX9_ASSERT(struct hbdx9_pkt_draw_indexed, 32);
HBDX9_ASSERT(struct hbdx9_pkt_draw_up, 24);
HBDX9_ASSERT(struct hbdx9_pkt_draw_indexed_up, 40);
HBDX9_ASSERT(struct hbdx9_pkt_update_buffer, 40);
HBDX9_ASSERT(struct hbdx9_pkt_update_texture, 56);
HBDX9_ASSERT(struct hbdx9_pkt_transform, 80);
HBDX9_ASSERT(struct hbdx9_light, 112);
HBDX9_ASSERT(struct hbdx9_pkt_lights, 16);
HBDX9_ASSERT(struct hbdx9_pkt_material, 88);
HBDX9_ASSERT(struct hbdx9_pkt_clip_plane, 32);
HBDX9_ASSERT(struct hbdx9_pkt_blit, 80);
HBDX9_ASSERT(struct hbdx9_pkt_color_fill, 48);
HBDX9_ASSERT(struct hbdx9_pkt_fence, 16);
HBDX9_ASSERT(struct hbdx9_pkt_query, 24);

#endif
