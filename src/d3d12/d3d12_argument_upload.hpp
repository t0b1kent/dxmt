// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include "d3d12_capture_ownership.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>
#include <vector>

namespace dxmt::argument_upload {
struct Block {
  uint32_t index = 0;
  std::vector<uint8_t> initial;
};
struct Field {
  uint32_t block = 0, offset = 0, source = 0, word = 0, count = 0;
};
struct Pointer {
  uint32_t block = 0, offset = 0, target = 0, target_offset = 0;
};
// Explicit compiler ABI only. This is not an inferred Metal argument-encoder
// layout, and resource words must never be replaced with public heap pointers.
struct Plan {
  std::vector<Block> blocks;
  std::vector<Field> fields;
  std::vector<Pointer> pointers;
};
inline capture::Status Validate(const Plan &plan, size_t sources) {
  using capture::Status;
  if (plan.blocks.size() > 31 || sources > 65536 || plan.fields.size() > 65536 || plan.pointers.size() > 65536)
    return Status::Unsupported;
  std::array<bool, 31> used{};
  uint64_t total = 0;
  for (const auto &block : plan.blocks) {
    if (block.index >= used.size() || used[block.index] || block.initial.empty()) return Status::Invalid;
    used[block.index] = true;
    total += block.initial.size();
    if (block.initial.size() > 16 * 1024 * 1024 || total > 64 * 1024 * 1024) return Status::Unsupported;
  }
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> writes;
  const auto add = [&](uint32_t block, uint32_t offset, uint32_t bytes) {
    if (block >= plan.blocks.size() || (offset & 7) ||
        !capture::Span(plan.blocks[block].initial.size(), offset, bytes)) return false;
    writes.emplace_back(block, offset, offset + bytes);
    return true;
  };
  for (const auto &field : plan.fields)
    if (field.source >= sources || field.word >= 4 || !field.count || field.count > 4 - field.word ||
        !add(field.block, field.offset, field.count * 8)) return Status::Invalid;
  for (const auto &pointer : plan.pointers)
    if (pointer.target >= plan.blocks.size() || pointer.target_offset >= plan.blocks[pointer.target].initial.size() ||
        (pointer.target_offset & 7) || !add(pointer.block, pointer.offset, 8)) return Status::Invalid;
  std::sort(writes.begin(), writes.end());
  for (size_t i = 1; i < writes.size(); ++i)
    if (std::get<0>(writes[i-1]) == std::get<0>(writes[i]) && std::get<2>(writes[i-1]) > std::get<1>(writes[i]))
      return Status::Invalid;
  return Status::Ready;
}
inline void Store64(std::vector<uint8_t> &bytes, size_t offset, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}
template<class Capture>
capture::Status Materialize(const Plan &plan, const std::vector<Capture> &sources,
                            const std::vector<uint64_t> &addresses, std::vector<std::vector<uint8_t>> &out) {
  using capture::Status;
  out.clear();
  const auto valid = Validate(plan, sources.size());
  if (valid != Status::Ready) return valid;
  if (addresses.size() != plan.blocks.size()) return Status::Invalid;
  for (size_t i = 0; i < addresses.size(); ++i)
    if (!addresses[i] || (addresses[i] & 7) || plan.blocks[i].initial.size() - 1 > UINT64_MAX - addresses[i])
      return Status::Invalid;
  std::vector<std::vector<uint8_t>> result;
  result.reserve(plan.blocks.size());
  for (const auto &block : plan.blocks) result.push_back(block.initial);
  for (const auto &field : plan.fields)
    for (uint32_t i = 0; i < field.count; ++i)
      Store64(result[field.block], field.offset + i * 8, sources[field.source].words[field.word + i]);
  for (const auto &pointer : plan.pointers)
    Store64(result[pointer.block], pointer.offset, addresses[pointer.target] + pointer.target_offset);
  out = std::move(result);
  return Status::Ready;
}

template<class Capture, class Buffer>
struct Upload {
  std::vector<Capture> sources;
  std::vector<Buffer> buffers;
  std::vector<std::vector<uint8_t>> bytes;
};
// The same preparation path is exercised with CPU fakes and native queue resources.
// Callbacks run before encoding; failure publishes no partially prepared packet.
template<class Capture, class Buffer, class Resolve, class Allocate>
capture::Status Prepare(const Plan &plan, size_t count, Resolve resolve, Allocate allocate,
                        Upload<Capture, Buffer> &out) {
  using capture::Status;
  out = {};
  const auto valid = Validate(plan, count);
  if (valid != Status::Ready) return valid;
  Upload<Capture, Buffer> next;
  next.sources.resize(count);
  for (size_t i = 0; i < count; ++i) {
    const auto status = resolve(i, next.sources[i]);
    if (status != Status::Ready) return status;
  }
  next.buffers.resize(plan.blocks.size());
  std::vector<uint64_t> addresses(plan.blocks.size());
  for (size_t i = 0; i < plan.blocks.size(); ++i) {
    const auto status = allocate(plan.blocks[i], next.buffers[i], addresses[i]);
    if (status != Status::Ready) return status;
  }
  const auto status = Materialize(plan, next.sources, addresses, next.bytes);
  if (status != Status::Ready) return status;
  out = std::move(next);
  return Status::Ready;
}
} // namespace dxmt::argument_upload
