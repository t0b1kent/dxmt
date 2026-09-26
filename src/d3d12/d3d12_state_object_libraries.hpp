// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_raytracing_library.hpp"
#include <new>

namespace dxmt {
struct StateObjectLibrary {
  uint32_t subobject;
  ray_library::Prepared prepared;
};

// First phase only: own library bytecode, reflected bindings and export renames.
// Other subobjects and executable linking remain the state-object builder's job.
inline HRESULT PrepareStateObjectLibraries(const D3D12_STATE_OBJECT_DESC &desc,
                                          std::vector<StateObjectLibrary> &out) {
  out.clear();
  if ((desc.Type != D3D12_STATE_OBJECT_TYPE_COLLECTION &&
       desc.Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) ||
      (desc.NumSubobjects && !desc.pSubobjects)) return E_INVALIDARG;
  try {
    std::vector<StateObjectLibrary> result;
    std::vector<std::string> names;
    for (uint32_t i = 0; i < desc.NumSubobjects; ++i) {
      const auto &subobject = desc.pSubobjects[i];
      if (subobject.Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) continue;
      if (!subobject.pDesc) return E_INVALIDARG;
      const auto &library = *static_cast<const D3D12_DXIL_LIBRARY_DESC *>(subobject.pDesc);
      if (library.NumExports && !library.pExports) return E_INVALIDARG;
      std::vector<ray_library::Export> exports;
      for (uint32_t j = 0; j < library.NumExports; ++j) {
        const auto &input = library.pExports[j];
        ray_library::Export e;
        if (input.Flags != D3D12_EXPORT_FLAG_NONE ||
            !ray_library::Utf8(input.Name, e.name) || e.name.empty() ||
            (input.ExportToRename && (!ray_library::Utf8(input.ExportToRename, e.source) || e.source.empty())))
          return E_INVALIDARG;
        exports.push_back(std::move(e));
      }
      StateObjectLibrary owned{i, {}};
      const auto status = ray_library::Prepare(library.DXILLibrary.pShaderBytecode,
                                               library.DXILLibrary.BytecodeLength, exports, owned.prepared);
      if (status == ray_library::Result::Invalid) return E_INVALIDARG;
      if (status != ray_library::Result::Ready) return DXGI_ERROR_UNSUPPORTED;
      for (const auto &e : owned.prepared.exports) names.push_back(e.name);
      result.push_back(std::move(owned));
    }
    std::sort(names.begin(), names.end());
    if (std::adjacent_find(names.begin(), names.end()) != names.end()) return E_INVALIDARG;
    out = std::move(result);
    return S_OK;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}
} // namespace dxmt
