// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_capture_ownership.hpp"
#include <cstdint>

namespace dxmt::resource_shape {
// DXIL ResourceKind, not the native Metal view type (1D can be lowered to 2D).
struct View {
  uint32_t kind = 0, stride = 0, format = 0, samples = 0;
};
inline uint32_t TextureKind(uint32_t dimension, bool uav) {
  if (uav && dimension != 2 && dimension != 3 && dimension != 4 && dimension != 5 && dimension != 8) return 0;
  switch (dimension) {
  case 2: return 1; case 3: return 6; case 4: return 2; case 5: return 7;
  case 6: return 3; case 7: return 8; case 8: return 4; case 9: return 5; case 10: return 9;
  default: return 0;
  }
}
inline View Buffer(uint32_t format, uint32_t stride, uint32_t flags, bool counter) {
  if (flags > 1) return {};
  if (flags == 1) return format == 39 && !stride && !counter ? View{11, 0, format, 0} : View{};
  if (!format) return stride && stride <= 2048 && !(stride & 3) ? View{12, stride, 0, 0} : View{};
  return !stride && !counter ? View{10, 0, format, 0} : View{};
}
template<class Resource>
capture::Status Metadata(const Resource &r) {
  using capture::Status;
  if (r.type > 3 || !r.kind || r.lower > r.upper) return Status::Invalid;
  if ((r.type != 1 && (r.flags & 0x27)) ||
      (r.type == 2 && r.kind != 13) || (r.type == 3 && r.kind != 14) ||
      (r.type < 2 && (r.kind == 13 || r.kind == 14)) ||
      (r.type == 1 && (r.kind == 5 || r.kind == 9 || r.kind == 16)) ||
      ((r.flags & 2) && r.kind != 12)) return Status::Invalid;
  if (r.kind > 14 || (r.flags & ~uint32_t(0xb))) return Status::Unsupported;
  return Status::Ready;
}
// Base RDAT has no component type, structured stride, or cbuffer size. This
// checks only the facts it actually supplies; those extra ABI facts need DXIL.
template<class Resource>
capture::Status Match(const Resource &r, const View &view, bool counter) {
  using capture::Status;
  const auto status = Metadata(r);
  if (status != Status::Ready) return status;
  if (!view.kind) return Status::Unsupported;
  if (view.kind != r.kind || ((r.flags & 2) && !counter) ||
      (counter && (r.type != 1 || view.kind != 12))) return Status::Invalid;
  if (view.kind == 12 && (!view.stride || view.stride > 2048 || (view.stride & 3))) return Status::Invalid;
  if (view.kind != 12 && view.stride) return Status::Invalid;
  if ((view.kind == 3 || view.kind == 8) ? !view.samples :
      (view.kind <= 9 && view.samples != 1)) return Status::Invalid;
  return Status::Ready;
}
template<class Resource>
capture::Status Root(const Resource &r, uint64_t address, uint64_t bytes) {
  using capture::Status;
  const auto status = Metadata(r);
  if (status != Status::Ready) return status;
  if (r.lower != r.upper || r.upper == UINT32_MAX || r.type == 3 || (r.flags & 2)) return Status::Invalid;
  if (r.kind != 11 && r.kind != 12 && r.kind != 13) return Status::Invalid;
  if (!address || !bytes || bytes - 1 > UINT64_MAX - address ||
      (r.kind == 13 ? ((address & 255) || bytes > 65536) : (address & 3))) return Status::Invalid;
  return Status::Ready;
}
} // namespace dxmt::resource_shape
