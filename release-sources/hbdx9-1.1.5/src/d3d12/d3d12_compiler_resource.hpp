// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_resource_shape.hpp"
#include "d3d12_raytracing_library.hpp"

namespace dxmt::compiler_resource {
// These are original DXIL facts, not inferred from a lowered SPIR-V array stride.
// They describe resource requirements, NOT the byte layout of a Metal argument buffer.
struct Footprint {
  ray_library::Resource resource{};
  uint32_t cbuffer_bytes = 0, structured_stride = 0, component = 0, samples = 0, sampler_kind = 0;
  bool unknown_properties = false;
};
enum class Domain { Unknown, Float, UInt, SInt };
inline Domain ComponentDomain(uint32_t component) {
  switch (component) {
  case 2: case 4: return Domain::SInt;
  case 3: case 5: return Domain::UInt;
  case 8: case 9: case 11: case 12: case 13: case 14: return Domain::Float;
  default: return Domain::Unknown;
  }
}
inline Domain FormatDomain(uint32_t format) {
  switch (format) {
  case 3: case 7: case 12: case 17: case 22: case 25: case 30: case 36: case 42: case 47:
  case 50: case 57: case 62: return Domain::UInt;
  case 4: case 8: case 14: case 18: case 32: case 38: case 43: case 52: case 59: case 64:
    return Domain::SInt;
  case 2: case 6: case 10: case 11: case 13: case 16: case 21: case 24: case 26:
  case 28: case 29: case 31: case 34: case 35: case 37: case 41: case 46: case 49: case 51:
  case 54: case 56: case 58: case 61: case 63: case 65: case 66: case 67: case 68: case 69:
  case 71: case 72: case 74: case 75: case 77: case 78: case 80: case 81: case 83: case 84:
  case 85: case 86: case 87: case 88: case 89: case 91: case 93: case 95: case 96: case 98:
  case 99: case 115: return Domain::Float;
  default: return Domain::Unknown;
  }
}
inline bool SameResource(const ray_library::Resource &a, const ray_library::Resource &b) {
  return a.type==b.type && a.id==b.id && a.kind==b.kind && a.space==b.space &&
         a.lower==b.lower && a.upper==b.upper && a.flags==b.flags;
}
inline capture::Status Validate(const Footprint &f) {
  using capture::Status;
  const auto status=resource_shape::Metadata(f.resource);
  if (status!=Status::Ready) return status;
  if (f.unknown_properties) return Status::Unsupported;
  const auto kind=f.resource.kind;
  if ((kind==13) != bool(f.cbuffer_bytes) || f.cbuffer_bytes>65536 ||
      (kind==12) != bool(f.structured_stride) || f.structured_stride>2048 || (f.structured_stride&3) ||
      ((kind!=3 && kind!=8) && f.samples) || (kind!=14 && f.sampler_kind)) return Status::Invalid;
  const bool typed=kind<=10;
  if (typed != bool(f.component)) return Status::Invalid;
  if (typed && ComponentDomain(f.component)==Domain::Unknown) return Status::Unsupported;
  if (kind==14) return Status::Unsupported; // Comparison/default sampler contract is not represented by View.
  return Status::Ready;
}
inline capture::Status Match(const Footprint &f, const resource_shape::View &view,
                             uint64_t bytes, bool counter, bool root) {
  using capture::Status;
  const auto valid=Validate(f);
  if (valid!=Status::Ready) return valid;
  if (root) {
    if (f.resource.kind!=11 && f.resource.kind!=12 && f.resource.kind!=13) return Status::Invalid;
    if (view.kind!=f.resource.kind || counter || !bytes) return Status::Invalid;
    // A root descriptor has no view stride. Its shader stride comes from DXIL;
    // dynamic access bounds still require the caller's separate byte-range proof.
  } else {
    const auto shape=resource_shape::Match(f.resource,view,counter);
    if (shape!=Status::Ready) return shape;
    if (f.structured_stride && view.stride!=f.structured_stride) return Status::Invalid;
    if (f.samples && view.samples!=f.samples) return Status::Invalid;
    if (f.component) {
      const auto actual=FormatDomain(view.format);
      if (actual==Domain::Unknown) return Status::Unsupported;
      if (actual!=ComponentDomain(f.component)) return Status::Invalid;
    }
  }
  if (f.cbuffer_bytes && bytes<f.cbuffer_bytes) return Status::Invalid;
  return Status::Ready;
}
} // namespace dxmt::compiler_resource
