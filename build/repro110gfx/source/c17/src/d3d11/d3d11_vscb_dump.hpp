#pragma once

/*
 * MACRUNNER_DXMT_VS_CB_DUMP — Hollow Knight degenerate-transform probe.
 *
 * Question owned by lane HK-TRANSFORM: are Hollow Knight's draws fed a
 * degenerate view/projection matrix (geometry off-screen or zero-area)?
 *
 * This header is deliberately self-contained (no Windows/DXMT headers) so the
 * matrix classifier can be compiled and self-tested on the host:
 *   tools/hk_vscb_classifier_selftest.cpp
 *
 * The runtime side lives in d3d11.cpp (single TU: counters + totals) and
 * d3d11_context_impl.cpp (draw-time snapshot). Default OFF; enable with
 * MACRUNNER_DXMT_VS_CB_DUMP=1, detail-line cap MACRUNNER_DXMT_VS_CB_DUMP_MAX.
 */

#include <cmath>
#include <cstdint>

namespace dxmt {

enum DXMTHKVSCBClass : uint32_t {
  DXMT_HK_VSCB_CLASS_IDENTITY = 0,
  DXMT_HK_VSCB_CLASS_ZERO = 1,
  DXMT_HK_VSCB_CLASS_NAN_OR_INF = 2,
  DXMT_HK_VSCB_CLASS_DEGENERATE = 3,
  DXMT_HK_VSCB_CLASS_PLAUSIBLE = 4,
  DXMT_HK_VSCB_CLASS_COUNT = 5,
};

inline const char *
dxmt_hk_vscb_class_name(uint32_t cls) {
  switch (cls) {
  case DXMT_HK_VSCB_CLASS_IDENTITY:
    return "IDENTITY";
  case DXMT_HK_VSCB_CLASS_ZERO:
    return "ZERO";
  case DXMT_HK_VSCB_CLASS_NAN_OR_INF:
    return "NAN_OR_INF";
  case DXMT_HK_VSCB_CLASS_DEGENERATE:
    return "DEGENERATE";
  case DXMT_HK_VSCB_CLASS_PLAUSIBLE:
    return "PLAUSIBLE";
  default:
    return "UNKNOWN";
  }
}

/* 4x4 determinant in double precision (row-major m[16]). */
inline double
dxmt_hk_vscb_det4(const float *m) {
  const double a0 = m[0], a1 = m[1], a2 = m[2], a3 = m[3];
  const double b0 = m[4], b1 = m[5], b2 = m[6], b3 = m[7];
  const double c0 = m[8], c1 = m[9], c2 = m[10], c3 = m[11];
  const double d0 = m[12], d1 = m[13], d2 = m[14], d3 = m[15];
  /* expansion by first row, 3x3 minors */
  const double m01 = b1 * (c2 * d3 - c3 * d2) - b2 * (c1 * d3 - c3 * d1) + b3 * (c1 * d2 - c2 * d1);
  const double m02 = b0 * (c2 * d3 - c3 * d2) - b2 * (c0 * d3 - c3 * d0) + b3 * (c0 * d2 - c2 * d0);
  const double m03 = b0 * (c1 * d3 - c3 * d1) - b1 * (c0 * d3 - c3 * d0) + b3 * (c0 * d1 - c1 * d0);
  const double m04 = b0 * (c1 * d2 - c2 * d1) - b1 * (c0 * d2 - c2 * d0) + b2 * (c0 * d1 - c1 * d0);
  return a0 * m01 - a1 * m02 + a2 * m03 - a3 * m04;
}

/*
 * Classify one 16-float 4x4 block.
 *
 * DEGENERATE is scale-invariant: |det| <= eps * prod(row norms). An absolute
 * epsilon would misclassify legitimate orthographic UI matrices (det ~ 1e-8
 * for 1024x768) as degenerate; the row-norm-normalized test does not.
 */
inline uint32_t
dxmt_hk_vscb_classify_matrix(const float *m) {
  static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  bool any_nonfinite = false;
  bool all_zero = true;
  bool is_identity = true;
  for (unsigned i = 0; i < 16; i++) {
    if (!std::isfinite(m[i])) {
      any_nonfinite = true;
    } else {
      if (m[i] != 0.0f)
        all_zero = false;
      if (std::fabs(m[i] - identity[i]) > 1e-6f)
        is_identity = false;
    }
  }
  if (any_nonfinite)
    return DXMT_HK_VSCB_CLASS_NAN_OR_INF;
  if (all_zero)
    return DXMT_HK_VSCB_CLASS_ZERO;
  if (is_identity)
    return DXMT_HK_VSCB_CLASS_IDENTITY;

  double row_norm[4];
  for (unsigned r = 0; r < 4; r++) {
    double acc = 0.0;
    for (unsigned c = 0; c < 4; c++) {
      const double v = m[r * 4 + c];
      acc += v * v;
    }
    row_norm[r] = std::sqrt(acc);
    if (row_norm[r] == 0.0)
      return DXMT_HK_VSCB_CLASS_DEGENERATE; /* a zero row kills rank */
  }
  const double det = dxmt_hk_vscb_det4(m);
  const double scale = row_norm[0] * row_norm[1] * row_norm[2] * row_norm[3];
  if (std::fabs(det) <= 1e-6 * scale)
    return DXMT_HK_VSCB_CLASS_DEGENERATE;
  return DXMT_HK_VSCB_CLASS_PLAUSIBLE;
}

/* runtime side (implemented in d3d11.cpp) */

bool dxmt_hk_vscb_dump_enabled();
unsigned long long dxmt_hk_vscb_dump_next_draw_ordinal();
bool dxmt_hk_vscb_dump_take_detail_slot();
void dxmt_hk_vscb_dump_record_buffer(unsigned n_matrices, const uint32_t *classes, bool readable);
void dxmt_hk_vscb_dump_emit_totals(const char *reason);

} // namespace dxmt
