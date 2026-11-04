// SPDX-License-Identifier: LGPL-2.1-or-later
// MacRunner observation-only reuse audit. No decision here controls GPU execution.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace dxmt::reuse {
constexpr uint64_t ByteBudget = 8 * 1024 * 1024;
constexpr size_t MaxPasses = 8192, MaxDraws = 65536, MaxPSOs = 512, MaxRegisters = 32768;
constexpr size_t MaxValues = 8, MaxBindings = 1024;
// Named-shader constant watch (MACRUNNER_DX12_REUSE_AUDIT_CB). Watched PSOs are
// admitted beyond MaxPSOs/MaxRegisters, but only within these separate bounds.
constexpr size_t MaxWatchedPSOs = 64, MaxWatchedRegisters = 8192;
constexpr uint32_t SlotRootDescriptor = 0xfffffffeu, SlotRootConstants = 0xfffffffdu, SlotUnknown = 0xffffffffu;
enum Part : unsigned { Pipeline, Root, Constants, Descriptors, Resources, Upload, Arguments, Raster, Targets, Effects, History, Parts };
inline constexpr const char *PartNames[] = {
  "pso", "root_signature", "root_constants", "descriptors", "resource_version", "upload_bytes",
  "draw_dispatch_indirect", "viewport_scissor_raster", "targets", "side_effects", "target_history", "incomplete", "first_frame", "pass_count"
};
enum Target : unsigned { Shadow, GBuffer, Other, Compute, TargetClasses };
inline constexpr const char *TargetNames[] = {"shadow_d32_4096x2048", "gbuffer", "other", "compute"};

// Two independent 64-bit fingerprints. This is a collision-resistant measurement,
// never an execution/skip proof. No padding or process pointer is hashed implicitly.
struct Digest {
  uint64_t a = 0xcbf29ce484222325ULL, b = 0x84222325cbf29ce4ULL;
  bool operator==(const Digest &) const = default;
  void Bytes(const void *data, size_t size) {
    auto p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
      a = (a ^ p[i]) * 0x100000001b3ULL;
      b ^= uint64_t(p[i]) + 0x9e3779b97f4a7c15ULL + (b << 6) + (b >> 2);
    }
  }
  void Word(uint64_t v) { Bytes(&v, sizeof(v)); }
  void Join(const Digest &v) { Word(v.a); Word(v.b); }
};

struct Signature {
  std::array<Digest, Parts> part;
  uint64_t unknown = 0;
  void Word(Part p, uint64_t v) { part[p].Word(v); }
  void Bytes(Part p, const void *v, size_t n) { part[p].Word(n); part[p].Bytes(v, n); }
  void Join(const Signature &s) {
    for (unsigned i = 0; i < Parts; ++i) part[i].Join(s.part[i]);
    unknown |= s.unknown;
  }
  unsigned Difference(const Signature &other) const {
    if (unknown || other.unknown) return Parts;
    for (unsigned i = 0; i < Parts; ++i) if (!(part[i] == other.part[i])) return i;
    return Parts + 3;
  }
};
enum Unknown : uint64_t {
  MissingResource = 1, ByteLimit = 2, BindingLimit = 4, MissingBinding = 8,
  UnsupportedCommand = 16, IndirectMutation = 32, MultipleQueues = 64,
  InvalidRange = 128, PassLimit = 256, ShaderMetadata = 512, GPUConstants = 1024,
  OutputHistory = 2048, Aliasing = 4096, DrawLimit = 8192
};

struct Budget {
  uint64_t used = 0, refused_bytes = 0, refused_ranges = 0;
  bool Take(uint64_t bytes) {
    if (bytes > ByteBudget - used) { refused_bytes += bytes; ++refused_ranges; return false; }
    used += bytes; return true;
  }
};

struct RegisterKey {
  uint32_t stage = 0, space = 0, binding = 0, index = 0;
  auto operator<=>(const RegisterKey &) const = default;
};
struct Register {
  std::array<std::array<uint8_t, 16>, MaxValues> values{};
  std::array<uint64_t, MaxValues> hits{};     // draws that carried values[i]
  uint64_t observations = 0, last_draw = 0, overflow_hits = 0;
  uint32_t param = SlotUnknown, slot = SlotUnknown; // root parameter / table offset of first observation
  unsigned distinct = 0;
  bool overflow = false, branch = false, loop = false, loaded = false, multiple_sources = false;
  void Observe(const void *data, uint64_t draw, bool is_branch, bool is_loop, bool is_loaded,
               uint32_t source_param = SlotUnknown, uint32_t source_slot = SlotUnknown) {
    // One value for one binding in a draw. Duplicate stage reflection cannot inflate coverage.
    if (last_draw == draw) return;
    last_draw = draw; ++observations;
    branch |= is_branch; loop |= is_loop; loaded |= is_loaded;
    if (param == SlotUnknown) { param = source_param; slot = source_slot; }
    else if (param != source_param || slot != source_slot) multiple_sources = true;
    for (unsigned i = 0; i < distinct; ++i) if (!std::memcmp(values[i].data(), data, 16)) { ++hits[i]; return; }
    if (distinct == MaxValues) { overflow = true; ++overflow_hits; return; }
    hits[distinct] = 1;
    std::memcpy(values[distinct++].data(), data, 16);
  }
};
struct PSO {
  uint64_t draws = 0, missed_registers = 0, metadata_unknown = 0;
  bool watched = false;
  std::vector<std::pair<uint32_t, std::string>> shaders; // stage slot -> bytecode SHA-1 (watched only)
  std::map<RegisterKey, uint64_t> gpu_constants;         // CBV binding (index 0) -> draws whose bytes are GPU-resident
  std::map<RegisterKey, uint64_t> unobserved_constants;  // CBV binding -> draws refused by budget/range
  std::map<RegisterKey, Register> registers;
};
struct Draw {
  Signature signature;
  uint64_t pso = 0;
};
struct Pass {
  struct Output { uint64_t id, before, after; };
  Signature signature;
  std::vector<Draw> draws;
  std::vector<Output> outputs;
  Target target = Other;
  uint64_t actual_draws = 0;
};
// Only encoder identities: immutable after Close, reusable at each Execute.
// Inputs themselves are always captured afresh on the public Execute caller.
struct EncoderPlan {
  std::vector<std::vector<const void *>> groups;
  bool incomplete = false;
};
struct Counts {
  uint64_t passes = 0, repeated_passes = 0, draws = 0, repeated_draws = 0, incomplete = 0;
  uint64_t compared = 0, retained_outputs = 0, feedback_draws = 0, feedback_passes = 0;
  std::array<uint64_t, Parts + 3> first_reason{};
};

