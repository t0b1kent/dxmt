/* GPU-PASS 24.10.2026 (MacRunner diagnostic, OFF by default): GPU time of every Metal encoder of a few frames, without a
 * Metal capture. Stage-boundary counter samples (MTLCounterSamplingPointAtStageBoundary): render encoders get
 * start/end of the vertex stage and start/end of the fragment stage, compute and blit encoders start/end of the encoder.
 *   MACRUNNER_DX12_PASS_TIME=N           a window every N presents (unset/0 = off: one predictable branch per call)
 *   MACRUNNER_DX12_PASS_TIME_FILE=/abs   output, created exclusively (O_EXCL); JSON lines
 *   MACRUNNER_DX12_PASS_TIME_CONTROL=C   frames per window recorded WITHOUT samples (control: GPU times of the command
 *                                        buffers only), default 3
 *   MACRUNNER_DX12_PASS_TIME_SAMPLED=S   frames per window WITH samples, default 5
 *   MACRUNNER_DX12_PASS_TIME_SKIP=K      presents before the first window, default 0
 * A frame = command buffers committed after one present-commit up to and including the next. Command buffers are
 * marked at creation (control/sampled); a frame is analysable only if every command buffer committed in it carries a
 * mark of one kind (the window footer counts unmarked commits per frame). Nothing is added to the GPU work: no command
 * buffers, encoders, fences, waits or readbacks; samples go into a fixed pool of 4 counter buffers created once.
 * The program cannot observe it (no state it reads changes). Output is formatted in completion handlers and written
 * from a utility queue. Printing for humans goes to stderr. */
#include <mach/mach_time.h>
#include <sys/types.h>

enum { PTPoolBuffers = 4, PTSamplesPerBuffer = 4096, PTMaxFrames = 48, PTPresentRing = 16 };
static const uint64_t PTMaxFileBytes = 512ull << 20;

typedef struct {
  uint32_t sample;               /* first sample index in pool buffer `pool`, UINT32_MAX = not sampled */
  uint8_t pool, kind, ncolor, flags; /* kind 'r' render, 'c' compute, 'b' blit, 'q' blit carrying the program's samples */
  uint32_t w, h, dw, dh, rsamples, layers;
  uint16_t cfmt[8];
  uint16_t dfmt, sfmt;
  uint32_t la, sa;               /* load/store action per slot (0-7 colours, 8 depth, 9 stencil), 3 bits each */
  uint64_t c0, d0;               /* trace-local identity of colour-0 and depth textures (pointer values) */
  uint32_t draws, draws_idx, draws_ind, draws_other, waits, updates;
  uint64_t verts, idx;           /* vertex_count x instances (Draw), index_count x instances (DrawIndexed) */
  uint32_t disp, disp_threads, disp_ind;
  uint64_t groups, threads;
  uint32_t ops;                  /* blit commands */
  /* schema 2 (GPU-PASS 24.10): the 'other' draws split, ICB ranges, memory barriers inside the encoder */
  uint32_t icb, icb_ind, mesh, geom, tess, tile, barriers;
  uint64_t icb_len;              /* sum of CPU range lengths of ExecuteCommandsInBuffer (upper bound when icb_ind) */
} PTEnc;

@interface PTRecord : NSObject {
@public
  uint64_t seq, commit, window;
  uint32_t frame, n, cap, dropped, status;
  uint8_t mode;                  /* 'c' control, 's' sampled */
  BOOL committed, present;
  PTEnc *enc;
  uint32_t lo[PTPoolBuffers], hi[PTPoolBuffers]; /* sampled index range per pool buffer */
  uint64_t *ts;                  /* 4 per encoder, resolved at completion */
  double gs, ge;
}
@end
@interface PTEncRef : NSObject { @public PTRecord *record; uint32_t index; }
@end

