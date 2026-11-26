// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

namespace dxmt::binding_elision {
inline bool ReadFlag(const char *name) {
  const DWORD saved = GetLastError();
  char value[4] = {};
  const bool enabled = GetEnvironmentVariableA(name, value, sizeof(value)) == 1 && value[0] == '1';
  SetLastError(saved);
  return enabled;
}
inline bool ReuseRootOnPSO() {
  static const bool enabled = ReadFlag("MACRUNNER_DX12_ROOT_PSO_REUSE");
  return enabled;
}
inline bool SkipEmptyStatic() {
  static const bool enabled = ReadFlag("MACRUNNER_DX12_SKIP_EMPTY_STATIC");
  return enabled;
}
} // namespace dxmt::binding_elision
