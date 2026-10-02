#ifndef WMT_TRACE_REGISTRY_H
#define WMT_TRACE_REGISTRY_H
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include "wmt_trace_relocate.h"

/* A descriptor is retained by its resource, not vice versa. Weak map keys
 * disappear on deallocation; reused native pointers get new recording IDs.
 * All access is serialized. This is not a retain/release interception layer. */
static NSMapTable *wmt_trace_registry;
static uint64_t wmt_trace_next_id;
static char wmt_trace_resource_key;

static NSMapTable *wmt_trace_registry_table(void) {
  @synchronized([NSMapTable class]) {
    if (!wmt_trace_registry)
      wmt_trace_registry = [[NSMapTable alloc]
          initWithKeyOptions:NSPointerFunctionsWeakMemory |
                             NSPointerFunctionsObjectPointerPersonality
                valueOptions:NSPointerFunctionsStrongMemory capacity:256];
    return wmt_trace_registry;
  }
}

static uint64_t wmt_trace_register_resource(id resource, uint32_t kind,
    uint64_t address, uint64_t length, uint64_t resource_id) {
  if (!resource || kind < 1 || kind > 4 ||
      (kind == 1 && (!address || !length)))
    return 0;
  NSMapTable *table = wmt_trace_registry_table();
  @synchronized(table) {
    NSData *previous = objc_getAssociatedObject(resource, &wmt_trace_resource_key);
    if (previous) {
      struct wmt_trace_resource old;
      [previous getBytes:&old length:sizeof(old)];
      return old.kind == kind && old.address == address && old.length == length &&
          old.resource_id == resource_id ? old.object_id : 0;
    }
    if (wmt_trace_next_id >= (UINT64_C(1)<<63)-1)
      return 0;
    struct wmt_trace_resource entry = {++wmt_trace_next_id, address, length,
                                      resource_id, kind};
    NSData *descriptor = [NSData dataWithBytes:&entry length:sizeof(entry)];
    objc_setAssociatedObject(resource, &wmt_trace_resource_key, descriptor,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [table setObject:descriptor forKey:resource];
    return entry.object_id;
  }
}

/* Opaque command objects (PSOs, fences, encoders) share the resource ID
 * namespace. Their construction descriptors must be recorded separately. */
static uint64_t wmt_trace_object_id(id object) {
  if (!object) return 0;
  NSMapTable *table = wmt_trace_registry_table();
  @synchronized(table) {
    NSData *descriptor = objc_getAssociatedObject(object, &wmt_trace_resource_key);
    if (descriptor) {
      struct wmt_trace_resource entry;
      [descriptor getBytes:&entry length:sizeof(entry)];
      return entry.object_id;
    }
    return wmt_trace_register_resource(object, 4, 0, 0, 0);
  }
}

/* Caller owns both copies under MRC. Retained keys freeze lifetimes while
 * resolving a snapshot. Entries contain runtime addresses, never disk bytes. */
static NSData *wmt_trace_copy_resources(NSArray **owners) {
  if (!owners)
    return nil;
  NSMapTable *table = wmt_trace_registry_table();
  @synchronized(table) {
    NSArray *keys = [[[table keyEnumerator] allObjects] copy];
    NSMutableData *entries = [NSMutableData data];
    for (id key in keys) {
      NSData *descriptor = [table objectForKey:key];
      if (descriptor)
        [entries appendData:descriptor];
    }
    *owners = keys;
    return [entries copy];
  }
}
#endif
