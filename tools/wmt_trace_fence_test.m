#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../src/winemetal/unix/wmt_trace_native_objects.h"
#include "../src/winemetal/unix/wmt_trace_commands.h"
#include <assert.h>

@interface FakeFenceDevice : NSObject
@end
@implementation FakeFenceDevice
- (id)newFence { return [[NSObject alloc] init]; }
@end
@interface FakeFenceEncoder : NSObject { @public NSUInteger ended; }
@end
@implementation FakeFenceEncoder
- (void)endEncoding { ++ended; }
@end
static uint64_t encode(obj_handle_t handle, void *context) { return handle ? 120 : 0; }
static obj_handle_t decode(uint64_t identifier, void *context) {
  return (obj_handle_t)((NSMutableDictionary *)context)[@(identifier)];
}

int main(void) {
  @autoreleasepool {
    NSMutableDictionary *objects = [NSMutableDictionary dictionary];
    FakeFenceDevice *device = [[[FakeFenceDevice alloc] init] autorelease];
    NSString *failure=nil;
    NSDictionary *event=@{@"event":@"fence",@"object":@120,@"fields":@{}};
    assert(wmt_trace_native_construct(event,(id<MTLDevice>)device,objects,&failure));
    assert(!failure && objects[@120]);
    assert(!wmt_trace_native_construct(event,(id<MTLDevice>)device,objects,&failure));
    assert(!wmt_trace_native_construct(@{@"event":@"fence",@"object":@121,@"fields":@{@"bad":@1}},(id<MTLDevice>)device,objects,&failure));
    const unsigned types[3][2]={{WMTBlitCommandWaitForFence,WMTBlitCommandUpdateFence},
      {WMTComputeCommandWaitForFence,WMTComputeCommandUpdateFence},
      {WMTRenderCommandWaitForFence,WMTRenderCommandUpdateFence}};
    for (unsigned kind=1;kind<=3;++kind) for (unsigned sibling=0;sibling<2;++sibling) {
      struct wmt_trace_command_schema schema;
      assert(wmt_trace_command_schema(kind,types[kind-1][sibling],&schema));
      assert(schema.handle_count==1 && schema.element_size==0);
      NSMutableData *body=[NSMutableData dataWithLength:schema.size];
      struct wmtcmd_base *command=body.mutableBytes;
      command->type=types[kind-1][sibling];
      obj_handle_t fence=(obj_handle_t)objects[@120];
      memcpy((char *)command+schema.handles[0],&fence,8);
      NSArray *pack=wmt_trace_pack_commands(kind,command,4096,10,encode,NULL,&failure);
      assert(pack && !failure);
      NSMutableArray *arena=[NSMutableArray array];
      struct wmtcmd_base *restored=wmt_trace_unpack_commands(kind,pack,4096,10,decode,objects,arena,&failure);
      assert(restored && !failure);
      obj_handle_t actual=0;memcpy(&actual,(char *)restored+schema.handles[0],8);
      assert(actual==fence);
      NSMutableDictionary *empty=[NSMutableDictionary dictionary];
      assert(!wmt_trace_unpack_commands(kind,pack,4096,10,decode,empty,[NSMutableArray array],&failure));
      assert([failure isEqualToString:@"missing replay object"]);
    }
    NSMutableSet *active=[NSMutableSet set];
    for (unsigned kind=1;kind<=3;++kind) {
      NSNumber *identifier=@(200+kind);
      FakeFenceEncoder *encoder=[[[FakeFenceEncoder alloc] init] autorelease];
      objects[identifier]=encoder;[active addObject:identifier];
    }
    wmt_trace_native_close_encoders(objects,active);
    assert(active.count==0);
    for (unsigned kind=1;kind<=3;++kind) assert(((FakeFenceEncoder *)objects[@(200+kind)])->ended==1);
    wmt_trace_native_close_encoders(objects,active);
    for (unsigned kind=1;kind<=3;++kind) assert(((FakeFenceEncoder *)objects[@(200+kind)])->ended==1);
    puts("PASS fence constructor + Blit/Compute/Render Wait/Update six siblings, missing generation rejection; owned encoder close exactly once/no commit; mock device only GPU=NOT_ENABLED Wine=NOT_ENABLED");
  }
  return 0;
}
