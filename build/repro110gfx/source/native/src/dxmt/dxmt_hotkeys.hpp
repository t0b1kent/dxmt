// SPDX-License-Identifier: LGPL-2.1-or-later
// MacRunner SHADERS-GPU 02.11.2026 (owner via curator 15:15): hot keys — switches that are SAFE TO CHANGE AT A FRAME BOUNDARY, flipped
// while the program runs, so one launch can measure A B B A instead of four launches with menus.
//
//   MACRUNNER_DX12_HOTKEYS_FILE=<absolute unix path>   off when unset: no file is ever opened, Poll() returns at its first branch.
//
// File format: lines KEY=VALUE (integer), '#' comments; keys not in the table below are ignored (reported once). The presenter polls
// the file at most once per second, at the start of encoding a present (Presenter::synchronizeLayerProperties: one call per present
// on the queue's submission thread), so a new value is seen by work recorded after that present. Every applied change prints one
// stderr line:  dxmt_hotkeys applied KEY=V old=O presents=N tick_ms=T   (presents = presents polled by this process so far).
// Initial value of every key = its environment variable (so a launch with the key set and a hot switch to it start the same).
//
// A key belongs in the table only if every reader samples it per use and nothing cached depends on it (no pipeline compiled with it,
// no allocation sized by it). Readers: hotkeys::Value(k) (relaxed atomic load).
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "util_env.hpp"

namespace dxmt::hotkeys {

enum Key : unsigned {
  Clear3dXyz = 0,   // MACRUNNER_DXMT_CLEAR3D_XYZ: 3D UAV clear kernel (dxmt_command_clear.hpp); read in ClearUAV::begin per clear
  TypedPrecise = 1, // GPU4 MACRUNNER_DX12_TYPED_PRECISE (d3d12_gpu4_overlap.hpp): read once per ExecuteCommandLists encoding and per draw
  ClearSink = 2,    // GPU4 MACRUNNER_DX12_CLEAR_SINK (d3d12_gpu4_overlap.hpp): same; a pass recorded partly with the key off = unknown
  TsNeighbor = 3,   // GPU4 MACRUNNER_DX12_TS_NEIGHBOR (d3d12_gpu4_overlap.hpp): read once per ExecuteCommandLists encoding
  VSerial = 4,      // GPU4 MACRUNNER_DX12_VSERIAL (d3d12_gpu4_overlap.hpp): read once per ExecuteCommandLists encoding
  ShadowLate = 5,   // GPU4 MACRUNNER_DX12_SHADOW_LATE (d3d12_gpu4_overlap.hpp): read once per ExecuteCommandLists encoding and per draw
  LightRename = 6,  // LIGHTRN MACRUNNER_DX12_LIGHT_RENAME (d3d12_light_rename.hpp): read once per ExecuteCommandLists encoding
  PostChain = 7,    // POSTCS MACRUNNER_DX12_POST_CHAIN (d3d12_post_chain.hpp): read once per ExecuteCommandLists encoding
  Count
};

struct Entry {
  const char *name;
  std::atomic<int> value;
  bool from_env;
};

inline Entry *Table() {
  static Entry table[Count] = {
      {"MACRUNNER_DXMT_CLEAR3D_XYZ", {0}, false},
      {"MACRUNNER_DX12_TYPED_PRECISE", {0}, false},   // GPU4 02.11
      {"MACRUNNER_DX12_CLEAR_SINK", {0}, false},      // GPU4 02.11
      {"MACRUNNER_DX12_TS_NEIGHBOR", {0}, false},     // GPU4 02.11
      {"MACRUNNER_DX12_VSERIAL", {0}, false},         // GPU4 02.11 16:5x
      {"MACRUNNER_DX12_SHADOW_LATE", {0}, false},     // GPU4 02.11 17:1x
      {"MACRUNNER_DX12_LIGHT_RENAME", {0}, false},    // LIGHTRN 02.11 22:xx
      {"MACRUNNER_DX12_POST_CHAIN", {0}, false},      // POSTCS 03.11
  };
  static const bool init = [] {
    for (unsigned k = 0; k < Count; ++k) {
      std::string v = env::getEnvVar(table[k].name);
      if (!v.empty()) { table[k].value.store(atoi(v.c_str()), std::memory_order_relaxed); table[k].from_env = true; }
    }
    return true;
  }();
  (void)init;
  return table;
}

inline const std::wstring &FilePath() {
  // the unix path as a Wine NT path (\\?\unix\...); empty = feature off
  static const std::wstring path = [] {
    std::string p = env::getEnvVar("MACRUNNER_DX12_HOTKEYS_FILE");
    if (p.empty() || p[0] != '/') return std::wstring();
    std::wstring w = L"\\\\?\\unix";
    for (char c : p) w.push_back(c == '/' ? L'\\' : wchar_t((unsigned char)c));
    return w;
  }();
  return path;
}

inline bool Enabled() { return !FilePath().empty(); }

inline int Value(Key k) { return Table()[k].value.load(std::memory_order_relaxed); }

inline void Line(const char *text) {
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  DWORD w = 0;
  if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, text, (DWORD)strlen(text), &w, nullptr);
}

// One call per present (frame boundary). Off -> one predictable branch.
inline void Poll() {
  if (!Enabled()) return;
  static std::atomic<uint64_t> last_ms{0}, presents{0};
  static std::atomic<bool> reported_open{false}, reported_unknown{false};
  const uint64_t n = presents.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t now = GetTickCount64(), last = last_ms.load(std::memory_order_relaxed);
  if (last && now - last < 1000) return;
  last_ms.store(now, std::memory_order_relaxed);
  HANDLE f = CreateFileW(FilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    if (!reported_open.exchange(true)) {
      char b[160]; snprintf(b, sizeof b, "dxmt_hotkeys open_failed error=%lu presents=%llu\n", (unsigned long)GetLastError(), (unsigned long long)n);
      Line(b);
    }
    return;
  }
  char buf[4096]; DWORD got = 0;
  BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
  CloseHandle(f);
  if (!ok) return;
  buf[got] = 0;
  Entry *t = Table();
  for (char *line = buf; line && *line;) {
    char *next = strchr(line, '\n');
    if (next) *next++ = 0;
    char *hash = strchr(line, '#'); if (hash) *hash = 0;
    char *eq = strchr(line, '=');
    if (eq) {
      *eq = 0;
      char *key = line; while (*key == ' ' || *key == '\t') ++key;
      char *end = eq; while (end > key && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = 0;
      const int v = atoi(eq + 1);
      bool known = false;
      for (unsigned k = 0; k < Count; ++k) {
        if (strcmp(key, t[k].name)) continue;
        known = true;
        const int old = t[k].value.exchange(v, std::memory_order_relaxed);
        if (old != v) {
          char b[256]; snprintf(b, sizeof b, "dxmt_hotkeys applied %s=%d old=%d presents=%llu tick_ms=%llu\n", t[k].name, v, old,
                                (unsigned long long)n, (unsigned long long)now);
          Line(b);
        }
      }
      if (!known && *key && !reported_unknown.exchange(true)) {
        char b[256]; snprintf(b, sizeof b, "dxmt_hotkeys unknown_key %s (not in the hot-key table; ignored) presents=%llu\n", key, (unsigned long long)n);
        Line(b);
      }
    }
    line = next;
  }
}

} // namespace dxmt::hotkeys
