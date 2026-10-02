#include "../src/winemetal/unix/wmt_trace_ownership.h"
#include <assert.h>
#include <stdio.h>

@interface NoOverwriteBuffer : NSObject {
  uint64_t capacity;
  void *memory;
}
- (id)initWithLength:(uint64_t)value;
- (uint64_t)length;
- (void *)contents;
@end
@implementation NoOverwriteBuffer
- (id)initWithLength:(uint64_t)value { self = [super init]; capacity = value; memory = calloc(1,value); return self; }
- (uint64_t)length { return capacity; }
- (void *)contents { return memory; }
- (void)dealloc { free(memory); [super dealloc]; }
@end

static void full_cpu(id buffer, uint64_t length) {
  assert(wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_BEGIN));
  assert(wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_END));
}

int main(void) {
  @autoreleasepool {
    /* Exact two buffer sizes and BEGIN/END/GPU histories from record06. */
    for (NSNumber *size in @[@1048576, @131072]) {
      uint64_t length = size.unsignedLongLongValue;
      id old = [[NoOverwriteBuffer alloc] initWithLength:length];
      full_cpu(old, length);
      assert(wmt_trace_owner_change(old, 0, length, WMT_OWNER_GPU));
      assert(!wmt_trace_owner_change(old, 0, length, WMT_OWNER_END));
      [old release];
      id buffer = [[NoOverwriteBuffer alloc] initWithLength:length];
      assert(!wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_NO_OVERWRITE_BEGIN));
      full_cpu(buffer, length);
      memset([buffer contents], 0x11, length);
      assert(wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_NO_OVERWRITE_BEGIN));
      memset([buffer contents], 0x22, length);
      assert(!wmt_trace_owner_change(buffer, 0, 1, WMT_OWNER_BEGIN));
      NSDictionary *state = wmt_trace_owner_copy(buffer);
      assert([state[@"pending"] count] == 1 && [state[@"owned"] count] == 0);
      NSData *prior = state[@"pending"][0][@"before-image"];
      assert(prior.length == length && ((const uint8_t *)prior.bytes)[0] == 0x11);
      [state release];
      assert(wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_END));
      state = wmt_trace_owner_copy(buffer);
      assert([state[@"pending"] count] == 0 && [state[@"owned"] count] == 1);
      assert([state[@"version"] unsignedLongLongValue] == 2);
      [state release];
      assert(wmt_trace_owner_gpu_begin(buffer, 64, 64, 9));
      assert(!wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_NO_OVERWRITE_BEGIN));
      assert(wmt_trace_owner_gpu_end(buffer, 9));
      /* Completed GPU output is still not CPU authority. */
      assert(!wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_NO_OVERWRITE_BEGIN));
      assert(wmt_trace_owner_change(buffer, 64, 64, WMT_OWNER_BEGIN));
      assert(wmt_trace_owner_change(buffer, 64, 64, WMT_OWNER_END));
      /* Coverage may consist of adjacent CPU intervals of different versions. */
      assert(wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_NO_OVERWRITE_BEGIN));
      assert(wmt_trace_owner_change(buffer, 0, length, WMT_OWNER_END));
      assert(wmt_trace_owner_change(buffer, length, 0, WMT_OWNER_NO_OVERWRITE_BEGIN));
      assert(!wmt_trace_owner_change(buffer, UINT64_MAX, 1, WMT_OWNER_NO_OVERWRITE_BEGIN));
      [buffer release];
    }
    puts("PASS record06 NO_OVERWRITE/Unmap siblings, unknown and GPU-written rejection, pending scope, interval union, zero/overflow; GPU=NOT_ENABLED Wine=NOT_ENABLED");
  }
  return 0;
}
