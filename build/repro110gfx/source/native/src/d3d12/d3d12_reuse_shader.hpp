// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_device.hpp"
#include "sha1/sha1_util.hpp"

namespace dxmt::reuse {
inline std::shared_ptr<const ShaderInfo> QueryShader(D3D12_SHADER_BYTECODE bytecode) {
  static std::mutex mutex;
  static std::map<std::string,std::shared_ptr<const ShaderInfo>> cache;
  if (!bytecode.pShaderBytecode || !bytecode.BytecodeLength) return {};
  std::lock_guard lock(mutex);
  const auto key=Sha1HashState::compute(bytecode.pShaderBytecode,bytecode.BytecodeLength).string();
  if (auto i=cache.find(key);i!=cache.end()) return i->second;
  auto info=std::make_shared<ShaderInfo>();
  info->sha1=key; info->watched=Watched(key);
  if (cache.size()>=1024) { info->unknown=1;return info; }
  struct Native {
    sm50_shader_t shader=nullptr;sm50_error_t error=nullptr;sm50_bitcode_t bits=nullptr;
    ~Native() { if(bits)SM50DestroyBitcode(bits);if(error)SM50FreeError(error);if(shader)SM50Destroy(shader); }
  } n;
  MTL_SHADER_REFLECTION reflection{};
  if (SM50Initialize(bytecode.pShaderBytecode,bytecode.BytecodeLength,&n.shader,&reflection,&n.error) || !n.shader) info->unknown=1;
  else {
    auto query=std::make_unique<SM50_SHADER_REUSE_AUDIT_DATA>();
    query->type=SM50_SHADER_REUSE_AUDIT_QUERY;query->size=sizeof(*query);
    if (n.error) { SM50FreeError(n.error);n.error=nullptr; }
    if (SM50Compile(n.shader,reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(query.get()),
                    "reuse_query",&n.bits,&n.error) || query->applied!=SM50_REUSE_AUDIT_APPLIED) info->unknown=1;
    else {
      info->unknown=query->unknown+query->overflow;
      for (unsigned i=0;i<query->count && i<SM50_REUSE_AUDIT_ROWS;++i) {
        auto &r=query->rows[i];info->registers.push_back({r.stage,r.space,r.binding,r.reg,r.bound,r.flags});
      }
    }
  }
  cache.emplace(key,info);return info;
}
// Stage slots: graphics {VS,PS,HS,DS,GS} = 0..4; compute passes first_slot=5.
inline std::shared_ptr<const ShaderInfo> PipelineShaders(std::initializer_list<D3D12_SHADER_BYTECODE> shaders,
                                                         uint32_t first_slot = 0) {
  if (!Enabled()) return {};
  try {
    auto out=std::make_shared<ShaderInfo>();
    uint32_t slot=first_slot;
    for (auto bytes:shaders) if (const uint32_t stage=slot++; auto s=QueryShader(bytes)) {
      out->hashes.push_back({stage,s->sha1}); out->watched|=s->watched;
      out->unknown+=s->unknown;
      out->registers.insert(out->registers.end(),s->registers.begin(),s->registers.end());
    }
    return out;
  } catch (...) { return {}; }
}
} // namespace dxmt::reuse
