/* GPU-PASS mode 3. Byte comparison of the attachments of ONE tess draw, with identical pre-draw contents.
 * No game resource leaves the GPU. Only counters/first coordinates return to the CPU. This changes pass scheduling and
 * must never be used for FPS acceptance. Unsupported attachments produce NOT_ENABLED, never a zero-difference claim.
 * Included after the render command interpreter. No MSC/D3DMetal internals are used. */
static char tpv_pool_key;
static id<MTLComputePipelineState> tpv_compare_pso;
static pthread_mutex_t tpv_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tpv_draws, tpv_bad, tpv_pixels, tpv_diffs;

@interface TPVPool : NSObject {
@public
  NSMutableDictionary *textures;
  NSMutableArray *retired;
  id<MTLBuffer> a, b, visibility;
  NSUInteger capacity, bytes;
}
@end
@implementation TPVPool
- (id)init { if ((self = [super init])) { textures = [[NSMutableDictionary alloc] init]; retired = [[NSMutableArray alloc] init]; } return self; }
- (void)dealloc { [textures release]; [retired release]; [a release]; [b release]; [visibility release]; [super dealloc]; }
@end

@interface TPVDraw : NSObject {
@public
  TPVPool *pool;
  struct WMTRenderPassInfo real, shadow;
  id<MTLBuffer> result;
  id<MTLBuffer> records;
  uint64_t pso;
  unsigned count, slot[10], width[10], height[10], bpp[10], row[10];
  id<MTLTexture> original[10], duplicate[10];
  unsigned level[10], slice[10];
  MTLBlitOption option[10];
}
@end
@implementation TPVDraw
- (void)dealloc { [records release]; [result release]; [pool release]; [super dealloc]; }
@end

static unsigned tpv_bpp(MTLPixelFormat f, unsigned slot) {
  if (slot == 8) {
    if (f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8) return 4;
    if (f == MTLPixelFormatDepth16Unorm) return 2;
    return 0;
  }
  if (slot == 9) return (f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8 ||
                         f == MTLPixelFormatX32_Stencil8) ? 1 : 0;
  switch (f) {
  case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Uint: case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Sint: return 1;
  case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
  case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Float:
  case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: return 2;
  case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm:
  case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
  case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
  case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG11B10Float:
  case MTLPixelFormatR32Float: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
  case MTLPixelFormatRG16Float: case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm:
  case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: return 4;
  case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm:
  case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
  case MTLPixelFormatRG32Float: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: return 8;
  case MTLPixelFormatRGBA32Float: case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: return 16;
  default: return 0;
  }
}

static BOOL tpv_pipeline(id<MTLDevice> device) {
  pthread_mutex_lock(&tpv_lock);
  if (!tpv_compare_pso) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void tpv(device const uchar *a [[buffer(0)]], device const uchar *b [[buffer(1)]],"
        "device atomic_uint *r [[buffer(2)]], constant uint4 &c [[buffer(3)]], uint2 p [[thread_position_in_grid]]) {"
        "if(p.x>=c.x || p.y>=c.y)return; uint off=p.y*c.w+p.x*c.z; bool diff=false,nz=false;"
        "for(uint i=0;i<c.z;++i){diff|=a[off+i]!=b[off+i];nz|=a[off+i]!=0;}"
        "atomic_fetch_add_explicit(r+1,1u,memory_order_relaxed);"
        "if(nz)atomic_fetch_add_explicit(r+3,1u,memory_order_relaxed);"
        "if(diff){atomic_fetch_add_explicit(r,1u,memory_order_relaxed);"
        "atomic_fetch_min_explicit(r+2,p.y*c.x+p.x,memory_order_relaxed);}}";
    NSError *error = nil;
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init]; opts.fastMathEnabled = NO;
    id<MTLLibrary> lib = [device newLibraryWithSource:source options:opts error:&error]; [opts release];
    id<MTLFunction> fn = [lib newFunctionWithName:@"tpv"];
    if (fn) tpv_compare_pso = [device newComputePipelineStateWithFunction:fn error:&error];
    if (!tpv_compare_pso) fprintf(stderr, "dx12_tess_verify status=FAILED reason=compare_pipeline error=%s\n",
                                 error ? [[error localizedDescription] UTF8String] : "missing_function");
    [fn release]; [lib release];
  }
  BOOL ok = tpv_compare_pso != nil;
  pthread_mutex_unlock(&tpv_lock);
  return ok;
}

