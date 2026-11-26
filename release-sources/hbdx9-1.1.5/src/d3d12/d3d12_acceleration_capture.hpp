// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <memory>
#include <vector>
#include <utility>
#include "d3d12_capture_ownership.hpp"

namespace dxmt::capture {
// Immutable two-level ownership: an instance scene keeps every primitive child.
// Allocation owns native resource/residency only, never recording-thread COM state.
template <typename Allocation>
class AccelerationTree {
public:
  enum class Kind { Primitive, Instance };
  using Ptr = std::shared_ptr<const AccelerationTree>;
private:
  Kind kind_;
  std::vector<Ptr> children_;
  std::shared_ptr<Allocation> allocation_; // Released before children.
  AccelerationTree(Kind kind, std::shared_ptr<Allocation> allocation, std::vector<Ptr> children)
      : kind_(kind), children_(std::move(children)), allocation_(std::move(allocation)) {}
public:
  static Status Create(Kind kind, std::shared_ptr<Allocation> allocation,
                       std::vector<Ptr> children, Ptr &out) {
    out.reset();
    if (!allocation || (kind != Kind::Primitive && kind != Kind::Instance) ||
        (kind == Kind::Primitive && !children.empty()) ||
        (kind == Kind::Instance && (children.empty() || children.size() > 4096))) return Status::Invalid;
    for (const auto &child : children)
      if (!child || child->kind_ != Kind::Primitive || child->allocation_ == allocation) return Status::Invalid;
    out = Ptr(new AccelerationTree(kind, std::move(allocation), std::move(children)));
    return Status::Ready;
  }
  Kind Type() const { return kind_; }
  const Allocation &Native() const { return *allocation_; }
  template <typename F> void ForEach(F &&use) const {
    use(*allocation_);
    for (const auto &child : children_) use(child->Native());
  }
};
} // namespace dxmt::capture
