/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ #include <mach/mach_time.h>
/* HyperBridge DX9 — native ARM64 core (Wine unix library hbdx9.so).  Lane HBDX9, 07.11.2026.  Protocol v2.
 *
 * Never translated: Metal objects, D3D9 state, shader translation (dxso -> DXBC -> airconv -> AIR) and the PSO cache
 * live here; every packet from the guest shim is executed here.  Objective-C++ with manual reference counting.
 * The same entry points serve __wine_unix_call_funcs and __wine_unix_call_wow64_funcs (hbdx9_protocol.h layouts are
 * identical for i386 and 64-bit guests).  hwnd == 0 in SWAPCHAIN_CREATE gives an offscreen swapchain (native stand). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <time.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "hbdx9_core.h"
#include "airconv_public.h"
#include "airconv_dxso.h"

using namespace hb;

typedef int NTSTATUS;
#define STATUS_SUCCESS 0

/* ---- winemac binding (layout from winemetal_unix.c; keep in step with dlls/winemac.drv) ---- */
typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct macdrv_opaque_window *macdrv_window;
typedef struct opaque_HWND *HWND;
struct macdrv_win_data { HWND hwnd; macdrv_window cocoa_window; macdrv_view cocoa_view; macdrv_view client_cocoa_view; };
struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(int);
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, int require_on_screen);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(macdrv_metal_device d);
  macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
  void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
  void (*on_main_thread)(dispatch_block_t b);
};

static int hb_trace(void) { static int t = -1; if (t < 0) t = getenv("HBDX9_TRACE") != NULL; return t; }
#define TRACE(...) do { if (hb_trace()) fprintf(stderr, "hbdx9-core: " __VA_ARGS__); } while (0)
#define WARN(...) fprintf(stderr, "hbdx9-core: " __VA_ARGS__)

