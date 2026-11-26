// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dxmt::native_wave {
struct Entry {
  const unsigned char *source;
  size_t source_size;
  const unsigned char *root;
  size_t root_size;
  const unsigned char *library;
  size_t library_size;
};

inline const Entry *MatchShader(const Entry *entries, size_t count, const void *source, size_t size) {
  if (!entries || !source || !size) return nullptr;
  for (size_t i = 0; i < count; ++i)
    if (entries[i].source_size == size && !std::memcmp(source, entries[i].source, size)) return &entries[i];
  return nullptr;
}

inline bool MatchesRoot(const Entry &entry, const void *container, size_t size) {
  if (!container || size < 36 || size > 4 * 1024 * 1024) return false;
  const auto *b = static_cast<const unsigned char *>(container);
  auto word = [b](size_t offset) { uint32_t value; std::memcpy(&value, b + offset, 4); return value; };
  if (std::memcmp(b, "DXBC", 4) || word(20) != 1 || word(24) != size) return false;
  const uint32_t count = word(28);
  if (!count || count > 64 || count > (size - 32) / 4) return false;
  const size_t header = 32 + 4 * size_t(count);
  unsigned roots = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t offset = word(32 + 4 * i);
    if (offset < header || offset > size - 8) return false;
    const size_t length = word(offset + 4);
    if (length > size - offset - 8) return false;
    for (uint32_t j = 0; j < i; ++j) {
      const size_t previous = word(32 + 4 * j), previous_end = previous + 8 + word(previous + 4);
      if (offset < previous_end && previous < offset + 8 + length) return false;
    }
    if (!std::memcmp(b + offset, "RTS0", 4)) {
      if (++roots != 1 || length != entry.root_size || std::memcmp(b + offset + 8, entry.root, length)) return false;
    }
  }
  return roots == 1;
}
} // namespace dxmt::native_wave
