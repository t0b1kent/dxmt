#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../src/winemetal/unix/wmt_trace_native_objects.h"
#include <assert.h>

static void seed(NSMutableDictionary *objects, NSDictionary *event) {
  NSNumber *identifier = event[@"object"];
  objects[identifier] = [[[NSObject alloc] init] autorelease];
  NSDictionary *definition = wmt_trace_native_definition(event, objects);
  assert(definition);
  wmt_trace_native_definitions(objects)[identifier] = definition;
}

int main(void) {
  @autoreleasepool {
    NSMutableDictionary *objects = [NSMutableDictionary dictionary];
    NSString *failure = nil;
    NSDictionary *depth = @{@"event":@"depth-stencil",@"object":@25,
      @"fields":@{@"depthCompareFunction":@7,@"depthWriteEnabled":@NO}};
    seed(objects,depth);
    assert(wmt_trace_native_construct(depth,nil,objects,&failure));
    assert(!failure);
    NSDictionary *changed = @{@"event":@"depth-stencil",@"object":@25,
      @"fields":@{@"depthCompareFunction":@7,@"depthWriteEnabled":@YES}};
    assert(!wmt_trace_native_construct(changed,nil,objects,&failure));
    assert([failure isEqualToString:@"conflicting or mutable duplicate constructor"]);
    NSDictionary *stencil=@{@"readMask":@4294967295u,@"writeMask":@4294967295u,
        @"stencilCompareFunction":@7,@"stencilFailureOperation":@0,
        @"depthFailureOperation":@0,@"depthStencilPassOperation":@0};
    NSDictionary *masked=@{@"readMask":@255,@"writeMask":@255,
        @"stencilCompareFunction":@7,@"stencilFailureOperation":@0,
        @"depthFailureOperation":@0,@"depthStencilPassOperation":@0};
    NSDictionary *wide=@{@"event":@"depth-stencil",@"object":@26,
        @"fields":@{@"depthCompareFunction":@7,@"depthWriteEnabled":@NO,
            @"frontFaceStencil":stencil,@"backFaceStencil":stencil}};
    seed(objects,wide);
    assert(wmt_trace_native_construct(@{@"event":@"depth-stencil",@"object":@26,
        @"fields":@{@"depthCompareFunction":@7,@"depthWriteEnabled":@NO,
            @"frontFaceStencil":masked,@"backFaceStencil":masked}},nil,objects,&failure));
    NSMutableDictionary *lowbit=[[masked mutableCopy] autorelease];lowbit[@"readMask"]=@254;
    assert(!wmt_trace_native_construct(@{@"event":@"depth-stencil",@"object":@26,
        @"fields":@{@"depthCompareFunction":@7,@"depthWriteEnabled":@NO,
            @"frontFaceStencil":lowbit,@"backFaceStencil":masked}},nil,objects,&failure));
    NSDictionary *sampler = @{@"event":@"sampler",@"object":@40,@"fields":@{@"minFilter":@1}};
    seed(objects,sampler);
    assert(wmt_trace_native_construct(sampler,nil,objects,&failure));
    NSData *data = [@"same AIR bytes" dataUsingEncoding:NSUTF8StringEncoding];
    seed(objects,@{@"event":@"library",@"object":@2,@"fields":@{@"metallib":data}});
    seed(objects,@{@"event":@"library",@"object":@103,@"fields":@{@"metallib":[NSData dataWithData:data]}});
    NSDictionary *function = @{@"event":@"function",@"object":@3,@"fields":@{@"library":@2,@"name":@"clear_texture_1d_uint"}};
    seed(objects,function);
    NSDictionary *alias = @{@"event":@"function",@"object":@3,@"fields":@{@"library":@103,@"name":@"clear_texture_1d_uint"}};
    assert(wmt_trace_native_construct(alias,nil,objects,&failure));
    NSDictionary *other = @{@"event":@"function",@"object":@3,@"fields":@{@"library":@103,@"name":@"other"}};
    assert(!wmt_trace_native_construct(other,nil,objects,&failure));
    seed(objects,@{@"event":@"library",@"object":@104,@"fields":@{@"metallib":[@"different AIR" dataUsingEncoding:NSUTF8StringEncoding]}});
    assert(!wmt_trace_native_construct(@{@"event":@"function",@"object":@3,@"fields":@{@"library":@104,@"name":@"clear_texture_1d_uint"}},nil,objects,&failure));
    NSDictionary *special = @{@"event":@"function-specialized",@"object":@4,@"fields":@{@"library":@2,@"name":@"f",@"constants":@[]}};
    seed(objects,special);
    assert(wmt_trace_native_construct(@{@"event":@"function-specialized",@"object":@4,@"fields":@{@"library":@103,@"name":@"f",@"constants":@[]}},nil,objects,&failure));
    for (NSString *type in @[@"buffer",@"texture",@"texture-view",@"queue",@"command-buffer",@"fence"]) {
      NSDictionary *mutable = @{@"event":type,@"object":@500,@"fields":@{}};
      seed(objects,mutable);
      assert(!wmt_trace_native_construct(mutable,nil,objects,&failure));
      [objects removeObjectForKey:@500];
    }
    for (NSString *type in @[@"render-pipeline",@"compute-pipeline"]) {
      NSDictionary *pipeline = @{@"event":type,@"object":@600,@"fields":@{@"function":@3}};
      seed(objects,pipeline);
      assert(wmt_trace_native_construct(pipeline,nil,objects,&failure));
      [objects removeObjectForKey:@600];
    }
    puts("PASS cached immutable constructor family: depth/sampler/function/specialized/library/pipelines; identical library aliases; conflicting fields/data/names and mutable duplicates rejected; GPU=NOT_ENABLED Wine=NOT_ENABLED");
  }
  return 0;
}
