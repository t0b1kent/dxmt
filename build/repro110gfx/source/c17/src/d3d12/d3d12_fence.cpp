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
#include "d3d12_pipeline_timing.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include <vector>
#include <algorithm>
#include <new>

namespace dxmt {

// Publish CPU-visible values and Win32 events in one ordered path. Native
// shared events remain GPU dependencies; their asynchronous listener is not a
// D3D12 completion publication barrier.
static bool OrderedFencePublication() {
  return fence_publication::Enabled();
}

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
  UINT64 published_value_ = 0;
  struct Completion {
    UINT64 value;
    HANDLE event; // owned duplicate; never a caller's potentially recycled handle
    Completion *next;
  };
  Completion *completions_ = nullptr;

  // mutex_ serializes registration, CPU signal, retirement and completed-value
  // reads. Set every due event before exposing the new value to other readers.
  HRESULT Publish(UINT64 value) {
    HRESULT result = S_OK;
    for (auto **link = &completions_; *link;) {
      auto *entry = *link;
      if (entry->value > value) { link = &entry->next; continue; }
      if (SetEvent(entry->event))
        fence_publication::Stats().events_published.fetch_add(1, std::memory_order_relaxed);
      else if (SUCCEEDED(result))
        result = HRESULT_FROM_WIN32(GetLastError());
      *link = entry->next;
      CloseHandle(entry->event);
      delete entry;
    }
    published_value_ = value; // D3D12 fences may rewind.
    return result;
  }

  HRESULT RegisterCompletion(UINT64 value, HANDLE event) {
    fence_publication::Stats().registrations.fetch_add(1, std::memory_order_relaxed);
    if (removed_ || published_value_ >= value) {
      if (!SetEvent(event)) return HRESULT_FROM_WIN32(GetLastError());
      fence_publication::Stats().events_published.fetch_add(1, std::memory_order_relaxed);
      return S_OK;
    }
    auto *entry = new (std::nothrow) Completion{value, nullptr, nullptr};
    if (!entry) return E_OUTOFMEMORY;
    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                         &entry->event, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
      delete entry;
      return hr;
    }
    // Append: registrations for a shared auto-reset event keep their order.
    auto **link = &completions_;
    while (*link) link = &(*link)->next;
    *link = entry;
    return S_OK;
  }

  HRESULT SetOrderedCompletion(UINT64 value, HANDLE event) {
    if (event) {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      return RegisterCompletion(value, event);
    }
    D3D12FencePublicationWait wait;
    wait.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wait.event) return HRESULT_FROM_WIN32(GetLastError());
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      const HRESULT hr = RegisterCompletion(value, wait.event);
      if (FAILED(hr)) return hr;
    }
    return fence_publication::Wait(wait.event) == WAIT_OBJECT_0 ?
        S_OK : HRESULT_FROM_WIN32(GetLastError());
  }

  void PreserveBeforeRewind(UINT64 value) {
    // DXMT replaces the native event on rewind. Removal must also release older-generation waits.
    if (value < last_signal_)
      retired_.emplace_back(fence_->sharedEvent());
    last_signal_ = value;
  }