static BOOL tpv_attachment(TPVDraw *v, id<MTLDevice> device, unsigned slot, obj_handle_t handle,
                           unsigned level, unsigned slice, unsigned plane, obj_handle_t *copy) {
  if (!handle) return YES;
  id<MTLTexture> tex = (id<MTLTexture>)handle;
  unsigned bpp = tpv_bpp(tex.pixelFormat, slot);
  if (!bpp || tex.sampleCount != 1 || plane ||
      (tex.textureType != MTLTextureType2D && tex.textureType != MTLTextureType2DArray) ||
      level >= tex.mipmapLevelCount || slice >= tex.arrayLength || tex.framebufferOnly) {
    fprintf(stderr, "dx12_tess_verify status=NOT_ENABLED pso=%llu slot=%u reason=attachment format=%lu type=%lu samples=%lu\n",
            (unsigned long long)v->pso, slot, (unsigned long)tex.pixelFormat, (unsigned long)tex.textureType,
            (unsigned long)tex.sampleCount);
    return NO;
  }
  unsigned w = MAX(1, tex.width >> level), h = MAX(1, tex.height >> level);
  // Depth and stencil of the same packed view must share one shadow texture. All attachment layers start at zero there.
  unsigned keyslot = slot == 9 && handle == v->real.depth.texture ? 8 : slot;
  NSString *key = [NSString stringWithFormat:@"%u:%lu:%u:%u", keyslot, (unsigned long)tex.pixelFormat, w, h];
  id<MTLTexture> dup = [v->pool->textures objectForKey:key];
  if (!dup) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex.pixelFormat width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
    NSUInteger bytes = [device heapTextureSizeAndAlignWithDescriptor:td].size;
    if (v->pool->bytes + bytes > 128u * 1024 * 1024) return NO;
    dup = [device newTextureWithDescriptor:td];
    if (!dup) return NO;
    [v->pool->textures setObject:dup forKey:key]; [dup release]; v->pool->bytes += bytes;
  }
  unsigned i = v->count++;
  v->slot[i] = slot; v->width[i] = w; v->height[i] = h; v->bpp[i] = bpp;
  v->row[i] = (w * bpp + 255) & ~255u;
  v->original[i] = tex; v->duplicate[i] = dup; v->level[i] = level; v->slice[i] = slice;
  v->option[i] = MTLBlitOptionNone;
  if (tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 || tex.pixelFormat == MTLPixelFormatX32_Stencil8)
    v->option[i] = slot == 8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionStencilFromDepthStencil;
  *copy = (obj_handle_t)dup;
  return YES;
}

