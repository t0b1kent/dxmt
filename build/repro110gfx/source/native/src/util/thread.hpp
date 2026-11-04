/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unknwn.h>

#include "util_error.hpp"


#include "./rc/util_rc.hpp"
#include "./rc/util_rc_ptr.hpp"

namespace dxmt {

/**
 * \brief Thread priority
 */
enum class ThreadPriority : int32_t {
  Normal,
  Lowest,
};

#ifdef _WIN32
/**
 * \brief Thread helper class
 *
 * This is needed mostly  for winelib builds. Wine needs to setup each thread
 * that calls Windows APIs. It means that in winelib builds, we can't let
 * standard C++ library create threads and need to use Wine for that instead. We
 * use a thin wrapper around Windows thread functions so that the rest of code
 * just has to use dxmt::thread class instead of std::thread.
 */
class ThreadFn : public RcObject {
  using Proc = std::function<void()>;

public:
  ThreadFn(Proc &&proc) : m_proc(std::move(proc)) {
    // Reference for the thread function
    this->incRef();

    m_handle = ::CreateThread(nullptr, 0x100000, ThreadFn::threadProc, this,
                              STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);

    if (m_handle == nullptr)
      throw MTLD3DError("Failed to create thread");
  }

  ~ThreadFn() {
    if (this->joinable())
      std::terminate();
  }

  void detach() {
    ::CloseHandle(m_handle);
    m_handle = nullptr;
  }

  void join() {
    if (::WaitForSingleObjectEx(m_handle, INFINITE, FALSE) == WAIT_FAILED)
      throw MTLD3DError("Failed to join thread");
    this->detach();
  }

  bool joinable() const { return m_handle != nullptr; }

  void set_priority(ThreadPriority priority) {
    int32_t value;
    switch (priority) {
    default:
    case ThreadPriority::Normal:
      value = THREAD_PRIORITY_NORMAL;
      break;
    case ThreadPriority::Lowest:
      value = THREAD_PRIORITY_LOWEST;
      break;
    }
    ::SetThreadPriority(m_handle, int32_t(value));
  }

private:
  Proc m_proc;
  HANDLE m_handle;

  static DWORD WINAPI threadProc(void *arg) {
    auto thread = reinterpret_cast<ThreadFn *>(arg);
    thread->m_proc();
    thread->decRef();
    return 0;
  }
};

/**
 * \brief RAII thread wrapper
 *
 * Wrapper for \c ThreadFn that can be used
 * as a drop-in replacement for \c std::thread.
 */
class thread {

public:
  thread() {}

  explicit thread(std::function<void()> &&func)
      : m_thread(new ThreadFn(std::move(func))) {}

  thread(thread &&other) : m_thread(std::move(other.m_thread)) {}

  thread &operator=(thread &&other) {
    m_thread = std::move(other.m_thread);
    return *this;
  }

  void detach() { m_thread->detach(); }

  void join() { m_thread->join(); }

  bool joinable() const { return m_thread != nullptr && m_thread->joinable(); }

  void set_priority(ThreadPriority priority) {
    m_thread->set_priority(priority);
  }

