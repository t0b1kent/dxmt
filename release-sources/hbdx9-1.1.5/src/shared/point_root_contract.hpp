// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "../airconv/dxbc_root_signature.hpp"
#include <cstring>
#include <limits>

namespace point_root {
struct Requirement {
  D3D12_DESCRIPTOR_RANGE_TYPE type;
  uint32_t first, last, space, constant_bytes = 0;
  bool comparison = false;
  bool root_descriptor_allowed = true;
};

inline bool covers(const D3D12_ROOT_SIGNATURE_DESC1 &root, D3D12_SHADER_VISIBILITY stage,
                   const Requirement &need) {
  if (need.last < need.first || need.last == UINT32_MAX || stage < 1 || stage > 5) return false;
  const uint32_t denied[] = {0, D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS, D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS,
    D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS, D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS};
  if (uint32_t(root.Flags) & denied[stage]) return false;
  unsigned matches = 0;
  for (UINT i = 0; i < root.NumParameters; ++i) {
    const auto &p = root.pParameters[i];
    if (p.ShaderVisibility != D3D12_SHADER_VISIBILITY_ALL && p.ShaderVisibility != stage) continue;
    if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      for (UINT j = 0; j < p.DescriptorTable.NumDescriptorRanges; ++j) {
        const auto &r = p.DescriptorTable.pDescriptorRanges[j];
        if (r.RangeType == need.type && r.RegisterSpace == need.space && r.NumDescriptors &&
            r.BaseShaderRegister <= need.first &&
            (r.NumDescriptors == UINT32_MAX || uint64_t(r.BaseShaderRegister) + r.NumDescriptors > need.last)) ++matches;
      }
    } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      if (need.type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && need.first == need.last &&
          p.Constants.ShaderRegister == need.first && p.Constants.RegisterSpace == need.space &&
          need.constant_bytes && uint64_t(p.Constants.Num32BitValues) * 4 >= need.constant_bytes) ++matches;
    } else if (need.root_descriptor_allowed && need.first == need.last && p.Descriptor.ShaderRegister == need.first && p.Descriptor.RegisterSpace == need.space &&
               ((p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV && need.type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) ||
                (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV && need.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) ||
                (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV && need.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV))) ++matches;
  }
  if (need.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER && need.first == need.last)
    for (UINT i = 0; i < root.NumStaticSamplers; ++i) {
      const auto &s = root.pStaticSamplers[i];
      if ((s.ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL || s.ShaderVisibility == stage) &&
          s.ShaderRegister == need.first && s.RegisterSpace == need.space &&
          bool(uint32_t(s.Filter) & 0x80) == need.comparison) ++matches;
    }
  return matches == 1;
}

// SM5.0 RDEF is used here only after the real container/program parser succeeds.
// This checks application root visibility/capacity, not source authenticity.
inline bool rdef(const D3D12_ROOT_SIGNATURE_DESC1 &root, D3D12_SHADER_VISIBILITY stage,
                 const uint8_t *b, size_t size) {
  if (size < 28) return false;
  const auto word = [&](size_t at) { uint32_t v; memcpy(&v,b+at,4); return v; };
  const auto bounded = [&](uint64_t start,uint64_t count,uint64_t stride) { return start <= size && count <= 128 && count * stride <= size - start; };
  const uint32_t cbn=word(0), cbo=word(4), n=word(8), at=word(12);
  if (!bounded(at,n,32) || !bounded(cbo,cbn,24)) return false;
  const auto string = [&](uint32_t off) -> const char* { return off < size && memchr(b+off,0,size-off) ? reinterpret_cast<const char*>(b+off) : nullptr; };
  for (uint32_t i=0;i<n;++i) {
    const size_t p=at+32*i; const uint32_t type=word(p+4),first=word(p+20),count=word(p+24);
    if (!count || uint64_t(first)+count > UINT32_MAX) return false;
    Requirement need{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,first,first+count-1,0,0,false};
    if (type == 2 || type == 4) need.root_descriptor_allowed = false;
    if (type == 0) {
      need.type=D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
      const auto name=string(word(p));if(!name)return false;unsigned found=0;
      for(uint32_t j=0;j<cbn;++j){const auto other=string(word(cbo+24*j));if(!other)return false;
        if(!strcmp(name,other)){need.constant_bytes=word(cbo+24*j+12);++found;}}
      if(found!=1||!need.constant_bytes)return false;
    } else if(type==3){need.type=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;need.comparison=word(p+28)&2;}
    else if(type==4||type==6||type==8||type==9||type==10||type==11)need.type=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    else if(type!=2&&type!=5&&type!=7)return false;
    if(!covers(root,stage,need))return false;
  }
  return true;
}
} // namespace point_root
