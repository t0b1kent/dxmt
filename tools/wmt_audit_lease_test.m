#include "../src/winemetal/unix/wmt_trace_ownership.h"
#include <assert.h>
@interface LeaseTestBuffer : NSObject @end
@implementation LeaseTestBuffer
- (uint64_t)length { return 256; }
@end
static void *foreign(void *arg) {
  @autoreleasepool { assert(!wmt_trace_owner_borrow_write((id)arg,0,32)); }
  return NULL;
}
int main(void) { @autoreleasepool {
  id b=[LeaseTestBuffer new];
  assert(!wmt_trace_owner_borrow_write(b,0,32));
  assert(wmt_trace_owner_change(b,0,256,WMT_OWNER_BEGIN));
  assert(wmt_trace_owner_change(b,0,256,WMT_OWNER_END));
  assert(wmt_trace_owner_change(b,0,32,WMT_OWNER_BEGIN));
  assert(wmt_trace_owner_borrow_write(b,0,32));
  assert(wmt_trace_owner_borrow_write(b,4,8));
  assert(!wmt_trace_owner_borrow_write(b,16,32));
  assert(!wmt_trace_owner_borrow_write(b,UINT64_MAX,2));
  assert(!wmt_trace_owner_change(b,0,32,WMT_OWNER_BEGIN));
  pthread_t t; assert(!pthread_create(&t,NULL,foreign,b)); assert(!pthread_join(t,NULL));
  NSDictionary *ledger=wmt_trace_owner_copy(b);
  assert([ledger[@"pending"] count]==1);
  assert([ledger[@"owned"] count]==1);
  assert([ledger[@"owned"][0][@"begin"] unsignedLongLongValue]==32); [ledger release];
  assert(wmt_trace_owner_change(b,0,32,WMT_OWNER_END));
  assert(!wmt_trace_owner_borrow_write(b,0,32));
  assert(wmt_trace_owner_gpu_begin(b,0,32,7));
  assert(!wmt_trace_owner_borrow_write(b,0,32));
  assert(!wmt_trace_owner_change(b,0,32,WMT_OWNER_BEGIN));
  assert(wmt_trace_owner_gpu_end(b,7));
  [b release];
  puts("PASS borrowed copy: exact/subrange same-writer only; outer publication; foreign/partial/overflow/GPU reject; no Metal/Wine");
} return 0; }
