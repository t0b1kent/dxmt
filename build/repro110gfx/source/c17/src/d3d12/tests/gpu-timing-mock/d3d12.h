#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
using DWORD = uint32_t;
using HANDLE = void *;
struct LARGE_INTEGER { int64_t QuadPart; };
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
constexpr DWORD STD_ERROR_HANDLE = DWORD(-12);
inline thread_local DWORD mock_error = 0;
inline std::atomic<int64_t> mock_clock{1000000};
inline std::atomic<unsigned> mock_clock_calls{0};
inline std::vector<std::string> mock_lines;
inline std::mutex mock_mutex;
inline DWORD GetLastError() { return mock_error; }
inline void SetLastError(DWORD v) { mock_error = v; }
inline DWORD GetEnvironmentVariableA(const char *name, char *out, DWORD capacity) {
  const char *value = std::getenv(name);
  if (!value) { mock_error = 203; return 0; }
  const size_t n = std::strlen(value);
  if (n >= capacity) return DWORD(n + 1);
  std::memcpy(out, value, n + 1);
  return DWORD(n);
}
inline bool QueryPerformanceCounter(LARGE_INTEGER *q) {
  ++mock_clock_calls; q->QuadPart = mock_clock.load(); mock_error = 901; return true;
}
inline bool QueryPerformanceFrequency(LARGE_INTEGER *q) { q->QuadPart = 1000000; return true; }
inline void GetSystemTimeAsFileTime(FILETIME *t) { *t = {123, 456}; }
inline HANDLE GetStdHandle(DWORD) { return nullptr; }
inline DWORD GetCurrentThreadId() { return 7; }
inline bool WriteFile(HANDLE, const void *line, DWORD size, DWORD *written, void *) {
  std::lock_guard<std::mutex> lock(mock_mutex);
  mock_lines.emplace_back(static_cast<const char *>(line), size);
  *written = size; mock_error = 902; return true;
}