static pthread_once_t pt_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t pt_lock = PTHREAD_MUTEX_INITIALIZER;
static bool pt_on;
static int pt_fd = -1;
static unsigned pt_every, pt_ctrl = 3, pt_samp = 5, pt_skip;
static uint64_t pt_presents, pt_commits, pt_seq, pt_window, pt_written;
static _Atomic int pt_state;     /* 0 idle, 1 control, 2 sampled, 3 closed (draining) */
static _Atomic long pt_live;     /* PTRecord objects alive: zero = skip every lookup */
static uint64_t pt_arm, pt_ctrl_end, pt_samp_end;
static uint32_t pt_created, pt_done, pt_dead;
static uint32_t pt_untracked[PTMaxFrames], pt_tracked_c[PTMaxFrames], pt_tracked_s[PTMaxFrames];
static NSMutableData *pt_text;
static id<MTLCounterSampleBuffer> pt_pool[PTPoolBuffers];
static uint32_t pt_pool_used[PTPoolBuffers];
static unsigned pt_pool_cur;
static uint64_t pt_cpu0, pt_gpu0;
static uint64_t pt_host0;
static id pt_present_ring[PTPresentRing];
static unsigned pt_present_next;
static NSMutableArray *pt_tainted;
static dispatch_queue_t pt_queue;
static char pt_cb_key, pt_enc_key;
static void PTFinishLocked(bool forced);

static uint64_t PTHostNS(void) {
  static mach_timebase_info_data_t tb;
  if (!tb.denom) mach_timebase_info(&tb);
  return (uint64_t)((__uint128_t)mach_absolute_time() * tb.numer / tb.denom);
}
static unsigned PTOption(const char *name, unsigned fallback, unsigned cap) {
  const char *s = getenv(name);
  if (!s || !*s) return fallback;
  char *end; unsigned long v = strtoul(s, &end, 10);
  return *end || v > cap ? fallback : (unsigned)v;
}
/* Only from pt_queue (or PTInit before the queue exists). */
static void PTWriteRaw(const void *bytes, size_t len) {
  if (pt_fd < 0 || pt_written + len > PTMaxFileBytes) return;
  const char *p = bytes; size_t left = len;
  while (left) { ssize_t k = write(pt_fd, p, left); if (k <= 0) { if (errno == EINTR) continue; break; } p += k; left -= (size_t)k; }
  pt_written += len;
}
static void PTInit(void) {
  pt_every = PTOption("MACRUNNER_DX12_PASS_TIME", 0, 1000000);
  if (!pt_every) return;
  const char *path = getenv("MACRUNNER_DX12_PASS_TIME_FILE");
  if (!path || path[0] != '/') { fprintf(stderr, "macrunner-pass-time: status=OFF reason=no_absolute_file\n"); return; }
  pt_fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (pt_fd < 0) { fprintf(stderr, "macrunner-pass-time: status=OFF reason=file_exists_or_unwritable errno=%d\n", errno); return; }
  pt_ctrl = PTOption("MACRUNNER_DX12_PASS_TIME_CONTROL", 3, PTMaxFrames / 2);
  pt_samp = PTOption("MACRUNNER_DX12_PASS_TIME_SAMPLED", 5, PTMaxFrames / 2);
  pt_skip = PTOption("MACRUNNER_DX12_PASS_TIME_SKIP", 0, 100000000);
  pt_queue = dispatch_queue_create("macrunner.pass-time", DISPATCH_QUEUE_SERIAL);
  pt_tainted = [[NSMutableArray alloc] init];
  pt_on = true;
  char line[512];
  int k = snprintf(line, sizeof line,
      "{\"t\":\"config\",\"schema\":2,\"every\":%u,\"control\":%u,\"sampled\":%u,\"skip\":%u,\"pool\":%u,\"per_buffer\":%u,"
      "\"enc\":[\"t\",\"w\",\"seq\",\"i\",\"k\",\"ts0\",\"ts1\",\"ts2\",\"ts3\",\"w\",\"h\",\"ncolor\",\"cfmt\",\"dfmt\",\"sfmt\",\"dw\",\"dh\","
      "\"rs\",\"layers\",\"la\",\"sa\",\"c0\",\"d0\",\"draws\",\"draws_idx\",\"draws_ind\",\"draws_other\",\"verts\",\"idx\",\"disp\",\"disp_threads\","
      "\"disp_ind\",\"groups\",\"threads\",\"ops\",\"waits\",\"updates\",\"icb\",\"icb_ind\",\"icb_len\",\"mesh\",\"geom\",\"tess\",\"tile\",\"barriers\"]}\n",
      pt_every, pt_ctrl, pt_samp, pt_skip, PTPoolBuffers, PTSamplesPerBuffer);
  PTWriteRaw(line, (size_t)k);
  fprintf(stderr, "macrunner-pass-time: status=ON every=%u control=%u sampled=%u skip=%u\n", pt_every, pt_ctrl, pt_samp, pt_skip);
}
static inline bool PTEnabled(void) { pthread_once(&pt_once, PTInit); return pt_on; }

