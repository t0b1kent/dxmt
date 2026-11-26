// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>

namespace dxmt::capture {

enum class Status { Ready, Invalid, Uninitialized, Unsupported, Retired, OutOfMemory };

// The allocation must be owned by the containing object until this lease is destroyed.
template <typename Owner, typename Allocation>
class ResidencyLease {
  std::shared_ptr<Owner> owner_;
  Allocation allocation_;
public:
  ResidencyLease(std::shared_ptr<Owner> owner, Allocation allocation) : owner_(std::move(owner)), allocation_(allocation) {
    owner_->Retain(allocation_);
  }
  ~ResidencyLease() { owner_->Release(allocation_); }
  ResidencyLease(const ResidencyLease &) = delete;
  ResidencyLease &operator=(const ResidencyLease &) = delete;
};

template <typename Resource, typename Payload>
struct Snapshot {
  Payload payload{};
  std::shared_ptr<Resource> resource, counter;
};

// Caller serializes slot writes/copies/reads with the same heap lock. Slots never own resources.
template <typename Resource, typename Payload>
class Slot {
  std::weak_ptr<Resource> resource_, counter_;
  bool initialized_ = false, supported_ = false, counter_required_ = false;
public:
  Payload payload{};

  void Publish(std::shared_ptr<Resource> resource, std::shared_ptr<Resource> counter = {}, bool counter_required = false) {
    initialized_ = true;
    supported_ = bool(resource) && (!counter_required || bool(counter));
    resource_ = resource;
    counter_ = counter;
    counter_required_ = counter_required;
  }

  Status Acquire(Snapshot<Resource, Payload> &out) const {
    out = {};
    if (!initialized_) return Status::Uninitialized;
    if (!supported_) return Status::Unsupported;
    auto resource = resource_.lock();
    if (!resource || !resource->Live()) return Status::Retired;
    std::shared_ptr<Resource> counter;
    if (counter_required_) {
      counter = counter_.lock();
      if (!counter || !counter->Live()) return Status::Retired;
    }
    out.payload = payload;
    out.resource = std::move(resource);
    out.counter = std::move(counter);
    return Status::Ready;
  }
};

inline bool Span(uint64_t total, uint64_t first, uint64_t count) {
  return first <= total && count <= total - first;
}

template <typename Slice>
Status BufferSlice(uint64_t total, uint64_t first, uint64_t count, uint32_t stride, Slice &out) {
  out = {};
  if (!stride || first > total / stride) return Status::Invalid;
  const uint64_t offset = first * stride;
  if (count > (total - offset) / stride) return Status::Invalid;
  const uint64_t length = count * stride;
  // The shared DXMT slice ABI is still 32-bit; never silently narrow a valid large view.
  if (first > UINT32_MAX || count > UINT32_MAX || offset > UINT32_MAX || length > UINT32_MAX)
    return Status::Unsupported;
  out.firstElement = uint32_t(first);
  out.elementCount = uint32_t(count);
  out.byteOffset = uint32_t(offset);
  out.byteLength = uint32_t(length);
  return Status::Ready;
}

inline Status CounterOffset(uint64_t total, uint64_t offset, bool present, uint32_t &out) {
  out = 0;
  if (!present) return offset ? Status::Invalid : Status::Ready;
  if ((offset & 4095) || !Span(total, offset, 4)) return Status::Invalid;
  if (offset > UINT32_MAX) return Status::Unsupported;
  out = uint32_t(offset);
  return Status::Ready;
}

template <typename Storage>
void ClearDescriptor(Storage &storage) {
  static_assert(sizeof(Storage) == 32 && std::is_trivially_copyable_v<Storage>);
  std::memset(&storage, 0, sizeof(storage));
}

// Copy in memmove order: no temporary resource ownership or allocation on overlapping copies.
template <typename Copy>
bool CopyRange(uint64_t source_size, uint64_t from, uint64_t destination_size,
               uint64_t to, uint64_t count, bool same, Copy copy) {
  if (!Span(source_size, from, count) || !Span(destination_size, to, count)) return false;
  if (same && from == to) return true;
  if (same && to > from && to - from < count) {
    for (uint64_t n = count; n; --n) copy(from + n - 1, to + n - 1);
  } else {
    for (uint64_t n = 0; n < count; ++n) copy(from + n, to + n);
  }
  return true;
}

} // namespace dxmt::capture
