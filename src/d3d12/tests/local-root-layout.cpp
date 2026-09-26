// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_local_root_layout.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <limits>
using namespace dxmt::local_root;
struct TestRange { uint32_t RangeType,NumDescriptors,BaseShaderRegister,RegisterSpace,OffsetInDescriptorsFromTableStart; };
struct TestParameter {
  uint32_t ParameterType=0,ShaderVisibility=0;
  struct { uint32_t NumDescriptorRanges=0; const TestRange* pDescriptorRanges=nullptr; } DescriptorTable;
  struct { uint32_t ShaderRegister=0,RegisterSpace=0,Num32BitValues=0; } Constants;
  struct { uint32_t ShaderRegister=0,RegisterSpace=0; } Descriptor;
};
struct TestSampler { uint32_t ShaderVisibility=0; };
struct TestDesc { uint32_t NumParameters=0;const TestParameter* pParameters=nullptr;uint32_t NumStaticSamplers=0;const TestSampler* pStaticSamplers=nullptr;uint32_t Flags=0x80; };
static TestParameter param(uint32_t type,uint32_t count=1,uint32_t reg=0,uint32_t space=0){
  TestParameter p;p.ParameterType=type;p.Constants={reg,space,count};p.Descriptor={reg,space};return p;
}
int main(){
  uint64_t checks=0;auto check=[&](bool v){assert(v);++checks;};Layout layout;
  TestDesc desc;check(Build(desc,layout)&&layout.bytes==0);
  std::array<TestParameter,6> p={param(Constants),param(Constants),param(CBV),param(Constants,3),param(SRV),param(UAV)};
  desc.NumParameters=p.size();desc.pParameters=p.data();check(Build(desc,layout));
  const uint32_t offsets[]={0,4,8,16,32,40};for(unsigned i=0;i<6;i++)check(layout.parameters[i].byte_offset==offsets[i]);check(layout.bytes==48);
  std::array<unsigned char,64> bytes;bytes.fill(0xa5);auto before=bytes;
  for(unsigned i=0;i<48;i++)bytes[8+i]=i;
  before=bytes;uint64_t value=0;
  check(Read(layout,0,bytes.data()+8,48,0,value)&&value==0x03020100);
  check(Read(layout,1,bytes.data()+8,48,0,value)&&value==0x07060504);
  check(Read(layout,2,bytes.data()+8,48,0,value)&&value==UINT64_C(0x0f0e0d0c0b0a0908));
  check(Read(layout,3,bytes.data()+8,48,2,value)&&value==0x1b1a1918);
  check(!Read(layout,3,bytes.data()+8,48,3,value)&&value==0);
  check(!Read(layout,2,bytes.data()+8,48,1,value));check(!Read(layout,0,nullptr,48,0,value));
  for(unsigned n=0;n<48;n++)check(!Read(layout,0,bytes.data()+8,n,0,value));
  check(!Read(layout,6,bytes.data()+8,48,0,value));check(bytes==before);
  for(unsigned badFlags: {0u,1u,0x81u,0x80000080u}){desc.Flags=badFlags;check(!Build(desc,layout)&&layout.parameters.empty());}desc.Flags=0x80;
  p[0].ShaderVisibility=1;check(!Build(desc,layout));p[0].ShaderVisibility=0;
  p[0].ParameterType=5;check(!Build(desc,layout));p[0].ParameterType=Constants;
  p[0].Constants.Num32BitValues=0;check(!Build(desc,layout));p[0].Constants.Num32BitValues=UINT32_MAX;check(!Build(desc,layout));p[0].Constants.Num32BitValues=1;
  desc.pParameters=nullptr;check(!Build(desc,layout));desc.pParameters=p.data();
  TestSampler sampler;desc.NumStaticSamplers=1;check(!Build(desc,layout));desc.pStaticSamplers=&sampler;check(Build(desc,layout));sampler.ShaderVisibility=5;check(!Build(desc,layout));desc.NumStaticSamplers=0;desc.pStaticSamplers=nullptr;
  std::vector<TestParameter> large(1016,param(Constants));desc.NumParameters=large.size();desc.pParameters=large.data();check(Build(desc,layout)&&layout.bytes==4064);
  large.push_back(param(Constants));desc.NumParameters=large.size();desc.pParameters=large.data();check(!Build(desc,layout));
  large.assign(508,param(CBV));desc.NumParameters=large.size();desc.pParameters=large.data();check(Build(desc,layout)&&layout.bytes==4064);
  large.push_back(param(SRV));desc.NumParameters=large.size();desc.pParameters=large.data();check(!Build(desc,layout));
  auto maxConstants=param(Constants,1016);desc={1,&maxConstants,0,nullptr,0x80};check(Build(desc,layout)&&layout.bytes==4064);
  maxConstants.Constants.Num32BitValues=1017;check(!Build(desc,layout));
  std::array<TestRange,3> ranges={TestRange{RangeSRV,3,10,2,5},TestRange{RangeUAV,2,4,1,UINT32_MAX},TestRange{RangeCBV,1,7,0,UINT32_MAX}};
  auto table=param(Table);table.DescriptorTable={3,ranges.data()};desc={1,&table,0,nullptr,0x80};check(Build(desc,layout)&&layout.bytes==8);
  check(layout.parameters[0].ranges[0].descriptor_offset==5);check(layout.parameters[0].ranges[1].descriptor_offset==8);check(layout.parameters[0].ranges[2].descriptor_offset==10);
  Binding binding;check(Find(layout,RangeSRV,12,2,binding)==Match::Found&&binding.table&&binding.descriptor_index==7);
  check(Find(layout,RangeUAV,5,1,binding)==Match::Found&&binding.descriptor_index==9);
  check(Find(layout,RangeCBV,7,0,binding)==Match::Found&&binding.descriptor_index==10);
  check(Find(layout,RangeSRV,13,2,binding)==Match::Missing);check(Find(layout,RangeSRV,12,1,binding)==Match::Missing);check(Find(layout,99,12,2,binding)==Match::Invalid);
  ranges[0].OffsetInDescriptorsFromTableStart=77;check(layout.parameters[0].ranges[0].descriptor_offset==5);ranges[0].OffsetInDescriptorsFromTableStart=5;
  ranges[1].RangeType=RangeSampler;check(!Build(desc,layout));ranges[1].RangeType=RangeUAV;
  ranges[0].NumDescriptors=UINT32_MAX;check(!Build(desc,layout));ranges[1].OffsetInDescriptorsFromTableStart=8;check(Build(desc,layout));
  check(Find(layout,RangeSRV,UINT32_MAX,2,binding)==Match::Found&&binding.descriptor_index==UINT64_C(4294967290));
  ranges[0].NumDescriptors=3;ranges[1].OffsetInDescriptorsFromTableStart=UINT32_MAX;
  table.DescriptorTable.pDescriptorRanges=nullptr;check(!Build(desc,layout));table.DescriptorTable.pDescriptorRanges=ranges.data();
  std::array<TestParameter,2> duplicate={param(Constants,1,3),param(CBV,1,3)};desc={2,duplicate.data(),0,nullptr,0x80};check(Build(desc,layout));check(Find(layout,RangeCBV,3,0,binding)==Match::Ambiguous&&binding.parameter==0&&!binding.table);
  duplicate[1].Descriptor.RegisterSpace=1;check(Build(desc,layout));check(Find(layout,RangeCBV,3,1,binding)==Match::Found&&binding.parameter==1);
  std::mt19937 rng(0x995122);
  for(unsigned round=0;round<20000;round++){
    std::vector<TestParameter> ps(1+rng()%32);uint32_t expected=0;
    for(unsigned i=0;i<ps.size();i++)ps[i]=param(rng()%5,1+rng()%8,i,i%3);
    desc={uint32_t(ps.size()),ps.data(),0,nullptr,0x80};check(Build(desc,layout));
    std::vector<unsigned char> data(layout.bytes);for(auto& b:data)b=rng();const auto copy=data;
    for(unsigned i=0;i<ps.size();i++){
      const auto& input=ps[i];if(input.ParameterType!=Constants)while(expected%8)expected++;
      check(layout.parameters[i].byte_offset==expected);
      const uint32_t size=input.ParameterType==Constants?4*input.Constants.Num32BitValues:8;
      check(layout.parameters[i].byte_size==size);const unsigned words=input.ParameterType==Constants?input.Constants.Num32BitValues:1;
      for(unsigned w=0;w<words;w++){
        uint64_t answer=0;const unsigned length=input.ParameterType==Constants?4:8,offset=expected+(input.ParameterType==Constants?w*4:0);
        for(unsigned n=0;n<length;n++)answer+=uint64_t(data[offset+n])*(UINT64_C(1)<<(8*n));
        check(Read(layout,i,data.data(),data.size(),w,value)&&value==answer);
      }expected+=size;
    }
    check(layout.bytes==expected);check(data==copy);
  }
  std::printf("LOCAL_ROOT_CPU_PASS checks=%llu randomLayouts=20000 gpu=0\n",static_cast<unsigned long long>(checks));
}
