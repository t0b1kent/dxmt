// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dxmt::ray_library {
enum class Result { Ready, Missing, Invalid, Unsupported };
struct Resource {
  uint32_t type, kind, id, space, lower, upper, flags;
  std::string name;
};
struct Function {
  std::string name, unmangled;
  std::vector<uint32_t> resources;
  std::vector<std::string> dependencies;
  uint32_t kind, payload, attributes, features_low, features_high, stages, target;
};
struct Library {
  std::vector<Resource> resources;
  std::vector<Function> functions;
  bool extended = false;
};

namespace detail {
struct Bytes {
  const uint8_t *data = nullptr;
  size_t size = 0;
  bool contains(uint64_t offset, uint64_t length) const {
    return offset <= size && length <= size - offset;
  }
  uint32_t word(size_t offset) const {
    return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
           (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
  }
  Bytes slice(size_t offset, size_t length) const { return {data + offset, length}; }
};
struct Budget {
  uint64_t left;
  bool exhausted = false;
  bool take(uint64_t n) { if (n > left) { exhausted = true; return false; } left -= n; return true; }
};
struct Part { uint32_t type; Bytes bytes; };

// All offsets are checked before reading. Intervals include each part header;
// overlapping parts and references back into the offset table are malformed.
inline bool Parts(Bytes bytes, uint32_t count, size_t offsets, std::vector<Part> &out) {
  if (!bytes.contains(offsets, uint64_t(count) * 4)) return false;
  const uint64_t first = offsets + uint64_t(count) * 4;
  std::vector<std::pair<uint64_t, uint64_t>> intervals;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t offset = bytes.word(offsets + size_t(i) * 4);
    if ((offset & 3) || offset < first || !bytes.contains(offset, 8)) return false;
    const uint32_t length = bytes.word(size_t(offset) + 4);
    if (!bytes.contains(uint64_t(offset) + 8, length)) return false;
    intervals.emplace_back(offset, uint64_t(offset) + 8 + length);
    out.push_back({bytes.word(offset), bytes.slice(size_t(offset) + 8, length)});
  }
  std::sort(intervals.begin(), intervals.end());
  for (size_t i = 1; i < intervals.size(); ++i)
    if (intervals[i].first < intervals[i - 1].second) return false;
  return true;
}
inline bool String(Bytes strings, uint32_t offset, std::string &out, Budget &budget) {
  out.clear();
  if (offset == UINT32_MAX) return true;
  if (!strings.contains(offset, 1)) return false;
  const auto *begin = strings.data + offset;
  const auto *end = static_cast<const uint8_t *>(std::memchr(begin, 0, strings.size - offset));
  if (!end) return false;
  for (auto p = begin; p < end;) {
    uint32_t c = *p++, extra = 0, minimum = 0;
    if (c < 0x80) continue;
    if (c >= 0xc2 && c <= 0xdf) { c &= 31; extra = 1; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { c &= 15; extra = 2; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { c &= 7; extra = 3; minimum = 0x10000; }
    else return false;
    if (size_t(end - p) < extra) return false;
    while (extra--) { if ((*p & 0xc0) != 0x80) return false; c = (c << 6) | (*p++ & 63); }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
  }
  if (!budget.take(size_t(end - begin) + 1)) return false;
  out.assign(reinterpret_cast<const char *>(begin), size_t(end - begin));
  return true;
}
inline bool Row(Bytes indices, uint32_t offset, Bytes &row) {
  row = {};
  if (offset == UINT32_MAX) return true;
  // RDAT index references are DWORD indices, not byte offsets.
  const uint64_t byte_offset = uint64_t(offset) * 4;
  if (!indices.contains(byte_offset, 4)) return false;
  const uint32_t count = indices.word(size_t(byte_offset));
  if (!indices.contains(byte_offset + 4, uint64_t(count) * 4)) return false;
  row = indices.slice(size_t(byte_offset) + 4, size_t(count) * 4);
  return true;
}
inline bool Table(Bytes bytes, uint32_t minimum, uint32_t &count, uint32_t &stride) {
  count = stride = 0;
  if (!bytes.data) return true;
  if (!bytes.contains(0, 8)) return false;
  count = bytes.word(0); stride = bytes.word(4);
  return stride >= minimum && !(stride & 3) && bytes.contains(8, uint64_t(count) * stride);
}
} // namespace detail

// Reads the documented DXC RDAT base records, not LLVM code or shader semantics.
// Output owns its metadata. Unknown/extended parts are flagged for the linker;
// they must not be silently interpreted as a complete executable state object.
inline Result Parse(const void *data, size_t size, Library &out) {
  out = {};
  if (!data || size < 32) return Result::Invalid;
  detail::Bytes container{static_cast<const uint8_t *>(data), size};
  if (container.word(0) != 0x43425844 || container.word(20) != 1) return Result::Invalid;
  const uint32_t length = container.word(24);
  if (length < 32 || length > size) return Result::Invalid;
  container.size = length;
  std::vector<detail::Part> chunks;
  if (!detail::Parts(container, container.word(28), 32, chunks)) return Result::Invalid;
  detail::Bytes rdat;
  bool dxil = false;
  for (auto chunk : chunks) {
    if (chunk.type == 0x4c495844) {
      if (dxil) return Result::Invalid;
      dxil = true;
    }
    if (chunk.type == 0x54414452) {
      if (rdat.data) return Result::Invalid;
      rdat = chunk.bytes;
    }
  }
  if (!rdat.data) return Result::Missing;
  if (!dxil || !rdat.contains(0, 8)) return Result::Invalid;
  if (rdat.word(0) != 0x10) return Result::Unsupported;
  std::vector<detail::Part> parts;
  if (!detail::Parts(rdat, rdat.word(4), 8, parts)) return Result::Invalid;
  Library result;
  detail::Bytes strings, indices, resources, functions;
  std::vector<uint32_t> seen;
  for (auto part : parts) {
    if (part.bytes.size & 3) return Result::Invalid;
    seen.push_back(part.type);
    switch (part.type) {
    case 1: strings = part.bytes; break;
    case 2: indices = part.bytes; break;
    case 3: resources = part.bytes; break;
    case 4: functions = part.bytes; break;
    default: result.extended = true; break;
    }
  }
  std::sort(seen.begin(), seen.end());
  if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) return Result::Invalid;
  uint32_t rc, rs, fc, fs;
  if (!detail::Table(resources, 32, rc, rs) || !detail::Table(functions, 44, fc, fs))
    return Result::Invalid;
  result.extended |= (rc && rs != 32) || (fc && fs != 44);
  // Shared rows/strings can amplify into quadratic owned output. Bound expansion
  // independently of allocation failure; this is a metadata preparation limit.
  detail::Budget budget{uint64_t(rdat.size) * 16 + 4096};
  if (!budget.take(uint64_t(rc) * sizeof(Resource) + uint64_t(fc) * sizeof(Function)))
    return Result::Unsupported;
  result.resources.reserve(rc); result.functions.reserve(fc);
  for (uint32_t i = 0; i < rc; ++i) {
    const auto row = resources.slice(8 + size_t(i) * rs, rs);
    Resource r{row.word(0), row.word(4), row.word(8), row.word(12), row.word(16), row.word(20), row.word(28), {}};
    if (r.type > 3 || r.lower > r.upper || !detail::String(strings, row.word(24), r.name, budget))
      return budget.exhausted ? Result::Unsupported : Result::Invalid;
    result.resources.push_back(std::move(r));
  }
  for (uint32_t i = 0; i < fc; ++i) {
    const auto row = functions.slice(8 + size_t(i) * fs, fs);
    Function f{};
    if (!detail::String(strings, row.word(0), f.name, budget) || f.name.empty() ||
        !detail::String(strings, row.word(4), f.unmangled, budget))
      return budget.exhausted ? Result::Unsupported : Result::Invalid;
    detail::Bytes rr, dd;
    if (!detail::Row(indices, row.word(8), rr) || !detail::Row(indices, row.word(12), dd))
      return Result::Invalid;
    if (!budget.take(rr.size + (dd.size / 4) * sizeof(std::string))) return Result::Unsupported;
    for (size_t j = 0; j < rr.size; j += 4) {
      const uint32_t index = rr.word(j);
      if (index >= rc) return Result::Invalid;
      f.resources.push_back(index);
    }
    for (size_t j = 0; j < dd.size; j += 4) {
      std::string name;
      if (!detail::String(strings, dd.word(j), name, budget) || name.empty())
        return budget.exhausted ? Result::Unsupported : Result::Invalid;
      f.dependencies.push_back(std::move(name));
    }
    f.kind = row.word(16); f.payload = row.word(20); f.attributes = row.word(24);
    f.features_low = row.word(28); f.features_high = row.word(32);
    f.stages = row.word(36); f.target = row.word(40);
    result.functions.push_back(std::move(f));
  }
  out = std::move(result);
  return Result::Ready;
}

struct Export { std::string name, source; };
struct SelectedExport { std::string name; uint32_t function; };
inline Result Select(const Library &library, const std::vector<Export> &exports,
                     std::vector<SelectedExport> &out) {
  out.clear();
  if (library.extended) return Result::Unsupported;
  std::vector<SelectedExport> selected;
  using NameIndex = std::pair<std::string_view, uint32_t>;
  std::vector<NameIndex> names_index, unmangled_index;
  for (uint32_t i = 0; i < library.functions.size(); ++i) {
    names_index.emplace_back(library.functions[i].name, i);
    unmangled_index.emplace_back(library.functions[i].unmangled, i);
  }
  std::sort(names_index.begin(), names_index.end());
  std::sort(unmangled_index.begin(), unmangled_index.end());
  const auto lookup = [](const std::vector<NameIndex> &index, std::string_view name, uint32_t &found) {
    auto it = std::lower_bound(index.begin(), index.end(), name,
        [](const NameIndex &entry, std::string_view value) { return entry.first < value; });
    if (it == index.end() || it->first != name) return 0;
    found = it->second;
    return it + 1 != index.end() && (it + 1)->first == name ? 2 : 1;
  };
  if (exports.empty()) {
    for (uint32_t i = 0; i < library.functions.size(); ++i)
      selected.push_back({library.functions[i].name, i});
  } else {
    for (const auto &e : exports) {
      if (e.name.empty()) return Result::Invalid;
      const auto &source = e.source.empty() ? e.name : e.source;
      uint32_t found = UINT32_MAX;
      int matches = lookup(names_index, source, found);
      if (matches == 2) return Result::Invalid;
      if (!matches) {
        matches = lookup(unmangled_index, source, found);
        if (matches == 2) return Result::Unsupported; // Overload expansion needs linker reflection.
      }
      if (!matches) return Result::Invalid;
      selected.push_back({e.name, found});
    }
  }
  std::vector<std::string> names;
  for (const auto &e : selected) names.push_back(e.name);
  std::sort(names.begin(), names.end());
  if (std::adjacent_find(names.begin(), names.end()) != names.end()) return Result::Invalid;
  out = std::move(selected);
  return Result::Ready;
}

// DXIL names use UTF-8; D3D12 export descriptors use Windows UTF-16. The wide
// variant also supports native CPU tests where wchar_t is a Unicode scalar.
inline bool Utf8(const wchar_t *name, std::string &out) {
  out.clear();
  if (!name) return false;
  std::string value;
  for (size_t i = 0; name[i]; ++i) {
    uint32_t c = uint32_t(name[i]);
    if (c >= 0xd800 && c <= 0xdbff) {
      const uint32_t low = uint32_t(name[++i]);
      if (low < 0xdc00 || low > 0xdfff) return false;
      c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
    } else if ((c >= 0xdc00 && c <= 0xdfff) || c > 0x10ffff) return false;
    if (c < 0x80) value += char(c);
    else if (c < 0x800) { value += char(0xc0 | (c >> 6)); value += char(0x80 | (c & 63)); }
    else if (c < 0x10000) {
      value += char(0xe0 | (c >> 12)); value += char(0x80 | ((c >> 6) & 63)); value += char(0x80 | (c & 63));
    } else {
      value += char(0xf0 | (c >> 18)); value += char(0x80 | ((c >> 12) & 63));
      value += char(0x80 | ((c >> 6) & 63)); value += char(0x80 | (c & 63));
    }
  }
  out = std::move(value);
  return true;
}

struct Prepared {
  std::vector<uint8_t> bytecode;
  Library reflection;
  std::vector<SelectedExport> exports;
};
inline Result Prepare(const void *data, size_t size, const std::vector<Export> &exports, Prepared &out) {
  out = {};
  Prepared result;
  auto status = Parse(data, size, result.reflection);
  if (status != Result::Ready) return status;
  status = Select(result.reflection, exports, result.exports);
  if (status != Result::Ready) return status;
  detail::Bytes bytes{static_cast<const uint8_t *>(data), size};
  result.bytecode.assign(bytes.data, bytes.data + bytes.word(24));
  out = std::move(result);
  return Result::Ready;
}
} // namespace dxmt::ray_library
