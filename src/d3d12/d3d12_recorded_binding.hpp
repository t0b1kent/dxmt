// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_capture_ownership.hpp"
#include <utility>

namespace dxmt::capture {

// Providers keep the recorded heap/index, never a pointer to mutable command-list state.
// Static descriptors freeze at recording; volatile descriptors resolve afresh per submit.
// This captures descriptor words/owners, NOT resource data or queue/fence ownership.
template <typename Provider, typename Captured>
class RecordedDescriptor {
  Provider provider_{};
  Captured fixed_{};
  bool recorded_ = false, volatile_ = false;
public:
  Status Record(Provider provider, bool is_volatile) {
    *this = {};
    if (!provider.Valid()) return Status::Invalid;
    Captured fixed;
    if (!is_volatile) {
      const auto status = provider.Acquire(fixed);
      if (status != Status::Ready) return status;
    }
    provider_ = std::move(provider);
    fixed_ = std::move(fixed);
    volatile_ = is_volatile;
    recorded_ = true;
    return Status::Ready;
  }
  Status Resolve(Captured &out) const {
    out = {};
    if (!recorded_) return Status::Uninitialized;
    if (volatile_) return provider_.Acquire(out);
    out = fixed_;
    return Status::Ready;
  }
};

// Copies native owners, not aliasing shared_ptrs whose deleter still owns a public source.
template <typename NativeCapture, typename PublicCapture>
NativeCapture DetachNative(const PublicCapture &source) {
  NativeCapture out;
  out.type = source.type;
  out.shape = source.shape;
  out.words = source.words;
  out.resource = source.resource ? source.resource->native : nullptr;
  out.counter = source.counter ? source.counter->native : nullptr;
  out.texture = source.texture;
  out.texel = source.texel;
  out.sampler = source.sampler;
  out.buffer_offset = source.buffer_offset;
  out.byte_length = source.byte_length;
  out.counter_offset = source.counter_offset;
  return out;
}

} // namespace dxmt::capture
