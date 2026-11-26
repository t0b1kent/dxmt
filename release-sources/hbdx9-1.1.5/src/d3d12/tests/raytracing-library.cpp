// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_raytracing_library.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

// Adapter is also instantiated against the real Windows SDK by the device
// translation unit. These native structs isolate its ownership/error behavior.
using HRESULT = int32_t;
constexpr HRESULT S_OK=0, E_INVALIDARG=-1, DXGI_ERROR_UNSUPPORTED=-2, E_OUTOFMEMORY=-3;
constexpr uint32_t D3D12_STATE_OBJECT_TYPE_COLLECTION=0, D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE=3;
constexpr uint32_t D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY=5, D3D12_EXPORT_FLAG_NONE=0;
struct D3D12_EXPORT_DESC { const wchar_t *Name, *ExportToRename; uint32_t Flags; };
struct Bytecode { const void *pShaderBytecode; size_t BytecodeLength; };
struct D3D12_DXIL_LIBRARY_DESC { Bytecode DXILLibrary; uint32_t NumExports; const D3D12_EXPORT_DESC *pExports; };
struct Subobject { uint32_t Type; const void *pDesc; };
struct D3D12_STATE_OBJECT_DESC { uint32_t Type, NumSubobjects; const Subobject *pSubobjects; };
#include "../d3d12_state_object_libraries.hpp"

