// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

// Two opt-in keys of the shader-source seam (d3d12_shader_source.hpp), both read
// once per process and both off when unset:
//
//   MACRUNNER_DX12_OWN_FALLBACK=1
//       A pipeline the provider was in charge of and could not build (missing or
//       unusable translation, another root signature, target type mismatch,
//       Metal refusing a library, a function or the pipeline) is rebuilt by the
//       legacy path instead of failing. Every pipeline that ends on the legacy
//       path while a provider is selected leaves ONE stderr line:
//         dx12_shader_source_fallback pipeline=<graphics|compute> owner=<p>
//             cause=<declined|failed> reason=<name> own_hr=<hex> legacy_hr=<hex>
//       declined: the provider never took the pipeline (no translation, stage
//       not served, SM5.0 container); failed: it took it and failed.
//       Unset: a provider failure fails the pipeline, exactly as before.
//
//   MACRUNNER_DX12_OWN_STATS=<seconds, 1..3600>
//       Cumulative counters on stderr, at most once per <seconds> (checked at
//       every pipeline creation and every Present; no thread of its own), when
//       the device is destroyed and when the module unloads:
//         dx12_shader_source_stats seq=<n> final=<0|1> uptime_ms=<n> fallback=<0|1>
//             graphics=<own>/<declined>/<fallback>/<failed>/<legacy_failed>
//             compute=<own>/<declined>/<fallback>/<failed>/<legacy_failed>
//             requests=<n> [source_libs=<built>/<found in process>/<ms spent building>] reasons=<name>:<graphics>/<compute>,...
//       own            built from provider libraries
//       declined       provider did not take it, legacy path built it
//       fallback       provider failed, legacy path built it (fallback key)
//       failed         provider failed, pipeline creation failed (no fallback key)
//       legacy_failed  the legacy path was taken and failed as well
//       requests       distinct translation requests left for the offline translator
//       reasons        every pipeline not counted as `own`, by reason

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dxmt::shader_source {

inline bool
FallbackEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool on = GetEnvironmentVariableA("MACRUNNER_DX12_OWN_FALLBACK", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return on;
  }();
  return enabled;
}