public:
  D3D12FenceState(WMT::Device device, UINT64 value) :
      fence_(new Fence(device)), last_signal_(value), published_value_(value) {
    fence_->signal(value);
  }

  ~D3D12FenceState() {
    while (completions_) {
      auto *entry = completions_;
      completions_ = entry->next;
      CloseHandle(entry->event);
      delete entry;
    }
  }

  bool UsesOrderedPublication() const override { return OrderedFencePublication(); }

  void CompleteQueueSignal(UINT64 value) override {
    if (!OrderedFencePublication()) return;
    fence_publication::Stats().queue_signals.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (!removed_ && FAILED(Publish(value)))
      ERR("D3D12 fence: failed to publish a registered event");
  }

  HRESULT PrepareQueueWait(UINT64 value, std::unique_ptr<D3D12FencePublicationWait> &wait) override {
    if (!OrderedFencePublication()) return S_OK;
    wait.reset(new (std::nothrow) D3D12FencePublicationWait);
    if (!wait) return E_OUTOFMEMORY;
    wait->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wait->event) return HRESULT_FROM_WIN32(GetLastError());
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return RegisterCompletion(value, wait->event);
  }

  HRESULT PrepareNativeSignal(UINT64 value, WMT::Reference<WMT::SharedEvent> &event) override {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    PreserveBeforeRewind(value);
    bool changed;
    auto native = fence_->reserveSignal(value, changed);
    if (!native) return E_OUTOFMEMORY;
    if (changed) event = native;
    return S_OK;
  }

  HRESULT PrepareNativeWait(UINT64 value, WMT::Reference<WMT::SharedEvent> &event,
                            std::unique_ptr<D3D12FencePublicationWait> &publication) override {
    if (OrderedFencePublication()) {
      publication.reset(new (std::nothrow) D3D12FencePublicationWait);
      if (!publication) return E_OUTOFMEMORY;
      publication->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!publication->event) return HRESULT_FROM_WIN32(GetLastError());
    }
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (removed_) return DXGI_ERROR_DEVICE_REMOVED;
    event = fence_->sharedEvent();
    return publication ? RegisterCompletion(value, publication->event) : S_OK;
  }

  void Remove() override {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (removed_)
      return;
    removed_ = true;
    if (OrderedFencePublication())
      Publish(UINT64_MAX);
    fence_->sharedEvent().signalValue(UINT64_MAX);
    for (auto &event : retired_)
      event.signalValue(UINT64_MAX);
  }

  UINT64 CompletedValue() {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return removed_ ? UINT64_MAX : (OrderedFencePublication() ? published_value_ : fence_->completedValue());
  }

  bool HasPendingEvents() override {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (OrderedFencePublication())
      return completions_ != nullptr;
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
    pipeline_timing::Get().Emit(pipeline_timing::NativeFence, pipeline_timing::Point, this,
        value, fence_->sharedEvent().handle, cmdbuf.handle);
    if (!cmdbuf && OrderedFencePublication()) {
      fence_publication::Stats().cpu_signals.fetch_add(1, std::memory_order_relaxed);
      return Publish(value);
    }
    return S_OK;
  }

  HRESULT WaitQueue(WMT::CommandBuffer cmdbuf, UINT64 value) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (removed_)
      return DXGI_ERROR_DEVICE_REMOVED;
    fence_->wait(cmdbuf, value);
    return S_OK;
  }

  HRESULT SetCompletion(EventListener *listener, UINT64 value, HANDLE event) {
    if (OrderedFencePublication())
      return SetOrderedCompletion(value, event);
    WMT::Reference<WMT::SharedEvent> native_event;
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      if (removed_ || fence_->completedValue() >= value) {
        pipeline_timing::Get().Emit(pipeline_timing::FenceBranch, pipeline_timing::Point, this,
            value, pipeline_timing::Pointer(event), fence_->sharedEvent().handle, 1);
        if (event && !SetEvent(event))
          return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
      }
      if (event) {
        pipeline_timing::Get().Emit(pipeline_timing::FenceBranch, pipeline_timing::Point, this,
            value, pipeline_timing::Pointer(event), fence_->sharedEvent().handle, 2);
        std::erase_if(pending_events_, [](auto &pending) {
          return pending.event.signaledValue() >= pending.value;
        });
        pending_events_.push_back({fence_->sharedEvent(), value});
        listener->setEventOnValue(fence_.ptr(), event, value);
        return S_OK;
      }
      native_event = fence_->sharedEvent();
      pipeline_timing::Get().Emit(pipeline_timing::FenceBranch, pipeline_timing::Point, this,
          value, 0, native_event.handle, 3);
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
    pipeline_timing::Get().Emit(pipeline_timing::FenceDestroy, pipeline_timing::Point, this,
        pipeline_timing::Pointer(state_.get()), 0, 0, 0, 0, 0, true);
    if (state_)
      device_->UnregisterFence(state_.get());
  }

  HRESULT
  Initialize(UINT64 InitialValue) {
    state_ = std::make_shared<D3D12FenceState>(device_->GetMTLDevice(), InitialValue);
    pipeline_timing::Get().Emit(pipeline_timing::FenceCreate, pipeline_timing::Point, this,
        pipeline_timing::Pointer(state_.get()), InitialValue, 0, 0, 0, 0, true);
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
    if (OrderedFencePublication()) {
      if (FAILED(device_->GetDeviceRemovedReason())) state_->Remove();
      const auto value = state_->CompletedValue();
      pipeline_timing::Get().Emit(pipeline_timing::FenceCompleted, pipeline_timing::Point, this,
          pipeline_timing::Pointer(state_.get()), value);
      return value;
    }
    const auto value = FAILED(device_->GetDeviceRemovedReason()) ? UINT64_MAX : state_->CompletedValue();
    pipeline_timing::Get().Emit(pipeline_timing::FenceCompleted, pipeline_timing::Point, this,
        pipeline_timing::Pointer(state_.get()), value);
    return value;
  }

  HRESULT STDMETHODCALLTYPE
  SetEventOnCompletion(UINT64 Value, HANDLE Event) {
    pipeline_timing::Scope pipeline(pipeline_timing::FenceCompletion, this, Value,
        pipeline_timing::Pointer(Event), pipeline_timing::Pointer(state_.get()));
    return pipeline.Result(state_->SetCompletion(device_->event_listener.get(), Value, Event));
  }

  HRESULT STDMETHODCALLTYPE
  Signal(UINT64 Value) {
    pipeline_timing::Scope pipeline(pipeline_timing::FenceSignal, this, Value,
        pipeline_timing::Pointer(state_.get()));
    const HRESULT hr = device_->GetDeviceRemovedReason();
    return pipeline.Result(FAILED(hr) ? hr : state_->Signal(Value));
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
