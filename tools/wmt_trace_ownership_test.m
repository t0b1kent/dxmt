#include "../src/winemetal/unix/wmt_trace_ownership.h"
#include <assert.h>
@interface OwnerTestBuffer : NSObject
@end
@implementation OwnerTestBuffer
- (uint64_t)length { return 128; }
@end
int main(void) {
  @autoreleasepool {
    id buffer = [OwnerTestBuffer new];
    assert(!wmt_trace_owner_copy(buffer));
    assert(!wmt_trace_owner_change(buffer, UINT64_MAX, 2, WMT_OWNER_BEGIN));
    assert(!wmt_trace_owner_change(buffer, 0, 16, WMT_OWNER_END));
    assert(wmt_trace_owner_change(buffer, 0, 64, WMT_OWNER_BEGIN));
    assert(!wmt_trace_owner_change(buffer, 32, 8, WMT_OWNER_BEGIN));
    assert(!wmt_trace_owner_change(buffer, 32, 8, WMT_OWNER_GPU));
    NSDictionary *state = wmt_trace_owner_copy(buffer);
    assert([state[@"pending"] count] == 1 && [state[@"owned"] count] == 0); [state release];
    assert(wmt_trace_owner_change(buffer, 0, 64, WMT_OWNER_END));
    assert(wmt_trace_owner_change(buffer, 16, 16, WMT_OWNER_GPU));
    state = wmt_trace_owner_copy(buffer);
    assert([state[@"owned"] count] == 2 && [state[@"pending"] count] == 0);
    assert([state[@"version"] unsignedLongLongValue] == 1);
    NSArray *ranges = state[@"owned"];
    assert([ranges[0][@"begin"] unsignedLongLongValue] == 0);
    assert([ranges[0][@"end"] unsignedLongLongValue] == 16);
    assert([ranges[1][@"begin"] unsignedLongLongValue] == 32);
    assert([ranges[1][@"end"] unsignedLongLongValue] == 64);
    assert(wmt_trace_owner_change(buffer, 16, 16, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_change(buffer, 16, 16, WMT_OWNER_END));
    /* A returned snapshot must not mutate when producer publishes another version. */
    assert([state[@"owned"] count] == 2); [state release];
    state = wmt_trace_owner_copy(buffer);
    assert([state[@"owned"] count] == 3 && [state[@"version"] unsignedLongLongValue] == 2);
    [state release];
    assert(wmt_trace_owner_gpu_begin(buffer, 16, 16, 100));
    assert(!wmt_trace_owner_gpu_begin(buffer, 16, 16, 100));
    assert(wmt_trace_owner_gpu_begin(buffer, 24, 16, 101));
    assert(!wmt_trace_owner_change(buffer, 16, 8, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_change(buffer, 0, 8, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_change(buffer, 0, 8, WMT_OWNER_END));
    state = wmt_trace_owner_copy(buffer);
    assert([state[@"gpu"] count] == 2);
    assert(wmt_trace_owner_gpu_end(buffer, 100));
    assert(!wmt_trace_owner_gpu_end(buffer, 100));
    assert(!wmt_trace_owner_change(buffer, 24, 8, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_change(buffer, 16, 8, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_change(buffer, 16, 8, WMT_OWNER_END));
    assert([state[@"gpu"] count] == 2); [state release];
    assert(wmt_trace_owner_gpu_end(buffer, 101));
    assert(wmt_trace_owner_change(buffer, 24, 16, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_change(buffer, 24, 16, WMT_OWNER_END));
    assert(!wmt_trace_owner_gpu_begin(buffer, UINT64_MAX, 2, 102));
    assert(!wmt_trace_owner_gpu_begin(buffer, 0, 0, 102));
    [buffer release];
    buffer = [OwnerTestBuffer new]; assert(!wmt_trace_owner_copy(buffer)); [buffer release];
    puts("PASS ownership: pending overlap, GPU split, queued overlapping tickets, completion isolation, CPU versions, immutable snapshot, lifetime, overflow; no GPU/Wine");
  }
  return 0;
}
