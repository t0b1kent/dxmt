// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
// CPU-only protocol doubles exercise the production bridge. No Metal device or GPU commands.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#include "../unix/winemetal_arguments.inc"

// Doubles mirror the function-reflection API used by the production bridge.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static unsigned checks, liveBuffers, allocations, bufferWrites, textureWrites, samplerWrites;
#define CHECK(x) do { ++checks; assert(x); } while (0)
@interface Probe : NSObject
@property(nonatomic, assign) Probe *device, *encoder, *reflection, *bufferStructType, *pointerType, *textureReferenceType;
@property(nonatomic, retain) NSArray *members;
@property(nonatomic) MTLFunctionType functionType;
@property(nonatomic) MTLArgumentType type;
@property(nonatomic) MTLDataType dataType, bufferDataType;
@property(nonatomic) NSUInteger index, argumentIndex, length, encodedLength, alignment, dataSize;
@property(nonatomic) BOOL elementIsArgumentBuffer, failAllocation, failEncoder, throwCreate, throwWrite, ownsMemory;
@property(nonatomic) MTLTextureType textureType;
@property(nonatomic) MTLTextureUsage usage;
@property(nonatomic) MTLBindingAccess access;
@property(nonatomic) uint64_t gpuAddress;
@property(nonatomic) MTLResourceID gpuResourceID;
@property(nonatomic) void *contents;
@end
@implementation Probe
- (id)newArgumentEncoderWithBufferIndex:(NSUInteger)index reflection:(MTLArgument **)out {
  if (_throwCreate) [NSException raise:@"probe" format:@"create"];
  if (_failEncoder) return nil;
  *out = (MTLArgument *)_reflection;
  return [_encoder retain];
}
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
  (void)options;
  ++allocations;
  if (_failAllocation) return nil;
  Probe *b = [Probe new];
  b.device = self; b.length = length; b.gpuAddress = 0x100000;
  b.contents = calloc(1, length); b.ownsMemory = YES;
  ++liveBuffers;
  return b;
}
- (void)setArgumentBuffer:(id)buffer offset:(NSUInteger)offset { (void)buffer; assert(offset == 0); }
- (void)setBuffer:(id)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
  (void)buffer; (void)offset; (void)index; ++bufferWrites;
  if (_throwWrite) [NSException raise:@"probe" format:@"write"];
}
- (void)setTexture:(id)texture atIndex:(NSUInteger)index { (void)texture; (void)index; ++textureWrites; }
- (void)setSamplerState:(id)sampler atIndex:(NSUInteger)index { (void)sampler; (void)index; ++samplerWrites; }
- (void)dealloc {
  if (_ownsMemory) { free(_contents); --liveBuffers; }
  [_members release];
  [super dealloc];
}
@end

