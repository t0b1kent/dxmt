#ifndef WMT_TRACE_API_LIFETIME_H
#define WMT_TRACE_API_LIFETIME_H
#include <objc/runtime.h>
#include <pthread.h>
/* CPU pool scopes are not Metal objects. Keep their addresses only in RAM,
 * without ObjC weak keys/associated objects (NSAutoreleasePool forbids weak).
 * Pool IDs use a reserved high-bit namespace; GPU generations use low bits. */
static NSMutableDictionary *wmt_trace_api_pools;
static NSMutableDictionary *wmt_trace_api_lifetime;
static uint64_t wmt_trace_api_pool_next=UINT64_C(1)<<63;
static uint64_t wmt_trace_api_lifetime_order;
static void wmt_trace_api_lifetime_delta(uint64_t identifier,BOOL pool,int delta) {
  if(!identifier)return;
  @synchronized([NSPropertyListSerialization class]) {
    if(!wmt_trace_api_lifetime)wmt_trace_api_lifetime=[NSMutableDictionary new];
    NSMutableDictionary *entry=wmt_trace_api_lifetime[@(identifier)];
    if(!entry) {
      entry=[@{@"object":@(identifier),@"pool":@(pool),@"retain":@0,@"release":@0,
          @"absolute-refcount":@"UNKNOWN"} mutableCopy];
      wmt_trace_api_lifetime[@(identifier)]=entry;[entry release];
    }
    NSString *key=delta>0?@"retain":@"release";
    entry[key]=@([entry[key] unsignedLongLongValue]+1);
    entry[@"last-order"]=@(wmt_trace_api_lifetime_order++);
  }
}
static void wmt_trace_api_pool_created(const void *pool) {
  if(!pool)return;
  @synchronized([NSPropertyListSerialization class]) {
    if(!wmt_trace_api_pools)wmt_trace_api_pools=[NSMutableDictionary new];
    NSNumber *key=@((uintptr_t)pool);
    if(wmt_trace_api_pools[key]) {
      wmt_trace_record_event(@"api-lifetime",@"WMT_API_POLICY",
          @{@"status":@"FAILED",@"reason":@"pool pointer reused without observed release"});
      return;
    }
    uint64_t thread=0;pthread_threadid_np(NULL,&thread);
    NSNumber *identifier=@(++wmt_trace_api_pool_next);
    wmt_trace_api_pools[key]=@{@"object":identifier,@"thread":@(thread)};
    wmt_trace_api_lifetime_delta(identifier.unsignedLongLongValue,YES,1);
  }
}
static void wmt_trace_api_object_delta(id object,int delta) {
  if(!object)return;
  @synchronized([NSPropertyListSerialization class]) {
    NSNumber *key=@((uintptr_t)object);
    NSDictionary *pool=wmt_trace_api_pools[key];
    if(pool) {
      wmt_trace_api_lifetime_delta([pool[@"object"] unsignedLongLongValue],YES,delta);
      if(delta<0)[wmt_trace_api_pools removeObjectForKey:key];
      return;
    }
    /* Only objects already registered in the GPU effect stream need this
     * ownership model. CPU query/cache/string owners are projected artifacts.
     * Never register an arbitrary owner merely because it was retained. */
    NSData *descriptor=objc_getAssociatedObject(object,&wmt_trace_resource_key);
    if(descriptor) {
      struct wmt_trace_resource resource;
      [descriptor getBytes:&resource length:sizeof(resource)];
      wmt_trace_api_lifetime_delta(resource.object_id,NO,delta);
    }
  }
}
static void wmt_trace_api_lifetime_flush(void) {
  @synchronized([NSPropertyListSerialization class]) {
    if(!wmt_trace_api_lifetime.count)return;
    NSArray *entries=[wmt_trace_api_lifetime.allValues sortedArrayUsingComparator:^NSComparisonResult(id a,id b) {
      return [a[@"object"] compare:b[@"object"]];
    }];
    wmt_trace_record_event(@"api-lifetime",@"WMT_API_POLICY",
        @{@"status":@"PRESENT",@"deltas":entries,@"absolute-refcount":@"UNKNOWN",
          @"native-model":@"independent retained registry through submitted GPU completion; no retainCount guess"});
    [wmt_trace_api_lifetime removeAllObjects];
  }
}
#endif
