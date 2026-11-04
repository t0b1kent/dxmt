// SPDX-License-Identifier: LGPL-2.1-or-later
// MacRunner: metadata only. Retain view history before first attachment use and report late views too.
#pragma once
#include "d3d12_c17_stats.hpp"
#include "d3d12.h"
#include "dxmt_texture.hpp"
#include <array>
#include <cstdarg>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt::attachment_audit {
inline bool Enabled() {
  static const bool v = c17::ReadKey("MACRUNNER_DX12_ATTACHMENT_AUDIT", 1) == 1;
  return v;
}
struct View {
  std::array<uint64_t, 11> fields; // kind, DXGI, dimension, key, native format, mip, mip count, slice, slices, plane, flags
};
struct Resource {
  uint64_t id, object;
  D3D12_RESOURCE_DESC desc;
  WMTTextureInfo native;
  unsigned raw_mips, typeless, heap_type, heap_flags, placed;
  bool announced = false, view_limit = false, use_limit = false;
  std::vector<View> views;
  std::vector<std::array<uint64_t, 5>> uses;
};
struct Registry {
  std::mutex mutex;
  uint64_t next_id = 0, rows = 0;
  std::unordered_map<uint64_t, Resource> resources;
  std::unordered_set<uint64_t> unknown;
};
inline Registry &Get() { static Registry r; return r; }
// Called with Registry::mutex held: records from this diagnostic cannot interleave.
inline void Line(const char *format, ...) {
  auto &r = Get();
  const auto row = r.rows++;
  if (row > 32768) return;
  const DWORD error = GetLastError();
  char text[1400];
  int n = std::snprintf(text, sizeof(text), "dx12_attachment row=%llu ", (unsigned long long)row);
  if (row == 32768) n += std::snprintf(text+n, sizeof(text)-n, "op=limit cap=32768 incomplete=1");
  else {
    va_list args; va_start(args, format);
    n += std::vsnprintf(text+n, sizeof(text)-n, format, args); va_end(args);
  }
  if (n > 0 && size_t(n)+1 < sizeof(text)) {
    text[n++]='\n'; DWORD written; WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, DWORD(n), &written, nullptr);
  }
  SetLastError(error);
}
inline void Register(uint64_t object, const D3D12_RESOURCE_DESC &desc, const WMTTextureInfo &native,
                     unsigned raw_mips, unsigned typeless, unsigned heap_type, unsigned heap_flags, bool placed) {
  if (!Enabled()) return;
  auto &r=Get(); std::lock_guard lock(r.mutex);
  if (r.resources.size() >= 8192 && !r.resources.count(object)) {
    Line("op=untracked object=%llx reason=resource_cap incomplete=1", (unsigned long long)object); return;
  }
  r.resources.insert_or_assign(object, Resource{++r.next_id,object,desc,native,raw_mips,typeless,heap_type,heap_flags,unsigned(placed)});
}
inline void PrintView(const Resource &r, const View &v) {
  const auto &f=v.fields;
  Line("op=view id=%llu object=%llx kind=%llu dxgi=%llu dimension=%llu key=%llx metal_format=%llu mip=%llu mips=%llu slice=%llu slices=%llu plane=%llu flags=%llu",
      (unsigned long long)r.id,(unsigned long long)r.object,(unsigned long long)f[0],(unsigned long long)f[1],
      (unsigned long long)f[2],(unsigned long long)f[3],(unsigned long long)f[4],(unsigned long long)f[5],
      (unsigned long long)f[6],(unsigned long long)f[7],(unsigned long long)f[8],(unsigned long long)f[9],(unsigned long long)f[10]);
}
inline void Forget(uint64_t object) {
  if (!Enabled()) return;
  auto &r=Get(); std::lock_guard lock(r.mutex);
  r.resources.erase(object); r.unknown.erase(object);
}
inline void CreatedView(uint64_t object, unsigned kind, unsigned dxgi, unsigned dimension, TextureViewKey key,
                        const TextureViewDescriptor &view, unsigned plane=0, unsigned flags=0) {
  if (!Enabled()) return;
  auto &r=Get(); std::lock_guard lock(r.mutex);
  auto it=r.resources.find(object); if (it==r.resources.end()) return;
  auto &resource=it->second;
  View v{{kind,dxgi,dimension,uint64_t(key),unsigned(view.format),view.firstMiplevel,view.miplevelCount,
          view.firstArraySlice,view.arraySize,plane,flags}};
  for (const auto &old:resource.views) if (old.fields==v.fields) return;
  if (resource.views.size()>=64) {
    if (!resource.view_limit) Line("op=limit id=%llu reason=view_cap cap=64 incomplete=1", (unsigned long long)resource.id);
    resource.view_limit=true; return;
  }
  resource.views.push_back(v);
  if (resource.announced) PrintView(resource,v);
}
inline void Use(uint64_t object, uint64_t view, unsigned slot, unsigned mip, unsigned slice, unsigned plane) {
  if (!Enabled()) return;
  auto &r=Get(); std::lock_guard lock(r.mutex);
  auto it=r.resources.find(object);
  if (it==r.resources.end()) {
    if (r.unknown.size()<8192 && r.unknown.insert(object).second)
      Line("op=unknown object=%llx incomplete=1",(unsigned long long)object);
    return;
  }
  auto &e=it->second; const auto &d=e.desc;
  if (!e.announced) {
    e.announced=true;
    Line("op=resource id=%llu object=%llx dxgi=%u typeless=%u flags=%u uav=%u simultaneous=%u dimension=%u width=%llu height=%u depth_or_array=%u raw_mips=%u mips=%u samples=%u metal_format=%u metal_usage=%u metal_options=%u heap_type=%u heap_flags=%u placed=%u incomplete=%u",
        (unsigned long long)e.id,(unsigned long long)object,unsigned(d.Format),e.typeless,unsigned(d.Flags),
        unsigned(bool(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)),unsigned(bool(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)),
        unsigned(d.Dimension),(unsigned long long)d.Width,d.Height,unsigned(d.DepthOrArraySize),e.raw_mips,unsigned(d.MipLevels),d.SampleDesc.Count,
        unsigned(e.native.pixel_format),unsigned(e.native.usage),unsigned(e.native.options),e.heap_type,e.heap_flags,e.placed,unsigned(e.view_limit));
    for (const auto &v:e.views) PrintView(e,v);
  }
  const std::array<uint64_t,5> use{view,slot,mip,slice,plane};
  for (const auto &old:e.uses) if (old==use) return;
  if (e.uses.size()>=64) {
    if (!e.use_limit) Line("op=limit id=%llu reason=use_cap cap=64 incomplete=1",(unsigned long long)e.id);
    e.use_limit=true;return;
  }
  e.uses.push_back(use);
  Line("op=attachment id=%llu object=%llx view_key=%llx slot=%u mip=%u slice=%u plane=%u",
      (unsigned long long)e.id,(unsigned long long)object,(unsigned long long)view,slot,mip,slice,plane);
}
} // namespace dxmt::attachment_audit
