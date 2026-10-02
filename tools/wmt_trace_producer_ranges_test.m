#include "../src/winemetal/unix/wmt_trace_cb.h"
#include "../src/winemetal/unix/wmt_trace_snapshot.h"
#include <assert.h>
#include <stdio.h>
@interface ProducerRangeBuffer : NSObject { uint8_t memory[4096]; }
- (NSUInteger)length;
- (NSUInteger)storageMode;
- (void *)contents;
@end
@implementation ProducerRangeBuffer
- (NSUInteger)length { return sizeof(memory); }
- (NSUInteger)storageMode { return 0; }
- (void *)contents { return memory; }
@end
int main(void) {
 @autoreleasepool {
  for (unsigned kind=2;kind<=3;++kind) {
   id buffer=[ProducerRangeBuffer new],encoder=[NSObject new],cb=[NSObject new];
   uint64_t bid=wmt_trace_register_resource(buffer,1,0x100000+kind*0x10000,4096,0);
   assert(wmt_trace_owner_change(buffer,0,176,WMT_OWNER_BEGIN));
   assert(wmt_trace_owner_change(buffer,0,176,WMT_OWNER_END));
   assert(wmt_trace_owner_change(buffer,256,176,WMT_OWNER_BEGIN));
   wmt_trace_cb_associate(encoder,cb); wmt_trace_cb_read(encoder,buffer,0,0,YES);
   struct wmtcmd_trace_buffer_read command={0};
   command.type=kind==2?WMTComputeCommandTraceBufferRead:WMTRenderCommandTraceBufferRead;
   command.buffer=(obj_handle_t)buffer;command.offset=0;command.length=176;
   struct wmt_trace_command_schema schema;
   assert(wmt_trace_command_schema(kind,command.type,&schema));
   assert(schema.size==sizeof(command)&&schema.handle_count==1);
   wmt_trace_cb_command_writes(kind,encoder,(const void *)&command);
   /* A later generic residency/bind callback cannot widen a closed producer span. */
   wmt_trace_cb_read(encoder,buffer,0,0,YES);
   NSDictionary *state=wmt_trace_cb_copy(cb);NSString *failure=nil;
   NSDictionary *input=wmt_trace_copy_ranged_owned_inputs(176,[NSSet setWithObject:@(bid)],state[@"reads"],&failure);
   assert(input&&!failure&&[input[@"raw-bytes"] unsignedLongLongValue]==176);[input release];
   [state release];
   id enc2=[NSObject new],cb2=[NSObject new];wmt_trace_cb_associate(enc2,cb2);
   command.offset=256;wmt_trace_cb_command_writes(kind,enc2,(const void *)&command);
   state=wmt_trace_cb_copy(cb2);
   assert(!wmt_trace_copy_ranged_owned_inputs(176,[NSSet setWithObject:@(bid)],state[@"reads"],&failure));
   assert(wmt_trace_owner_change(buffer,256,176,WMT_OWNER_END));
   input=wmt_trace_copy_ranged_owned_inputs(176,[NSSet setWithObject:@(bid)],state[@"reads"],&failure);
   assert(input&&!failure&&[input[@"raw-bytes"] unsignedLongLongValue]==176);[input release];
   [state release];[enc2 release];[cb2 release];[buffer release];[encoder release];[cb release];
  }
  puts("PASS record09 exact4096/176/256/176 fixture; compute/render metadata siblings; residency not widened; pending used range rejected then END admitted; GPU=NOT_ENABLED Wine=NOT_ENABLED");
 }
 return 0;
}
