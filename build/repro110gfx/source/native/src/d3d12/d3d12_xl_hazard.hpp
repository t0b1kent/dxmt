// SPDX-License-Identifier: LGPL-2.1-or-later
// XL-GAPS (23.10.2026): subresource-level hazard observer for MERGE_REDUNDANT_TRANSITIONS=2. Observation only:
// nothing is merged, skipped or reordered by this file. Default off.
//
//   MACRUNNER_DX12_XL_HAZARD=1          record, per render pass, the descriptor spans each draw can read (root table
//                                       ranges intersected with the SRV/UAV registers its pipeline declares: DXIL PSV0,
//                                       DXBC RDEF; unknown declaration = the whole table range), and at every
//                                       ExecuteCommandLists judge every pair of passes that mode 2 would consider (same
//                                       targets, only barrier passes between, inside one list or across lists) by the
//                                       mode-2 rules; for both passes of each pair report whether any descriptor a draw can
//                                       read is the SAME SUBRESOURCE (texture, mip, array slice, plane) as a written
//                                       attachment of the pair, or only another mip / slice / plane / a read-only plane.
//   MACRUNNER_DX12_XL_HAZARD_EVERY=K    judge only every K-th frame (default 1 = every frame); recording stays on.
//   MACRUNNER_DX12_XL_HAZARD_REPORT=P   cumulative report lines every P presents (default 600).
//   MACRUNNER_DX12_XL_HAZARD_DETAIL=F   per-hit rows for frame F (default none), at most 1024 rows.
// Lines (stderr, WriteFile): dx12_xl_hazard / dx12_xl_hazard_class / dx12_xl_hazard_pair / dx12_xl_hazard_hit.
#pragma once
#include "d3d12_c17_stats.hpp"
#include "d3d12.h"
#include "com/com_pointer.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace dxmt {
class Texture;
}

namespace dxmt::xl_hazard {

inline bool Enabled() { static const bool v = c17::ReadKey("MACRUNNER_DX12_XL_HAZARD", 1) == 1; return v; }
inline unsigned Every() { static const unsigned v = std::max(1u, c17::ReadKey("MACRUNNER_DX12_XL_HAZARD_EVERY", 100000)); return v; }
inline unsigned ReportPeriod() {
  static const unsigned v = [] { const unsigned r = c17::ReadKey("MACRUNNER_DX12_XL_HAZARD_REPORT", 1000000); return r ? r : 600u; }();
  return v;
}
inline unsigned DetailFrame() {
  static const unsigned v = [] {
    char text[16]{};
    const DWORD error = GetLastError();
    const DWORD n = GetEnvironmentVariableA("MACRUNNER_DX12_XL_HAZARD_DETAIL", text, sizeof(text));
    SetLastError(error);
    return n ? c17::ReadKey("MACRUNNER_DX12_XL_HAZARD_DETAIL", 100000000) : 0xffffffffu;
  }();
  return v;
}

// SRV/UAV registers one pipeline declares. stage_visibility: D3D12_SHADER_VISIBILITY of the declaring stage.
struct Declared {
  uint8_t visibility = 0, uav = 0;
  uint32_t space = 0, lower = 0, upper = 0;   // inclusive; upper = UINT32_MAX for an unbounded array
};
// Graphics pipelines whose declarations were read (counted at creation, printed in the dx12_xl_hazard head line).
inline std::atomic<uint64_t> g_pso_dxil{0}, g_pso_dxbc{0}, g_pso_unknown{0};
struct PipelineDeclared {
  std::vector<Declared> ranges;
  bool known = true;          // false: some stage's declaration could not be read (whole table ranges are used)
  uint8_t sources = 0;        // bit0 DXIL PSV0, bit1 DXBC RDEF, bit2 unreadable
};

// One SRV/UAV range of a bound descriptor table (whole range, clamped to the heap), in the CBV/SRV/UAV heap bound at
// that draw, and the parts of it the pipeline declares (decls[first_decl, first_decl + decl_count): absolute heap
// indices). declared = 0: the pipeline's declarations could not be read, the whole range counts as readable.
struct Span {
  const void *heap = nullptr;   // MTLD3D12DescriptorHeap (kept alive by PassBindings::heaps)
  uint32_t start = 0, count = 0, first_decl = 0, decl_count = 0;
  uint8_t uav = 0, declared = 1;
};
struct Interval { uint32_t start = 0, count = 0; };
struct Snapshot {
  uint32_t draws = 0, first_span = 0, span_count = 0;
  uint8_t flags = 0;   // 1 a declared register range whose table is not readable (unset table / no heap / no root
                       //   parameters), 2 indirect root mutation; draws with flags are not proven either way
};
struct PassBindings {
  std::vector<Snapshot> snapshots;
  std::vector<Span> spans;
  std::vector<Interval> decls;
  std::vector<Com<ID3D12DescriptorHeap>> heaps;   // keeps every span's heap alive until the pass is released
  uint64_t last_epoch = UINT64_MAX;
  const void *last_pso = nullptr, *last_root = nullptr;
  uint32_t draws = 0;
};

} // namespace dxmt::xl_hazard
