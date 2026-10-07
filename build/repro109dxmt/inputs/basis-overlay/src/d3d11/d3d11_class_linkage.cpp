
#include "d3d11_class_linkage.hpp"
#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace dxmt {

MTLD3D11ClassInstance::MTLD3D11ClassInstance(
    MTLD3D11Device *pDevice, MTLD3D11ClassLinkage *pLinkage,
    const D3D11_CLASS_INSTANCE_DESC &Desc, std::string TypeName,
    std::string InstanceName)
    : MTLD3D11DeviceChild<ID3D11ClassInstance>(pDevice), linkage_(pLinkage),
      desc_(Desc), type_name_(std::move(TypeName)),
      instance_name_(std::move(InstanceName)) {}

HRESULT STDMETHODCALLTYPE
MTLD3D11ClassInstance::QueryInterface(REFIID riid, void **ppvObject) {
  if (ppvObject == nullptr)
    return E_POINTER;

  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
      riid == __uuidof(ID3D11ClassInstance)) {
    *ppvObject = ref(this);
    return S_OK;
  }

  if (logQueryInterfaceError(__uuidof(ID3D11ClassInstance), riid)) {
    Logger::warn("D3D11ClassInstance::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
  }

  return E_NOINTERFACE;
}

void STDMETHODCALLTYPE
MTLD3D11ClassInstance::GetClassLinkage(ID3D11ClassLinkage **ppLinkage) {
  if (ppLinkage)
    *ppLinkage = linkage_.ref();
}

void STDMETHODCALLTYPE
MTLD3D11ClassInstance::GetDesc(D3D11_CLASS_INSTANCE_DESC *pDesc) {
  if (pDesc)
    *pDesc = desc_;
}

void STDMETHODCALLTYPE MTLD3D11ClassInstance::GetInstanceName(
    LPSTR pInstanceName, SIZE_T *pBufferLength) {
  ReturnName(pInstanceName, pBufferLength,
             desc_.Created ? std::string() : instance_name_);
}

void STDMETHODCALLTYPE
MTLD3D11ClassInstance::GetTypeName(LPSTR pTypeName, SIZE_T *pBufferLength) {
  ReturnName(pTypeName, pBufferLength,
             desc_.Created ? type_name_ : std::string());
}

void MTLD3D11ClassInstance::ReturnName(LPSTR pName, SIZE_T *pBufferLength,
                                       const std::string &Name) {
  if (!pBufferLength)
    return;

  SIZE_T buffer_length = *pBufferLength;
  *pBufferLength = Name.length() + 1;

  if (!pName || !buffer_length)
    return;

  SIZE_T copy_length = std::min(buffer_length - 1, Name.length());
  std::memcpy(pName, Name.c_str(), copy_length);
  pName[copy_length] = '\0';
}

MTLD3D11ClassLinkage::MTLD3D11ClassLinkage(MTLD3D11Device *pDevice)
    : MTLD3D11DeviceChild<ID3D11ClassLinkage>(pDevice) {}

MTLD3D11ClassLinkage::~MTLD3D11ClassLinkage() {}

UINT MTLD3D11ClassLinkage::GetTypeId(const std::string &TypeName) {
  auto entry = type_ids_.find(TypeName);
  if (entry != type_ids_.end())
    return entry->second;

  UINT type_id = next_type_id_++;
  type_ids_.emplace(TypeName, type_id);
  return type_id;
}

HRESULT STDMETHODCALLTYPE
MTLD3D11ClassLinkage::QueryInterface(REFIID riid, void **ppvObject) {
  if (ppvObject == nullptr)
    return E_POINTER;

  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
      riid == __uuidof(ID3D11ClassLinkage)) {
    *ppvObject = ref(this);
    return S_OK;
  }

  if (logQueryInterfaceError(__uuidof(ID3D11ClassLinkage), riid)) {
    Logger::warn("D3D11ClassLinkage::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
  }

  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE MTLD3D11ClassLinkage::CreateClassInstance(
    LPCSTR pClassTypeName, UINT ConstantBufferOffset, UINT ConstantVectorOffset,
    UINT TextureOffset, UINT SamplerOffset, ID3D11ClassInstance **ppInstance) {
  InitReturnPtr(ppInstance);

  if (!ppInstance || !pClassTypeName)
    return E_INVALIDARG;

  D3D11_CLASS_INSTANCE_DESC desc = {};
  desc.TypeId = GetTypeId(pClassTypeName);
  desc.ConstantBuffer = ConstantBufferOffset;
  desc.BaseConstantBufferOffset = ConstantVectorOffset;
  desc.BaseTexture = TextureOffset;
  desc.BaseSampler = SamplerOffset;
  desc.Created = TRUE;

  *ppInstance = ref(new MTLD3D11ClassInstance(m_parent, this, desc,
                                              pClassTypeName, std::string()));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D11ClassLinkage::GetClassInstance(
    LPCSTR pClassInstanceName, UINT InstanceIndex,
    ID3D11ClassInstance **ppInstance) {
  InitReturnPtr(ppInstance);

  if (!ppInstance || !pClassInstanceName)
    return E_INVALIDARG;

  D3D11_CLASS_INSTANCE_DESC desc = {};
  desc.InstanceIndex = InstanceIndex;
  desc.BaseTexture = 127;
  desc.BaseSampler = 15;

  *ppInstance = ref(new MTLD3D11ClassInstance(
      m_parent, this, desc, std::string(), pClassInstanceName));
  return S_OK;
}

} // namespace dxmt
