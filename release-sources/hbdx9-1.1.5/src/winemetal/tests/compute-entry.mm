// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
// CPU-only protocol doubles exercise production reflection/validation. No Metal device or GPU work.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#include "../unix/winemetal_compute_entry.inc"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static unsigned checks, creates;
#define CHECK(x) do { ++checks; assert(x); } while (0)
@interface EntryProbe : NSObject
@property(nonatomic, assign) EntryProbe *device, *pipeline, *reflection, *bufferPointerType, *heap;
@property(nonatomic) NSUInteger size;
@property(nonatomic, retain) NSArray *arguments;
@property(nonatomic) MTLFunctionType functionType;
@property(nonatomic) MTLArgumentType type;
@property(nonatomic) MTLBindingAccess access;
@property(nonatomic) NSUInteger index, arrayLength, bufferAlignment, bufferDataSize, maxTotalThreadsPerThreadgroup, length;
@property(nonatomic, getter=isActive) BOOL active;
@property(nonatomic) BOOL elementIsArgumentBuffer, isDepthTexture, failPipeline, failReflection, throwCreate, throwFormat;
@property(nonatomic) MTLTextureType textureType;
@property(nonatomic) MTLDataType textureDataType;
@property(nonatomic) MTLPixelFormat format;
@property(nonatomic) MTLTextureUsage usage;
@end
@implementation EntryProbe
- (void)doesNotRecognizeSelector:(SEL)selector {
  fprintf(stderr,"CPU double missing selector: %s\n",sel_getName(selector));
  [super doesNotRecognizeSelector:selector];
}
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)descriptor
    options:(MTLPipelineOption)options reflection:(MTLComputePipelineReflection **)out error:(NSError **)error {
  ++creates; CHECK(descriptor.computeFunction != nil);
  CHECK((options & MTLPipelineOptionArgumentInfo) && (options & MTLPipelineOptionBufferTypeInfo));
  *error = nil;
  if (_throwCreate) [NSException raise:@"entry-probe" format:@"create"];
  *out = _failReflection ? nil : (MTLComputePipelineReflection *)_reflection;
  return _failPipeline ? nil : [_pipeline retain];
}
- (MTLPixelFormat)pixelFormat {
  if (_throwFormat) [NSException raise:@"entry-probe" format:@"format"];
  return _format;
}
- (void)dealloc { [_arguments release]; [super dealloc]; }
@end
int main() {
 @autoreleasepool {
  EntryProbe *device=[EntryProbe new], *other=[EntryProbe new], *fn=[EntryProbe new];
  EntryProbe *pso=[EntryProbe new], *reflection=[EntryProbe new];
  EntryProbe *ba=[EntryProbe new], *ta=[EntryProbe new], *ab=[EntryProbe new], *pointer=[EntryProbe new];
  EntryProbe *buffer=[EntryProbe new], *texture=[EntryProbe new], *inactive=[EntryProbe new];
  device.pipeline=pso; device.reflection=reflection;
  fn.device=device; fn.functionType=MTLFunctionTypeKernel; pso.maxTotalThreadsPerThreadgroup=256;
  ba.type=MTLArgumentTypeBuffer; ba.index=1; ba.active=YES; ba.bufferAlignment=16; ba.bufferDataSize=32;
  ta.type=MTLArgumentTypeTexture; ta.index=0; ta.active=YES; ta.textureType=MTLTextureType2D;
  ta.textureDataType=MTLDataTypeFloat; ta.access=MTLBindingAccessWriteOnly;
  ab.type=MTLArgumentTypeBuffer; ab.index=29; ab.active=YES; ab.bufferAlignment=16;
  ab.bufferDataSize=64; ab.bufferPointerType=pointer; pointer.elementIsArgumentBuffer=YES;
  reflection.arguments=@[ba,ta,ab,inactive];
  WMTComputeBindingLayout layout{};
  unixcall_mtl_reflected_compute p{};
  p.device=(obj_handle_t)device; p.function=(obj_handle_t)fn; p.layout.ptr=&layout;
  auto run=[&](WMTArgumentStatus want) {
    memset(&layout,0xCD,sizeof(layout)); p.ret=~uint64_t(0);
    auto before=pso.retainCount, beforeFn=fn.retainCount;
    CHECK(_MTLDevice_newReflectedComputePipeline(&p)==STATUS_SUCCESS);
    if(p.status!=want) fprintf(stderr,"reflection case checks=%u creates=%u got=%llu want=%llu\n",
        checks,creates,(unsigned long long)p.status,(unsigned long long)want);
    CHECK(p.status==want); CHECK(fn.retainCount==beforeFn);
    if(want==WMTArgumentStatusReady) { CHECK(p.ret==(obj_handle_t)pso); [(id)p.ret release]; }
    else { CHECK(!p.ret); WMTComputeBindingLayout zero{}; CHECK(!memcmp(&layout,&zero,sizeof(layout))); }
    CHECK(pso.retainCount==before);
  };
  run(WMTArgumentStatusReady);
  CHECK(layout.count==3 && layout.max_threads==256);
  CHECK(layout.bindings[0].kind==WMTComputeBindingBuffer && layout.bindings[0].minimum_size==32);
  CHECK(layout.bindings[1].kind==WMTComputeBindingTexture && layout.bindings[1].access==MTLBindingAccessWriteOnly);
  CHECK(layout.bindings[2].kind==WMTComputeBindingArgumentBuffer);
  const auto valid=layout;
  fn.device=other; run(WMTArgumentStatusInvalid); fn.device=device;
  fn.functionType=MTLFunctionTypeVertex; run(WMTArgumentStatusInvalid); fn.functionType=MTLFunctionTypeKernel;
  device.failPipeline=YES; run(WMTArgumentStatusFailed); device.failPipeline=NO;
  device.failReflection=YES; run(WMTArgumentStatusFailed); device.failReflection=NO;
  device.throwCreate=YES; run(WMTArgumentStatusFailed); device.throwCreate=NO;
  pso.maxTotalThreadsPerThreadgroup=0; run(WMTArgumentStatusUnsupported); pso.maxTotalThreadsPerThreadgroup=256;
  ba.index=31; run(WMTArgumentStatusInvalid); ba.index=1;
  ta.index=128; run(WMTArgumentStatusInvalid); ta.index=0;
  ba.bufferAlignment=0; run(WMTArgumentStatusInvalid); ba.bufferAlignment=16;
  ba.arrayLength=2; run(WMTArgumentStatusUnsupported); ba.arrayLength=1;
  ba.access=(MTLBindingAccess)3; run(WMTArgumentStatusUnsupported); ba.access=MTLBindingAccessReadOnly;
  ab.index=1; run(WMTArgumentStatusInvalid); ab.index=29;
  for(auto kind:{MTLArgumentTypeSampler,MTLArgumentTypeThreadgroupMemory,MTLArgumentTypeIntersectionFunctionTable}) {
    ba.type=kind; run(WMTArgumentStatusUnsupported);
  }
  ba.type=MTLArgumentTypeBuffer;
  EntryProbe *scene=[EntryProbe new], *sceneArgument=[EntryProbe new];
  scene.device=device; scene.size=1024;
  sceneArgument.active=YES; sceneArgument.index=6;
  for(auto type:{MTLArgumentTypePrimitiveAccelerationStructure,MTLArgumentTypeInstanceAccelerationStructure}) {
    sceneArgument.type=type;
    reflection.arguments=@[ba,ta,ab,sceneArgument];
    run(WMTArgumentStatusReady);
    CHECK(layout.count==4 && layout.bindings[3].kind ==
      (type==MTLArgumentTypePrimitiveAccelerationStructure ? WMTComputeBindingPrimitiveAccelerationStructure :
                                                          WMTComputeBindingInstanceAccelerationStructure));
    CHECK(!layout.bindings[3].alignment && !layout.bindings[3].minimum_size);
    sceneArgument.access=MTLBindingAccessWriteOnly; run(WMTArgumentStatusInvalid); sceneArgument.access=MTLBindingAccessReadOnly;
    sceneArgument.index=29; run(WMTArgumentStatusInvalid); sceneArgument.index=6;
    sceneArgument.arrayLength=2; run(WMTArgumentStatusUnsupported); sceneArgument.arrayLength=1;
    sceneArgument.index=31; run(WMTArgumentStatusInvalid); sceneArgument.index=6;
  }
  reflection.arguments=@[ba,ta,ab,inactive];
  reflection.arguments=nil; run(WMTArgumentStatusUnsupported); reflection.arguments=@[ba,ta,ab,inactive];
  // Same numeric slot is legal in different resource namespaces.
  ta.index=1; run(WMTArgumentStatusReady); ta.index=0;
  reflection.arguments=@[]; run(WMTArgumentStatusReady); CHECK(!layout.count);
  reflection.arguments=@[ba,ta,ab,inactive];
  const auto oldCreates=creates;
  for(unsigned i=0;i<1000;++i) run(WMTArgumentStatusReady);
  CHECK(creates==oldCreates+1000);
  p.layout.ptr=nullptr; CHECK(_MTLDevice_newReflectedComputePipeline(&p)==0 && !p.ret);
  p.layout.ptr=&layout;

  buffer.device=device; buffer.length=128;
  texture.device=device; texture.textureType=MTLTextureType2D; texture.format=MTLPixelFormatRGBA8Unorm;
  texture.usage=MTLTextureUsageShaderWrite;
  WMTComputeBinding req[]={valid.bindings[0],valid.bindings[1]};
  WMTArgumentBinding binding[]={{1,WMTArgumentKindBuffer,(obj_handle_t)buffer,16,32},
      {0,WMTArgumentKindTexture,(obj_handle_t)texture,0,0}};
  unixcall_mtl_validate_compute v{};
  v.device=(obj_handle_t)device; v.requirements.ptr=req; v.bindings.ptr=binding; v.count=2;
  auto validate=[&](WMTArgumentStatus want) {
    v.status=WMTArgumentStatusReady;
    CHECK(_MTLDevice_validateComputeBindings(&v)==0); CHECK(v.status==want);
  };
  validate(WMTArgumentStatusReady);
  binding[0].length=31; validate(WMTArgumentStatusInvalid); binding[0].length=32;
  binding[0].offset=1; validate(WMTArgumentStatusInvalid);
  binding[0].offset=UINT64_MAX; validate(WMTArgumentStatusInvalid); binding[0].offset=16;
  binding[0].length=UINT64_MAX; validate(WMTArgumentStatusInvalid); binding[0].length=32;
  binding[0].resource=0; validate(WMTArgumentStatusInvalid); binding[0].resource=(obj_handle_t)buffer;
  binding[0].index=2; validate(WMTArgumentStatusInvalid); binding[0].index=1;
  req[0].alignment=0; validate(WMTArgumentStatusInvalid); req[0].alignment=16;
  req[0].index=31; binding[0].index=31; validate(WMTArgumentStatusInvalid); req[0].index=binding[0].index=1;
  req[0].kind=WMTComputeBindingArgumentBuffer; validate(WMTArgumentStatusUnsupported); req[0].kind=WMTComputeBindingBuffer;
  buffer.device=other; validate(WMTArgumentStatusInvalid); buffer.device=device;
  binding[0].kind=WMTArgumentKindSampler; validate(WMTArgumentStatusInvalid); binding[0].kind=WMTArgumentKindBuffer;
  texture.device=other; validate(WMTArgumentStatusInvalid); texture.device=device;
  texture.textureType=MTLTextureTypeCube; validate(WMTArgumentStatusInvalid); texture.textureType=MTLTextureType2D;
  binding[1].offset=1; validate(WMTArgumentStatusInvalid); binding[1].offset=0;
  binding[1].length=1; validate(WMTArgumentStatusInvalid); binding[1].length=0;
  texture.usage=MTLTextureUsageShaderRead; validate(WMTArgumentStatusInvalid);
  texture.usage=MTLTextureUsageUnknown; validate(WMTArgumentStatusReady);
  texture.usage=MTLTextureUsageShaderWrite;
  req[1].access=MTLBindingAccessReadWrite; validate(WMTArgumentStatusInvalid);
  texture.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite; validate(WMTArgumentStatusReady);
  req[1].access=MTLBindingAccessWriteOnly;
  texture.format=MTLPixelFormatR32Uint; validate(WMTArgumentStatusInvalid);
  req[1].texture_data_type=MTLDataTypeUInt; validate(WMTArgumentStatusReady);
  texture.format=MTLPixelFormatRGBA16Sint; validate(WMTArgumentStatusInvalid);
  req[1].texture_data_type=MTLDataTypeInt; validate(WMTArgumentStatusReady);
  texture.format=MTLPixelFormatDepth32Float; req[1].texture_data_type=MTLDataTypeFloat;
  validate(WMTArgumentStatusInvalid); req[1].depth=1; validate(WMTArgumentStatusReady);
  req[1].depth=0; texture.format=MTLPixelFormatRGBA8Unorm;
  req[1].texture_data_type=MTLDataTypeHalf; validate(WMTArgumentStatusReady);
  req[1].texture_data_type=MTLDataTypeStruct; validate(WMTArgumentStatusUnsupported);
  req[1].texture_data_type=MTLDataTypeFloat;
  texture.format=MTLPixelFormatInvalid; validate(WMTArgumentStatusUnsupported);
  texture.format=MTLPixelFormatRGBA8Unorm; texture.throwFormat=YES; validate(WMTArgumentStatusFailed);
  texture.throwFormat=NO;
  v.count=160; validate(WMTArgumentStatusInvalid); v.count=2;
  v.bindings.ptr=nullptr; validate(WMTArgumentStatusInvalid); v.bindings.ptr=binding;
  v.requirements.ptr=nullptr; validate(WMTArgumentStatusInvalid); v.requirements.ptr=req;
  v.count=0; validate(WMTArgumentStatusReady); v.count=2;
  for(unsigned i=0;i<10000;++i) { binding[0].offset=(i%7)*16; validate(WMTArgumentStatusReady); }
  WMTComputeBinding sr{}; sr.index=6;
  WMTArgumentBinding sb{6,WMTArgumentKindPrimitiveAccelerationStructure,(obj_handle_t)scene,0,0};
  v.requirements.ptr=&sr; v.bindings.ptr=&sb; v.count=1;
  for(auto kind:{WMTComputeBindingPrimitiveAccelerationStructure,WMTComputeBindingInstanceAccelerationStructure}) {
    sr.kind=kind; sb.kind=static_cast<WMTArgumentKind>(kind);
    validate(WMTArgumentStatusReady);
    sb.kind=WMTArgumentKindBuffer; validate(WMTArgumentStatusInvalid); sb.kind=static_cast<WMTArgumentKind>(kind);
    sr.access=MTLBindingAccessReadWrite; validate(WMTArgumentStatusInvalid); sr.access=0;
    sr.alignment=16; validate(WMTArgumentStatusInvalid); sr.alignment=0;
    sr.minimum_size=4; validate(WMTArgumentStatusInvalid); sr.minimum_size=0;
    sr.texture_type=2; validate(WMTArgumentStatusInvalid); sr.texture_type=0;
    sr.texture_data_type=3; validate(WMTArgumentStatusInvalid); sr.texture_data_type=0;
    sr.depth=1; validate(WMTArgumentStatusInvalid); sr.depth=0;
    sb.offset=1; validate(WMTArgumentStatusInvalid); sb.offset=0;
    sb.length=4; validate(WMTArgumentStatusInvalid); sb.length=0;
    scene.device=other; validate(WMTArgumentStatusInvalid); scene.device=device;
    scene.size=0; validate(WMTArgumentStatusInvalid); scene.size=1024;
    scene.heap=other; validate(WMTArgumentStatusInvalid); scene.heap=nil;
    sb.resource=0; validate(WMTArgumentStatusInvalid); sb.resource=(obj_handle_t)scene;
    sr.index=sb.index=31; validate(WMTArgumentStatusInvalid); sr.index=sb.index=6;
    for(unsigned i=0;i<1000;++i) validate(WMTArgumentStatusReady);
  }
  [scene release]; [sceneArgument release];
  for(EntryProbe *o in @[device,other,fn,pso,reflection,ba,ta,ab,pointer,buffer,texture,inactive]) [o release];
 }
 printf("entry_cpu_checks=%u reflection_replays=1000 binding_replays=10000 GPU=0\n",checks);
}
