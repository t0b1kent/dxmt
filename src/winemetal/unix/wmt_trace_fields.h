#ifndef WMT_TRACE_FIELDS_H
#define WMT_TRACE_FIELDS_H

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdint.h>

/* The associated ledger dies with the buffer, so handle reuse cannot inherit
 * old fields. Reset each newly allocated argument-buffer range before encoding.
 * This file is shared with a native, GPU-free contract test. */
static char wmt_trace_fields_key;

static BOOL
wmt_trace_declare_fields(id buffer, uint64_t offset, uint64_t count,
                         uint64_t stride, uint32_t kind) {
  if (!buffer || kind > 3)
    return NO;
  uint64_t length = (uint64_t)[buffer length];
  if (offset > length)
    return NO;
  if (kind == 0) {
    if (stride != 1 || count > length - offset)
      return NO;
  } else if (count) {
    /* Division before multiplication keeps malformed PE input overflow-safe. */
    if ((offset & 7) || (stride & 7) || stride < 8 ||
        length - offset < 8 || (count - 1) > (length - offset - 8) / stride)
      return NO;
  }
  @synchronized(buffer) {
    NSMutableDictionary *fields = objc_getAssociatedObject(buffer, &wmt_trace_fields_key);
    if (!fields) {
      fields = [NSMutableDictionary dictionary];
      objc_setAssociatedObject(buffer, &wmt_trace_fields_key, fields,
                               OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    if (kind == 0) {
      /* Also clear partial overlap: no 8-byte field may straddle a reused range. */
      for (NSNumber *key in [fields allKeys]) {
        uint64_t position = [key unsignedLongLongValue];
        if (count && position < offset + count && position + 8 > offset)
          [fields removeObjectForKey:key];
      }
    } else {
      /* Validate the whole batch before mutating it. */
      for (uint64_t i = 0; i < count; ++i) {
        NSNumber *old = fields[@(offset + i * stride)];
        if (old && [old unsignedIntValue] != kind)
          return NO;
      }
      for (uint64_t i = 0; i < count; ++i)
        fields[@(offset + i * stride)] = @(kind);
    }
  }
  return YES;
}

/* Caller owns the returned immutable copy (MRC); no object handles or values
 * are guessed here. Snapshot code will resolve actual values through its map. */
static NSDictionary *
wmt_trace_copy_fields(id buffer) {
  @synchronized(buffer) {
    NSDictionary *fields = objc_getAssociatedObject(buffer, &wmt_trace_fields_key);
    return fields ? [fields copy] : [[NSDictionary alloc] init];
  }
}
#endif
