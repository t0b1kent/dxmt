#include "d3d11_fence.hpp"
#include "d3d11_device_child.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"

namespace dxmt {

class MTLD3D11FenceImpl : public MTLD3D11DeviceChild<MTLD3D11Fence> {
public:
  MTLD3D11FenceImpl(MTLD3D11Device *pDevice, WMT::Reference<WMT::SharedEvent> event, D3DKMT_HANDLE handle) :
      MTLD3D11DeviceChild<MTLD3D11Fence>(pDevice) {
    this->event = std::move(event);
    local_kmt = handle;
  };

  ~MTLD3D11FenceImpl() {
    if (local_kmt) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
      destroy.hSyncObject = local_kmt;
      D3DKMTDestroySynchronizationObject(&destroy);
    }
  };

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11Fence)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11Query), riid)) {
      WARN("D3D11Fence: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(const SECURITY_ATTRIBUTES *pAttributes, DWORD Access,
                     const WCHAR *Name, HANDLE *pHandle) final {
    InitReturnPtr(pHandle);
    if (!local_kmt)
      return E_INVALIDARG;

    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr);
    attr.SecurityDescriptor = const_cast<SECURITY_ATTRIBUTES*>(pAttributes);

    WCHAR buffer[MAX_PATH];
    UNICODE_STRING name_str;
    if (Name) {
      DWORD session, len, name_len = wcslen(Name);

      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
      memcpy(buffer + len, Name, (name_len + 1) * sizeof(WCHAR));
      name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
      name_str.MaximumLength += sizeof(WCHAR);
      name_str.Buffer = buffer;

      attr.ObjectName = &name_str;
      attr.Attributes = OBJ_CASE_INSENSITIVE;
    }

    if (D3DKMTShareObjects(1, &local_kmt, &attr, Access, pHandle)) {
      ERR("D3D11Fence: Failed to create shared handle");
      return E_FAIL;
    }

    return S_OK;
  };

  UINT64 STDMETHODCALLTYPE GetCompletedValue() final {
    return event.signaledValue();
  };

  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 Value,
                                                 HANDLE Event) final {
    auto shared_event_listener = this->m_parent->GetDXMTDevice().queue().GetSharedEventListener();
    MTLSharedEvent_setWin32EventAtValue(event.handle, shared_event_listener, Event, Value);
    return S_OK;
  };
};

HRESULT
CreateFence(MTLD3D11Device *pDevice, UINT64 InitialValue, D3D11_FENCE_FLAG Flags, REFIID riid, void **ppFence) {
  bool shared = !!(Flags & (D3D11_FENCE_FLAG_SHARED | D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER));
  fprintf(stderr, "macrunner-dxmt-fence: CreateFence ENTER InitialValue=%llu Flags=0x%x shared=%d\n",
          (unsigned long long)InitialValue, (unsigned)Flags, (int)shared);
  auto event = pDevice->GetMTLDevice().newSharedEvent();
  if (!event) {
    fprintf(stderr, "macrunner-dxmt-fence: newSharedEvent FAILED\n");
  } else {
    fprintf(stderr, "macrunner-dxmt-fence: newSharedEvent OK\n");
  }
  D3DKMT_HANDLE local_kmt = 0;
  if (shared) {
    /* MacRunner 2026-06-18: WDDM/D3DKMT shared-fence handles are unavailable
     * under Wine/MacRunner (D3DKMTCreateSynchronizationObject2/Escape fail), so
     * the cross-process sharing path can't be built. DEGRADE to a local
     * (non-shared) MTLSharedEvent-backed fence rather than failing CreateFence
     * outright — returning E_FAIL here broke Unity's GpuFence::Create at
     * first-frame init. The fence still works for in-process GPU<->CPU
     * signal/wait; only CreateSharedHandle (cross-process) stays unavailable. */
    local_kmt = [&]() -> D3DKMT_HANDLE {
      if (!(pDevice->GetLocalD3DKMT() & 0xc0000000))
        return 0;
      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {};
      create.hDevice = pDevice->GetLocalD3DKMT();
      create.Info.Type = D3DDDI_FENCE;
      create.Info.Flags.Shared = 1;
      create.Info.Flags.NtSecuritySharing = 1;
      if (D3DKMTCreateSynchronizationObject2(&create))
        return 0;
      D3DKMT_HANDLE kmt = create.hSyncObject;

      mach_port_t mach_port = event.createMachPort();
      if (!mach_port)
        return 0;
      char mach_port_name[54];
      MakeUniqueSharedName(mach_port_name);
      if (!WMTBootstrapRegister(mach_port_name, mach_port))
        return 0;
      D3DKMT_ESCAPE escape = {};
      escape.Type = D3DKMT_ESCAPE_UPDATE_RESOURCE_WINE;
      escape.pPrivateDriverData = mach_port_name;
      escape.PrivateDriverDataSize = sizeof(mach_port_name);
      escape.hContext = kmt;
      if (!D3DKMTEscape(&escape))
        return 0;
      return kmt;
    }();
    if (!local_kmt)
      fprintf(stderr, "macrunner-dxmt-fence: shared fence DEGRADED to local (D3DKMT unavailable)\n");
    else
      fprintf(stderr, "macrunner-dxmt-fence: shared fence kept shared kmt=0x%x\n", (unsigned)local_kmt);
  } else {
    fprintf(stderr, "macrunner-dxmt-fence: shared=false, local fence\n");
  }
  event.signalValue(InitialValue);
  auto fence = Com(new MTLD3D11FenceImpl(pDevice, std::move(event), local_kmt));
  HRESULT hr = fence->QueryInterface(riid, ppFence);
  fprintf(stderr, "macrunner-dxmt-fence: CreateFence EXIT hr=0x%08x local_kmt=0x%x\n",
          (unsigned)hr, (unsigned)local_kmt);
  return hr;
}

HRESULT
OpenSharedFence(MTLD3D11Device *pDevice, HANDLE hResource,
                REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);

  if (reinterpret_cast<uintptr_t>(hResource) & 0xc0000000) {
    WARN("OpenSharedFence: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppFence == nullptr)
    return S_FALSE;

  char mach_port_name[54];

  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hNtHandle = hResource;
  query.pPrivateRuntimeData = mach_port_name;
  query.PrivateRuntimeDataSize = sizeof(mach_port_name);

  if (D3DKMTQueryResourceInfoFromNtHandle(&query)) {
    WARN(str::format("OpenSharedFence: Failed to query resource: ", hResource));
    return E_INVALIDARG;
  }

  if (query.PrivateRuntimeDataSize != sizeof(mach_port_name)) {
    WARN(str::format("OpenSharedFence: Unexpected size: ", query.PrivateRuntimeDataSize));
    return E_INVALIDARG;
  }

  D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open = {};
  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hNtHandle = hResource;

  if (D3DKMTOpenSyncObjectFromNtHandle2(&open)) {
    WARN(str::format("OpenSharedFence: Failed to open resource: ", hResource));
    return E_INVALIDARG;
  }

  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    return E_INVALIDARG;
  }

  auto fence = Com(new MTLD3D11FenceImpl(
      pDevice,
      pDevice->GetMTLDevice().newSharedEventWithMachPort(mach_port),
      open.hSyncObject));
  return fence->QueryInterface(riid, ppFence);
}

} // namespace dxmt
