// instr.cpp — implementation of the safe Win32-only instrumentation logger.
#include "instr.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace dxmt {
namespace instr {

#ifdef _WIN32

static CRITICAL_SECTION s_cs;
static bool s_cs_inited = false;
static HANDLE s_file = INVALID_HANDLE_VALUE;
static bool s_opened = false;

static void ensure_cs() {
  if (!s_cs_inited) {
    InitializeCriticalSection(&s_cs);
    s_cs_inited = true;
  }
}

static HANDLE get_file_locked() {
  // caller holds s_cs
  if (s_opened)
    return s_file;
  s_opened = true;

  char path[MAX_PATH];
  path[0] = 0;
  DWORD n = GetEnvironmentVariableA("DXMT_INSTR_LOG", path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    // default: <cwd>/dxmt-instr.log
    DWORD cwdLen = GetCurrentDirectoryA(MAX_PATH - 24, path);
    if (cwdLen == 0 || cwdLen >= MAX_PATH - 24) {
      // fall back to just the bare filename
      path[0] = 0;
    } else if (cwdLen > 0 &&
               path[cwdLen - 1] != '\\' && path[cwdLen - 1] != '/') {
      path[cwdLen] = '\\';
      path[cwdLen + 1] = 0;
    }
    lstrlenA(path); // ensure nul-terminated
    // append filename
    const char *fn = "dxmt-instr.log";
    size_t pl = lstrlenA(path);
    if (pl + lstrlenA(fn) < MAX_PATH) {
      lstrcatA(path, fn);
    } else {
      lstrcpyA(path, fn);
    }
  }

  s_file = CreateFileA(path, FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (s_file == INVALID_HANDLE_VALUE) {
    // last resort: try bare filename in CWD (no dir)
    s_file = CreateFileA("dxmt-instr.log", FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  return s_file;
}

void logf(const char *fmt, ...) {
  ensure_cs();
  EnterCriticalSection(&s_cs);
  HANDLE f = get_file_locked();

  char buf[1400];
  // prefix: [pid/tid]
  DWORD pid = GetCurrentProcessId();
  DWORD tid = GetCurrentThreadId();
  int pre = _snprintf(buf, sizeof(buf), "[%-6u/%-6u] ", pid, tid);
  if (pre < 0) pre = 0;
  if ((size_t)pre >= sizeof(buf)) pre = (int)sizeof(buf) - 1;

  va_list ap;
  va_start(ap, fmt);
  int n = _vsnprintf(buf + pre, sizeof(buf) - pre - 2, fmt, ap);
  va_end(ap);
  if (n < 0) n = (int)sizeof(buf) - pre - 2;
  int total = pre + n;
  if ((size_t)total >= sizeof(buf) - 1) total = (int)sizeof(buf) - 2;
  buf[total] = '\n';
  buf[total + 1] = 0;
  DWORD toWrite = total + 1;

  if (f != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(f, buf, toWrite, &written, nullptr);
    FlushFileBuffers(f);
  }
  LeaveCriticalSection(&s_cs);
}

void lograw(const char *b, uint32_t len) {
  ensure_cs();
  EnterCriticalSection(&s_cs);
  HANDLE f = get_file_locked();
  if (f != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(f, b, len, &written, nullptr);
    FlushFileBuffers(f);
  }
  LeaveCriticalSection(&s_cs);
}

void shutdown() {
  ensure_cs();
  EnterCriticalSection(&s_cs);
  if (s_file != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(s_file);
    CloseHandle(s_file);
    s_file = INVALID_HANDLE_VALUE;
  }
  LeaveCriticalSection(&s_cs);
}

#else  // !_WIN32

void logf(const char * /*fmt*/, ...) {}
void lograw(const char * /*b*/, uint32_t /*len*/) {}
void shutdown() {}

#endif

} // namespace instr
} // namespace dxmt
