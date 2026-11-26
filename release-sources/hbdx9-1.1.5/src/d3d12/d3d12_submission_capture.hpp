// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include "d3d12_command_capture.hpp"
#include <array>
#include <algorithm>

namespace dxmt {

// Owned bytes only: no COM/Metal references, resource reads, or deferred pointer dereferences.
// Keep the beginning and the end of a submission. Report every omitted/truncated record.
class D3D12SubmissionCapture {
public:
  static constexpr std::size_t Capacity = 256;
  static constexpr std::size_t HeadCount = 32;
  static constexpr std::size_t TailCount = Capacity - HeadCount;
  struct Context {
    std::uintptr_t list = 0, pass = 0;
    std::uint64_t pass_id = 0;
    std::uint32_t encoder_type = 0;
  };
  struct Record {
    Context context;
    const char *kind = nullptr; // Only static-lifetime labels are accepted.
    std::uint64_t ordinal = 0;
    std::size_t size = 0, captured = 0;
    std::array<unsigned char, WMTCommandCaptureMaxPODBytes> bytes;
  };

  void Reset() { count_ = truncated_ = 0; }
  std::uint64_t Count() const { return count_; }
  std::uint64_t Omitted() const { return count_ > Capacity ? count_ - Capacity : 0; }
  std::uint64_t Truncated() const { return truncated_; }

  void Add(Context context, const char *kind, const void *pod, std::size_t size) {
    const auto ordinal = count_++;
    const auto slot = ordinal < HeadCount ? ordinal : HeadCount + (ordinal - HeadCount) % TailCount;
    auto &r = records_[slot];
    r.context = context;
    r.kind = kind;
    r.ordinal = ordinal;
    r.size = size;
    r.captured = pod ? std::min(size, r.bytes.size()) : 0;
    if (r.captured) std::memcpy(r.bytes.data(), pod, r.captured);
    if (r.captured != size) ++truncated_;
  }

  template <typename T> void AddPOD(Context context, const char *kind, const T &pod) {
    static_assert(std::is_trivially_copyable<T>::value, "capture must own POD bytes");
    Add(context, kind, &pod, sizeof(pod));
  }

  // The producer owns this immutable chain until encoding returns. Only next is followed.
  void Commands(Context context, WMTCommandCaptureFamily family, const wmtcmd_base *head) {
    using namespace wmt_command_capture_detail;
    const void *current = head;
    std::size_t n = 0;
    while (current && n < WMTCommandCaptureMaxCommands) {
      std::uint16_t type = 0;
      std::memcpy(&type, current, sizeof(type));
      const auto info = record(family, type);
      if (!info.size) {
        AddPOD(context, "chain.unknown_type", type);
        ++truncated_;
        return;
      }
      Add(context, info.kind, current, info.size);
      wmtcmd_base base;
      std::memcpy(&base, current, sizeof(base));
#if defined(__i386__)
      if (base.next.high_part) {
        Add(context, "chain.inaccessible_next", nullptr, 0);
        ++truncated_;
        return;
      }
#endif
      current = base.next.ptr;
      ++n;
    }
    if (current) {
      AddPOD(context, "chain.command_limit", n);
      ++truncated_;
    }
  }

  void RenderPass(Context context, const WMTRenderPassInfo &info) {
    const std::uint64_t shape[] = {info.render_target_width, info.render_target_height,
        info.render_target_array_length, info.default_raster_sample_count, info.visibility_buffer};
    AddPOD(context, "render.shape", shape);
    for (std::size_t i = 0; i < std::size(info.colors); ++i) {
      if (!info.colors[i].texture && !info.colors[i].resolve_texture) continue;
      AddPOD(context, "render.color_index", i);
      AddPOD(context, "render.color", info.colors[i]);
    }
    if (info.depth.texture) AddPOD(context, "render.depth", info.depth);
    if (info.stencil.texture) AddPOD(context, "render.stencil", info.stencil);
  }

  template <typename Visit> void ForEach(Visit &&visit) const {
    const auto head = std::min<std::uint64_t>(count_, HeadCount);
    for (std::uint64_t i = 0; i < head; ++i) visit(records_[i]);
    const auto first = count_ > Capacity ? count_ - TailCount : HeadCount;
    for (std::uint64_t i = first; i < count_; ++i)
      visit(records_[HeadCount + (i - HeadCount) % TailCount]);
  }

  // Formatting only after GPU failure, not on the normal per-command hot path.
  template <typename Emit> void Dump(Emit &&emit) const {
    char line[2048];
    std::snprintf(line, sizeof(line),
        "event=begin records=%llu retained=%llu omitted=%llu truncated=%llu capacity=256 "
        "scope=bounded_submission_pods resource_contents=not_captured pointer_payloads=not_captured "
        "api_calls_outside_recorded_chains=not_exhaustive",
        (unsigned long long)count_, (unsigned long long)(count_ - Omitted()),
        (unsigned long long)Omitted(), (unsigned long long)truncated_);
    emit(line);
    ForEach([&](const Record &r) {
      const int prefix = std::snprintf(line, sizeof(line),
          "event=record ordinal=%llu list=%llx pass=%llx id=%llu encoder=%u kind=%s size=%zu "
          "captured=%zu truncated=%u pod_hex=",
          (unsigned long long)r.ordinal, (unsigned long long)r.context.list,
          (unsigned long long)r.context.pass, (unsigned long long)r.context.pass_id,
          r.context.encoder_type, r.kind, r.size, r.captured, unsigned(r.size != r.captured));
      if (prefix < 0 || std::size_t(prefix) + 2 * r.captured >= sizeof(line)) {
        emit("event=format_limit truncated=1");
        return;
      }
      static constexpr char hex[] = "0123456789abcdef";
      std::size_t pos = std::size_t(prefix);
      for (std::size_t i = 0; i < r.captured; ++i) {
        line[pos++] = hex[r.bytes[i] >> 4];
        line[pos++] = hex[r.bytes[i] & 15];
      }
      line[pos] = 0;
      emit(line);
    });
    emit("event=end");
  }

private:
  std::array<Record, Capacity> records_;
  std::uint64_t count_ = 0, truncated_ = 0;
};

} // namespace dxmt
