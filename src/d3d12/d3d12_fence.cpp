/*
 * Copyright 2026 Feifan He for CodeWeavers
 * Modified 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include <vector>
#include <algorithm>

namespace dxmt {

class D3D12FenceState final : public D3D12RemovableFence {
  dxmt::mutex mutex_;
  Rc<Fence> fence_;
  std::vector<WMT::Reference<WMT::SharedEvent>> retired_;
  struct PendingEvent {
    WMT::Reference<WMT::SharedEvent> event;
    UINT64 value;
  };
  std::vector<PendingEvent> pending_events_;
  UINT64 last_signal_ = 0;
  bool removed_ = false;

  void PreserveBeforeRewind(UINT64 value) {
    // DXMT replaces the native event on rewind. Removal must also release older-generation waits.
    if (value < last_signal_)
      retired_.emplace_back(fence_->sharedEvent());
    last_signal_ = value;
  }

public:
  D3D12FenceState(WMT::Device device, UINT64 value) : fence_(new Fence(device)), last_signal_(value) {
    fence_->signal(value);
  }

  void Remove() override {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (removed_)
      return;
    removed_ = true;
    fence_->sharedEvent().signalValue(UINT64_MAX);
    for (auto &event : retired_)
      event.signalValue(UINT64_MAX);
  }

  UINT64 CompletedValue() {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return removed_ ? UINT64_MAX : fence_->completedValue();
  }

  bool HasPendingEvents() override {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    std::erase_if(pending_events_, [](auto &pending) {
      return pending.event.signaledValue() >= pending.value;
    });
    return !pending_events_.empty();
  }

  HRESULT Signal(UINT64 value, WMT::CommandBuffer cmdbuf = {}) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (removed_)
      return DXGI_ERROR_DEVICE_REMOVED;
    PreserveBeforeRewind(value);
    if (cmdbuf)
      fence_->signal(cmdbuf, value);
    else
      fence_->signal(value);
    return S_OK;
  }

  HRESULT WaitQueue(WMT::CommandBuffer cmdbuf, UINT64 value) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (removed_)
      return DXGI_ERROR_DEVICE_REMOVED;
    fence_->wait(cmdbuf, value);
    return S_OK;
  }

  HRESULT SetCompletion(EventListener &listener, UINT64 value, HANDLE event) {
    WMT::Reference<WMT::SharedEvent> native_event;
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      if (removed_ || fence_->completedValue() >= value) {
        if (event && !SetEvent(event))
          return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
      }
      if (event) {
        std::erase_if(pending_events_, [](auto &pending) {
          return pending.event.signaledValue() >= pending.value;
        });
        pending_events_.push_back({fence_->sharedEvent(), value});
        listener.setEventOnValue(fence_.ptr(), event, value);
        return S_OK;
      }
      native_event = fence_->sharedEvent();
    }
    native_event.waitUntilSignaledValue(value, ~0ULL);
    return S_OK;
  }
};

class MTLD3D12FenceImpl : public MTLD3D12DeviceChild<MTLD3D12Fence> {
  D3D12_FENCE_FLAGS flags_;
  std::shared_ptr<D3D12FenceState> state_;

public:
  MTLD3D12FenceImpl(MTLD3D12Device *pDevice, D3D12_FENCE_FLAGS flags) :
      MTLD3D12DeviceChild<MTLD3D12Fence>(pDevice),
      flags_(flags) {}

  ~MTLD3D12FenceImpl() {
    if (state_)
      device_->UnregisterFence(state_.get());
  }

  HRESULT
  Initialize(UINT64 InitialValue) {
    state_ = std::make_shared<D3D12FenceState>(device_->GetMTLDevice(), InitialValue);
    return device_->RegisterFence(state_);
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Fence) || riid == __uuidof(ID3D12Fence1)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Fence1), riid)) {
      WARN("ID3D12Fence: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  UINT64 STDMETHODCALLTYPE
  GetCompletedValue() {
    return FAILED(device_->GetDeviceRemovedReason()) ? UINT64_MAX : state_->CompletedValue();
  }

  HRESULT STDMETHODCALLTYPE
  SetEventOnCompletion(UINT64 Value, HANDLE Event) {
    return state_->SetCompletion(device_->event_listener, Value, Event);
  }

  HRESULT STDMETHODCALLTYPE
  Signal(UINT64 Value) {
    const HRESULT hr = device_->GetDeviceRemovedReason();
    return FAILED(hr) ? hr : state_->Signal(Value);
  }

  HRESULT SignalQueue(WMT::CommandBuffer cmdbuf, UINT64 value) override {
    const HRESULT hr = device_->GetDeviceRemovedReason();
    return FAILED(hr) ? hr : state_->Signal(value, cmdbuf);
  }

  std::shared_ptr<D3D12RemovableFence> GetRemovalState() override {
    return state_;
  }

  HRESULT WaitQueue(WMT::CommandBuffer cmdbuf, UINT64 value) override {
    const HRESULT hr = device_->GetDeviceRemovedReason();
    return FAILED(hr) ? hr : state_->WaitQueue(cmdbuf, value);
  }

  D3D12_FENCE_FLAGS STDMETHODCALLTYPE
  GetCreationFlags() {
    return flags_;
  }
};

HRESULT
CreateFence(MTLD3D12Device *pDevice, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) {
  if (ppFence)
    *ppFence = nullptr;
  if (Flags != D3D12_FENCE_FLAG_NONE)
    return DXGI_ERROR_UNSUPPORTED;
  auto fence = Com(new MTLD3D12FenceImpl(pDevice, Flags));
  HRESULT hr = fence->Initialize(InitialValue);
  if (FAILED(hr))
    return hr;
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

}; // namespace dxmt