struct Window {
  uint64_t presents = 0, total_presents = 0, draw_count = 0, register_count = 0;
  uint64_t watched_psos = 0, watched_registers = 0, watched_skipped = 0;
  uint64_t skipped_passes = 0, skipped_psos = 0, bytes = 0, refused_bytes = 0, refused_ranges = 0;
  uint64_t observed_ticks = 0;
  Budget budget;
  std::array<Counts, TargetClasses> counts;
  std::vector<Pass> previous, current;
  std::map<uint64_t, PSO> psos;

  PSO *ObservePSO(uint64_t id, bool watched = false) {
    auto it = psos.find(id);
    if (it == psos.end()) {
      // Watched PSOs have their own bound, so a busy window cannot hide the named shaders.
      if (watched) {
        if (watched_psos == MaxWatchedPSOs) { ++watched_skipped; return nullptr; }
        ++watched_psos;
      } else if (psos.size() - watched_psos >= MaxPSOs) { ++skipped_psos; return nullptr; }
      it = psos.emplace(id, PSO{}).first; it->second.watched = watched;
    }
    ++it->second.draws;
    return &it->second;
  }
  void ObserveRegister(PSO *pso, const RegisterKey &key, const void *bytes,
                       bool branch = false, bool loop = false, bool loaded = false,
                       uint32_t param = SlotUnknown, uint32_t slot = SlotUnknown) {
    if (!pso) return;
    auto it = pso->registers.find(key);
    if (it == pso->registers.end()) {
      if (pso->watched) {
        if (watched_registers == MaxWatchedRegisters) { ++pso->missed_registers; return; }
        ++watched_registers;
      } else {
        if (register_count == MaxRegisters) { ++pso->missed_registers; return; }
        ++register_count;
      }
      it = pso->registers.emplace(key, Register{}).first;
    }
    it->second.Observe(bytes, pso->draws, branch, loop, loaded, param, slot);
  }
  void Add(Pass pass) {
    auto &c = counts[pass.target]; ++c.passes; c.draws += pass.actual_draws;
    if (current.size() >= MaxPasses) { ++skipped_passes; ++c.incomplete; ++c.first_reason[Parts]; return; }
    const size_t position = current.size();
    for (const auto &draw : pass.draws) pass.signature.Join(draw.signature);
    pass.signature.Word(Effects, pass.actual_draws);
    unsigned reason = Parts + 1;
    if (position < previous.size()) {
      const auto &old = previous[position];
      ++c.compared;
      reason = pass.signature.Difference(old.signature);
      if (pass.target != old.target) reason = Targets;
      if (reason == Parts + 3) {
        ++c.repeated_passes;
        bool retained = !pass.outputs.empty() && pass.outputs.size() == old.outputs.size();
        for (size_t i = 0; retained && i < pass.outputs.size(); ++i)
          retained = pass.outputs[i].id == old.outputs[i].id && pass.outputs[i].before == old.outputs[i].after;
        if (retained) ++c.retained_outputs;
      }
      for (size_t i = 0; i < pass.draws.size() && i < old.draws.size(); ++i)
        if (pass.target == old.target && pass.signature.part[Targets] == old.signature.part[Targets] &&
            !pass.signature.unknown && !old.signature.unknown &&
            pass.draws[i].signature.Difference(old.draws[i].signature) == Parts + 3) ++c.repeated_draws;
    } else if (!previous.empty()) reason = Parts + 2;
    if (pass.signature.unknown) { reason = Parts; ++c.incomplete; }
    if (reason < c.first_reason.size()) ++c.first_reason[reason];
    current.push_back(std::move(pass));
  }
  void Present() {
    ++presents; ++total_presents;
    previous = std::move(current); current.clear();
    bytes += budget.used; refused_bytes += budget.refused_bytes; refused_ranges += budget.refused_ranges;
    budget = {}; draw_count = 0;
  }
  void ResetWindow() {
    presents = register_count = skipped_passes = skipped_psos = bytes = refused_bytes = refused_ranges = observed_ticks = 0;
    watched_psos = watched_registers = watched_skipped = 0;
    counts = {}; psos.clear();
    // Keep the previous frame for the next window. A window never loses its comparison baseline.
  }
};
} // namespace dxmt::reuse
