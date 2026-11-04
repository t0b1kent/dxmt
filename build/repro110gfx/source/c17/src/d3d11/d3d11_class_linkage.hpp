#pragma once

#include "com/com_pointer.hpp"
#include "d3d11_device_child.hpp"

#include <string>
#include <unordered_map>

namespace dxmt {

class MTLD3D11Device;

class MTLD3D11ClassLinkage : public MTLD3D11DeviceChild<ID3D11ClassLinkage> {

public:
  MTLD3D11ClassLinkage(MTLD3D11Device *pDevice);

  ~MTLD3D11ClassLinkage();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;

  HRESULT STDMETHODCALLTYPE
  CreateClassInstance(LPCSTR pClassTypeName, UINT ConstantBufferOffset,
                      UINT ConstantVectorOffset, UINT TextureOffset,
                      UINT SamplerOffset, ID3D11ClassInstance **ppInstance);

  HRESULT STDMETHODCALLTYPE GetClassInstance(LPCSTR pClassInstanceName,
                                             UINT InstanceIndex,
                                             ID3D11ClassInstance **ppInstance);

private:
  UINT GetTypeId(const std::string &TypeName);

  std::unordered_map<std::string, UINT> type_ids_;
  UINT next_type_id_ = 1;
};

class MTLD3D11ClassInstance
    : public MTLD3D11DeviceChild<ID3D11ClassInstance> {

public:
  MTLD3D11ClassInstance(MTLD3D11Device *pDevice,
                        MTLD3D11ClassLinkage *pLinkage,
                        const D3D11_CLASS_INSTANCE_DESC &Desc,
                        std::string TypeName, std::string InstanceName);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;

  void STDMETHODCALLTYPE GetClassLinkage(ID3D11ClassLinkage **ppLinkage) final;

  void STDMETHODCALLTYPE GetDesc(D3D11_CLASS_INSTANCE_DESC *pDesc) final;

  void STDMETHODCALLTYPE GetInstanceName(LPSTR pInstanceName,
                                         SIZE_T *pBufferLength) final;

  void STDMETHODCALLTYPE GetTypeName(LPSTR pTypeName,
                                     SIZE_T *pBufferLength) final;

private:
  void ReturnName(LPSTR pName, SIZE_T *pBufferLength,
                  const std::string &Name);

  Com<MTLD3D11ClassLinkage, false> linkage_;
  D3D11_CLASS_INSTANCE_DESC desc_;
  std::string type_name_;
  std::string instance_name_;
};

} // namespace dxmt
