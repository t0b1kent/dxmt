// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_capture_ownership.hpp"
#include "../winemetal/winemetal.h"
#include <unordered_map>
#include <memory>

namespace dxmt::scene_build {
inline bool ByteSpan(uint64_t count, uint64_t stride, uint64_t element, uint64_t &bytes) {
  bytes=0;
  if (!count || !element || stride < element || count-1 > (UINT64_MAX-element)/stride) return false;
  bytes=(count-1)*stride+element;
  return true;
}
inline bool Rebase(uint64_t total, uint64_t view_offset, uint64_t view_length,
                   uint64_t relative, uint64_t bytes, uint64_t &absolute) {
  absolute=0;
  if (!bytes || !capture::Span(total,view_offset,view_length) ||
      !capture::Span(view_length,relative,bytes)) return false;
  absolute=view_offset+relative;
  return true;
}
// Per ExecuteCommandLists batch. Publishing a repeated producer replaces only
// its lookup; earlier packets keep their independently allocated immutable scene.
template <typename Producer, typename Scene>
class SubmissionScenes {
  std::unordered_map<const Producer *,std::shared_ptr<const Scene>> ready_;
public:
  std::shared_ptr<const Scene> Find(const Producer *producer) const {
    const auto it=ready_.find(producer);
    return it==ready_.end()?nullptr:it->second;
  }
  void Publish(const Producer *producer,std::shared_ptr<const Scene> scene) {
    ready_.insert_or_assign(producer,std::move(scene));
  }
};
inline bool Charge(uint64_t structure, uint64_t scratch, uint64_t &used) {
  constexpr uint64_t per_resource=64*1024*1024, per_batch=256*1024*1024;
  if (!structure || !scratch || structure>per_resource || scratch>per_resource ||
      used>per_batch || structure+scratch>per_batch-used) return false;
  used+=structure+scratch;
  return true;
}
} // namespace dxmt::scene_build