int main() {
 @autoreleasepool {
  Probe *device = [Probe new], *other = [Probe new], *function = [Probe new], *encoder = [Probe new];
  Probe *reflection = [Probe new], *structure = [Probe new], *pointer = [Probe new], *textureType = [Probe new];
  Probe *bm = [Probe new], *tm = [Probe new], *sm = [Probe new];
  Probe *buffer = [Probe new], *texture = [Probe new], *sampler = [Probe new];
  function.device=device; function.functionType=MTLFunctionTypeKernel;
  function.encoder=encoder; function.reflection=reflection;
  encoder.encodedLength=256; encoder.alignment=16;
  reflection.type=MTLArgumentTypeBuffer; reflection.index=29; reflection.bufferDataType=MTLDataTypeStruct;
  reflection.bufferStructType=structure;
  bm.dataType=MTLDataTypePointer; bm.argumentIndex=3; bm.pointerType=pointer;
  tm.dataType=MTLDataTypeTexture; tm.argumentIndex=7; tm.textureReferenceType=textureType;
  sm.dataType=MTLDataTypeSampler; sm.argumentIndex=9;
  structure.members=@[bm,tm,sm]; pointer.alignment=16; pointer.dataSize=32;
  textureType.textureType=MTLTextureType2D; textureType.access=MTLBindingAccessReadOnly;
  buffer.device=device; buffer.length=1024;
  texture.device=device; texture.textureType=MTLTextureType2D; texture.usage=MTLTextureUsageShaderRead;
  sampler.device=device; sampler.gpuResourceID=(MTLResourceID){1};
  WMTArgumentBinding bindings[]={{3,WMTArgumentKindBuffer,(obj_handle_t)buffer,16,32},
      {7,WMTArgumentKindTexture,(obj_handle_t)texture,0,0},{9,WMTArgumentKindSampler,(obj_handle_t)sampler,0,0}};
  WMTBufferInfo info{};
  unixcall_mtlfunction_newargumentbuffer p{};
  p.function=(obj_handle_t)function; p.index=29; p.bindings.ptr=bindings; p.count=3; p.info.ptr=&info;
  auto run=[&](WMTArgumentStatus want) {
    memset(&info,0xCD,sizeof(info)); p.ret=~uint64_t(0);
    auto before=encoder.retainCount;
    CHECK(_MTLFunction_newArgumentBuffer(&p)==STATUS_SUCCESS);
    CHECK(p.status==want); CHECK(encoder.retainCount==before);
    if (want==WMTArgumentStatusReady) {
      CHECK(p.ret); CHECK(info.length==256); CHECK(!info.memory.ptr);
      CHECK(info.gpu_address==0x100000); CHECK(liveBuffers==1);
      [(id)p.ret release]; p.ret=0;
    } else {
      CHECK(!p.ret); WMTBufferInfo zero{}; CHECK(memcmp(&zero,&info,sizeof(info))==0);
    }
    CHECK(liveBuffers==0);
  };
  run(WMTArgumentStatusReady);
  CHECK(bufferWrites==1 && textureWrites==1 && samplerWrites==1);
  bindings[1].index=3; run(WMTArgumentStatusInvalid); bindings[1].index=7;
  bindings[2].index=42; run(WMTArgumentStatusInvalid); bindings[2].index=9;
  bindings[0].offset=1; run(WMTArgumentStatusInvalid); bindings[0].offset=16;
  bindings[0].length=31; run(WMTArgumentStatusInvalid);
  bindings[0].length=1009; run(WMTArgumentStatusInvalid);
  bindings[0].length=~uint64_t(0); run(WMTArgumentStatusInvalid); bindings[0].length=32;
  bindings[0].offset=~uint64_t(0); run(WMTArgumentStatusInvalid); bindings[0].offset=16;
  bindings[0].resource=0; run(WMTArgumentStatusInvalid); bindings[0].resource=(obj_handle_t)buffer;
  bindings[0].kind=WMTArgumentKindTexture; run(WMTArgumentStatusInvalid); bindings[0].kind=WMTArgumentKindBuffer;
  buffer.device=other; run(WMTArgumentStatusInvalid); buffer.device=device;
  pointer.elementIsArgumentBuffer=YES; run(WMTArgumentStatusUnsupported); pointer.elementIsArgumentBuffer=NO;
  pointer.alignment=0; run(WMTArgumentStatusInvalid); pointer.alignment=16;
  tm.dataType=MTLDataTypeArray; run(WMTArgumentStatusUnsupported); tm.dataType=MTLDataTypeTexture;
  texture.textureType=MTLTextureTypeCube; run(WMTArgumentStatusInvalid); texture.textureType=MTLTextureType2D;
  texture.usage=MTLTextureUsageShaderWrite; run(WMTArgumentStatusInvalid);
  texture.usage=MTLTextureUsageUnknown; run(WMTArgumentStatusReady); texture.usage=MTLTextureUsageShaderRead;
  sampler.gpuResourceID=(MTLResourceID){0}; run(WMTArgumentStatusInvalid); sampler.gpuResourceID=(MTLResourceID){1};
  sampler.device=other; run(WMTArgumentStatusInvalid); sampler.device=device;
  bindings[2].offset=1; run(WMTArgumentStatusInvalid); bindings[2].offset=0;
  p.count=2; run(WMTArgumentStatusInvalid); p.count=257; run(WMTArgumentStatusInvalid); p.count=3;
  p.index=31; run(WMTArgumentStatusInvalid); p.index=28; run(WMTArgumentStatusUnsupported); p.index=29;
  function.functionType=MTLFunctionTypeVertex; run(WMTArgumentStatusUnsupported); function.functionType=MTLFunctionTypeKernel;
  function.failEncoder=YES; run(WMTArgumentStatusUnsupported); function.failEncoder=NO;
  encoder.encodedLength=17*1024*1024; run(WMTArgumentStatusUnsupported); encoder.encodedLength=256;
  encoder.alignment=3; run(WMTArgumentStatusUnsupported); encoder.alignment=16;
  device.failAllocation=YES; run(WMTArgumentStatusOutOfMemory); device.failAllocation=NO;
  function.throwCreate=YES; run(WMTArgumentStatusFailed); function.throwCreate=NO;
  encoder.throwWrite=YES; run(WMTArgumentStatusFailed); encoder.throwWrite=NO;
  // Each replay must build new storage and encode every resource again.
  const auto old=allocations;
  for(unsigned i=0;i<10000;++i) { bindings[0].offset=(i%32)*16; run(WMTArgumentStatusReady); }
  CHECK(allocations==old+10000);
  p.info.ptr=nullptr; CHECK(_MTLFunction_newArgumentBuffer(&p)==0); CHECK(!p.ret);
  for(Probe *o in @[device,other,function,encoder,reflection,structure,pointer,textureType,bm,tm,sm,buffer,texture,sampler])
    [o release];
 }
 CHECK(liveBuffers==0);
 printf("CPU bridge checks=%u fresh_replays=10000 GPU=0\n",checks);
}
