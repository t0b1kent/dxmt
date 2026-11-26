// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
// CPU protocol doubles exercise production AS descriptor/query/encode/retention paths.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstddef>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#include "../unix/winemetal_acceleration.inc"
static unsigned checks, sizesQueries, builds, live;
#define CHECK(x) do{++checks;assert(x);}while(0)
static_assert(sizeof(WMTASUserIDInstance)==sizeof(MTLAccelerationStructureUserIDInstanceDescriptor));
static_assert(offsetof(WMTASUserIDInstance,options)==offsetof(MTLAccelerationStructureUserIDInstanceDescriptor,options));
static_assert(offsetof(WMTASUserIDInstance,user_id)==offsetof(MTLAccelerationStructureUserIDInstanceDescriptor,userID));
@interface ASProbe : NSObject
@property(nonatomic,assign) ASProbe *device,*encoder,*heap;
@property(nonatomic) BOOL supportsRaytracing,failSizes,failNew,failEncoder,throwSizes,throwBuild;
@property(nonatomic) NSUInteger length,size;
@property(nonatomic) MTLCommandBufferStatus status;
@property(nonatomic,retain) NSMutableArray *handlers,*events;
@property(nonatomic,retain) MTLAccelerationStructureDescriptor *seen;
@end
@implementation ASProbe
- (instancetype)init {self=[super init];if(self){++live;_handlers=[NSMutableArray new];_events=[NSMutableArray new];}return self;}
- (MTLAccelerationStructureSizes)accelerationStructureSizesWithDescriptor:(MTLAccelerationStructureDescriptor*)descriptor {
 ++sizesQueries;self.seen=descriptor;
 if(_throwSizes)[NSException raise:@"as-probe" format:@"query"];
 return _failSizes ? (MTLAccelerationStructureSizes){0,0,0} : (MTLAccelerationStructureSizes){1024,512,256};
}
- (id)newAccelerationStructureWithSize:(NSUInteger)size {
 if(_failNew)return nil;
 ASProbe *p=[ASProbe new];p.device=self;p.size=size;return p;
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {id copy=[handler copy];[_handlers addObject:copy];[copy release];}
- (id)accelerationStructureCommandEncoder {[_events addObject:@"encoder"];return _failEncoder?nil:_encoder;}
- (void)waitForFence:(id)fence {(void)fence;[_events addObject:@"wait"];}
- (void)updateFence:(id)fence {(void)fence;[_events addObject:@"update"];}
- (void)endEncoding {[_events addObject:@"end"];}
- (void)buildAccelerationStructure:(id)target descriptor:(MTLAccelerationStructureDescriptor*)descriptor
 scratchBuffer:(id)scratch scratchBufferOffset:(NSUInteger)offset {
 (void)target;(void)descriptor;(void)scratch;CHECK(offset%256==0);++builds;[_events addObject:@"build"];
 if(_throwBuild)[NSException raise:@"as-probe" format:@"build"];
}
- (void)dealloc {[_seen release];[_handlers release];[_events release];--live;[super dealloc];}
@end
int main() {
 @autoreleasepool {
  ASProbe *device=[ASProbe new],*other=[ASProbe new],*vertices=[ASProbe new],*indices=[ASProbe new];
  ASProbe *target=[ASProbe new],*scratch=[ASProbe new],*fence=[ASProbe new],*cmd=[ASProbe new],*encoder=[ASProbe new];
  ASProbe *instances=[ASProbe new],*child=[ASProbe new];
  device.supportsRaytracing=YES;
  for(ASProbe *p in @[vertices,indices,target,scratch,fence,cmd,instances,child])p.device=device;
  vertices.length=36;indices.length=12;target.size=1024;scratch.length=1024;
  instances.length=136;child.size=1024;cmd.encoder=encoder;
  WMTASTriangleGeometry g{};g.vertex_buffer=(obj_handle_t)vertices;g.vertex_stride=12;g.vertex_count=3;g.triangle_count=1;
  WMTASBuildDesc desc{};desc.geometries.ptr=&g;desc.geometry_count=1;
  WMTASSizeInfo info{};
  unixcall_mtl_as_sizes query{};query.device=(obj_handle_t)device;query.desc.ptr=&desc;query.info.ptr=&info;
  auto run=[&](WMTArgumentStatus status) {
    memset(&info,0xcd,sizeof(info));CHECK(_MTLDevice_accelerationStructureSizes(&query)==0);CHECK(query.status==status);
    if(status==WMTArgumentStatusReady)CHECK(info.structure_size==1024&&info.build_scratch_size==512&&info.refit_scratch_size==256);
    else {WMTASSizeInfo zero{};CHECK(!memcmp(&info,&zero,sizeof(info)));}
    device.seen=nil;
  };
  run(WMTArgumentStatusReady);
  device.supportsRaytracing=NO;run(WMTArgumentStatusUnsupported);device.supportsRaytracing=YES;
  desc.flags=1;run(WMTArgumentStatusUnsupported);desc.flags=0;
  desc.geometry_count=4097;run(WMTArgumentStatusInvalid);desc.geometry_count=1;
  desc.geometries.ptr=nullptr;run(WMTArgumentStatusInvalid);desc.geometries.ptr=&g;
  desc.instance_buffer=(obj_handle_t)instances;run(WMTArgumentStatusInvalid);desc.instance_buffer=0;
  g.vertex_offset=4;run(WMTArgumentStatusInvalid);g.vertex_offset=0;
  g.vertex_stride=13;run(WMTArgumentStatusInvalid);g.vertex_stride=12;
  g.vertex_count=2;run(WMTArgumentStatusInvalid);g.vertex_count=3;
  g.triangle_count=UINT64_MAX;run(WMTArgumentStatusInvalid);g.triangle_count=1;
  g.vertex_count=UINT64_MAX;run(WMTArgumentStatusInvalid);g.vertex_count=3;
  vertices.length=35;run(WMTArgumentStatusInvalid);vertices.length=36;
  vertices.device=other;run(WMTArgumentStatusInvalid);vertices.device=device;
  vertices.heap=other;run(WMTArgumentStatusInvalid);vertices.heap=nil;
  g.opaque=2;run(WMTArgumentStatusInvalid);g.opaque=1;run(WMTArgumentStatusReady);g.opaque=0;
  g.intersection_function_offset=UINT64_MAX;run(WMTArgumentStatusInvalid);g.intersection_function_offset=0;
  g.index_type=(WMTASIndexType)3;run(WMTArgumentStatusUnsupported);
  g.index_type=WMTASIndexUInt16;run(WMTArgumentStatusInvalid);g.index_buffer=(obj_handle_t)indices;
  indices.length=6;run(WMTArgumentStatusReady);indices.length=5;run(WMTArgumentStatusInvalid);
  indices.length=12;g.index_type=WMTASIndexUInt32;run(WMTArgumentStatusReady);
  g.index_offset=2;run(WMTArgumentStatusInvalid);g.index_offset=UINT64_MAX;run(WMTArgumentStatusInvalid);
  g.index_offset=0;g.index_type=WMTASIndexNone;run(WMTArgumentStatusInvalid);g.index_buffer=0;
  device.failSizes=YES;run(WMTArgumentStatusFailed);device.failSizes=NO;
  device.throwSizes=YES;run(WMTArgumentStatusFailed);device.throwSizes=NO;
  query.info.ptr=nullptr;CHECK(_MTLDevice_accelerationStructureSizes(&query)==0&&query.status==WMTArgumentStatusInvalid);query.info.ptr=&info;
  WMTASBuildDesc triangles=desc;
  obj_handle_t children[]={(obj_handle_t)child};
  desc={};desc.kind=WMTASBuildInstances;desc.instance_buffer=(obj_handle_t)instances;desc.instance_count=2;
  desc.instance_stride=sizeof(WMTASUserIDInstance);desc.acceleration_structures.ptr=children;desc.acceleration_structure_count=1;
  run(WMTArgumentStatusReady);
  desc.instance_offset=4;run(WMTArgumentStatusInvalid);desc.instance_offset=0;
  desc.instance_stride=64;run(WMTArgumentStatusInvalid);desc.instance_stride=68;
  desc.instance_stride=69;run(WMTArgumentStatusInvalid);desc.instance_stride=68;
  instances.length=135;run(WMTArgumentStatusInvalid);instances.length=136;
  desc.instance_count=UINT64_MAX;run(WMTArgumentStatusInvalid);desc.instance_count=2;
  children[0]=0;run(WMTArgumentStatusInvalid);children[0]=(obj_handle_t)child;
  child.device=other;run(WMTArgumentStatusInvalid);child.device=device;
  child.size=0;run(WMTArgumentStatusInvalid);child.size=1024;
  child.heap=other;run(WMTArgumentStatusInvalid);child.heap=nil;
  desc.acceleration_structure_count=4097;run(WMTArgumentStatusInvalid);desc.acceleration_structure_count=1;
  desc.kind=(WMTASBuildKind)2;run(WMTArgumentStatusUnsupported);desc.kind=WMTASBuildInstances;
  WMTASBuildDesc tlas=desc;
  unixcall_mtl_as_new allocate{};allocate.device=(obj_handle_t)device;allocate.size=1024;
  CHECK(_MTLDevice_newAccelerationStructure(&allocate)==0&&allocate.status==WMTArgumentStatusReady&&allocate.ret);
  CHECK([(ASProbe*)allocate.ret size]==1024);[(id)allocate.ret release];
  device.failNew=YES;CHECK(_MTLDevice_newAccelerationStructure(&allocate)==0&&!allocate.ret&&allocate.status==WMTArgumentStatusOutOfMemory);device.failNew=NO;
  allocate.size=0;CHECK(_MTLDevice_newAccelerationStructure(&allocate)==0&&!allocate.ret&&allocate.status==WMTArgumentStatusInvalid);
  unixcall_mtl_as_build build{};build.command_buffer=(obj_handle_t)cmd;build.desc.ptr=&desc;
  build.target=(obj_handle_t)target;build.scratch=(obj_handle_t)scratch;build.fence=(obj_handle_t)fence;
  auto encode=[&](WMTArgumentStatus status,bool emits){
    unsigned before=builds;[encoder.events removeAllObjects];[cmd.events removeAllObjects];
    CHECK(_MTLCommandBuffer_buildAccelerationStructure(&build)==0&&build.status==status);
    CHECK(builds==before+unsigned(emits));device.seen=nil;
    if(status==WMTArgumentStatusReady)CHECK(([encoder.events isEqual:@[@"wait",@"build",@"update",@"end"]]));
    [cmd.handlers removeAllObjects];
  };
  desc=triangles;encode(WMTArgumentStatusReady,true);
  desc=tlas;encode(WMTArgumentStatusReady,true);
  build.scratch_offset=1;encode(WMTArgumentStatusInvalid,false);build.scratch_offset=768;encode(WMTArgumentStatusInvalid,false);build.scratch_offset=256;encode(WMTArgumentStatusReady,true);build.scratch_offset=0;
  target.size=1023;encode(WMTArgumentStatusInvalid,false);target.size=1024;
  scratch.device=other;encode(WMTArgumentStatusInvalid,false);scratch.device=device;
  scratch.heap=other;encode(WMTArgumentStatusInvalid,false);scratch.heap=nil;
  build.target=(obj_handle_t)child;encode(WMTArgumentStatusInvalid,false);build.target=(obj_handle_t)target;
  build.scratch=(obj_handle_t)instances;encode(WMTArgumentStatusInvalid,false);build.scratch=(obj_handle_t)scratch;
  cmd.status=MTLCommandBufferStatusCommitted;encode(WMTArgumentStatusInvalid,false);cmd.status=MTLCommandBufferStatusNotEnqueued;
  fence.device=other;encode(WMTArgumentStatusInvalid,false);fence.device=device;
  cmd.failEncoder=YES;encode(WMTArgumentStatusFailed,false);cmd.failEncoder=NO;
  encoder.throwBuild=YES;encode(WMTArgumentStatusFailed,true);CHECK([encoder.events.lastObject isEqual:@"end"]);encoder.throwBuild=NO;
  // The completion block must retain inputs after the descriptor autorelease pool drains.
  const auto beforeInstances=instances.retainCount,beforeChild=child.retainCount,beforeScratch=scratch.retainCount;
  CHECK(_MTLCommandBuffer_buildAccelerationStructure(&build)==0&&build.status==WMTArgumentStatusReady);
  device.seen=nil;
  CHECK(instances.retainCount>beforeInstances&&child.retainCount>beforeChild&&scratch.retainCount>beforeScratch);
  for(MTLCommandBufferHandler callback in cmd.handlers)callback((id<MTLCommandBuffer>)cmd);
  [cmd.handlers removeAllObjects];
  CHECK(instances.retainCount==beforeInstances&&child.retainCount==beforeChild&&scratch.retainCount==beforeScratch);
  for(unsigned i=0;i<1000;++i){desc=i%2?tlas:triangles;encode(WMTArgumentStatusReady,true);}
  uint64_t bytes;CHECK(!wmt_as_span(UINT64_MAX,UINT64_MAX,68,&bytes));
  CHECK(wmt_as_span(2,68,68,&bytes)&&bytes==136);
  for(ASProbe *p in @[device,other,vertices,indices,target,scratch,fence,cmd,encoder,instances,child])[p release];
 }
 CHECK(live==0);
 printf("AS_BRIDGE_CPU_PASS checks=%u repeated_builds=1000 GPU=0\n",checks);
}