@implementation PTRecord
- (void)dealloc {
  pthread_mutex_lock(&pt_lock);
  if (!committed && window == pt_window && atomic_load(&pt_state) != 0) { ++pt_dead; PTFinishLocked(false); }
  pthread_mutex_unlock(&pt_lock);
  free(enc); free(ts); atomic_fetch_sub(&pt_live, 1); [super dealloc];
}
@end
@implementation PTEncRef
- (void)dealloc { [record release]; [super dealloc]; }
@end

/* Caller holds pt_lock. Pool buffers are created once (device from the first present), on the commit thread. */
static bool PTPoolReadyLocked(id<MTLDevice> device) {
  if (pt_pool[0]) return true;
  static bool reported;
  if (!reported) {
    reported = true;
    fprintf(stderr, "macrunner-pass-time: sampling stage=%d draw=%d dispatch=%d blit=%d tile=%d\n",
            (int)[device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary],
            (int)[device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary],
            (int)[device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary],
            (int)[device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary],
            (int)[device supportsCounterSampling:MTLCounterSamplingPointAtTileDispatchBoundary]);
    char line[256];
    int k = snprintf(line, sizeof line, "{\"t\":\"support\",\"stage\":%d,\"draw\":%d,\"dispatch\":%d,\"blit\":%d,\"tile\":%d}\n",
        (int)[device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary],
        (int)[device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary],
        (int)[device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary],
        (int)[device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary],
        (int)[device supportsCounterSampling:MTLCounterSamplingPointAtTileDispatchBoundary]);
    NSData *copy = [[NSData alloc] initWithBytes:line length:(NSUInteger)k];
    dispatch_async(pt_queue, ^{ PTWriteRaw(copy.bytes, copy.length); [copy release]; });
  }
  if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) return false;
  id<MTLCounterSet> set = nil;
  for (id<MTLCounterSet> candidate in device.counterSets)
    if ([candidate.name isEqualToString:MTLCommonCounterSetTimestamp]) { set = candidate; break; }
  if (!set) return false;
  for (unsigned i = 0; i < PTPoolBuffers; ++i) {
    MTLCounterSampleBufferDescriptor *desc = [[MTLCounterSampleBufferDescriptor alloc] init];
    desc.counterSet = set; desc.sampleCount = PTSamplesPerBuffer; desc.storageMode = MTLStorageModeShared;
    NSError *error = nil;
    pt_pool[i] = [device newCounterSampleBufferWithDescriptor:desc error:&error];
    [desc release];
    if (!pt_pool[i]) {
      fprintf(stderr, "macrunner-pass-time: status=FAILED reason=counter_buffer index=%u\n", i);
      for (unsigned j = 0; j < i; ++j) { [pt_pool[j] release]; pt_pool[j] = nil; }
      return false;
    }
  }
  return true;
}

/* Caller holds pt_lock. Finishes the window when it is closed and every marked command buffer completed (or died
 * uncommitted). forced = deadline: some never completed; the pool is retired (tainted) instead of reused. */
