// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_compiler_resource.hpp"
#include "../d3d12_recorded_binding.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>
#include <stdexcept>
using namespace dxmt;
using compiler_resource::Footprint;
using capture::Status;
static size_t checks=0;
static void Check(bool ok){++checks;if(!ok)throw std::runtime_error("check "+std::to_string(checks));}
static Footprint Fact(uint32_t kind) {
  Footprint f;
  f.resource={kind==13?2u:kind==14?3u:0u,kind,0,0,0,0,0,""};
  if(kind==13)f.cbuffer_bytes=148;
  if(kind==12)f.structured_stride=16;
  if(kind<=10)f.component=9;
  return f;
}
struct Snapshot {resource_shape::View shape;uint64_t bytes=0;bool counter=false;};
struct Provider {
  std::shared_ptr<Snapshot> value;
  bool Valid()const{return bool(value);}
  Status Acquire(Snapshot &out)const{out=*value;return Status::Ready;}
};
static Status Resolve(const capture::RecordedDescriptor<Provider,Snapshot> &record,const Footprint &f) {
  Snapshot out;auto status=record.Resolve(out);
  return status==Status::Ready?compiler_resource::Match(f,out.shape,out.bytes,out.counter,false):status;
}
static void Basic() {
  auto cb=Fact(13);
  Check(compiler_resource::Validate(cb)==Status::Ready);
  Check(compiler_resource::Match(cb,{13,0,0,0},148,false,false)==Status::Ready);
  Check(compiler_resource::Match(cb,{13,0,0,0},147,false,false)==Status::Invalid);
  Check(compiler_resource::Match(cb,{13,0,0,0},256,false,true)==Status::Ready);
  Check(compiler_resource::Match(cb,{13,0,0,0},0,false,true)==Status::Invalid);
  for(uint32_t size:{0u,65537u,UINT32_MAX}){auto f=cb;f.cbuffer_bytes=size;Check(compiler_resource::Validate(f)==Status::Invalid);}
  auto structured=Fact(12);
  Check(compiler_resource::Match(structured,{12,16,0,0},64,false,false)==Status::Ready);
  Check(compiler_resource::Match(structured,{12,12,0,0},64,false,false)==Status::Invalid);
  Check(compiler_resource::Match(structured,{12,0,0,0},64,false,true)==Status::Ready);
  for(uint32_t stride:{0u,1u,3u,2049u,UINT32_MAX}){auto f=structured;f.structured_stride=stride;Check(compiler_resource::Validate(f)==Status::Invalid);}
  auto raw=Fact(11);
  Check(compiler_resource::Match(raw,{11,0,39,0},64,false,false)==Status::Ready);
  Check(compiler_resource::Match(raw,{12,16,0,0},64,false,false)==Status::Invalid);
  auto texture=Fact(2);
  for(uint32_t format:{28u,29u,41u,54u,77u,95u})Check(compiler_resource::Match(texture,{2,0,format,1},0,false,false)==Status::Ready);
  for(uint32_t format:{30u,32u,42u,43u})Check(compiler_resource::Match(texture,{2,0,format,1},0,false,false)==Status::Invalid);
  for(uint32_t format:{0u,1u,27u,39u,100u,UINT32_MAX})Check(compiler_resource::Match(texture,{2,0,format,1},0,false,false)==Status::Unsupported);
  Check(compiler_resource::Match(texture,{2,0,28,1},16,false,true)==Status::Invalid);
  for(uint32_t component:{1u,6u,7u,10u,15u,16u,999u}){auto f=texture;f.component=component;Check(compiler_resource::Validate(f)==Status::Unsupported);}
  for(uint32_t component:{2u,4u}){auto f=texture;f.component=component;Check(compiler_resource::Match(f,{2,0,43,1},0,false,false)==Status::Ready);Check(compiler_resource::Match(f,{2,0,42,1},0,false,false)==Status::Invalid);}
  for(uint32_t component:{3u,5u}){auto f=texture;f.component=component;Check(compiler_resource::Match(f,{2,0,42,1},0,false,false)==Status::Ready);}
  auto ms=Fact(3);ms.samples=4;
  Check(compiler_resource::Match(ms,{3,0,28,4},0,false,false)==Status::Ready);
  Check(compiler_resource::Match(ms,{3,0,28,8},0,false,false)==Status::Invalid);
  ms.samples=0;
  Check(compiler_resource::Match(ms,{3,0,28,8},0,false,false)==Status::Ready);
  Check(compiler_resource::Match(ms,{3,0,28,0},0,false,false)==Status::Invalid);
  Check(compiler_resource::Validate(Fact(14))==Status::Unsupported);
  Check(compiler_resource::Validate(Fact(16))==Status::Unsupported);
  texture.unknown_properties=true;Check(compiler_resource::Validate(texture)==Status::Unsupported);
  auto counter=Fact(12);counter.resource.type=1;counter.resource.flags=2;
  Check(compiler_resource::Match(counter,{12,16,0,0},64,true,false)==Status::Ready);
  Check(compiler_resource::Match(counter,{12,16,0,0},64,false,false)==Status::Invalid);
  auto a=cb.resource,b=a;Check(compiler_resource::SameResource(a,b));
  ++b.id;Check(!compiler_resource::SameResource(a,b));b=a;++b.space;Check(!compiler_resource::SameResource(a,b));
  for(unsigned i=0;i<10000;++i) {
    auto f=Fact(13);f.cbuffer_bytes=4+(i%16383)*4;
    auto value=std::make_shared<Snapshot>(Snapshot{{13,0,0,0},f.cbuffer_bytes,false});
    capture::RecordedDescriptor<Provider,Snapshot> stable,vol;
    Check(stable.Record({value},false)==Status::Ready);
    Check(vol.Record({value},true)==Status::Ready);
    Check(Resolve(stable,f)==Status::Ready && Resolve(vol,f)==Status::Ready);
    --value->bytes;
    Check(Resolve(stable,f)==Status::Ready && Resolve(vol,f)==Status::Invalid);
    value->bytes+=2;
    Check(Resolve(vol,f)==Status::Ready);
    value->shape.kind=11;
    Check(Resolve(vol,f)==Status::Invalid && Resolve(stable,f)==Status::Ready);
  }
}
static void Corpus(const char *path) {
  std::ifstream in(path);Check(bool(in));std::string line;size_t resources=0,supported=0,cbuffers=0;
  while(std::getline(in,line)) {
    std::istringstream stream(line);Footprint f;unsigned unknown;
    auto &r=f.resource;
    Check(bool(stream>>r.type>>r.id>>r.kind>>r.space>>r.lower>>r.upper>>r.flags>>
      f.cbuffer_bytes>>f.structured_stride>>f.component>>f.samples>>f.sampler_kind>>unknown));
    f.unknown_properties=unknown;++resources;
    const auto valid=compiler_resource::Validate(f);
    Check(valid!=Status::Invalid);
    if(valid==Status::Unsupported)continue;
    ++supported;
    const uint32_t format=f.component?(compiler_resource::ComponentDomain(f.component)==compiler_resource::Domain::Float?41u:
      compiler_resource::ComponentDomain(f.component)==compiler_resource::Domain::UInt?42u:43u):0u;
    resource_shape::View view{r.kind,f.structured_stride,format,
      r.kind==3||r.kind==8?std::max(1u,f.samples):r.kind<=9?1u:0u};
    Check(compiler_resource::Match(f,view,f.cbuffer_bytes,bool(r.flags&2),false)==Status::Ready);
    if(f.cbuffer_bytes){++cbuffers;Check(compiler_resource::Match(f,view,f.cbuffer_bytes-1,false,false)==Status::Invalid);}
    if(f.component){view.format=format==41?42:41;Check(compiler_resource::Match(f,view,0,bool(r.flags&2),false)==Status::Invalid);}
  }
  Check(resources==31485 && cbuffers==9768);
  std::cout<<"CORPUS resources="<<resources<<" supported="<<supported<<" cbuffers="<<cbuffers<<" realStructuredResources=0\n";
}
int main(int argc,char **argv) {
  try{Basic();if(argc==2)Corpus(argv[1]);std::cout<<"FOOTPRINT_PASS checks="<<checks<<" static_volatile_replays=10000 GPU=0\n";}
  catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
