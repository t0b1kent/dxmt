#pragma once
#include "thread.hpp"
#include <atomic>
#include <cstdint>

namespace dxmt {

/* MacRunner 2026-07-03 (lane dxmtpoll): previously this class waited with a
 * Sleep(1) poll backstop because libc++ std::atomic<>::notify_all() wakes are
 * lost under HyperBridge x86->ARM64 emulation (the __ulock_wake /
 * WakeByAddressSingle contention-table path is not reliably delivered). That
 * turned every fence wait into a >=1ms-cadence poll plus a NtDelayExecution
 * round-trip through the whole JIT+wine+msync stack — a constant background
 * tax on every title.
 *
 * Fix: drive the wake through dxmt::condition_variable (Win32
 * SleepConditionVariableSRW / WakeConditionVariable, routed via wine's
 * SRW-futex / wineserver-wait path), which IS reliably delivered under HB
 * (the same mechanism dxmt_tasks.hpp::task_scheduler already relies on). The
 * atomic value stays the source of truth and remains lock-free for
 * signaledValue() readers (allocator hot paths); the mutex+condvar only guard
 * the predicate-check-vs-wait ordering, which makes lost wakeups impossible by
 * construction. No std::chrono is used (libc++ sleep_for's duration<long
 * double> overflow guard hits the x87 80-bit JIT gap). */
class CpuFence {
public:
  void wait(uint64_t value) {
    std::unique_lock<dxmt::mutex> lock(wake_m_);
    wake_cv_.wait(lock, [this, value]() {
      return value_.load(std::memory_order_acquire) >= value;
    });
  }

  void signal(uint64_t value) {
    std::unique_lock<dxmt::mutex> lock(wake_m_);
    auto current = value_.load(std::memory_order_relaxed);
    do {
      if (value <= current)
        return;
    } while (!value_.compare_exchange_weak(current, value, std::memory_order_release, std::memory_order_relaxed));
    wake_cv_.notify_all();
  }

  uint64_t signaledValue() {
    return value_.load(std::memory_order_acquire);
  }

  CpuFence(): value_(0) {}
  CpuFence(uint64_t initial_value): value_(initial_value) {}

private:
  std::atomic<uint64_t> value_;
  dxmt::mutex wake_m_;
  dxmt::condition_variable wake_cv_;
};
} // namespace dxmt
