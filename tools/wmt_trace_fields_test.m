#import "../src/winemetal/unix/wmt_trace_fields.h"
#include <assert.h>
#include <stdio.h>

/* Foundation-only test. No Wine, Metal device, game, or GPU work. */
@interface TestBuffer : NSObject
- (NSUInteger)length;
@end
@implementation TestBuffer
- (NSUInteger)length { return 256; }
@end

static NSUInteger count_fields(id buffer) {
  NSDictionary *copy = wmt_trace_copy_fields(buffer);
  NSUInteger count = [copy count];
  [copy release];
  return count;
}

int main(void) {
  @autoreleasepool {
    TestBuffer *buffer = [[TestBuffer alloc] init];
    assert(wmt_trace_declare_fields(buffer, 0, 4, 16, 1)); /* vertex */
    assert(count_fields(buffer) == 4);
    assert(wmt_trace_declare_fields(buffer, 64, 2, 8, 3)); /* sampler pair */
    assert(wmt_trace_declare_fields(buffer, 96, 1, 8, 2)); /* SRV texture */
    assert(wmt_trace_declare_fields(buffer, 112, 1, 8, 1)); /* UAV counter */
    assert(count_fields(buffer) == 8); /* length/stride/metadata excluded */
    assert(!wmt_trace_declare_fields(buffer, 64, 1, 8, 2)); /* kind conflict */
    assert(!wmt_trace_declare_fields(buffer, 249, 1, 8, 1));
    assert(!wmt_trace_declare_fields(buffer, 0, UINT64_MAX, 8, 1));
    assert(!wmt_trace_declare_fields(buffer, 0, 2, UINT64_MAX, 1));
    assert(!wmt_trace_declare_fields(buffer, 0, 2, 0, 1));
    assert(!wmt_trace_declare_fields(buffer, 0, 1, 8, 4));
    assert(!wmt_trace_declare_fields(nil, 0, 1, 8, 1));
    /* Whole-batch rejection must not insert a preceding valid member. */
    assert(!wmt_trace_declare_fields(buffer, 56, 2, 8, 2));
    assert(count_fields(buffer) == 8);
    assert(wmt_trace_declare_fields(buffer, 64, 16, 1, 0));
    assert(count_fields(buffer) == 6);
    assert(wmt_trace_declare_fields(buffer, 64, 1, 8, 2));
    /* Reset overlapping only the final byte removes the entire field. */
    assert(wmt_trace_declare_fields(buffer, 71, 1, 1, 0));
    assert(count_fields(buffer) == 6);
    assert(wmt_trace_declare_fields(buffer, 256, 0, 8, 1));
    assert(wmt_trace_declare_fields(buffer, 0, 256, 1, 0));
    assert(count_fields(buffer) == 0);
    [buffer release];
    buffer = [[TestBuffer alloc] init];
    assert(count_fields(buffer) == 0); /* fresh lifetime */
    [buffer release];
  }
  puts("PASS native field ledger: vertex/sampler/SRV/counter, range reuse, overflow, atomic rejection, lifetime");
  return 0;
}
