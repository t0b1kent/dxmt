// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "sha1/sha1_util.hpp"

using DWORD = uint32_t;
using BOOL = int;
using SIZE_T = size_t;
using HRESULT = int32_t;
using HANDLE = intptr_t;
constexpr HANDLE INVALID_HANDLE_VALUE = -1, STD_ERROR_HANDLE = -12;
constexpr DWORD GENERIC_READ = 1, GENERIC_WRITE = 2, FILE_SHARE_READ = 1;
constexpr DWORD OPEN_EXISTING = 3, CREATE_NEW = 1, FILE_ATTRIBUTE_NORMAL = 128;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 16, FILE_FLAG_WRITE_THROUGH = 0x80000000;
constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xffffffff;
constexpr DWORD ERROR_WRITE_FAULT = 29, ERROR_DISK_FULL = 112, ERROR_FILE_TOO_LARGE = 223;
constexpr DWORD ERROR_FILENAME_EXCED_RANGE = 206, ERROR_INVALID_NAME = 123, ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr HRESULT E_FAIL = HRESULT(0x80004005u);
struct FILETIME { DWORD dwLowDateTime{}, dwHighDateTime{}; };
struct LARGE_INTEGER { int64_t QuadPart{}; };
struct D3D12_SHADER_BYTECODE { const void *pShaderBytecode{}; SIZE_T BytecodeLength{}; };
struct D3D12_GRAPHICS_PIPELINE_STATE_DESC { D3D12_SHADER_BYTECODE VS, PS, HS, DS, GS; };
struct D3D12_COMPUTE_PIPELINE_STATE_DESC { D3D12_SHADER_BYTECODE CS; };

namespace mock {
struct Open { std::wstring path; size_t position = 0; bool writable = false; };
std::map<std::wstring, std::vector<uint8_t>> files;
std::map<HANDLE, Open> handles;
std::string stderr_text;
DWORD error = 0;
HANDLE next = 1;
bool frame = true, dir = true, collision = false, fail_binary = false, fail_flush = false;
bool fail_read = false, fail_memory = false, fail_close = false, zero_write = false, partial_fail = false;
bool fail_journal = false;
size_t max_write = SIZE_MAX, writes = 0, creates = 0;
void reset() {
  assert(handles.empty()); files.clear(); stderr_text.clear(); error = 0; next = 1;
  frame = dir = true;
  collision = fail_binary = fail_flush = fail_read = fail_memory = fail_close = false;
  zero_write = partial_fail = fail_journal = false;
  max_write = SIZE_MAX; writes = creates = 0;
}
bool binary(const std::wstring &p) { return p.size() >= 4 && p.substr(p.size()-4) == L".bin"; }
std::string journal() {
  for (auto &[path, bytes] : files)
    if (path.size() >= 6 && path.substr(path.size()-6) == L".jsonl") return {bytes.begin(), bytes.end()};
  return {};
}
size_t count(const std::string &s, const std::string &n) {
  size_t result = 0, p = 0;
  while ((p = s.find(n, p)) != std::string::npos) { ++result; p += n.size(); }
  return result;
}
}

