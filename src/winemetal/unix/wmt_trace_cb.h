#ifndef WMT_TRACE_CB_H
#define WMT_TRACE_CB_H
#import <Metal/Metal.h>
#include "wmt_trace_commands.h"
#include "wmt_trace_registry.h"
#include "wmt_trace_ownership.h"

/* Shared state has no back-reference to CB/encoder. It retains referenced buffers
 * until the CB's completion handler is released. Whole allocations deliberately
 * overestimate explicit buffer ranges; shader/texture aliases remain INCOMPLETE. */
static char wmt_trace_cb_key;
static char wmt_trace_backing_buffer_key;
/* Constructor-established alias: retain the allocation, never infer pitch/range
 * from the texture format. Views inherit the same root allocation. */
static void wmt_trace_cb_alias(id texture, id parent, BOOL parent_is_buffer) {
  if (!texture || !parent) return;
  id buffer = parent_is_buffer ? parent :
      objc_getAssociatedObject(parent, &wmt_trace_backing_buffer_key);
  if (buffer) objc_setAssociatedObject(texture, &wmt_trace_backing_buffer_key,
      buffer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static void wmt_trace_cb_associate(id encoder, id cb) {
  if (!cb) return;
  @synchronized(cb) {
    NSMutableDictionary *state = objc_getAssociatedObject(cb, &wmt_trace_cb_key);
    if (!state) {
      state = [NSMutableDictionary dictionaryWithDictionary:@{
          @"buffers": [NSMutableDictionary dictionary], @"writes": [NSMutableDictionary dictionary],
          @"reads": [NSMutableDictionary dictionary],
          @"producer-reads": [NSMutableSet set],
          @"resources": [NSMutableSet set], @"sealed": @NO}];
      objc_setAssociatedObject(cb, &wmt_trace_cb_key, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    if (encoder)
      objc_setAssociatedObject(encoder, &wmt_trace_cb_key, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
}

static void wmt_trace_cb_use(id encoder, id resource, BOOL write) {
  NSMutableDictionary *state = objc_getAssociatedObject(encoder, &wmt_trace_cb_key);
  if (!state || !resource) return;
  NSData *descriptor = objc_getAssociatedObject(resource, &wmt_trace_resource_key);
  if (!descriptor || [descriptor length] != sizeof(struct wmt_trace_resource)) return;
  struct wmt_trace_resource entry;
  [descriptor getBytes:&entry length:sizeof(entry)];
  if (entry.kind != 1 && entry.kind != 2) return;
  @synchronized(state) {
    if ([state[@"sealed"] boolValue]) { state[@"failure"] = @"encoding after commit"; return; }
    if ([state[@"resources"] count] >= 65536 &&
        ![state[@"resources"] containsObject:@(entry.object_id)]) {
      state[@"failure"] = @"CB resource cap"; return;
    }
    [state[@"resources"] addObject:@(entry.object_id)];
    if (entry.kind == 1) {
      state[@"buffers"][@(entry.object_id)] = resource;
      if (write) state[@"writes"][@(entry.object_id)] = resource;
    }
  }
  if (entry.kind == 2) {
    id backing = objc_getAssociatedObject(resource, &wmt_trace_backing_buffer_key);
    if (backing) wmt_trace_cb_use(encoder, backing, write);
  }
}

/* Blit source spans are explicit in the ABI. Shader bindings and buffer-backed
 * textures conservatively read the whole allocation; never infer shader bounds. */
static void wmt_trace_cb_read(id encoder, id resource, uint64_t offset,
    uint64_t length, BOOL whole) {
  wmt_trace_cb_use(encoder, resource, NO);
  NSMutableDictionary *state = objc_getAssociatedObject(encoder, &wmt_trace_cb_key);
  if (!state || !resource) return;
  NSData *descriptor = objc_getAssociatedObject(resource, &wmt_trace_resource_key);
  if (descriptor.length != sizeof(struct wmt_trace_resource)) return;
  struct wmt_trace_resource entry; [descriptor getBytes:&entry length:sizeof(entry)];
  if (entry.kind == 2) {
    id backing = objc_getAssociatedObject(resource, &wmt_trace_backing_buffer_key);
    if (backing) wmt_trace_cb_read(encoder, backing, 0, 0, YES);
    return;
  }
  if (entry.kind != 1) return;
  @synchronized(state) {
    NSNumber *key = @(entry.object_id);
    if (whole && [state[@"producer-reads"] containsObject:key]) return;
    if (whole) { state[@"reads"][key] = [NSNull null]; return; }
    uint64_t capacity = [resource length];
    if (offset > capacity || length > capacity-offset) {
      state[@"failure"] = @"explicit blit read outside allocation"; return;
    }
    if (!length || state[@"reads"][key] == [NSNull null]) return;
    NSMutableArray *ranges = state[@"reads"][key];
    if (!ranges) state[@"reads"][key] = ranges = [NSMutableArray array];
    uint64_t begin = offset, end = offset+length;
    NSMutableArray *remaining = [NSMutableArray array];
    for (NSDictionary *range in ranges) {
      uint64_t a = [range[@"begin"] unsignedLongLongValue], b = [range[@"end"] unsignedLongLongValue];
      if (a <= end && b >= begin) { begin = MIN(begin,a); end = MAX(end,b); }
      else [remaining addObject:range];
    }
    [remaining addObject:@{@"begin": @(begin), @"end": @(end)}];
    [ranges setArray:remaining];
  }
}

/* All logical Buffer reads (raw/view, all shader stages) are declared by the
 * DXMT access family. These replace generic residency's allocation-wide read;
 * precise blit spans continue to be unioned with them. */
static void wmt_trace_cb_producer_read(id encoder, id buffer, uint64_t offset, uint64_t length) {
  NSMutableDictionary *state = objc_getAssociatedObject(encoder, &wmt_trace_cb_key);
  if (!state || !buffer) return;
  NSNumber *key = @(wmt_trace_object_id(buffer));
  @synchronized(state) {
    if (![state[@"producer-reads"] containsObject:key]) {
      [state[@"producer-reads"] addObject:key];
      if (state[@"reads"][key] == [NSNull null]) [state[@"reads"] removeObjectForKey:key];
    }
    wmt_trace_cb_read(encoder,buffer,offset,length,NO);
  }
}

static void wmt_trace_cb_blit_pitch(id encoder, id resource, uint64_t offset,
    uint64_t row_pitch, uint64_t image_pitch, struct WMTSize size) {
  if (!size.width || !size.height || !size.depth) return;
  uint64_t image = image_pitch;
  if (!image) {
    if (row_pitch && size.height > UINT64_MAX/row_pitch) {
      wmt_trace_cb_read(encoder, resource, 0, 0, YES); return;
    }
    image = row_pitch*size.height;
  }
  if (image && size.depth > UINT64_MAX/image) {
    wmt_trace_cb_read(encoder, resource, 0, 0, YES); return;
  }
  uint64_t capacity = [resource length];
  /* A legal last row/image need not include trailing pitch padding. This is a
   * conservative span, capped at allocation end, not a pixel-format guess. */
  uint64_t span = image*size.depth;
  if (offset <= capacity) span = MIN(span,capacity-offset);
  wmt_trace_cb_read(encoder, resource, offset, span, NO);
}

/* Called only after the bounded command codec validated the complete list. All
 * direct handles are tracked by the codec callback. Buffer destinations are
 * invalidated conservatively as whole allocations, not guessed texture pitches. */
static void wmt_trace_cb_command_writes(unsigned kind, id encoder,
    const struct wmtcmd_base *node) {
  for (; node; node = node->next.ptr) {
    struct wmt_trace_command_schema schema;
    if (!wmt_trace_command_schema(kind, node->type, &schema)) return;
    if ((kind == 2 && node->type == WMTComputeCommandTraceBufferRead) ||
        (kind == 3 && node->type == WMTRenderCommandTraceBufferRead)) {
      const struct wmtcmd_trace_buffer_read *body = (const void *)node;
      wmt_trace_cb_producer_read(encoder, (id)body->buffer, body->offset, body->length);
      continue;
    }
    if (kind == 1) {
      if (node->type == WMTBlitCommandCopyFromBufferToBuffer) {
        const struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (const void *)node;
        wmt_trace_cb_read(encoder, (id)body->src, body->src_offset, body->copy_length, NO);
      } else if (node->type == WMTBlitCommandCopyFromBufferToTexture) {
        const struct wmtcmd_blit_copy_from_buffer_to_texture *body = (const void *)node;
        wmt_trace_cb_blit_pitch(encoder, (id)body->src, body->src_offset,
            body->bytes_per_row, body->bytes_per_image, body->size);
      } else if (node->type == WMTBlitCommandCopyFromBufferToTextureWithBlitOption) {
        const struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *body = (const void *)node;
        wmt_trace_cb_blit_pitch(encoder, (id)body->src, body->src_offset,
            body->bytes_per_row, body->bytes_per_image, body->size);
      } else if (schema.handle_count && node->type != WMTBlitCommandFillBuffer &&
          node->type != WMTBlitCommandUpdateFence && node->type != WMTBlitCommandWaitForFence) {
        obj_handle_t source;
        memcpy(&source, (const char *)node+schema.handles[0], sizeof(source));
        wmt_trace_cb_read(encoder, (id)source, 0, 0, YES);
      }
      if (node->type == WMTBlitCommandFillBuffer) {
        const struct wmtcmd_blit_fillbuffer *body = (const void *)node;
        wmt_trace_cb_use(encoder, (id)body->buffer, YES);
      } else if (schema.handle_count == 2 && node->type != WMTBlitCommandUpdateFence &&
          node->type != WMTBlitCommandWaitForFence) {
        obj_handle_t destination;
        memcpy(&destination, (const char *)node + schema.handles[1], sizeof(destination));
        wmt_trace_cb_use(encoder, (id)destination, YES);
      }
    } else if (kind == 2 && node->type == WMTComputeCommandUseResource) {
      const struct wmtcmd_compute_useresource *body = (const void *)node;
      wmt_trace_cb_use(encoder, (id)body->resource, (body->usage & 2) != 0);
    } else if (kind == 3 && node->type == WMTRenderCommandUseResource) {
      const struct wmtcmd_render_useresource *body = (const void *)node;
      wmt_trace_cb_use(encoder, (id)body->resource, (body->usage & 2) != 0);
    }
  }
}

static NSDictionary *wmt_trace_cb_copy(id cb) {
  NSMutableDictionary *state = objc_getAssociatedObject(cb, &wmt_trace_cb_key);
  if (!state) return nil;
  @synchronized(state) {
    if ([state[@"sealed"] boolValue]) state[@"failure"] = @"duplicate commit";
    state[@"sealed"] = @YES;
    return [@{@"buffers": [NSDictionary dictionaryWithDictionary:state[@"buffers"]],
        @"reads": [NSDictionary dictionaryWithDictionary:state[@"reads"]],
        @"writes": [NSDictionary dictionaryWithDictionary:state[@"writes"]],
        @"resources": [[state[@"resources"] allObjects] sortedArrayUsingSelector:@selector(compare:)],
        @"failure": state[@"failure"] ?: @""} retain];
  }
}

/* Shared by the unix producer and native correctness fixture. The copied block
 * retains pending buffers until completion; waitUntilCompleted alone is not a
 * substitute for waiting for the ownership callback. */
static BOOL wmt_trace_cb_arm_writes(id<MTLCommandBuffer> cb, NSDictionary *writes,
    uint64_t ticket, void (^report)(id<MTLCommandBuffer>, NSDictionary *)) {
  NSMutableArray *queued = [NSMutableArray array];
  BOOL accepted = YES;
  for (id buffer in [writes allValues]) {
    if (wmt_trace_owner_gpu_begin(buffer, 0, [buffer length], ticket))
      [queued addObject:buffer];
    else accepted = NO;
  }
  NSArray *pending = [NSArray arrayWithArray:queued];
  [cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
    @autoreleasepool {
      BOOL released = YES;
      for (id buffer in pending)
        if (!wmt_trace_owner_gpu_end(buffer, ticket)) released = NO;
      report(completed, @{@"status": @([completed status]),
          @"GPUStartTime": @([completed GPUStartTime]),
          @"GPUEndTime": @([completed GPUEndTime]),
          @"ownership-release": released ? @"PRESENT" : @"FAILED",
          @"queued-buffer-count": @([pending count]),
          @"error": [[completed error] localizedDescription] ?: @""});
    }
  }];
  return accepted;
}
#endif