  static uint32_t hardware_concurrency() {
    SYSTEM_INFO info = {};
    ::GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
  }

private:
  Rc<ThreadFn> m_thread;
};

namespace this_thread {
inline void yield() { SwitchToThread(); }

inline uint32_t get_id() { return uint32_t(GetCurrentThreadId()); }

bool isInModuleDetachment();
} // namespace this_thread

/**
 * \brief SRW-based mutex implementation
 *
 * Drop-in replacement for \c std::mutex that uses Win32
 * SRW locks, which are implemented with \c futex in wine.
 */
// GAPS 01.11 (MacRunner) MACRUNNER_DX12_LOCK_STATS=1, default 0, observation only: a lock that is busy at the first try is
// counted per lock site, with the time spent waiting; after 256 and then every 4 096 contentions one block of "dx12_lock_stats" lines
// (top 16 sites by total wait). A site is TWO return addresses: ra0 = the instruction after the call of the slow path (inside
// mutex::lock() when the compiler kept lock() out of line, as it does in d3d12.dll; inside the locking function when lock() was
// inlined), ra1 = lock()'s own return address (__builtin_return_address(0) taken in lock(): the locking function when lock() is out of
// line, its caller when inlined): "site=<module>+0x<ra0><<module>+0x<ra1>". GAPS 01.11 14:xx: with ra0 alone every contention of
// d3d12.dll landed on one address inside dxmt::mutex::lock() (stand LC1: d3d12.dll+0xa8e8 = lock()+0x4c); a frame-record walk instead
// of ra1 skipped the locking function (lock() saves lr without a frame record) and named its caller.
// The slow path keeps a frame record of its own (ARM64EC code here omits them): without it the sampler's PE chain of a contended wait
// skipped every d3d12.dll frame and ended at the game's x64 call into the layer (stand LC2 14:04), with it at d3d12.dll (LC2 13:59).
namespace lock_stats {
inline bool Enabled() {
  static const bool v = [] {
    const DWORD saved = GetLastError();
    char t[4] = {};
    const bool r = GetEnvironmentVariableA("MACRUNNER_DX12_LOCK_STATS", t, sizeof(t)) == 1 && t[0] == '1';
    if (r) {
      static const char marker[] = "dx12_lock_stats enabled=1\n";
      DWORD written;
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), marker, sizeof(marker) - 1, &written, nullptr);
    }
    SetLastError(saved);
    return r;
  }();
  return v;
}
struct Site { std::atomic<uintptr_t> key{0}, ra0{0}, ra1{0}; std::atomic<uint64_t> count{0}, ticks{0}; std::atomic<unsigned> kind{0}; };
inline Site *Table() { static Site t[512]; return t; }
inline std::atomic<uint64_t> &Total() { static std::atomic<uint64_t> n{0}; return n; }
inline uint64_t Qpc() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return uint64_t(v.QuadPart); }
inline void Report() {
  static std::atomic<unsigned> printing{0};
  if (printing.exchange(1)) return;
  LARGE_INTEGER f; QueryPerformanceFrequency(&f);
  Site *t = Table();
  bool used[512] = {};
  char line[256];
  DWORD written;
  int n = snprintf(line, sizeof(line), "dx12_lock_stats contended=%llu\n", (unsigned long long)Total().load());
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
  for (int k = 0; k < 16; ++k) {
    int best = -1;
    for (int i = 0; i < 512; ++i)
      if (!used[i] && t[i].count.load() && (best < 0 || t[i].ticks.load() > t[best].ticks.load())) best = i;
    if (best < 0) break;
    used[best] = true;
    char names[2][96];
    for (int r = 0; r < 2; ++r) {
      const uintptr_t ra = r ? t[best].ra1.load() : t[best].ra0.load();
      HMODULE m = nullptr; char path[MAX_PATH] = "?";
      if (ra && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(ra), &m) && m)
        GetModuleFileNameA(m, path, sizeof(path));
      const char *base = path;
      for (const char *q = path; *q; ++q) if (*q == '\\' || *q == '/') base = q + 1;
      if (m) snprintf(names[r], sizeof(names[r]), "%s+0x%llx", base, (unsigned long long)(ra - reinterpret_cast<uintptr_t>(m)));
      else snprintf(names[r], sizeof(names[r]), "?@0x%llx", (unsigned long long)ra);
    }
    n = snprintf(line, sizeof(line), "dx12_lock_stats_site rank=%d site=%s<%s kind=%u count=%llu wait_us=%.1f\n", k, names[0], names[1],
                 t[best].kind.load(), (unsigned long long)t[best].count.load(), double(t[best].ticks.load()) * 1e6 / double(f.QuadPart));
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
  }
  printing = 0;
}
inline void Note(uintptr_t ra0, uintptr_t ra1, uint64_t ticks, unsigned kind) {
  Site *t = Table();
  const uintptr_t key = (ra0 ^ (ra1 * 0x9e3779b97f4a7c15ull)) | 1;
  size_t h = (key >> 2) * 0x9e3779b97f4a7c15ull >> 55;   // 512 slots
  for (int probe = 0; probe < 512; ++probe, h = (h + 1) & 511) {
    uintptr_t cur = t[h].key.load(std::memory_order_relaxed);
    if (cur == 0 && t[h].key.compare_exchange_strong(cur, key)) {
      cur = key;
      t[h].ra0.store(ra0, std::memory_order_relaxed);
      t[h].ra1.store(ra1, std::memory_order_relaxed);
    }
    if (cur == key) {
      t[h].count.fetch_add(1, std::memory_order_relaxed);
      t[h].ticks.fetch_add(ticks, std::memory_order_relaxed);
      t[h].kind.store(kind, std::memory_order_relaxed);
      break;
    }
  }
  const uint64_t n = Total().fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 256 || n % 4096 == 0) Report();
}
__attribute__((noinline)) inline void ContendedExclusive(PSRWLOCK l, uintptr_t ra1) {
  const uintptr_t ra0 = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  __asm__ volatile("" ::"r"(__builtin_frame_address(0)));   // keep a frame record: the PC sampler's PE chain (PECHAIN) then ends in d3d12.dll for this wait
  const uint64_t t0 = Qpc();
  AcquireSRWLockExclusive(l);
  Note(ra0, ra1, Qpc() - t0, 1);
}
__attribute__((noinline)) inline void ContendedShared(PSRWLOCK l, uintptr_t ra1) {
  const uintptr_t ra0 = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  __asm__ volatile("" ::"r"(__builtin_frame_address(0)));   // keep a frame record: the PC sampler's PE chain (PECHAIN) then ends in d3d12.dll for this wait
  const uint64_t t0 = Qpc();
  AcquireSRWLockShared(l);
  Note(ra0, ra1, Qpc() - t0, 2);
}
__attribute__((noinline)) inline void ContendedCritical(PCRITICAL_SECTION l, uintptr_t ra1) {
  const uintptr_t ra0 = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  __asm__ volatile("" ::"r"(__builtin_frame_address(0)));   // keep a frame record: the PC sampler's PE chain (PECHAIN) then ends in d3d12.dll for this wait
  const uint64_t t0 = Qpc();
  EnterCriticalSection(l);
  Note(ra0, ra1, Qpc() - t0, 3);
}
} // namespace lock_stats

