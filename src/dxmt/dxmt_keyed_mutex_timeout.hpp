/* LGPL-2.1-or-later. MacRunner adaptation of DXMT 6d50ddff timeout conversion. */
#pragma once
#include <cstdint>
#include <optional>

namespace dxmt {
inline std::optional<int64_t> keyedMutexRelativeTimeout(uint32_t milliseconds) {
  if (milliseconds == UINT32_MAX) // Win32 INFINITE: null timeout pointer.
    return std::nullopt;
  return -static_cast<int64_t>(milliseconds) * INT64_C(10000);
}
}
