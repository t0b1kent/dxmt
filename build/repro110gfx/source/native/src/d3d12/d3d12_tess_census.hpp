#pragma once
// GPU-PASS 25.10.2026 (curator): MACRUNNER_DX12_TESS_CENSUS=<N> (default 0 = off, nothing is computed). Observation only:
// nothing is skipped, merged or reordered. Every N presents prints, to stderr:
//  * per tessellation pipeline (id, HS/DS SHA-1 prefix of the application's bytecode, partitioning, domain, max tess factor,
//    control points): draws, patches, instances, indirect draws, and frame-to-frame repetition of its inputs, matched by
//    (pipeline, n-th draw of that pipeline in the frame), each part separately:
//      const   root constants + root CBV/SRV/UAV addresses (CBV CONTENTS are not hashed: no GPU-address -> CPU map here)
//      vbib    vertex/index buffer views + draw arguments
//      tables  descriptor-table base handles
//      tex     the textures the pre-raster stages (VS/HS/DS/GS) can read: declared SRV registers of tables visible to them,
//              resolved to Metal texture handles (unknown when a range is undeclared/unbounded or the heap is missing)
//    Tessellation factors are computed on the GPU by the hull shader and are not visible here (only the declared maximum).
//  * copies (CopyBufferRegion / CopyTextureRegion / CopyResource): count and bytes by kind and source/destination heap type,
//    DEFAULT-heap destinations copied again while still in COPY_DEST without a transition out of it (written twice, read never
//    between),
//    and content repetition for CPU-visible sources (hash of the first and last 32 KB) per destination range.
// Lines: dx12_tess_census / dx12_tess_census_total / dx12_copy_census.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace dxmt::tess_census {

inline unsigned
Period() {
  static const unsigned n = [] {
    const DWORD error = GetLastError();
    char value[12] = {};
    unsigned parsed = 0;
    const DWORD len = GetEnvironmentVariableA("MACRUNNER_DX12_TESS_CENSUS", value, sizeof(value));
    if (len && len < 8) for (DWORD i = 0; i < len && value[i] >= '0' && value[i] <= '9'; ++i) parsed = parsed * 10 + unsigned(value[i] - '0');
    SetLastError(error);
    return parsed;
  }();
  return n;
}
inline bool Enabled() { return Period() != 0; }