class mutex {

public:
  using native_handle_type = PSRWLOCK;

  mutex() {}

  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  void lock() {
    if (lock_stats::Enabled()) { if (!TryAcquireSRWLockExclusive(&m_lock)) lock_stats::ContendedExclusive(&m_lock, reinterpret_cast<uintptr_t>(__builtin_return_address(0))); return; }
    AcquireSRWLockExclusive(&m_lock);
  }

  void unlock() { ReleaseSRWLockExclusive(&m_lock); }

  bool try_lock() { return TryAcquireSRWLockExclusive(&m_lock); }

  native_handle_type native_handle() { return &m_lock; }

private:
  SRWLOCK m_lock = SRWLOCK_INIT;
};

/**
 * \brief SRW-based shared mutex implementation
 */
class shared_mutex {

public:
  using native_handle_type = PSRWLOCK;

  shared_mutex() {}

  shared_mutex(const shared_mutex &) = delete;
  shared_mutex &operator=(const shared_mutex &) = delete;

  void lock() {
    if (lock_stats::Enabled()) { if (!TryAcquireSRWLockExclusive(&m_lock)) lock_stats::ContendedExclusive(&m_lock, reinterpret_cast<uintptr_t>(__builtin_return_address(0))); return; }
    AcquireSRWLockExclusive(&m_lock);
  }

  void lock_shared() {
    if (lock_stats::Enabled()) { if (!TryAcquireSRWLockShared(&m_lock)) lock_stats::ContendedShared(&m_lock, reinterpret_cast<uintptr_t>(__builtin_return_address(0))); return; }
    AcquireSRWLockShared(&m_lock);
  }

  void unlock() { ReleaseSRWLockExclusive(&m_lock); }

  void unlock_shared() { ReleaseSRWLockShared(&m_lock); }

  bool try_lock() { return TryAcquireSRWLockExclusive(&m_lock); }

  bool try_lock_shared() { return TryAcquireSRWLockShared(&m_lock); }

  native_handle_type native_handle() { return &m_lock; }

private:
  SRWLOCK m_lock = SRWLOCK_INIT;
};

/**
 * \brief Recursive mutex implementation
 *
 * Drop-in replacement for \c std::recursive_mutex that
 * uses Win32 critical sections.
 */
class recursive_mutex {

public:
  using native_handle_type = PCRITICAL_SECTION;

  recursive_mutex() { InitializeCriticalSection(&m_lock); }

