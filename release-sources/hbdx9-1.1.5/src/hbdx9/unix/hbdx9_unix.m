/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HyperBridge DX9 — native ARM64 core (Wine unix library d3d9.so).  Lane HBDX9, 07.11.2026.
 *
 * Never translated: this is where Metal objects live and where every packet from the guest shim is executed.
 * Milestone (a): device + queue, CAMetalLayer bound to the guest HWND through winemac (same route as winemetal's
 * CreateMetalViewFromHWND), back buffer texture, CLEAR (scissored full-screen triangle, so rects work), PRESENT
 * (blit back buffer -> drawable), READBACK (blit -> shared buffer -> guest memory).
 * The same functions serve __wine_unix_call_funcs and __wine_unix_call_wow64_funcs: hbdx9_protocol.h layouts are
 * identical for i386 and 64-bit guests and guest pointers travel as uint64_t. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hbdx9_protocol.h"

typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)

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

struct hb_device {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLRenderPipelineState> clear_pso;
  id<MTLCommandBuffer> cmd;            /* open command buffer of the current frame, nil when none */
};
struct hb_surface {
  id<MTLTexture> texture;
};
struct hb_swapchain {
  struct hb_device *dev;
  macdrv_metal_view view;
  CAMetalLayer *layer;
  struct hb_surface *backbuffer;
  uint32_t width, height;
};

static int hb_trace(void) { static int t = -1; if (t < 0) t = getenv("HBDX9_TRACE") != NULL; return t; }

