// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_acceleration_build.hpp"
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>
using namespace dxmt;
static unsigned checks;
#define CHECK(x) do {++checks;assert(x);}while(0)
struct Producer{};
struct Scene{unsigned generation;explicit Scene(unsigned n):generation(n){}};
int main(){
 uint64_t bytes=99,offset=99;
 CHECK(!scene_build::ByteSpan(0,12,12,bytes)&&!bytes);
 CHECK(!scene_build::ByteSpan(3,4,12,bytes)&&!bytes);
 CHECK(scene_build::ByteSpan(3,16,12,bytes)&&bytes==44);
 CHECK(scene_build::ByteSpan(1,UINT64_MAX,12,bytes)&&bytes==12);
 CHECK(!scene_build::ByteSpan(UINT64_MAX,UINT64_MAX,12,bytes)&&!bytes);
 CHECK(!scene_build::ByteSpan(2,0,0,bytes)&&!bytes);
 CHECK(scene_build::Rebase(4096,256,128,16,64,offset)&&offset==272);
 CHECK(!scene_build::Rebase(4096,256,128,100,64,offset)&&!offset);
 CHECK(!scene_build::Rebase(UINT64_MAX,UINT64_MAX,1,0,1,offset)&&!offset);
 CHECK(!scene_build::Rebase(100,20,80,80,0,offset)&&!offset);
 uint64_t seed=0x761bec9432ull;
 auto random=[&](){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;};
 for(unsigned i=0;i<20000;++i){
   uint64_t n=random(),stride=random(),element=random();
   if(i%2){n%=1000;stride%=1024;element%=512;}
   const __uint128_t mathematical=n?__uint128_t(n-1)*stride+element:0;
   bool want=n&&element&&stride>=element&&mathematical<=UINT64_MAX;
   CHECK(scene_build::ByteSpan(n,stride,element,bytes)==want);
   CHECK(bytes==(want?uint64_t(mathematical):0));
   uint64_t total=random(),start=random(),length=random(),relative=random(),count=random();
   if(i%2){total%=4096;start%=1024;length%=1024;relative%=1024;count%=512;}
   bool valid=count&&__uint128_t(start)+length<=total&&__uint128_t(relative)+count<=length;
   CHECK(scene_build::Rebase(total,start,length,relative,count,offset)==valid);
   CHECK(offset==(valid?start+relative:0));
 }
 uint64_t used=0;
 CHECK(!scene_build::Charge(0,1,used)&&!used);
 CHECK(!scene_build::Charge(1,0,used)&&!used);
 CHECK(!scene_build::Charge(UINT64_MAX,1,used)&&!used);
 CHECK(!scene_build::Charge(1,64*1024*1024+1,used)&&!used);
 CHECK(scene_build::Charge(64*1024*1024,64*1024*1024,used));
 CHECK(scene_build::Charge(64*1024*1024,64*1024*1024,used));
 CHECK(!scene_build::Charge(1,1,used)&&used==256*1024*1024);
 Producer geometry,top;
 for(unsigned replay=0;replay<1000;++replay){
   scene_build::SubmissionScenes<Producer,Scene> batch;
   CHECK(!batch.Find(&geometry)&&!batch.Find(&top));
   auto first=std::make_shared<Scene>(replay*2);
   std::weak_ptr<const Scene> weak=first;
   batch.Publish(&geometry,first);
   auto oldDispatch=batch.Find(&geometry);
   first.reset();CHECK(!weak.expired());
   auto second=std::make_shared<Scene>(replay*2+1);
   batch.Publish(&geometry,second);
   CHECK(batch.Find(&geometry)==second&&oldDispatch->generation==replay*2);
   CHECK(!batch.Find(&top));
   batch.Publish(&top,oldDispatch);
   oldDispatch.reset();CHECK(!weak.expired());
   batch.Publish(&top,second);CHECK(weak.expired());
   scene_build::SubmissionScenes<Producer,Scene> otherBatch;
   CHECK(!otherBatch.Find(&geometry));
 }
 printf("SCENE_PRODUCER_CPU_PASS checks=%u range_cases=20000 repeated_occurrences=1000 GPU=0\n",checks);
}
