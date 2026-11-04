/*
 * Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include "d3d12_device.hpp"

namespace dxmt {

// Device5 has no flags or initial-state argument, and its output is not optional:
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-createlifetimetracker
inline HRESULT
CreateLifetimeTracker(MTLD3D12Device *pDevice, ID3D12LifetimeOwner *pOwner, REFIID riid, void **ppvTracker) {
  if (!ppvTracker)
    return E_POINTER;

  *ppvTracker = nullptr;

  if (!pDevice || !pOwner)
    return E_INVALIDARG;

  if (riid != __uuidof(IUnknown) && riid != __uuidof(ID3D12Object) && riid != __uuidof(ID3D12DeviceChild) &&
      riid != __uuidof(ID3D12LifetimeTracker))
    return E_NOINTERFACE;

  // CreateSharedResource associates resources with a tracker, but this fork has
  // no compatibility-resource ownership or lifetime-transition implementation:
  // https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3d12compatibility.idl
  // DestroyOwnedObject applies to an owned, lifetime-tracked object, not an
  // arbitrary device child. Releasing the caller's reference is not a substitute:
  // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12lifetimetracker-destroyownedobject
  // Do not expose a COM-only tracker as functional support, retain the owner, or
  // invent destructor notifications until resource lifetime tracking exists.
  return E_NOTIMPL;
}

} // namespace dxmt
