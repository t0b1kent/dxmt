#include "../src/winemetal/unix/wmt_trace_cb.h"
#include "../src/winemetal/unix/wmt_trace_snapshot.h"
#include <assert.h>
#include <stdio.h>

@interface RangeBuffer : NSObject { NSMutableData *memory; }
- (id)initWithLength:(NSUInteger)length;
- (NSUInteger)length;
- (NSUInteger)storageMode;
- (void *)contents;
@end
@implementation RangeBuffer
- (id)initWithLength:(NSUInteger)length { self=[super init]; memory=[[NSMutableData alloc] initWithLength:length]; return self; }
- (NSUInteger)length { return memory.length; }
- (NSUInteger)storageMode { return 0; }
- (void *)contents { return memory.mutableBytes; }
- (void)dealloc { [memory release]; [super dealloc]; }
@end

int main(void) {
 @autoreleasepool {
  id source=[[RangeBuffer alloc] initWithLength:33554432];
  id destination=[[RangeBuffer alloc] initWithLength:33554432];
  id texture=[NSObject new], encoder=[NSObject new], cb=[NSObject new];
  uint64_t sid=wmt_trace_register_resource(source,1,0x10000000,[source length],0);
  uint64_t did=wmt_trace_register_resource(destination,1,0x20000000,[destination length],0);
  assert(sid && did && wmt_trace_register_resource(texture,2,0,0,99));
  assert(wmt_trace_owner_change(source,0,[source length],WMT_OWNER_BEGIN));
  assert(wmt_trace_owner_change(source,0,[source length],WMT_OWNER_END));
  memset([source contents],0x11,[source length]);
  wmt_trace_cb_associate(encoder,cb);
  struct wmtcmd_blit_copy_from_buffer_to_buffer a={0};
  a.type=WMTBlitCommandCopyFromBufferToBuffer;
  a.src=(obj_handle_t)source; a.dst=(obj_handle_t)destination;
  a.src_offset=1024; a.copy_length=256;
  wmt_trace_cb_command_writes(1,encoder,(const void *)&a);
  /* Overlapping explicit spans must be unioned, not charged twice. */
  a.src_offset=1152;
  wmt_trace_cb_command_writes(1,encoder,(const void *)&a);
  struct wmtcmd_blit_copy_from_buffer_to_texture b={0};
  b.type=WMTBlitCommandCopyFromBufferToTexture; b.src=(obj_handle_t)source;
  b.src_offset=4096; b.dst=(obj_handle_t)texture;
  b.bytes_per_row=64; b.bytes_per_image=256; b.size=(struct WMTSize){16,4,1};
  wmt_trace_cb_command_writes(1,encoder,(const void *)&b);
  struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption c={0};
  c.type=WMTBlitCommandCopyFromBufferToTextureWithBlitOption;
  c.src=(obj_handle_t)source; c.src_offset=8192; c.dst=(obj_handle_t)texture;
  c.bytes_per_row=64; c.size=(struct WMTSize){16,4,1};
  wmt_trace_cb_command_writes(1,encoder,(const void *)&c);
  NSDictionary *state=wmt_trace_cb_copy(cb);
  assert([state[@"reads"] count]==1 && state[@"reads"][@(did)]==nil);
  NSSet *selected=[NSSet setWithArray:[state[@"reads"] allKeys]];
  NSString *failure=nil;
  assert(!wmt_trace_copy_selected_owned_inputs(1024,selected,&failure));
  NSDictionary *snapshot=wmt_trace_copy_ranged_owned_inputs(896,selected,state[@"reads"],&failure);
  assert(snapshot && !failure && [snapshot[@"raw-bytes"] unsignedLongLongValue]==896);
  [snapshot release];
  assert(!wmt_trace_copy_ranged_owned_inputs(895,selected,state[@"reads"],&failure));
  /* A normal writer in an unused staging suballocation is disjoint. */
  assert(wmt_trace_owner_change(source,16384,16,WMT_OWNER_BEGIN));
  snapshot=wmt_trace_copy_ranged_owned_inputs(896,selected,state[@"reads"],&failure);
  assert(snapshot && !failure); [snapshot release];
  assert(wmt_trace_owner_change(source,16384,16,WMT_OWNER_END));
  /* Active NO_OVERWRITE uses pre-Map bytes, not concurrently modified memory. */
  assert(wmt_trace_owner_change(source,0,[source length],WMT_OWNER_NO_OVERWRITE_BEGIN));
  memset([source contents],0x22,[source length]);
  snapshot=wmt_trace_copy_ranged_owned_inputs(896,selected,state[@"reads"],&failure);
  assert(snapshot && !failure);
  for (NSDictionary *part in snapshot[@"snapshots"]) {
    NSData *bytes=part[@"bytes"]; assert(((const uint8_t *)bytes.bytes)[0]==0x11);
    assert([part[@"origin"] isEqual:@"immutable-pre-NO_OVERWRITE-Map"]);
  }
  [snapshot release];
  assert(wmt_trace_owner_change(source,0,[source length],WMT_OWNER_END));
  snapshot=wmt_trace_copy_ranged_owned_inputs(896,selected,state[@"reads"],&failure);
  assert(snapshot && !failure);
  for (NSDictionary *part in snapshot[@"snapshots"]) assert(((const uint8_t *)[part[@"bytes"] bytes])[0]==0x22);
  [snapshot release];
  assert(wmt_trace_owner_change(source,1024,16,WMT_OWNER_BEGIN));
  assert(!wmt_trace_copy_ranged_owned_inputs(896,selected,state[@"reads"],&failure));
  [state release]; [source release]; [destination release]; [texture release]; [encoder release]; [cb release];
  puts("PASS blit buffer/buffer, buffer/texture, options sibling, span union, pitch0, write-only exclusion, budget895/896, disjoint writer, immutable NO_OVERWRITE, overlapping ordinary writer rejection; GPU=NOT_ENABLED Wine=NOT_ENABLED");
 }
 return 0;
}