DWORD GetLastError() { return mock::error; }
void SetLastError(DWORD value) { mock::error = value; }
HANDLE GetCurrentProcess() { return 999; }
HANDLE GetStdHandle(HANDLE) { return 9999; }
DWORD GetCurrentProcessId() { return 42; }
BOOL GetProcessTimes(HANDLE, FILETIME *c, FILETIME *, FILETIME *, FILETIME *) { c->dwLowDateTime = 7; return 1; }
DWORD GetEnvironmentVariableA(const char *, char *p, DWORD size) {
  if (!mock::frame) return 0;
  if (size >= 2) { p[0] = '1'; p[1] = 0; } return 1;
}
DWORD GetEnvironmentVariableW(const wchar_t *, wchar_t *p, DWORD size) {
  if (!mock::dir) return 0;
  const wchar_t value[] = L"Z:\\capture";
  if (!p || size < 11) return 11;
  std::memcpy(p, value, sizeof(value)); return 10;
}
DWORD GetFileAttributesW(const wchar_t *) { return FILE_ATTRIBUTE_DIRECTORY; }
HANDLE CreateFileW(const wchar_t *p, DWORD access, DWORD, void *, DWORD disposition, DWORD, void *) {
  ++mock::creates;
  if (disposition == CREATE_NEW) {
    if (mock::files.count(p) || (mock::fail_binary && mock::binary(p))) { mock::error = 80; return -1; }
    mock::files[p] = {};
  } else if (!mock::files.count(p)) { mock::error = 2; return -1; }
  HANDLE h = mock::next++;
  mock::handles[h] = {p, 0, access == GENERIC_WRITE};
  mock::error = 4321;
  return h;
}
BOOL CloseHandle(HANDLE h) {
  if (!mock::handles.count(h)) return 0;
  bool fail = mock::fail_close && mock::binary(mock::handles.at(h).path);
  mock::handles.erase(h);
  if (fail) mock::error = 6;
  return !fail;
}
BOOL WriteFile(HANDLE h, const void *p, DWORD n, DWORD *written, void *) {
  ++mock::writes;
  if (h == 9999) { mock::stderr_text.append(static_cast<const char *>(p), n); *written = n; return 1; }
  auto &open = mock::handles.at(h);
  bool binary = mock::binary(open.path);
  if ((mock::zero_write && binary) || (mock::fail_journal && !binary)) {
    *written = 0; mock::error = 112; return mock::zero_write;
  }
  *written = DWORD(std::min<size_t>(n, mock::max_write));
  if (mock::partial_fail && binary) *written = std::min<DWORD>(*written, 3);
  auto &bytes = mock::files.at(open.path);
  bytes.insert(bytes.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + *written);
  mock::error = 29;
  return !(mock::partial_fail && binary);
}
BOOL FlushFileBuffers(HANDLE h) {
  bool ok = !(mock::fail_flush && mock::binary(mock::handles.at(h).path));
  if (!ok) mock::error = 29;
  return ok;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) { size->QuadPart = mock::files.at(mock::handles.at(h).path).size(); return 1; }
BOOL ReadFile(HANDLE h, void *p, DWORD n, DWORD *read, void *) {
  if (mock::fail_read) { *read = 0; mock::error = 30; return 0; }
  auto &open = mock::handles.at(h); auto &bytes = mock::files.at(open.path);
  *read = DWORD(std::min<size_t>(n, bytes.size() - open.position));
  std::memcpy(p, bytes.data() + open.position, *read); open.position += *read; return 1;
}
BOOL ReadProcessMemory(HANDLE, const void *p, void *out, SIZE_T n, SIZE_T *read) {
  if (!p || mock::fail_memory) { *read = 0; mock::error = 299; return 0; }
  std::memcpy(out, p, n); *read = n; return 1;
}

namespace dxmt {
struct CaptureTestHash {
  static Sha1Digest compute(const void *data, size_t size) {
    return mock::collision ? Sha1Digest{} : Sha1HashState::compute(data, size);
  }
};
}
#define Sha1HashState CaptureTestHash
#include "../d3d12_shader_capture.hpp"
#undef Sha1HashState

