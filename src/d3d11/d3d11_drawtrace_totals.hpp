#pragma once

#include <cstdint>

namespace dxmt {

enum class DXMTHKDrawTraceCounter : uint8_t {
  Draw,
  DrawIndexed,
  DrawInstanced,
  DrawIndexedInstanced,
  ClearRenderTargetView,
  OMSetRenderTargets,
  Present,
  Present1,
  Count,
};

bool dxmt_hk_drawtrace_enabled();
void dxmt_hk_drawtrace_record(DXMTHKDrawTraceCounter counter);
void dxmt_hk_drawtrace_emit_totals(const char *reason);

} // namespace dxmt