namespace stats {

enum Kind : unsigned { Graphics, Compute, KindCount };
enum Outcome : unsigned { Own, Declined, Fallback, Failed, LegacyFailed, OutcomeCount };

// Reasons a pipeline is not built from provider libraries. Fixed table: no allocation on the creation path.
inline constexpr const char *kReasons[] = {
    "stage_not_served_tessellation", // hull or domain stage: legacy path only
    "stage_not_served_geometry",     // geometry stage: legacy path only
    "stage_not_served_no_vs",
    "not_dxil",                 // SM5.0 container: nothing to translate, the legacy translator takes it
    "no_translation",           // no library for this shader under this root signature
    "no_translation_variant",   // the base pixel function may exist, the variant (sm, ds, ds.sm) does not
    "translation_not_admitted", // description the seam does not admit
    "unusable_translation",     // library or description broken, or translated as another variant
    "root_signature_mismatch",  // translated for another root argument layout
    "target_type_mismatch",     // target format class differs from the declared output type
    "vertex_inputs",            // the input layout does not feed an input the function pulls
    "invalid_state",            // pipeline description the seam refuses (bounds, formats)
    "vs_library", "vs_entry", "ps_library", "ps_entry", "render_pipeline",
    "thread_group_size", "cs_library", "cs_entry", "compute_pipeline",
    "pipeline_state",           // failed after the stage functions were made
    "other",
};
constexpr unsigned kReasonCount = sizeof(kReasons) / sizeof(kReasons[0]);

struct Data {
  std::atomic<uint64_t> outcomes[KindCount][OutcomeCount] = {};
  std::atomic<uint64_t> reasons[kReasonCount][KindCount] = {};
  std::atomic<uint64_t> requests{0};
  std::atomic<uint64_t> source_built{0}, source_hit{0}, source_us{0}; // MACRUNNER_DX12_OWN_SHADER_KIND=source libraries
  std::atomic<uint64_t> changes{0}, printed_changes{0};
  std::atomic<uint64_t> last_print_ms{0};
  std::atomic<uint32_t> seq{0};
  uint64_t start_ms = GetTickCount64();
};

inline Data &
Get() {
  static Data data;
  return data;
}

// Seconds between lines; 0 = no counters line at all.
inline uint32_t
Period() {
  static const uint32_t period = [] {
    const DWORD error = GetLastError();
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA("MACRUNNER_DX12_OWN_STATS", value, sizeof(value));
    SetLastError(error);
    uint32_t parsed = 0;
    if (!length || length >= sizeof(value)) return 0u;
    for (DWORD i = 0; i < length; i++) {
      if (value[i] < '0' || value[i] > '9') return 0u;
      parsed = parsed * 10 + uint32_t(value[i] - '0');
    }
    return parsed <= 3600 ? parsed : 0u;
  }();
  return period;
}

inline unsigned
ReasonIndex(const char *reason) {
  if (reason)
    for (unsigned i = 0; i < kReasonCount; i++)
      if (!std::strcmp(reason, kReasons[i])) return i;
  return kReasonCount - 1;
}

inline void
Count(Kind kind, Outcome outcome, const char *reason = nullptr) {
  auto &data = Get();
  data.outcomes[kind][outcome].fetch_add(1, std::memory_order_relaxed);
  if (outcome != Own) data.reasons[ReasonIndex(reason)][kind].fetch_add(1, std::memory_order_relaxed);
  data.changes.fetch_add(1, std::memory_order_relaxed);
}

inline void
CountRequest() {
  auto &data = Get();
  data.requests.fetch_add(1, std::memory_order_relaxed);
  data.changes.fetch_add(1, std::memory_order_relaxed);
}

// A library of kind "source": built from text (`built`, with the microseconds the system took) or found in the process.
inline void
CountSourceLibrary(bool built, uint64_t microseconds) {
  auto &data = Get();
  if (built) {
    data.source_built.fetch_add(1, std::memory_order_relaxed);
    data.source_us.fetch_add(microseconds, std::memory_order_relaxed);
  } else {
    data.source_hit.fetch_add(1, std::memory_order_relaxed);
  }
  data.changes.fetch_add(1, std::memory_order_relaxed);
}

inline void
Print(bool final) {
  auto &data = Get();
  const DWORD error = GetLastError();
  char line[2048];
  const auto get = [](const std::atomic<uint64_t> &value) { return (unsigned long long)value.load(std::memory_order_relaxed); };
  const uint64_t changes = data.changes.load(std::memory_order_relaxed);
  char source_libs[96] = "";
  if (get(data.source_built) + get(data.source_hit))
    std::snprintf(source_libs, sizeof(source_libs), " source_libs=%llu/%llu/%llums", get(data.source_built),
                  get(data.source_hit), get(data.source_us) / 1000);
  int length = std::snprintf(
      line, sizeof(line),
      "dx12_shader_source_stats seq=%u final=%u uptime_ms=%llu fallback=%u graphics=%llu/%llu/%llu/%llu/%llu "
      "compute=%llu/%llu/%llu/%llu/%llu requests=%llu%s reasons=",
      data.seq.fetch_add(1, std::memory_order_relaxed) + 1, unsigned(final),
      (unsigned long long)(GetTickCount64() - data.start_ms), unsigned(FallbackEnabled()),
      get(data.outcomes[Graphics][Own]), get(data.outcomes[Graphics][Declined]), get(data.outcomes[Graphics][Fallback]),
      get(data.outcomes[Graphics][Failed]), get(data.outcomes[Graphics][LegacyFailed]),
      get(data.outcomes[Compute][Own]), get(data.outcomes[Compute][Declined]), get(data.outcomes[Compute][Fallback]),
      get(data.outcomes[Compute][Failed]), get(data.outcomes[Compute][LegacyFailed]), get(data.requests), source_libs);
  bool any = false;
  for (unsigned i = 0; i < kReasonCount && length > 0 && size_t(length) < sizeof(line) - 96; i++) {
    const unsigned long long graphics = get(data.reasons[i][Graphics]), compute = get(data.reasons[i][Compute]);
    if (!graphics && !compute) continue;
    length += std::snprintf(line + length, sizeof(line) - size_t(length), "%s%s:%llu/%llu", any ? "," : "", kReasons[i],
                            graphics, compute);
    any = true;
  }
  if (length > 0 && size_t(length) < sizeof(line) - 8) {
    length += std::snprintf(line + length, sizeof(line) - size_t(length), "%s\n", any ? "" : "none");
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
  }
  data.printed_changes.store(changes, std::memory_order_relaxed);
  SetLastError(error);
}

// Prints the counters when they changed and the period has passed. Called at every
// pipeline creation and every Present: two relaxed loads when there is nothing to do.
inline void
Tick() {
  const uint32_t period = Period();
  if (!period) return;
  auto &data = Get();
  if (data.changes.load(std::memory_order_relaxed) == data.printed_changes.load(std::memory_order_relaxed)) return;
  const uint64_t now = GetTickCount64();
  uint64_t last = data.last_print_ms.load(std::memory_order_relaxed);
  if (last && now - last < uint64_t(period) * 1000) return;
  // One thread prints; a racing one sees the new time and leaves.
  if (!data.last_print_ms.compare_exchange_strong(last, now ? now : 1, std::memory_order_relaxed)) return;
  Print(false);
}

// The last line: device destruction and module unload (a killed process prints neither —
// the periodic line is what remains then).
inline void
Final() {
  if (!Period()) return;
  auto &data = Get();
  static std::atomic<uint64_t> final_changes{~0ull};
  const uint64_t changes = data.changes.load(std::memory_order_relaxed);
  if (final_changes.exchange(changes, std::memory_order_relaxed) == changes) return; // nothing new since the last final line
  Print(true);
}

struct AtUnload {
  ~AtUnload() { Final(); }
};

// Arms the module-unload line; called once a provider is selected.
inline void
Arm() {
  static AtUnload at_unload;
  (void)at_unload;
}

} // namespace stats

// The one line of a pipeline that ended on the legacy path while a provider is selected.
inline void
FallbackLine(const char *pipeline, const void *owner, const char *cause, const char *reason, HRESULT own_hr,
             HRESULT legacy_hr) {
  if (!FallbackEnabled()) return;
  const DWORD error = GetLastError();
  char line[320];
  const int length = std::snprintf(line, sizeof(line),
                                   "dx12_shader_source_fallback pipeline=%s owner=%p cause=%s reason=%s own_hr=%08x "
                                   "legacy_hr=%08x\n",
                                   pipeline, owner, cause, reason ? reason : "other", unsigned(own_hr), unsigned(legacy_hr));
  DWORD written;
  if (length > 0 && size_t(length) < sizeof(line))
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr);
  SetLastError(error);
}

} // namespace dxmt::shader_source