inline uint64_t
Hash(uint64_t h, const void *data, size_t n) {   // FNV-1a 64
  const auto *p = static_cast<const unsigned char *>(data);
  if (!h) h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
inline uint64_t Word(uint64_t h, uint64_t v) { return Hash(h, &v, sizeof(v)); }

struct PsoInfo {
  uint32_t id = 0, partition = 0, domain = 0, in_cp = 0, out_cp = 0;
  float max_factor = 0;
  char hs[13] = {}, ds[13] = {};
};
struct Sig { uint64_t c = 0, v = 0, tb = 0, t = 0, cp = 0, tbp = 0; bool tex_unknown = false; };   // cp/tbp: root parameters visible to pre-raster stages

struct PsoAgg {
  PsoInfo info;
  uint64_t draws = 0, patches = 0, instances = 0, indirect = 0, indirect_max = 0, patches_max = 0;
  uint64_t compared = 0, same_c = 0, same_v = 0, same_tb = 0, same_t = 0, same_all = 0, tex_unknown = 0, any_prev = 0;
  // twins: a draw of another pipeline with the SAME HS/DS earlier in the same frame (first such draw is the reference)
  uint64_t twin_compared = 0, twin_c = 0, twin_v = 0, twin_tb = 0, twin_all = 0;
};
struct CopyAgg { uint64_t n = 0, bytes = 0, hashed = 0, repeat = 0, unread_overwrite = 0; };

struct State {
  std::mutex m;
  std::map<uint32_t, PsoAgg> agg;
  std::map<std::pair<uint32_t, uint32_t>, Sig> cur, prev;
  std::map<uint32_t, uint32_t> occ;
  std::map<std::string, std::pair<uint32_t, Sig>> twin_first;   // HS+DS id -> (pipeline, signature) of its first draw this frame
  std::set<std::tuple<uint32_t, uint64_t, uint64_t, uint64_t, uint64_t>> cur_all, prev_all;
  std::map<std::string, CopyAgg> copies;
  std::map<std::tuple<const void *, uint64_t, uint64_t>, uint64_t> last_content;
  std::set<const void *> pending;   // copy destinations not yet transitioned out of COPY_DEST
  uint64_t presents = 0, frames = 0;
  uint32_t next_id = 1;
};
inline State &Get() { static State s; return s; }
inline uint64_t Frame() { auto &s = Get(); std::lock_guard<std::mutex> g(s.m); return s.presents; }

inline std::shared_ptr<const PsoInfo>
MakeInfo(const std::string &hs_hex, const std::string &ds_hex, uint32_t partition, uint32_t domain, uint32_t in_cp,
         uint32_t out_cp, float max_factor) {
  auto info = std::make_shared<PsoInfo>();
  {
    auto &s = Get(); std::lock_guard<std::mutex> g(s.m);
    info->id = s.next_id++;
  }
  info->partition = partition; info->domain = domain; info->in_cp = in_cp; info->out_cp = out_cp; info->max_factor = max_factor;
  std::memcpy(info->hs, hs_hex.c_str(), hs_hex.size() < 12 ? hs_hex.size() : 12);
  std::memcpy(info->ds, ds_hex.c_str(), ds_hex.size() < 12 ? ds_hex.size() : 12);
  return info;
}

inline void
Draw(const PsoInfo &info, uint64_t patches, uint64_t instances, uint64_t indirect_max, const Sig &sig) {
  auto &s = Get(); std::lock_guard<std::mutex> g(s.m);
  auto &a = s.agg[info.id];
  a.info = info;
  ++a.draws;
  if (indirect_max) { ++a.indirect; a.indirect_max += indirect_max; }
  a.patches += patches * instances; a.instances += instances;
  if (patches * instances > a.patches_max) a.patches_max = patches * instances;
  if (sig.tex_unknown) ++a.tex_unknown;
  const uint32_t n = s.occ[info.id]++;
  const auto key = std::make_pair(info.id, n);
  s.cur[key] = sig;
  s.cur_all.insert({info.id, sig.c, sig.v, sig.tb, sig.t});
  if (s.prev_all.count({info.id, sig.c, sig.v, sig.tb, sig.t})) ++a.any_prev;
  {
    const std::string twin_key = std::string(info.hs) + "/" + info.ds;
    auto t = s.twin_first.find(twin_key);
    if (t == s.twin_first.end()) s.twin_first.emplace(twin_key, std::make_pair(info.id, sig));
    else if (t->second.first != info.id) {
      const auto &f = t->second.second;
      ++a.twin_compared;
      const bool c = f.cp == sig.cp, v = f.v == sig.v, tb = f.tbp == sig.tbp;   // pre-raster-visible root parameters only
      a.twin_c += c; a.twin_v += v; a.twin_tb += tb; a.twin_all += c && v && tb;
    }
  }
  auto it = s.prev.find(key);
  if (it == s.prev.end()) return;
  ++a.compared;
  const auto &p = it->second;
  const bool c = p.c == sig.c, v = p.v == sig.v, tb = p.tb == sig.tb, t = p.t == sig.t && !p.tex_unknown && !sig.tex_unknown;
  a.same_c += c; a.same_v += v; a.same_tb += tb; a.same_t += t; a.same_all += c && v && tb && t;
}

inline const char *PartitionName(uint32_t p) {
  switch (p) { case 1: return "integer"; case 2: return "pow2"; case 3: return "frac_odd"; case 4: return "frac_even"; default: return "?"; }
}
inline const char *DomainName(uint32_t d) {
  switch (d) { case 1: return "isoline"; case 2: return "tri"; case 3: return "quad"; default: return "?"; }
}

inline void
Copy(const char *kind, uint32_t src_heap, uint32_t dst_heap, uint64_t bytes, const void *dst, uint64_t dst_off,
     const void *src_cpu, uint64_t src_bytes) {
  auto &s = Get(); std::lock_guard<std::mutex> g(s.m);
  char key[64];
  std::snprintf(key, sizeof(key), "%s_%u_%u", kind, src_heap, dst_heap);
  auto &a = s.copies[key];
  ++a.n; a.bytes += bytes;
  if (dst && dst_heap == 1) {   // DEFAULT heap only: a READBACK/UPLOAD destination is read by the CPU without a transition
    if (s.pending.count(dst)) ++a.unread_overwrite;
    if (s.pending.size() > 200000) s.pending.clear();
    s.pending.insert(dst);
  }
  if (src_cpu && src_bytes) {
    const uint64_t head = src_bytes < 32768 ? src_bytes : 32768;
    uint64_t h = Hash(0, src_cpu, size_t(head));
    if (src_bytes > 65536) h = Hash(h, static_cast<const char *>(src_cpu) + src_bytes - 32768, 32768);
    else if (src_bytes > head) h = Hash(h, static_cast<const char *>(src_cpu) + head, size_t(src_bytes - head));
    h = Word(h, src_bytes);
    ++a.hashed;
    if (s.last_content.size() > 200000) s.last_content.clear();
    auto &last = s.last_content[{dst, dst_off, bytes}];
    if (last == h) ++a.repeat;
    last = h;
  }
}
inline void
Transition(const void *resource, uint32_t before, uint32_t after) {
  constexpr uint32_t kCopyDest = 0x400;   // D3D12_RESOURCE_STATE_COPY_DEST
  if (!(before & kCopyDest) || (after & kCopyDest)) return;
  auto &s = Get(); std::lock_guard<std::mutex> g(s.m);
  s.pending.erase(resource);
}

inline void
Presented() {
  const unsigned n = Period();
  if (!n) return;
  auto &s = Get(); std::lock_guard<std::mutex> g(s.m);
  ++s.presents; ++s.frames;
  s.prev = std::move(s.cur); s.cur.clear(); s.occ.clear(); s.twin_first.clear();
  s.prev_all = std::move(s.cur_all); s.cur_all.clear();
  if (s.presents % n) return;
  uint64_t td = 0, tp = 0, ti = 0;
  for (const auto &[id, a] : s.agg) {
    std::fprintf(stderr,
                 "dx12_tess_census presents=%llu window=%u pso=%u hs=%s ds=%s part=%s dom=%s maxtf=%.1f incp=%u outcp=%u draws=%llu "
                 "patches=%llu patches_max=%llu instances=%llu indirect=%llu indirect_max=%llu compared=%llu same_const=%llu "
                 "same_vbib=%llu same_tables=%llu same_tex=%llu same_all=%llu any_prev=%llu tex_unknown=%llu twin_compared=%llu "
                 "twin_const=%llu twin_vbib=%llu twin_tables=%llu twin_all=%llu\n",
                 (unsigned long long)s.presents, n, id, a.info.hs[0] ? a.info.hs : "-", a.info.ds[0] ? a.info.ds : "-",
                 PartitionName(a.info.partition), DomainName(a.info.domain), a.info.max_factor, a.info.in_cp, a.info.out_cp,
                 (unsigned long long)a.draws, (unsigned long long)a.patches, (unsigned long long)a.patches_max,
                 (unsigned long long)a.instances, (unsigned long long)a.indirect, (unsigned long long)a.indirect_max,
                 (unsigned long long)a.compared, (unsigned long long)a.same_c, (unsigned long long)a.same_v,
                 (unsigned long long)a.same_tb, (unsigned long long)a.same_t, (unsigned long long)a.same_all,
                 (unsigned long long)a.any_prev, (unsigned long long)a.tex_unknown, (unsigned long long)a.twin_compared,
                 (unsigned long long)a.twin_c, (unsigned long long)a.twin_v, (unsigned long long)a.twin_tb, (unsigned long long)a.twin_all);
    td += a.draws; tp += a.patches; ti += a.instances;
  }
  std::fprintf(stderr, "dx12_tess_census_total presents=%llu window=%u pipelines=%u draws=%llu patches=%llu instances=%llu\n",
               (unsigned long long)s.presents, n, unsigned(s.agg.size()), (unsigned long long)td, (unsigned long long)tp,
               (unsigned long long)ti);
  for (const auto &[k, a] : s.copies)
    std::fprintf(stderr, "dx12_copy_census presents=%llu window=%u kind_heaps=%s n=%llu bytes=%llu hashed=%llu repeat=%llu unread_overwrite=%llu\n",
                 (unsigned long long)s.presents, n, k.c_str(), (unsigned long long)a.n, (unsigned long long)a.bytes,
                 (unsigned long long)a.hashed, (unsigned long long)a.repeat, (unsigned long long)a.unread_overwrite);
  s.agg.clear(); s.copies.clear();
}

} // namespace dxmt::tess_census
