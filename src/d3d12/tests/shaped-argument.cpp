// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_resource_shape.hpp"
#include "../d3d12_raytracing_bindings.hpp"
#include "../d3d12_local_root_layout.hpp"
#include "../d3d12_argument_access.hpp"
#include "../d3d12_recorded_binding.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <array>

using namespace dxmt;
static uint64_t checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)
using S = capture::Status;
static ray_library::Resource Resource(uint32_t type, uint32_t kind, uint32_t flags = 0) {
  return {type, kind, 0, 0, 3, 3, flags, "test"};
}
static void Shapes() {
  const uint32_t kinds[] = {0,0,1,6,2,7,3,8,4,5,9,0};
  for (uint32_t d=0; d<12; ++d) {
    CHECK(resource_shape::TextureKind(d,false)==kinds[d]);
    CHECK(resource_shape::TextureKind(d,true)==((d==2||d==3||d==4||d==5||d==8)?kinds[d]:0));
  }
  CHECK(resource_shape::Buffer(39,0,1,false).kind==11);
  CHECK(resource_shape::Buffer(0,16,0,true).kind==12);
  CHECK(resource_shape::Buffer(41,0,0,false).kind==10);
  for (auto v : {resource_shape::Buffer(0,0,0,false),resource_shape::Buffer(39,4,1,false),
                 resource_shape::Buffer(39,0,1,true),resource_shape::Buffer(0,2049,0,false),
                 resource_shape::Buffer(41,16,0,false),resource_shape::Buffer(41,0,0,true),
                 resource_shape::Buffer(41,0,2,false)}) CHECK(!v.kind);
  for (uint32_t kind=1;kind<=14;++kind) {
    auto r=Resource(kind==13?2:kind==14?3:0,kind);
    for(uint32_t actual=0;actual<=18;++actual) {
      resource_shape::View v{actual,actual==12?16u:0u,0,actual<=9?1u:0u};
      const S expected=!actual?S::Unsupported:actual==kind?S::Ready:S::Invalid;
      CHECK(resource_shape::Match(r,v,false)==expected);
    }
  }
  auto counter=Resource(1,12,2);
  CHECK(resource_shape::Match(counter,{12,16,0,0},true)==S::Ready);
  CHECK(resource_shape::Match(counter,{12,16,0,0},false)==S::Invalid);
  CHECK(resource_shape::Match(Resource(1,12),{12,16,0,0},true)==S::Ready);
  CHECK(resource_shape::Match(Resource(0,12),{12,16,0,0},true)==S::Invalid);
  CHECK(resource_shape::Match(Resource(1,11,2),{11,0,39,0},false)==S::Invalid);
  CHECK(resource_shape::Match(Resource(0,16),{16,0,0,0},false)==S::Unsupported);
  CHECK(resource_shape::Match(Resource(1,2,16),{2,0,0,1},false)==S::Unsupported);
  CHECK(resource_shape::Match(Resource(0,12),{12,0,0,0},false)==S::Invalid);
  CHECK(resource_shape::Match(Resource(0,8),{8,0,0,0},false)==S::Invalid);
  CHECK(resource_shape::Match(Resource(0,2),{2,0,0,4},false)==S::Invalid);
}
struct Snapshot { resource_shape::View shape; bool counter=false; uint64_t words=0; };
struct Provider {
  std::shared_ptr<Snapshot> slot;
  ray_library::Resource required;
  bool Valid() const { return bool(slot); }
  S Acquire(Snapshot &out) const {
    out={};
    const auto status=resource_shape::Match(required,slot->shape,slot->counter);
    if(status==S::Ready) out=*slot;
    return status;
  }
};
static void Replay() {
  std::mt19937 random(19391);
  for(uint32_t i=0;i<20000;++i) {
    auto slot=std::make_shared<Snapshot>();slot->shape={2,0,28,1};slot->words=random();
    const auto original=slot->words;
    capture::RecordedDescriptor<Provider,Snapshot> fixed,changed;
    auto r=Resource(0,2);
    CHECK(fixed.Record({slot,r},false)==S::Ready);
    CHECK(changed.Record({slot,r},true)==S::Ready);
    slot->shape.kind=7;slot->words=random();
    Snapshot out;
    CHECK(fixed.Resolve(out)==S::Ready);CHECK(out.shape.kind==2&&out.words==original);
    CHECK(changed.Resolve(out)==S::Invalid);CHECK(!out.shape.kind&&!out.words);
    slot->shape.kind=2;
    CHECK(changed.Resolve(out)==S::Ready);CHECK(out.words==slot->words);
  }
}
static void Arguments() {
  std::mt19937 random(1147);
  for(uint32_t i=0;i<20000;++i) {
    const uint32_t type=i%3,kind=type==2?13:11;
    ray_binding::RootLayout root;root.local=i&1;
    const uint32_t start=1+random()%1000,count=1+random()%32,table_offset=random()%64;
    const uint32_t flags=(i%2)?1:8;
    root.spans.push_back({type,0,start,uint64_t(start)+count,ray_binding::Source::Table,0,0,table_offset,0,flags});
    auto r=Resource(type,kind);r.lower=start;r.upper=start+count-1;
    ray_binding::Binding binding;
    CHECK(ray_binding::Find(root.local?nullptr:&root,root.local?&root:nullptr,r,binding)==ray_binding::Status::Ready);
    CHECK(binding.flags==flags);
    const uint32_t element=random()%count;
    const ray_binding::Span *span=nullptr;uint64_t offset=0;
    CHECK(ray_binding::SelectResource(root,binding,r,element,span,offset)==ray_binding::Status::Ready);
    CHECK(span==&root.spans[0]&&offset==table_offset+element);
    CHECK(ray_binding::SelectResource(root,binding,r,count,span,offset)==ray_binding::Status::Invalid);
    CHECK(!span&&!offset);
    binding.flags^=8;
    CHECK(ray_binding::SelectResource(root,binding,r,0,span,offset)==ray_binding::Status::Invalid);
    binding.flags^=8;
    const root_argument::HeapRange heap{0x100000,256,32,type==3?1u:0u};
    root_argument::TableLocation location;
    CHECK(root_argument::Locate(heap,heap.base+32,table_offset+element,1,location));
    CHECK(location.index==1+table_offset+element);
    local_root::Layout local{16,{{local_root::Constants,0,4,0,0,{}},{local_root::Table,8,8,0,0,{}}}};
    std::array<uint8_t,16> bytes{};
    const uint64_t handle=heap.base+32;
    for(unsigned n=0;n<8;++n)bytes[8+n]=uint8_t(handle>>(8*n));
    uint64_t value=0;
    CHECK(local_root::Read(local,1,bytes.data(),bytes.size(),0,value)&&value==handle);
    CHECK(!local_root::Read(local,1,bytes.data(),15,0,value)&&!value);
    CHECK(!local_root::Read(local,1,bytes.data(),16,1,value)&&!value);
    CHECK(!local_root::Read(local,2,bytes.data(),16,0,value)&&!value);
    CHECK(!root_argument::Locate(heap,handle+1,0,1,location));
    // The local record was consumed, not retained as a pointer to mutable bytes.
    CHECK(local_root::Read(local,1,bytes.data(),16,0,value));
    bytes.fill(0);CHECK(value==handle);
    auto direct=Resource(type,kind);
    CHECK(resource_shape::Root(direct,0x100000,64)==S::Ready);
    CHECK(resource_shape::Root(direct,0x100001,64)==S::Invalid);
    CHECK(resource_shape::Root(direct,0x100000,0)==S::Invalid);
    CHECK(resource_shape::Root(direct,UINT64_MAX-3,16)==S::Invalid);
    direct.upper++;
    CHECK(resource_shape::Root(direct,0x100000,64)==S::Invalid);
  }
  CHECK(resource_shape::Root(Resource(0,2),0x100000,64)==S::Invalid);
  CHECK(resource_shape::Root(Resource(0,16),0x100000,64)==S::Unsupported);
  CHECK(resource_shape::Root(Resource(2,13),0x100000,65537)==S::Invalid);
  CHECK(resource_shape::Root(Resource(0,12),0x100000,64)==S::Ready);
}
int main() {
  Shapes();Replay();Arguments();
  std::cout<<"SHAPED_ARGUMENT_CPU_PASS checks="<<checks<<" replay_cases=20000 argument_cases=20000 gpu=0\n";
}