static const char *clear_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V hb_clear_vs(uint id [[vertex_id]]) {\n"
    "  V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
    "fragment float4 hb_clear_fs(constant float4 &c [[buffer(0)]]) { return c; }\n";

static id<MTLCommandBuffer> hb_cmd(struct hb_device *d) {
  if (!d->cmd) d->cmd = [[d->queue commandBuffer] retain];
  return d->cmd;
}
static void hb_commit(struct hb_device *d, int wait) {
  if (!d->cmd) return;
  [d->cmd commit];
  if (wait) [d->cmd waitUntilCompleted];
  [d->cmd release];
  d->cmd = nil;
}

static NTSTATUS hb_init(void *args) {
  struct hbdx9_init_params *p = args;
  @autoreleasepool {
    if (p->version != HBDX9_PROTOCOL_VERSION) { p->status = 1; return STATUS_SUCCESS; }
    struct hb_device *d = calloc(1, sizeof(*d));
    d->device = MTLCreateSystemDefaultDevice();
    if (!d->device) { free(d); p->status = 2; return STATUS_SUCCESS; }
    d->queue = [d->device newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [d->device newLibraryWithSource:[NSString stringWithUTF8String:clear_msl] options:nil error:&err];
    if (!lib) { fprintf(stderr, "hbdx9-core: clear library: %s\n", [[err description] UTF8String]); p->status = 3; return STATUS_SUCCESS; }
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [[lib newFunctionWithName:@"hb_clear_vs"] autorelease];
    pd.fragmentFunction = [[lib newFunctionWithName:@"hb_clear_fs"] autorelease];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d->clear_pso = [d->device newRenderPipelineStateWithDescriptor:pd error:&err];
    [pd release]; [lib release];
    if (!d->clear_pso) { p->status = 4; return STATUS_SUCCESS; }
    p->device = (uint64_t)(uintptr_t)d;
    snprintf(p->name, sizeof(p->name), "%s (HyperBridge DX9)", [[d->device name] UTF8String]);
    p->vendor_id = 0x106b; p->device_id = 0;
    p->vram_bytes = [d->device recommendedMaxWorkingSetSize];
    p->status = 0;
    fprintf(stderr, "hbdx9-core: init device=%p name=\"%s\"\n", (void *)d, p->name);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS hb_swapchain_create(void *args) {
  struct hbdx9_swapchain_params *p = args;
  struct hb_device *d = (struct hb_device *)(uintptr_t)p->device;
  if (!d || !p->width || !p->height) { p->status = 1; return STATUS_SUCCESS; }
  @autoreleasepool {
    struct macdrv_functions_t *mf = dlsym(RTLD_DEFAULT, "macdrv_functions");
    if (!mf || !mf->get_win_data || !mf->macdrv_view_create_metal_view || !mf->macdrv_view_get_metal_layer) {
      fprintf(stderr, "hbdx9-core: winemac functions not found\n"); p->status = 2; return STATUS_SUCCESS;
    }
    struct macdrv_win_data *wd = mf->get_win_data((HWND)(uintptr_t)p->hwnd);
    macdrv_metal_view view = NULL;
    if (wd && wd->client_cocoa_view) view = mf->macdrv_view_create_metal_view(wd->client_cocoa_view, (macdrv_metal_device)d->device);
    if (wd) mf->release_win_data(wd);
    CAMetalLayer *layer = view ? (CAMetalLayer *)mf->macdrv_view_get_metal_layer(view) : nil;
    if (!layer) { fprintf(stderr, "hbdx9-core: no CAMetalLayer for hwnd=%llx\n", (unsigned long long)p->hwnd); p->status = 3; return STATUS_SUCCESS; }
    layer.device = d->device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.drawableSize = CGSizeMake(p->width, p->height);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:p->width height:p->height mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    struct hb_swapchain *sc = calloc(1, sizeof(*sc));
    sc->dev = d; sc->view = view; sc->layer = [layer retain]; sc->width = p->width; sc->height = p->height;
    sc->backbuffer = calloc(1, sizeof(*sc->backbuffer));
    sc->backbuffer->texture = [d->device newTextureWithDescriptor:td];
    p->swapchain = (uint64_t)(uintptr_t)sc;
    p->backbuffer = (uint64_t)(uintptr_t)sc->backbuffer;
    p->status = 0;
    fprintf(stderr, "hbdx9-core: swapchain %ux%u hwnd=%llx layer=%p\n", p->width, p->height, (unsigned long long)p->hwnd, (void *)layer);
  }
  return STATUS_SUCCESS;
}

static void hb_clear(struct hb_device *d, const struct hbdx9_pkt_clear *c, const struct hbdx9_rect *rects) {
  struct hb_surface *s = (struct hb_surface *)(uintptr_t)c->target;
  if (!s || !s->texture || !c->rect_count) return;
  float color[4] = {((c->color >> 16) & 0xff) / 255.0f, ((c->color >> 8) & 0xff) / 255.0f, (c->color & 0xff) / 255.0f,
                    ((c->color >> 24) & 0xff) / 255.0f};
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = s->texture;
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  NSUInteger w = s->texture.width, h = s->texture.height;
  const struct hbdx9_rect *r0 = &rects[0];
  if (c->rect_count == 1 && r0->x1 <= 0 && r0->y1 <= 0 && (NSUInteger)r0->x2 >= w && (NSUInteger)r0->y2 >= h) {
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;  /* whole target: clear on load, no draw */
    rp.colorAttachments[0].clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);
    id<MTLRenderCommandEncoder> e = [hb_cmd(d) renderCommandEncoderWithDescriptor:rp];
    [e endEncoding];
    return;
  }
  rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
  id<MTLRenderCommandEncoder> e = [hb_cmd(d) renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:d->clear_pso];
  [e setFragmentBytes:color length:sizeof(color) atIndex:0];
  for (uint32_t i = 0; i < c->rect_count; i++) {
    MTLScissorRect sr = {(NSUInteger)rects[i].x1, (NSUInteger)rects[i].y1, (NSUInteger)(rects[i].x2 - rects[i].x1),
                         (NSUInteger)(rects[i].y2 - rects[i].y1)};
    if (sr.x + sr.width > w || sr.y + sr.height > h || !sr.width || !sr.height) continue;
    [e setScissorRect:sr];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  [e endEncoding];
}

static void hb_present(struct hb_device *d, struct hb_swapchain *sc) {
  id<CAMetalDrawable> drawable = [sc->layer nextDrawable];
  id<MTLCommandBuffer> cmd = hb_cmd(d);
  if (drawable && drawable.texture.width == sc->width && drawable.texture.height == sc->height) {
    id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
    [b copyFromTexture:sc->backbuffer->texture toTexture:drawable.texture];
    [b endEncoding];
    [cmd presentDrawable:drawable];
  } else if (hb_trace()) {
    fprintf(stderr, "hbdx9-core: present skipped drawable=%p\n", (void *)drawable);
  }
  hb_commit(d, 0);
}

static NTSTATUS hb_submit(void *args) {
  struct hbdx9_submit_params *p = args;
  struct hb_device *d = (struct hb_device *)(uintptr_t)p->device;
  const uint8_t *data = (const uint8_t *)(uintptr_t)p->data;
  uint32_t at = 0, index = 0;
  if (!d || !data) { p->status = 1; return STATUS_SUCCESS; }
  @autoreleasepool {
    while (at + sizeof(struct hbdx9_packet) <= p->size) {
      const struct hbdx9_packet *h = (const struct hbdx9_packet *)(data + at);
      index++;
      if (h->size < sizeof(*h) || at + h->size > p->size) { p->status = index; return STATUS_SUCCESS; }
      switch (h->type) {
      case HBDX9_PKT_CLEAR: {
        const struct hbdx9_pkt_clear *c = (const struct hbdx9_pkt_clear *)h;
        if (h->size < sizeof(*c) + c->rect_count * sizeof(struct hbdx9_rect)) { p->status = index; return STATUS_SUCCESS; }
        hb_clear(d, c, (const struct hbdx9_rect *)(c + 1));
        break;
      }
      case HBDX9_PKT_PRESENT: {
        const struct hbdx9_pkt_present *pr = (const struct hbdx9_pkt_present *)h;
        hb_present(d, (struct hb_swapchain *)(uintptr_t)pr->swapchain);
        break;
      }
      default:
        p->status = index; return STATUS_SUCCESS;
      }
      at += (h->size + 3) & ~3u;
    }
  }
  p->status = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS hb_readback(void *args) {
  struct hbdx9_readback_params *p = args;
  struct hb_device *d = (struct hb_device *)(uintptr_t)p->device;
  struct hb_surface *s = (struct hb_surface *)(uintptr_t)p->surface;
  uint8_t *dst = (uint8_t *)(uintptr_t)p->dst;
  if (!d || !s || !dst || p->width != s->texture.width || p->height != s->texture.height || p->pitch < p->width * 4) {
    p->status = 1; return STATUS_SUCCESS;
  }
  @autoreleasepool {
    NSUInteger row = p->width * 4, bytes = row * p->height;
    id<MTLBuffer> buf = [d->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmd = hb_cmd(d);
    id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
    [b copyFromTexture:s->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(p->width, p->height, 1) toBuffer:buf destinationOffset:0
       destinationBytesPerRow:row destinationBytesPerImage:bytes];
    [b endEncoding];
    hb_commit(d, 1);
    const uint8_t *src = [buf contents];
    for (uint32_t y = 0; y < p->height; y++) memcpy(dst + (size_t)y * p->pitch, src + (size_t)y * row, row);
    [buf release];
  }
  p->status = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS hb_release(void *args) {
  struct hbdx9_release_params *p = args;
  @autoreleasepool {
    if (p->kind == 1) {
      struct hb_swapchain *sc = (struct hb_swapchain *)(uintptr_t)p->object;
      if (sc) {
        hb_commit(sc->dev, 1);
        [sc->backbuffer->texture release]; free(sc->backbuffer);
        [sc->layer release];
        struct macdrv_functions_t *mf = dlsym(RTLD_DEFAULT, "macdrv_functions");
        if (mf && mf->macdrv_view_release_metal_view && sc->view) mf->macdrv_view_release_metal_view(sc->view);
        free(sc);
      }
    }
  }
  p->status = 0;
  return STATUS_SUCCESS;
}

typedef NTSTATUS (*unixlib_entry_t)(void *args);
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[] = {
    hb_init, hb_swapchain_create, hb_submit, hb_readback, hb_release,
};
__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_wow64_funcs[] = {
    hb_init, hb_swapchain_create, hb_submit, hb_readback, hb_release,
};
_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == HBDX9_CALL_COUNT, "table");