using namespace dxmt::ray_library;
using Data = std::vector<uint8_t>;
static uint64_t checks=0;
static void Check(bool value) { ++checks; if(!value) throw std::runtime_error("check "+std::to_string(checks)); }
static void Put(Data &b, size_t at, uint32_t x) { for(unsigned j=0;j<4;++j) b.at(at+j)=uint8_t(x>>(j*8)); }
static uint32_t Word(const Data &b, size_t at) { return detail::Bytes{b.data(),b.size()}.word(at); }
static void Append(Data &b, uint32_t x) { size_t o=b.size(); b.resize(o+4); Put(b,o,x); }
static Data Words(std::initializer_list<uint32_t> v) { Data b; for(auto x:v) Append(b,x); return b; }
static uint32_t Text(Data &b, const char *s) { auto at=uint32_t(b.size()); b.insert(b.end(),s,s+std::strlen(s)+1); return at; }
static Data Parts(const std::vector<std::pair<uint32_t,Data>> &parts, bool container) {
  Data out(container?32:8,0);
  Put(out,0,container?0x43425844:0x10);
  Put(out,container?28:4,uint32_t(parts.size()));
  if(container)Put(out,20,1);
  const size_t offsets=out.size(); out.resize(out.size()+4*parts.size());
  for(size_t i=0;i<parts.size();++i){
    while(out.size()%4)out.push_back(0);
    Put(out,offsets+4*i,uint32_t(out.size()));
    Append(out,parts[i].first); Append(out,uint32_t(parts[i].second.size()));
    out.insert(out.end(),parts[i].second.begin(),parts[i].second.end());
  }
  if(container)Put(out,24,uint32_t(out.size())); return out;
}
static Data Fixture() {
  Data strings{0};
  const auto cb=Text(strings,"constants"),as=Text(strings,"scene"),tex=Text(strings,"output");
  const auto rg=Text(strings,"?ray@@entry"),rgu=Text(strings,"ray"),miss=Text(strings,"?miss@@entry"),mu=Text(strings,"miss"),dep=Text(strings,"external_helper");
  while(strings.size()%4)strings.push_back(0);
  auto resources=Words({3,32,2,13,0,3,1,1,cb,0, 0,16,0,5,64,64,as,8, 1,2,0,5,0,3,tex,1});
  auto functions=Words({2,44,rg,rgu,0,4,7,0,0,1,2,128,0x70063,
                       miss,mu,UINT32_MAX,UINT32_MAX,11,16,0,0,0,2048,0xb0063});
  auto rdat=Parts({{1,strings},{3,resources},{4,functions},{2,Words({3,0,1,2,1,dep})}},false);
  return Parts({{0x4c495844,Words({0x60063,6,0x4c495844,0x100,16,0})},{0x54414452,rdat}},true);
}
static size_t RDAT(const Data &b) { return Word(b,36)+8; }
static size_t Part(const Data &b, unsigned index) { const auto r=RDAT(b); return r+Word(b,r+8+index*4)+8; }
static void Invalid(Data b) { Library l; Check(Parse(b.data(),b.size(),l)==Result::Invalid); Check(l.functions.empty()&&l.resources.empty()); }
static void SelfTest() {
  auto blob=Fixture(); Library l;
  Check(Parse(blob.data(),blob.size(),l)==Result::Ready);
  Check(l.resources.size()==3&&l.functions.size()==2&&!l.extended);
  Check(l.resources[0].type==2&&l.resources[0].space==3&&l.resources[0].lower==1);
  Check(l.resources[1].kind==16&&l.resources[1].lower==64&&l.resources[1].flags==8);
  Check(l.resources[2].upper==3&&l.resources[2].name=="output");
  Check(l.functions[0].name=="?ray@@entry"&&l.functions[0].unmangled=="ray");
  Check(l.functions[0].kind==7&&l.functions[0].resources==std::vector<uint32_t>({0,1,2}));
  Check(l.functions[0].dependencies==std::vector<std::string>({"external_helper"}));
  Check(l.functions[0].features_low==1&&l.functions[0].features_high==2&&l.functions[0].target==0x70063);
  Check(l.functions[1].payload==16&&l.functions[1].kind==11&&l.functions[1].resources.empty());
  std::vector<SelectedExport> e;
  Check(Select(l,{},e)==Result::Ready&&e.size()==2);
  Check(Select(l,{{"renamed","ray"},{"alias","?ray@@entry"},{"miss",""}},e)==Result::Ready);
  Check(e.size()==3&&e[0].function==0&&e[1].function==0&&e[2].function==1);
  Check(Select(l,{{"unknown",""}},e)==Result::Invalid&&e.empty());
  Check(Select(l,{{"same","ray"},{"same","miss"}},e)==Result::Invalid&&e.empty());
  Check(Select(l,{{"","ray"}},e)==Result::Invalid);
  auto overload=l; overload.functions[1].unmangled="ray";
  Check(Select(overload,{{"ray",""}},e)==Result::Unsupported);
  Check(Select(overload,{{"explicit","?ray@@entry"}},e)==Result::Ready);
  overload.extended=true; Check(Select(overload,{},e)==Result::Unsupported&&e.empty());
  Prepared owned; Check(Prepare(blob.data(),blob.size(),{},owned)==Result::Ready);
  auto saved=blob;std::fill(blob.begin(),blob.end(),0);Check(owned.bytecode==saved&&owned.reflection.functions[0].unmangled=="ray");blob=saved;
  for(size_t n=0;n<blob.size();++n){Library v;Check(Parse(blob.data(),n,v)==Result::Invalid);Check(v.functions.empty());}
  for(auto [offset,value]:std::vector<std::pair<size_t,uint32_t>>{
      {0,0},{20,2},{24,UINT32_MAX},{28,UINT32_MAX},{32,1},{36,Word(blob,32)},
      {RDAT(blob)+4,UINT32_MAX},{RDAT(blob)+8,4},{Part(blob,1),UINT32_MAX},{Part(blob,1)+4,28},
      {Part(blob,1)+8,4},{Part(blob,1)+8+16,2},{Part(blob,1)+8+24,UINT32_MAX-1},
      {Part(blob,2)+4,40},{Part(blob,2)+8,UINT32_MAX},{Part(blob,2)+8+8,UINT32_MAX-1},
      {Part(blob,3),UINT32_MAX},{Part(blob,3)+4,3},{Part(blob,3)+20,UINT32_MAX}}){auto b=blob;Put(b,offset,value);Invalid(b);}
  auto b=blob;Put(b,RDAT(b),17);Check(Parse(b.data(),b.size(),l)==Result::Unsupported&&l.functions.empty());
  b=blob;b[Part(b,0)+1]=0x80;Invalid(b);
  b=blob;b[Part(b,0)+1]=0xc0;b[Part(b,0)+2]=0xaf;Invalid(b);
  b=blob;b[Part(b,0)+1]=0xed;b[Part(b,0)+2]=0xa0;b[Part(b,0)+3]=0x80;Invalid(b);
  b=blob;Put(b,Word(b,36),0x12345678);Check(Parse(b.data(),b.size(),l)==Result::Missing);
  b=blob;Put(b,Part(b,3)-8,6);Check(Parse(b.data(),b.size(),l)==Result::Invalid); // referenced index table absent
  std::string name;
  Check(Utf8(L"ray",name)&&name=="ray");
  const wchar_t wide[]={0x41,0x416,0xd83d,0xde00,0};
  Check(Utf8(wide,name)&&name==std::string("A\xd0\x96\xf0\x9f\x98\x80"));
  const wchar_t bad[]={0xd800,0};Check(!Utf8(bad,name)&&name.empty());
  const wchar_t low[]={0xdc00,0};Check(!Utf8(low,name));Check(!Utf8(nullptr,name));
  D3D12_DXIL_LIBRARY_DESC input{{blob.data(),blob.size()},0,nullptr};
  Subobject node{5,&input};D3D12_STATE_OBJECT_DESC desc{3,1,&node};
  std::vector<dxmt::StateObjectLibrary> snapshots;
  Check(dxmt::PrepareStateObjectLibraries(desc,snapshots)==S_OK&&snapshots.size()==1&&snapshots[0].prepared.exports.size()==2);
  desc.Type=2;Check(dxmt::PrepareStateObjectLibraries(desc,snapshots)==E_INVALIDARG&&snapshots.empty());desc.Type=3;
  desc.pSubobjects=nullptr;Check(dxmt::PrepareStateObjectLibraries(desc,snapshots)==E_INVALIDARG);desc.pSubobjects=&node;
  node.pDesc=nullptr;Check(dxmt::PrepareStateObjectLibraries(desc,snapshots)==E_INVALIDARG);node.pDesc=&input;
  D3D12_EXPORT_DESC alias{L"renamed",L"ray",0};input.NumExports=1;input.pExports=&alias;
  Check(dxmt::PrepareStateObjectLibraries(desc,snapshots)==S_OK&&snapshots[0].prepared.exports[0].name=="renamed");
  alias.Flags=1;Check(dxmt::PrepareStateObjectLibraries(desc,snapshots)==E_INVALIDARG);alias.Flags=0;
  input.pExports=nullptr;Check(dxmt::PrepareStateObjectLibraries(desc,snapshots)==E_INVALIDARG);input.pExports=&alias;
  Subobject two[]={node,node};desc.NumSubobjects=2;desc.pSubobjects=two;
  Check(dxmt::PrepareStateObjectLibraries(desc,snapshots)==E_INVALIDARG&&snapshots.empty());
  Library many;std::vector<Export> requested;
  for(unsigned i=0;i<10000;++i){Function f{};f.name="decorated"+std::to_string(i);f.unmangled="function"+std::to_string(i);many.functions.push_back(f);requested.push_back({"alias"+std::to_string(i),f.unmangled});}
  Check(Select(many,requested,e)==Result::Ready&&e.size()==10000);
  for(unsigned i=0;i<e.size();++i)Check(e[i].function==i);
  uint32_t rng=0x513a9041;
  for(unsigned i=0;i<50000;++i){
    auto v=blob; rng^=rng<<13;rng^=rng>>17;rng^=rng<<5; v[rng%v.size()]^=uint8_t(1u<<(rng%8));
    Library output;const auto result=Parse(v.data(),v.size(),output);
    if(result!=Result::Ready)Check(output.functions.empty()&&output.resources.empty());
    else for(const auto &f:output.functions)for(auto id:f.resources)Check(id<output.resources.size());
  }
  std::cout<<"RAY_LIBRARY_CPU_PASS checks="<<checks<<" mutations=50000 gpu=0\n";
}
static std::string Hex(const std::string &s) { std::string r;for(uint8_t c:s){r+="0123456789abcdef"[c>>4];r+="0123456789abcdef"[c&15];}return r; }
static void Reflect(const std::string &file) {
  std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("input absent");
  Data data((std::istreambuf_iterator<char>(input)),{});Prepared p;
  const auto status=Prepare(data.data(),data.size(),{},p);
  if(status!=Result::Ready)throw std::runtime_error(file+" prepare status "+std::to_string(int(status)));
  Check(true);
  auto &l=p.reflection;
  std::cout<<"{\"id\":\""<<file.substr(file.find_last_of('/')+1)<<"\",\"resources\":[";
  for(size_t i=0;i<l.resources.size();++i){const auto &r=l.resources[i];if(i)std::cout<<',';
    std::cout<<'['<<r.type<<','<<r.kind<<','<<r.id<<','<<r.space<<','<<r.lower<<','<<r.upper<<','<<r.flags<<",\""<<Hex(r.name)<<"\"]";}
  std::cout<<"],\"functions\":[";
  for(size_t i=0;i<l.functions.size();++i){const auto &f=l.functions[i];if(i)std::cout<<',';
    std::cout<<"{\"name\":\""<<Hex(f.name)<<"\",\"unmangled\":\""<<Hex(f.unmangled)<<"\",\"info\":["<<f.kind<<','<<f.payload<<','<<f.attributes<<','<<f.features_low<<','<<f.features_high<<','<<f.stages<<','<<f.target<<"],\"resources\":[";
    for(size_t j=0;j<f.resources.size();++j){if(j)std::cout<<',';std::cout<<f.resources[j];}std::cout<<"],\"dependencies\":[";
    for(size_t j=0;j<f.dependencies.size();++j){if(j)std::cout<<',';std::cout<<'"'<<Hex(f.dependencies[j])<<'"';}std::cout<<"]}";}
  std::cout<<"],\"exports\":"<<p.exports.size()<<"}\n";
}
int main(int argc,char **argv) {
  try {if(argc==1)SelfTest();else{std::ifstream list(argv[1]);if(!list)throw std::runtime_error("list absent");std::string f;while(std::getline(list,f))if(!f.empty())Reflect(f);}return 0;}
  catch(const std::exception &e){std::cerr<<"RAY_LIBRARY_FAIL "<<e.what()<<'\n';return 1;}
}
