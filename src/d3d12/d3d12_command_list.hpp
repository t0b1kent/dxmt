// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dxmt {

class MTLD3D12GraphicsCommandListImpl;

// Value-only snapshot owned by the list; valid until its next recording initialization.
class D3D12RecordingTrace {
  friend class MTLD3D12GraphicsCommandListImpl;
  std::vector<std::string> entries_;
  uint64_t epoch_ = 0;
  uint64_t dropped_ = 0;

public:
  const std::vector<std::string> &GetRecordingTrace() const { return entries_; }
  uint64_t GetRecordingTraceEpoch() const { return epoch_; }
  uint64_t GetRecordingTraceDropped() const { return dropped_; }
};

} // namespace dxmt
