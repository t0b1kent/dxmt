#import <Foundation/Foundation.h>
#include "../src/winemetal/unix/wmt_trace_snapshot.h"
#include <assert.h>

@interface TraceTestBuffer : NSObject {
  NSMutableData *data;
}
- (NSUInteger)length;
- (NSUInteger)storageMode;
- (void *)contents;
@end
@implementation TraceTestBuffer
- (id)init { if ((self = [super init])) data = [[NSMutableData alloc] initWithLength:32]; return self; }
- (void)dealloc { [data release]; [super dealloc]; }
- (NSUInteger)length { return [data length]; }
- (NSUInteger)storageMode { return 0; }
- (void *)contents { return [data mutableBytes]; }
@end

int main(void) {
  @autoreleasepool {
    TraceTestBuffer *buffer = [[TraceTestBuffer alloc] init];
    NSObject *texture = [[NSObject alloc] init];
    uint64_t buffer_id = wmt_trace_register_resource(buffer, 1, 0x1000, 32, 0);
    uint64_t texture_id = wmt_trace_register_resource(texture, 2, 0, 0, 77);
    uint64_t input[] = {0x1018, 77, 0x1018, 123};
    memcpy([buffer contents], input, sizeof(input));
    assert(wmt_trace_declare_fields(buffer, 0, 1, 8, 1));
    assert(wmt_trace_declare_fields(buffer, 8, 1, 8, 2));
    NSString *error = nil;
    NSArray *snapshot = wmt_trace_copy_snapshots(32, &error);
    assert(snapshot && !error && [snapshot count] == 1);
    NSDictionary *entry = snapshot[0];
    assert([entry[@"object"] unsignedLongLongValue] == buffer_id);
    uint64_t output[4];
    [entry[@"bytes"] getBytes:output length:sizeof(output)];
    assert(!output[0] && !output[1] && output[2] == input[2] && output[3] == 123);
    NSArray *relocations = entry[@"relocations"];
    assert([relocations count] == 2);
    assert([relocations[0][@"object"] unsignedLongLongValue] == buffer_id);
    assert([relocations[0][@"addend"] unsignedLongLongValue] == 24);
    assert([relocations[1][@"object"] unsignedLongLongValue] == texture_id);
    struct wmt_trace_relocation fixups[2];
    for (NSUInteger i = 0; i < 2; ++i) {
      NSDictionary *r = relocations[i];
      fixups[i] = (struct wmt_trace_relocation){
          [r[@"offset"] unsignedLongLongValue], [r[@"object"] unsignedLongLongValue],
          [r[@"addend"] unsignedLongLongValue], [r[@"kind"] unsignedIntValue]};
    }
    struct wmt_trace_resource replay_map[] = {
        {buffer_id, 0x9000, 32, 0, 1}, {texture_id, 0, 0, 99, 2}};
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
      [entry[@"bytes"] getBytes:output length:sizeof(output)];
      assert(wmt_trace_relocate(output, sizeof(output), fixups, 2, replay_map, 2));
      assert(output[0] == 0x9018 && output[1] == 99 &&
             output[2] == 0x1018 && output[3] == 123);
    }
    NSError *serialization_error = nil;
    NSData *disk = [NSPropertyListSerialization dataWithPropertyList:snapshot
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:&serialization_error];
    assert(disk && !serialization_error);
    NSArray *loaded = [NSPropertyListSerialization propertyListWithData:disk
        options:NSPropertyListImmutable format:NULL error:&serialization_error];
    assert(loaded && !serialization_error && [loaded isEqual:snapshot]);
    NSArray *again = wmt_trace_copy_snapshots(32, &error);
    assert([snapshot isEqual:again]);
    [again release];
    assert(!wmt_trace_copy_snapshots(31, &error) && error);
    NSDictionary *owned = wmt_trace_copy_owned_inputs(0, &error);
    assert(owned && !error && [owned[@"snapshots"] count] == 0);
    assert([owned[@"unknown-readable-buffers"] count] == 1);
    [owned release];
    assert(wmt_trace_owner_change(buffer, 0, 16, WMT_OWNER_BEGIN));
    assert(!wmt_trace_copy_owned_inputs(32, &error) && error);
    assert(wmt_trace_owner_change(buffer, 0, 16, WMT_OWNER_END));
    owned = wmt_trace_copy_owned_inputs(16, &error);
    assert(owned && !error && [owned[@"raw-bytes"] unsignedLongLongValue] == 16);
    assert([owned[@"unknown-readable-buffers"] count] == 0);
    assert([owned[@"snapshots"] count] == 1);
    assert([owned[@"snapshots"][0][@"relocations"] count] == 2);
    assert([owned[@"snapshots"][0][@"bytes"] length] == 16);
    assert([owned[@"status"] isEqual:@"INCOMPLETE"]);
    // Unowned scalar bytes are deliberately absent, even though readable.
    assert(!wmt_trace_copy_owned_inputs(15, &error) && error);
    assert(wmt_trace_owner_change(buffer, 8, 8, WMT_OWNER_GPU));
    NSDictionary *partial = wmt_trace_copy_owned_inputs(8, &error);
    assert(partial && !error && [partial[@"raw-bytes"] unsignedLongLongValue] == 8);
    assert([partial[@"snapshots"][0][@"relocations"] count] == 1);
    // The earlier immutable commit payload survives later invalidation.
    assert([owned[@"snapshots"][0][@"bytes"] length] == 16);
    [partial release]; [owned release];
    assert(wmt_trace_owner_change(buffer, 4, 4, WMT_OWNER_GPU));
    assert(!wmt_trace_copy_owned_inputs(32, &error) && error);
    input[1] = 78;
    memcpy([buffer contents], input, sizeof(input));
    assert(!wmt_trace_copy_snapshots(32, &error) && error);
    /* A later failure must not alter the earlier immutable snapshot. */
    assert([entry[@"bytes"] length] == 32);
    [snapshot release];
    [buffer release];
    [texture release];
  }
  puts("PASS native CPU snapshot: typed relocation, scalar preservation, determinism, budget, unresolved rejection, 3 relocated reconstructions, disk roundtrip; owned inputs: UNKNOWN, pending reject, partial GPU invalidation, crossing-field reject, immutable payload");
  return 0;
}