static void PTFinishLocked(bool forced) {
  if (atomic_load(&pt_state) != 3) return;
  if (!forced && pt_done + pt_dead < pt_created) return;
  NSMutableString *f = [NSMutableString stringWithFormat:
      @"{\"t\":\"window_end\",\"w\":%llu,\"arm\":%llu,\"ctrl_end\":%llu,\"samp_end\":%llu,\"created\":%u,\"done\":%u,\"dead\":%u,\"forced\":%s,"
      "\"pool_used\":[%u,%u,%u,%u],\"host_end_ns\":%llu,\"untracked\":[",
      pt_window, pt_arm, pt_ctrl_end, pt_samp_end, pt_created, pt_done, pt_dead, forced ? "true" : "false",
      pt_pool_used[0], pt_pool_used[1], pt_pool_used[2], pt_pool_used[3], PTHostNS()];
  for (unsigned i = 0; i < PTMaxFrames; ++i) [f appendFormat:@"%s%u", i ? "," : "", pt_untracked[i]];
  [f appendString:@"],\"tracked_c\":["];
  for (unsigned i = 0; i < PTMaxFrames; ++i) [f appendFormat:@"%s%u", i ? "," : "", pt_tracked_c[i]];
  [f appendString:@"],\"tracked_s\":["];
  for (unsigned i = 0; i < PTMaxFrames; ++i) [f appendFormat:@"%s%u", i ? "," : "", pt_tracked_s[i]];
  [f appendString:@"]}\n"];
  [pt_text appendData:[f dataUsingEncoding:NSUTF8StringEncoding]];
  NSMutableData *text = pt_text; pt_text = nil;
  if (forced) {
    for (unsigned i = 0; i < PTPoolBuffers; ++i) { if (pt_pool[i]) [pt_tainted addObject:pt_pool[i]]; [pt_pool[i] release]; pt_pool[i] = nil; }
  }
  atomic_store(&pt_state, 0);
  dispatch_async(pt_queue, ^{ PTWriteRaw(text.bytes, text.length); [text release]; });
}

/* At command buffer creation (any thread). */
static void PTOnCreate(id<MTLCommandBuffer> cb) {
  if (!PTEnabled() || !cb) return;
  int state = atomic_load(&pt_state);
  if (state != 1 && state != 2) return;
  PTRecord *r = [[PTRecord alloc] init];
  atomic_fetch_add(&pt_live, 1);
  pthread_mutex_lock(&pt_lock);
  state = atomic_load(&pt_state);
  if (state == 1 || state == 2) {
    r->seq = ++pt_seq; r->window = pt_window; r->mode = state == 2 ? 's' : 'c'; ++pt_created;
    for (unsigned i = 0; i < PTPoolBuffers; ++i) { r->lo[i] = UINT32_MAX; r->hi[i] = 0; }
    pthread_mutex_unlock(&pt_lock);
    objc_setAssociatedObject(cb, &pt_cb_key, r, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  } else {
    r->window = UINT64_MAX; pthread_mutex_unlock(&pt_lock);
  }
  [r release];
}

static PTRecord *PTRecordOf(id<MTLCommandBuffer> cb) {
  if (!pt_on || !atomic_load(&pt_live)) return nil;
  return objc_getAssociatedObject(cb, &pt_cb_key);
}

static uint32_t PTActions(MTLRenderPassDescriptor *d, bool store) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 10; ++i) {
    MTLRenderPassAttachmentDescriptor *a = i < 8 ? d.colorAttachments[i] : i == 8 ? (id)d.depthAttachment : (id)d.stencilAttachment;
    if (!a.texture) continue;
    v |= (uint32_t)((store ? a.storeAction : a.loadAction) & 7) << (3 * i);
  }
  return v;
}

/* Before an encoder is created on cb: append its record and, in a sampled buffer, attach pool samples to the
 * descriptor (render: MTLRenderPassDescriptor, compute: MTLComputePassDescriptor, blit: MTLBlitPassDescriptor). */
