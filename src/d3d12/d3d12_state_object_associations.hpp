// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_state_object_libraries.hpp"
#include "d3d12_raytracing_state_plan.hpp"
#include <set>

namespace dxmt {
// capture_root must retain each canonical root in the enclosing preparation
// owner and return its stable index. Plan indices never borrow API array pointers.
template<typename CaptureRoot>
HRESULT PrepareStateObjectAssociations(const D3D12_STATE_OBJECT_DESC &desc,
                                      const std::vector<StateObjectLibrary> &libraries,
                                      CaptureRoot capture_root, state_plan::Plan &out) {
  out = {};
  if (desc.Type == D3D12_STATE_OBJECT_TYPE_COLLECTION) return DXGI_ERROR_UNSUPPORTED;
  if (desc.Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE ||
      (desc.NumSubobjects && !desc.pSubobjects)) return E_INVALIDARG;
  try {
    state_plan::Input input;
    input.items.resize(desc.NumSubobjects);
    std::set<uint32_t> library_indices;
    for (uint32_t i = 0; i < libraries.size(); ++i) {
      const auto &library = libraries[i];
      if (library.subobject >= desc.NumSubobjects ||
          desc.pSubobjects[library.subobject].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY ||
          !library_indices.insert(library.subobject).second) return E_INVALIDARG;
      for (const auto &e : library.prepared.exports) {
        if (e.function >= library.prepared.reflection.functions.size()) return E_INVALIDARG;
        const auto &f = library.prepared.reflection.functions[e.function];
        input.shaders.push_back({e.name, e.name == f.name ? f.unmangled : std::string{},
            f.kind, f.payload, f.attributes, i, e.function, !f.dependencies.empty()});
      }
    }
    for (uint32_t i = 0; i < desc.NumSubobjects; ++i) {
      const auto &sub = desc.pSubobjects[i];
      auto &item = input.items[i];
      if (!sub.pDesc) return E_INVALIDARG;
      switch (sub.Type) {
      case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
        if (!library_indices.count(i)) return E_INVALIDARG;
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
      case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
        const bool local = sub.Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
        ID3D12RootSignature *root = local
            ? static_cast<const D3D12_LOCAL_ROOT_SIGNATURE *>(sub.pDesc)->pLocalRootSignature
            : static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>(sub.pDesc)->pGlobalRootSignature;
        item.kind = local ? state_plan::LocalRoot : state_plan::GlobalRoot;
        const HRESULT hr = capture_root(root, local, item.key);
        if (hr != S_OK) return hr;
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
        const auto &config = *static_cast<const D3D12_RAYTRACING_SHADER_CONFIG *>(sub.pDesc);
        item = {state_plan::ShaderConfig, state_plan::None, config.MaxPayloadSizeInBytes, config.MaxAttributeSizeInBytes};
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: {
        const auto &config = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG *>(sub.pDesc);
        item = {state_plan::PipelineConfig, state_plan::None, config.MaxTraceRecursionDepth, 0};
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1: {
        const auto &config = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1 *>(sub.pDesc);
        item = {state_plan::PipelineConfig, state_plan::None, config.MaxTraceRecursionDepth, uint32_t(config.Flags)};
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
        const auto &desc = *static_cast<const D3D12_HIT_GROUP_DESC *>(sub.pDesc);
        state_plan::Hit hit;
        hit.type = uint32_t(desc.Type);
        if (!ray_library::Utf8(desc.HitGroupExport, hit.name) || hit.name.empty()) return E_INVALIDARG;
        const wchar_t *imports[] = {desc.AnyHitShaderImport, desc.ClosestHitShaderImport, desc.IntersectionShaderImport};
        for (uint32_t j = 0; j < 3; ++j)
          if (imports[j] && (!ray_library::Utf8(imports[j], hit.imports[j]) || hit.imports[j].empty())) return E_INVALIDARG;
        input.hits.push_back(std::move(hit));
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
        const auto &desc_assoc = *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(sub.pDesc);
        if (!desc_assoc.pSubobjectToAssociate || (desc_assoc.NumExports && !desc_assoc.pExports)) return E_INVALIDARG;
        const auto base = reinterpret_cast<uintptr_t>(desc.pSubobjects);
        const auto address = reinterpret_cast<uintptr_t>(desc_assoc.pSubobjectToAssociate);
        if (address < base || (address - base) % sizeof(*desc.pSubobjects) ||
            (address - base) / sizeof(*desc.pSubobjects) >= desc.NumSubobjects) return E_INVALIDARG;
        state_plan::Association association{uint32_t((address - base) / sizeof(*desc.pSubobjects)), {}};
        for (uint32_t j = 0; j < desc_assoc.NumExports; ++j) {
          std::string name;
          if (!ray_library::Utf8(desc_assoc.pExports[j], name) || name.empty()) return E_INVALIDARG;
          association.exports.push_back(std::move(name));
        }
        input.associations.push_back(std::move(association));
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
        if (static_cast<const D3D12_STATE_OBJECT_CONFIG *>(sub.pDesc)->Flags != 0) return DXGI_ERROR_UNSUPPORTED;
        break;
      case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
        if (static_cast<const D3D12_NODE_MASK *>(sub.pDesc)->NodeMask > 1) return DXGI_ERROR_UNSUPPORTED;
        break;
      default:
        // Existing collections, named DXIL associations and future subobjects
        // need their own scope/link handling, not a silent skip.
        return DXGI_ERROR_UNSUPPORTED;
      }
    }
    const auto status = state_plan::Resolve(input, out);
    if (status == state_plan::Status::Invalid) return E_INVALIDARG;
    return status == state_plan::Status::Ready ? S_OK : DXGI_ERROR_UNSUPPORTED;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}
} // namespace dxmt
