// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
// DXIL metadata layout: Microsoft DirectXShaderCompiler, DxilMetadataHelper.h.
// This offline LLVM adapter exports original requirements; it does not compile a pipeline.
#include "../d3d12_compiler_resource.hpp"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <set>
using namespace llvm;
using dxmt::compiler_resource::Footprint;
static bool UInt(Metadata *m, uint32_t &out) {
  auto *v=dyn_cast_or_null<ConstantAsMetadata>(m);
  auto *n=v?dyn_cast<ConstantInt>(v->getValue()):nullptr;
  if (!n || n->getBitWidth()>32) return false;
  out=uint32_t(n->getZExtValue());return true;
}
static bool UInt(const MDNode *n,unsigned i,uint32_t &out) {
  return n && i<n->getNumOperands() && UInt(n->getOperand(i).get(),out);
}
static bool Extract(Module &module, std::vector<Footprint> &out, std::string &error) {
  auto fail=[&](const char *s){error=s;return false;};
  auto *named=module.getNamedMetadata("dx.resources");
  if (!named) return true;
  if (named->getNumOperands()!=1) return fail("resource root count");
  auto *root=named->getOperand(0);
  if (root->getNumOperands()!=4) return fail("resource class count");
  std::set<std::pair<uint32_t,uint32_t>> identities;
  for(unsigned type=0;type<4;++type) {
    auto *raw=root->getOperand(type).get();
    auto *list=dyn_cast_or_null<MDNode>(raw);
    if (!raw) continue;
    if (!list || list->getNumOperands()>65536) return fail("resource class list");
    for(const auto &item:list->operands()) {
      auto *n=dyn_cast_or_null<MDNode>(item.get());
      const unsigned fields=type==0?9:type==1?11:8, tagsIndex=fields-1;
      if (!n || n->getNumOperands()!=fields) return fail("resource field count");
      Footprint f;f.resource.type=type;
      uint32_t count=0;
      if (!UInt(n,0,f.resource.id)||!UInt(n,3,f.resource.space)||!UInt(n,4,f.resource.lower)||!UInt(n,5,count)||!count)
        return fail("resource identity/range");
      if (!identities.emplace(type,f.resource.id).second) return fail("duplicate resource id");
      if (count==UINT32_MAX) f.resource.upper=UINT32_MAX;
      else {
        if (count-1>UINT32_MAX-f.resource.lower) return fail("resource range overflow");
        f.resource.upper=f.resource.lower+count-1;
      }
      auto *name=dyn_cast_or_null<MDString>(n->getOperand(2).get());
      if (!name) return fail("resource name");
      f.resource.name=name->getString().str();
      if (type<2) {
        if (!UInt(n,6,f.resource.kind)) return fail("resource kind");
        if (!type) {
          if (!UInt(n,7,f.samples)) return fail("sample count");
          // Non-MS declarations carry sample count zero in original DXIL.
          if (f.resource.kind!=3 && f.resource.kind!=8 && f.samples) return fail("non-MS sample count");
        } else {
          uint32_t coherent=0,counter=0,rov=0;
          if(!UInt(n,7,coherent)||!UInt(n,8,counter)||!UInt(n,9,rov)||coherent>1||counter>1||rov>1)
            return fail("UAV flags");
          f.resource.flags=coherent|(counter<<1)|(rov<<2);
        }
      } else if (type==2) {
        f.resource.kind=13;if(!UInt(n,6,f.cbuffer_bytes)) return fail("cbuffer size");
      } else {
        f.resource.kind=14;if(!UInt(n,6,f.sampler_kind)) return fail("sampler kind");
      }
      auto *tagValue=n->getOperand(tagsIndex).get();
      auto *tags=dyn_cast_or_null<MDNode>(tagValue);
      if (tagValue && (!tags || tags->getNumOperands()>64 || tags->getNumOperands()%2)) return fail("extended tags");
      std::set<uint32_t> seen;
      if(tags) for(unsigned i=0;i<tags->getNumOperands();i+=2) {
        uint32_t tag=0,value=0;
        if(!UInt(tags,i,tag)||!UInt(tags,i+1,value)||!seen.insert(tag).second) return fail("extended tag value");
        switch(tag) {
        case 0:f.component=value;break;
        case 1:f.structured_stride=value;break;
        case 3:
          if(value>1) return fail("atomic64 flag");
          f.resource.flags|=value<<3;break;
        default:f.unknown_properties=true;break;
        }
      }
      out.push_back(std::move(f));
    }
  }
  return true;
}
static json::Object Run(StringRef data, StringRef path) {
  json::Object row{{"file",path}};
  auto fail=[&](StringRef error){row["status"]="REJECTED";row["error"]=error;return std::move(row);};
  dxmt::ray_library::Library rdat;
  if(dxmt::ray_library::Parse(data.data(),data.size(),rdat)!=dxmt::ray_library::Result::Ready)
    return fail("RDAT/container parse failed");
  auto u32=[&](size_t p){auto *b=reinterpret_cast<const uint8_t*>(data.data()+p);
    return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);};
  StringRef bitcode;
  for(uint32_t i=0;i<u32(28);++i) {
    size_t p=u32(32+4*i);
    if(data.substr(p,4)!="DXIL")continue;
    const uint64_t length=u32(p+4);
    if(!bitcode.empty()||length<24||data.substr(p+16,4)!="DXIL")return fail("DXIL chunk");
    const uint64_t offset=u32(p+24),bytes=u32(p+28);
    if(offset<16||offset>length-8||bytes>length-8-offset)return fail("bitcode bounds");
    bitcode=data.substr(p+16+offset,bytes);
  }
  if(bitcode.empty())return fail("DXIL bitcode missing");
  LLVMContext context;
  auto module=parseBitcodeFile(MemoryBufferRef(bitcode,path),context);
  if(!module){consumeError(module.takeError());return fail("LLVM parse failed");}
  std::vector<Footprint> facts;std::string error;
  if(!Extract(**module,facts,error))return fail(error);
  if(facts.size()!=rdat.resources.size())return fail("DXIL/RDAT resource count");
  json::Array resources;
  for(const auto &f:facts) {
    const auto &r=f.resource;
    const auto it=std::find_if(rdat.resources.begin(),rdat.resources.end(),[&](const auto &q){return q.type==r.type&&q.id==r.id;});
    if(it==rdat.resources.end()||!dxmt::compiler_resource::SameResource(r,*it))return fail("DXIL/RDAT identity mismatch");
    const auto support=dxmt::compiler_resource::Validate(f);
    resources.push_back(json::Object{{"type",r.type},{"id",r.id},{"kind",r.kind},{"space",r.space},
      {"lower",r.lower},{"upper",r.upper},{"flags",r.flags},{"name",r.name},{"cbufferBytes",f.cbuffer_bytes},
      {"structuredStride",f.structured_stride},{"component",f.component},{"samples",f.samples},
      {"samplerKind",f.sampler_kind},{"unknownProperties",f.unknown_properties},
      {"runtimeFootprintStatus",support==dxmt::capture::Status::Ready?"READY":support==dxmt::capture::Status::Invalid?"INVALID":"UNSUPPORTED"}});
  }
  row["status"]="DXIL_RDAT_MATCHED";row["resources"]=std::move(resources);return row;
}
int main() {
  std::string file;bool failed=false;
  while(std::getline(std::cin,file)) {
    auto input=MemoryBuffer::getFile(file);
    json::Object row;
    if(!input)row=json::Object{{"file",file},{"status","REJECTED"},{"error","input read failed"}};
    else if((*input)->getBufferSize()>64*1024*1024)row=json::Object{{"file",file},{"status","REJECTED"},{"error","input budget"}};
    else row=Run((*input)->getBuffer(),file);
    const auto status=row.getString("status");
    if(!status || *status!="DXIL_RDAT_MATCHED")failed=true;
    outs()<<json::Value(std::move(row))<<'\n';
  }
  return failed?1:0;
}