int main(int argc, char **argv) {
  using dxmt::shader_capture::State;
  int tests = 0;
  const uint8_t first[] = {1,2,3,4}, second[] = {5,6,7,8};
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs{{first, sizeof(first)}};
  auto run = [&](State &state, HRESULT hr = 0) { auto id = state.begin("compute", nullptr, &cs); state.end(id, hr, true); };
  for (bool frame : {false,true}) {
    mock::reset(); mock::frame = frame; mock::dir = !frame;
    { State s; run(s); } assert(mock::files.empty()); ++tests;
  }
  mock::reset();
  { State s; mock::error = 77; run(s); assert(mock::error == 77); run(s, E_FAIL); assert(mock::error == 77); }
  assert(mock::files.size() == 2);
  assert(mock::count(mock::journal(), "\"deduplicated\":true") == 1);
  assert(mock::journal().find("\"hr\":\"80004005\"") != std::string::npos);
  assert(mock::journal().find("\"event\":\"session.end\"") != std::string::npos); ++tests;
  mock::reset();
  { State s; D3D12_GRAPHICS_PIPELINE_STATE_DESC g{cs.CS,cs.CS,cs.CS,cs.CS,cs.CS};
    auto id = s.begin("graphics", nullptr, &g); s.end(id, 0, true); run(s); }
  assert(mock::count(mock::journal(), "\"event\":\"shader.end\"") == 6);
  assert(mock::files.size() == 2); ++tests;
  mock::reset(); mock::collision = true;
  { State s; run(s); auto d = cs; d.CS.pShaderBytecode = second;
    auto id = s.begin("compute", nullptr, &d); s.end(id, 0, true); run(s); }
  assert(mock::files.size() == 3);
  assert(mock::count(mock::journal(), "\"deduplicated\":true") == 1); ++tests;
  mock::reset(); mock::max_write = 2;
  { State s; run(s); }
  assert(mock::journal().find("\"errors\":0") != std::string::npos); ++tests;
  for (int failure = 0; failure < 6; ++failure) {
    mock::reset();
    { State s;
      if (failure == 0) mock::fail_binary = true;
      if (failure == 1) mock::fail_flush = true;
      if (failure == 2) mock::zero_write = true;
      if (failure == 3) mock::partial_fail = true;
      if (failure == 4) mock::fail_close = true;
      if (failure == 5) mock::fail_journal = true;
      run(s); auto writes = mock::writes; auto creates = mock::creates;
      for (int i = 0; i < 40; ++i) run(s);
      assert(mock::writes == writes && mock::creates == creates);
    }
    assert(mock::count(mock::stderr_text, "dx12_shader_capture_incomplete") == 1);
    if (failure != 5) assert(mock::journal().find("\"suppressed_psos\":40") != std::string::npos);
    ++tests;
  }
  mock::reset();
  { State s; run(s); mock::fail_read = true; run(s); }
  assert(mock::journal().find("dedup_read") != std::string::npos); ++tests;
  mock::reset();
  { State s; run(s); for (auto &[p,b] : mock::files) if (mock::binary(p)) b.push_back(1); run(s); }
  assert(mock::journal().find("dedup_size") != std::string::npos); ++tests;
  mock::reset();
  { State s; auto huge = cs; huge.CS.BytecodeLength = (64ull<<20)+1;
    auto id = s.begin("compute", nullptr, &huge); s.end(id, 0, true); run(s); }
  assert(mock::journal().find("original_size_quota") != std::string::npos);
  assert(mock::count(mock::journal(), "\"event\":\"shader.end\"") == 1); ++tests;
  mock::reset();
  { State s; auto invalid = cs; invalid.CS.pShaderBytecode = nullptr;
    auto id = s.begin("compute", nullptr, &invalid); s.end(id, E_FAIL, true); }
  assert(mock::journal().find("original_memory_read") != std::string::npos); ++tests;
  mock::reset();
  { State s; auto id = s.begin("compute", nullptr, &cs); s.end(id, E_FAIL, false); }
  assert(mock::journal().find("\"hr\":null,\"returned\":false") != std::string::npos); ++tests;
  mock::reset();
  { State s; auto a = s.begin("compute", nullptr, &cs), b = s.begin("compute", nullptr, &cs);
    s.end(a, 0, true); s.end(b, 0, true); }
  assert(mock::journal().find("\"incomplete\":true") == std::string::npos); ++tests;
  mock::reset();
  { State s; for (uint32_t i=0; i<2055; ++i) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC d{{&i,sizeof(i)}};
      auto id=s.begin("compute",nullptr,&d); s.end(id,0,true);
    } }
  assert(mock::files.size() == 2056);
  assert(mock::count(mock::journal(), "\"event\":\"pso.result\"") == 2055); ++tests;
  if (argc == 2) {
    FILE *file = std::fopen(argv[1], "wb"); assert(file);
    auto journal = mock::journal();
    assert(std::fwrite(journal.data(), 1, journal.size(), file) == journal.size());
    assert(std::fclose(file) == 0);
  }
  // A later initialization in the same process must not overwrite an earlier inventory.
  const auto saved = mock::journal();
  { State s; run(s); }
  assert(mock::journal() == saved);
  assert(mock::stderr_text.find("inventory_create_new") != std::string::npos); ++tests;
  std::printf("capture-all mocked I/O: %d tests PASS\n", tests);
}
