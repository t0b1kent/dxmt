// SPDX-License-Identifier: LGPL-2.1-or-later
// MacRunner reuse observation; disabled unless the owner requests a present window.
#pragma once
#include "d3d12_reuse_model.hpp"
#include "d3d12_c17_stats.hpp"
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <deque>
#include <atomic>

namespace dxmt::reuse {
inline unsigned Period() { static const auto n = c17::ReadKey("MACRUNNER_DX12_REUSE_AUDIT", 1000000); return n; }
inline bool Enabled() { return Period() != 0; }
// MACRUNNER_DX12_REUSE_AUDIT_FROM=<F> (default 0 = every window): capture only presents [F, F+N), i.e. exactly one
// report window, then stop. Outside it only resource identities, CPU write epochs and PSO metadata are kept.
inline unsigned From() { static const auto n = c17::ReadKey("MACRUNNER_DX12_REUSE_AUDIT_FROM", 100000000); return n; }
inline std::atomic<uint64_t> &PresentCount() { static std::atomic<uint64_t> n{0}; return n; }
inline bool InWindow(uint64_t present) { return !From() || (present >= From() && present < uint64_t(From()) + Period()); }
inline bool Active() { return Enabled() && InWindow(PresentCount().load(std::memory_order_relaxed)); }
// MACRUNNER_DX12_REUSE_AUDIT_CB=<sha1-prefix>[,<sha1-prefix>...] names shaders whose CBV
// 16-byte registers are printed with exact values. Default: the two loop-threshold PSs
// (tbl-counters 22.10, cb(k28,i0) words 24/25/28/29/30). "none" disables the dump.
inline const std::vector<std::string> &WatchedShaders() {
  static const std::vector<std::string> list = [] {
    std::vector<std::string> out;
    const DWORD error = GetLastError();
    char text[512]{};
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_REUSE_AUDIT_CB", text, sizeof(text));
    SetLastError(error);
    const std::string value = n > 0 && n < sizeof(text) ? std::string(text, n) : std::string("1413cf47,9ed68bd4");
    if (value == "none") return out;
    std::string item;
    auto flush = [&] {
      bool hex = !item.empty() && item.size() <= 40;
      for (auto &c : item) { if (c >= 'A' && c <= 'F') c = char(c - 'A' + 'a'); hex &= (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
      if (hex && out.size() < 16) out.push_back(item);
      item.clear();
    };
    for (char c : value) { if (c == ',') flush(); else item.push_back(c); }
    flush();
    return out;
  }();
  return list;
}
inline bool Watched(const std::string &sha1) {
  for (const auto &prefix : WatchedShaders()) if (sha1.compare(0, prefix.size(), prefix) == 0) return true;
  return false;
}
struct Resource {
  uint64_t id = 0, version = 1, gpu_writes = 0, cpu_writes = 0;
  uint64_t object = 0, address = 0, length = 0, heap = 0;
  const void *identity = nullptr;
  const uint8_t *mapped = nullptr;
  uint32_t width = 0, height = 0, format = 0;
  bool cpu_visible = false, sparse = false, live = true;
  struct Clear {
    bool known = false;
    uint64_t before = 0, view = 0;
    uint32_t width = 0, height = 0, array = 0, plane = 0, flags = 0;
    Digest value;
  } clear;
};
using ResourcePtr = std::shared_ptr<Resource>;
struct Binding {
  ResourcePtr resource, counter;
  uint64_t offset = 0, length = 0, counter_offset = 0;
  Digest descriptor;
  uint32_t type = 0;
  bool write = false, cbv = false;
};
struct ShaderRegister { uint32_t stage, space, binding, reg, bound, flags; };
struct ShaderInfo {
  std::vector<ShaderRegister> registers;
  uint64_t unknown = 0;
  std::string sha1;                                      // single shader: bytecode SHA-1
  std::vector<std::pair<uint32_t, std::string>> hashes;  // pipeline: stage slot -> SHA-1
  bool watched = false;
  static bool Visible(uint32_t stage, uint32_t visibility) {
    constexpr unsigned visible[] = {5,1,4,2,3,0};
    return visibility == 0 || (stage < 6 && visible[stage] == visibility);
  }
  uint32_t Flags(RegisterKey key) const {
    unsigned flags=0;
    for (const auto &r:registers) if (r.space==key.space && r.binding==key.binding && r.reg==key.index && Visible(r.stage,key.stage)) flags|=r.flags;
    return flags;
  }
  uint64_t Bound(RegisterKey key) const {
    uint64_t bytes=0;
    for (const auto &r:registers) if (r.space==key.space && r.binding==key.binding && Visible(r.stage,key.stage)) bytes=std::max(bytes,uint64_t(r.bound)*16);
    return bytes;
  }
};
struct State {
  std::recursive_mutex mutex;
  std::mutex public_submission;
  uint64_t next_id = 0, gpu_writes = 0, cpu_writes = 0, missing_writes = 0;
  uint64_t registry_overflow = 0, exceptions = 0;
  std::unordered_map<const void *, ResourcePtr> identities;
  std::unordered_map<uint64_t, ResourcePtr> objects;
  std::map<uint64_t, ResourcePtr> addresses;
  std::set<const void *> queues;
  std::unordered_map<const void *, std::deque<Pass>> pending;
  uint64_t pending_count = 0;
  bool frame_incomplete = false;
  Window window;
};
inline State &Get() { static State s; return s; }
// On mode only: preserves one public Execute/Present boundary while the existing
// async Present wait drains its FIFO. The submit worker never takes this lock.
struct PublicSubmission {
  std::unique_lock<std::mutex> lock;
  explicit PublicSubmission(bool public_call) {
    if (Active() && public_call) lock = std::unique_lock(Get().public_submission);
  }
};
inline uint64_t ObjectId() {
  if (!Enabled()) return 0;
  auto &s = Get(); std::lock_guard lock(s.mutex); return ++s.next_id;
}

inline ResourcePtr Register(const void *identity, uint64_t object, uint64_t address, uint64_t length,
                            const void *mapped, bool cpu_visible, uint64_t heap,
                            uint32_t width, uint32_t height, uint32_t format, bool sparse = false) {
  if (!Enabled() || !identity || !object) return {};
  auto &s = Get(); std::lock_guard lock(s.mutex);
  try {
    if (s.identities.size() >= 65536) { ++s.registry_overflow; return {}; }
    auto r = std::make_shared<Resource>();
    r->id = ++s.next_id; r->identity = identity; r->object = object; r->address = address; r->length = length;
    r->mapped = static_cast<const uint8_t *>(mapped); r->cpu_visible = cpu_visible; r->heap = heap;
    r->width = width; r->height = height; r->format = format; r->sparse = sparse;
    s.identities[identity] = r; s.objects[object] = r; if (address) s.addresses[address] = r;
    return r;
  } catch (...) { ++s.exceptions; return {}; }
}
inline void Retire(const ResourcePtr &r) {
  if (!r) return;
  auto &s = Get(); std::lock_guard lock(s.mutex); r->live = false; r->mapped = nullptr;
  if (auto i = s.identities.find(r->identity); i != s.identities.end() && i->second == r) s.identities.erase(i);
  if (auto i = s.objects.find(r->object); i != s.objects.end() && i->second == r) s.objects.erase(i);
  if (auto i = s.addresses.find(r->address); i != s.addresses.end() && i->second == r) s.addresses.erase(i);
}
inline ResourcePtr FindIdentity(const void *identity) {
  auto &s = Get(); std::lock_guard lock(s.mutex);
  auto it = s.identities.find(identity); return it == s.identities.end() ? ResourcePtr{} : it->second;
}
inline ResourcePtr FindObject(uint64_t object) {
  auto &s = Get(); std::lock_guard lock(s.mutex);
  auto it = s.objects.find(object); return it == s.objects.end() ? ResourcePtr{} : it->second;
}
inline ResourcePtr FindAddress(uint64_t address, uint64_t &offset) {
  auto &s = Get(); std::lock_guard lock(s.mutex); offset = 0;
  auto it = s.addresses.upper_bound(address);
  if (it == s.addresses.begin()) return {};
  --it; auto r = it->second;
  if (address - it->first >= r->length) return {};
  offset = address - it->first; return r;
}
inline void Write(const ResourcePtr &r, bool cpu = false) {
  if (!Enabled()) return;
  auto &s = Get(); std::lock_guard lock(s.mutex);
  if (!r || !r->live) { ++s.missing_writes; return; }
  ++r->version;
  r->clear.known = false;
  if (cpu) { ++r->cpu_writes; ++s.cpu_writes; } else { ++r->gpu_writes; ++s.gpu_writes; }
  // Conservative alias invalidation: all allocations in a placement heap share an epoch.
  // Sparse resources remain explicitly incomplete until tile mappings are captured.
  if (r->heap) for (auto &[_, other] : s.identities)
    if (other != r && other->heap == r->heap) { ++other->version; other->clear.known = false; }
}
inline void UnknownWrite() {
  auto &s=Get();std::lock_guard lock(s.mutex);++s.missing_writes;
  for (auto &[_,r]:s.identities) { ++r->version;r->clear.known=false; }
}
inline void WriteObject(uint64_t object) {
  if (!Enabled() || !object) return;
  auto r=FindObject(object);
  if (r) Write(r);else UnknownWrite();
}

inline void Read(Signature &sig, const Binding &b, Budget &budget, PSO *pso = nullptr,
                 RegisterKey key = {}, const ShaderInfo *shader = nullptr,
                 uint32_t param = SlotUnknown, uint32_t slot = SlotUnknown) {
  auto &w = Get().window;
  sig.part[Descriptors].Join(b.descriptor);
  const auto &r = b.resource;
  auto unobserved = [&] { if (b.cbv && pso && pso->watched) ++pso->unobserved_constants[RegisterKey{key.stage, key.space, key.binding, 0}]; };
  if (!r || !r->live) { sig.unknown |= MissingResource; unobserved(); return; }
  sig.Word(Resources, r->id); sig.Word(Resources, r->version);
  sig.Word(Resources, b.offset); sig.Word(Resources, b.length);
  if (r->sparse) sig.unknown |= Aliasing;
  if (b.offset > r->length || (r->address && b.length > r->length - b.offset)) { sig.unknown |= InvalidRange; unobserved(); return; }
  if (r->cpu_visible) {
    if (!r->mapped || !b.length) { sig.unknown |= InvalidRange; unobserved(); return; }
    if (!budget.Take(b.length)) { sig.unknown |= ByteLimit; unobserved(); return; }
    const void *data = r->mapped + b.offset;
    sig.Bytes(Upload, data, size_t(b.length));
    if (b.cbv) for (uint64_t offset = 0; offset + 16 <= b.length; offset += 16) {
      key.index = uint32_t(offset / 16);
      const auto flags=shader?shader->Flags(key):0;
      w.ObserveRegister(pso, key, r->mapped + b.offset + offset, flags&2, flags&4, flags&1, param, slot);
    }
  } else if (b.cbv && pso) {
    ++pso->metadata_unknown; // GPU-written constants are never read before completion.
    if (pso->watched) ++pso->gpu_constants[RegisterKey{key.stage, key.space, key.binding, 0}];
  }
  if (b.counter) {
    sig.Word(Resources, b.counter->id); sig.Word(Resources, b.counter->version);
  }
}

template <typename... Args> inline void Line(const char *fmt, Args... args) {
  const DWORD error = GetLastError(); char line[1024];
  const int n = std::snprintf(line, sizeof(line), fmt, args...); DWORD written;
  if (n > 0 && size_t(n) < sizeof(line)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
  SetLastError(error);
}
inline void Report() {
  auto &s = Get(); auto &w = s.window;
  Line("dx12_reuse window=%llu presents=%llu hash_bytes=%llu refused_bytes=%llu refused_ranges=%llu cap_bytes_per_frame=%llu skipped_passes=%llu skipped_psos=%llu gpu_writes=%llu cpu_writes=%llu unknown_writes=%llu exceptions=%llu ticks=%llu\n",
       w.total_presents / Period(), w.presents, w.bytes, w.refused_bytes, w.refused_ranges, ByteBudget,
       w.skipped_passes, w.skipped_psos, s.gpu_writes, s.cpu_writes, s.missing_writes, s.exceptions, w.observed_ticks);
  for (unsigned i = 0; i < TargetClasses; ++i) {
    auto &c = w.counts[i];
    Line("dx12_reuse_class class=%s metal_segments=%llu compared=%llu identical=%llu draws=%llu identical_draws=%llu incomplete=%llu ideal_redundant_segments=%llu feedback_draws=%llu feedback_passes=%llu\n",
         TargetNames[i], c.passes, c.compared, c.repeated_passes, c.draws, c.repeated_draws, c.incomplete, c.retained_outputs,
         c.feedback_draws, c.feedback_passes);
    for (unsigned j = 0; j < c.first_reason.size(); ++j) if (c.first_reason[j])
      Line("dx12_reuse_mismatch class=%s first=%s count=%llu\n", TargetNames[i], PartNames[j], c.first_reason[j]);
  }
  std::vector<std::pair<uint64_t, const PSO *>> top;
  for (const auto &[id, pso] : w.psos) top.push_back({id, &pso});
  std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.second->draws > b.second->draws; });
  if (top.size() > 16) top.resize(16);
  for (auto [id, p] : top) {
    uint64_t stable = 0, branch = 0, loop = 0, loaded = 0, partial = 0, overflow = 0;
    std::array<uint64_t, MaxValues + 1> histogram{};
    for (const auto &[key, reg] : p->registers) {
      if (reg.observations != p->draws) { ++partial; continue; }
      if (reg.overflow) { ++overflow; ++histogram[MaxValues]; }
      else if (reg.distinct) ++histogram[reg.distinct - 1];
      if (reg.distinct != 1 || reg.overflow) continue;
      ++stable; branch += reg.branch; loop += reg.loop; loaded += reg.loaded;
    }
    Line("dx12_reuse_pso id=%llx draws=%llu registers=%llu stable=%llu partial=%llu stable_branch=%llu stable_loop=%llu ideal_removed_constant_registers=%llu physical_register_saving=unknown metadata_unknown=%llu register_cap=%llu values_gt8=%llu\n",
         id, p->draws, uint64_t(p->registers.size()), stable, partial, branch, loop, loaded,
         p->metadata_unknown, p->missed_registers, overflow);
    for (unsigned i = 0; i < histogram.size(); ++i) if (histogram[i])
      Line("dx12_reuse_values pso=%llx distinct=%s%u registers=%llu\n", id, i == MaxValues ? ">" : "", i == MaxValues ? unsigned(MaxValues) : i + 1, histogram[i]);
    for (const auto &[key, reg] : p->registers)
      Line("dx12_reuse_register pso=%llx visibility=%u space=%u cb=%u reg=%u observed=%llu draws=%llu distinct=%s%u stable=%u loaded=%u may_branch=%u may_loop=%u\n",
           id,key.stage,key.space,key.binding,key.index,reg.observations,p->draws,reg.overflow?">":"",reg.distinct,
           unsigned(reg.observations==p->draws && reg.distinct==1 && !reg.overflow),unsigned(reg.loaded),unsigned(reg.branch),unsigned(reg.loop));
  }
  // Named shaders: exact CBV register values, every value with its draw count.
  Line("dx12_reuse_cbwatch watched_psos=%llu watched_registers=%llu watched_skipped=%llu prefixes=%llu\n",
       w.watched_psos, w.watched_registers, w.watched_skipped, uint64_t(WatchedShaders().size()));
  unsigned dump_lines = 0, dump_dropped = 0;
  auto source = [](uint32_t v, char *out, size_t n) {
    if (v == SlotUnknown) std::snprintf(out, n, "?");
    else if (v == SlotRootDescriptor) std::snprintf(out, n, "root_cbv");
    else if (v == SlotRootConstants) std::snprintf(out, n, "root_constants");
    else std::snprintf(out, n, "%u", v);
  };
  for (const auto &[id, p] : w.psos) {
    if (!p.watched) continue;
    std::string stages;
    static constexpr const char *names[] = {"vs", "ps", "hs", "ds", "gs", "cs"};
    for (const auto &[stage, hash] : p.shaders) {
      if (!stages.empty()) stages += ',';
      stages += stage < 6 ? names[stage] : "?"; stages += ':'; stages += hash;
    }
    Line("dx12_reuse_cbdump pso=%llx draws=%llu registers=%llu register_cap=%llu metadata_unknown=%llu shaders=%s\n",
         id, p.draws, uint64_t(p.registers.size()), p.missed_registers, p.metadata_unknown, stages.c_str());
    for (const auto &[key, n] : p.gpu_constants)
      Line("dx12_reuse_cbgpu pso=%llx visibility=%u space=%u cb=%u gpu_resident_draws=%llu values=unobservable\n",
           id, key.stage, key.space, key.binding, n);
    for (const auto &[key, n] : p.unobserved_constants)
      Line("dx12_reuse_cbmiss pso=%llx visibility=%u space=%u cb=%u refused_draws=%llu values=unobserved\n",
           id, key.stage, key.space, key.binding, n);
    for (const auto &[key, reg] : p.registers) {
      char param[24], slot[24]; source(reg.param, param, sizeof(param)); source(reg.slot, slot, sizeof(slot));
      for (unsigned i = 0; i < reg.distinct; ++i) {
        if (dump_lines >= 4096) { ++dump_dropped; continue; }
        ++dump_lines;
        uint32_t u[4]; float f[4]; std::memcpy(u, reg.values[i].data(), 16); std::memcpy(f, reg.values[i].data(), 16);
        Line("dx12_reuse_cbval pso=%llx visibility=%u space=%u cb=%u param=%s slot=%s%s reg=%u words=%u-%u observed=%llu draws=%llu value=%u/%s%u value_draws=%llu loaded=%u may_branch=%u may_loop=%u hex=%08x,%08x,%08x,%08x f=%.9g,%.9g,%.9g,%.9g\n",
             id, key.stage, key.space, key.binding, param, slot, reg.multiple_sources ? "+" : "", key.index,
             key.index * 4, key.index * 4 + 3, reg.observations, p.draws, i + 1, reg.overflow ? ">" : "", reg.distinct,
             reg.hits[i], unsigned(reg.loaded), unsigned(reg.branch), unsigned(reg.loop),
             u[0], u[1], u[2], u[3], double(f[0]), double(f[1]), double(f[2]), double(f[3]));
      }
      if (reg.overflow)
        Line("dx12_reuse_cbval_overflow pso=%llx visibility=%u space=%u cb=%u reg=%u draws_with_9th_or_later_value=%llu\n",
             id, key.stage, key.space, key.binding, key.index, reg.overflow_hits);
    }
  }
  if (dump_dropped) Line("dx12_reuse_cbdump_cap printed=%u dropped=%u\n", dump_lines, dump_dropped);
  std::vector<ResourcePtr> written;
  for (const auto &[identity,r]:s.identities) if (r->gpu_writes) written.push_back(r);
  std::sort(written.begin(),written.end(),[](auto &a,auto &b){return a->gpu_writes>b->gpu_writes;});
  if (written.size()>16) written.resize(16);
  for (const auto &r:written)
    Line("dx12_reuse_resource id=%llx bytes=%llu width=%u height=%u format=%u version=%llu gpu_write_boundaries=%llu cpu_write_boundaries=%llu\n",
         r->id,r->length,r->width,r->height,r->format,r->version,r->gpu_writes,r->cpu_writes);
}
inline void Presented() {
  if (!Enabled()) return;
  if (!InWindow(PresentCount().fetch_add(1, std::memory_order_relaxed))) return;
  auto &s = Get(); std::lock_guard lock(s.mutex);
  try {
    if (s.frame_incomplete) {
      // A late cap/failure must retract earlier claims in this reporting window.
      for (auto &c:s.window.counts) { c.repeated_passes=c.repeated_draws=c.retained_outputs=0; c.incomplete=c.passes; }
      for (auto &p:s.window.current) p.signature.unknown|=PassLimit;
    }
    // Pending full clears absorbed into a render encoder are intentionally absent
    // from its actual encoder plan. No captured input may survive into another frame.
    s.pending.clear(); s.pending_count=0;
    s.window.Present(); s.frame_incomplete=false;
    if (s.window.presents >= Period()) { Report(); s.window.ResetWindow(); }
  }
  catch (...) { ++s.exceptions; }
}
struct RecordedPass;
} // namespace dxmt::reuse
