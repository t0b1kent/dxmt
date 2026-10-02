#include "../src/winemetal/unix/wmt_trace_cb.h"
#include "../src/winemetal/unix/wmt_trace_snapshot.h"
#include <assert.h>
@interface CBTestBuffer : NSObject { unsigned char bytes[128]; }
@end
@implementation CBTestBuffer
- (uint64_t)length { return sizeof(bytes); }
- (NSUInteger)storageMode { return 0; }
- (void *)contents { return bytes; }
@end
int main(void) {
  @autoreleasepool {
    id first = [CBTestBuffer new], second = [CBTestBuffer new], unused = [CBTestBuffer new];
    uint64_t a = wmt_trace_register_resource(first, 1, 0x1000, 128, 0);
    uint64_t b = wmt_trace_register_resource(second, 1, 0x2000, 128, 0);
    assert(wmt_trace_register_resource(unused, 1, 0x3000, 128, 0));
    assert(wmt_trace_owner_change(first, 0, 128, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_change(first, 0, 128, WMT_OWNER_END));
    id cb1 = [NSObject new], cb2 = [NSObject new];
    id enc1 = [NSObject new], enc2 = [NSObject new], enc3 = [NSObject new];
    assert(!wmt_trace_cb_copy(cb1));
    wmt_trace_cb_associate(enc1, cb1); wmt_trace_cb_associate(enc2, cb1);
    wmt_trace_cb_associate(enc3, cb2);
    wmt_trace_cb_use(enc1, first, NO); wmt_trace_cb_use(enc2, second, NO);
    wmt_trace_cb_use(enc3, first, NO);
    struct wmtcmd_blit_copy_from_buffer_to_buffer copy = {0};
    copy.type = WMTBlitCommandCopyFromBufferToBuffer;
    copy.src = (obj_handle_t)first; copy.dst = (obj_handle_t)second; copy.copy_length = 16;
    wmt_trace_cb_command_writes(1, enc1, (const void *)&copy);
    NSDictionary *one = wmt_trace_cb_copy(cb1), *two = wmt_trace_cb_copy(cb2);
    assert([one[@"buffers"] count] == 2 && [two[@"buffers"] count] == 1);
    assert(one[@"writes"][@(b)] == second && !one[@"writes"][@(a)]);
    assert([two[@"writes"] count] == 0);
    id cb3 = [NSObject new], enc4 = [NSObject new];
    wmt_trace_cb_associate(enc4, cb3);
    struct wmtcmd_compute_useresource use = {0};
    use.type = WMTComputeCommandUseResource; use.resource = (obj_handle_t)first; use.usage = 1;
    wmt_trace_cb_command_writes(2, enc4, (const void *)&use);
    use.usage = 2; use.resource = (obj_handle_t)second;
    wmt_trace_cb_command_writes(2, enc4, (const void *)&use);
    struct wmtcmd_render_useresource render = {0};
    render.type = WMTRenderCommandUseResource; render.resource = (obj_handle_t)unused; render.usage = 2;
    wmt_trace_cb_command_writes(3, enc4, (const void *)&render);
    NSDictionary *three = wmt_trace_cb_copy(cb3);
    assert(!three[@"writes"][@(a)] && [three[@"writes"] count] == 2);
    [three release]; [enc4 release]; [cb3 release];
    id empty = [NSObject new]; wmt_trace_cb_associate(nil, empty);
    NSDictionary *empty_state = wmt_trace_cb_copy(empty);
    assert(empty_state && [empty_state[@"buffers"] count] == 0 && ![empty_state[@"failure"] length]);
    [empty_state release]; [empty release];
    id tex = [NSObject new], view = [NSObject new], view2 = [NSObject new];
    assert(wmt_trace_register_resource(tex, 2, 0, 0, 101));
    assert(wmt_trace_register_resource(view, 2, 0, 0, 102));
    assert(wmt_trace_register_resource(view2, 2, 0, 0, 103));
    wmt_trace_cb_alias(tex, second, YES);
    wmt_trace_cb_alias(view, tex, NO); wmt_trace_cb_alias(view2, view, NO);
    id alias_cb = [NSObject new], alias_enc = [NSObject new];
    wmt_trace_cb_associate(alias_enc, alias_cb);
    wmt_trace_cb_use(alias_enc, tex, NO);
    wmt_trace_cb_use(alias_enc, view2, YES);
    NSDictionary *aliases = wmt_trace_cb_copy(alias_cb);
    assert([aliases[@"buffers"] count] == 1 && aliases[@"writes"][@(b)] == second);
    assert([aliases[@"resources"] count] == 3);
    [aliases release]; [alias_enc release]; [alias_cb release];
    [view2 release]; [view release]; [tex release];
    NSString *error = nil;
    NSDictionary *inputs = wmt_trace_copy_selected_owned_inputs(128,
        [NSSet setWithArray:[one[@"buffers"] allKeys]], &error);
    assert(inputs && !error && [inputs[@"raw-bytes"] unsignedLongLongValue] == 128);
    assert(([inputs[@"unknown-readable-buffers"] isEqual:@[@(b)]]));
    assert([inputs[@"snapshots"] count] == 1); [inputs release];
    assert(wmt_trace_owner_gpu_begin(second, 0, 128, 1));
    assert(!wmt_trace_owner_change(second, 0, 16, WMT_OWNER_BEGIN));
    assert(wmt_trace_owner_gpu_end(second, 1));
    wmt_trace_cb_use(enc1, unused, NO);
    NSDictionary *duplicate = wmt_trace_cb_copy(cb1);
    assert([duplicate[@"failure"] length]);
    assert([one[@"buffers"] count] == 2); [duplicate release];
    [one release]; [two release];
    [enc1 release]; [enc2 release]; [enc3 release]; [cb1 release]; [cb2 release];
    [first release]; [second release]; [unused release];
    puts("PASS CB: shared encoder state, CB isolation, explicit destination, selected CPU snapshots, unused UNKNOWN excluded, queued write, immutable copy, post-commit reject; no GPU/Wine");
  }
  return 0;
}