/* ================================================================ formats ================================== */
struct FormatInfo {
  MTLPixelFormat pf;
  MTLPixelFormat srgb_pf;         /* view format for SRGBTEXTURE / SRGBWRITEENABLE (Invalid: no sRGB variant) */
  uint32_t block, bytes;          /* block size in pixels (1 or 4) and bytes per block */
  bool depth, stencil, alpha_one; /* alpha_one: X8 formats read alpha = 1 */
  MTLTextureSwizzleChannels swz;
  bool has_swz;
  uint8_t conv = 0, gbytes = 0;   /* (DX9FULL) CONV_*: the guest writes gbytes per texel, the core converts on upload */
};
enum : uint8_t { CONV_NONE = 0, CONV_L6V5U5, CONV_X8L8V8U8, CONV_A2W10V10U10, CONV_A4L4, CONV_YUY2, CONV_UYVY, CONV_RGBG, CONV_GRGB };
static FormatInfo format_info(uint32_t f) {
  FormatInfo i{MTLPixelFormatInvalid, MTLPixelFormatInvalid, 1, 4, false, false, false, MTLTextureSwizzleChannelsDefault, false};
  auto S = [&](MTLTextureSwizzle r, MTLTextureSwizzle g, MTLTextureSwizzle b, MTLTextureSwizzle a) {
    i.swz = MTLTextureSwizzleChannelsMake(r, g, b, a); i.has_swz = true;
  };
  switch (f) {
  case FMT_A8R8G8B8: i.pf = MTLPixelFormatBGRA8Unorm; break;
  case FMT_X8R8G8B8: i.pf = MTLPixelFormatBGRA8Unorm; i.alpha_one = true; break;
  case FMT_A8B8G8R8: i.pf = MTLPixelFormatRGBA8Unorm; break;
  case FMT_X8B8G8R8: i.pf = MTLPixelFormatRGBA8Unorm; i.alpha_one = true; break;
  case FMT_R5G6B5: i.pf = MTLPixelFormatB5G6R5Unorm; i.bytes = 2; break;
  case FMT_A1R5G5B5: i.pf = MTLPixelFormatBGR5A1Unorm; i.bytes = 2; break;
  case FMT_X1R5G5B5: i.pf = MTLPixelFormatBGR5A1Unorm; i.bytes = 2; i.alpha_one = true; break;
  case FMT_A4R4G4B4: /* Metal ABGR4 reads the D3D nibbles (A,R,G,B) as (R,G,B,A): probe cell "formats" vs wined3d */
    i.pf = MTLPixelFormatABGR4Unorm; i.bytes = 2; S(MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha, MTLTextureSwizzleRed); break;
  case FMT_A8: i.pf = MTLPixelFormatA8Unorm; i.bytes = 1; break;
  case FMT_L8: i.pf = MTLPixelFormatR8Unorm; i.bytes = 1; S(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne); break;
  case FMT_A8L8: i.pf = MTLPixelFormatRG8Unorm; i.bytes = 2; S(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen); break;
  case FMT_L16: i.pf = MTLPixelFormatR16Unorm; i.bytes = 2; S(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne); break;
  case FMT_V8U8: i.pf = MTLPixelFormatRG8Snorm; i.bytes = 2; S(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
  case FMT_Q8W8V8U8: i.pf = MTLPixelFormatRGBA8Snorm; break;
  case FMT_V16U16: i.pf = MTLPixelFormatRG16Snorm; S(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
  case FMT_A2B10G10R10: i.pf = MTLPixelFormatRGB10A2Unorm; break;
  case FMT_A2R10G10B10: i.pf = MTLPixelFormatBGR10A2Unorm; break;
  case FMT_G16R16: i.pf = MTLPixelFormatRG16Unorm; S(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
  case FMT_A16B16G16R16: i.pf = MTLPixelFormatRGBA16Unorm; i.bytes = 8; break;
  case FMT_R16F: i.pf = MTLPixelFormatR16Float; i.bytes = 2; S(MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
  case FMT_G16R16F: i.pf = MTLPixelFormatRG16Float; S(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
  case FMT_A16B16G16R16F: i.pf = MTLPixelFormatRGBA16Float; i.bytes = 8; break;
  case FMT_R32F: i.pf = MTLPixelFormatR32Float; S(MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
  case FMT_G32R32F: i.pf = MTLPixelFormatRG32Float; i.bytes = 8; S(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
  case FMT_A32B32G32R32F: i.pf = MTLPixelFormatRGBA32Float; i.bytes = 16; break;
  case FMT_DXT1: i.pf = MTLPixelFormatBC1_RGBA; i.block = 4; i.bytes = 8; break;
  case FMT_DXT2: case FMT_DXT3: i.pf = MTLPixelFormatBC2_RGBA; i.block = 4; i.bytes = 16; break;
  case FMT_DXT4: case FMT_DXT5: i.pf = MTLPixelFormatBC3_RGBA; i.block = 4; i.bytes = 16; break;
  /* (CRYSIS 12.11.2026) 3Dc.  ATI1 = BC4: red, the rest (0, 0, 1).  ATI2 ("ATI2N") = BC5 with the two blocks in the other
     order than DXGI's BC5: the first block is Y and the second X, so a D3D9 application reads X in .r and Y in .g from
     (second, first) = (BC5.g, BC5.r); blue and alpha read 1 (the same mapping as wined3d's ATI2N fixup and DXVK). */
  case FMT_ATI1: i.pf = MTLPixelFormatBC4_RUnorm; i.block = 4; i.bytes = 8;
    S(MTLTextureSwizzleRed, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleOne); break;
  case FMT_ATI2: i.pf = MTLPixelFormatBC5_RGUnorm; i.block = 4; i.bytes = 16;
    S(MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne); break;
  /* (DX9FULL 13.11.2026) DXVK d3d9_format.cpp: X4R4G4B4 = A4R4G4B4 storage (alpha reads 1 here), Q16W16V16U16 = RGBA16 snorm,
     DF16 / DF24 = D16 / X8_D24 depth sampled raw as (d, 0, 0, 1) (dxso: both depth_samplers bits) */
  /* (DX9FULL) converted formats: bump maps to RGBA16F (DXVK: same target), A4L4 to RG8 read (L, L, L, A), packed YUV / RGBG
     4:2:2 to BGRA8 (YUV: BT.601 video range; DXVK's shader matrix uses integer division 298/256 = 1, this follows its intent) */
  case FMT_L6V5U5: i.pf = MTLPixelFormatRGBA16Float; i.bytes = 8; i.conv = CONV_L6V5U5; i.gbytes = 2; break;
  case FMT_X8L8V8U8: i.pf = MTLPixelFormatRGBA16Float; i.bytes = 8; i.conv = CONV_X8L8V8U8; i.gbytes = 4; break;
  case FMT_A2W10V10U10: i.pf = MTLPixelFormatRGBA16Float; i.bytes = 8; i.conv = CONV_A2W10V10U10; i.gbytes = 4; break;
  case FMT_A4L4: i.pf = MTLPixelFormatRG8Unorm; i.bytes = 2; i.conv = CONV_A4L4; i.gbytes = 1;
    S(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen); break;
  case FMT_YUY2: i.pf = MTLPixelFormatBGRA8Unorm; i.conv = CONV_YUY2; i.gbytes = 2; break;
  case FMT_UYVY: i.pf = MTLPixelFormatBGRA8Unorm; i.conv = CONV_UYVY; i.gbytes = 2; break;
  case FMT_R8G8_B8G8: i.pf = MTLPixelFormatBGRA8Unorm; i.conv = CONV_RGBG; i.gbytes = 2; break;
  case FMT_G8R8_G8B8: i.pf = MTLPixelFormatBGRA8Unorm; i.conv = CONV_GRGB; i.gbytes = 2; break;
  case FMT_X4R4G4B4: i.pf = MTLPixelFormatABGR4Unorm; i.bytes = 2; S(MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha, MTLTextureSwizzleOne); break;
  case FMT_Q16W16V16U16: i.pf = MTLPixelFormatRGBA16Snorm; i.bytes = 8; break;
  case FMT_DF16: i.pf = MTLPixelFormatDepth16Unorm; i.bytes = 2; i.depth = true; break;
  case FMT_DF24: i.pf = MTLPixelFormatDepth32Float; i.bytes = 4; i.depth = true; break;
  case FMT_D16: i.pf = MTLPixelFormatDepth16Unorm; i.bytes = 2; i.depth = true; break;
  case FMT_D24S8: case FMT_D24FS8: case FMT_INTZ: i.pf = MTLPixelFormatDepth32Float_Stencil8; i.bytes = 4; i.depth = i.stencil = true; break;
  case FMT_D24X8: case FMT_D32: case FMT_D32F_LOCKABLE: i.pf = MTLPixelFormatDepth32Float; i.bytes = 4; i.depth = true; break;
  default: break;
  }
  if (i.alpha_one) S(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
  if (!i.conv) switch (i.pf) {
  case MTLPixelFormatBGRA8Unorm: i.srgb_pf = MTLPixelFormatBGRA8Unorm_sRGB; break;
  case MTLPixelFormatRGBA8Unorm: i.srgb_pf = MTLPixelFormatRGBA8Unorm_sRGB; break;
  case MTLPixelFormatBC1_RGBA: i.srgb_pf = MTLPixelFormatBC1_RGBA_sRGB; break;
  case MTLPixelFormatBC2_RGBA: i.srgb_pf = MTLPixelFormatBC2_RGBA_sRGB; break;
  case MTLPixelFormatBC3_RGBA: i.srgb_pf = MTLPixelFormatBC3_RGBA_sRGB; break;
  default: break;
  }
  return i;
}

/* ================================================================ objects ================================== */
struct hb_device;

struct hb_resource {
  uint32_t kind = 0, format = 0, width = 0, height = 0, depth = 1, levels = 1, usage = 0;
  FormatInfo fi{};
  id<MTLBuffer> buffer = nil;          /* HBDX9_RES_BUFFER (shared storage) */
  id<MTLTexture> texture = nil;
  id<MTLTexture> sample_view = nil;    /* swizzled view for sampling (X8, L8, ...) */
  id<MTLTexture> srgb_view = nil;      /* sRGB render view (SRGBWRITEENABLE), created on first use */
  id<MTLTexture> srgb_sample_view = nil; /* sRGB sampling view (SRGBTEXTURE), swizzled like sample_view */
  uint64_t last_serial = 0;            /* last command buffer that referenced it */
  uint32_t samples = 1;
  bool autogen = false, mips_dirty = false; /* D3DUSAGE_AUTOGENMIPMAP: full native chain, regenerated after writes */
  bool pass_written = false;           /* attached to the open render pass and written there (clear or draw): its memory is stale */
  bool pass_sampled = false;           /* attached to the open render pass and bound to a sampler of a draw in it */
  bool is_backbuffer = false;          /* a swap chain's back buffer (session 7 diagnostics) */
  uint32_t lod = 0;                    /* (DX9FULL v12) SetLOD: the most detailed level sampled (DXVK: the view's base level) */
};

struct hb_shader_variant {
  uint64_t key = 0;
  id<MTLFunction> function = nil;
  std::vector<MTL_SM50_SHADER_ARGUMENT> cbufs, args;
  uint32_t vb_slot_mask = 0;
};

struct hb_shader {
  uint32_t stage = 0;
  std::vector<uint32_t> tokens;
  dxso_info info{};                    /* from the default variant (defs pointer cleared) */
  std::vector<dxso_def> defs;
  std::vector<hb_shader_variant *> variants;
};

struct hb_chunk { id<MTLBuffer> buf; uint8_t *cpu; uint64_t gpu; uint32_t size; uint64_t serial; };

struct hb_swapchain {
  hb_device *dev = nullptr;
  macdrv_metal_view view = nullptr;
  CAMetalLayer *layer = nil;
  hb_resource *backbuffer = nullptr, *depth = nullptr;
  uint32_t width = 0, height = 0;
  uint32_t vblanks = 1;                /* (DX9FULL) D3DPRESENT_INTERVAL_TWO/THREE/FOUR: 2..4 refreshes per frame */
  double refresh_hz = 60.0;
  /* (DX9FULL) frame limiter (DXVK FpsLimiter: sleep until the next frame time, then present); 0 = off */
  uint64_t pace_ns = 0, pace_next = 0;
  bool sync = false;
  bool layer_sync = false;             /* (DX9FULL v12) what the layer has now (D3DPRESENT_FORCEIMMEDIATE turns it off for its frames) */
};
/* (DX9FULL) present timing of a device, for the counters: the displayed-time step between consecutive drawables as Core Animation
   reports it (presentedTime; 0 = never shown, e.g. a window on no display), drawable waits, limiter sleeps.  Written from
   presented handlers on Core Animation's thread: atomics, and never freed (a handler may run after the device is gone). */
struct hb_present_stats {
  std::atomic<uint64_t> shown{0}, unshown{0}, bins[8]{};   /* ms steps: <5, 5-7, 7-10, 10-14, 14-20, 20-30, 30-45, >=45 */
  std::atomic<uint64_t> last_us{0};
};

struct hb_pso_entry { uint64_t key[6]; id<MTLRenderPipelineState> pso; };

struct hb_state {
  uint32_t rs[256];
  uint32_t ss[20][14];
  uint32_t tss[8][33];
  hb_resource *tex[20];
  struct { hb_resource *buf; uint32_t offset, stride; } stream[16];
  hb_resource *ib;
  hb_shader *vs, *ps;
  float vsf[256][4], psf[256][4];
  int32_t vsi[16][4], psi[16][4];
  uint32_t vsb[16], psb[16];
  hbdx9_pkt_viewport vp;
  hbdx9_rect scissor;
  hb_resource *rt[4]; uint32_t rt_level[4];
  hb_resource *ds;
  bool use_fvf; uint32_t fvf;
  std::vector<hbdx9_vertex_element> decl;
  float world[16], view[16], proj[16];
  float worlds[3][16];                 /* (DX9FULL) D3DTS_WORLD1..3 for vertex blending */
  hbdx9_light lights[8]; uint32_t nlights;
  float material[17];                  /* diffuse, ambient, specular, emissive (rgba each), power */
  float clip[6][4];
  uint32_t freq[16];                   /* SetStreamSourceFreq: D3DSTREAMSOURCE_INDEXEDDATA / INSTANCEDATA | n */
  float texmat[8][16];                 /* D3DTS_TEXTURE0..7 */
  uint32_t fetch4;                     /* (DX9FULL) sampler s asked for Fetch4 (MIPMAPLODBIAS 'GET4' sets, 'GET1' clears; DXVK) */
};

/* persistent shader cache, next to DXMT's (same file as D3D11: <user cache>/dxmt/<exe>/shaders_<metal>.db, own table) */
struct hb_cache {
  sqlite3 *db = nullptr; sqlite3_stmt *get = nullptr, *put = nullptr;
  uint32_t hits = 0, misses = 0, writes = 0;
  std::string dir;
  /* Pipeline cache: Metal binary archive of render PSOs (GPU code, no compile on a hit). */
  id<MTLBinaryArchive> archive = nil;
  std::string archive_path;
  bool archive_dirty = false;
  std::vector<MTLRenderPipelineDescriptor *> archive_entries;
  uint32_t pso_hits = 0, pso_misses = 0;
};

struct hb_query_pass { std::vector<uint32_t> offsets; uint32_t slot = 0, value = 0; };

struct hb_device {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLCommandBuffer> cmd = nil;
  uint64_t serial = 1;                 /* serial of the open (or next) command buffer */
  std::atomic<uint64_t> completed{0};
  dispatch_semaphore_t frames = nullptr;
  std::vector<hb_chunk *> chunks_free, chunks_used;
  hb_chunk *chunk = nullptr; uint32_t chunk_off = 0;
  /* render pass */
  id<MTLRenderCommandEncoder> enc = nil;
  hb_resource *enc_rt[4] = {}; uint32_t enc_rt_level[4] = {}; hb_resource *enc_ds = nullptr;
  bool enc_fresh = true;
  id<MTLRenderPipelineState> enc_pso = nil;
  id<MTLDepthStencilState> enc_dss = nil;
  /* caches */
  std::vector<hb_pso_entry> psos;
  std::unordered_map<uint64_t, id<MTLDepthStencilState>> dss;
  std::unordered_map<uint64_t, id<MTLSamplerState>> samplers;
  std::vector<std::pair<FfpVsKey, hb_shader *>> ffp_vs;
  std::vector<std::pair<FfpPsKey, hb_shader *>> ffp_ps;
  std::unordered_map<uint64_t, id<MTLRenderPipelineState>> clear_psos;
  hb_resource *dummy_tex = nullptr;
  std::vector<std::pair<uint64_t, id>> deferred_release; /* (serial, object) */
  hb_state st;
  uint64_t draws = 0, pso_misses = 0, frames_presented = 0;
  hb_present_stats *pres = new hb_present_stats;                 /* (DX9FULL) see hb_present_stats; deliberately leaked */
  uint64_t drawable_wait_ns = 0, drawable_waits = 0, pace_sleep_ns = 0, pace_sleeps = 0, force_immediate = 0, sync_toggles = 0, lod_textures = 0;
  uint32_t last_interval = 0, last_pace_mhz = 0; double last_refresh = 0; bool last_sync = false;
  /* fences / queries: values are written into guest memory by the completion handler of the command buffer */
  volatile uint32_t *fence_page = nullptr;
  std::vector<std::pair<uint32_t, uint32_t>> pending_fences;   /* (slot, value) of the open command buffer */
  std::vector<std::pair<uint32_t, uint32_t>> pending_ts;       /* (DX9FULL) timestamp queries (slot, value) ending it */
  id<MTLBuffer> vis = nil; uint32_t vis_next = 0;               /* visibility results, 8 bytes per pass segment */
  bool query_active = false; hb_query_pass query;
  std::vector<hb_query_pass> pending_queries;
  bool enc_srgb = false;
  /* utility pipelines (StretchRect) */
  id<MTLLibrary> util_lib = nil;
  std::unordered_map<uint64_t, id<MTLRenderPipelineState>> blit_psos;
  std::unordered_map<uint64_t, id<MTLSamplerState>> blit_samplers;
  uint32_t metal_version = 310;
  hb_cache cache;
  uint32_t variants_compiled = 0, variants_cached = 0;
  uint32_t cur_depth_samplers = 0;     /* of the draw being bound (compare samplers) */
  /* uploads (session 5): what came across and what the core did with it.  HBDX9_UPLOADS=old = session-4 behaviour */
  bool uploads_old = false;
  struct { uint64_t draws = 0, color = 0, depth = 0, depth_written = 0, splits = 0; std::vector<uint64_t> first_frames; } fb; /* feedback */
  bool feedback_off = false;
  int serial_debug = 0; uint32_t split_n = 1, class_in_pass = 0;          /* HBDX9_SERIAL=1: commit and wait after every draw (determinism diagnostic) */
  /* session 7 diagnostic HBDX9_FEEDBACK_DETACH=1: a draw that samples a colour target (index >= 1) bound to it but does not write
     it runs in a pass WITHOUT that attachment, so no tile load/store of that target races with the sampling (tiled GPU) */
  /* session 7: HBDX9_PROFILE=1 - worker time per hb_draw phase (mach ticks, printed at release) */
  bool prof_on = false; uint64_t prof[8] = {}; uint64_t prof_draws = 0;
  id<MTLFence> pass_fence = nil; bool pass_fence_set = false; uint64_t diag_passes = 0, diag_fence_waits = 0, diag_commit_waits = 0, diag_iso = 0;
  bool feedback_detach = false; uint32_t want_detach = 0, enc_detach = 0; uint64_t detach_passes = 0, detach_draws = 0;
  bool feedback_barrier = false;       /* HBDX9_FEEDBACK_BARRIER=1: end the pass before a draw sampling an attachment written in
                                          it (DXVK semantics).  Off by default: +11.7 ms/frame median on the Gothic world tape
                                          (frames 716-1033, ~2.2 splits a frame), 9 frames change, run-to-run noise unchanged */
  /* (DX9FULL 13.11.2026) HBDX9_FMT_EXT (default on, "0" = the s8j+ATI2 behaviour): DF16/DF24 raw depth, Fetch4, NVDB depth
     bounds, ATOC alpha-to-coverage, BOTHSRCALPHA/BOTHINVSRCALPHA, sampler border colour */
  bool fmt_ext = true;
  bool enc_db_on = false; float enc_db_min = 0, enc_db_max = 1;   /* depth bounds set on the open encoder (Metal default: off) */
  bool enc_bc_set = false; uint32_t enc_bc = 0;                  /* blend colour set on the open encoder */
  uint64_t ext_blendfactor_draws = 0, ext_point_draws = 0, ext_blend_draws = 0;
  uint64_t ext_df_draws = 0, ext_fetch4_draws = 0, ext_nvdb_draws = 0, ext_atoc_draws = 0, ext_border_samplers = 0;
  bool rt_init_off = false; uint64_t rt_inits = 0;
  bool tex_init_off = false; uint64_t tex_inits = 0;
  /* (session 6) asynchronous submission, HBDX9_ASYNC=1: SUBMIT copies the packet buffer and returns; a native worker thread
     of the device runs the packets in order.  Every other call drains the queue first.  The sequence number of the last
     SUBMIT that has been run goes into fence_page[HBDX9_SEQ_SLOT] (both modes), so the shim knows when guest memory that a
     packet references by pointer may change again. */
  bool async = false;
  std::mutex amx; std::condition_variable acv_work, acv_done;
  /* (INT32G patch 0004) a queued SUBMIT: its packets are a private copy (vec: as before; owned: HBDX9_TSO_COPY) or stay in guest
     memory (HBDX9_SUBMIT_NOCOPY, a chunk of the ring) */
  struct hb_job { std::vector<uint8_t> *vec = nullptr; uint8_t *owned = nullptr; const uint8_t *data = nullptr;
                  uint32_t size = 0, seq = 0, presents = 0; bool ring = false; };
  std::deque<hb_job> aq; std::atomic<uint32_t> aq_pending{0};
  bool tso_copy = false;                                   /* HBDX9_TSO_COPY=1 */
  hbdx9_ring *ring = nullptr;                              /* the chunk ring, if the shim attached one */
  uint32_t ring_tail = 0, ring_presents_done = 0;          /* the worker's own counts (the ring's fields are their published copies) */
  uint32_t ring_spin_us = 100, ring_park_us = 5000;
  uint64_t copied_jobs = 0, tso_copy_jobs = 0, nocopy_jobs = 0, ring_chunks = 0, ring_bytes = 0, ring_parks = 0, ring_wakes = 0,
           ring_waits = 0, ring_spin_hits = 0;
  bool aworking = false;
  uint32_t seq_submitted = 0, presents_enq = 0, presents_done = 0;
  uint64_t async_jobs = 0, async_bytes = 0, pacing_waits = 0, drains = 0, drain_waits = 0;
  uint64_t pacing_wait_ns = 0, drain_wait_ns = 0, worker_ns = 0, sync_submit_ns = 0;           /* HBDX9_FEEDBACK=off: diagnostic, session-5 behaviour (no split, write masks as set) */
  bool alpha_float = false;            /* HBDX9_ALPHATEST=float: diagnostic, wined3d's model (float alpha vs ref/255) */
  bool noov_rename = false;            /* HBDX9_NOOV_RENAME=1: diagnostic, NOOVERWRITE on a busy buffer renames like any lock */
  bool alpha_8bit = false;             /* HBDX9_ALPHATEST=8bit: diagnostic, session-4 model (8-bit for every target) */
  struct {
    uint64_t buf_n = 0, buf_offered = 0, buf_compared = 0, buf_written = 0, buf_nochange = 0, buf_renames = 0, buf_inplace_busy = 0;
    uint64_t tex_n = 0, tex_bytes = 0, tex_blits = 0;
  } up;
};

static void hb_drain(hb_device *d);   /* asynchronous submission (session 6), defined next to hb_submit */
static void hb_worker(hb_device *d);

/* ================================================================ command buffers / ring ================== */
static id<MTLCommandBuffer> hb_cmd(hb_device *d) {
  if (!d->cmd) d->cmd = [[d->queue commandBuffer] retain];
  return d->cmd;
}
static void hb_end_pass(hb_device *d) {
  /* session 7 diagnostic HBDX9_SERIAL=9: every render pass waits for the previous one's fragment stage (one MTLFence chained
     through all render passes) - separates races BETWEEN passes from effects inside one pass */
  if (d->enc && d->serial_debug == 9) {
    if (!d->pass_fence) d->pass_fence = [d->device newFence];
    [d->enc updateFence:d->pass_fence afterStages:MTLRenderStageFragment];
    d->pass_fence_set = true;
  }
  if (d->enc) { [d->enc endEncoding]; [d->enc release]; d->enc = nil; }
  d->enc_pso = nil; d->enc_dss = nil; d->enc_db_on = false; d->enc_bc_set = false;
}
static void hb_reap(hb_device *d) {
  uint64_t done = d->completed.load();
  for (size_t i = 0; i < d->deferred_release.size();) {
    if (d->deferred_release[i].first <= done) { [d->deferred_release[i].second release]; d->deferred_release[i] = d->deferred_release.back(); d->deferred_release.pop_back(); }
    else i++;
  }
  for (size_t i = 0; i < d->chunks_used.size();) {
    if (d->chunks_used[i]->serial <= done) { d->chunks_free.push_back(d->chunks_used[i]); d->chunks_used[i] = d->chunks_used.back(); d->chunks_used.pop_back(); }
    else i++;
  }
}
static void hb_commit(hb_device *d, bool wait, bool frame) {
  /* session 7 diagnostics: HBDX9_SERIAL=7 waits for the GPU at every frame end, =8 at every commit (the CPU never runs ahead
     of the GPU, render passes unchanged) - separates CPU/GPU races on shared resources from in-pass GPU effects */
  if (d->serial_debug == 8 || (d->serial_debug == 7 && frame)) wait = true;
  hb_end_pass(d);
  if (!d->cmd) return;
  uint64_t s = d->serial;
  hb_device *dev = d;
  dispatch_semaphore_t sem = frame ? d->frames : nullptr;
  /* fences and finished occlusion queries of this command buffer: written into guest memory on completion */
  auto *fences = d->pending_fences.empty() ? nullptr : new std::vector<std::pair<uint32_t, uint32_t>>(std::move(d->pending_fences));
  auto *queries = d->pending_queries.empty() ? nullptr : new std::vector<hb_query_pass>(std::move(d->pending_queries));
  auto *stamps = d->pending_ts.empty() ? nullptr : new std::vector<std::pair<uint32_t, uint32_t>>(std::move(d->pending_ts));
  d->pending_fences.clear(); d->pending_queries.clear(); d->pending_ts.clear();
  volatile uint32_t *page = d->fence_page;
  const uint64_t *vis = d->vis ? (const uint64_t *)[d->vis contents] : nullptr;
  [d->cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
    uint64_t prev = dev->completed.load();
    while (prev < s && !dev->completed.compare_exchange_weak(prev, s)) {}
    if (page && queries) {
      for (auto &q : *queries) {
        uint64_t n = 0;
        for (uint32_t o : q.offsets) n += vis[o / 8];
        page[q.slot] = (uint32_t)std::min<uint64_t>(n, 0xffffffffu);
        __atomic_store_n((uint32_t *)&page[q.slot + 1], q.value, __ATOMIC_RELEASE);
      }
    }
    if (page && fences) for (auto &f : *fences) __atomic_store_n((uint32_t *)&page[f.first], f.second, __ATOMIC_RELEASE);
    if (page && stamps) {
      /* (DX9FULL) D3DQUERYTYPE_TIMESTAMP: the GPU end time of this command buffer (CACurrentMediaTime timebase), in ns */
      double g = cb.GPUEndTime;
      uint64_t ns = g > 0 ? (uint64_t)(g * 1e9) : clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      for (auto &s : *stamps) {
        page[s.first] = (uint32_t)ns; page[s.first + 1] = (uint32_t)(ns >> 32);
        __atomic_store_n((uint32_t *)&page[s.first + 2], s.second, __ATOMIC_RELEASE);
      }
    }
    delete fences; delete queries; delete stamps;
    if (sem) dispatch_semaphore_signal(sem);
  }];
  if (frame) dispatch_semaphore_wait(d->frames, DISPATCH_TIME_FOREVER);
  [d->cmd commit];
  if (wait) { [d->cmd waitUntilCompleted]; d->diag_commit_waits++; }
  [d->cmd release];
  d->cmd = nil;
  d->serial++;
  hb_reap(d);
}
/* returns cpu pointer; *buf/*off for binding, *gpu for indirect addressing */
static uint8_t *hb_alloc(hb_device *d, uint32_t size, uint32_t align, id<MTLBuffer> *buf, uint32_t *off, uint64_t *gpu) {
  size = (size + 15) & ~15u;
  if (!d->chunk || ((d->chunk_off + align - 1) & ~(align - 1)) + size > d->chunk->size) {
    if (d->chunk) d->chunks_used.push_back(d->chunk);
    d->chunk = nullptr;
    hb_reap(d);
    for (size_t i = 0; i < d->chunks_free.size(); i++)
      if (d->chunks_free[i]->size >= size) { d->chunk = d->chunks_free[i]; d->chunks_free.erase(d->chunks_free.begin() + i); break; }
    if (!d->chunk) {
      uint32_t sz = std::max<uint32_t>(4u << 20, size);
      auto *c = new hb_chunk;
      c->buf = [d->device newBufferWithLength:sz options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
      c->cpu = (uint8_t *)[c->buf contents]; c->gpu = [c->buf gpuAddress]; c->size = sz; c->serial = 0;
      d->chunk = c;
    }
    d->chunk_off = 0;
  }
  uint32_t o = (d->chunk_off + align - 1) & ~(align - 1);
  d->chunk_off = o + size;
  d->chunk->serial = d->serial;
  *buf = d->chunk->buf; *off = o; if (gpu) *gpu = d->chunk->gpu + o;
  return d->chunk->cpu + o;
}
static bool hb_busy(hb_device *d, hb_resource *r) { return r->last_serial > d->completed.load(); }
static void hb_defer_release(hb_device *d, id obj, uint64_t serial) { if (obj) d->deferred_release.push_back({serial, obj}); }

/* ================================================================ resources ================================ */
static hb_resource *hb_new_texture(hb_device *d, uint32_t kind, uint32_t format, uint32_t w, uint32_t h, uint32_t depth,
                                   uint32_t levels, uint32_t usage, bool rt, uint32_t samples = 1) {
  FormatInfo fi = format_info(format);
  if (fi.pf == MTLPixelFormatInvalid) return nullptr;
  uint32_t full = 1; for (uint32_t m = std::max(std::max(w, h), depth); m > 1; m >>= 1) full++;
  bool autogen = usage & 0x400 /* D3DUSAGE_AUTOGENMIPMAP */;
  if (!levels || levels > full || autogen) levels = full;
  if (samples < 2) samples = 1;
  if (samples > 1) levels = 1;
  MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
  td.textureType = samples > 1 ? MTLTextureType2DMultisample : kind == HBDX9_RES_TEXTURE_CUBE ? MTLTextureTypeCube
                 : kind == HBDX9_RES_TEXTURE_3D ? MTLTextureType3D : MTLTextureType2D;
  td.pixelFormat = fi.pf; td.width = w; td.height = h; td.depth = kind == HBDX9_RES_TEXTURE_3D ? depth : 1;
  td.mipmapLevelCount = levels; td.sampleCount = samples;
  bool renderable = rt || fi.depth || (usage & 0x3 /* RENDERTARGET|DEPTHSTENCIL */) || (autogen && fi.block == 1);
  td.usage = MTLTextureUsageShaderRead | (renderable ? MTLTextureUsageRenderTarget : 0) | MTLTextureUsagePixelFormatView;
  td.storageMode = (renderable || samples > 1) ? MTLStorageModePrivate : MTLStorageModeShared;
  if (fi.block > 1 || fi.depth) td.storageMode = fi.depth ? MTLStorageModePrivate : MTLStorageModeShared;
  id<MTLTexture> t = [d->device newTextureWithDescriptor:td];
  MTLTextureType ttype = td.textureType;
  [td release];
  if (!t) return nullptr;
  auto *r = new hb_resource;
  r->kind = kind; r->format = format; r->width = w; r->height = h; r->depth = depth; r->levels = levels; r->usage = usage;
  r->fi = fi; r->texture = t; r->samples = samples; r->autogen = autogen;
  if (fi.has_swz)
    r->sample_view = [t newTextureViewWithPixelFormat:fi.pf textureType:ttype levels:NSMakeRange(0, levels)
                                               slices:NSMakeRange(0, kind == HBDX9_RES_TEXTURE_CUBE ? 6 : 1) swizzle:fi.swz];
  return r;
}

static NTSTATUS hb_create_resource(void *args) {
  auto *p = (hbdx9_create_resource_params *)args;
  auto *d = (hb_device *)(uintptr_t)p->device;
  p->status = 1; p->resource = 0;
  if (!d) return STATUS_SUCCESS;
  hb_drain(d);
  @autoreleasepool {
    hb_resource *r = nullptr;
    if (p->kind == HBDX9_RES_BUFFER) {
      r = new hb_resource;
      r->kind = p->kind; r->format = p->format; r->width = p->width; r->usage = p->usage;
      r->buffer = [d->device newBufferWithLength:std::max<uint32_t>(p->width, 16) options:MTLResourceStorageModeShared];
      if (!r->buffer) { delete r; p->status = 2; return STATUS_SUCCESS; }
      memset([r->buffer contents], 0, [r->buffer length]);
    } else if (p->format == FMT_NULL && (p->kind == HBDX9_RES_SURFACE_RT || p->kind == HBDX9_RES_TEXTURE_2D)) {
      /* D3DFMT_NULL render target: depth-only passes; no colour attachment is bound for it */
      r = new hb_resource;
      r->kind = HBDX9_RES_SURFACE_RT; r->format = p->format; r->width = p->width; r->height = p->height; r->levels = 1; r->usage = p->usage;
    } else {
      bool rt = p->kind == HBDX9_RES_SURFACE_RT || p->kind == HBDX9_RES_SURFACE_DS;
      uint32_t kind = rt ? HBDX9_RES_TEXTURE_2D : p->kind;
      r = hb_new_texture(d, kind, p->format, p->width, p->height, p->depth, rt ? 1 : p->levels, p->usage, rt, rt ? p->samples : 1);
      if (r) r->kind = p->kind;
    }
    if (!r) { WARN("create resource kind=%u format=%u %ux%u failed\n", p->kind, p->format, p->width, p->height); return STATUS_SUCCESS; }
    /* (session 6) sampled textures start as zeros: a level the game has not uploaded yet (streamed mip chains) would otherwise
       be sampled from undefined memory, which differs run to run.  HBDX9_TEX_INIT=0 switches it off. */
    if (r->texture && r->texture.storageMode == MTLStorageModeShared && !d->tex_init_off) {
      uint32_t bw0 = (r->width + r->fi.block - 1) / r->fi.block, bh0 = (r->height + r->fi.block - 1) / r->fi.block;
      size_t row0 = (size_t)bw0 * r->fi.bytes, need = row0 * bh0;
      static std::vector<uint8_t> zeros;
      if (zeros.size() < need) zeros.assign(need, 0);
      bool is3d = r->kind == HBDX9_RES_TEXTURE_3D;
      uint32_t layers = r->kind == HBDX9_RES_TEXTURE_CUBE ? 6 : 1;
      for (uint32_t l = 0; l < r->levels; l++) {
        uint32_t w = std::max<uint32_t>(1, r->width >> l), h = std::max<uint32_t>(1, r->height >> l);
        uint32_t dd = is3d ? std::max<uint32_t>(1, r->depth >> l) : 1;
        uint32_t bw = (w + r->fi.block - 1) / r->fi.block, bh = (h + r->fi.block - 1) / r->fi.block;
        size_t row = (size_t)bw * r->fi.bytes;
        for (uint32_t s = 0; s < layers; s++)
          for (uint32_t z = 0; z < dd; z++)
            [r->texture replaceRegion:MTLRegionMake3D(0, 0, z, w, h, 1) mipmapLevel:l slice:s withBytes:zeros.data()
                          bytesPerRow:row bytesPerImage:0];
        (void)bh;
      }
      d->tex_inits++;
    }
    /* (session 6) render targets and depth textures start defined: Metal leaves a new Private texture's contents undefined
       (whatever memory it gets), D3D9 calls them undefined too; one clear at creation makes every replay start from the
       same bytes (colour 0, depth 1, stencil 0).  HBDX9_RT_INIT=0 switches it off. */
    if (r->texture && (r->texture.usage & MTLTextureUsageRenderTarget) && r->samples == 1 && !d->rt_init_off &&
        r->kind != HBDX9_RES_TEXTURE_CUBE && r->kind != HBDX9_RES_TEXTURE_3D) {
      hb_end_pass(d);
      MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
      if (r->fi.depth) {
        rp.depthAttachment.texture = r->texture; rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 1.0;
        if (r->fi.stencil) {
          rp.stencilAttachment.texture = r->texture; rp.stencilAttachment.loadAction = MTLLoadActionClear;
          rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = 0;
        }
      } else {
        rp.colorAttachments[0].texture = r->texture; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
      }
      id<MTLRenderCommandEncoder> ce = [hb_cmd(d) renderCommandEncoderWithDescriptor:rp];
      [ce endEncoding];
      r->last_serial = d->serial; d->rt_inits++;
    }
    p->out_levels = r->levels;
    p->resource = (uint64_t)(uintptr_t)r;
    p->status = 0;
    TRACE("resource %p kind=%u fmt=%u %ux%ux%u levels=%u\n", (void *)r, p->kind, p->format, p->width, p->height, p->depth, r->levels);
  }
  return STATUS_SUCCESS;
}

/* write only the runs of `src` that differ from `dst` (64-byte blocks, runs closer than 512 bytes merged); returns bytes
   written.  Both are CPU memory here: the guest shadow and the shared MTLBuffer the GPU reads. */
static uint64_t hb_write_changed(uint8_t *dst, const uint8_t *src, size_t n) {
  const size_t B = 64, GAP = 512;
  uint64_t written = 0;
  size_t i = 0, a = SIZE_MAX, e = 0;
  for (; i < n; i += B) {
    size_t m = n - i < B ? n - i : B;
    bool eq;
    if (m == B) {
      uint64_t x = 0, w0, w1;
      for (size_t k = 0; k < B; k += 8) { memcpy(&w0, dst + i + k, 8); memcpy(&w1, src + i + k, 8); x |= w0 ^ w1; }
      eq = !x;
    } else eq = !memcmp(dst + i, src + i, m);
    if (eq) continue;
    if (a != SIZE_MAX && i - e > GAP) { memcpy(dst + a, src + a, e - a); written += e - a; a = SIZE_MAX; }
    if (a == SIZE_MAX) a = i;
    e = i + m;
  }
  if (a != SIZE_MAX) { memcpy(dst + a, src + a, e - a); written += e - a; }
  return written;
}

static void hb_update_buffer(hb_device *d, const hbdx9_pkt_update_buffer *u, const uint8_t *data) {
  auto *r = (hb_resource *)(uintptr_t)u->buffer;
  if (!r || !r->buffer || (uint64_t)u->offset + u->size > [r->buffer length]) return;
  d->up.buf_n++; d->up.buf_offered += u->size;
  NSUInteger len = [r->buffer length];
  if (d->uploads_old) {
    if (hb_busy(d, r)) {
      /* the GPU (or a command buffer still being recorded) reads the old contents: rename, copy-on-write */
      id<MTLBuffer> nb = [d->device newBufferWithLength:len options:MTLResourceStorageModeShared];
      if (!(u->flags & 0x2000 /* D3DLOCK_DISCARD */) || u->offset || u->size != len)
        memcpy([nb contents], [r->buffer contents], len);
      hb_defer_release(d, r->buffer, r->last_serial);
      r->buffer = nb; r->last_serial = 0; d->up.buf_renames++;
    }
    memcpy((uint8_t *)[r->buffer contents] + u->offset, data, u->size);
    d->up.buf_written += u->size;
    return;
  }
  /* The buffer's contents are what every draw encoded so far will read.  Bytes equal to them need not move; a lock
     without changes moves nothing and renames nothing.  NOOVERWRITE (DYNAMIC only, masked by the shim) promises that no
     pending draw reads the bytes being changed: those go in place even while the buffer is busy, as on D3D9 hardware. */
  uint8_t *cur = (uint8_t *)[r->buffer contents] + u->offset;
  bool discard = u->flags & 0x2000, nooverwrite = u->flags & 0x1000;
  /* A lock WITHOUT NOOVERWRITE on a busy buffer renames even when its bytes are unchanged: D3D9's NOOVERWRITE promise
     covers only draws since the last lock without the flag, so the draws before this lock must keep the old storage, or a
     later in-place NOOVERWRITE write would reach them (07.11 s5: skipping such an identical lock changed 19 of 1068
     world-tape frames). */
  if (hb_busy(d, r) && (!nooverwrite || d->noov_rename)) {
    id<MTLBuffer> nb = [d->device newBufferWithLength:len options:MTLResourceStorageModeShared];
    bool whole = discard && !u->offset && u->size == len;
    if (!whole) memcpy([nb contents], [r->buffer contents], len);
    hb_defer_release(d, r->buffer, r->last_serial);
    r->buffer = nb; r->last_serial = 0; d->up.buf_renames++;
    uint8_t *dst = (uint8_t *)[nb contents] + u->offset;
    if (whole) { memcpy(dst, data, u->size); d->up.buf_written += u->size; }
    else { d->up.buf_compared += u->size; d->up.buf_written += hb_write_changed(dst, data, u->size); }
    return;
  }
  d->up.buf_compared += u->size;
  if (!memcmp(cur, data, u->size)) { d->up.buf_nochange++; return; }
  if (hb_busy(d, r)) d->up.buf_inplace_busy++;
  d->up.buf_written += hb_write_changed(cur, data, u->size);
}

static void hb_gen_mips(hb_device *d, hb_resource *r) {
  if (!r || !r->texture || r->levels < 2 || r->fi.block > 1 || r->fi.depth) { if (r) r->mips_dirty = false; return; }
  hb_end_pass(d);
  id<MTLBlitCommandEncoder> b = [hb_cmd(d) blitCommandEncoder];
  [b generateMipmapsForTexture:r->texture];
  [b endEncoding];
  r->last_serial = d->serial; r->mips_dirty = false;
}

/* (DX9FULL 13.11.2026) upload conversion of the formats Metal lacks (DXVK d3d9_convert_*.comp, done here on the CPU) */
static void hb_store_half4(uint8_t *o, float a, float b, float c, float e) {
  _Float16 h[4] = {(_Float16)a, (_Float16)b, (_Float16)c, (_Float16)e}; memcpy(o, h, 8);
}
static float hb_snorm(int v, int bits) { float f = (float)v / (float)((1 << (bits - 1)) - 1); return f < -1.0f ? -1.0f : f; }
static int hb_sext(uint32_t v, int bits) { return (int)(v << (32 - bits)) >> (32 - bits); }
static uint8_t hb_clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void hb_yuv_bgra(int y, int u, int v, uint8_t *o) {
  int c = y - 16, dd = u - 128, e = v - 128;
  o[2] = hb_clamp8((298 * c + 409 * e + 128) >> 8); o[1] = hb_clamp8((298 * c - 100 * dd - 208 * e + 128) >> 8);
  o[0] = hb_clamp8((298 * c + 516 * dd + 128) >> 8); o[3] = 255;
}
static void hb_convert_rows(uint8_t conv, const uint8_t *src, uint32_t spitch, uint32_t w, uint32_t h, uint8_t *dst, uint32_t dpitch) {
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t *s = src + (size_t)y * spitch; uint8_t *o = dst + (size_t)y * dpitch;
    switch (conv) {
    case CONV_L6V5U5:
      for (uint32_t x = 0; x < w; x++) { uint32_t v = s[2 * x] | s[2 * x + 1] << 8;
        hb_store_half4(o + 8 * x, hb_snorm(hb_sext(v, 5), 5), hb_snorm(hb_sext(v >> 5, 5), 5), (float)(v >> 10 & 63) / 63.0f, 1.0f); }
      break;
    case CONV_X8L8V8U8:
      for (uint32_t x = 0; x < w; x++) { uint32_t v; memcpy(&v, s + 4 * x, 4);
        hb_store_half4(o + 8 * x, hb_snorm(hb_sext(v, 8), 8), hb_snorm(hb_sext(v >> 8, 8), 8), (float)(v >> 16 & 255) / 255.0f, 1.0f); }
      break;
    case CONV_A2W10V10U10:
      for (uint32_t x = 0; x < w; x++) { uint32_t v; memcpy(&v, s + 4 * x, 4);
        hb_store_half4(o + 8 * x, hb_snorm(hb_sext(v, 10), 10), hb_snorm(hb_sext(v >> 10, 10), 10), hb_snorm(hb_sext(v >> 20, 10), 10),
                       (float)(v >> 30) / 3.0f); }
      break;
    case CONV_A4L4:
      for (uint32_t x = 0; x < w; x++) { o[2 * x] = (uint8_t)((s[x] & 15) * 17); o[2 * x + 1] = (uint8_t)((s[x] >> 4) * 17); }
      break;
    case CONV_YUY2: case CONV_UYVY: case CONV_RGBG: case CONV_GRGB:
      for (uint32_t x = 0; x < w; x += 2) {
        const uint8_t *q = s + 2 * x; uint8_t *p0 = o + 4 * x, *p1 = x + 1 < w ? p0 + 4 : nullptr, t1[4];
        if (!p1) p1 = t1;
        if (conv == CONV_YUY2) { hb_yuv_bgra(q[0], q[1], q[3], p0); hb_yuv_bgra(q[2], q[1], q[3], p1); }        /* Y0 U Y1 V */
        else if (conv == CONV_UYVY) { hb_yuv_bgra(q[1], q[0], q[2], p0); hb_yuv_bgra(q[3], q[0], q[2], p1); }   /* U Y0 V Y1 */
        else if (conv == CONV_RGBG) {   /* DXVK: G8B8G8R8_422 = bytes G0 B G1 R */
          p0[0] = q[1]; p0[1] = q[0]; p0[2] = q[3]; p0[3] = 255; p1[0] = q[1]; p1[1] = q[2]; p1[2] = q[3]; p1[3] = 255;
        } else {                        /* DXVK: B8G8R8G8_422 = bytes B G0 R G1 */
          p0[0] = q[0]; p0[1] = q[1]; p0[2] = q[2]; p0[3] = 255; p1[0] = q[0]; p1[1] = q[3]; p1[2] = q[2]; p1[3] = 255;
        }
      }
      break;
    default: break;
    }
  }
}

static void hb_update_texture(hb_device *d, const hbdx9_pkt_update_texture *u, const uint8_t *data) {
  auto *r = (hb_resource *)(uintptr_t)u->texture;
  if (!r || !r->texture || u->level >= r->levels) return;
  std::vector<uint8_t> conv_buf; hbdx9_pkt_update_texture uc;
  if (r->fi.conv) {   /* (DX9FULL) the guest layout (gbytes per texel) becomes the Metal layout */
    uint32_t dp = u->width * r->fi.bytes;
    conv_buf.resize((size_t)dp * u->height);
    hb_convert_rows(r->fi.conv, data, u->pitch, u->width, u->height, conv_buf.data(), dp);
    uc = *u; uc.pitch = dp; uc.size = dp * u->height; u = &uc; data = conv_buf.data();
  }
  d->up.tex_n++; d->up.tex_bytes += u->size;
  if (r->autogen && u->level == 0) r->mips_dirty = true;
  if (hb_busy(d, r) || r->texture.storageMode == MTLStorageModePrivate) {
    /* blit from the ring keeps command order: end the pass, copy, continue */
    uint32_t bw = (u->width + r->fi.block - 1) / r->fi.block, bh = (u->height + r->fi.block - 1) / r->fi.block;
    uint32_t row = bw * r->fi.bytes;
    id<MTLBuffer> buf; uint32_t off;
    uint8_t *dst = hb_alloc(d, row * bh, 256, &buf, &off, nullptr);
    for (uint32_t y = 0; y < bh; y++) memcpy(dst + y * row, data + (size_t)y * u->pitch, row);
    hb_end_pass(d);
    id<MTLBlitCommandEncoder> b = [hb_cmd(d) blitCommandEncoder];
    bool is3d = r->kind == HBDX9_RES_TEXTURE_3D;
    [b copyFromBuffer:buf sourceOffset:off sourceBytesPerRow:row sourceBytesPerImage:row * bh
           sourceSize:MTLSizeMake(u->width, u->height, 1) toTexture:r->texture
     destinationSlice:is3d ? 0 : u->face destinationLevel:u->level destinationOrigin:MTLOriginMake(u->x, u->y, is3d ? u->face : 0)];
    [b endEncoding];
    r->last_serial = d->serial; d->up.tex_blits++;
    return;
  }
  bool is3d = r->kind == HBDX9_RES_TEXTURE_3D;
  [r->texture replaceRegion:MTLRegionMake3D(u->x, u->y, is3d ? u->face : 0, u->width, u->height, 1) mipmapLevel:u->level
                      slice:is3d ? 0 : u->face withBytes:data bytesPerRow:u->pitch bytesPerImage:0];
}

static void hb_release_resource(hb_device *d, hb_resource *r) {
  if (!r) return;
  for (auto &t : d->st.tex) if (t == r) t = nullptr;
  for (auto &s : d->st.stream) if (s.buf == r) s.buf = nullptr;
  if (d->st.ib == r) d->st.ib = nullptr;
  for (auto &t : d->st.rt) if (t == r) t = nullptr;
  if (d->st.ds == r) d->st.ds = nullptr;
  for (auto &t : d->enc_rt) if (t == r) { hb_end_pass(d); }
  if (d->enc_ds == r) hb_end_pass(d);
  uint64_t s = std::max(r->last_serial, d->serial);
  hb_defer_release(d, r->buffer, s); hb_defer_release(d, r->texture, s); hb_defer_release(d, r->sample_view, s);
  hb_defer_release(d, r->srgb_view, s); hb_defer_release(d, r->srgb_sample_view, s);
  delete r;
}

/* ================================================================ shaders ================================== */
static hb_shader *hb_shader_from_tokens(const uint32_t *tok, size_t max_dwords, uint32_t stage, char *err, size_t errlen) {
  dxso_result *res = nullptr;
  dxso_variant v{};
  if (dxso_translate(tok, max_dwords, &v, &res, err, errlen)) return nullptr;
  const dxso_info *info = dxso_result_info(res);
  if (info->stage != stage) { snprintf(err, errlen, "stage mismatch"); dxso_result_free(res); return nullptr; }
  auto *s = new hb_shader;
  s->stage = stage;
  s->tokens.assign(tok, tok + info->token_count);
  s->info = *info;
  s->defs.assign(info->defs, info->defs + info->def_count);
  s->info.defs = nullptr;
  dxso_result_free(res);
  return s;
}

static NTSTATUS hb_create_shader(void *args) {
  auto *p = (hbdx9_create_shader_params *)args;
  hb_drain((hb_device *)(uintptr_t)p->device);
  p->status = 1; p->shader = 0;
  const uint32_t *tok = (const uint32_t *)(uintptr_t)p->tokens;
  if (!tok) return STATUS_SUCCESS;
  char err[256] = "";
  size_t max = p->max_dwords ? p->max_dwords : (1u << 20);
  hb_shader *s = hb_shader_from_tokens(tok, max, p->stage, err, sizeof(err));
  if (!s) { WARN("create shader (stage %u, version %08x) failed: %s\n", p->stage, tok[0], err); return STATUS_SUCCESS; }
  p->shader = (uint64_t)(uintptr_t)s;
  p->token_count = s->info.token_count;
  p->status = 0;
  TRACE("shader %p stage=%u version=%08x tokens=%u slots_w=%x slots_r=%x samplers=%x defs=%zu\n", (void *)s, p->stage, tok[0],
        s->info.token_count, s->info.slots_written, s->info.slots_read, s->info.sampler_mask, s->defs.size());
  return STATUS_SUCCESS;
}

static uint64_t fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

static MTLAttributeFormat decl_format(uint8_t t, bool *is_int) {
  *is_int = false;
  switch (t) {
  case DT_FLOAT1: return MTLAttributeFormatFloat;
  case DT_FLOAT2: return MTLAttributeFormatFloat2;
  case DT_FLOAT3: return MTLAttributeFormatFloat3;
  case DT_FLOAT4: return MTLAttributeFormatFloat4;
  case DT_D3DCOLOR: return MTLAttributeFormatUChar4Normalized_BGRA;
  case DT_UBYTE4: *is_int = true; return MTLAttributeFormatUChar4;
  case DT_SHORT2: *is_int = true; return MTLAttributeFormatShort2;
  case DT_SHORT4: *is_int = true; return MTLAttributeFormatShort4;
  case DT_UBYTE4N: return MTLAttributeFormatUChar4Normalized;
  case DT_SHORT2N: return MTLAttributeFormatShort2Normalized;
  case DT_SHORT4N: return MTLAttributeFormatShort4Normalized;
  case DT_USHORT2N: return MTLAttributeFormatUShort2Normalized;
  case DT_USHORT4N: return MTLAttributeFormatUShort4Normalized;
  case DT_DEC3N: return MTLAttributeFormatInt1010102Normalized;
  case DT_UDEC3: *is_int = true; return MTLAttributeFormatUInt1010102Normalized;
  case DT_FLOAT16_2: return MTLAttributeFormatHalf2;
  case DT_FLOAT16_4: return MTLAttributeFormatHalf4;
  default: return MTLAttributeFormatInvalid;
  }
}

/* ---- persistent cache (same database file DXMT's D3D11 path uses for this exe; own table) ---- */
static const char k_build_stamp[] = "hbdx9 " __DATE__ " " __TIME__;
struct hb_cache_hdr { uint32_t magic, vb_slot_mask, ncbufs, nargs, lib_size, pad; char fname[64]; };
static const uint32_t kCacheMagic = 0x43394248; /* 'HB9C' */

static NSURL *hb_archive_url(const char *path) {
  return [NSClassFromString(@"NSURL") fileURLWithPath:[NSString stringWithUTF8String:path]];
}

static id<MTLBinaryArchive> hb_archive_open(hb_device *d, const char *path, NSError **error) {
  /* Direct class references to these classes made hbdx9.so fail to load under Wine. */
  MTLBinaryArchiveDescriptor *ad = [[NSClassFromString(@"MTLBinaryArchiveDescriptor") alloc] init];
  if (path) ad.url = hb_archive_url(path);
  id<MTLBinaryArchive> archive = [d->device newBinaryArchiveWithDescriptor:ad error:error];
  [ad release];
  return archive;
}

static void hb_cache_open(hb_device *d, const char *exe) {
  const char *off = getenv("DXMT_SHADER_CACHE");
  if (off && !strcmp(off, "0")) return;
  std::string dir;
  const char *env = getenv("DXMT_SHADER_CACHE_PATH");
  if (env && env[0] == '/') { dir = env; if (dir.back() != '/') dir += '/'; }
  else {
    char base[1024]; size_t n = confstr(_CS_DARWIN_USER_CACHE_DIR, base, sizeof(base));
    if (!n) return;
    dir = base; if (dir.empty() || dir.back() != '/') dir += '/';
    dir += "dxmt/"; dir += (exe && exe[0]) ? exe : "unknown.exe"; dir += "/";
  }
  for (size_t i = 1; i < dir.size(); i++) if (dir[i] == '/') { std::string sub = dir.substr(0, i); mkdir(sub.c_str(), 0755); }
  std::string path = dir + "shaders_" + std::to_string(d->metal_version) + ".db";
  /* the same open protocol as DXMT's CacheWriter: an flock'd -lock file around open + CREATE TABLE */
  std::string lock = path + "-lock";
  int fd = open(lock.c_str(), O_RDWR | O_CREAT, 0666);
  if (fd >= 0) flock(fd, LOCK_EX);
  bool ok = sqlite3_open_v2(path.c_str(), &d->cache.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK;
  char sql[256];
  if (ok) {
    sqlite3_exec(d->cache.db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(d->cache.db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS hbdx9_%d (key BLOB PRIMARY KEY, value BLOB NOT NULL);", AIRCONV_VERSION);
    ok = sqlite3_exec(d->cache.db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  }
  if (fd >= 0) { flock(fd, LOCK_UN); close(fd); }
  if (ok) { snprintf(sql, sizeof(sql), "SELECT value FROM hbdx9_%d WHERE key = ?;", AIRCONV_VERSION); ok = sqlite3_prepare_v2(d->cache.db, sql, -1, &d->cache.get, nullptr) == SQLITE_OK; }
  if (ok) { snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO hbdx9_%d (key, value) VALUES (?, ?);", AIRCONV_VERSION); ok = sqlite3_prepare_v2(d->cache.db, sql, -1, &d->cache.put, nullptr) == SQLITE_OK; }
  if (!ok) {
    WARN("shader cache %s unavailable: %s\n", path.c_str(), d->cache.db ? sqlite3_errmsg(d->cache.db) : "open");
    if (d->cache.get) sqlite3_finalize(d->cache.get); if (d->cache.put) sqlite3_finalize(d->cache.put);
    if (d->cache.db) sqlite3_close(d->cache.db);
    d->cache = hb_cache{};
    return;
  }
  d->cache.dir = dir;
  fprintf(stderr, "hbdx9-core: shader cache %s table=hbdx9_%d\n", path.c_str(), AIRCONV_VERSION);
  /* pipeline archive next to it (DXMT's D3D12 pipeline cache does the same with MTLBinaryArchive) */
  d->cache.archive_path = dir + "hbdx9_pso_" + std::to_string(d->metal_version) + ".metalarchive";
  @autoreleasepool {
    struct stat sb;
    if (stat(d->cache.archive_path.c_str(), &sb) == 0 && sb.st_size > 0)
      d->cache.archive = hb_archive_open(d, d->cache.archive_path.c_str(), nullptr);
    bool loaded = d->cache.archive != nil;
    if (!d->cache.archive) d->cache.archive = hb_archive_open(d, nullptr, nullptr);
    fprintf(stderr, "hbdx9-core: pipeline archive %s %s\n", d->cache.archive_path.c_str(),
            d->cache.archive ? (loaded ? "loaded" : "new") : "unavailable");
  }
}

/* Metal can reject an append to a loaded archive even if the same PSO
   serializes in a fresh one. Rebuild from this device's hits and misses.
   On failure, bisect the untested range and replay the accepted entries in
   a fresh trial each time. Keep the original file until a trial succeeds. */
static bool hb_archive_recover(hb_device *d, const std::string &dir, const char *trial_path) {
  auto &cache = d->cache;
  const std::string checkpoint = dir + "/accepted";
  std::vector<MTLRenderPipelineDescriptor *> accepted;
  size_t rejected = 0;
  unsigned trials = 0;
  constexpr unsigned max_trials = 32;  /* Bound save-time work if many descriptors fail. */
  auto retry = [&](auto &self, size_t first, size_t last) -> bool {
    if (trials == max_trials) return true;
    ++trials;
    @autoreleasepool {
      NSError *err = nil;
      id<MTLBinaryArchive> archive = hb_archive_open(d, nullptr, &err);
      if (!archive) {
        WARN("pipeline archive recovery open failed: %s\n", err ? [[err description] UTF8String] : "?");
        return false;
      }
      for (auto *pd : accepted) {
        if (![archive addRenderPipelineFunctionsWithDescriptor:pd error:&err]) {
          WARN("pipeline archive recovery replay failed: %s\n", err ? [[err description] UTF8String] : "?");
          [archive release];
          return false;
        }
      }
      size_t i = first;
      for (; i < last; ++i) {
        if (![archive addRenderPipelineFunctionsWithDescriptor:cache.archive_entries[i] error:&err]) break;
      }
      bool serialized = i == last && [archive serializeToURL:hb_archive_url(trial_path) error:&err];
      [archive release];
      if (serialized) {
        if (rename(trial_path, checkpoint.c_str()) != 0) return false;
        accepted.insert(accepted.end(), cache.archive_entries.begin() + first, cache.archive_entries.begin() + last);
        return true;
      }
      unlink(trial_path);
      if (last - first == 1) {
        ++rejected;
        TRACE("pipeline archive entry rejected: %s\n", err ? [[err description] UTF8String] : "?");
        return true;
      }
    }
    size_t middle = first + (last - first) / 2;
    return self(self, first, middle) && self(self, middle, last);
  };

  bool ok = retry(retry, 0, cache.archive_entries.size());
  if (ok && !accepted.empty()) {
    ok = rename(checkpoint.c_str(), cache.archive_path.c_str()) == 0;
  }
  unlink(checkpoint.c_str());
  if (ok) WARN("pipeline archive recovery: kept=%zu rejected=%zu untested=%zu trials=%u\n",
               accepted.size(), rejected, cache.archive_entries.size() - accepted.size() - rejected, trials);
  return ok;
}

/* Keep the last on-disk archive intact until Metal produces a replacement. */
static void hb_archive_save(hb_device *d) {
  auto &cache = d->cache;
  if (!cache.archive_dirty) return;
  @autoreleasepool {
    std::string dir = cache.archive_path + ".tmp-XXXXXX";
    bool saved = false;
    if (mkdtemp(dir.data())) {
      const std::string trial = dir + "/trial";
      NSError *err = nil;
      if ([cache.archive serializeToURL:hb_archive_url(trial.c_str()) error:&err]) {
        saved = rename(trial.c_str(), cache.archive_path.c_str()) == 0;
      } else {
        WARN("pipeline archive serialization failed, trying recovery: %s\n", err ? [[err description] UTF8String] : "?");
        unlink(trial.c_str());
        saved = hb_archive_recover(d, dir, trial.c_str());
      }
      unlink(trial.c_str());
      rmdir(dir.c_str());
    }
    if (!saved) WARN("pipeline archive save failed: %s\n", cache.archive_path.c_str());
  }
}

/* compile one variant through airconv (or load it from the persistent cache); nullptr on failure (logged once) */
static hb_shader_variant *hb_variant(hb_device *d, hb_shader *s, uint32_t fill_slots, const std::vector<hbdx9_vertex_element> *layout,
                                     const uint32_t *rt_formats, uint32_t flags = 0, uint32_t vs_flags = 0, const uint32_t *steps = nullptr,
                                     uint32_t depth_samplers = 0) {
  /* variant key */
  struct { uint32_t fill, nlayout; uint32_t pf[4]; uint32_t flags, vs_flags; uint32_t steps[16]; uint32_t depth; } kh{fill_slots, layout ? (uint32_t)layout->size() : 0, {0, 0, 0, 0}, flags, vs_flags, {}, depth_samplers};
  if (rt_formats) memcpy(kh.pf, rt_formats, sizeof(kh.pf));
  if (steps) memcpy(kh.steps, steps, sizeof(kh.steps));
  uint64_t key = fnv(&kh, sizeof(kh));
  if (layout) key = fnv(layout->data(), layout->size() * sizeof(hbdx9_vertex_element), key);
  for (auto *v : s->variants) if (v->key == key) return v->function ? v : nullptr;
  auto *v = new hb_shader_variant;
  v->key = key;
  s->variants.push_back(v);

  /* vertex input layout: match VS inputs to declaration elements by (usage, index) */
  std::vector<SM50_IA_INPUT_ELEMENT> elements;
  uint32_t int_inputs = 0, slot_mask = 0;
  if (layout) {
    for (uint32_t i = 0; i < s->info.input_count; i++) {
      const dxso_io_elem &in = s->info.inputs[i];
      for (const auto &e : *layout) {
        if (e.usage != in.usage || e.usage_index != in.usage_index || e.type == DT_UNUSED) continue;
        bool is_int = false;
        MTLAttributeFormat f = decl_format(e.type, &is_int);
        if (f == MTLAttributeFormatInvalid) break;
        if (is_int) int_inputs |= 1u << in.reg;
        SM50_IA_INPUT_ELEMENT el{};
        el.reg = in.reg; el.slot = e.stream; el.aligned_byte_offset = e.offset; el.format = (uint32_t)f;
        uint32_t step = (steps && e.stream < 16) ? steps[e.stream] : 0;   /* 0 per vertex, n: per n instances */
        el.step_function = step ? 1 : 0; el.step_rate = step;
        elements.push_back(el);
        slot_mask |= 1u << e.stream;
        break;
      }
    }
  }
  v->vb_slot_mask = slot_mask;
  dxso_variant dv{fill_slots, int_inputs, flags, vs_flags, depth_samplers};

  /* persistent cache */
  uint8_t ckey[CC_SHA256_DIGEST_LENGTH];
  {
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, k_build_stamp, sizeof(k_build_stamp));
    CC_SHA256_Update(&c, &d->metal_version, 4);
    CC_SHA256_Update(&c, &s->stage, 4);
    CC_SHA256_Update(&c, s->tokens.data(), (CC_LONG)(s->tokens.size() * 4));
    CC_SHA256_Update(&c, &dv, sizeof(dv));
    if (!elements.empty()) CC_SHA256_Update(&c, elements.data(), (CC_LONG)(elements.size() * sizeof(elements[0])));
    CC_SHA256_Update(&c, kh.pf, sizeof(kh.pf));
    CC_SHA256_Final(ckey, &c);
  }
  if (d->cache.get) {
    sqlite3_reset(d->cache.get); sqlite3_clear_bindings(d->cache.get);
    sqlite3_bind_blob(d->cache.get, 1, ckey, sizeof(ckey), SQLITE_STATIC);
    if (sqlite3_step(d->cache.get) == SQLITE_ROW) {
      const uint8_t *b = (const uint8_t *)sqlite3_column_blob(d->cache.get, 0);
      size_t n = (size_t)sqlite3_column_bytes(d->cache.get, 0);
      hb_cache_hdr h{};
      if (b && n >= sizeof(h)) memcpy(&h, b, sizeof(h));
      size_t need = sizeof(h) + (size_t)(h.ncbufs + h.nargs) * sizeof(MTL_SM50_SHADER_ARGUMENT) + h.lib_size;
      if (h.magic == kCacheMagic && n == need && h.vb_slot_mask == slot_mask) {
        const uint8_t *p = b + sizeof(h);
        v->cbufs.assign((const MTL_SM50_SHADER_ARGUMENT *)p, (const MTL_SM50_SHADER_ARGUMENT *)p + h.ncbufs); p += h.ncbufs * sizeof(MTL_SM50_SHADER_ARGUMENT);
        v->args.assign((const MTL_SM50_SHADER_ARGUMENT *)p, (const MTL_SM50_SHADER_ARGUMENT *)p + h.nargs); p += h.nargs * sizeof(MTL_SM50_SHADER_ARGUMENT);
        h.fname[63] = 0;
        @autoreleasepool {
          dispatch_data_t data = dispatch_data_create(p, h.lib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
          NSError *nerr = nil;
          id<MTLLibrary> lib = [d->device newLibraryWithData:data error:&nerr];
          dispatch_release(data);
          if (lib) v->function = [lib newFunctionWithName:[NSString stringWithUTF8String:h.fname]];
          [lib release];
        }
      }
    }
    sqlite3_reset(d->cache.get);
    if (v->function) { d->cache.hits++; d->variants_cached++; return v; }
    v->cbufs.clear(); v->args.clear();
    d->cache.misses++;
  }

  dxso_result *res = nullptr; char err[256] = "";
  if (dxso_translate(s->tokens.data(), s->tokens.size(), &dv, &res, err, sizeof(err))) {
    WARN("translate failed (%08x): %s\n", s->tokens[0], err); return nullptr;
  }
  size_t dxbc_size; const void *dxbc = dxso_result_dxbc(res, &dxbc_size);
  sm50_shader_t sh = nullptr; sm50_error_t e = nullptr; MTL_SHADER_REFLECTION refl{};
  if (SM50Initialize(dxbc, dxbc_size, &sh, &refl, &e)) {
    WARN("SM50Initialize failed (%08x): %s\n", s->tokens[0], SM50GetErrorMessageString(e).c_str()); SM50FreeError(e);
    dxso_result_free(res); return nullptr;
  }
  v->cbufs.resize(refl.NumConstantBuffers); v->args.resize(refl.NumArguments);
  SM50GetArgumentsInfo(sh, v->cbufs.data(), v->args.data());
  SM50_SHADER_COMMON_DATA common{}; common.type = SM50_SHADER_COMMON;
  common.metal_version = d->metal_version == 320 ? SM50_SHADER_METAL_320 : SM50_SHADER_METAL_310;
  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia{}; SM50_SHADER_PSO_PIXEL_SHADER_DATA ps{};
  void *chain = &common;
  if (s->stage == DXSO_STAGE_VERTEX) {
    ia.type = SM50_SHADER_IA_INPUT_LAYOUT; ia.next = &common; ia.slot_mask = slot_mask;
    ia.num_elements = (uint32_t)elements.size(); ia.elements = elements.data();
    chain = &ia;
  } else {
    ps.type = SM50_SHADER_PSO_PIXEL_SHADER; ps.next = &common;
    ps.sample_mask = (vs_flags & 0x80000000u) ? (vs_flags & 0x7fffffffu) : 0xffffffff;   /* (DX9FULL) MULTISAMPLEMASK */
    if (rt_formats) for (int i = 0; i < 4; i++) ps.pixel_formats[i] = rt_formats[i];
    chain = &ps;
  }
  char fname[64]; snprintf(fname, sizeof(fname), "hb%s_%016llx", s->stage ? "ps" : "vs", (unsigned long long)fnv(s->tokens.data(), s->tokens.size() * 4, key));
  sm50_bitcode_t bc = nullptr;
  if (SM50Compile(sh, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)chain, fname, &bc, &e)) {
    WARN("SM50Compile failed (%08x): %s\n", s->tokens[0], SM50GetErrorMessageString(e).c_str()); SM50FreeError(e);
    SM50Destroy(sh); dxso_result_free(res); return nullptr;
  }
  SM50_COMPILED_BITCODE out{}; SM50GetCompiledBitcode(bc, &out);
  @autoreleasepool {
    dispatch_data_t data = dispatch_data_create(out.Data, out.Size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *nerr = nil;
    id<MTLLibrary> lib = [d->device newLibraryWithData:data error:&nerr];
    dispatch_release(data);
    if (lib) v->function = [lib newFunctionWithName:[NSString stringWithUTF8String:fname]];
    if (!v->function) WARN("library/function failed (%08x): %s\n", s->tokens[0], nerr ? [[nerr description] UTF8String] : "?");
    [lib release];
  }
  d->variants_compiled++;
  if (v->function && d->cache.put) {
    hb_cache_hdr h{kCacheMagic, slot_mask, (uint32_t)v->cbufs.size(), (uint32_t)v->args.size(), (uint32_t)out.Size, 0, {}};
    snprintf(h.fname, sizeof(h.fname), "%s", fname);
    std::vector<uint8_t> blob(sizeof(h));
    memcpy(blob.data(), &h, sizeof(h));
    blob.insert(blob.end(), (const uint8_t *)v->cbufs.data(), (const uint8_t *)(v->cbufs.data() + v->cbufs.size()));
    blob.insert(blob.end(), (const uint8_t *)v->args.data(), (const uint8_t *)(v->args.data() + v->args.size()));
    blob.insert(blob.end(), (const uint8_t *)out.Data, (const uint8_t *)out.Data + out.Size);
    sqlite3_reset(d->cache.put); sqlite3_clear_bindings(d->cache.put);
    sqlite3_bind_blob(d->cache.put, 1, ckey, sizeof(ckey), SQLITE_STATIC);
    sqlite3_bind_blob(d->cache.put, 2, blob.data(), (int)blob.size(), SQLITE_STATIC);
    if (sqlite3_step(d->cache.put) == SQLITE_DONE) d->cache.writes++;
    sqlite3_reset(d->cache.put);
  }
  TRACE("variant %s fill=%x elements=%zu dxbc=%zu metallib=%zu cbufs=%zu args=%zu\n", fname, fill_slots, elements.size(),
        dxbc_size, (size_t)out.Size, v->cbufs.size(), v->args.size());
  SM50DestroyBitcode(bc); SM50Destroy(sh); dxso_result_free(res);
  return v->function ? v : nullptr;
}

static hb_shader *hb_ffp_vs(hb_device *d, const FfpVsKey &k) {
  for (auto &e : d->ffp_vs) if (e.first == k) return e.second;
  auto tok = ffp_vs_tokens(k);
  char err[256];
  hb_shader *s = hb_shader_from_tokens(tok.data(), tok.size(), DXSO_STAGE_VERTEX, err, sizeof(err));
  if (!s) WARN("ffp vs failed: %s\n", err);
  d->ffp_vs.push_back({k, s});
  return s;
}
static hb_shader *hb_ffp_ps(hb_device *d, const FfpPsKey &k) {
  for (auto &e : d->ffp_ps) if (e.first == k) return e.second;
  auto tok = ffp_ps_tokens(k);
  char err[256];
  hb_shader *s = hb_shader_from_tokens(tok.data(), tok.size(), DXSO_STAGE_PIXEL, err, sizeof(err));
  if (!s) WARN("ffp ps failed: %s\n", err);
  d->ffp_ps.push_back({k, s});
  return s;
}

/* ================================================================ state objects ============================ */
static MTLBlendFactor blend_factor(uint32_t b) {
  switch (b) {
  case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor;
  case 4: return MTLBlendFactorOneMinusSourceColor; case 5: return MTLBlendFactorSourceAlpha;
  case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha;
  case 8: return MTLBlendFactorOneMinusDestinationAlpha; case 9: return MTLBlendFactorDestinationColor;
  case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated;
  case 14: return MTLBlendFactorBlendColor; case 15: return MTLBlendFactorOneMinusBlendColor;
  case 16: return MTLBlendFactorSource1Color; case 17: return MTLBlendFactorOneMinusSource1Color;
  default: return MTLBlendFactorOne;
  }
}
static MTLBlendOperation blend_op(uint32_t o) {
  switch (o) {
  case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract;
  case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; default: return MTLBlendOperationAdd;
  }
}
static MTLCompareFunction cmp_func(uint32_t f) {
  switch (f) {
  case 1: return MTLCompareFunctionNever; case 2: return MTLCompareFunctionLess; case 3: return MTLCompareFunctionEqual;
  case 4: return MTLCompareFunctionLessEqual; case 5: return MTLCompareFunctionGreater; case 6: return MTLCompareFunctionNotEqual;
  case 7: return MTLCompareFunctionGreaterEqual; default: return MTLCompareFunctionAlways;
  }
}
static MTLStencilOperation stencil_op(uint32_t o) {
  switch (o) {
  case 2: return MTLStencilOperationZero; case 3: return MTLStencilOperationReplace; case 4: return MTLStencilOperationIncrementClamp;
  case 5: return MTLStencilOperationDecrementClamp; case 6: return MTLStencilOperationInvert; case 7: return MTLStencilOperationIncrementWrap;
  case 8: return MTLStencilOperationDecrementWrap; default: return MTLStencilOperationKeep;
  }
}
static MTLSamplerAddressMode address_mode(uint32_t a) {
  switch (a) {
  case 2: return MTLSamplerAddressModeMirrorRepeat; case 3: return MTLSamplerAddressModeClampToEdge;
  case 4: return MTLSamplerAddressModeClampToBorderColor; case 5: return MTLSamplerAddressModeMirrorClampToEdge;
  default: return MTLSamplerAddressModeRepeat;
  }
}

static id<MTLSamplerState> hb_sampler(hb_device *d, uint32_t s, bool compare = false, uint32_t tex_lod = 0) {
  const uint32_t *ss = d->st.ss[s];
  /* (DX9FULL 13.11.2026) D3DSAMP_BORDERCOLOR for D3DTADDRESS_BORDER: Metal offers three fixed border colours only
     (MTLSamplerBorderColor), so the D3D9 ARGB colour goes to the nearest of them; 0 = not used (key unchanged from s8j) */
  uint32_t border = 0;
  if (d->fmt_ext && (ss[SAMP_ADDRESSU] == 4 || ss[SAMP_ADDRESSV] == 4 || ss[SAMP_ADDRESSW] == 4)) {
    uint32_t c = ss[SAMP_BORDERCOLOR], a = c >> 24, l = ((c >> 16 & 0xff) * 3 + (c >> 8 & 0xff) * 6 + (c & 0xff)) / 10;
    border = a < 0x80 ? 1 : l < 0x80 ? 2 : 3;   /* 1 transparent black, 2 opaque black, 3 opaque white */
  }
  uint32_t key[9] = {ss[SAMP_ADDRESSU], ss[SAMP_ADDRESSV], ss[SAMP_ADDRESSW], ss[SAMP_MAGFILTER], ss[SAMP_MINFILTER],
                     ss[SAMP_MIPFILTER], ss[SAMP_MAXANISOTROPY], std::max(ss[SAMP_MAXMIPLEVEL], tex_lod), (uint32_t)compare | border << 4};
  /* (DX9FULL v12) the bound texture's SetLOD clamps like MAXMIPLEVEL: the most detailed level sampled is the larger of the two */
  uint64_t h = fnv(key, sizeof(key));
  auto it = d->samplers.find(h);
  if (it != d->samplers.end()) return it->second;
  MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
  sd.sAddressMode = address_mode(key[0]); sd.tAddressMode = address_mode(key[1]); sd.rAddressMode = address_mode(key[2]);
  sd.magFilter = key[3] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  sd.minFilter = key[4] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  sd.mipFilter = key[5] == 0 ? MTLSamplerMipFilterNotMipmapped : key[5] == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
  if (key[4] == 3 /* ANISOTROPIC */ || key[3] == 3) sd.maxAnisotropy = std::max<uint32_t>(1, std::min<uint32_t>(16, key[6]));
  sd.lodMinClamp = (float)key[7];
  if (border) {
    sd.borderColor = border == 1 ? MTLSamplerBorderColorTransparentBlack : border == 2 ? MTLSamplerBorderColorOpaqueBlack
                                                                         : MTLSamplerBorderColorOpaqueWhite;
    d->ext_border_samplers++;
  }
  if (compare) sd.compareFunction = MTLCompareFunctionLessEqual;
  /* session 7 diagnostics for the shadow-map draws (HBDX9_SERIAL=10 isolates them and the world tape becomes exact):
     =15 the depth compare always passes (the result no longer depends on the shadow map's contents), =16 the compare is
     point-sampled (no hardware PCF filtering) */
  if (compare && d->serial_debug == 15) sd.compareFunction = MTLCompareFunctionAlways;
  if (compare && d->serial_debug == 16) { sd.magFilter = MTLSamplerMinMagFilterNearest; sd.minFilter = MTLSamplerMinMagFilterNearest; }
  if (compare && (d->serial_debug == 15 || d->serial_debug == 16)) d->diag_iso++;
  sd.supportArgumentBuffers = YES;
  id<MTLSamplerState> st = [d->device newSamplerStateWithDescriptor:sd];
  [sd release];
  d->samplers[h] = st;
  return st;
}

static id<MTLDepthStencilState> hb_dss(hb_device *d, bool has_depth, bool has_stencil) {
  const uint32_t *rs = d->st.rs;
  bool z = has_depth && rs[RS_ZENABLE], s = has_stencil && rs[RS_STENCILENABLE];
  uint32_t key[14] = {z, z && rs[RS_ZWRITEENABLE], z ? rs[RS_ZFUNC] : 0, s, s ? rs[RS_STENCILFUNC] : 0, s ? rs[RS_STENCILFAIL] : 0,
                      s ? rs[RS_STENCILZFAIL] : 0, s ? rs[RS_STENCILPASS] : 0, s ? rs[RS_STENCILMASK] : 0,
                      s ? rs[RS_STENCILWRITEMASK] : 0, s && rs[RS_TWOSIDEDSTENCILMODE], s ? rs[RS_CCW_STENCILFUNC] : 0,
                      s ? rs[RS_CCW_STENCILPASS] : 0, s ? rs[RS_CCW_STENCILFAIL] * 16 + rs[RS_CCW_STENCILZFAIL] : 0};
  uint64_t h = fnv(key, sizeof(key));
  auto it = d->dss.find(h);
  if (it != d->dss.end()) return it->second;
  MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
  dd.depthCompareFunction = z ? cmp_func(key[2]) : MTLCompareFunctionAlways;
  dd.depthWriteEnabled = key[1] ? YES : NO;
  if (s) {
    MTLStencilDescriptor *f = [[MTLStencilDescriptor alloc] init];
    f.stencilCompareFunction = cmp_func(key[4]); f.stencilFailureOperation = stencil_op(key[5]);
    f.depthFailureOperation = stencil_op(key[6]); f.depthStencilPassOperation = stencil_op(key[7]);
    f.readMask = key[8]; f.writeMask = key[9];
    dd.frontFaceStencil = f;
    if (key[10]) {
      MTLStencilDescriptor *b = [[MTLStencilDescriptor alloc] init];
      b.stencilCompareFunction = cmp_func(key[11]); b.depthStencilPassOperation = stencil_op(key[12]);
      b.stencilFailureOperation = stencil_op(rs[RS_CCW_STENCILFAIL]); b.depthFailureOperation = stencil_op(rs[RS_CCW_STENCILZFAIL]);
      b.readMask = key[8]; b.writeMask = key[9];
      dd.backFaceStencil = b; [b release];
    } else dd.backFaceStencil = f;
    [f release];
  }
  id<MTLDepthStencilState> st = [d->device newDepthStencilStateWithDescriptor:dd];
  [dd release];
  d->dss[h] = st;
  return st;
}

/* ================================================================ render pass ============================== */
static id<MTLTexture> rt_texture(hb_resource *r) { return r ? r->texture : nil; }

static id<MTLTexture> hb_srgb_view(hb_resource *r) {
  if (!r->srgb_view && r->fi.srgb_pf != MTLPixelFormatInvalid)
    r->srgb_view = [r->texture newTextureViewWithPixelFormat:r->fi.srgb_pf];
  return r->srgb_view ? r->srgb_view : r->texture;
}
static bool hb_srgb_write(hb_device *d) { return d->st.rs[RS_SRGBWRITEENABLE] != 0; }
/* pixel format a draw renders with into rt i (sRGB view when SRGBWRITEENABLE and the format has one) */
static MTLPixelFormat hb_rt_pf(hb_device *d, hb_resource *r) {
  return hb_srgb_write(d) && r->fi.srgb_pf != MTLPixelFormatInvalid ? r->fi.srgb_pf : r->fi.pf;
}

static bool hb_begin_pass(hb_device *d, const uint32_t *clear_color_flags, const float *color, float z, uint32_t stencil, uint32_t clear_flags) {
  hb_state &st = d->st;
  bool srgb = hb_srgb_write(d);
  bool same = d->enc && d->enc_ds == st.ds && d->enc_srgb == srgb;
  uint32_t det = d->want_detach;
  for (int i = 0; i < 4; i++) same = same && d->enc_rt[i] == (det >> i & 1 ? nullptr : st.rt[i]) && d->enc_rt_level[i] == st.rt_level[i];
  same = same && d->enc_detach == det;
  if (same && !clear_flags) return true;
  hb_end_pass(d);
  if (!st.rt[0] && !st.ds) return false;
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  for (int i = 0; i < 4; i++) {
    if (!st.rt[i] || !st.rt[i]->texture || (det >> i & 1)) continue;
    bool cube = st.rt[i]->kind == HBDX9_RES_TEXTURE_CUBE;
    rp.colorAttachments[i].texture = srgb ? hb_srgb_view(st.rt[i]) : rt_texture(st.rt[i]);
    rp.colorAttachments[i].level = cube ? st.rt_level[i] & 0xffff : st.rt_level[i];
    if (cube) rp.colorAttachments[i].slice = st.rt_level[i] >> 16;
    if (st.rt[i]->autogen) st.rt[i]->mips_dirty = true;
    rp.colorAttachments[i].storeAction = MTLStoreActionStore;
    if ((clear_flags & 1) && clear_color_flags && clear_color_flags[i]) {
      rp.colorAttachments[i].loadAction = MTLLoadActionClear;
      rp.colorAttachments[i].clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);
    } else rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
    st.rt[i]->last_serial = d->serial;
    st.rt[i]->pass_written = rp.colorAttachments[i].loadAction == MTLLoadActionClear;
    st.rt[i]->pass_sampled = false;
  }
  if (st.ds) {
    rp.depthAttachment.texture = st.ds->texture;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.depthAttachment.loadAction = (clear_flags & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.clearDepth = z;
    if (st.ds->fi.stencil) {
      rp.stencilAttachment.texture = st.ds->texture;
      rp.stencilAttachment.storeAction = MTLStoreActionStore;
      rp.stencilAttachment.loadAction = (clear_flags & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
      rp.stencilAttachment.clearStencil = stencil;
    }
    st.ds->last_serial = d->serial;
    st.ds->pass_written = (clear_flags & 6) != 0;
    st.ds->pass_sampled = false;
  }
  if (d->vis) rp.visibilityResultBuffer = d->vis;
  d->enc = [[hb_cmd(d) renderCommandEncoderWithDescriptor:rp] retain];
  d->diag_passes++;
  if (d->serial_debug == 9 && d->pass_fence_set) { [d->enc waitForFence:d->pass_fence beforeStages:MTLRenderStageVertex]; d->diag_fence_waits++; }
  for (int i = 0; i < 4; i++) { d->enc_rt[i] = det >> i & 1 ? nullptr : st.rt[i]; d->enc_rt_level[i] = st.rt_level[i]; }
  d->enc_ds = st.ds; d->enc_srgb = srgb; d->enc_detach = det; if (det) d->detach_passes++;
  d->enc_fresh = true; d->enc_pso = nil; d->enc_dss = nil; d->class_in_pass = 0;
  if (d->query_active) {
    /* an occlusion query spanning passes: one visibility slot per pass, summed at completion */
    uint32_t off = (d->vis_next++ % 8192) * 8;
    d->query.offsets.push_back(off);
    [d->enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:off];
  }
  return true;
}

/* ================================================================ clear ==================================== */
static id<MTLRenderPipelineState> hb_clear_pso(hb_device *d) {
  MTLPixelFormat pf[4] = {}; MTLPixelFormat df = MTLPixelFormatInvalid;
  for (int i = 0; i < 4; i++)
    if (d->enc_rt[i] && d->enc_rt[i]->texture) pf[i] = d->enc_srgb && d->enc_rt[i]->fi.srgb_pf != MTLPixelFormatInvalid ? d->enc_rt[i]->fi.srgb_pf : d->enc_rt[i]->fi.pf;
  if (d->enc_ds) df = d->enc_ds->fi.pf;
  uint32_t samples = d->enc_rt[0] ? d->enc_rt[0]->samples : d->enc_ds ? d->enc_ds->samples : 1;
  uint64_t key = fnv(pf, sizeof(pf), fnv(&df, sizeof(df), samples));
  auto it = d->clear_psos.find(key);
  if (it != d->clear_psos.end()) return it->second;
  static const char *msl =
      "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
      "vertex V hb_clear_vs(uint id [[vertex_id]], constant float4 &c [[buffer(1)]]) {\n"
      "  V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2.0 - 1.0, c.x, 1.0); return o; }\n"
      "struct F { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };\n"
      "fragment F hb_clear_fs(constant float4 &c [[buffer(0)]]) { F f; f.c0 = c; f.c1 = c; f.c2 = c; f.c3 = c; return f; }\n";
  NSError *err = nil;
  id<MTLLibrary> lib = [d->device newLibraryWithSource:[NSString stringWithUTF8String:msl] options:nil error:&err];
  MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
  pd.vertexFunction = [[lib newFunctionWithName:@"hb_clear_vs"] autorelease];
  pd.fragmentFunction = [[lib newFunctionWithName:@"hb_clear_fs"] autorelease];
  for (int i = 0; i < 4; i++) pd.colorAttachments[i].pixelFormat = pf[i];
  pd.depthAttachmentPixelFormat = df; pd.rasterSampleCount = samples;
  if (d->enc_ds && d->enc_ds->fi.stencil) pd.stencilAttachmentPixelFormat = df;
  id<MTLRenderPipelineState> pso = [d->device newRenderPipelineStateWithDescriptor:pd error:&err];
  [pd release]; [lib release];
  if (!pso) WARN("clear pso: %s\n", err ? [[err description] UTF8String] : "?");
  d->clear_psos[key] = pso;
  return pso;
}

static void hb_clear(hb_device *d, const hbdx9_pkt_clear *c, const hbdx9_rect *rects) {
  hb_state &st = d->st;
  float color[4] = {((c->color >> 16) & 0xff) / 255.0f, ((c->color >> 8) & 0xff) / 255.0f, (c->color & 0xff) / 255.0f,
                    ((c->color >> 24) & 0xff) / 255.0f};
  uint32_t flags = (c->flags & 1 ? 1 : 0) | (c->flags & 2 && st.ds ? 2 : 0) | (c->flags & 4 && st.ds && st.ds->fi.stencil ? 4 : 0);
  if (!flags || !c->rect_count) return;
  hb_resource *ref = st.rt[0] ? st.rt[0] : st.ds;
  uint32_t lvl = st.rt[0] ? st.rt_level[0] & 0xffff : 0;
  uint32_t w = std::max<uint32_t>(1, ref->width >> lvl), h = std::max<uint32_t>(1, ref->height >> lvl);
  bool scissor = st.rs[RS_SCISSORTESTENABLE];
  const hbdx9_rect *r0 = &rects[0];
  bool full = c->rect_count == 1 && r0->x1 <= 0 && r0->y1 <= 0 && (uint32_t)r0->x2 >= w && (uint32_t)r0->y2 >= h &&
              (!scissor || (st.scissor.x1 <= 0 && st.scissor.y1 <= 0 && (uint32_t)st.scissor.x2 >= w && (uint32_t)st.scissor.y2 >= h));
  if (full) {
    uint32_t all[4] = {1, 1, 1, 1};
    hb_begin_pass(d, all, color, c->z, c->stencil, flags);   /* load-action clear; the pass stays open for the draws */
    return;
  }
  if (!(flags & 1)) { TRACE("partial depth/stencil clear not implemented\n"); return; }
  if (!hb_begin_pass(d, nullptr, nullptr, 0, 0, 0)) return;
  id<MTLRenderPipelineState> pso = hb_clear_pso(d);
  if (!pso) return;
  [d->enc setRenderPipelineState:pso]; d->enc_pso = nil;
  if (d->query_active) [d->enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
  [d->enc setDepthStencilState:hb_dss(d, false, false)]; d->enc_dss = nil;
  [d->enc setCullMode:MTLCullModeNone];
  [d->enc setFragmentBytes:color length:sizeof(color) atIndex:0];
  float zv[4] = {c->z, 0, 0, 0};
  [d->enc setVertexBytes:zv length:sizeof(zv) atIndex:1];
  MTLViewport vp = {0, 0, (double)w, (double)h, 0, 1};
  [d->enc setViewport:vp];
  for (uint32_t i = 0; i < c->rect_count; i++) {
    int32_t x1 = std::max(rects[i].x1, 0), y1 = std::max(rects[i].y1, 0);
    int32_t x2 = std::min<int32_t>(rects[i].x2, (int32_t)w), y2 = std::min<int32_t>(rects[i].y2, (int32_t)h);
    if (scissor) { x1 = std::max(x1, st.scissor.x1); y1 = std::max(y1, st.scissor.y1); x2 = std::min(x2, st.scissor.x2); y2 = std::min(y2, st.scissor.y2); }
    if (x2 <= x1 || y2 <= y1) continue;
    MTLScissorRect sr = {(NSUInteger)x1, (NSUInteger)y1, (NSUInteger)(x2 - x1), (NSUInteger)(y2 - y1)};
    [d->enc setScissorRect:sr];
    [d->enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  if (d->query_active && !d->query.offsets.empty())
    [d->enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:d->query.offsets.back()];
  for (int i = 0; i < 4; i++) if (st.rt[i]) st.rt[i]->pass_written = true;   /* the clear quad writes every bound target */
  d->enc_fresh = true; /* viewport/scissor/cull must be re-applied by the next draw */
}

/* ================================================================ draw ===================================== */
static uint32_t prim_vertex_count(uint32_t pt, uint32_t prims) {
  switch (pt) {
  case PT_POINTLIST: return prims; case PT_LINELIST: return prims * 2; case PT_LINESTRIP: return prims + 1;
  case PT_TRIANGLELIST: return prims * 3; case PT_TRIANGLESTRIP: case PT_TRIANGLEFAN: return prims + 2;
  default: return 0;
  }
}
static MTLPrimitiveType prim_type(uint32_t pt) {
  switch (pt) {
  case PT_POINTLIST: return MTLPrimitiveTypePoint; case PT_LINELIST: return MTLPrimitiveTypeLine;
  case PT_LINESTRIP: return MTLPrimitiveTypeLineStrip; case PT_TRIANGLESTRIP: return MTLPrimitiveTypeTriangleStrip;
  default: return MTLPrimitiveTypeTriangle;
  }
}

/* constant bank snapshot (with the shader's def values) -> ring; returns gpu address */
static int hb_cb_guard(void) { static int g = -1; if (g < 0) { const char *e = getenv("HBDX9_CB_GUARD"); g = e && e[0] == '1'; } return g; }
static uint64_t hb_upload_bank(hb_device *d, const void *bank, uint32_t bytes, const std::vector<dxso_def> &defs, uint32_t kind) {
  id<MTLBuffer> buf; uint32_t off; uint64_t gpu;
  uint8_t *p;
  if (hb_cb_guard()) {
    /* diagnostic (session 6): zeroed guard of one bank size on both sides, so a relative index outside the bank reads 0
       instead of whatever the ring holds next to it */
    p = hb_alloc(d, bytes * 3, 256, &buf, &off, &gpu);
    memset(p, 0, bytes); memset(p + 2 * bytes, 0, bytes);
    p += bytes; gpu += bytes;
  } else p = hb_alloc(d, bytes, 256, &buf, &off, &gpu);
  memcpy(p, bank, bytes);
  for (const auto &df : defs) {
    if (df.kind != kind || (uint32_t)(df.reg + 1) * 16 > bytes) continue;
    if (kind == DXSO_DEF_BOOL) { uint32_t v[4] = {df.value[0] ? 1u : 0u, 0, 0, 0}; memcpy(p + df.reg * 16, v, 16); }
    else memcpy(p + df.reg * 16, df.value, 16);
  }
  return gpu;
}

static void mat_mul(const float *a, const float *b, float *o) {
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
    float s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c]; o[r * 4 + c] = s;
  }
}

struct DrawGeometry {
  bool up; const uint8_t *up_data; uint32_t up_bytes, up_stride;
};

static bool hb_bind_stage(hb_device *d, hb_shader_variant *v, bool vertex, const hb_shader *s, const void *fbank, uint32_t fbytes,
                          const void *ibank, const void *bbank, const void *cb3, uint32_t cb3_bytes,
                          std::vector<id<MTLResource>> &used_res, std::vector<id<MTLTexture>> &used_tex) {
  hb_state &st = d->st;
  id<MTLRenderCommandEncoder> e = d->enc;
  /* constant buffers */
  if (!v->cbufs.empty()) {
    uint32_t n = 0; for (auto &a : v->cbufs) n = std::max(n, a.StructurePtrOffset + 1);
    id<MTLBuffer> tb; uint32_t toff;
    uint64_t *table = (uint64_t *)hb_alloc(d, n * 8, 256, &tb, &toff, nullptr);
    for (auto &a : v->cbufs) {
      uint64_t addr = 0;
      if (a.SM50BindingSlot == 0) addr = hb_upload_bank(d, fbank, fbytes, s->defs, DXSO_DEF_FLOAT);
      else if (a.SM50BindingSlot == 3) {
        id<MTLBuffer> rb; uint32_t roff;
        void *p = hb_alloc(d, cb3_bytes, 256, &rb, &roff, &addr);
        memcpy(p, cb3, cb3_bytes);
      } else if (a.SM50BindingSlot == 1) {
        int32_t ib[16][4]; memcpy(ib, ibank, sizeof(ib));
        addr = hb_upload_bank(d, ib, sizeof(ib), s->defs, DXSO_DEF_INT);
      } else {
        uint32_t bb[16][4] = {}; const uint32_t *src = (const uint32_t *)bbank;
        for (int i = 0; i < 16; i++) bb[i][0] = src[i] ? 1 : 0;
        addr = hb_upload_bank(d, bb, sizeof(bb), s->defs, DXSO_DEF_BOOL);
      }
      table[a.StructurePtrOffset] = addr;
    }
    if (vertex) [e setVertexBuffer:tb offset:toff atIndex:SM50_BINDING_INDEX_CONSTANT_BUFFER];
    else [e setFragmentBuffer:tb offset:toff atIndex:SM50_BINDING_INDEX_CONSTANT_BUFFER];
  }
  /* textures and samplers */
  if (!v->args.empty()) {
    uint32_t n = 0; for (auto &a : v->args) n = std::max(n, a.StructurePtrOffset + 3);
    id<MTLBuffer> tb; uint32_t toff;
    uint64_t *table = (uint64_t *)hb_alloc(d, n * 8, 256, &tb, &toff, nullptr);
    memset(table, 0, n * 8);
    for (auto &a : v->args) {
      uint32_t slot = a.SM50BindingSlot + (vertex ? 16 : 0);
      if (slot >= 20) continue;
      if (a.Type == SM50BindingType::Sampler) {
        /* compare sampler: shadow bit only (both bits = DF16/DF24 raw depth, DX9FULL) */
        id<MTLSamplerState> smp = hb_sampler(d, slot, !vertex && slot < 16 && (d->cur_depth_samplers >> slot & 1) &&
                                                      !(d->cur_depth_samplers >> (16 + slot) & 1), st.tex[slot] ? st.tex[slot]->lod : 0);
        uint64_t id_ = [smp gpuResourceID]._impl;
        float bias; memcpy(&bias, &st.ss[slot][SAMP_MIPMAPLODBIAS], 4);
        table[a.StructurePtrOffset] = id_; table[a.StructurePtrOffset + 1] = id_;
        table[a.StructurePtrOffset + 2] = (uint64_t)*(uint32_t *)&bias;
      } else if (a.Type == SM50BindingType::SRV) {
        hb_resource *r = st.tex[slot];
        id<MTLTexture> t = r ? (r->sample_view ? r->sample_view : r->texture) : d->dummy_tex->texture;
        if (r && st.ss[slot][SAMP_SRGBTEXTURE] && r->fi.srgb_pf != MTLPixelFormatInvalid) {
          if (!r->srgb_sample_view) {
            MTLTextureType tt = r->texture.textureType;
            NSUInteger slices = tt == MTLTextureTypeCube ? 6 : 1;
            r->srgb_sample_view = [r->texture newTextureViewWithPixelFormat:r->fi.srgb_pf textureType:tt levels:NSMakeRange(0, r->levels)
                                                                      slices:NSMakeRange(0, slices) swizzle:r->fi.has_swz ? r->fi.swz : MTLTextureSwizzleChannelsDefault];
          }
          if (r->srgb_sample_view) t = r->srgb_sample_view;
        }
        if (r) r->last_serial = d->serial;
        if (r) {
          /* session 6 diagnostic: a texture sampled while attached to the open pass (D3D9: undefined; on a tiled GPU the
             sampled memory is what the last pass stored, racing with this pass's tile stores) */
          bool as_rt = false; for (int k = 0; k < 4; k++) as_rt |= d->enc_rt[k] == r;
          bool as_ds = d->enc_ds == r;
          if (as_rt || as_ds) {
            d->fb.draws++; if (as_rt) d->fb.color++; if (as_ds) { d->fb.depth++; if (st.rs[RS_ZWRITEENABLE] && st.rs[RS_ZENABLE]) d->fb.depth_written++; }
            if (d->fb.first_frames.size() < 12 && (d->fb.first_frames.empty() || d->fb.first_frames.back() != d->frames_presented))
              d->fb.first_frames.push_back(d->frames_presented);
          }
        }
        table[a.StructurePtrOffset] = [t gpuResourceID]._impl;
        table[a.StructurePtrOffset + 1] = (uint64_t)(r && r->kind == HBDX9_RES_TEXTURE_CUBE ? 6 : 1) << 32;
        used_tex.push_back(t);
      }
    }
    if (vertex) [e setVertexBuffer:tb offset:toff atIndex:SM50_BINDING_INDEX_ARGUMENT_TABLE];
    else [e setFragmentBuffer:tb offset:toff atIndex:SM50_BINDING_INDEX_ARGUMENT_TABLE];
  }
  (void)used_res;
  return true;
}

static bool mat_inv4(const float *m, float *out) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (det == 0) return false;
  for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
  return true;
}
static void color4(uint32_t c, float *o) {
  o[0] = ((c >> 16) & 0xff) / 255.0f; o[1] = ((c >> 8) & 0xff) / 255.0f; o[2] = (c & 0xff) / 255.0f; o[3] = ((c >> 24) & 0xff) / 255.0f;
}
static float rsf(const uint32_t *rs, uint32_t i) { float f; memcpy(&f, &rs[i], 4); return f; }
static uint32_t rt_mip(uint32_t level) { return level & 0xffff; }

/* wined3d utils.c compute_texture_matrix: the FFP's texture coordinate transform as a plain 4x4 (row vector) matrix */
static void ffp_tex_matrix(const float *M, uint32_t flags, uint32_t attrib_count, float *m) {
  uint32_t count = flags & ~0x100u;
  static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (count < 2 || count > 4) {
    memcpy(m, ident, 64);
    if (attrib_count < 4) m[15] = 0;
    if (flags & 0x100) { if (attrib_count == 1) m[3] = 1; else if (attrib_count == 2) m[7] = 1; else if (attrib_count == 3) m[11] = 1; }
    return;
  }
  memcpy(m, M, 64);
  if (attrib_count == 1) { m[12] = m[4]; m[13] = m[5]; m[14] = m[6]; m[15] = m[7]; }
  else if (attrib_count == 2) { m[12] = m[8]; m[13] = m[9]; m[14] = m[10]; m[15] = m[11]; }
  if (count < 4) { m[3] = m[7] = m[11] = m[15] = 0; }
  if (count < 3) { m[2] = m[6] = m[10] = m[14] = 0; }
  if (flags & 0x100) {
    if (count == 2) { m[3] = m[1]; m[7] = m[5]; m[11] = m[9]; m[15] = m[13]; }
    else if (count == 3) { m[3] = m[2]; m[7] = m[6]; m[11] = m[10]; m[15] = m[14]; }
  }
}
static uint32_t decl_components(uint8_t t) {
  switch (t) {
  case DT_FLOAT1: return 1; case DT_FLOAT2: case DT_SHORT2: case DT_SHORT2N: case DT_USHORT2N: case DT_FLOAT16_2: return 2;
  case DT_FLOAT3: case DT_UDEC3: case DT_DEC3N: return 3; default: return 4;
  }
}

static void hb_draw(hb_device *d, uint32_t pt, uint32_t prims, uint32_t start_vertex, bool indexed, int32_t base_vertex,
                    uint32_t start_index, const DrawGeometry &g, const uint8_t *up_index, uint32_t up_index_format) {
  hb_state &st = d->st;
  const uint32_t *rs = st.rs;
  uint32_t nverts = prim_vertex_count(pt, prims);
  if (!nverts) return;
  uint64_t prof_t = d->prof_on ? mach_absolute_time() : 0;
#define HB_PROF(k) do { if (d->prof_on) { uint64_t n_ = mach_absolute_time(); d->prof[k] += n_ - prof_t; prof_t = n_; } } while (0)
  if (d->prof_on) d->prof_draws++;
  /* autogenerated mip chains of sampled textures are refreshed before the pass (a blit cannot run inside it) */
  for (int i = 0; i < 20; i++) {
    hb_resource *t = st.tex[i];
    if (!t || !t->mips_dirty) continue;
    bool bound_rt = false;
    for (int j = 0; j < 4; j++) bound_rt |= st.rt[j] == t;
    if (!bound_rt) hb_gen_mips(d, t);
  }
  /* session 7 diagnostics HBDX9_SERIAL=10..14: run every draw of ONE class alone in its own render pass (pass ended before and
     after it): 10 samples a depth texture (shadow map), 11 alpha test on, 12 alpha blend on, 13 depth test on with z-write
     off, 14 depth func EQUAL - which draws make the world-tape noise when they share a pass */
  bool iso = false;
  if (d->serial_debug >= 10 && d->serial_debug <= 14) {
    switch (d->serial_debug) {
    case 10: for (int j = 0; j < 20; j++) iso |= st.tex[j] && st.tex[j]->fi.depth; break;
    case 11: iso = rs[RS_ALPHATESTENABLE] != 0; break;
    case 12: iso = rs[RS_ALPHABLENDENABLE] != 0; break;
    case 13: iso = st.ds && rs[RS_ZENABLE] && !rs[RS_ZWRITEENABLE]; break;
    case 14: iso = st.ds && rs[RS_ZENABLE] && rs[RS_ZFUNC] == 3 /* D3DCMP_EQUAL */; break;
    }
    if (iso) { d->diag_iso++; if (d->enc && !d->enc_fresh) hb_end_pass(d); }
  }
  if (d->feedback_detach) {
    uint32_t det = 0;
    for (int i = 1; i < 4; i++) {
      hb_resource *t = st.rt[i];
      if (!t || !t->texture) continue;
      bool sampled = false; for (int j = 0; j < 20; j++) sampled |= st.tex[j] == t;
      bool written = (st.ps ? (st.ps->info.rt_mask >> i & 1) != 0 : false) && (rs[RS_COLORWRITEENABLE1 + i - 1] & 0xf);
      if (sampled && !written) det |= 1u << i;
    }
    d->want_detach = det; if (det) d->detach_draws++;
  }
  bool begun = hb_begin_pass(d, nullptr, nullptr, 0, 0, 0);
  if (!begun) { d->want_detach = 0; return; }
  /* (session 6) a texture sampled while attached to the open pass and already written in it: end the pass, so the sampling
     reads memory that holds every earlier draw (DXVK: barrier before a render-target hazard).  Without it the sampled
     memory lacks this pass's earlier writes, and on a tiled GPU what it holds depends on tile order.  Gothic 3: a G32R32F
     RT1 sampled while bound, up to 28 draws a frame from frame 716 (tape/hbfeedback.py). */
  if (d->feedback_barrier) {
    /* read-after-write: a sampled attachment already written in the pass; write-after-read (session 6): an attachment this
       draw may write that an earlier draw of the pass sampled (on a tiled GPU the earlier sampling of other tiles races with
       this pass's tile stores) */
    bool hazard = false;
    for (int i = 0; i < 20 && !hazard; i++) {
      hb_resource *t = st.tex[i];
      if (!t || !t->pass_written) continue;
      for (int k = 0; k < 4; k++) hazard |= d->enc_rt[k] == t;
      hazard |= d->enc_ds == t;
    }
    for (int k = 0; k < 4 && !hazard; k++) {
      hb_resource *t = d->enc_rt[k];
      uint32_t wm = k == 0 ? rs[RS_COLORWRITEENABLE] : rs[RS_COLORWRITEENABLE1 + k - 1];
      if (t && t->pass_sampled && (wm & 0xf)) hazard = true;
    }
    if (!hazard && d->enc_ds && d->enc_ds->pass_sampled && ((rs[RS_ZENABLE] && rs[RS_ZWRITEENABLE]) || rs[RS_STENCILENABLE])) hazard = true;
    if (hazard) {
      hb_end_pass(d); d->fb.splits++;
      if (!hb_begin_pass(d, nullptr, nullptr, 0, 0, 0)) { d->want_detach = 0; return; }
    }
  }
  d->want_detach = 0;   /* only this draw's pass is built without the detached targets; clears and blits see all of them */
  HB_PROF(0);   /* 0: mips, pass begin, hazard checks */

  /* vertex layout */
  std::vector<hbdx9_vertex_element> fvf_layout;
  const std::vector<hbdx9_vertex_element> *layout = &st.decl;
  if (st.use_fvf) { fvf_layout = fvf_to_elements(st.fvf); layout = &fvf_layout; }
  bool rhw = false, has_normal = false, has_psize = false;
  for (auto &e : *layout) { if (e.usage == DU_POSITIONT) rhw = true; if (e.usage == DU_NORMAL) has_normal = true; if (e.usage == DU_PSIZE) has_psize = true; }
  /* (DX9FULL 13.11.2026) point size / sprites: a non-indexed POINTLIST with sprites, scaling, a size other than 1 or per-vertex
     PSIZE (fixed function) is drawn as one instanced 4-vertex strip per point (dxso point epilogue); other point lists keep
     Metal points (size 1) as before.  Indexed point lists: not expanded (boundary). */
  bool points = d->fmt_ext && pt == PT_POINTLIST && !indexed &&
                (rs[RS_POINTSPRITEENABLE] || rs[RS_POINTSCALEENABLE] || rsf(rs, RS_POINTSIZE) != 1.0f || (!st.vs && has_psize));
  if (points) d->ext_point_draws++;

  /* fog (wined3d model: the VS emits a fog coordinate into slot 10, the PS evaluates the equation) */
  hb_shader *vs = st.vs, *ps = st.ps;
  bool ffp_vs = !vs, ffp_ps = !ps;
  uint32_t table = rs[RS_FOGTABLEMODE], vmode = rs[RS_FOGVERTEXMODE];
  auto fog_eq = [](uint32_t m) -> uint32_t { return m == 1 ? 2u : m == 2 ? 3u : 1u; }; /* D3DFOG_EXP/EXP2/LINEAR -> 2/3/1 */
  uint32_t ps_fog = 0;
  if (rs[RS_FOGENABLE] && (ffp_ps || ps->info.major < 3)) ps_fog = table ? fog_eq(table) : (!ffp_vs || rhw) ? 1 : fog_eq(vmode ? vmode : 3);
  float fog_end = 0, fog_scale = -1;
  if (table || (vmode && ffp_vs && !rhw)) {
    float fs = rsf(rs, RS_FOGSTART), fe = rsf(rs, RS_FOGEND);
    if (fs == fe && !table) { fog_end = 0; fog_scale = 0; } else { fog_end = fe; fog_scale = 1.0f / (fe - fs); }
  }
  bool ortho = st.proj[3] == 0 && st.proj[7] == 0 && st.proj[11] == 0 && st.proj[15] == 1;
  uint32_t vs_fog_src = 0; uint8_t ffp_fog = FFP_FOG_NONE;
  if (ps_fog) {
    if (!ffp_vs) {
      if (vs->info.major < 3 && table) vs_fog_src = ortho ? (rhw ? 0 : 1) : 2;
    } else if (table || vmode) {
      if (!table && rs[RS_RANGEFOGENABLE] && !rhw) ffp_fog = FFP_FOG_RANGE;
      else if (rhw) ffp_fog = FFP_FOG_RAW_Z;
      else if (ortho) vs_fog_src = 1;
      else ffp_fog = FFP_FOG_EYE_Z;
    } else ffp_fog = FFP_FOG_SPECULAR_W;
  }
  /* user clip planes: clip space for shaders; world space for fixed function (moved to clip space here) */
  uint32_t clip_mask = rs[RS_CLIPPING] ? rs[RS_CLIPPLANEENABLE] & 0x3f : 0;
  if (ffp_vs && rhw) clip_mask = 0;
  float vs_cb3[8][4] = {}; uint32_t vs_cb3_bytes = 6 * 16;   /* (DX9FULL) [6], [7]: point parameters */
  if (clip_mask) {
    if (!ffp_vs) memcpy(vs_cb3, st.clip, sizeof(st.clip));
    else {
      float vp[16], inv[16]; mat_mul(st.view, st.proj, vp);
      if (mat_inv4(vp, inv))
        for (int i = 0; i < 6; i++) for (int r = 0; r < 4; r++) {
          float sum = 0; for (int c = 0; c < 4; c++) sum += inv[r * 4 + c] * st.clip[i][c]; vs_cb3[i][r] = sum;
        }
    }
  }

  /* shaders: user or fixed function */
  float ffp_vs_bank[FFP_VS_BANK_BLEND][4] = {}; uint32_t ffp_vs_bytes = FFP_VS_BANK * 16;   /* (DX9FULL) 144 only when blending */
  float ffp_ps_bank[FFP_PS_BANK_EXT][4] = {}; uint32_t ffp_ps_bytes = 3 * 16;   /* (DX9FULL) c3..c26 only when the key uses them */
  if (ffp_vs) {
    FfpVsKey k{}; uint32_t tc = 0;
    for (auto &e : *layout) {
      if (e.usage == DU_POSITIONT) k.pretransformed = 1;
      if (e.usage == DU_COLOR && e.usage_index == 0) k.has_color0 = 1;
      if (e.usage == DU_COLOR && e.usage_index == 1) k.has_color1 = 1;
      if (e.usage == DU_TEXCOORD) tc = std::max<uint32_t>(tc, e.usage_index + 1);
    }
    k.texcoords = (uint8_t)std::min<uint32_t>(tc, 8);
    k.fog_coord = ffp_fog;
    if (points) k.point = (uint8_t)(1 | (has_psize ? 2 : 0) | (rs[RS_POINTSCALEENABLE] && !k.pretransformed ? 4 : 0));
    /* (DX9FULL) D3DRS_VERTEXBLEND 1..3 weights (2..4 world matrices), not indexed, with BLENDWEIGHT in the vertex */
    if (d->fmt_ext && !k.pretransformed && rs[RS_VERTEXBLEND] >= 1 && rs[RS_VERTEXBLEND] <= 3 && !rs[RS_INDEXEDVERTEXBLENDENABLE]) {
      bool has_w = false; for (auto &e : *layout) has_w |= e.usage == DU_BLENDWEIGHT && e.usage_index == 0;
      if (has_w) { k.blend = (uint8_t)(rs[RS_VERTEXBLEND] + 1); d->ext_blend_draws++; }
    }
    /* texture coordinates per stage (TEXCOORDINDEX source/texgen + TEXTURETRANSFORMFLAGS) */
    for (uint32_t i = 0; i < 8; i++) {
      uint32_t tci = st.tss[i][TSS_TEXCOORDINDEX], gen = (tci >> 16) & 0xF, idx = tci & 7, flags = st.tss[i][TSS_TEXTURETRANSFORMFLAGS];
      bool have = gen ? !k.pretransformed : idx < k.texcoords;
      k.tc_src[i] = have ? (uint8_t)((gen << 4) | idx) : 0xFF;
      uint32_t count = flags & ~0x100u;
      k.tc_xform[i] = have && !k.pretransformed && (((count >= 2 && count <= 4)) || (flags & 0x100));
    }
    bool texgen = false;
    for (uint32_t i = 0; i < 8; i++) if (k.tc_src[i] != 0xFF && (k.tc_src[i] >> 4)) texgen = true;
    if (texgen) { k.has_normal = has_normal; k.normalize = rs[RS_NORMALIZENORMALS] != 0; }
    if (rs[RS_LIGHTING] && !k.pretransformed) {
      k.lighting = 1; k.has_normal = has_normal; k.normalize = rs[RS_NORMALIZENORMALS] != 0;
      k.specular = rs[RS_SPECULARENABLE] != 0; k.localviewer = rs[RS_LOCALVIEWER] != 0;
      auto src = [&](uint32_t v) -> uint8_t {
        if (!rs[RS_COLORVERTEX]) return 0;
        return (v == 1 && k.has_color0) ? 1 : (v == 2 && k.has_color1) ? 2 : 0;
      };
      k.diffuse_src = src(rs[RS_DIFFUSEMATERIALSOURCE]); k.ambient_src = src(rs[RS_AMBIENTMATERIALSOURCE]);
      k.specular_src = src(rs[RS_SPECULARMATERIALSOURCE]); k.emissive_src = src(rs[RS_EMISSIVEMATERIALSOURCE]);
      k.light_count = (uint8_t)std::min<uint32_t>(st.nlights, 8);
      for (uint32_t i = 0; i < k.light_count; i++) k.light_type[i] = (uint8_t)st.lights[i].type;
    }
    vs = hb_ffp_vs(d, k);
    for (uint32_t i = 0; i < 8; i++) {
      if (!k.tc_xform[i]) continue;
      uint32_t tci = st.tss[i][TSS_TEXCOORDINDEX], attrib = 3;
      if (!(tci >> 16)) {
        attrib = 0;
        for (auto &e : *layout) if (e.usage == DU_TEXCOORD && e.usage_index == (tci & 7)) attrib = decl_components(e.type);
      }
      float m[16]; ffp_tex_matrix(st.texmat[i], st.tss[i][TSS_TEXTURETRANSFORMFLAGS], attrib, m);
      for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) ffp_vs_bank[FFP_TEXMAT_BASE + 4 * i + c][r] = m[r * 4 + c];
    }
    if (points) {
      ffp_vs_bank[FFP_VS_POINT][0] = rsf(rs, RS_POINTSIZE); ffp_vs_bank[FFP_VS_POINT][1] = rsf(rs, RS_POINTSCALE_A);
      ffp_vs_bank[FFP_VS_POINT][2] = rsf(rs, RS_POINTSCALE_B); ffp_vs_bank[FFP_VS_POINT][3] = rsf(rs, RS_POINTSCALE_C);
      ffp_vs_bank[FFP_VS_POINT + 1][0] = (float)st.vp.height;
    }
    float W = (float)st.vp.width, H = (float)st.vp.height, X = (float)st.vp.x, Y = (float)st.vp.y;
    float zr = st.vp.max_z - st.vp.min_z; if (zr == 0) zr = 1;
    float c0[4] = {2.0f / W, -2.0f / H, -1.0f - 2.0f * X / W, 1.0f + 2.0f * Y / H};
    float c1[4] = {1.0f / zr, -st.vp.min_z / zr, 0, 0};
    memcpy(ffp_vs_bank[0], c0, 16); memcpy(ffp_vs_bank[1], c1, 16);
    float white[4] = {1, 1, 1, 1}, spec0[4] = {0, 0, 0, 1}; memcpy(ffp_vs_bank[2], white, 16); memcpy(ffp_vs_bank[3], spec0, 16);
    float wv[16], wvp[16]; mat_mul(st.world, st.view, wv); mat_mul(wv, st.proj, wvp);
    if (k.blend) {   /* (DX9FULL) per matrix: world_i x view columns, normal-matrix rows; projection columns */
      for (uint32_t i = 0; i < k.blend; i++) {
        float wvi[16], wv4[16], inv[16];
        mat_mul(i ? st.worlds[i - 1] : st.world, st.view, wvi);
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) ffp_vs_bank[FFP_BLEND_WV + 4 * i + r][c] = wvi[c * 4 + r];
        memcpy(wv4, wvi, 64); wv4[3] = wv4[7] = wv4[11] = 0; wv4[12] = wv4[13] = wv4[14] = 0; wv4[15] = 1;
        if (mat_inv4(wv4, inv)) for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) ffp_vs_bank[FFP_BLEND_N + 3 * i + r][c] = inv[r * 4 + c];
      }
      for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) ffp_vs_bank[FFP_BLEND_PROJ + r][c] = st.proj[c * 4 + r];
      ffp_vs_bytes = sizeof(ffp_vs_bank);
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { ffp_vs_bank[4 + r][c] = wvp[c * 4 + r]; ffp_vs_bank[8 + r][c] = wv[c * 4 + r]; }
    if (k.lighting || texgen) {
      float wv4[16], inv[16];
      memcpy(wv4, wv, 64); wv4[3] = wv4[7] = wv4[11] = 0; wv4[12] = wv4[13] = wv4[14] = 0; wv4[15] = 1;
      if (mat_inv4(wv4, inv)) for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) ffp_vs_bank[12 + r][c] = inv[r * 4 + c];
    }
    if (k.lighting) {
      /* normal matrix: rows of inverse(world*view) (upper 3x3), so n_eye[r] = dot(n, row r) = n * inverse^T */
      float wv4[16], inv[16];
      memcpy(wv4, wv, 64); wv4[3] = wv4[7] = wv4[11] = 0; wv4[12] = wv4[13] = wv4[14] = 0; wv4[15] = 1;
      if (mat_inv4(wv4, inv)) for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) ffp_vs_bank[12 + r][c] = inv[r * 4 + c];
      memcpy(ffp_vs_bank[15], &st.material[0], 16); memcpy(ffp_vs_bank[16], &st.material[4], 16);
      memcpy(ffp_vs_bank[17], &st.material[8], 16); memcpy(ffp_vs_bank[18], &st.material[12], 16);
      ffp_vs_bank[19][0] = st.material[16];
      color4(rs[RS_AMBIENT], ffp_vs_bank[20]);
      for (uint32_t i = 0; i < k.light_count; i++) {
        const hbdx9_light &L = st.lights[i];
        float (*c)[4] = &ffp_vs_bank[FFP_LIGHT_BASE + 7 * i];
        memcpy(c[0], L.diffuse, 16); memcpy(c[1], L.specular, 16); memcpy(c[2], L.ambient, 16);
        for (int col = 0; col < 3; col++) {
          c[3][col] = L.position[0] * st.view[col] + L.position[1] * st.view[4 + col] + L.position[2] * st.view[8 + col] + st.view[12 + col];
          c[4][col] = L.direction[0] * st.view[col] + L.direction[1] * st.view[4 + col] + L.direction[2] * st.view[8 + col];
          if (L.type == 3) c[4][col] = -c[4][col];
        }
        c[3][3] = 1; c[4][3] = 0;
        c[5][0] = L.range; c[5][1] = L.attenuation0; c[5][2] = L.attenuation1; c[5][3] = L.attenuation2;
        c[6][0] = cosf(L.theta / 2.0f); c[6][1] = cosf(L.phi / 2.0f); c[6][2] = L.falloff; c[6][3] = 0;
      }
    }
  }
  if (ffp_ps) {
    FfpPsKey k{};
    uint32_t n = 0;
    for (; n < 8; n++) {
      const uint32_t *t = st.tss[n];
      if (t[TSS_COLOROP] == 1) break;
      FfpStage &s = k.stage[n];
      s.colorop = (uint8_t)t[TSS_COLOROP]; s.colorarg1 = (uint8_t)t[TSS_COLORARG1]; s.colorarg2 = (uint8_t)t[TSS_COLORARG2];
      s.alphaop = (uint8_t)t[TSS_ALPHAOP]; s.alphaarg1 = (uint8_t)t[TSS_ALPHAARG1]; s.alphaarg2 = (uint8_t)t[TSS_ALPHAARG2];
      s.texcoord = (uint8_t)n;   /* the FFP VS writes stage n's coordinate into slot n */
      s.projected = (t[TSS_TEXTURETRANSFORMFLAGS] & 0x100) != 0;
      if (d->fmt_ext) {   /* (DX9FULL) ARG0 only where an op reads it, RESULTARG only when TEMP: other keys stay as in s8j */
        if (s.colorop == 25 || s.colorop == 26) s.colorarg0 = (uint8_t)t[TSS_COLORARG0];
        if (s.alphaop == 25 || s.alphaop == 26) s.alphaarg0 = (uint8_t)t[TSS_ALPHAARG0];
        if ((t[TSS_RESULTARG] & 0xF) == 5) s.resultarg = 5;
      }
      hb_resource *tx = st.tex[n];
      s.has_texture = !tx ? 0 : tx->kind == HBDX9_RES_TEXTURE_CUBE ? 2 : tx->kind == HBDX9_RES_TEXTURE_3D ? 3 : 1;
    }
    k.stages = (uint8_t)n;
    k.specular_add = rs[RS_SPECULARENABLE] != 0;
    k.ext = d->fmt_ext ? 1 : 0;
    ps = hb_ffp_ps(d, k);
    color4(rs[RS_TEXTUREFACTOR], ffp_ps_bank[0]);
    float one[4] = {1, 1, 1, 1}, half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    memcpy(ffp_ps_bank[1], one, 16); memcpy(ffp_ps_bank[2], half, 16);
    if (k.ext) {
      bool need = false;
      for (uint32_t i = 0; i < n; i++) {
        const FfpStage &s = k.stage[i];
        auto is_c = [](uint8_t a) { return (a & 0xF) == 6; };
        need |= is_c(s.colorarg0) || is_c(s.colorarg1) || is_c(s.colorarg2) || is_c(s.alphaarg0) || is_c(s.alphaarg1) || is_c(s.alphaarg2) ||
                s.colorop == 22 || s.colorop == 23;
      }
      if (need) {
        for (uint32_t i = 0; i < 8; i++) {
          color4(st.tss[i][TSS_CONSTANT], ffp_ps_bank[FFP_PS_CONST_BASE + i]);
          float m00, m01, m10, m11;
          memcpy(&m00, &st.tss[i][TSS_BUMPENVMAT00], 4); memcpy(&m01, &st.tss[i][TSS_BUMPENVMAT01], 4);
          memcpy(&m10, &st.tss[i][TSS_BUMPENVMAT10], 4); memcpy(&m11, &st.tss[i][TSS_BUMPENVMAT11], 4);
          float *bm = ffp_ps_bank[FFP_PS_BUMPMAT_BASE + i]; bm[0] = m00; bm[1] = m10; bm[2] = m01; bm[3] = m11;
          memcpy(&ffp_ps_bank[FFP_PS_BUMPL_BASE + i][0], &st.tss[i][TSS_BUMPENVLSCALE], 4);
          memcpy(&ffp_ps_bank[FFP_PS_BUMPL_BASE + i][1], &st.tss[i][TSS_BUMPENVLOFFSET], 4);
        }
        ffp_ps_bytes = sizeof(ffp_ps_bank);
      }
    }
  }
  if (!vs || !ps) return;

  /* fixed state the pixel stage applies (alpha reference, fog, bump-env) */
  float ps_cb3[20][4] = {};
  /* alpha test precision = upstream DXVK D3D9DeviceEx::GetAlphaTestPrecision (render target 0's format): the D3D9 test
     runs in the precision of the target, so the 8-bit reference is widened to that many bits (or /255 for float) */
  uint32_t alpha_prec = 0;
  if (d->alpha_float) alpha_prec = 0xF;
  else if (d->alpha_8bit) alpha_prec = 0;
  else if (st.rt[0]) switch (st.rt[0]->format) {
    case 31: case 35: case 67: alpha_prec = 2; break;                       /* A2B10G10R10, A2R10G10B10, A2W10V10U10 */
    case 111: case 112: case 113: alpha_prec = 7; break;                    /* R16F, G16R16F, A16B16G16R16F */
    case 34: case 36: case 64: case 81: case 110: alpha_prec = 8; break;    /* G16R16, A16B16G16R16, V16U16, L16, Q16W16V16U16 */
    case 114: case 115: case 116: alpha_prec = 0xF; break;                  /* R32F, G32R32F, A32B32G32R32F */
    default: break;
  }
  {
    uint32_t ref = rs[RS_ALPHAREF] & 0xff;
    ps_cb3[0][0] = alpha_prec > 8 ? (float)ref / 255.0f : (float)((ref << alpha_prec) | (ref >> (8 - alpha_prec)));
  }
  color4(rs[RS_FOGCOLOR], ps_cb3[1]);
  ps_cb3[2][0] = fog_end; ps_cb3[2][1] = fog_scale; ps_cb3[2][2] = rsf(rs, RS_FOGDENSITY);
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 4; j++) memcpy(&ps_cb3[4 + i][j], &st.tss[i][TSS_BUMPENVMAT00 + j], 4);
    memcpy(&ps_cb3[12 + i][0], &st.tss[i][TSS_BUMPENVLSCALE], 4); memcpy(&ps_cb3[12 + i][1], &st.tss[i][TSS_BUMPENVLOFFSET], 4);
  }

  HB_PROF(1);   /* 1: vertex layout, fog/clip, FFP shaders and their constant banks */
  uint32_t rt_pf[4] = {};
  for (int i = 0; i < 4; i++) if (st.rt[i] && st.rt[i]->texture && !(d->enc_detach >> i & 1)) rt_pf[i] = (uint32_t)hb_rt_pf(d, st.rt[i]);
  hb_resource *ref0 = st.rt[0] ? st.rt[0] : st.ds;
  uint32_t samples = ref0 ? ref0->samples : 1;
  /* (DX9FULL 13.11.2026) ATOC, the NVIDIA/Intel driver hack DXVK serves to every vendor but AMD: D3DRS_ADAPTIVETESS_Y = 'ATOC'
     with alpha test on and a multisampled RT0 turns the alpha test into alpha-to-coverage (DXVK UpdateAlphaToCoverangeAndAlphaTest) */
  bool atoc = d->fmt_ext && rs[RS_ADAPTIVETESS_Y] == HACK_ATOC && rs[RS_ALPHATESTENABLE] && st.rt[0] && st.rt[0]->samples > 1;
  bool alpha_test = rs[RS_ALPHATESTENABLE] && !atoc;
  if (atoc) d->ext_atoc_draws++;
  uint32_t ps_flags = (alpha_test ? (rs[RS_ALPHAFUNC] & 0xF) : 0) | (ps_fog << DXSO_PS_FOG_SHIFT);
  if (d->fmt_ext && rs[RS_SHADEMODE] == 1 /* D3DSHADE_FLAT */) ps_flags |= 1u << 10;   /* (DX9FULL) flat colour interpolants */
  if (alpha_test) ps_flags |= alpha_prec << DXSO_PS_ALPHA_PREC_SHIFT;
  if (!ffp_ps && ps->info.major == 1)
    for (int i = 0; i < 8; i++) if (st.tex[i]) {
      uint32_t t = st.tex[i]->kind == HBDX9_RES_TEXTURE_CUBE ? 1 : st.tex[i]->kind == HBDX9_RES_TEXTURE_3D ? 2 : 0;
      ps_flags |= t << (DXSO_PS1_SAMPLER_SHIFT + 2 * i);
    }
  /* depth textures: D24S8/D16/D32 compare (wined3d: depth channel -> shadow sampler); INTZ raw depth */
  uint32_t depth_samplers = 0;
  if (ffp_ps || ps->info.major >= 2)
    for (int i = 0; i < 16; i++) {
      hb_resource *t = st.tex[i];
      if (!t || !t->fi.depth) continue;
      bool df = d->fmt_ext && (t->format == FMT_DF16 || t->format == FMT_DF24);   /* (DX9FULL) raw, (d, 0, 0, 1) */
      depth_samplers |= df ? (1u << i | 1u << (16 + i)) : t->format == FMT_INTZ ? 1u << (16 + i) : 1u << i;
      if (df) d->ext_df_draws++;
    }
  d->cur_depth_samplers = depth_samplers;
  /* (DX9FULL) Fetch4 (DXVK UpdateActiveFetch4): asked for by the sampler, a single-channel 2D texture, MAGFILTER point; PS sm2/3
     and fixed function only (ps_1_x uses bits 16-31 for sampler types) */
  uint32_t f4_alpha = 0;   /* (DX9FULL v13) Fetch4 samplers whose single channel is alpha (A8) */
  if (d->fmt_ext && st.fetch4 && (ffp_ps || ps->info.major >= 2)) {
    uint32_t f4 = 0;
    for (int i = 0; i < 16; i++) {
      hb_resource *t = st.tex[i];
      if (!(st.fetch4 >> i & 1) || !t || t->kind != HBDX9_RES_TEXTURE_2D || st.ss[i][SAMP_MAGFILTER] != 1) continue;
      switch (t->format) {
      case FMT_INTZ: case FMT_DF16: case FMT_DF24: case FMT_R16F: case FMT_R32F: case FMT_L8: case FMT_L16: f4 |= 1u << i; break;
      /* (DX9FULL v13) A8 (in DXVK's list too): ours is MTLPixelFormatA8Unorm, so the gather takes the alpha channel - the four
         texels' single channel, as for L8 / R32F */
      case FMT_A8: f4 |= 1u << i; f4_alpha |= 1u << i; break;
      default: break;
      }
    }
    if (f4) { ps_flags |= f4 << DXSO_PS_FETCH4_SHIFT; d->ext_fetch4_draws++; }
  }
  /* (DX9FULL) D3DRS_MULTISAMPLEMASK on a multisampled target: the PS variant ANDs its coverage with it (airconv
     SM50_SHADER_PSO_PIXEL_SHADER sample_mask -> [[sample_mask]] output); passed in the PS variant's otherwise unused vs_flags with
     bit 31 as the marker */
  uint32_t ps_mask = 0;
  if (d->fmt_ext && samples > 1 && samples < 32) {
    uint32_t full = (1u << samples) - 1, m = rs[RS_MULTISAMPLEMASK] & full;
    if (m != full) ps_mask = 0x80000000u | m;
  }
  /* (DX9FULL v13) the PS variant's fill_slots (a VS field, 0 for pixel shaders until now) carries the alpha-gather samplers */
  hb_shader_variant *pv = hb_variant(d, ps, f4_alpha, nullptr, rt_pf, ps_flags, ps_mask, nullptr, depth_samplers);
  uint32_t ps_reads = ps->info.slots_read | (ps_fog ? 1u << DXSO_SLOT_FOG : 0);
  uint32_t fill = ps_reads & ~vs->info.slots_written;
  if (vs_fog_src) fill |= 1u << DXSO_SLOT_FOG;
  /* hardware instancing (indexed draws only, as D3D9): stream 0 INDEXEDDATA|n gives n instances, INSTANCEDATA|k steps */
  uint32_t instances = 1, steps[16] = {};
  if (indexed && (st.freq[0] & 0x40000000u)) {
    instances = std::max<uint32_t>(1, st.freq[0] & 0x3fffffffu);
    for (int i = 0; i < 16; i++) if (st.freq[i] & 0x80000000u) steps[i] = std::max<uint32_t>(1, st.freq[i] & 0x3fffffffu);
  }
  uint32_t vs_point_flags = 0;
  if (points) {
    for (int i = 0; i < 16; i++) steps[i] = 1;   /* every vertex attribute per instance: instance = the point */
    vs_point_flags = 1u << DXSO_VS_POINT_SHIFT | (rs[RS_POINTSPRITEENABLE] ? 1u << (DXSO_VS_POINT_SHIFT + 1) : 0);
    float mx = rsf(rs, RS_POINTSIZE_MAX);
    vs_cb3[6][0] = rsf(rs, RS_POINTSIZE); vs_cb3[6][1] = rsf(rs, RS_POINTSIZE_MIN); vs_cb3[6][2] = mx > 0 ? mx : 256.0f;
    vs_cb3[7][0] = st.vp.width ? 1.0f / (float)st.vp.width : 0; vs_cb3[7][1] = st.vp.height ? 1.0f / (float)st.vp.height : 0;
    vs_cb3_bytes = sizeof(vs_cb3);
  }
  hb_shader_variant *vv = hb_variant(d, vs, fill, layout, nullptr, 0, vs_fog_src | (clip_mask << DXSO_VS_CLIP_SHIFT) | vs_point_flags, steps);
  if (!vv || !pv) return;

  HB_PROF(2);   /* 2: shader variant lookup */
  /* pipeline */
  uint64_t pkey[6];
  pkey[0] = (uint64_t)(uintptr_t)vv; pkey[1] = (uint64_t)(uintptr_t)pv;
  pkey[2] = fnv(rt_pf, sizeof(rt_pf), st.ds ? (uint64_t)st.ds->fi.pf : 0);
  uint32_t blend[10] = {rs[RS_ALPHABLENDENABLE], rs[RS_SRCBLEND], rs[RS_DESTBLEND], rs[RS_BLENDOP], rs[RS_SEPARATEALPHABLENDENABLE],
                        rs[RS_SRCBLENDALPHA], rs[RS_DESTBLENDALPHA], rs[RS_BLENDOPALPHA], rs[RS_COLORWRITEENABLE],
                        (rs[RS_COLORWRITEENABLE1] & 0xf) | (rs[RS_COLORWRITEENABLE1 + 1] & 0xf) << 4 | (rs[RS_COLORWRITEENABLE1 + 2] & 0xf) << 8 |
                        (d->feedback_off ? 1u << 31 : 0)};
  /* (DX9FULL 13.11.2026) D3DBLEND_BOTHSRCALPHA / BOTHINVSRCALPHA (12 / 13, advertised in SrcBlendCaps): the source factor sets
     both (src = SRCALPHA, dst = INVSRCALPHA, or the reverse), as wined3d / DXVK; before, they fell to ONE */
  uint32_t sb = rs[RS_SRCBLEND], db = rs[RS_DESTBLEND], sba = rs[RS_SRCBLENDALPHA], dba = rs[RS_DESTBLENDALPHA];
  if (d->fmt_ext) {
    if (sb == 12) { sb = 5; db = 6; } else if (sb == 13) { sb = 6; db = 5; }
    if (sba == 12) { sba = 5; dba = 6; } else if (sba == 13) { sba = 6; dba = 5; }
  }
  pkey[3] = fnv(blend, sizeof(blend)); pkey[4] = samples; pkey[5] = atoc ? 1 : 0;
  if (sb != rs[RS_SRCBLEND] || db != rs[RS_DESTBLEND] || sba != rs[RS_SRCBLENDALPHA] || dba != rs[RS_DESTBLENDALPHA]) pkey[5] |= 2;
  id<MTLRenderPipelineState> pso = nil;
  for (auto &p : d->psos) if (!memcmp(p.key, pkey, sizeof(pkey))) { pso = p.pso; break; }
  if (!pso) {
    d->pso_misses++;
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vv->function; pd.fragmentFunction = pv->function;
    pd.rasterSampleCount = samples;
    if (atoc) pd.alphaToCoverageEnabled = YES;   /* (DX9FULL) */
    bool writes_color = false;
    for (int i = 0; i < 4; i++) {
      if (!st.rt[i] || !st.rt[i]->texture || (d->enc_detach >> i & 1)) continue;
      auto *ca = pd.colorAttachments[i];
      ca.pixelFormat = (MTLPixelFormat)rt_pf[i];
      uint32_t wm = i == 0 ? rs[RS_COLORWRITEENABLE] : rs[RS_COLORWRITEENABLE1 + i - 1];
      /* (session 6) an attachment the pixel shader does not write keeps its contents (D3D9 drivers / DXVK, which binds only
         written targets); Metal leaves it undefined unless its write mask is empty */
      if (!d->feedback_off && !(ps->info.rt_mask >> i & 1)) wm = 0;
      ca.writeMask = (wm & 1 ? MTLColorWriteMaskRed : 0) | (wm & 2 ? MTLColorWriteMaskGreen : 0) | (wm & 4 ? MTLColorWriteMaskBlue : 0) |
                     (wm & 8 ? MTLColorWriteMaskAlpha : 0);
      writes_color |= ca.pixelFormat != MTLPixelFormatInvalid && ca.writeMask != MTLColorWriteMaskNone;
      if (rs[RS_ALPHABLENDENABLE]) {
        ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = blend_factor(sb); ca.destinationRGBBlendFactor = blend_factor(db);
        ca.rgbBlendOperation = blend_op(rs[RS_BLENDOP]);
        bool sep = rs[RS_SEPARATEALPHABLENDENABLE];
        ca.sourceAlphaBlendFactor = blend_factor(sep ? sba : sb);
        ca.destinationAlphaBlendFactor = blend_factor(sep ? dba : db);
        ca.alphaBlendOperation = blend_op(sep ? rs[RS_BLENDOPALPHA] : rs[RS_BLENDOP]);
      }
    }
    if (st.ds) {
      pd.depthAttachmentPixelFormat = st.ds->fi.pf;
      if (st.ds->fi.stencil) pd.stencilAttachmentPixelFormat = st.ds->fi.pf;
    }
    /* Metal can omit the fragment stage when no color attachment is written. */
    id<MTLBinaryArchive> archive = writes_color && pd.fragmentFunction && pd.rasterizationEnabled
                                ? d->cache.archive : nil;
    NSError *err = nil;
    if (archive) {
      /* archive first: a hit creates the pipeline from stored GPU code; a miss compiles and adds it */
      pd.binaryArchives = [NSClassFromString(@"NSArray") arrayWithObject:archive];
      pso = [d->device newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&err];
      pd.binaryArchives = nil;  /* Compilation and recovery must not use the failed lookup. */
      if (pso) d->cache.pso_hits++;   /* new...: +1 already */
      err = nil;
    }
    if (!pso) {
      pso = [d->device newRenderPipelineStateWithDescriptor:pd error:&err];
      if (pso && archive) {
        d->cache.pso_misses++;
        NSError *aerr = nil;
        if ([archive addRenderPipelineFunctionsWithDescriptor:pd error:&aerr]) d->cache.archive_dirty = true;
        else TRACE("archive add failed: %s\n", aerr ? [[aerr description] UTF8String] : "?");
      }
    }
    if (pso && archive) d->cache.archive_entries.push_back([pd retain]);
    [pd release];
    if (!pso) { WARN("pipeline failed: %s\n", err ? [[err description] UTF8String] : "?"); }
    hb_pso_entry pe; memcpy(pe.key, pkey, sizeof(pkey)); pe.pso = pso;
    d->psos.push_back(pe);
    TRACE("pso %p vs=%p ps=%p (misses %llu)\n", (void *)pso, (void *)vv, (void *)pv, (unsigned long long)d->pso_misses);
  }
  if (!pso) return;

  id<MTLRenderCommandEncoder> e = d->enc;
  HB_PROF(3);   /* 3: pipeline key + lookup (+ creation on a miss) */
  if (d->enc_pso != pso) { [e setRenderPipelineState:pso]; d->enc_pso = pso; }
  /* (DX9FULL 13.11.2026) D3DRS_BLENDFACTOR for D3DBLEND_BLENDFACTOR / INVBLENDFACTOR (14 / 15): s8j never set Metal's blend
     colour, so those factors read (0, 0, 0, 0) */
  if (d->fmt_ext && rs[RS_ALPHABLENDENABLE] && (sb == 14 || sb == 15 || db == 14 || db == 15 ||
      (rs[RS_SEPARATEALPHABLENDENABLE] && (sba == 14 || sba == 15 || dba == 14 || dba == 15)))) {
    uint32_t c = rs[RS_BLENDFACTOR];
    if (!d->enc_bc_set || d->enc_bc != c) {
      [e setBlendColorRed:(float)(c >> 16 & 0xff) / 255.0f green:(float)(c >> 8 & 0xff) / 255.0f blue:(float)(c & 0xff) / 255.0f
                    alpha:(float)(c >> 24) / 255.0f];
      d->enc_bc_set = true; d->enc_bc = c;
    }
    d->ext_blendfactor_draws++;
  }
  id<MTLDepthStencilState> dss = hb_dss(d, st.ds != nullptr, st.ds && st.ds->fi.stencil);
  if (d->enc_dss != dss) { [e setDepthStencilState:dss]; d->enc_dss = dss; }
  if (st.ds && st.ds->fi.stencil) [e setStencilReferenceValue:rs[RS_STENCILREF]];
  /* (DX9FULL 13.11.2026) NVDB, NVIDIA's depth bounds hack: D3DRS_ADAPTIVETESS_X = 'NVDB' enables it, Z / W are the bounds
     (floats, clamped to [0, 1]; max < min = off, as DXVK).  Metal: -[MTLRenderCommandEncoder setDepthTestMinBound:maxBound:]
     (macOS 26), tests the STORED depth like the D3D9 / Vulkan depth bounds test; (0, 1) = off.  Only touched when wanted or
     already set on this encoder, so a game that never asks keeps the encoder exactly as before. */
  if (d->fmt_ext) {
    bool want = rs[RS_ADAPTIVETESS_X] == HACK_NVDB && st.ds && rs[RS_ZENABLE];
    float mn = 0, mx = 1;
    if (want) {
      mn = std::min(std::max(rsf(rs, RS_ADAPTIVETESS_Z), 0.0f), 1.0f); mx = std::min(std::max(rsf(rs, RS_ADAPTIVETESS_W), 0.0f), 1.0f);
      if (!(mn <= mx)) { mn = 0; mx = 1; }
      d->ext_nvdb_draws++;
    }
    bool on = !(mn == 0 && mx == 1);
    if ((on || d->enc_db_on) && (on != d->enc_db_on || mn != d->enc_db_min || mx != d->enc_db_max)) {
      [e setDepthTestMinBound:mn maxBound:mx];
      d->enc_db_on = on; d->enc_db_min = mn; d->enc_db_max = mx;
    }
  }
  /* rasterizer: D3D9 front faces are clockwise; CULL_CW culls clockwise (front) faces */
  [e setFrontFacingWinding:MTLWindingClockwise];
  [e setCullMode:points ? MTLCullModeNone : rs[RS_CULLMODE] == 2 ? MTLCullModeFront : rs[RS_CULLMODE] == 3 ? MTLCullModeBack : MTLCullModeNone];
  [e setTriangleFillMode:rs[RS_FILLMODE] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
  { float bias = rsf(rs, RS_DEPTHBIAS), slope = rsf(rs, RS_SLOPESCALEDEPTHBIAS);
    if (bias != 0 || slope != 0) [e setDepthBias:bias * 16777216.0f slopeScale:slope clamp:0]; else [e setDepthBias:0 slopeScale:0 clamp:0]; }
  /* viewport with the D3D9 half-pixel convention: D3D9 pixel centres sit on integer coordinates */
  MTLViewport vp = {(double)st.vp.x + 0.5, (double)st.vp.y + 0.5, (double)st.vp.width, (double)st.vp.height, st.vp.min_z, st.vp.max_z};
  [e setViewport:vp];
  hb_resource *ref = st.rt[0] ? st.rt[0] : st.ds;
  uint32_t lvl = st.rt[0] ? rt_mip(st.rt_level[0]) : 0;
  uint32_t tw = std::max<uint32_t>(1, ref->width >> lvl), th = std::max<uint32_t>(1, ref->height >> lvl);
  MTLScissorRect sr = {0, 0, tw, th};
  if (rs[RS_SCISSORTESTENABLE]) {
    int32_t x1 = std::max(st.scissor.x1, 0), y1 = std::max(st.scissor.y1, 0);
    int32_t x2 = std::min<int32_t>(st.scissor.x2, (int32_t)tw), y2 = std::min<int32_t>(st.scissor.y2, (int32_t)th);
    if (x2 <= x1 || y2 <= y1) return;
    sr = {(NSUInteger)x1, (NSUInteger)y1, (NSUInteger)(x2 - x1), (NSUInteger)(y2 - y1)};
  }
  [e setScissorRect:sr];

  /* constants, textures */
  HB_PROF(4);   /* 4: depth-stencil / raster / viewport state */
  std::vector<id<MTLResource>> used_res; std::vector<id<MTLTexture>> used_tex;
  hb_bind_stage(d, vv, true, vs, ffp_vs ? (const void *)ffp_vs_bank : (const void *)st.vsf, ffp_vs ? ffp_vs_bytes : 256 * 16,
                st.vsi, st.vsb, vs_cb3, vs_cb3_bytes, used_res, used_tex);
  hb_bind_stage(d, pv, false, ps, ffp_ps ? (const void *)ffp_ps_bank : (const void *)st.psf, ffp_ps ? ffp_ps_bytes : 256 * 16,
                st.psi, st.psb, ps_cb3, sizeof(ps_cb3), used_res, used_tex);

  /* vertex buffer table (slot_mask order) */
  {
    struct VBE { uint64_t addr; uint32_t stride, length; };
    VBE vbe[16]; uint32_t nvb = 0;
    for (uint32_t slot = 0; slot < 16; slot++) {
      if (!(vv->vb_slot_mask >> slot & 1)) continue;
      VBE &x = vbe[nvb++];
      if (g.up) {
        id<MTLBuffer> b; uint32_t off; uint64_t gpu;
        uint8_t *p = hb_alloc(d, g.up_bytes, 16, &b, &off, &gpu);
        memcpy(p, g.up_data, g.up_bytes);
        x = {gpu, g.up_stride, g.up_bytes};
        used_res.push_back(b);
      } else if (st.stream[slot].buf && st.stream[slot].buf->buffer) {
        hb_resource *r = st.stream[slot].buf;
        uint32_t len = (uint32_t)[r->buffer length];
        uint32_t off = std::min(st.stream[slot].offset, len);
        x = {[r->buffer gpuAddress] + off, st.stream[slot].stride, len - off};
        r->last_serial = d->serial;
        used_res.push_back(r->buffer);
      } else x = {0, 0, 0};
    }
    if (nvb) {
      id<MTLBuffer> b; uint32_t off;
      void *p = hb_alloc(d, nvb * sizeof(VBE), 256, &b, &off, nullptr);
      memcpy(p, vbe, nvb * sizeof(VBE));
      [e setVertexBuffer:b offset:off atIndex:SM50_BINDING_INDEX_VERTEX_BUFFER];
    }
  }
  HB_PROF(5);   /* 5: argument tables, constants, vertex buffers */
  /* residency for indirectly addressed resources */
  if (d->chunk) used_res.push_back(d->chunk->buf);
  for (auto r : used_res) [e useResource:r usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
  for (auto t : used_tex) [e useResource:t usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];

  HB_PROF(6);   /* 6: useResource */
  /* draw */
  MTLPrimitiveType mpt = prim_type(pt);
  if (pt == PT_TRIANGLEFAN && !indexed) {
    id<MTLBuffer> ib; uint32_t ioff;
    uint32_t *idx = (uint32_t *)hb_alloc(d, prims * 12, 16, &ib, &ioff, nullptr);
    for (uint32_t i = 0; i < prims; i++) { idx[3 * i] = start_vertex; idx[3 * i + 1] = start_vertex + i + 1; idx[3 * i + 2] = start_vertex + i + 2; }
    [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:prims * 3 indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:ioff];
  } else if (points) {
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:nverts baseInstance:start_vertex];
  } else if (!indexed) {
    [e drawPrimitives:mpt vertexStart:start_vertex vertexCount:nverts];
  } else {
    uint32_t isz = 2; id<MTLBuffer> ib = nil; uint32_t ioff = 0; const uint8_t *src = nullptr;
    if (up_index) {
      isz = up_index_format == FMT_INDEX32 ? 4 : 2;
      void *p = hb_alloc(d, nverts * isz, 16, &ib, &ioff, nullptr);
      memcpy(p, up_index, nverts * isz); src = (const uint8_t *)p;
    } else {
      hb_resource *r = st.ib;
      if (!r || !r->buffer) return;
      isz = r->format == FMT_INDEX32 ? 4 : 2;
      ib = r->buffer; ioff = start_index * isz; src = (const uint8_t *)[r->buffer contents] + ioff;
      if ((uint64_t)ioff + nverts * isz > [r->buffer length]) return;
      r->last_serial = d->serial;
    }
    if (pt == PT_TRIANGLEFAN) {
      id<MTLBuffer> fb; uint32_t foff;
      uint32_t *idx = (uint32_t *)hb_alloc(d, prims * 12, 16, &fb, &foff, nullptr);
      auto at = [&](uint32_t i) { return isz == 4 ? ((const uint32_t *)src)[i] : (uint32_t)((const uint16_t *)src)[i]; };
      for (uint32_t i = 0; i < prims; i++) { idx[3 * i] = at(0); idx[3 * i + 1] = at(i + 1); idx[3 * i + 2] = at(i + 2); }
      [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:prims * 3 indexType:MTLIndexTypeUInt32 indexBuffer:fb
             indexBufferOffset:foff instanceCount:1 baseVertex:base_vertex baseInstance:0];
    } else {
      [e drawIndexedPrimitives:mpt indexCount:nverts indexType:isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:ib
             indexBufferOffset:ioff instanceCount:instances baseVertex:base_vertex baseInstance:0];
    }
  }
  d->draws++;
  d->enc_fresh = false;
  for (int i = 0; i < 4; i++) {
    if (!st.rt[i] || !(ps->info.rt_mask >> i & 1) || (d->enc_detach >> i & 1)) continue;
    uint32_t wm = i == 0 ? rs[RS_COLORWRITEENABLE] : rs[RS_COLORWRITEENABLE1 + i - 1];
    if (wm & 0xf) st.rt[i]->pass_written = true;
  }
  if (st.ds && ((rs[RS_ZENABLE] && rs[RS_ZWRITEENABLE]) || rs[RS_STENCILENABLE])) st.ds->pass_written = true;
  for (int i = 0; i < 20; i++) {
    hb_resource *t = st.tex[i];
    if (!t) continue;
    for (int k = 0; k < 4; k++) if (d->enc_rt[k] == t) t->pass_sampled = true;
    if (d->enc_ds == t) t->pass_sampled = true;
  }
  /* diagnostics (session 6): HBDX9_SERIAL=1 every draw runs alone on the GPU (commit + wait); =2 every draw is its own
     render pass (no wait) */
  if (d->serial_debug == 1) hb_commit(d, true, false);
  else if (d->serial_debug == 2) hb_end_pass(d);
  else if (iso) hb_end_pass(d);
  else if (d->serial_debug >= 3) {
    /* session 7: isolate one class of draws in their own passes - 3 depth-only (NULL/no colour target), 4 multi-target
       (RT1 bound), 5 post-processing (one colour target, not a back buffer), 6 back buffer */
    hb_resource *c0 = st.rt[0];
    bool depth_only = !c0 || !c0->texture, mrt = st.rt[1] != nullptr, bb = c0 && c0->is_backbuffer;
    int cls = depth_only ? 3 : mrt ? 4 : bb ? 6 : 5;
    /* HBDX9_SPLIT_N=N: end the pass after every N draws of the class (default 1) - the cheapest deterministic split */
    if (cls == d->serial_debug && ++d->class_in_pass >= d->split_n) hb_end_pass(d);
  }
  HB_PROF(7);   /* 7: draw encoding + bookkeeping */
#undef HB_PROF
}

/* ================================================================ StretchRect / ColorFill ================== */
static id<MTLRenderPipelineState> hb_util_pso(hb_device *d, MTLPixelFormat pf, bool fill) {
  uint64_t key = (uint64_t)pf * 2 + (fill ? 1 : 0);
  auto it = d->blit_psos.find(key);
  if (it != d->blit_psos.end()) return it->second;
  NSError *err = nil;
  if (!d->util_lib) {
    static const char *msl =
        "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V hb_util_vs(uint id [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
        "  float2 c = float2(id & 1, id >> 1); V o; o.p = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);\n"
        "  o.uv = mix(r.xy, r.zw, c); return o; }\n"
        "fragment float4 hb_blit_fs(V i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, i.uv); }\n"
        "fragment float4 hb_fill_fs(V i [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }\n";
    d->util_lib = [d->device newLibraryWithSource:[NSString stringWithUTF8String:msl] options:nil error:&err];
    if (!d->util_lib) { WARN("util library: %s\n", err ? [[err description] UTF8String] : "?"); return nil; }
  }
  MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
  pd.vertexFunction = [[d->util_lib newFunctionWithName:@"hb_util_vs"] autorelease];
  pd.fragmentFunction = [[d->util_lib newFunctionWithName:fill ? @"hb_fill_fs" : @"hb_blit_fs"] autorelease];
  pd.colorAttachments[0].pixelFormat = pf;
  id<MTLRenderPipelineState> pso = [d->device newRenderPipelineStateWithDescriptor:pd error:&err];
  [pd release];
  if (!pso) WARN("util pso: %s\n", err ? [[err description] UTF8String] : "?");
  d->blit_psos[key] = pso;
  return pso;
}

/* one render pass with dst (level/face) as the only colour attachment */
static id<MTLRenderCommandEncoder> hb_util_pass(hb_device *d, hb_resource *dst, uint32_t level, uint32_t face, bool clear, const float *color) {
  hb_end_pass(d);
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = dst->texture;
  rp.colorAttachments[0].level = level;
  if (dst->kind == HBDX9_RES_TEXTURE_CUBE) rp.colorAttachments[0].slice = face;
  rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
  if (clear) rp.colorAttachments[0].clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  dst->last_serial = d->serial;
  return [hb_cmd(d) renderCommandEncoderWithDescriptor:rp];
}

static void hb_color_fill(hb_device *d, const hbdx9_pkt_color_fill *f) {
  auto *r = (hb_resource *)(uintptr_t)f->dst;
  if (!r || !r->texture || r->fi.depth || r->samples > 1) return;
  uint32_t w = std::max<uint32_t>(1, r->width >> f->level), h = std::max<uint32_t>(1, r->height >> f->level);
  int32_t x1 = std::max(f->rect.x1, 0), y1 = std::max(f->rect.y1, 0);
  int32_t x2 = std::min<int32_t>(f->rect.x2, (int32_t)w), y2 = std::min<int32_t>(f->rect.y2, (int32_t)h);
  if (x2 <= x1 || y2 <= y1) return;
  float c[4]; color4(f->color, c);
  bool full = x1 == 0 && y1 == 0 && (uint32_t)x2 == w && (uint32_t)y2 == h;
  if (r->texture.storageMode == MTLStorageModeShared && r->fi.block == 1 && !(r->texture.usage & MTLTextureUsageRenderTarget)) {
    /* not renderable (plain managed texture): write the pixels through the ring with a blit */
    hbdx9_pkt_update_texture u{}; u.texture = f->dst; u.level = f->level; u.face = f->face;
    u.x = x1; u.y = y1; u.width = x2 - x1; u.height = y2 - y1; u.pitch = u.width * r->fi.bytes; u.size = u.pitch * u.height;
    std::vector<uint8_t> px(u.size);
    uint32_t argb = f->color;
    for (uint32_t i = 0; i < u.width * u.height; i++) {
      if (r->fi.bytes == 4) memcpy(&px[i * 4], &argb, 4);
      else if (r->fi.bytes == 2 && r->format == FMT_R5G6B5) { uint16_t v = (uint16_t)(((argb >> 8) & 0xf800) | ((argb >> 5) & 0x7e0) | ((argb >> 3) & 0x1f)); memcpy(&px[i * 2], &v, 2); }
      else memset(&px[i * r->fi.bytes], 0, r->fi.bytes);
    }
    hb_update_texture(d, &u, px.data());
    return;
  }
  @autoreleasepool {
    id<MTLRenderCommandEncoder> e = hb_util_pass(d, r, f->level, f->face, full, c);
    if (!full) {
      id<MTLRenderPipelineState> pso = hb_util_pso(d, r->fi.pf, true);
      if (pso) {
        [e setRenderPipelineState:pso];
        MTLViewport vp = {0, 0, (double)w, (double)h, 0, 1}; [e setViewport:vp];
        MTLScissorRect sr = {(NSUInteger)x1, (NSUInteger)y1, (NSUInteger)(x2 - x1), (NSUInteger)(y2 - y1)}; [e setScissorRect:sr];
        float rr[4] = {0, 0, 1, 1};
        [e setVertexBytes:rr length:16 atIndex:0];
        [e setFragmentBytes:c length:16 atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      }
    }
    [e endEncoding];
  }
}

static void hb_blit(hb_device *d, const hbdx9_pkt_blit *b) {
  auto *src = (hb_resource *)(uintptr_t)b->src, *dst = (hb_resource *)(uintptr_t)b->dst;
  if (!src || !dst || !src->texture || !dst->texture) return;
  uint32_t sw = std::max<uint32_t>(1, src->width >> b->src_level), sh = std::max<uint32_t>(1, src->height >> b->src_level);
  uint32_t dw = std::max<uint32_t>(1, dst->width >> b->dst_level), dh = std::max<uint32_t>(1, dst->height >> b->dst_level);
  hbdx9_rect sr = b->src_rect, dr = b->dst_rect;
  if (sr.x2 <= sr.x1 || sr.y2 <= sr.y1) sr = {0, 0, (int32_t)sw, (int32_t)sh};
  if (dr.x2 <= dr.x1 || dr.y2 <= dr.y1) dr = {0, 0, (int32_t)dw, (int32_t)dh};
  if (sr.x1 < 0 || sr.y1 < 0 || (uint32_t)sr.x2 > sw || (uint32_t)sr.y2 > sh || dr.x1 < 0 || dr.y1 < 0 || (uint32_t)dr.x2 > dw || (uint32_t)dr.y2 > dh) return;
  uint32_t cw = sr.x2 - sr.x1, ch = sr.y2 - sr.y1;
  bool same_size = cw == (uint32_t)(dr.x2 - dr.x1) && ch == (uint32_t)(dr.y2 - dr.y1);
  bool is_cube_s = src->kind == HBDX9_RES_TEXTURE_CUBE, is_cube_d = dst->kind == HBDX9_RES_TEXTURE_CUBE;
  @autoreleasepool {
    hb_end_pass(d);
    src->last_serial = d->serial; dst->last_serial = d->serial;
    if (src->samples > 1 && dst->samples == 1) {
      /* resolve: whole surface, same size and format (D3D9 StretchRect from a multisampled surface) */
      if (src->width == dst->width && src->height == dst->height && src->fi.pf == dst->fi.pf && !dst->fi.depth) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = src->texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        rp.colorAttachments[0].resolveTexture = dst->texture;
        rp.colorAttachments[0].resolveLevel = b->dst_level;
        if (is_cube_d) rp.colorAttachments[0].resolveSlice = b->dst_face;
        [[hb_cmd(d) renderCommandEncoderWithDescriptor:rp] endEncoding];
        if (dst->autogen) dst->mips_dirty = true;
      } else WARN("StretchRect from multisampled surface with different size/format not supported\n");
      return;
    }
    if (same_size && src->fi.pf == dst->fi.pf && src->samples == dst->samples) {
      id<MTLBlitCommandEncoder> be = [hb_cmd(d) blitCommandEncoder];
      [be copyFromTexture:src->texture sourceSlice:is_cube_s ? b->src_face : 0 sourceLevel:b->src_level
             sourceOrigin:MTLOriginMake(sr.x1, sr.y1, 0) sourceSize:MTLSizeMake(cw, ch, 1) toTexture:dst->texture
         destinationSlice:is_cube_d ? b->dst_face : 0 destinationLevel:b->dst_level destinationOrigin:MTLOriginMake(dr.x1, dr.y1, 0)];
      [be endEncoding];
      if (dst->autogen) dst->mips_dirty = true;
      return;
    }
    if (src->fi.depth || dst->fi.depth || dst->samples > 1) { WARN("StretchRect depth/scale combination not supported\n"); return; }
    id<MTLRenderPipelineState> pso = hb_util_pso(d, dst->fi.pf, false);
    if (!pso) return;
    id<MTLTexture> view = [src->texture newTextureViewWithPixelFormat:src->fi.pf textureType:MTLTextureType2D
                                                               levels:NSMakeRange(b->src_level, 1) slices:NSMakeRange(is_cube_s ? b->src_face : 0, 1)
                                                              swizzle:src->fi.has_swz ? src->fi.swz : MTLTextureSwizzleChannelsDefault];
    bool linear = b->filter == 2 /* D3DTEXF_LINEAR */;
    auto sit = d->blit_samplers.find(linear);
    id<MTLSamplerState> smp;
    if (sit == d->blit_samplers.end()) {
      MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
      sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
      sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
      smp = [d->device newSamplerStateWithDescriptor:sd]; [sd release];
      d->blit_samplers[linear] = smp;
    } else smp = sit->second;
    id<MTLRenderCommandEncoder> e = hb_util_pass(d, dst, b->dst_level, b->dst_face, false, nullptr);
    [e setRenderPipelineState:pso];
    MTLViewport vp = {(double)dr.x1, (double)dr.y1, (double)(dr.x2 - dr.x1), (double)(dr.y2 - dr.y1), 0, 1}; [e setViewport:vp];
    MTLScissorRect sc = {(NSUInteger)dr.x1, (NSUInteger)dr.y1, (NSUInteger)(dr.x2 - dr.x1), (NSUInteger)(dr.y2 - dr.y1)}; [e setScissorRect:sc];
    float rr[4] = {(float)sr.x1 / sw, (float)sr.y1 / sh, (float)sr.x2 / sw, (float)sr.y2 / sh};
    [e setVertexBytes:rr length:16 atIndex:0];
    [e setFragmentTexture:view atIndex:0];
    [e setFragmentSamplerState:smp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
    hb_defer_release(d, view, d->serial);
    if (dst->autogen) dst->mips_dirty = true;
  }
}

/* ================================================================ entry points ============================= */
static void hb_reset_state(hb_device *d) {
  hb_state &st = d->st;
  memset(st.rs, 0, sizeof(st.rs));
  st.rs[RS_ZENABLE] = 1; st.rs[RS_FILLMODE] = 3; st.rs[RS_SHADEMODE] = 2; st.rs[RS_ZWRITEENABLE] = 1; st.rs[RS_SRCBLEND] = 2;
  st.rs[RS_DESTBLEND] = 1; st.rs[RS_CULLMODE] = 3; st.rs[RS_ZFUNC] = 4; st.rs[RS_ALPHAFUNC] = 8; st.rs[RS_STENCILFUNC] = 8;
  st.rs[RS_STENCILFAIL] = st.rs[RS_STENCILZFAIL] = st.rs[RS_STENCILPASS] = 1; st.rs[RS_STENCILMASK] = st.rs[RS_STENCILWRITEMASK] = 0xffffffff;
  st.rs[RS_CCW_STENCILFAIL] = st.rs[RS_CCW_STENCILZFAIL] = st.rs[RS_CCW_STENCILPASS] = 1; st.rs[RS_CCW_STENCILFUNC] = 8;
  st.rs[RS_TEXTUREFACTOR] = 0xffffffff; st.rs[RS_LIGHTING] = 1; st.rs[RS_COLORVERTEX] = 1; st.rs[RS_BLENDOP] = 1;
  st.rs[RS_COLORWRITEENABLE] = st.rs[RS_COLORWRITEENABLE1] = st.rs[RS_COLORWRITEENABLE2] = st.rs[RS_COLORWRITEENABLE3] = 0xf;
  st.rs[RS_SRCBLENDALPHA] = 2; st.rs[RS_DESTBLENDALPHA] = 1; st.rs[RS_BLENDOPALPHA] = 1; st.rs[RS_BLENDFACTOR] = 0xffffffff;
  for (auto &s : st.ss) {
    memset(s, 0, sizeof(s));
    s[SAMP_ADDRESSU] = s[SAMP_ADDRESSV] = s[SAMP_ADDRESSW] = 1; s[SAMP_MAGFILTER] = s[SAMP_MINFILTER] = 1; s[SAMP_MAXANISOTROPY] = 1;
  }
  for (int i = 0; i < 8; i++) {
    uint32_t *t = st.tss[i];
    memset(t, 0, sizeof(st.tss[i]));
    t[TSS_COLOROP] = i ? 1 : 4; t[TSS_COLORARG1] = 2; t[TSS_COLORARG2] = 1;
    t[TSS_ALPHAOP] = i ? 1 : 2; t[TSS_ALPHAARG1] = 2; t[TSS_ALPHAARG2] = 1; t[TSS_TEXCOORDINDEX] = i; t[TSS_RESULTARG] = 1;
  }
  memset(st.tex, 0, sizeof(st.tex)); memset(st.stream, 0, sizeof(st.stream)); st.ib = nullptr; st.vs = st.ps = nullptr;
  memset(st.vsf, 0, sizeof(st.vsf)); memset(st.psf, 0, sizeof(st.psf)); memset(st.vsi, 0, sizeof(st.vsi)); memset(st.psi, 0, sizeof(st.psi));
  memset(st.vsb, 0, sizeof(st.vsb)); memset(st.psb, 0, sizeof(st.psb));
  st.use_fvf = false; st.fvf = 0; st.decl.clear();
  static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  memcpy(st.world, ident, 64); memcpy(st.view, ident, 64); memcpy(st.proj, ident, 64);
  for (auto &w : st.worlds) memcpy(w, ident, 64);   /* (DX9FULL) */
  for (auto &m : st.texmat) memcpy(m, ident, 64);
  memset(st.rt, 0, sizeof(st.rt)); memset(st.rt_level, 0, sizeof(st.rt_level)); st.ds = nullptr;
  for (auto &f : st.freq) f = 1;
  memset(st.lights, 0, sizeof(st.lights)); st.nlights = 0; memset(st.material, 0, sizeof(st.material)); memset(st.clip, 0, sizeof(st.clip));
  st.rs[RS_CLIPPING] = 1; st.rs[RS_LOCALVIEWER] = 1; st.rs[RS_DIFFUSEMATERIALSOURCE] = 1; st.rs[RS_SPECULARMATERIALSOURCE] = 2;
  st.rs[RS_MULTISAMPLEANTIALIAS] = 1;
  float one = 1.0f; memcpy(&st.rs[RS_FOGEND], &one, 4); memcpy(&st.rs[RS_FOGDENSITY], &one, 4);
  memcpy(&st.rs[RS_ADAPTIVETESS_Z], &one, 4);   /* (DX9FULL) D3D9 default, the shim's shadow has the same */
  st.rs[RS_MULTISAMPLEMASK] = 0xffffffff;        /* (DX9FULL) D3D9 default (the shim's shadow had it; the core had 0, unused) */
  { float pmax = 64.0f;                           /* (DX9FULL) point defaults as the shim's shadow (D3D9 / DXVK) */
    memcpy(&st.rs[RS_POINTSIZE], &one, 4); memcpy(&st.rs[RS_POINTSIZE_MIN], &one, 4); memcpy(&st.rs[RS_POINTSIZE_MAX], &pmax, 4);
    memcpy(&st.rs[RS_POINTSCALE_A], &one, 4); }
  st.fetch4 = 0;
}

static NTSTATUS hb_init(void *args) {
  auto *p = (hbdx9_init_params *)args;
  @autoreleasepool {
    if (p->version != HBDX9_PROTOCOL_VERSION) { p->status = 1; return STATUS_SUCCESS; }
    char exe[65]; memcpy(exe, p->exe_name, 64); exe[64] = 0;
    for (char *c = exe; *c; c++) if (*c == '/' || *c == '\\') *c = '_';
    /* the Metal framework cache (compiled pipelines) goes where DXMT puts it for this exe: dxmt/<exe>/com.apple.metal */
    {
      const char *keep = getenv("DXMT_USE_DEFAULT_METAL_CACHE");
      typedef void (*set_path_t)(NSString *);
      auto set_path = (set_path_t)dlsym(RTLD_DEFAULT, "MTLSetShaderCachePath");
      char base[1024]; size_t n = confstr(_CS_DARWIN_USER_CACHE_DIR, base, sizeof(base));
      if (!(keep && !strcmp(keep, "1")) && set_path && n && exe[0]) {
        std::string dir = std::string(base) + (base[n - 2] == '/' ? "" : "/") + "dxmt/" + exe + "/com.apple.metal";
        for (size_t i = 1; i < dir.size(); i++) if (dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
        mkdir(dir.c_str(), 0755);
        set_path([NSString stringWithUTF8String:dir.c_str()]);
      }
    }
    auto *d = new hb_device;
    { const char *u = getenv("HBDX9_UPLOADS"); d->uploads_old = u && !strcasecmp(u, "old"); }
    /* (s8) HBDX9_CORE_UPLOADS=old: only the core's buffer path of session 4 (rename when busy, no compare), shim unchanged */
    { const char *u = getenv("HBDX9_CORE_UPLOADS"); if (u && !strcasecmp(u, "old")) d->uploads_old = true; }
    { const char *u = getenv("HBDX9_NOOV_RENAME"); d->noov_rename = u && u[0] == '1'; }
    { const char *u = getenv("HBDX9_FEEDBACK"); d->feedback_off = u && !strcasecmp(u, "off"); }
    { const char *u = getenv("HBDX9_SERIAL"); d->serial_debug = u ? atoi(u) : 0; }
    { const char *u = getenv("HBDX9_SPLIT_N"); d->split_n = u && atoi(u) > 0 ? (uint32_t)atoi(u) : 1; }
    { const char *u = getenv("HBDX9_FEEDBACK_BARRIER"); d->feedback_barrier = u && u[0] == '1' && !d->feedback_off; }
    { const char *u = getenv("HBDX9_FEEDBACK_DETACH"); d->feedback_detach = u && u[0] == '1'; }
    { const char *u = getenv("HBDX9_PROFILE"); d->prof_on = u && u[0] == '1'; }
    /* RT/texture zero-init at creation: diagnostics, off by default (did not remove the world-tape noise; texture init cost
       ~2.6 s of streaming on the Gothic tape) - HBDX9_RT_INIT=1 / HBDX9_TEX_INIT=1 */
    { const char *u = getenv("HBDX9_FMT_EXT"); d->fmt_ext = !(u && u[0] == '0'); }   /* (DX9FULL) default on */
    { const char *u = getenv("HBDX9_RT_INIT"); d->rt_init_off = !(u && u[0] == '1'); }
    { const char *u = getenv("HBDX9_TEX_INIT"); d->tex_init_off = !(u && u[0] == '1'); }
    { const char *a = getenv("HBDX9_ALPHATEST"); d->alpha_float = a && !strcasecmp(a, "float"); d->alpha_8bit = a && !strcasecmp(a, "8bit"); }
    d->device = MTLCreateSystemDefaultDevice();
    if (!d->device) { delete d; p->status = 2; return STATUS_SUCCESS; }
    d->queue = [d->device newCommandQueue];
    d->fence_page = (volatile uint32_t *)(uintptr_t)p->fence_page;
    { const char *u = getenv("HBDX9_ASYNC"); d->async = u && u[0] == '1'; }
    { const char *u = getenv("HBDX9_TSO_COPY"); d->tso_copy = u && u[0] == '1'; }
    { const char *u = getenv("HBDX9_RING_SPIN_US"); if (u && *u) d->ring_spin_us = (uint32_t)strtoul(u, nullptr, 10); }
    { const char *u = getenv("HBDX9_RING_PARK_US"); if (u && *u) d->ring_park_us = (uint32_t)strtoul(u, nullptr, 10); }
    p->caps = d->async ? (HBDX9_CAP_SUBMIT_NOCOPY | HBDX9_CAP_RING) : 0;
    if (d->fmt_ext) p->caps |= HBDX9_CAP_TIMESTAMP | HBDX9_CAP_FEATURES | HBDX9_CAP_TEXTURE_LOD;   /* (DX9FULL) */
    if (d->async) std::thread(hb_worker, d).detach();
    d->vis = [d->device newBufferWithLength:8192 * 8 options:MTLResourceStorageModeShared];
    /* AIR language version as DXMT picks it (dxmt_device.cpp GetShaderMetalVersion): 3.2 on macOS 15+ with Apple7 */
    NSOperatingSystemVersion osv = [[NSProcessInfo processInfo] operatingSystemVersion];
    d->metal_version = (osv.majorVersion >= 15 && [d->device supportsFamily:MTLGPUFamilyApple7]) ? 320 : 310;
    hb_cache_open(d, exe);
    p->max_samples = 1;
    for (uint32_t c : {2u, 4u, 8u}) if ([d->device supportsTextureSampleCount:c]) p->max_samples = c;
    d->frames = dispatch_semaphore_create(3);
    hb_reset_state(d);
    d->dummy_tex = hb_new_texture(d, HBDX9_RES_TEXTURE_2D, FMT_A8R8G8B8, 1, 1, 1, 1, 0, false);
    uint32_t black = 0xff000000u;
    [d->dummy_tex->texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
    p->device = (uint64_t)(uintptr_t)d;
    snprintf(p->name, sizeof(p->name), "%s (HyperBridge DX9)", [[d->device name] UTF8String]);
    p->vendor_id = 0x106b; p->device_id = 0;
    p->vram_bytes = [d->device recommendedMaxWorkingSetSize];
    p->status = 0;
    fprintf(stderr, "hbdx9-core: init device=%p name=\"%s\" protocol=%u metal=%u exe=%s max_samples=%u\n", (void *)d, p->name,
            HBDX9_PROTOCOL_VERSION, d->metal_version, exe, p->max_samples);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS hb_swapchain_create(void *args) {
  auto *p = (hbdx9_swapchain_params *)args;
  auto *d = (hb_device *)(uintptr_t)p->device;
  hb_drain(d);
  if (!d || !p->width || !p->height) { p->status = 1; return STATUS_SUCCESS; }
  @autoreleasepool {
    macdrv_metal_view view = nullptr;
    CAMetalLayer *layer = nil;
    if (p->hwnd) {
      auto *mf = (macdrv_functions_t *)dlsym(RTLD_DEFAULT, "macdrv_functions");
      if (!mf || !mf->get_win_data || !mf->macdrv_view_create_metal_view || !mf->macdrv_view_get_metal_layer) {
        WARN("winemac functions not found\n"); p->status = 2; return STATUS_SUCCESS;
      }
      struct macdrv_win_data *wd = mf->get_win_data((HWND)(uintptr_t)p->hwnd);
      if (wd && wd->client_cocoa_view) view = mf->macdrv_view_create_metal_view(wd->client_cocoa_view, (macdrv_metal_device)d->device);
      if (wd) mf->release_win_data(wd);
      layer = view ? (CAMetalLayer *)mf->macdrv_view_get_metal_layer(view) : nil;
      if (!layer) { WARN("no CAMetalLayer for hwnd=%llx\n", (unsigned long long)p->hwnd); p->status = 3; return STATUS_SUCCESS; }
      /* layer properties exactly as DXMT's D3D11 presenter sets them in 1.1.0 (dxmt_presenter.cpp + winemetal_unix.c
         _MetalLayer_setProps/_MetalLayer_setColorSpace): on the main thread; opaque, not framebuffer-only, display sync
         off (DXMT paces itself; here it follows the D3D9 presentation interval), colour space sRGB, EDR off */
      id<MTLDevice> dev = d->device;
      uint32_t w = p->width, h = p->height;
      BOOL sync = p->interval != 0x80000000u /* D3DPRESENT_INTERVAL_IMMEDIATE */;
      dispatch_block_t props = ^{
        layer.device = dev;
        layer.opaque = YES;
        layer.framebufferOnly = NO;
        layer.displaySyncEnabled = sync;
        layer.drawableSize = CGSizeMake(w, h);
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer.colorspace = cs;
        layer.wantsExtendedDynamicRangeContent = NO;
        CGColorSpaceRelease(cs);
      };
      if ([NSThread isMainThread]) props(); else dispatch_sync(dispatch_get_main_queue(), props);
    }
    auto *sc = new hb_swapchain;
    sc->dev = d; sc->view = view; sc->layer = [layer retain]; sc->width = p->width; sc->height = p->height;
    sc->sync = p->interval != 0x80000000u; sc->layer_sync = sc->sync;
    {
      CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
      double hz = m ? CGDisplayModeGetRefreshRate(m) : 0; if (m) CGDisplayModeRelease(m);
      sc->refresh_hz = hz > 1 ? hz : 60.0;
    }
    if (d->fmt_ext) {   /* (DX9FULL) intervals TWO (2), THREE (4), FOUR (8): presentDrawable:afterMinimumDuration: n refreshes */
      sc->vblanks = p->interval == 2 ? 2 : p->interval == 4 ? 3 : p->interval == 8 ? 4 : 1;
    }
    /* (DX9FULL) frame limiter from the shim: bit 31 = always (HBDX9_MAX_FRAME_RATE); else (a full-screen mode's refresh rate under
       vsync) only when it is below what display sync already gives (DXVK engages its limiter only when the game runs faster) */
    if (p->pace_mhz) {
      double hz = (p->pace_mhz & 0x7fffffffu) / 1000.0;
      bool always = p->pace_mhz & 0x80000000u;
      if (hz > 0.5 && (always || !sc->sync || hz < 0.97 * sc->refresh_hz / sc->vblanks)) sc->pace_ns = (uint64_t)(1e9 / hz);
    }
    d->last_interval = p->interval; d->last_pace_mhz = p->pace_mhz; d->last_refresh = sc->refresh_hz; d->last_sync = sc->sync;
    uint32_t fmt = p->format == FMT_A8R8G8B8 ? FMT_A8R8G8B8 : FMT_X8R8G8B8;
    uint32_t samples = p->samples > 1 ? p->samples : 1;
    sc->backbuffer = hb_new_texture(d, HBDX9_RES_TEXTURE_2D, fmt, p->width, p->height, 1, 1, 1, true, samples);
    if (sc->backbuffer) sc->backbuffer->is_backbuffer = true;
    if (!sc->backbuffer) { WARN("back buffer %ux%u samples=%u failed\n", p->width, p->height, samples); delete sc; p->status = 4; return STATUS_SUCCESS; }
    sc->backbuffer->kind = HBDX9_RES_SURFACE_RT;
    if (p->depth_format) {
      sc->depth = hb_new_texture(d, HBDX9_RES_TEXTURE_2D, p->depth_format, p->width, p->height, 1, 1, 2, true, samples);
      if (sc->depth) sc->depth->kind = HBDX9_RES_SURFACE_DS;
    }
    d->st.rt[0] = sc->backbuffer; d->st.ds = sc->depth;
    d->st.vp = {{0, 0, 0}, 0, 0, p->width, p->height, 0.0f, 1.0f};
    d->st.scissor = {0, 0, (int32_t)p->width, (int32_t)p->height};
    p->swapchain = (uint64_t)(uintptr_t)sc;
    p->backbuffer = (uint64_t)(uintptr_t)sc->backbuffer;
    p->depth = (uint64_t)(uintptr_t)sc->depth;
    p->status = 0;
    fprintf(stderr, "hbdx9-core: swapchain %ux%u hwnd=%llx layer=%p depth=%u samples=%u interval=%x sync=%d vblanks=%u refresh=%.2f pace_hz=%.3f\n",
            p->width, p->height, (unsigned long long)p->hwnd, (void *)layer, p->depth_format, samples, p->interval, (int)sc->sync, sc->vblanks,
            sc->refresh_hz, sc->pace_ns ? 1e9 / (double)sc->pace_ns : 0.0);
  }
  return STATUS_SUCCESS;
}

static void hb_present(hb_device *d, hb_swapchain *sc, uint32_t flags = 0) {
  if (!sc) return;
  hb_end_pass(d);
  /* (DX9FULL v12) D3DPRESENT_FORCEIMMEDIATE (DXVK: sync interval 0 for this present): display sync off while such frames come,
     back on with the first frame without it.  CAMetalLayer properties are set on the main thread (as at creation), asynchronously:
     the change reaches the next drawables */
  if (sc->layer && sc->sync) {
    bool want = !(flags & HBDX9_PRESENT_FORCE_IMMEDIATE);
    if (!want) d->force_immediate++;
    if (want != sc->layer_sync) {
      sc->layer_sync = want; d->sync_toggles++;
      CAMetalLayer *l = sc->layer; BOOL w = want;
      dispatch_async(dispatch_get_main_queue(), ^{ l.displaySyncEnabled = w; });
    }
  }
  if (sc->pace_ns) {   /* (DX9FULL) DXVK FpsLimiter::delay: sleep until the frame's time, the next one an interval later */
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (t1 < sc->pace_next) {
      uint64_t left = sc->pace_next - t1; struct timespec ts = {(time_t)(left / 1000000000u), (long)(left % 1000000000u)};
      nanosleep(&ts, nullptr); d->pace_sleeps++; d->pace_sleep_ns += left;
    }
    sc->pace_next = t1 < sc->pace_next + sc->pace_ns ? sc->pace_next + sc->pace_ns : t1 + sc->pace_ns;
  }
  id<MTLCommandBuffer> cmd = hb_cmd(d);
  if (sc->layer) {
    uint64_t w0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    id<CAMetalDrawable> drawable = [sc->layer nextDrawable];
    uint64_t w = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - w0;
    d->drawable_wait_ns += w; if (w > 1000000) d->drawable_waits++;
    if (drawable) {
      hb_present_stats *ps = d->pres;
      [drawable addPresentedHandler:^(id<MTLDrawable> dr) {
        CFTimeInterval t = dr.presentedTime;
        if (t <= 0) { ps->unshown.fetch_add(1, std::memory_order_relaxed); return; }
        uint64_t us = (uint64_t)(t * 1e6), prev = ps->last_us.exchange(us, std::memory_order_relaxed);
        ps->shown.fetch_add(1, std::memory_order_relaxed);
        if (prev && us > prev) {
          double ms = (us - prev) / 1000.0;
          int b = ms < 5 ? 0 : ms < 7 ? 1 : ms < 10 ? 2 : ms < 14 ? 3 : ms < 20 ? 4 : ms < 30 ? 5 : ms < 45 ? 6 : 7;
          ps->bins[b].fetch_add(1, std::memory_order_relaxed);
        }
      }];
    }
    if (drawable && drawable.texture.width == sc->width && drawable.texture.height == sc->height) {
      if (sc->backbuffer->samples > 1) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = sc->backbuffer->texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        rp.colorAttachments[0].resolveTexture = drawable.texture;
        [[cmd renderCommandEncoderWithDescriptor:rp] endEncoding];
      } else {
        id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
        [b copyFromTexture:sc->backbuffer->texture toTexture:drawable.texture];
        [b endEncoding];
      }
      if (sc->vblanks > 1) [cmd presentDrawable:drawable afterMinimumDuration:(double)sc->vblanks / sc->refresh_hz];
      else [cmd presentDrawable:drawable];
    } else TRACE("present skipped drawable=%p\n", (void *)drawable);
  }
  sc->backbuffer->last_serial = d->serial;
  d->frames_presented++;
  if (hb_trace() && (d->frames_presented % 300) == 1)
    TRACE("frame %llu draws=%llu psos=%zu\n", (unsigned long long)d->frames_presented, (unsigned long long)d->draws, d->psos.size());
  hb_commit(d, false, true);
}

template <typename T> static const T *pkt(const hbdx9_packet *h) { return h->size >= sizeof(T) ? (const T *)h : nullptr; }

static NTSTATUS hb_execute_submit(void *args) {
  auto *p = (hbdx9_submit_params *)args;
  auto *d = (hb_device *)(uintptr_t)p->device;
  const uint8_t *data = (const uint8_t *)(uintptr_t)p->data;
  uint32_t at = 0, index = 0;
  if (!d || !data) { p->status = 1; return STATUS_SUCCESS; }
  hb_state &st = d->st;
  @autoreleasepool {
    while (at + sizeof(hbdx9_packet) <= p->size) {
      const hbdx9_packet *h = (const hbdx9_packet *)(data + at);
      index++;
      if (h->size < sizeof(*h) || at + h->size > p->size) { p->status = index; return STATUS_SUCCESS; }
      bool bad = false;
      switch (h->type) {
      case HBDX9_PKT_CLEAR: {
        auto *c = pkt<hbdx9_pkt_clear>(h);
        if (!c || h->size < sizeof(*c) + c->rect_count * sizeof(hbdx9_rect)) { bad = true; break; }
        hb_clear(d, c, (const hbdx9_rect *)(c + 1));
        break;
      }
      case HBDX9_PKT_PRESENT: { auto *pr = pkt<hbdx9_pkt_present>(h); if (!pr) { bad = true; break; } hb_present(d, (hb_swapchain *)(uintptr_t)pr->swapchain, pr->hdr.pad); break; }
      case HBDX9_PKT_TEXTURE_LOD: {   /* (DX9FULL v12) SetLOD */
        auto *o = pkt<hbdx9_pkt_object>(h); if (!o) { bad = true; break; }
        auto *r = (hb_resource *)(uintptr_t)o->object;
        if (r) { uint32_t l = r->levels ? std::min(o->index, r->levels - 1) : 0; if (l && !r->lod) d->lod_textures++; r->lod = l; }
        break;
      }
      case HBDX9_PKT_RENDER_STATE: { auto *s = pkt<hbdx9_pkt_state>(h); if (!s) { bad = true; break; } if (s->state < 256) st.rs[s->state] = s->value; break; }
      case HBDX9_PKT_SAMPLER_STATE: { auto *s = pkt<hbdx9_pkt_state>(h); if (!s) { bad = true; break; } if (s->index < 20 && s->state < 14) st.ss[s->index][s->state] = s->value;
        /* (DX9FULL) Fetch4 switch (DXVK SetSamplerState): 'GET4' enables, 'GET1' disables, any other bias leaves it */
        if (s->index < 16 && s->state == SAMP_MIPMAPLODBIAS) { if (s->value == HACK_GET4) st.fetch4 |= 1u << s->index; else if (s->value == HACK_GET1) st.fetch4 &= ~(1u << s->index); }
        break; }
      case HBDX9_PKT_TEXTURE_STAGE_STATE: { auto *s = pkt<hbdx9_pkt_state>(h); if (!s) { bad = true; break; } if (s->index < 8 && s->state < 33) st.tss[s->index][s->state] = s->value; break; }
      case HBDX9_PKT_STREAM_FREQ: { auto *s = pkt<hbdx9_pkt_state>(h); if (!s) { bad = true; break; } if (s->index < 16) st.freq[s->index] = s->value; break; }
      case HBDX9_PKT_TEXTURE: { auto *o = pkt<hbdx9_pkt_object>(h); if (!o) { bad = true; break; } if (o->index < 20) st.tex[o->index] = (hb_resource *)(uintptr_t)o->object; break; }
      case HBDX9_PKT_INDICES: { auto *o = pkt<hbdx9_pkt_object>(h); if (!o) { bad = true; break; } st.ib = (hb_resource *)(uintptr_t)o->object; break; }
      case HBDX9_PKT_SHADER: {
        auto *o = pkt<hbdx9_pkt_object>(h); if (!o) { bad = true; break; }
        if (o->index == 0) st.vs = (hb_shader *)(uintptr_t)o->object; else st.ps = (hb_shader *)(uintptr_t)o->object;
        break;
      }
      case HBDX9_PKT_RENDER_TARGET: {
        auto *o = pkt<hbdx9_pkt_object>(h); if (!o) { bad = true; break; }
        if (o->index < 4) { st.rt[o->index] = (hb_resource *)(uintptr_t)o->object; st.rt_level[o->index] = o->level; }
        break;
      }
      case HBDX9_PKT_DEPTH_STENCIL: { auto *o = pkt<hbdx9_pkt_object>(h); if (!o) { bad = true; break; } st.ds = (hb_resource *)(uintptr_t)o->object; break; }
      case HBDX9_PKT_STREAM_SOURCE: {
        auto *s = pkt<hbdx9_pkt_stream_source>(h); if (!s) { bad = true; break; }
        if (s->stream < 16) st.stream[s->stream] = {(hb_resource *)(uintptr_t)s->buffer, s->offset, s->stride};
        break;
      }
      case HBDX9_PKT_CONSTANTS: {
        auto *c = pkt<hbdx9_pkt_constants>(h); if (!c) { bad = true; break; }
        const uint8_t *src = (const uint8_t *)(c + 1);
        uint32_t elem = (c->kind == HBDX9_CONST_VS_B || c->kind == HBDX9_CONST_PS_B) ? 4 : 16;
        if (h->size < sizeof(*c) + (uint64_t)c->count * elem) { bad = true; break; }
        void *dst = nullptr; uint32_t limit = 0;
        switch (c->kind) {
        case HBDX9_CONST_VS_F: dst = st.vsf; limit = 256; break; case HBDX9_CONST_PS_F: dst = st.psf; limit = 256; break;
        case HBDX9_CONST_VS_I: dst = st.vsi; limit = 16; break; case HBDX9_CONST_PS_I: dst = st.psi; limit = 16; break;
        case HBDX9_CONST_VS_B: dst = st.vsb; limit = 16; break; case HBDX9_CONST_PS_B: dst = st.psb; limit = 16; break;
        default: bad = true; break;
        }
        if (dst && c->start < limit) memcpy((uint8_t *)dst + c->start * elem, src, std::min(c->count, limit - c->start) * elem);
        break;
      }
      case HBDX9_PKT_VIEWPORT: { auto *v = pkt<hbdx9_pkt_viewport>(h); if (!v) { bad = true; break; } st.vp = *v; break; }
      case HBDX9_PKT_SCISSOR: { auto *s = pkt<hbdx9_pkt_scissor>(h); if (!s) { bad = true; break; } st.scissor = s->rect; break; }
      case HBDX9_PKT_FVF: { auto *f = pkt<hbdx9_pkt_fvf>(h); if (!f) { bad = true; break; } st.use_fvf = true; st.fvf = f->fvf; break; }
      case HBDX9_PKT_VERTEX_DECL: {
        auto *v = pkt<hbdx9_pkt_vertex_decl>(h); if (!v || h->size < sizeof(*v) + v->count * sizeof(hbdx9_vertex_element)) { bad = true; break; }
        const auto *e = (const hbdx9_vertex_element *)(v + 1);
        st.decl.assign(e, e + v->count); st.use_fvf = false;
        break;
      }
      case HBDX9_PKT_TRANSFORM: {
        auto *t = pkt<hbdx9_pkt_transform>(h); if (!t) { bad = true; break; }
        if (t->state == TS_WORLD) memcpy(st.world, t->m, 64);
        else if (t->state >= TS_WORLD1 && t->state <= TS_WORLD3) memcpy(st.worlds[t->state - TS_WORLD1], t->m, 64);   /* (DX9FULL) */
        else if (t->state == TS_VIEW) memcpy(st.view, t->m, 64);
        else if (t->state == TS_PROJECTION) memcpy(st.proj, t->m, 64);
        else if (t->state >= TS_TEXTURE0 && t->state < TS_TEXTURE0 + 8) memcpy(st.texmat[t->state - TS_TEXTURE0], t->m, 64);
        break;
      }
      case HBDX9_PKT_DRAW: {
        auto *dr = pkt<hbdx9_pkt_draw>(h); if (!dr) { bad = true; break; }
        DrawGeometry g{false, nullptr, 0, 0};
        hb_draw(d, dr->primitive, dr->primitive_count, dr->start_vertex, false, 0, 0, g, nullptr, 0);
        break;
      }
      case HBDX9_PKT_DRAW_INDEXED: {
        auto *dr = pkt<hbdx9_pkt_draw_indexed>(h); if (!dr) { bad = true; break; }
        DrawGeometry g{false, nullptr, 0, 0};
        hb_draw(d, dr->primitive, dr->primitive_count, 0, true, dr->base_vertex, dr->start_index, g, nullptr, 0);
        break;
      }
      case HBDX9_PKT_DRAW_UP: {
        auto *dr = pkt<hbdx9_pkt_draw_up>(h); if (!dr || h->size < sizeof(*dr) + dr->vertex_bytes) { bad = true; break; }
        DrawGeometry g{true, (const uint8_t *)(dr + 1), dr->vertex_bytes, dr->stride};
        hb_draw(d, dr->primitive, dr->primitive_count, 0, false, 0, 0, g, nullptr, 0);
        break;
      }
      case HBDX9_PKT_DRAW_INDEXED_UP: {
        auto *dr = pkt<hbdx9_pkt_draw_indexed_up>(h);
        uint32_t ib = dr ? (dr->index_bytes + 3) & ~3u : 0;
        if (!dr || h->size < sizeof(*dr) + ib + dr->vertex_bytes) { bad = true; break; }
        const uint8_t *idx = (const uint8_t *)(dr + 1);
        DrawGeometry g{true, idx + ib, dr->vertex_bytes, dr->stride};
        hb_draw(d, dr->primitive, dr->primitive_count, 0, true, 0, 0, g, idx, dr->index_format);
        break;
      }
      case HBDX9_PKT_UPDATE_BUFFER: {
        auto *u = pkt<hbdx9_pkt_update_buffer>(h); if (!u) { bad = true; break; }
        const uint8_t *src = u->guest_data ? (const uint8_t *)(uintptr_t)u->guest_data : (const uint8_t *)(u + 1);
        if (!u->guest_data && h->size < sizeof(*u) + u->size) { bad = true; break; }
        hb_update_buffer(d, u, src);
        break;
      }
      case HBDX9_PKT_UPDATE_TEXTURE: {
        auto *u = pkt<hbdx9_pkt_update_texture>(h); if (!u) { bad = true; break; }
        const uint8_t *src = u->guest_data ? (const uint8_t *)(uintptr_t)u->guest_data : (const uint8_t *)(u + 1);
        if (!u->guest_data && h->size < sizeof(*u) + u->size) { bad = true; break; }
        hb_update_texture(d, u, src);
        break;
      }
      case HBDX9_PKT_RELEASE_OBJECT: {
        auto *o = pkt<hbdx9_pkt_object>(h); if (!o) { bad = true; break; }
        if (o->index == 1) hb_release_resource(d, (hb_resource *)(uintptr_t)o->object);
        else if (o->index == 2) {
          auto *s = (hb_shader *)(uintptr_t)o->object;
          if (st.vs == s) st.vs = nullptr; if (st.ps == s) st.ps = nullptr;
          // variants stay referenced by cached PSOs; the shader record is small, keep it (TODO: refcount)
        }
        break;
      }
      case HBDX9_PKT_LIGHTS: {
        auto *l = pkt<hbdx9_pkt_lights>(h);
        if (!l || l->count > 8 || h->size < sizeof(*l) + l->count * sizeof(hbdx9_light)) { bad = true; break; }
        memcpy(st.lights, l + 1, l->count * sizeof(hbdx9_light)); st.nlights = l->count;
        break;
      }
      case HBDX9_PKT_MATERIAL: {
        auto *m = pkt<hbdx9_pkt_material>(h); if (!m) { bad = true; break; }
        memcpy(st.material, m->diffuse, 16); memcpy(st.material + 4, m->ambient, 16); memcpy(st.material + 8, m->specular, 16);
        memcpy(st.material + 12, m->emissive, 16); st.material[16] = m->power;
        break;
      }
      case HBDX9_PKT_CLIP_PLANE: { auto *c = pkt<hbdx9_pkt_clip_plane>(h); if (!c) { bad = true; break; } if (c->index < 6) memcpy(st.clip[c->index], c->plane, 16); break; }
      case HBDX9_PKT_BLIT: { auto *b = pkt<hbdx9_pkt_blit>(h); if (!b) { bad = true; break; } hb_blit(d, b); break; }
      case HBDX9_PKT_COLOR_FILL: { auto *f = pkt<hbdx9_pkt_color_fill>(h); if (!f) { bad = true; break; } hb_color_fill(d, f); break; }
      case HBDX9_PKT_FENCE: {
        auto *f = pkt<hbdx9_pkt_fence>(h); if (!f || f->slot >= 1024) { bad = true; break; }
        hb_cmd(d); d->pending_fences.push_back({f->slot, f->value});
        break;
      }
      case HBDX9_PKT_FLUSH: hb_commit(d, false, false); break;
      case HBDX9_PKT_TIMESTAMP: {   /* (DX9FULL) */
        auto *f = pkt<hbdx9_pkt_fence>(h); if (!f || f->slot + 2 >= 1023) { bad = true; break; }
        hb_cmd(d); d->pending_ts.push_back({f->slot, f->value}); hb_commit(d, false, false);
        break;
      }
      case HBDX9_PKT_QUERY: {
        auto *q = pkt<hbdx9_pkt_query>(h); if (!q || q->slot + 1 >= 1024) { bad = true; break; }
        if (q->begin) {
          if (d->query_active) { if (d->enc) [d->enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0]; hb_cmd(d); d->pending_queries.push_back(d->query); }
          d->query_active = true; d->query = hb_query_pass{}; d->query.slot = q->slot; d->query.value = q->value;
          if (d->enc) {
            uint32_t off = (d->vis_next++ % 8192) * 8;
            d->query.offsets.push_back(off);
            [d->enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:off];
          }
        } else if (d->query_active && d->query.slot == q->slot) {
          if (d->enc) [d->enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
          d->query_active = false; d->query.value = q->value;
          hb_cmd(d); d->pending_queries.push_back(d->query);
        } else {
          /* an end without a begin: report 0 at completion */
          hb_query_pass e; e.slot = q->slot; e.value = q->value; hb_cmd(d); d->pending_queries.push_back(e);
        }
        break;
      }
      case HBDX9_PKT_GENERATE_MIPS: { auto *o = pkt<hbdx9_pkt_object>(h); if (!o) { bad = true; break; } hb_gen_mips(d, (hb_resource *)(uintptr_t)o->object); break; }
      case HBDX9_PKT_RESET_STATE: hb_end_pass(d); hb_reset_state(d); break;
      default: bad = true; break;
      }
      if (bad) { p->status = index; WARN("bad packet %u type=%u size=%u\n", index, h->type, h->size); return STATUS_SUCCESS; }
      at += (h->size + 7) & ~7u;
    }
  }
  p->status = 0;
  return STATUS_SUCCESS;
}

#define HBDX9_SEQ_SLOT 1023   /* fence_page slot the shim reserves for the run-SUBMIT sequence (session 6) */
static void hb_publish_seq(hb_device *d, uint32_t seq) {
  if (d->fence_page) __atomic_store_n((uint32_t *)&d->fence_page[HBDX9_SEQ_SLOT], seq, __ATOMIC_RELEASE);
}
static uint32_t hb_count_presents(const uint8_t *data, uint32_t size) {
  uint32_t n = 0;
  for (uint32_t at = 0; at + sizeof(hbdx9_packet) <= size;) {
    const hbdx9_packet *h = (const hbdx9_packet *)(data + at);
    if (h->size < sizeof(*h)) break;
    n += h->type == HBDX9_PKT_PRESENT;
    at += (h->size + 7) & ~7u;
  }
  return n;
}
/* (INT32G patch 0004) HBDX9_TSO_COPY=1: a copy made on a calling (guest) thread.  The translator leaves hardware TSO on there;
   on performance cores a copy that moves pairs of 128-bit registers (libsystem memcpy, and adjacent loads/stores the compiler
   pairs up) then runs 3-5 GB/s, 16 bytes per step 35-40.  In assembly so that the compiler cannot pair the steps. */
__attribute__((noinline)) static void hb_copy_under_tso(uint8_t *d, const uint8_t *s, size_t n) {
  size_t steps = n >> 4;
  if (steps)
    __asm__ volatile("1: ldr q0, [%[s]], #16\n str q0, [%[d]], #16\n subs %[k], %[k], #1\n b.ne 1b"
                     : [d] "+r"(d), [s] "+r"(s), [k] "+r"(steps) : : "v0", "cc", "memory");
  for (n &= 15; n; n--) *d++ = *s++;
}
/* (INT32G patch 0004) the next chunk of the ring, if any (worker only, under the queue lock).  The load of `head` is an
   acquire load: the slot is read after it, and the shim wrote the slot before it moved `head`. */
static bool hb_ring_next(hb_device *d, hb_device::hb_job &job) {
  hbdx9_ring *r = d->ring;
  if (!r || __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) == d->ring_tail) return false;
  const hbdx9_ring_slot *s = &r->slot[d->ring_tail & (HBDX9_RING_SLOTS - 1)];
  job.data = (const uint8_t *)(uintptr_t)s->data; job.size = s->size; job.presents = s->presents; job.ring = true;
  return true;
}
static void hb_worker(hb_device *d) {
  bool watch = false;                  /* a chunk was just run: the next one is likely on its way */
  for (;;) {
    hb_device::hb_job job;
    {
      std::unique_lock<std::mutex> l(d->amx);
      for (;;) {
        /* queued SUBMITs first: a direct SUBMIT drains the ring before it queues, so whatever is queued is older than the ring */
        if (!d->aq.empty()) { job = d->aq.front(); d->aq.pop_front(); d->aq_pending.fetch_sub(1, std::memory_order_relaxed); break; }
        if (hb_ring_next(d, job)) { job.seq = ++d->seq_submitted; d->ring_chunks++; d->ring_bytes += job.size; break; }
        if (!d->ring) { d->acv_work.wait(l, [d] { return !d->aq.empty() || d->ring; }); continue; }
        hbdx9_ring *r = d->ring;
        if (watch && d->ring_spin_us) {
          /* the ring ran dry right after work: watch it for a while without the lock, then park */
          uint32_t tail = d->ring_tail; bool hit = false;
          l.unlock();
          for (uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), n = 0;; n++) {
            if (__atomic_load_n(&r->head, __ATOMIC_ACQUIRE) != tail || d->aq_pending.load(std::memory_order_acquire)) { hit = true; break; }
            __asm__ volatile("isb");
            if ((n & 31) == 31 && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0 > (uint64_t)d->ring_spin_us * 1000) break;
          }
          l.lock();
          watch = false;
          if (hit) { d->ring_spin_hits++; continue; }
          if (d->ring != r) continue;                          /* attached anew meanwhile */
        }
        __atomic_store_n(&r->sleeping, 1u, __ATOMIC_SEQ_CST);  /* store, then load head: the shim does store head, then load sleeping */
        if (__atomic_load_n(&r->head, __ATOMIC_SEQ_CST) == d->ring_tail && d->aq.empty()) {
          d->ring_parks++;
          d->acv_work.wait_for(l, std::chrono::microseconds(d->ring_park_us));
        }
        __atomic_store_n(&r->sleeping, 0u, __ATOMIC_SEQ_CST);
      }
      d->aworking = true;
    }
    hbdx9_submit_params sp{};
    sp.device = (uint64_t)(uintptr_t)d; sp.data = (uint64_t)(uintptr_t)job.data; sp.size = job.size;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    @autoreleasepool { hb_execute_submit(&sp); }
    d->worker_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    if (sp.status) WARN("async submit %u: bad packet %u\n", job.seq, sp.status);
    delete job.vec; free(job.owned); watch = true;
    {
      std::lock_guard<std::mutex> g(d->amx);
      d->aworking = false; d->presents_done += job.presents;
      hb_publish_seq(d, job.seq);
      if (job.ring) {
        d->ring_tail++; d->ring_presents_done += job.presents;
        __atomic_store_n(&d->ring->presents_done, d->ring_presents_done, __ATOMIC_RELEASE);
        __atomic_store_n(&d->ring->tail, d->ring_tail, __ATOMIC_RELEASE);
      }
    }
    d->acv_done.notify_all();
  }
}
/* the caller of any non-SUBMIT call: wait until the worker has run everything queued (the core is then single-threaded) */
static void hb_drain(hb_device *d) {
  if (!d || !d->async) return;
  std::unique_lock<std::mutex> l(d->amx);
  d->drains++;
  /* (INT32G patch 0004) idle = nothing queued, nothing running, nothing in the ring (the caller is the ring's only producer) */
  auto idle = [d] { return d->aq.empty() && !d->aworking && (!d->ring || __atomic_load_n(&d->ring->head, __ATOMIC_ACQUIRE) == d->ring_tail); };
  if (!idle()) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (d->ring) d->acv_work.notify_one();
    d->drain_waits++; d->acv_done.wait(l, idle);
    d->drain_wait_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
  }
}
/* (INT32G patch 0004) the calls of the chunk ring that travel as SUBMIT with a flag: never a submit, no sequence number */
static NTSTATUS hb_ring_call(hb_device *d, hbdx9_submit_params *p, uint32_t flags) {
  p->status = 0;
  if (flags & HBDX9_SUBMIT_RING_ATTACH) {
    auto *r = (hbdx9_ring *)(uintptr_t)p->data;
    if (r && (p->size < sizeof(hbdx9_ring) || r->magic != HBDX9_RING_MAGIC || r->slots != HBDX9_RING_SLOTS)) { p->status = 1; return STATUS_SUCCESS; }
    if (r && d->ring && d->ring != r) { p->status = 2; return STATUS_SUCCESS; }   /* one ring per native device: a second shim device keeps calling */
    hb_drain(d);
    {
      std::lock_guard<std::mutex> g(d->amx);
      d->ring = r; d->ring_tail = r ? r->head : 0; d->ring_presents_done = 0;
      if (r) { r->tail = d->ring_tail; r->presents_done = 0; r->sleeping = 0; }
    }
    d->acv_work.notify_one();
    return STATUS_SUCCESS;
  }
  if (flags & HBDX9_SUBMIT_RING_WAKE) {
    { std::lock_guard<std::mutex> g(d->amx); d->ring_wakes++; }      /* the lock: the worker is either before its check or already waiting */
    d->acv_work.notify_one();
    return STATUS_SUCCESS;
  }
  std::unique_lock<std::mutex> l(d->amx);                            /* HBDX9_SUBMIT_RING_WAIT */
  hbdx9_ring *r = d->ring; uint32_t kind = flags & 0xff, want = p->size;
  if (!r) { p->status = 1; return STATUS_SUCCESS; }
  d->ring_waits++;
  d->acv_work.notify_one();
  uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (kind == 1) d->acv_done.wait(l, [&] { return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - d->ring_tail < HBDX9_RING_SLOTS; });
  else if (kind == 2) {
    d->pacing_waits++;
    d->acv_done.wait(l, [&] { return (int32_t)(want - d->ring_presents_done) <= 2; });
    d->pacing_wait_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
  } else d->acv_done.wait(l, [&] { return d->aq.empty() && !d->aworking && __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) == d->ring_tail; });
  return STATUS_SUCCESS;
}
static NTSTATUS hb_submit(void *args) {
  auto *p = (hbdx9_submit_params *)args;
  auto *d = (hb_device *)(uintptr_t)p->device;
  uint32_t flags = p->status;          /* (INT32G patch 0004) on entry: HBDX9_SUBMIT_* flags; a shim before patch 0005 passes 0 */
  if (d && d->async && (flags & (HBDX9_SUBMIT_RING_ATTACH | HBDX9_SUBMIT_RING_WAKE | HBDX9_SUBMIT_RING_WAIT))) return hb_ring_call(d, p, flags);
  if (!d || !p->data) { p->status = 1; return STATUS_SUCCESS; }
  if (d->ring) hb_drain(d);            /* a direct SUBMIT after ring chunks: one order, one sequence counter */
  uint32_t seq = ++d->seq_submitted;
  if (!d->async) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    NTSTATUS st = hb_execute_submit(p);
    d->sync_submit_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    hb_publish_seq(d, seq);
    return st;
  }
  /* copy the packet buffer (the guest reuses it at once); memory a packet references by pointer stays the guest's until the
     shim sees this sequence number published */
  hb_device::hb_job job; job.seq = seq; job.size = p->size;
  const uint8_t *src = (const uint8_t *)(uintptr_t)p->data;
  if (flags & HBDX9_SUBMIT_NOCOPY) { job.data = src; d->nocopy_jobs++; }       /* (INT32G patch 0004) the shim rotates its buffers */
  else if (d->tso_copy) {                                                       /* (INT32G patch 0004) HBDX9_TSO_COPY=1 */
    job.owned = (uint8_t *)malloc(p->size ? p->size : 1);
    hb_copy_under_tso(job.owned, src, p->size); job.data = job.owned; d->tso_copy_jobs++;
  } else {
    job.vec = new std::vector<uint8_t>(8 + (size_t)p->size);
    memcpy(job.vec->data(), &seq, 4);
    memcpy(job.vec->data() + 8, src, p->size);
    job.data = job.vec->data() + 8; d->copied_jobs++;
  }
  job.presents = (flags & HBDX9_SUBMIT_HAS_PRESENTS) ? (flags & 0xff) : hb_count_presents(job.data, p->size);
  uint32_t presents = job.presents;
  {
    std::unique_lock<std::mutex> l(d->amx);
    d->aq.push_back(job); d->aq_pending.fetch_add(1, std::memory_order_release);
    d->async_jobs++; d->async_bytes += p->size; d->presents_enq += presents;
    d->acv_work.notify_one();
    /* frame pacing: the game may run at most two presented frames ahead of the worker */
    if (d->presents_enq - d->presents_done > 2) {
      d->pacing_waits++;
      uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      d->acv_done.wait(l, [d] { return d->presents_enq - d->presents_done <= 2; });
      d->pacing_wait_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    }
  }
  p->status = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS hb_readback(void *args) {
  auto *p = (hbdx9_readback_params *)args;
  auto *d = (hb_device *)(uintptr_t)p->device;
  auto *r = (hb_resource *)(uintptr_t)p->surface;
  uint8_t *dst = (uint8_t *)(uintptr_t)p->dst;
  hb_drain(d);
  if (!d || !r || !r->texture || !dst) { p->status = 1; return STATUS_SUCCESS; }
  uint32_t w = std::max<uint32_t>(1, r->width >> p->level), h = std::max<uint32_t>(1, r->height >> p->level);
  uint32_t bpp = r->fi.bytes;
  if (r->fi.conv && p->width == w && p->height == h && p->pitch >= w * r->fi.gbytes) {
    /* (DX9FULL) a converted format is not converted back: a lock sees zeros (D3D9 games lock such surfaces to write a frame) */
    for (uint32_t y = 0; y < h; y++) memset(dst + (size_t)y * p->pitch, 0, (size_t)w * r->fi.gbytes);
    p->status = 0; return STATUS_SUCCESS;
  }
  if (p->width != w || p->height != h || r->fi.block != 1 || p->pitch < w * bpp) { p->status = 1; return STATUS_SUCCESS; }
  @autoreleasepool {
    NSUInteger row = w * bpp, bytes = row * h;
    id<MTLBuffer> buf = [d->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    hb_end_pass(d);
    id<MTLBlitCommandEncoder> b = [hb_cmd(d) blitCommandEncoder];
    [b copyFromTexture:r->texture sourceSlice:0 sourceLevel:p->level sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:bytes];
    [b endEncoding];
    hb_commit(d, true, false);
    const uint8_t *src = (const uint8_t *)[buf contents];
    for (uint32_t y = 0; y < h; y++) {
      if (d->tso_copy) hb_copy_under_tso(dst + (size_t)y * p->pitch, src + (size_t)y * row, row);     /* (INT32G patch 0004) */
      else memcpy(dst + (size_t)y * p->pitch, src + (size_t)y * row, row);
    }
    [buf release];
  }
  p->status = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS hb_release(void *args) {
  auto *p = (hbdx9_release_params *)args;
  @autoreleasepool {
    if (p->kind == 1) {
      auto *sc = (hb_swapchain *)(uintptr_t)p->object;
      if (sc) hb_drain(sc->dev);
      if (sc) {
        hb_commit(sc->dev, true, false);
        hb_release_resource(sc->dev, sc->backbuffer);
        hb_release_resource(sc->dev, sc->depth);
        [sc->layer release];
        auto *mf = (macdrv_functions_t *)dlsym(RTLD_DEFAULT, "macdrv_functions");
        if (mf && mf->macdrv_view_release_metal_view && sc->view) mf->macdrv_view_release_metal_view(sc->view);
        delete sc;
      }
    }
  }
  p->status = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS hb_stats(void *args) {
  auto *p = (hbdx9_stats_params *)args;
  auto *d = (hb_device *)(uintptr_t)p->device;
  if (!d) return STATUS_SUCCESS;
  hb_drain(d);
  p->variants_compiled = d->variants_compiled; p->variants_cached = d->variants_cached;
  p->psos_created = (uint32_t)d->psos.size(); p->draws = (uint32_t)d->draws;
  p->cache_rows_written = d->cache.writes; p->cache_ready = d->cache.db != nullptr;
  p->pad[0] = d->cache.pso_hits; p->pad[1] = d->cache.pso_misses;
  hb_archive_save(d);
  /* The shim requests stats when it releases the device. */
  for (auto *pd : d->cache.archive_entries) [pd release];
  d->cache.archive_entries.clear();
  d->cache.archive_dirty = false;
  fprintf(stderr, "hbdx9-core: stats variants_compiled=%u variants_cached=%u cache_writes=%u psos=%u pso_archive_hits=%u pso_archive_misses=%u draws=%u cache=%s\n",
          p->variants_compiled, p->variants_cached, p->cache_rows_written, p->psos_created, d->cache.pso_hits, d->cache.pso_misses,
          p->draws, p->cache_ready ? "on" : "off");
  fprintf(stderr, "hbdx9-core: uploads mode=%s buffer n=%llu offered=%llu compared=%llu written=%llu nochange=%llu renames=%llu inplace_busy=%llu "
          "texture n=%llu bytes=%llu blits=%llu\n", d->uploads_old ? "old" : "new",
          (unsigned long long)d->up.buf_n, (unsigned long long)d->up.buf_offered, (unsigned long long)d->up.buf_compared,
          (unsigned long long)d->up.buf_written, (unsigned long long)d->up.buf_nochange, (unsigned long long)d->up.buf_renames,
          (unsigned long long)d->up.buf_inplace_busy, (unsigned long long)d->up.tex_n, (unsigned long long)d->up.tex_bytes,
          (unsigned long long)d->up.tex_blits);
  fprintf(stderr, "hbdx9-core: async mode=%s jobs=%llu bytes=%llu pacing_waits=%llu pacing_wait_ms=%llu drains=%llu drain_waits=%llu "
          "drain_wait_ms=%llu worker_ms=%llu sync_submit_ms=%llu submits=%u frames=%llu\n",
          d->async ? "on" : "off", (unsigned long long)d->async_jobs, (unsigned long long)d->async_bytes,
          (unsigned long long)d->pacing_waits, (unsigned long long)(d->pacing_wait_ns / 1000000), (unsigned long long)d->drains,
          (unsigned long long)d->drain_waits, (unsigned long long)(d->drain_wait_ns / 1000000), (unsigned long long)(d->worker_ns / 1000000),
          (unsigned long long)(d->sync_submit_ns / 1000000), d->seq_submitted, (unsigned long long)d->frames_presented);
  fprintf(stderr, "hbdx9-core: transport jobs copied=%llu tso_copied=%llu nocopy=%llu; ring attached=%d chunks=%llu bytes=%llu parks=%llu "
          "wakes=%llu waits=%llu spin_hits=%llu spin_us=%u park_us=%u\n", (unsigned long long)d->copied_jobs, (unsigned long long)d->tso_copy_jobs,
          (unsigned long long)d->nocopy_jobs, d->ring ? 1 : 0, (unsigned long long)d->ring_chunks, (unsigned long long)d->ring_bytes,
          (unsigned long long)d->ring_parks, (unsigned long long)d->ring_wakes, (unsigned long long)d->ring_waits,
          (unsigned long long)d->ring_spin_hits, d->ring_spin_us, d->ring_park_us);
  if (d->prof_on) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    auto ms = [&](uint64_t t) { return (double)t * tb.numer / tb.denom / 1e6; };
    fprintf(stderr, "hbdx9-core: profile draws=%llu ms: begin_pass=%.1f layout_ffp=%.1f variants=%.1f pso=%.1f state=%.1f args_consts_vb=%.1f useresource=%.1f encode=%.1f\n",
            (unsigned long long)d->prof_draws, ms(d->prof[0]), ms(d->prof[1]), ms(d->prof[2]), ms(d->prof[3]), ms(d->prof[4]), ms(d->prof[5]), ms(d->prof[6]), ms(d->prof[7]));
  }
  {
    hb_present_stats *ps = d->pres;
    fprintf(stderr, "hbdx9-core: present interval=%x sync=%d refresh=%.2f pace_mhz=%#x frames=%llu shown=%llu unshown=%llu "
            "shown_step_ms <5:%llu 5-7:%llu 7-10:%llu 10-14:%llu 14-20:%llu 20-30:%llu 30-45:%llu >=45:%llu drawable_wait_ms=%llu "
            "drawable_waits=%llu pace_sleeps=%llu pace_sleep_ms=%llu force_immediate=%llu sync_toggles=%llu lod_textures=%llu\n", d->last_interval, (int)d->last_sync, d->last_refresh, d->last_pace_mhz,
            (unsigned long long)d->frames_presented, (unsigned long long)ps->shown.load(), (unsigned long long)ps->unshown.load(),
            (unsigned long long)ps->bins[0].load(), (unsigned long long)ps->bins[1].load(), (unsigned long long)ps->bins[2].load(),
            (unsigned long long)ps->bins[3].load(), (unsigned long long)ps->bins[4].load(), (unsigned long long)ps->bins[5].load(),
            (unsigned long long)ps->bins[6].load(), (unsigned long long)ps->bins[7].load(), (unsigned long long)(d->drawable_wait_ns / 1000000),
            (unsigned long long)d->drawable_waits, (unsigned long long)d->pace_sleeps, (unsigned long long)(d->pace_sleep_ns / 1000000),
            (unsigned long long)d->force_immediate, (unsigned long long)d->sync_toggles, (unsigned long long)d->lod_textures);
  }
  /* (DX9FULL 13.11.2026) the new paths as used by this device (0 everywhere = the game never asked for any of them) */
  fprintf(stderr, "hbdx9-core: fmt-ext on=%d df_draws=%llu fetch4_draws=%llu nvdb_draws=%llu atoc_draws=%llu border_samplers=%llu blendfactor_draws=%llu point_draws=%llu vertexblend_draws=%llu\n",
          (int)d->fmt_ext, (unsigned long long)d->ext_df_draws, (unsigned long long)d->ext_fetch4_draws, (unsigned long long)d->ext_nvdb_draws,
          (unsigned long long)d->ext_atoc_draws, (unsigned long long)d->ext_border_samplers, (unsigned long long)d->ext_blendfactor_draws,
          (unsigned long long)d->ext_point_draws, (unsigned long long)d->ext_blend_draws);
  fprintf(stderr, "hbdx9-core: diag serial=%d split_n=%u render_passes=%llu fence_waits=%llu commit_waits=%llu isolated_draws=%llu\n", d->serial_debug, d->split_n,
          (unsigned long long)d->diag_passes, (unsigned long long)d->diag_fence_waits, (unsigned long long)d->diag_commit_waits,
          (unsigned long long)d->diag_iso);
  {
    std::string fr; for (auto f : d->fb.first_frames) fr += std::to_string(f) + " ";
    fprintf(stderr, "hbdx9-core: feedback sampled-while-attached bindings=%llu color=%llu depth=%llu depth_with_zwrite=%llu barrier_splits=%llu barrier=%s detach=%s detach_draws=%llu detach_passes=%llu rt_inits=%llu tex_inits=%llu first_frames=[%s]\n",
            (unsigned long long)d->fb.draws, (unsigned long long)d->fb.color, (unsigned long long)d->fb.depth,
            (unsigned long long)d->fb.depth_written, (unsigned long long)d->fb.splits, d->feedback_barrier ? "on" : "off", d->feedback_detach ? "on" : "off",
            (unsigned long long)d->detach_draws, (unsigned long long)d->detach_passes, (unsigned long long)d->rt_inits, (unsigned long long)d->tex_inits, fr.c_str());
  }
  return STATUS_SUCCESS;
}

/* (DX9FULL 13.11.2026) measured features.  Depth bounds: Metal declares -[MTLRenderCommandEncoder setDepthTestMinBound:maxBound:]
   from macOS 26 without a capability query, and an Apple M1 Pro (Apple7, macOS 27.0.1) ignores it (stand dbtest.m: bounds
   [0.5, 1] leave a fragment over stored depth 0.25 drawn, the positive control - depth compare LESS - rejects it).  So the
   layer offers NVDB only where this 4x1 draw shows the bounds rejecting: stored depth 0.25 | 0.75, bounds [0.5, 1], red at 0.5. */
static int hb_depth_bounds_work(hb_device *d) {
  static const char *msl =
      "#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
      "vertex V hb_db_vs(uint id [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
      "  float2 c = float2(id & 1, id >> 1); V o; o.p = float4(mix(r.x, r.y, c.x), mix(-1.0, 1.0, c.y), r.z, 1.0); return o; }\n"
      "fragment float4 hb_db_fs() { return float4(1.0, 0.0, 0.0, 1.0); }\n";
  @autoreleasepool {
    NSError *err = nil;
    id<MTLLibrary> lib = [d->device newLibraryWithSource:[NSString stringWithUTF8String:msl] options:nil error:&err];
    if (!lib) return 0;
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vf = [lib newFunctionWithName:@"hb_db_vs"], ff = [lib newFunctionWithName:@"hb_db_fs"];
    pd.vertexFunction = vf; pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> pso = [d->device newRenderPipelineStateWithDescriptor:pd error:&err];
    [pd release]; [vf release]; [ff release]; [lib release];
    if (!pso) return 0;
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dw = [d->device newDepthStencilStateWithDescriptor:dd];
    dd.depthWriteEnabled = NO;
    id<MTLDepthStencilState> dn = [d->device newDepthStencilStateWithDescriptor:dd];
    [dd release];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:4 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> col = [d->device newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatDepth32Float;
    id<MTLTexture> dep = [d->device newTextureWithDescriptor:td];
    id<MTLBuffer> rb = [d->device newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [d->queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = col; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.depthAttachment.texture = dep; rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare; rp.depthAttachment.clearDepth = 1.0;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pso];
    float l[4] = {-1, 0, 0.25f, 0}, r[4] = {0, 1, 0.75f, 0}, f[4] = {-1, 1, 0.5f, 0};
    [e setDepthStencilState:dw];
    [e setVertexBytes:l length:16 atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e setVertexBytes:r length:16 atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e setDepthStencilState:dn];
    [e setDepthTestMinBound:0.5f maxBound:1.0f];
    [e setVertexBytes:f length:16 atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:col sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 1, 1)
              toBuffer:rb destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:16];
    [b endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    const uint32_t *px = (const uint32_t *)[rb contents];
    int ok = cb.status == MTLCommandBufferStatusCompleted && (px[0] & 0xffffff) == 0 && (px[3] & 0xffffff) == 0xff0000;
    fprintf(stderr, "hbdx9-core: depth bounds self-test left=%08x right=%08x -> %s\n", px[0], px[3], ok ? "honoured" : "ignored by this GPU");
    [rb release]; [dep release]; [col release]; [dw release]; [dn release]; [pso release];
    return ok;
  }
}
static NTSTATUS hb_features(void *args) {
  auto *p = (hbdx9_features_params *)args;
  auto *d = (hb_device *)(uintptr_t)p->device;
  p->result = 0;
  if (!d) return STATUS_SUCCESS;
  hb_drain(d);
  if (p->query == 1) {
    static int db = -1;   /* one GPU per process */
    if (db < 0) db = hb_depth_bounds_work(d);
    p->result = (uint32_t)db;
  }
  return STATUS_SUCCESS;
}

typedef NTSTATUS (*unixlib_entry_t)(void *args);
extern "C" __attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[] = {
    hb_init, hb_swapchain_create, hb_submit, hb_readback, hb_release, hb_create_resource, hb_create_shader, hb_stats, hb_features,
};
extern "C" __attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    hb_init, hb_swapchain_create, hb_submit, hb_readback, hb_release, hb_create_resource, hb_create_shader, hb_stats, hb_features,
};
static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == HBDX9_CALL_COUNT, "table");
