// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include "../d3d12_raytracing_state_plan.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>

using HRESULT=int32_t;
constexpr HRESULT S_OK=0,E_INVALIDARG=-1,DXGI_ERROR_UNSUPPORTED=-2,E_OUTOFMEMORY=-3;
constexpr uint32_t D3D12_STATE_OBJECT_TYPE_COLLECTION=0,D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE=3,D3D12_EXPORT_FLAG_NONE=0;
enum {
  D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG=0,D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE=1,
  D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE=2,D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK=3,
  D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY=5,D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION=7,
  D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG=9,D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG=10,
  D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP=11,D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1=12
};
struct ID3D12RootSignature { uint32_t key;bool local; };
struct D3D12_EXPORT_DESC { const wchar_t *Name,*ExportToRename;uint32_t Flags; };
struct Bytecode { const void *pShaderBytecode;size_t BytecodeLength; };
struct D3D12_DXIL_LIBRARY_DESC { Bytecode DXILLibrary;uint32_t NumExports;const D3D12_EXPORT_DESC *pExports; };
struct D3D12_STATE_SUBOBJECT { uint32_t Type;const void *pDesc; };
struct D3D12_STATE_OBJECT_DESC { uint32_t Type,NumSubobjects;const D3D12_STATE_SUBOBJECT *pSubobjects; };
struct D3D12_LOCAL_ROOT_SIGNATURE { ID3D12RootSignature *pLocalRootSignature; };
struct D3D12_GLOBAL_ROOT_SIGNATURE { ID3D12RootSignature *pGlobalRootSignature; };
struct D3D12_RAYTRACING_SHADER_CONFIG { uint32_t MaxPayloadSizeInBytes,MaxAttributeSizeInBytes; };
struct D3D12_RAYTRACING_PIPELINE_CONFIG { uint32_t MaxTraceRecursionDepth; };
struct D3D12_RAYTRACING_PIPELINE_CONFIG1 { uint32_t MaxTraceRecursionDepth,Flags; };
struct D3D12_HIT_GROUP_DESC { const wchar_t *HitGroupExport;uint32_t Type;const wchar_t *AnyHitShaderImport,*ClosestHitShaderImport,*IntersectionShaderImport; };
struct D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION { const D3D12_STATE_SUBOBJECT *pSubobjectToAssociate;uint32_t NumExports;const wchar_t *const *pExports; };
struct D3D12_STATE_OBJECT_CONFIG { uint32_t Flags; };
struct D3D12_NODE_MASK { uint32_t NodeMask; };
#include "../d3d12_state_object_associations.hpp"

