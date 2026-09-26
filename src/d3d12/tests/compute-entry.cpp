// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_metal_argument.hpp"
#include "../d3d12_compute_commands.hpp"
#include <array>
#include <cassert>
#include <cstdio>
using namespace dxmt;
static unsigned checks;
#define CHECK(x) do {++checks; assert(x);} while(0)
int main() {
 metal_argument::Plan p{{{29,{{3,0,WMTArgumentKindBuffer},{7,1,WMTArgumentKindTexture},{9,2,WMTArgumentKindSampler}}}},
     {{1,0,WMTArgumentKindBuffer},{0,1,WMTArgumentKindTexture}}};
 WMTComputeBindingLayout layout{};
 layout.count=3; layout.max_threads=256;
 layout.bindings[0]={29,WMTComputeBindingArgumentBuffer,0,16,32,0,0,0};
 layout.bindings[1]={1,WMTComputeBindingBuffer,0,16,32,0,0,0};
 layout.bindings[2]={0,WMTComputeBindingTexture,1,0,0,2,3,0};
 auto match=[&](const metal_argument::Plan &q,const WMTComputeBindingLayout &l,bool want){
   CHECK((metal_argument::Match(q,3,l)==capture::Status::Ready)==want);
 };
 match(p,layout,true);
 auto q=p; q.direct.pop_back(); match(q,layout,false);
 q=p; q.blocks.clear(); match(q,layout,false);
 q=p; q.direct[0].index=29; match(q,layout,false);
 q=p; q.direct[1].kind=WMTArgumentKindSampler; match(q,layout,false);
 q=p; q.direct[1].source=3; match(q,layout,false);
 q=p; q.direct.push_back(q.direct[1]); match(q,layout,false);
 q=p; q.direct[1].index=128; match(q,layout,false);
 auto l=layout; l.count=160; match(p,l,false);
 l=layout; l.count=2; match(p,l,false);
 l=layout; l.max_threads=0; match(p,l,false);
 l=layout; l.bindings[1].index=29; match(p,l,false);
 l=layout; l.bindings[0].kind=WMTComputeBindingBuffer; match(p,l,false);
 l=layout; l.bindings[1].kind=static_cast<WMTComputeBindingKind>(3); match(p,l,false);
 l=layout; l.bindings[1].access=3; match(p,l,false);
 l=layout; l.bindings[1].alignment=0; match(p,l,false);
 q=p; l=layout; q.direct[1].index=1; l.bindings[2].index=1; match(q,l,true);
 for(unsigned i=0;i<10000;++i) {
   l=layout; q=p; q.direct[1].index=i%128; l.bindings[2].index=i%128;
   match(q,l,true);
 }
 CHECK(metal_argument::Threads({8,8,4},256));
 CHECK(!metal_argument::Threads({8,8,5},256));
 CHECK(!metal_argument::Threads({1,0,1},256));
 CHECK(!metal_argument::Threads({1,1,1},0));
 CHECK(!metal_argument::Threads({UINT64_MAX,2,1},256));
 CHECK(!metal_argument::Threads({UINT64_MAX,2,1},UINT64_MAX));
 CHECK(metal_argument::Threads({UINT64_MAX,1,1},UINT64_MAX));
 CHECK(!metal_argument::Threads({UINT64_MAX,1,2},UINT64_MAX));
 CHECK(metal_argument::Threads({1,1,UINT64_MAX},UINT64_MAX));
 // Saturate both independent resource namespaces and check exact, allocation-free linkage.
 std::array<wmtcmd_compute_useresource,9> uses{};
 std::array<wmtcmd_compute_setbuffer,31> buffers{};
 std::array<wmtcmd_compute_settexture,128> textures{};
 wmtcmd_compute_dispatch launch{};
 WMTMemoryPointer first{};
 compute_commands::Link(first,uses,buffers,textures,launch);
 CHECK(first.ptr==&uses[0]);
 for(size_t i=0;i<uses.size();++i) CHECK(uses[i].next.ptr==(i+1<uses.size()?(void*)&uses[i+1]:(void*)&buffers[0]));
 for(size_t i=0;i<buffers.size();++i) CHECK(buffers[i].next.ptr==(i+1<buffers.size()?(void*)&buffers[i+1]:(void*)&textures[0]));
 for(size_t i=0;i<textures.size();++i) CHECK(textures[i].next.ptr==(i+1<textures.size()?(void*)&textures[i+1]:(void*)&launch));
 CHECK(!launch.next.ptr);
 compute_commands::Link(first,{}, {}, {},launch);
 CHECK(first.ptr==&launch && !launch.next.ptr);
 compute_commands::Link(first,{}, {}, textures,launch);
 CHECK(first.ptr==&textures[0] && textures.back().next.ptr==&launch);
 // Acceleration structures share buffer slots and are read-only, never argument-block members.
 for (auto kind : {WMTArgumentKindPrimitiveAccelerationStructure,WMTArgumentKindInstanceAccelerationStructure}) {
   auto q=p; auto l=layout;
   q.direct.push_back({6,2,kind}); l.bindings[3]={6,static_cast<WMTComputeBindingKind>(kind),0,0,0,0,0,0}; l.count=4;
   match(q,l,true);
   q.direct.back().index=29; l.bindings[3].index=29; match(q,l,false);
   q.direct.back().index=6; l.bindings[3].index=6;
   l.bindings[3].access=1; match(q,l,false); l.bindings[3].access=0;
   l.bindings[3].alignment=16; match(q,l,false); l.bindings[3].alignment=0;
   l.bindings[3].minimum_size=64; match(q,l,false); l.bindings[3].minimum_size=0;
   q.blocks[0].members[0].kind=kind; match(q,l,false); q.blocks[0].members[0].kind=WMTArgumentKindBuffer;
   l.bindings[3].kind=kind==WMTArgumentKindPrimitiveAccelerationStructure ?
     WMTComputeBindingInstanceAccelerationStructure:WMTComputeBindingPrimitiveAccelerationStructure; match(q,l,false);
 }
 std::array<wmtcmd_compute_setaccelerationstructure,31> scenes{};
 compute_commands::Link(first,uses,{},textures,launch,scenes);
 CHECK(textures.back().next.ptr==&scenes[0]);
 for(size_t i=0;i<scenes.size();++i) CHECK(scenes[i].next.ptr==(i+1<scenes.size()?(void*)&scenes[i+1]:(void*)&launch));
 CHECK(!launch.next.ptr);
 compute_commands::Link(first,{},{},{},launch,scenes);
 CHECK(first.ptr==&scenes[0]);
 printf("entry_plan_chain_checks=%u replay_checks=10000 GPU=0\n",checks);
}
