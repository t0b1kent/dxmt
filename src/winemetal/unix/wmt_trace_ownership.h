#ifndef WMT_TRACE_OWNERSHIP_H
#define WMT_TRACE_OWNERSHIP_H
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdint.h>
#include <pthread.h>

/* Producer must bracket actual writes; stable bytes alone are not ownership.
 * Pending ranges are rejected by the snapshot caller. GPU writes invalidate
 * intersecting CPU ranges instead of becoming CPU data through a fingerprint.
 * Ledger lifetime is allocation lifetime, not an address-indexed global map. */
static char wmt_trace_ownership_key;
static uint64_t wmt_trace_owner_thread(void) {
  uint64_t thread = 0; pthread_threadid_np(NULL, &thread); return thread;
}
enum wmt_trace_owner_action {
  WMT_OWNER_BEGIN = 1, WMT_OWNER_END = 2, WMT_OWNER_GPU = 3,
  WMT_OWNER_NO_OVERWRITE_BEGIN = 4
};

static BOOL wmt_trace_owner_overlap(NSDictionary *range, uint64_t start, uint64_t end) {
  return [range[@"begin"] unsignedLongLongValue] < end &&
      [range[@"end"] unsignedLongLongValue] > start;
}

static BOOL wmt_trace_owner_change(id buffer, uint64_t offset, uint64_t length,
    unsigned action) {
  if (!buffer || action < WMT_OWNER_BEGIN || action > WMT_OWNER_NO_OVERWRITE_BEGIN) return NO;
  uint64_t capacity = [buffer length];
  if (offset > capacity || length > capacity - offset) return NO;
  if (!length) return YES;
  uint64_t end = offset + length;
  @synchronized(buffer) {
    NSMutableDictionary *ledger = objc_getAssociatedObject(buffer, &wmt_trace_ownership_key);
    if (!ledger) {
      ledger = [NSMutableDictionary dictionaryWithDictionary:
          @{@"pending": [NSMutableArray array], @"owned": [NSMutableArray array], @"version": @0}];
      objc_setAssociatedObject(buffer, &wmt_trace_ownership_key, ledger, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    NSMutableArray *pending = ledger[@"pending"], *owned = ledger[@"owned"];
    NSData *before_image = nil;
    if (action != WMT_OWNER_GPU)
      for (NSDictionary *range in ledger[@"gpu"])
        if (wmt_trace_owner_overlap(range, offset, end)) return NO;
    if (action == WMT_OWNER_NO_OVERWRITE_BEGIN) {
      /* GPU reads do not change these bytes. NO_OVERWRITE may leave any byte
       * unchanged, so every byte must already have CPU provenance. Do not
       * promote unknown or previously GPU-written bytes by taking a snapshot. */
      uint64_t cursor = offset;
      while (cursor < end) {
        uint64_t next = cursor;
        for (NSDictionary *range in owned)
          if ([range[@"begin"] unsignedLongLongValue] <= cursor &&
              [range[@"end"] unsignedLongLongValue] > next)
            next = [range[@"end"] unsignedLongLongValue];
        if (next == cursor) return NO;
        cursor = next < end ? next : end;
      }
      const void *bytes = [(id<MTLBuffer>)buffer contents];
      if (!bytes || length > NSUIntegerMax) return NO;
      /* Before Map returns to the CPU writer, preserve the last CPU image.
       * D3D11 NO_OVERWRITE promises that bytes already used by queued GPU reads
       * remain unchanged. A commit during this lease can use this immutable
       * image; it must never sample memory concurrently being written. */
      before_image = [NSData dataWithBytes:(const char *)bytes + offset length:(NSUInteger)length];
    }
    if (action == WMT_OWNER_END) {
      if ([ledger[@"version"] unsignedLongLongValue] == UINT64_MAX) return NO;
      NSDictionary *match = nil;
      for (NSDictionary *range in pending)
        if ([range[@"begin"] unsignedLongLongValue] == offset &&
            [range[@"end"] unsignedLongLongValue] == end) { match = range; break; }
      if (!match) return NO;
      [pending removeObject:match];
    } else {
      for (NSDictionary *range in pending)
        if (wmt_trace_owner_overlap(range, offset, end)) return NO;
    }
    /* Split old intervals: a partial GPU write must preserve unrelated CPU bytes. */
    NSMutableArray *replacement = [NSMutableArray array];
    for (NSDictionary *range in owned) {
      if (!wmt_trace_owner_overlap(range, offset, end)) { [replacement addObject:range]; continue; }
      uint64_t begin = [range[@"begin"] unsignedLongLongValue];
      uint64_t finish = [range[@"end"] unsignedLongLongValue];
      if (begin < offset) [replacement addObject:@{@"begin": @(begin), @"end": @(offset),
          @"version": range[@"version"]}];
      if (finish > end) [replacement addObject:@{@"begin": @(end), @"end": @(finish),
          @"version": range[@"version"]}];
    }
    [owned setArray:replacement];
    if (action == WMT_OWNER_BEGIN || action == WMT_OWNER_NO_OVERWRITE_BEGIN) {
      [pending addObject:before_image ? @{@"begin": @(offset), @"end": @(end), @"writer-thread": @(wmt_trace_owner_thread()),
          @"before-image": before_image, @"version": ledger[@"version"],
          @"promise": @"D3D11-NO_OVERWRITE"} : @{@"begin": @(offset), @"end": @(end), @"writer-thread": @(wmt_trace_owner_thread())}];
    } else if (action == WMT_OWNER_END) {
      uint64_t version = [ledger[@"version"] unsignedLongLongValue];
      ledger[@"version"] = @(version + 1);
      [owned addObject:@{@"begin": @(offset), @"end": @(end), @"version": @(version + 1)}];
    }
    return YES;
  }
}

/* An explicit synchronous copy can execute inside its caller's write lease.
 * It borrows that lease; only the outer END publishes CPU authority. Generic
 * BEGIN remains non-reentrant and foreign/partial overlapping writers reject.
 * NO_OVERWRITE is deliberately excluded: it promises unchanged queued inputs. */
static BOOL wmt_trace_owner_borrow_write(id buffer, uint64_t offset, uint64_t length) {
  if (!buffer || !length || offset > [buffer length] || length > [buffer length] - offset) return NO;
  @synchronized(buffer) {
    NSDictionary *ledger = objc_getAssociatedObject(buffer, &wmt_trace_ownership_key);
    for (NSDictionary *range in ledger[@"gpu"])
      if (wmt_trace_owner_overlap(range, offset, offset + length)) return NO;
    for (NSDictionary *range in ledger[@"pending"])
      if ([range[@"writer-thread"] unsignedLongLongValue] == wmt_trace_owner_thread() &&
          !range[@"promise"] && [range[@"begin"] unsignedLongLongValue] <= offset &&
          [range[@"end"] unsignedLongLongValue] >= offset + length) return YES;
    return NO;
  }
}

/* Invalidation must last until completion, not merely until submission. Multiple
 * queued command buffers may write overlapping ranges. A ticket removes only
 * its own range, so an earlier completion cannot reopen a still-busy interval. */
static BOOL wmt_trace_owner_gpu_begin(id buffer, uint64_t offset, uint64_t length,
    uint64_t ticket) {
  if (!buffer || !ticket || !length) return NO;
  @synchronized(buffer) {
    NSDictionary *previous = objc_getAssociatedObject(buffer, &wmt_trace_ownership_key);
    for (NSDictionary *range in previous[@"gpu"])
      if ([range[@"ticket"] unsignedLongLongValue] == ticket &&
          [range[@"begin"] unsignedLongLongValue] == offset &&
          [range[@"end"] unsignedLongLongValue] == offset + length) return NO;
    if (!wmt_trace_owner_change(buffer, offset, length, WMT_OWNER_GPU)) return NO;
    NSMutableDictionary *ledger = objc_getAssociatedObject(buffer, &wmt_trace_ownership_key);
    if (!ledger[@"gpu"]) ledger[@"gpu"] = [NSMutableArray array];
    [ledger[@"gpu"] addObject:@{@"begin": @(offset), @"end": @(offset + length),
        @"ticket": @(ticket)}];
    return YES;
  }
}

static BOOL wmt_trace_owner_gpu_end(id buffer, uint64_t ticket) {
  if (!buffer || !ticket) return NO;
  @synchronized(buffer) {
    NSMutableDictionary *ledger = objc_getAssociatedObject(buffer, &wmt_trace_ownership_key);
    NSMutableArray *ranges = ledger[@"gpu"];
    NSMutableArray *finished = [NSMutableArray array];
    for (NSDictionary *range in ranges)
      if ([range[@"ticket"] unsignedLongLongValue] == ticket) [finished addObject:range];
    if (![finished count]) return NO;
    [ranges removeObjectsInArray:finished];
    return YES;
  }
}

/* Immutable snapshot for commit intersection. No ledger means UNKNOWN, not empty. */
static NSDictionary *wmt_trace_owner_copy(id buffer) {
  @synchronized(buffer) {
    NSDictionary *ledger = objc_getAssociatedObject(buffer, &wmt_trace_ownership_key);
    if (!ledger) return nil;
    return [@{@"pending": [NSArray arrayWithArray:ledger[@"pending"]],
        @"gpu": [NSArray arrayWithArray:ledger[@"gpu"] ?: @[]],
        @"owned": [NSArray arrayWithArray:ledger[@"owned"]], @"version": ledger[@"version"]} retain];
  }
}
#endif