static NTSTATUS _MTLCommandBuffer_tessVerifyBegin(void *obj) {
  struct unixcall_tess_verify_begin *p = obj;
  p->ret = 0;
  id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)p->cmdbuf;
  id<MTLFence> fence = (id<MTLFence>)p->fence;
  struct WMTRenderPassInfo *info = p->info.ptr;
  TPVDraw *v = [[TPVDraw alloc] init]; v->real = *info; v->shadow = *info; v->pso = p->pso;
  for (const struct wmtcmd_base *c=p->head.ptr; c && c!=p->end.ptr; c=c->next.ptr) {
    if (c->type==WMTRenderCommandSetMeshBuffer) {
      const struct wmtcmd_render_setbuffer *b=(const void *)c;
      if (b->index==26 && !b->offset) v->records=[(id<MTLBuffer>)b->buffer retain];
    }
  }
  TPVPool *pool = objc_getAssociatedObject(cb, &tpv_pool_key);
  if (!pool) {
    pool = [[TPVPool alloc] init]; objc_setAssociatedObject(cb, &tpv_pool_key, pool, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [pool release];
  }
  v->pool = [pool retain];
  BOOL ok = info->render_target_array_length <= 1 && !info->tile_width && !info->tile_height && tpv_pipeline(cb.device);
  for (unsigned i = 0; i < 8 && ok; ++i) {
    struct WMTColorAttachmentInfo *a = &v->real.colors[i], *b = &v->shadow.colors[i];
    if (!a->texture) continue;
    ok = !a->resolve_texture && tpv_attachment(v, cb.device, i, a->texture, a->level, a->slice, a->depth_plane, &b->texture);
    if (a->load_action == WMTLoadActionDontCare) a->load_action = WMTLoadActionClear;
    a->store_action = WMTStoreActionStore; b->load_action = a->load_action; b->store_action = a->store_action;
    b->level = b->slice = b->depth_plane = 0;
  }
  if (ok && info->depth.texture) {
    struct WMTDepthAttachmentInfo *a = &v->real.depth, *b = &v->shadow.depth;
    ok = tpv_attachment(v, cb.device, 8, a->texture, a->level, a->slice, a->depth_plane, &b->texture);
    if (a->load_action == WMTLoadActionDontCare) a->load_action = WMTLoadActionClear;
    a->store_action = WMTStoreActionStore; b->load_action = a->load_action; b->store_action = a->store_action;
    b->level = b->slice = b->depth_plane = 0;
  }
  if (ok && info->stencil.texture) {
    struct WMTStencilAttachmentInfo *a = &v->real.stencil, *b = &v->shadow.stencil;
    ok = tpv_attachment(v, cb.device, 9, a->texture, a->level, a->slice, a->depth_plane, &b->texture);
    if (a->load_action == WMTLoadActionDontCare) a->load_action = WMTLoadActionClear;
    a->store_action = WMTStoreActionStore; b->load_action = a->load_action; b->store_action = a->store_action;
    b->level = b->slice = b->depth_plane = 0;
  }
  NSUInteger need = 0;
  for (unsigned i = 0; i < v->count; ++i) need = MAX(need, (NSUInteger)v->row[i] * v->height[i]);
  if (ok && need && need <= 32u * 1024 * 1024) {
    if (need > pool->capacity) {
      // Keep replaced allocations until CB completion, including with unretained-reference command buffers.
      if (pool->a) { [pool->retired addObject:pool->a]; [pool->retired addObject:pool->b]; pool->bytes += 2*pool->capacity; }
      [pool->a release]; [pool->b release];
      pool->a = [cb.device newBufferWithLength:need options:MTLResourceStorageModePrivate];
      pool->b = [cb.device newBufferWithLength:need options:MTLResourceStorageModePrivate]; pool->capacity = need;
    }
    v->result = [cb.device newBufferWithLength:160 options:MTLResourceStorageModeShared];
    ok = pool->a && pool->b && v->result;
  } else ok = NO;
  if (ok && info->visibility_buffer) {
    NSUInteger size = [(id<MTLBuffer>)info->visibility_buffer length];
    if (!pool->visibility || pool->visibility.length < size) {
      if (pool->visibility) { [pool->retired addObject:pool->visibility]; pool->bytes += pool->visibility.length; }
      [pool->visibility release]; pool->visibility = [cb.device newBufferWithLength:size options:MTLResourceStorageModePrivate];
    }
    ok = pool->visibility != nil; v->shadow.visibility_buffer = (obj_handle_t)pool->visibility;
  }
  if (!ok) {
    fprintf(stderr, "dx12_tess_verify status=FAILED pso=%llu reason=prepare_or_budget attachments=%u\n",
            (unsigned long long)p->pso, v->count);
    [v release]; return STATUS_SUCCESS;
  }
  uint32_t *words = v->result.contents; memset(words, 0, 160);
  for (unsigned i = 0; i < 10; ++i) words[4*i+2] = UINT32_MAX;
  id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder]; [blit waitForFence:fence];
  // Copy every attachment, including both depth/stencil planes, before either path renders. Clear actions then match too.
  for (unsigned i = 0; i < v->count; ++i) {
    if (i && v->slot[i] == 9 && v->duplicate[i] == v->duplicate[i-1]) continue;
    [blit copyFromTexture:v->original[i] sourceSlice:v->slice[i] sourceLevel:v->level[i]
        sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(v->width[i],v->height[i],1)
        toTexture:v->duplicate[i] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
  }
  [blit updateFence:fence]; [blit endEncoding];
  struct unixcall_generic_obj_uint64_obj_ret rp = {p->cmdbuf, (uint64_t)&v->shadow, 0};
  _MTLCommandBuffer_renderCommandEncoder(&rp);
  id<MTLRenderCommandEncoder> re = (id<MTLRenderCommandEncoder>)rp.ret;
  if (!re) { fprintf(stderr, "dx12_tess_verify status=FAILED pso=%llu reason=shadow_encoder\n", (unsigned long long)p->pso);
    [v release]; return STATUS_SUCCESS; }
  [re waitForFence:fence beforeStages:MTLRenderStageVertex | MTLRenderStageObject | MTLRenderStageMesh];
  struct unixcall_generic_obj_cmd_noret cmd = {0}; cmd.encoder = rp.ret; cmd.cmd_head = p->head;
  winemetal_render_encode_range(&cmd, p->end.ptr);
  cmd.cmd_head = p->mesh_draw; winemetal_render_encode_range(&cmd, NULL);
  [re updateFence:fence afterStages:MTLRenderStageFragment];
  struct unixcall_generic_obj_noret end = {rp.ret}; _MTLCommandEncoder_endEncoding(&end);
  *info = v->real;
  p->ret = (obj_handle_t)v;
  return STATUS_SUCCESS;
}

