// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once
// MacRunner (curator, 06.10.2026): finer locking for the command-recording hot path. Diagnostic key, default off.
//   MACRUNNER_DX12_FINE_LOCKS=1
//     * descriptor heaps: one lock per stripe of 16 slots instead of one lock per heap. Direct3D 12 lets threads write
//       different descriptors of one heap at the same time; a heap-wide lock makes the game's recording threads queue
//       behind each other on every descriptor write and copy.
//     * device lookup tables (address -> buffer, resource -> residency source): lookups take the lock shared,
//       registration keeps it exclusive.
//   With the key off every index maps to stripe 0 and lookups lock exclusively: exactly the previous behaviour.
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include "thread.hpp"

namespace dxmt {

inline bool
FineLocksEnabled() {
  static const bool enabled = [] {
    const DWORD error = GetLastError();
    char value[4] = {};
    const bool on = GetEnvironmentVariableA("MACRUNNER_DX12_FINE_LOCKS", value, sizeof(value)) == 1 && value[0] == '1';
    SetLastError(error);
    return on;
  }();
  return enabled;
}

class DescriptorSlotLocks {
  static constexpr size_t kStripes = 256;
  std::array<dxmt::mutex, kStripes> stripes_;

public:
  dxmt::mutex &
  For(uint64_t index) {
    return stripes_[FineLocksEnabled() ? size_t((index >> 4) & (kStripes - 1)) : 0];
  }
};

class SharedLookupLock {
  dxmt::shared_mutex &mutex_;
  const bool shared_;

public:
  explicit SharedLookupLock(dxmt::shared_mutex &mutex) : mutex_(mutex), shared_(FineLocksEnabled()) {
    if (shared_)
      mutex_.lock_shared();
    else
      mutex_.lock();
  }
  ~SharedLookupLock() {
    if (shared_)
      mutex_.unlock_shared();
    else
      mutex_.unlock();
  }
  SharedLookupLock(const SharedLookupLock &) = delete;
  SharedLookupLock &operator=(const SharedLookupLock &) = delete;
};

} // namespace dxmt