static PTEncRef *PTBegin(id<MTLCommandBuffer> cb, char kind, id descriptor) {
  PTRecord *r = PTRecordOf(cb);
  if (!r || r->committed) return nil;
  if (r->n == r->cap) {
    uint32_t cap = r->cap ? r->cap * 2 : 16;
    PTEnc *grown = realloc(r->enc, cap * sizeof(PTEnc));
    if (!grown) { ++r->dropped; return nil; }
    r->enc = grown; r->cap = cap;
  }
  PTEnc *e = &r->enc[r->n]; memset(e, 0, sizeof *e);
  e->kind = (uint8_t)kind; e->sample = UINT32_MAX;
  if (kind == 'r') {
    MTLRenderPassDescriptor *d = descriptor;
    for (unsigned i = 0; i < 8; ++i) {
      id<MTLTexture> t = d.colorAttachments[i].texture;
      if (!t) continue;
      e->cfmt[i] = (uint16_t)t.pixelFormat; ++e->ncolor;
      if (!e->c0) {
        unsigned lv = (unsigned)d.colorAttachments[i].level;
        e->c0 = (uint64_t)t; e->w = (uint32_t)MAX(1, t.width >> lv); e->h = (uint32_t)MAX(1, t.height >> lv);
      }
    }
    id<MTLTexture> dt = d.depthAttachment.texture, st = d.stencilAttachment.texture;
    if (dt) {
      unsigned lv = (unsigned)d.depthAttachment.level;
      e->dfmt = (uint16_t)dt.pixelFormat; e->dw = (uint32_t)MAX(1, dt.width >> lv); e->dh = (uint32_t)MAX(1, dt.height >> lv); e->d0 = (uint64_t)dt;
    }
    if (st) e->sfmt = (uint16_t)st.pixelFormat;
    if (d.renderTargetWidth) { e->w = (uint32_t)d.renderTargetWidth; e->h = (uint32_t)d.renderTargetHeight; }
    else if (!e->w && dt) { e->w = e->dw; e->h = e->dh; }
    e->rsamples = (uint32_t)d.defaultRasterSampleCount; e->layers = (uint32_t)d.renderTargetArrayLength;
    e->la = PTActions(d, false); e->sa = PTActions(d, true);
  }
  if (r->mode == 's' && kind != 'q') {
    unsigned need = kind == 'r' ? 4 : 2;
    pthread_mutex_lock(&pt_lock);
    while (pt_pool_cur < PTPoolBuffers && pt_pool_used[pt_pool_cur] + need > PTSamplesPerBuffer) ++pt_pool_cur;
    if (pt_pool_cur < PTPoolBuffers && pt_pool[pt_pool_cur] && r->window == pt_window) {
      e->pool = (uint8_t)pt_pool_cur; e->sample = pt_pool_used[pt_pool_cur]; pt_pool_used[pt_pool_cur] += need;
    }
    pthread_mutex_unlock(&pt_lock);
    if (e->sample != UINT32_MAX) {
      id<MTLCounterSampleBuffer> sb = pt_pool[e->pool];
      if (r->lo[e->pool] > e->sample) r->lo[e->pool] = e->sample;
      if (r->hi[e->pool] < e->sample + need) r->hi[e->pool] = e->sample + need;
      if (kind == 'r') {
        MTLRenderPassSampleBufferAttachmentDescriptor *a = ((MTLRenderPassDescriptor *)descriptor).sampleBufferAttachments[0];
        a.sampleBuffer = sb;
        a.startOfVertexSampleIndex = e->sample; a.endOfVertexSampleIndex = e->sample + 1;
        a.startOfFragmentSampleIndex = e->sample + 2; a.endOfFragmentSampleIndex = e->sample + 3;
      } else if (kind == 'c') {
        MTLComputePassSampleBufferAttachmentDescriptor *a = ((MTLComputePassDescriptor *)descriptor).sampleBufferAttachments[0];
        a.sampleBuffer = sb; a.startOfEncoderSampleIndex = e->sample; a.endOfEncoderSampleIndex = e->sample + 1;
      } else {
        MTLBlitPassSampleBufferAttachmentDescriptor *a = ((MTLBlitPassDescriptor *)descriptor).sampleBufferAttachments[0];
        a.sampleBuffer = sb; a.startOfEncoderSampleIndex = e->sample; a.endOfEncoderSampleIndex = e->sample + 1;
      }
    } else ++r->dropped;
  }
  PTEncRef *ref = [[PTEncRef alloc] init];
  ref->record = [r retain]; ref->index = r->n++;
  return [ref autorelease];
}
static bool PTSampled(PTEncRef *ref) { return ref && ref->record->enc[ref->index].sample != UINT32_MAX; }
static void PTAttach(id encoder, PTEncRef *ref) {
  if (encoder && ref) objc_setAssociatedObject(encoder, &pt_enc_key, ref, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static PTEnc *PTEncOf(id encoder) {
  if (!pt_on || !atomic_load(&pt_live)) return NULL;
  PTEncRef *ref = objc_getAssociatedObject(encoder, &pt_enc_key);
  return ref ? &ref->record->enc[ref->index] : NULL;
}

static void PTRenderCommand(PTEnc *e, const struct wmtcmd_base *c) {
  switch ((enum WMTRenderCommandType)c->type) {
  case WMTRenderCommandDraw: {
    const struct wmtcmd_render_draw *b = (const void *)c;
    ++e->draws; e->verts += b->vertex_count * (uint64_t)b->instance_count; break;
  }
  case WMTRenderCommandDrawIndexed: {
    const struct wmtcmd_render_draw_indexed *b = (const void *)c;
    ++e->draws_idx; e->idx += b->index_count * (uint64_t)b->instance_count; break;
  }
  case WMTRenderCommandDrawIndirect: case WMTRenderCommandDrawIndexedIndirect: ++e->draws_ind; break;
  case WMTRenderCommandDrawMeshThreadgroups: case WMTRenderCommandDrawMeshThreadgroupsIndirect: ++e->draws_other; ++e->mesh; break;
  case WMTRenderCommandDXMTGeometryDraw: case WMTRenderCommandDXMTGeometryDrawIndexed:
  case WMTRenderCommandDXMTGeometryDrawIndirect: case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: ++e->draws_other; ++e->geom; break;
  case WMTRenderCommandDXMTTessellationMeshDraw: case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
  case WMTRenderCommandDXMTTessellationMeshDrawIndirect: case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: ++e->draws_other; ++e->tess; break;
  case WMTRenderCommandDispatchThreadsPerTile: ++e->draws_other; ++e->tile; break;
  case WMTRenderCommandExecuteCommandsInBuffer: {
    const struct wmtcmd_render_executecommands *b = (const void *)c;
    ++e->draws_other; ++e->icb; e->icb_len += b->length; if (b->indirect_range_buffer) ++e->icb_ind; break;
  }
  case WMTRenderCommandMemoryBarrier: ++e->barriers; break;
  case WMTRenderCommandWaitForFence: ++e->waits; break;
  case WMTRenderCommandUpdateFence: ++e->updates; break;
  default: break;
  }
}
static void PTComputeCommand(PTEnc *e, const struct wmtcmd_base *c) {
  switch ((enum WMTComputeCommandType)c->type) {
  case WMTComputeCommandDispatch: {
    const struct wmtcmd_compute_dispatch *b = (const void *)c;
    ++e->disp; e->groups += (uint64_t)b->size.width * b->size.height * b->size.depth; break;
  }
  case WMTComputeCommandDispatchThreads: {
    const struct wmtcmd_compute_dispatch *b = (const void *)c;
    ++e->disp_threads; e->threads += (uint64_t)b->size.width * b->size.height * b->size.depth; break;
  }
  case WMTComputeCommandDispatchIndirect: ++e->disp_ind; break;
  case WMTComputeCommandMemoryBarrier: ++e->barriers; break;
  case WMTComputeCommandExecuteCommandsInBuffer: {
    const struct wmtcmd_compute_executecommands *b = (const void *)c;
    ++e->icb; e->icb_len += b->length; if (b->indirect_range_buffer) ++e->icb_ind; break;
  }
  case WMTComputeCommandWaitForFence: ++e->waits; break;
  case WMTComputeCommandUpdateFence: ++e->updates; break;
  default: break;
  }
}

/* Completion handler of a marked command buffer (Metal's thread): copy the GPU times and this buffer's own samples,
 * nothing else; formatting and the window bookkeeping run on the serial queue. */
static void PTFormat(PTRecord *r);
static void PTComplete(PTRecord *r, id<MTLCommandBuffer> cb) {
  r->gs = cb.GPUStartTime; r->ge = cb.GPUEndTime; r->status = (uint32_t)cb.status;
  bool ok = cb.status == MTLCommandBufferStatusCompleted && r->window == pt_window;
  if (r->n && r->mode == 's' && ok) {
    r->ts = malloc((size_t)r->n * 4 * sizeof(uint64_t));
    if (r->ts) memset(r->ts, 0xff, (size_t)r->n * 4 * sizeof(uint64_t));
    for (unsigned i = 0; i < PTPoolBuffers && r->ts; ++i) {
      id<MTLCounterSampleBuffer> sb = pt_pool[i];
      if (r->lo[i] >= r->hi[i] || !sb) continue;
      NSData *data = [sb resolveCounterRange:NSMakeRange(r->lo[i], r->hi[i] - r->lo[i])];
      if (!data || data.length != (r->hi[i] - r->lo[i]) * sizeof(MTLCounterResultTimestamp)) continue;
      const MTLCounterResultTimestamp *t = (const MTLCounterResultTimestamp *)data.bytes;
      for (uint32_t k = 0; k < r->n; ++k) {
        PTEnc *e = &r->enc[k];
        if (e->sample == UINT32_MAX || e->pool != i) continue;
        unsigned need = e->kind == 'r' ? 4 : 2;
        for (unsigned j = 0; j < need; ++j) r->ts[4 * k + j] = t[e->sample - r->lo[i] + j].timestamp;
      }
    }
  }
  [r retain];
  dispatch_async(pt_queue, ^{ PTFormat(r); [r release]; });
}
static void PTFormat(PTRecord *r) {
  NSMutableString *s = [NSMutableString stringWithFormat:
      @"{\"t\":\"cb\",\"w\":%llu,\"seq\":%llu,\"commit\":%llu,\"f\":%u,\"m\":\"%c\",\"p\":%d,\"gs\":%llu,\"ge\":%llu,\"st\":%u,\"n\":%u,\"drop\":%u}\n",
      r->window, r->seq, r->commit, r->frame, r->mode, (int)r->present, (unsigned long long)(r->gs * 1e9),
      (unsigned long long)(r->ge * 1e9), r->status, r->n, r->dropped];
  for (uint32_t i = 0; i < r->n; ++i) {
    PTEnc *e = &r->enc[i];
    const uint64_t none[4] = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    const uint64_t *ts = r->ts ? &r->ts[4 * i] : none;
    [s appendFormat:@"[\"e\",%llu,%llu,%u,\"%c\",%llu,%llu,%llu,%llu,%u,%u,%u,[%u,%u,%u,%u,%u,%u,%u,%u],%u,%u,%u,%u,%u,%u,%u,%u,"
        "\"%llx\",\"%llx\",%u,%u,%u,%u,%llu,%llu,%u,%u,%u,%llu,%llu,%u,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%u]\n",
        r->window, r->seq, i, e->kind, ts[0], ts[1], ts[2], ts[3], e->w, e->h, e->ncolor,
        e->cfmt[0], e->cfmt[1], e->cfmt[2], e->cfmt[3], e->cfmt[4], e->cfmt[5], e->cfmt[6], e->cfmt[7],
        e->dfmt, e->sfmt, e->dw, e->dh, e->rsamples, e->layers, e->la, e->sa, e->c0, e->d0,
        e->draws, e->draws_idx, e->draws_ind, e->draws_other, e->verts, e->idx, e->disp, e->disp_threads, e->disp_ind,
        e->groups, e->threads, e->ops, e->waits, e->updates, e->icb, e->icb_ind, e->icb_len, e->mesh, e->geom, e->tess,
        e->tile, e->barriers];
  }
  NSData *bytes = [s dataUsingEncoding:NSUTF8StringEncoding];
  pthread_mutex_lock(&pt_lock);
  if (r->window == pt_window && pt_text) { [pt_text appendData:bytes]; ++pt_done; PTFinishLocked(false); }
  pthread_mutex_unlock(&pt_lock);
}

/* Arm a window: caller holds pt_lock, pt_state == 0, at a present-commit. */
static void PTArmLocked(id<MTLCommandBuffer> cb) {
  if (!PTPoolReadyLocked(cb.device)) return;
  ++pt_window; pt_arm = pt_presents; pt_ctrl_end = pt_arm + pt_ctrl; pt_samp_end = pt_ctrl_end + pt_samp;
  pt_created = pt_done = pt_dead = 0;
  memset(pt_untracked, 0, sizeof pt_untracked); memset(pt_tracked_c, 0, sizeof pt_tracked_c); memset(pt_tracked_s, 0, sizeof pt_tracked_s);
  memset(pt_pool_used, 0, sizeof pt_pool_used); pt_pool_cur = 0;
  [cb.device sampleTimestamps:&pt_cpu0 gpuTimestamp:&pt_gpu0]; pt_host0 = PTHostNS();
  pt_text = [[NSMutableData alloc] initWithCapacity:1 << 20];
  NSString *h = [NSString stringWithFormat:@"{\"t\":\"window\",\"w\":%llu,\"arm\":%llu,\"ctrl_end\":%llu,\"samp_end\":%llu,\"cpu\":%llu,\"gpu\":%llu,\"host_ns\":%llu}\n",
      pt_window, pt_arm, pt_ctrl_end, pt_samp_end, pt_cpu0, pt_gpu0, pt_host0];
  [pt_text appendData:[h dataUsingEncoding:NSUTF8StringEncoding]];
  atomic_store(&pt_state, pt_ctrl ? 1 : 2);
}

static void PTMarkPresent(id<MTLCommandBuffer> cb) {
  if (!PTEnabled() || !cb) return;
  pthread_mutex_lock(&pt_lock);
  pt_present_ring[pt_present_next++ % PTPresentRing] = cb;   /* identity only, never dereferenced */
  pthread_mutex_unlock(&pt_lock);
}

/* Immediately before [cb commit]. */
static void PTOnCommit(id<MTLCommandBuffer> cb) {
  if (!pt_on || !cb) return;
  PTRecord *r = PTRecordOf(cb);
  pthread_mutex_lock(&pt_lock);
  bool present = false;
  for (unsigned i = 0; i < PTPresentRing; ++i) if (pt_present_ring[i] == cb) { present = true; pt_present_ring[i] = nil; }
  uint64_t frame = pt_presents; ++pt_commits;
  int state = atomic_load(&pt_state);
  if (r && r->window == pt_window && state != 0) {
    r->committed = YES; r->commit = pt_commits; r->frame = (uint32_t)frame; r->present = present;
    if (frame >= pt_arm && frame - pt_arm < PTMaxFrames) { if (r->mode == 's') ++pt_tracked_s[frame - pt_arm]; else ++pt_tracked_c[frame - pt_arm]; }
  } else {
    if (r) r->committed = YES;   /* stale mark of a finished window */
    if (state != 0 && frame >= pt_arm && frame - pt_arm < PTMaxFrames) ++pt_untracked[frame - pt_arm];
    r = nil;
  }
  if (present) {
    ++pt_presents;
    if (state == 1 && pt_presents >= pt_ctrl_end) atomic_store(&pt_state, state = 2);
    if (state == 2 && pt_presents >= pt_samp_end) {
      atomic_store(&pt_state, state = 3);
      uint64_t w = pt_window;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), pt_queue, ^{
        pthread_mutex_lock(&pt_lock); if (pt_window == w) PTFinishLocked(true); pthread_mutex_unlock(&pt_lock);
      });
      PTFinishLocked(false);
    }
    if (atomic_load(&pt_state) == 0 && pt_presents >= pt_skip && (pt_presents - pt_skip) % pt_every == 0) PTArmLocked(cb);
  }
  pthread_mutex_unlock(&pt_lock);
  if (r) [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { PTComplete(r, done); }];
}