static NTSTATUS _MTLCommandBuffer_tessVerifyEnd(void *obj) {
  struct unixcall_tess_verify_end *p = obj;
  id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)p->cmdbuf;
  id<MTLFence> fence = (id<MTLFence>)p->fence;
  TPVDraw *v = (TPVDraw *)p->verifier;
  for (unsigned i = 0; i < v->count; ++i) {
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder]; [blit waitForFence:fence];
    [blit copyFromTexture:v->duplicate[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(v->width[i],v->height[i],1) toBuffer:v->pool->a destinationOffset:0
        destinationBytesPerRow:v->row[i] destinationBytesPerImage:v->row[i]*v->height[i] options:v->option[i]];
    [blit copyFromTexture:v->original[i] sourceSlice:v->slice[i] sourceLevel:v->level[i] sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(v->width[i],v->height[i],1) toBuffer:v->pool->b destinationOffset:0
        destinationBytesPerRow:v->row[i] destinationBytesPerImage:v->row[i]*v->height[i] options:v->option[i]];
    [blit updateFence:fence]; [blit endEncoding];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce waitForFence:fence];
    [ce setComputePipelineState:tpv_compare_pso];
    [ce setBuffer:v->pool->a offset:0 atIndex:0]; [ce setBuffer:v->pool->b offset:0 atIndex:1];
    [ce setBuffer:v->result offset:i*16 atIndex:2];
    const uint32_t caps[4] = {v->width[i],v->height[i],v->bpp[i],v->row[i]}; [ce setBytes:caps length:16 atIndex:3];
    [ce dispatchThreads:MTLSizeMake(v->width[i],v->height[i],1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
    [ce updateFence:fence]; [ce endEncoding];
  }
  [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
    const uint32_t *r = v->result.contents;
    uint64_t diff = 0, pixels = 0; BOOL valid = done.status == MTLCommandBufferStatusCompleted;
    for (unsigned i = 0; i < v->count; ++i) { diff += r[4*i]; pixels += r[4*i+1];
      valid &= r[4*i+1] == v->width[i]*v->height[i]; }
    pthread_mutex_lock(&tpv_lock);
    if (v->records && v->records.length>=128) {
      const uint32_t *d=v->records.contents;
      if (d && d[0] && d[7]==0x54524431) fprintf(stderr,
          "dx12_tess_records status=%s pso=%llu mesh_vertices=%u pull_vertices=%u missing_mesh=%u key_diff=%u "
          "cp_diff_lanes=%u pc_diff_lanes=%u position_diff_lanes=%u data_diff_lanes=%u clip_diff_lanes=%u "
          "mesh_over_cap=%u pull_over_cap=%u compared_output_lanes=%u compared_input_lanes=%u bytes=%lu "
          "position_ulp_max=%u data_ulp_max=%u position_ulp_le4=%u data_ulp_le4=%u "
          "position_ulp_gt4=%u data_ulp_gt4=%u nonfinite_diff=%u position_abs_le_1em5=%u data_abs_le_1em5=%u\n",
          !valid?"FAILED":d[9]&&!d[20]?"NOT_ENABLED":"PRESENT",(unsigned long long)v->pso,d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15],d[16],
          d[17],d[18],d[20],d[21],(unsigned long)v->records.length,
          d[22],d[23],d[24],d[25],d[26],d[27],d[28],d[30],d[31]);
    }
    ++tpv_draws; tpv_bad += !valid || diff != 0; tpv_pixels += pixels; tpv_diffs += diff;
    // One compact receipt per draw. The bounded diagnostic launcher bounds the log; no silent skipped samples.
    {
      fprintf(stderr, "dx12_tess_verify status=%s pso=%llu draws=%llu bad_draws=%llu attachment_pixels=%llu diff_pixels=%llu "
              "total_pixels=%llu total_diff=%llu observer_pool_bytes=%lu\n", valid ? "PRESENT" : "FAILED",
              (unsigned long long)v->pso, (unsigned long long)tpv_draws, (unsigned long long)tpv_bad,
              (unsigned long long)pixels, (unsigned long long)diff, (unsigned long long)tpv_pixels,
              (unsigned long long)tpv_diffs, (unsigned long)(v->pool->bytes + 2*v->pool->capacity));
      for (unsigned i=0;i<v->count;++i) if (r[4*i]) fprintf(stderr,
          "dx12_tess_verify_first pso=%llu slot=%u different=%u x=%u y=%u width=%u height=%u bpp=%u\n",
          (unsigned long long)v->pso, v->slot[i], r[4*i], r[4*i+2]%v->width[i], r[4*i+2]/v->width[i],
          v->width[i],v->height[i],v->bpp[i]);
    }
    pthread_mutex_unlock(&tpv_lock);
  }];
  return STATUS_SUCCESS;
}
