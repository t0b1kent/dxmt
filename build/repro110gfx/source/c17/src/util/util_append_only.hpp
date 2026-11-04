#pragma once
// MacRunner (D3D12-MATRIX, 17.10.2026): append-only array whose elements never move.
//
// Readers need no lock: an element is readable once size() (acquire) is greater than its index, and it is never
// moved, reallocated or overwritten while the array lives. One writer at a time: the owner serializes push_back with
// its own mutex. push_back writes the element (and, for a new segment, the segment pointer) before the release store
// of the size, so a reader that saw the size also sees both.
//
// Segment s holds kFirst << s elements; segment 0 is inline. Capacity: kFirst * (2^kSegments - 1) elements.
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace dxmt {

template <typename T, uint32_t kFirst, uint32_t kSegments>
class append_only_array {
  static_assert(std::has_single_bit(kFirst) && kSegments >= 1 && kSegments <= 30);
  static constexpr uint32_t kShift = std::countr_zero(kFirst);

  T inline_[kFirst] = {};
  std::atomic<T *> segments_[kSegments] = {};   // segments_[0] stays null: segment 0 is inline_
  std::atomic<uint32_t> size_{0};

  static uint32_t segment_of(uint32_t i) { return uint32_t(std::bit_width((i >> kShift) + 1u)) - 1u; }
  static uint32_t first_of(uint32_t s) { return kFirst * ((1u << s) - 1u); }

  T *segment(uint32_t s) const {
    return s ? segments_[s].load(std::memory_order_relaxed) : const_cast<T *>(inline_);
  }

  template <typename U> void append(U &&value) {
    const uint32_t n = size_.load(std::memory_order_relaxed);
    const uint32_t s = segment_of(n);
    if (s >= kSegments) std::abort();
    T *seg = segment(s);
    if (!seg) {
      seg = new T[kFirst << s]();
      segments_[s].store(seg, std::memory_order_relaxed);
    }
    seg[n - first_of(s)] = std::forward<U>(value);
    size_.store(n + 1, std::memory_order_release);
  }

public:
  append_only_array() = default;
  append_only_array(const append_only_array &) = delete;
  append_only_array &operator=(const append_only_array &) = delete;
  ~append_only_array() {
    for (uint32_t s = 1; s < kSegments; s++)
      delete[] segments_[s].load(std::memory_order_relaxed);
  }

  // Number of published elements; acquire pairs with the release in push_back.
  uint32_t size() const { return size_.load(std::memory_order_acquire); }

  // Only for i < a size() this thread has observed (or for the writer, under the owner's lock).
  T &operator[](uint32_t i) {
    const uint32_t s = segment_of(i);
    return segment(s)[i - first_of(s)];
  }
  const T &operator[](uint32_t i) const {
    const uint32_t s = segment_of(i);
    return segment(s)[i - first_of(s)];
  }

  // Writer only, serialized by the owner.
  void push_back(T &&value) { append(std::move(value)); }
  void push_back(const T &value) { append(value); }
};

} // namespace dxmt