  ~recursive_mutex() { DeleteCriticalSection(&m_lock); }

  recursive_mutex(const recursive_mutex &) = delete;
  recursive_mutex &operator=(const recursive_mutex &) = delete;

  void lock() {
    if (lock_stats::Enabled()) { if (!TryEnterCriticalSection(&m_lock)) lock_stats::ContendedCritical(&m_lock, reinterpret_cast<uintptr_t>(__builtin_return_address(0))); return; }
    EnterCriticalSection(&m_lock);
  }

  void unlock() { LeaveCriticalSection(&m_lock); }

  bool try_lock() { return TryEnterCriticalSection(&m_lock); }

  native_handle_type native_handle() { return &m_lock; }

private:
  CRITICAL_SECTION m_lock;
};

/**
 * \brief SRW-based condition variable implementation
 *
 * Drop-in replacement for \c std::condition_variable that
 * uses Win32 condition variables on SRW locks.
 */
class condition_variable {

public:
  using native_handle_type = PCONDITION_VARIABLE;

  condition_variable() { InitializeConditionVariable(&m_cond); }

  condition_variable(condition_variable &) = delete;

  condition_variable &operator=(condition_variable &) = delete;

  void notify_one() { WakeConditionVariable(&m_cond); }

  void notify_all() { WakeAllConditionVariable(&m_cond); }

  void wait(std::unique_lock<dxmt::mutex> &lock) {
    auto srw = lock.mutex()->native_handle();
    SleepConditionVariableSRW(&m_cond, srw, INFINITE, 0);
  }

  template <typename Predicate>
  void wait(std::unique_lock<dxmt::mutex> &lock, Predicate pred) {
    while (!pred())
      wait(lock);
  }

  template <typename Clock, typename Duration>
  std::cv_status
  wait_until(std::unique_lock<dxmt::mutex> &lock,
             const std::chrono::time_point<Clock, Duration> &time) {
    auto now = Clock::now();

    return (now < time) ? wait_for(lock, now - time) : std::cv_status::timeout;
  }

  template <typename Clock, typename Duration, typename Predicate>
  bool wait_until(std::unique_lock<dxmt::mutex> &lock,
                  const std::chrono::time_point<Clock, Duration> &time,
                  Predicate pred) {
    if (pred())
      return true;

    auto now = Clock::now();
    return now < time && wait_for(lock, now - time, pred);
  }

  template <typename Rep, typename Period>
  std::cv_status wait_for(std::unique_lock<dxmt::mutex> &lock,
                          const std::chrono::duration<Rep, Period> &timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    auto srw = lock.mutex()->native_handle();

    return SleepConditionVariableSRW(&m_cond, srw, ms.count(), 0)
               ? std::cv_status::no_timeout
               : std::cv_status::timeout;
  }

  template <typename Rep, typename Period, typename Predicate>
  bool wait_for(std::unique_lock<dxmt::mutex> &lock,
                const std::chrono::duration<Rep, Period> &timeout,
                Predicate pred) {
    bool result = pred();

    if (!result && wait_for(lock, timeout) == std::cv_status::no_timeout)
      result = pred();

    return result;
  }

  native_handle_type native_handle() { return &m_cond; }

private:
  CONDITION_VARIABLE m_cond;
};

#else
class thread : public std::thread {
public:
  using std::thread::thread;

  void set_priority(ThreadPriority priority) {
    ::sched_param param = {};
    int32_t policy;
    switch (priority) {
    default:
    case ThreadPriority::Normal:
      policy = SCHED_OTHER;
      break;
    case ThreadPriority::Lowest:
#ifdef __APPLE__
      policy = SCHED_FIFO; /* No SCHED_IDLE on macOS */
#else
      policy = SCHED_IDLE;
#endif
      break;
    }
    ::pthread_setschedparam(this->native_handle(), policy, &param);
  }
};

using mutex = std::mutex;
using shared_mutex = std::shared_mutex;
using recursive_mutex = std::recursive_mutex;
using condition_variable = std::condition_variable;

namespace this_thread {
inline void yield() { std::this_thread::yield(); }

uint32_t get_id();

inline bool isInModuleDetachment() { return false; }
} // namespace this_thread
#endif

struct null_mutex {
  void lock() {}
  void unlock() noexcept {}
  bool try_lock() { return true; }
};

} // namespace dxmt