using namespace dxmt::state_plan;
static uint64_t checks=0;
static void Check(bool ok) { ++checks;if(!ok)throw std::runtime_error("check "+std::to_string(checks)); }
static Input Fixture() {
  Input x;
  x.items={{GlobalRoot,10,0,0},{LocalRoot,20,0,0},{ShaderConfig,None,16,8},{PipelineConfig,None,2,0},{Other,None,0,0}};
  x.shaders={{"rg","",7,0,0,0,0,false},{"ah","",9,4,8,0,1,false},{"ch_mangled","ch",10,12,8,0,2,false},{"ms","",11,12,0,0,3,false}};
  x.hits={{"hit",0,{"ah","ch",""}}};
  return x;
}
static void Expect(const Input &x,Status expected) { Plan p;p.items.push_back({});Check(Resolve(x,p)==expected);if(expected!=Status::Ready)Check(p.items.empty()&&p.shaders.empty()&&p.hit_imports.empty()); }
static std::vector<uint32_t> Semantic(const Plan &p) {
  std::vector<uint32_t> out;
  for(const auto *list:{&p.shader_bindings,&p.hit_bindings})for(const auto &node:*list)for(auto i:node){
    if(i==None){out.insert(out.end(),{None,None,None,None});continue;}
    const auto &s=p.items.at(i);out.insert(out.end(),{s.kind,s.key,s.first,s.second});
  }
  return out;
}
static void CoreTests() {
  auto x=Fixture();Plan p;Check(Resolve(x,p)==Status::Ready);Check(p.shader_bindings.size()==4&&p.hit_bindings.size()==1);
  Check(p.hit_imports[0]==std::array<uint32_t,3>{1,2,None});
  for(const auto &b:p.shader_bindings)Check(b==Bindings{0,1,2,3});
  x.associations={{1,{"hit"}}};Check(Resolve(x,p)==Status::Ready);
  Check(p.shader_bindings[0][LocalRoot]==None&&p.shader_bindings[3][LocalRoot]==None);
  Check(p.shader_bindings[1][LocalRoot]==1&&p.shader_bindings[2][LocalRoot]==1&&p.hit_bindings[0][LocalRoot]==1);
  x.associations.push_back({1,{}});Check(Resolve(x,p)==Status::Ready&&p.shader_bindings[0][LocalRoot]==1);
  x=Fixture();x.items.push_back({LocalRoot,21,0,0});x.associations={{5,{"hit"}}};Check(Resolve(x,p)==Status::Ready);
  Check(p.shader_bindings[0][LocalRoot]==1&&p.shader_bindings[1][LocalRoot]==5);
  x.associations.push_back({1,{"ah"}});Expect(x,Status::Unsupported);
  x.items[5].key=20;Expect(x,Status::Ready); // Distinct subobjects with a canonical-identical root.
  x=Fixture();x.items.push_back({GlobalRoot,30,0,0});x.associations={{5,{"rg"}}};Expect(x,Status::Ready);
  x=Fixture();x.items.push_back({ShaderConfig,None,12,8});x.associations={{5,{"rg"}}};Expect(x,Status::Invalid);
  x.items[5].first=16;Expect(x,Status::Ready);
  x=Fixture();x.items[2].kind=Other;Expect(x,Status::Invalid);
  x=Fixture();x.items[3].kind=Other;Expect(x,Status::Invalid);
  x=Fixture();x.items[2].second=33;Expect(x,Status::Invalid);
  x=Fixture();x.items[3].first=32;Expect(x,Status::Invalid);
  x=Fixture();x.items[3].second=0x100;Expect(x,Status::Ready);
  x.items[3].second=1;Expect(x,Status::Unsupported);
  x=Fixture();x.shaders[2].payload=17;Expect(x,Status::Invalid);
  x=Fixture();x.shaders[2].attributes=9;Expect(x,Status::Invalid);
  x=Fixture();x.shaders[3].kind=12;x.shaders[3].payload=1000;Expect(x,Status::Ready); // Callable payload not limited by shader config.
  x=Fixture();x.shaders[0].kind=6;Expect(x,Status::Unsupported);
  x=Fixture();x.shaders[0].external_dependencies=true;Expect(x,Status::Unsupported);
  x=Fixture();x.hits[0].imports[1]="rg";Expect(x,Status::Invalid);
  x=Fixture();x.hits[0].imports[0]="absent";Expect(x,Status::Invalid);
  x=Fixture();x.hits[0].type=1;Expect(x,Status::Invalid);
  x.shaders.push_back({"intersection","",8,0,0,0,4,false});x.hits[0].imports[2]="intersection";Expect(x,Status::Ready);
  x.hits[0].type=0;Expect(x,Status::Invalid);
  x=Fixture();x.hits[0].type=2;Expect(x,Status::Invalid);
  x=Fixture();x.hits[0].imports={"","",""};Expect(x,Status::Ready);
  x=Fixture();x.hits[0].name="rg";Expect(x,Status::Invalid);
  x=Fixture();x.associations={{4,{"rg"}}};Expect(x,Status::Invalid);
  x.associations={{99,{"rg"}}};Expect(x,Status::Invalid);
  x.associations={{1,{"unknown"}}};Expect(x,Status::Invalid);
  x.associations={{1,{""}}};Expect(x,Status::Invalid);
  x=Fixture();x.shaders[0].alias="ch";x.associations={{1,{"ch"}}};Expect(x,Status::Unsupported);
  x.associations={{1,{"ch_mangled"}}};x.hits[0].imports[1]="ch_mangled";Expect(x,Status::Unsupported); // Other hit component has no matching local root.
  x.associations={{1,{"hit"}}};Expect(x,Status::Ready);
  x=Fixture();x.hits.push_back({"second",0,{"ah","ch",""}});x.items.push_back({LocalRoot,21,0,0});x.associations={{1,{"hit"}},{5,{"second"}}};Expect(x,Status::Unsupported);
  x.items[5].key=20;Expect(x,Status::Ready);
  x=Fixture();x.items.push_back({LocalRoot,99,0,0});Expect(x,Status::Unsupported); // Competing defaults.
  x.items[5].key=20;Expect(x,Status::Ready);
  x=Fixture();Check(Resolve(x,p)==Status::Ready);auto before=Semantic(p);x.items.clear();x.shaders.clear();Check(Semantic(p)==before&&p.shaders[2].name=="ch_mangled");
  std::mt19937 rng(0x79a351);
  for(unsigned iteration=0;iteration<5000;++iteration){
    x=Fixture();x.items.push_back({LocalRoot,21,0,0});x.associations={{5,{"hit"}},{0,{"rg","ah","ch","ms","hit"}},{1,{}}};
    Plan reference;Check(Resolve(x,reference)==Status::Ready);
    std::vector<uint32_t> order(x.items.size());for(uint32_t i=0;i<order.size();++i)order[i]=i;
    std::shuffle(order.begin(),order.end(),rng);auto original=x.items;std::vector<uint32_t> inverse(order.size());
    for(uint32_t i=0;i<order.size();++i){x.items[i]=original[order[i]];inverse[order[i]]=i;}
    for(auto &a:x.associations)a.target=inverse[a.target];std::shuffle(x.associations.begin(),x.associations.end(),rng);
    Check(Resolve(x,p)==Status::Ready);Check(Semantic(p)==Semantic(reference));
  }
  for(unsigned i=0;i<20000;++i){x=Fixture();x.associations={{rng()%8,{(rng()%3) ? "hit" : "invalid"}}};x.shaders[rng()%4].kind=rng()%16;auto status=Resolve(x,p);
    if(status==Status::Ready){for(const auto &node:p.shader_bindings)for(auto target:node)Check(target==None||target<p.items.size());}
    else Check(p.items.empty()&&p.shaders.empty()&&p.hit_bindings.empty());
  }
}
static void AdapterTests() {
  const auto fixture=Fixture();std::vector<dxmt::StateObjectLibrary> libraries(1);libraries[0].subobject=0;
  for(const auto &s:fixture.shaders){dxmt::ray_library::Function f{};f.name=s.name;f.unmangled=s.alias;f.kind=s.kind;f.payload=s.payload;f.attributes=s.attributes;auto &prepared=libraries[0].prepared;prepared.exports.push_back({s.name,uint32_t(prepared.reflection.functions.size())});prepared.reflection.functions.push_back(f);}
  D3D12_DXIL_LIBRARY_DESC library{{nullptr,0},0,nullptr}; // Phase-one owned metadata supplied above; this phase does not re-read bytecode.
  ID3D12RootSignature local{7,true},global{8,false};D3D12_LOCAL_ROOT_SIGNATURE lr{&local};D3D12_GLOBAL_ROOT_SIGNATURE gr{&global};
  D3D12_RAYTRACING_SHADER_CONFIG sc{16,8};D3D12_RAYTRACING_PIPELINE_CONFIG pc{2};D3D12_HIT_GROUP_DESC hit{L"hit",0,L"ah",L"ch",nullptr};
  D3D12_STATE_OBJECT_CONFIG config{0};D3D12_NODE_MASK mask{0};const wchar_t *names[]={L"hit"};
  D3D12_STATE_SUBOBJECT nodes[9]={{5,&library},{2,&lr},{1,&gr},{9,&sc},{10,&pc},{11,&hit},{7,nullptr},{0,&config},{3,&mask}};
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{&nodes[1],1,names};nodes[6].pDesc=&association;
  D3D12_STATE_OBJECT_DESC desc{3,9,nodes};Plan p;
  auto capture=[](ID3D12RootSignature *r,bool local,uint32_t &key)->HRESULT {if(!r)return DXGI_ERROR_UNSUPPORTED;if(r->local!=local)return E_INVALIDARG;key=r->key;return S_OK;};
  auto run=[&](HRESULT expected){Check(dxmt::PrepareStateObjectAssociations(desc,libraries,capture,p)==expected);if(expected!=S_OK)Check(p.items.empty()&&p.shaders.empty());};
  run(S_OK);Check(p.hit_imports[0][1]==2&&p.shader_bindings[0][LocalRoot]==None&&p.shader_bindings[1][LocalRoot]==1);
  std::swap(nodes[1],nodes[6]);association.pSubobjectToAssociate=&nodes[6];run(S_OK);Check(p.hit_bindings[0][LocalRoot]==6);
  std::swap(nodes[1],nodes[6]);association.pSubobjectToAssociate=&nodes[1];
  association.pSubobjectToAssociate=nodes+9;run(E_INVALIDARG);
  association.pSubobjectToAssociate=reinterpret_cast<const D3D12_STATE_SUBOBJECT *>(reinterpret_cast<uintptr_t>(nodes)+1);run(E_INVALIDARG);
  association.pSubobjectToAssociate=reinterpret_cast<const D3D12_STATE_SUBOBJECT *>(reinterpret_cast<uintptr_t>(nodes)-sizeof(nodes[0]));run(E_INVALIDARG);
  association.pSubobjectToAssociate=&nodes[6];run(E_INVALIDARG);
  association.pSubobjectToAssociate=&nodes[0];run(E_INVALIDARG);
  association.pSubobjectToAssociate=&nodes[1];association.pExports=nullptr;run(E_INVALIDARG);association.pExports=names;
  names[0]=L"absent";run(E_INVALIDARG);names[0]=L"hit";
  local.local=false;run(E_INVALIDARG);local.local=true;
  lr.pLocalRootSignature=nullptr;run(DXGI_ERROR_UNSUPPORTED);lr.pLocalRootSignature=&local;
  config.Flags=1;run(DXGI_ERROR_UNSUPPORTED);config.Flags=0;
  mask.NodeMask=2;run(DXGI_ERROR_UNSUPPORTED);mask.NodeMask=1;run(S_OK);
  nodes[8].Type=6;run(DXGI_ERROR_UNSUPPORTED);nodes[8].Type=3;
  desc.Type=0;run(DXGI_ERROR_UNSUPPORTED);desc.Type=2;run(E_INVALIDARG);desc.Type=3;
  desc.pSubobjects=nullptr;run(E_INVALIDARG);desc.pSubobjects=nodes;
  auto old=nodes[5].pDesc;nodes[5].pDesc=nullptr;run(E_INVALIDARG);nodes[5].pDesc=old;
  libraries[0].subobject=7;run(E_INVALIDARG);libraries[0].subobject=0;
  libraries[0].prepared.exports[0].function=999;run(E_INVALIDARG);libraries[0].prepared.exports[0].function=0;
  D3D12_RAYTRACING_PIPELINE_CONFIG1 pc1{2,0x200};nodes[4]={12,&pc1};run(S_OK);pc1.Flags=1;run(DXGI_ERROR_UNSUPPORTED);
  nodes[4]={10,&pc};pc.MaxTraceRecursionDepth=32;run(E_INVALIDARG);pc.MaxTraceRecursionDepth=2;
  sc.MaxAttributeSizeInBytes=33;run(E_INVALIDARG);sc.MaxAttributeSizeInBytes=8;
  run(S_OK);local.key=99;names[0]=L"changed";Check(p.items[1].key==7&&p.hits[0].name=="hit");
}
static void Corpus(const char *file_list) {
  std::ifstream list(file_list);Check(bool(list));std::string file;uint32_t count=0;uint64_t resources=0;
  while(std::getline(list,file)) {
    if(file.empty())continue;
    std::ifstream source(file,std::ios::binary);Check(bool(source));
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(source)),{});
    dxmt::ray_library::Prepared library;Check(dxmt::ray_library::Prepare(bytes.data(),bytes.size(),{},library)==dxmt::ray_library::Result::Ready);
    Input input;input.items={{GlobalRoot,10,0,0},{LocalRoot,20,0,0},{ShaderConfig,None,16,8},{PipelineConfig,None,2,0}};
    for(const auto &e:library.exports){const auto &f=library.reflection.functions[e.function];
      input.shaders.push_back({e.name,f.unmangled,f.kind,f.payload,f.attributes,0,e.function,!f.dependencies.empty()});resources+=f.resources.size();
      if(f.kind==9||f.kind==10){Hit h;h.name="test-hit-"+std::to_string(input.hits.size());h.imports[f.kind==9?0:1]=e.name;input.associations.push_back({1,{h.name}});input.hits.push_back(h);}
      else input.associations.push_back({1,{e.name}});
    }
    Plan plan;if(Resolve(input,plan)!=Status::Ready)throw std::runtime_error("corpus association "+file);
    for(const auto &binding:plan.shader_bindings)Check(binding==Bindings{0,1,2,3});
    ++count;
  }
  Check(count==4281);Check(resources==31485);
  std::cout<<"STATE_PLAN_CORPUS_PASS libraries="<<count<<" resources="<<resources<<" checks="<<checks<<" syntheticAssociations=1 actualGameAssociations=0 gpu=0\n";
}
int main(int argc,char **argv) {
  try{if(argc>1)Corpus(argv[1]);else{CoreTests();AdapterTests();std::cout<<"STATE_PLAN_CPU_PASS checks="<<checks<<" permutations=5000 mutations=20000 gpu=0\n";}return 0;}
  catch(const std::exception &e){std::cerr<<"STATE_PLAN_FAIL "<<e.what()<<'\n';return 1;}
}
