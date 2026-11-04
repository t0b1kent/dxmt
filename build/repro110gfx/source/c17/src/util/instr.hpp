// instr.hpp — safe, stream-free entry-point instrumentation logger for DXMT.
//
// Rationale: the existing dxmt::Logger uses std::ofstream/std::stringstream
// (Meyers-singleton), whose vtable is uninitialized under the ARM64X EC entry,
// causing c0000005 on first use (see log.cpp MACRUNNER guard). This logger uses
// ONLY Win32 CreateFileA/WriteFile + CRT _vsnprintf (no C++ stream objects),
// so it is safe to call from any DXMT entry point, including the very first
// DllMain/static-init path.
//
// Output: one shared log file (per process) in append mode.
//   - Override path with env var DXMT_INSTR_LOG (full path).
//   - Default: <cwd>/dxmt-instr.log
// Each line is prefixed with [pid/tid] and flushed on write. Thread-safe
// via a CRITICAL_SECTION.
#pragma once

#include <cstdint>

namespace dxmt {
namespace instr {

// Append one formatted line to the instrumentation log. Safe to call from
// any thread, any time. Returns void (never throws / never crashes on its own).
void logf(const char *fmt, ...);

// Optional: write a marker line + a raw byte blob (for hex dumps). Unused for now.
void lograw(const char *buf, uint32_t len);

// Hint that the process is detaching (DllMain DETACH). Flushes the file.
void shutdown();

} // namespace instr
} // namespace dxmt

// Convenience macros for entry-point CALL/RETURN tracing.
#define INSTR_CALL(name, ...) ::dxmt::instr::logf("CALL  %s " __VA_ARGS__, name)
#define INSTR_RET(name, ...)  ::dxmt::instr::logf("RET   %s " __VA_ARGS__, name)
#define INSTR_LOG(...)        ::dxmt::instr::logf(__VA_ARGS__)
