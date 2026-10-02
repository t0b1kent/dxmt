#import <Foundation/Foundation.h>
#include "../src/winemetal/unix/wmt_trace_registry.h"
#include <assert.h>

int main(void) {
  @autoreleasepool {
    NSObject *a = [[NSObject alloc] init];
    NSObject *b = [[NSObject alloc] init];
    uint64_t first = wmt_trace_register_resource(a, 1, 0x1000, 256, 0);
    assert(first && wmt_trace_register_resource(a, 1, 0x1000, 256, 0) == first);
    assert(!wmt_trace_register_resource(a, 1, 0x2000, 256, 0));
    uint64_t second = wmt_trace_register_resource(b, 2, 0, 0, 77);
    assert(second > first);
    NSArray *owners = nil;
    NSData *entries = wmt_trace_copy_resources(&owners);
    assert([owners count] == 2 && [entries length] == 2 * sizeof(struct wmt_trace_resource));
    struct wmt_trace_relocation r;
    assert(wmt_trace_resolve(8, 1, 0x1018, [entries bytes], 2, &r));
    assert(r.object_id == first && r.addend == 24);
    [entries release];
    [owners release];
    [a release];
    [b release];
  }
  @autoreleasepool {
    NSArray *owners = nil;
    NSData *entries = wmt_trace_copy_resources(&owners);
    assert(![owners count] && ![entries length]);
    [entries release];
    [owners release];
    NSObject *c = [[NSObject alloc] init];
    assert(wmt_trace_register_resource(c, 1, 0x1000, 256, 0) > 2);
    [c release];
  }
  puts("PASS native registry: identity, conflict, lifetime, generation, relocation");
  return 0;
}
