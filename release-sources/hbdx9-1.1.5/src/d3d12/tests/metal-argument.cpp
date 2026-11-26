// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_metal_argument.hpp"
#include <cassert>
#include <cstdio>
using namespace dxmt;
int main() {
 unsigned checks=0;
 auto check=[&](const metal_argument::Plan &p,size_t n,bool valid){++checks; assert((metal_argument::Validate(p,n)==capture::Status::Ready)==valid);};
 metal_argument::Plan p{{{29,{{3,0,WMTArgumentKindBuffer},{7,1,WMTArgumentKindTexture},{9,2,WMTArgumentKindSampler}}}}, {}};
 check(p,3,true); check({},3,false); check(p,2,false); check(p,65537,false);
 auto q=p; q.blocks.push_back(p.blocks[0]); check(q,3,false);
 q=p; q.blocks[0].index=31; check(q,3,false);
 q=p; q.blocks[0].members[2].index=3; check(q,3,false);
 q=p; q.blocks[0].members[0].kind=static_cast<WMTArgumentKind>(3); check(q,3,false);
 q=p; q.blocks[0].members.clear(); check(q,3,false);
 q=p; q.blocks[0].members.resize(257); check(q,3,false);
 for(unsigned i=0;i<10000;++i) {
   q=p; q.blocks[0].index=i%31; q.blocks[0].members[0].source=i%3;
   check(q,3,true);
 }
 printf("plan_checks=%u GPU=0\n",checks);
}
