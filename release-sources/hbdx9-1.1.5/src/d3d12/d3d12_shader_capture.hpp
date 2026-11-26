// SPDX-License-Identifier: LGPL-2.1-or-later
// Added 2026 by the MacRunner project (D3D12 extensions); see README-MACRUNNER.md
#pragma once

#include "sha1/sha1_util.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxmt::shader_capture {

struct LastErrorGuard {
  DWORD value = GetLastError();
  ~LastErrorGuard() noexcept { SetLastError(value); }
};

struct File {
  HANDLE handle = INVALID_HANDLE_VALUE;
  explicit File(HANDLE h) : handle(h) {}
  ~File() noexcept { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  bool close() noexcept {
    HANDLE h = handle;
    handle = INVALID_HANDLE_VALUE;
    return CloseHandle(h) != 0;
  }
};

class State {
  struct Blob { std::string file; size_t size; };
  static constexpr uint64_t BlobLimit = 64ull << 20;
  static constexpr uint64_t TotalLimit = 16ull << 30;
  static constexpr uint64_t InventoryLimit = 512ull << 20;
  static constexpr uint64_t EmergencyReserve = 4096;
  HANDLE journal_ = INVALID_HANDLE_VALUE;
  bool enabled_ = false, stopped_ = false, incomplete_ = false, emergency_reported_ = false;
  std::wstring directory_;
  std::string prefix_;
  std::unordered_map<std::string, std::vector<Blob>> blobs_;
  std::mutex mutex_;
  DWORD pid_ = 0;
  uint64_t sequence_ = 0, psos_ = 0, open_ = 0, occurrences_ = 0, unique_ = 0;
  uint64_t errors_ = 0, suppressed_ = 0, blob_bytes_ = 0, inventory_bytes_ = 0, next_blob_ = 0;

  std::wstring path(const std::string &name) const {
    return directory_ + L"\\" + std::wstring(name.begin(), name.end());
  }

  void emergency(const char *reason, DWORD error) noexcept {
    if (emergency_reported_) return;
    emergency_reported_ = true;
    char line[256];
    int n = std::snprintf(line, sizeof(line),
        "dx12_shader_capture_incomplete pid=%lu reason=%s win32_error=%lu\n",
        (unsigned long)pid_, reason, (unsigned long)error);
    if (n > 0 && size_t(n) < sizeof(line)) {
      DWORD written = 0;
      WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(n), &written, nullptr);
    }
  }

  bool write(HANDLE file, const void *data, size_t size, uint64_t &counter) noexcept {
    const auto *bytes = static_cast<const uint8_t *>(data);
    while (size) {
      DWORD count = DWORD(std::min<size_t>(size, 1u << 20)), written = 0;
      BOOL ok = WriteFile(file, bytes, count, &written, nullptr);
      if (written > count) { SetLastError(ERROR_WRITE_FAULT); return false; }
      counter += written;
      if (!ok || !written) {
        if (ok) SetLastError(ERROR_WRITE_FAULT);
        return false;
      }
      bytes += written;
      size -= written;
    }
    return true;
  }

  bool append(const char *event, const char *fields, bool emergency_record = false) noexcept {
    if (journal_ == INVALID_HANDLE_VALUE) return false;
    char line[2048];
    int n = std::snprintf(line, sizeof(line),
        "{\"v\":1,\"pid\":%lu,\"seq\":%llu,\"event\":\"%s\"%s}\n",
        (unsigned long)pid_, (unsigned long long)++sequence_, event, fields);
    uint64_t limit = InventoryLimit - (emergency_record ? 0 : EmergencyReserve);
    if (n <= 0 || size_t(n) >= sizeof(line) || inventory_bytes_ > limit || uint64_t(n) > limit - inventory_bytes_) {
      SetLastError(ERROR_DISK_FULL);
      return false;
    }
    return write(journal_, line, size_t(n), inventory_bytes_) && FlushFileBuffers(journal_);
  }

  void failure(uint64_t pso, const char *stage, const char *reason, DWORD error, bool stop) noexcept {
    ++errors_;
    incomplete_ = true;
    char fields[512];
    std::snprintf(fields, sizeof(fields),
        ",\"pso\":%llu,\"stage\":\"%s\",\"reason\":\"%s\",\"win32_error\":%lu,\"incomplete\":true",
        (unsigned long long)pso, stage, reason, (unsigned long)error);
    // One best-effort error record; a torn journal still fails host verification.
    bool logged = !stopped_ && append("capture.error", fields, true);
    if (!logged || stop) {
      stopped_ = true;
      emergency(reason, error);
    }
  }

  bool record(uint64_t pso, const char *stage, const char *event, const char *fields) noexcept {
    if (stopped_) return false;
    if (append(event, fields)) return true;
    failure(pso, stage, "inventory_write_or_quota", GetLastError(), true);
    return false;
  }

  void totals(const char *event, uint64_t pso, bool final = false) noexcept {
    char fields[768];
    std::snprintf(fields, sizeof(fields),
        ",\"pso\":%llu,\"psos\":%llu,\"open_psos\":%llu,\"shader_occurrences\":%llu,\"unique_shaders\":%llu,"
        "\"errors\":%llu,\"suppressed_psos\":%llu,\"blob_bytes\":%llu,\"inventory_bytes\":%llu,\"incomplete\":%s",
        (unsigned long long)pso, (unsigned long long)psos_, (unsigned long long)open_,
        (unsigned long long)occurrences_, (unsigned long long)unique_, (unsigned long long)errors_,
        (unsigned long long)suppressed_, (unsigned long long)blob_bytes_, (unsigned long long)inventory_bytes_,
        incomplete_ || (final && open_) ? "true" : "false");
    if (final && stopped_) {
      // Do not retry on every PSO after storage failure. Shutdown gets one try.
      if (!append(event, fields, true)) emergency("final_inventory_write", GetLastError());
    } else record(pso, "", event, fields);
  }

  bool same(const Blob &blob, const std::vector<uint8_t> &bytes, uint64_t pso, const char *stage) {
    File file(CreateFileW(path(blob.file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.handle == INVALID_HANDLE_VALUE) {
      failure(pso, stage, "dedup_open", GetLastError(), true);
      return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size) || uint64_t(size.QuadPart) != blob.size) {
      failure(pso, stage, "dedup_size", GetLastError(), true);
      return false;
    }
    std::array<uint8_t, 8192> buffer;
    bool identical = true;
    for (size_t offset = 0; offset < bytes.size();) {
      DWORD count = DWORD(std::min(buffer.size(), bytes.size() - offset)), read = 0;
      if (!ReadFile(file.handle, buffer.data(), count, &read, nullptr) || read != count) {
        failure(pso, stage, "dedup_read", GetLastError(), true);
        return false;
      }
      identical &= std::memcmp(bytes.data() + offset, buffer.data(), count) == 0;
      offset += count;
    }
    if (!file.close()) {
      failure(pso, stage, "dedup_close", GetLastError(), true);
      return false;
    }
    return identical;
  }

  void shader(uint64_t pso, const char *stage, D3D12_SHADER_BYTECODE code) {
    ++occurrences_;
    if (stopped_) return;
    char fields[768];
    std::snprintf(fields, sizeof(fields), ",\"pso\":%llu,\"stage\":\"%s\",\"length\":%llu",
        (unsigned long long)pso, stage, (unsigned long long)code.BytecodeLength);
    if (!record(pso, stage, "shader.begin", fields)) return;
    if (!code.BytecodeLength) {
      std::strcat(fields, ",\"empty\":true");
      record(pso, stage, "shader.end", fields);
      return;
    }
    if (code.BytecodeLength > BlobLimit) {
      failure(pso, stage, "original_size_quota", ERROR_FILE_TOO_LARGE, false);
      return;
    }
    std::vector<uint8_t> bytes(code.BytecodeLength);
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), code.pShaderBytecode, bytes.data(), bytes.size(), &read) || read != bytes.size()) {
      failure(pso, stage, "original_memory_read", GetLastError(), false);
      return;
    }
    const auto hash = Sha1HashState::compute(bytes.data(), bytes.size()).string();
    const auto key = hash + ":" + std::to_string(bytes.size());
    std::string name;
    bool duplicate = false;
    auto found = blobs_.find(key);
    if (found != blobs_.end()) {
      for (const auto &candidate : found->second) {
        if (same(candidate, bytes, pso, stage)) { name = candidate.file; duplicate = true; break; }
        if (stopped_) return;
      }
    }
    if (!duplicate) {
      if (blob_bytes_ > TotalLimit || bytes.size() > TotalLimit - blob_bytes_) {
        failure(pso, stage, "binary_total_quota", ERROR_DISK_FULL, true);
        return;
      }
      name = prefix_ + "-s" + std::to_string(++next_blob_) + ".bin";
      File file(CreateFileW(path(name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
      if (file.handle == INVALID_HANDLE_VALUE) {
        failure(pso, stage, "binary_create_new", GetLastError(), true);
        return;
      }
      if (!write(file.handle, bytes.data(), bytes.size(), blob_bytes_) || !FlushFileBuffers(file.handle)) {
        failure(pso, stage, "binary_write_or_flush", GetLastError(), true);
        return;
      }
      if (!file.close()) { failure(pso, stage, "binary_close", GetLastError(), true); return; }
      blobs_[key].push_back({name, bytes.size()});
      ++unique_;
    }
    std::snprintf(fields, sizeof(fields),
        ",\"pso\":%llu,\"stage\":\"%s\",\"length\":%llu,\"sha1\":\"%s\",\"file\":\"%s\",\"deduplicated\":%s",
        (unsigned long long)pso, stage, (unsigned long long)bytes.size(), hash.c_str(), name.c_str(),
        duplicate ? "true" : "false");
    record(pso, stage, "shader.end", fields);
  }

  void initialize() {
    char flag[4]{};
    if (GetEnvironmentVariableA("MACRUNNER_DX12_FRAME_CAPTURE", flag, sizeof(flag)) != 1 || flag[0] != '1') return;
    DWORD length = GetEnvironmentVariableW(L"MACRUNNER_DX12_SHADER_CAPTURE_DIR", nullptr, 0);
    if (!length) return;
    enabled_ = true;
    pid_ = GetCurrentProcessId();
    if (length > 32767) { failure(0, "", "directory_length", ERROR_FILENAME_EXCED_RANGE, true); return; }
    std::vector<wchar_t> directory(length);
    DWORD actual = GetEnvironmentVariableW(L"MACRUNNER_DX12_SHADER_CAPTURE_DIR", directory.data(), length);
    if (!actual || actual >= length) { failure(0, "", "directory_environment", GetLastError(), true); return; }
    directory_.assign(directory.data(), actual);
    // Only absolute Windows paths. The host supplies and precreates a private directory.
    if (!(directory_.size() >= 3 && directory_[1] == L':' &&
          (directory_[2] == L'\\' || directory_[2] == L'/')) &&
        !(directory_.size() >= 3 && directory_[0] == L'\\' && directory_[1] == L'\\')) {
      failure(0, "", "directory_not_absolute", ERROR_INVALID_NAME, true); return;
    }
    DWORD attrs = GetFileAttributesW(directory_.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      failure(0, "", "directory_not_precreated", GetLastError(), true); return;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
      failure(0, "", "process_creation_time", GetLastError(), true); return;
    }
    char prefix[96];
    std::snprintf(prefix, sizeof(prefix), "capture-p%lu-%08lx%08lx", (unsigned long)pid_,
                  (unsigned long)creation.dwHighDateTime, (unsigned long)creation.dwLowDateTime);
    prefix_ = prefix;
    journal_ = CreateFileW(path(prefix_ + ".jsonl").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (journal_ == INVALID_HANDLE_VALUE) { failure(0, "", "inventory_create_new", GetLastError(), true); return; }
    record(0, "", "session.begin",
        ",\"blob_limit\":67108864,\"binary_limit\":17179869184,\"inventory_limit\":536870912,"
        "\"stages\":[\"VS\",\"PS\",\"HS\",\"DS\",\"GS\",\"CS\"],"
        "\"coverage\":[\"graphics_desc\",\"compute_desc\",\"valid_legacy_stage_stream_dispatch\"],"
        "\"unsupported_routes\":[\"stream_rejected_before_dispatch\",\"AS\",\"MS\",\"raytracing_state_object\",\"pipeline_library\"]");
  }

public:
  State() noexcept {
    LastErrorGuard guard;
    try { initialize(); }
    catch (...) { enabled_ = true; failure(0, "", "capture_initialization_exception", ERROR_NOT_ENOUGH_MEMORY, true); }
  }
  ~State() noexcept {
    LastErrorGuard guard;
    if (enabled_) totals("session.end", 0, true);
    if (journal_ != INVALID_HANDLE_VALUE && !CloseHandle(journal_)) emergency("inventory_close", GetLastError());
  }
  State(const State &) = delete;
  static State &instance() noexcept { static State state; return state; }

  template<typename Desc>
  uint64_t begin(const char *kind, const void *owner, const Desc *desc) noexcept {
    LastErrorGuard guard;
    if (!enabled_) return 0;
    uint64_t id = 0;
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      id = ++psos_;
      ++open_;
      if (stopped_) { ++suppressed_; return id; }
      char fields[256];
      std::snprintf(fields, sizeof(fields),
          ",\"pso\":%llu,\"kind\":\"%s\",\"owner\":\"%p\",\"descriptor_present\":%s",
          (unsigned long long)id, kind, owner, desc ? "true" : "false");
      if (!record(id, "", "pso.begin", fields)) return id;
      if (desc) {
        Desc snapshot{};
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), desc, &snapshot, sizeof(snapshot), &read) || read != sizeof(snapshot))
          failure(id, "", "descriptor_memory_read", GetLastError(), false);
        else capture(id, snapshot);
      }
    } catch (...) {
      // Allocation/locking exceptions belong to diagnostics, never to the application.
      try { std::lock_guard<std::mutex> lock(mutex_); failure(id, "", "capture_exception", ERROR_NOT_ENOUGH_MEMORY, true); }
      catch (...) { emergency("capture_lock_exception", ERROR_NOT_ENOUGH_MEMORY); }
    }
    return id;
  }

  void capture(uint64_t id, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
    shader(id, "VS", desc.VS); shader(id, "PS", desc.PS); shader(id, "HS", desc.HS);
    shader(id, "DS", desc.DS); shader(id, "GS", desc.GS);
  }
  void capture(uint64_t id, const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc) { shader(id, "CS", desc.CS); }

  void end(uint64_t id, HRESULT hr, bool returned) noexcept {
    LastErrorGuard guard;
    if (!id) return;
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (open_) --open_;
      if (!returned) incomplete_ = true;
      if (stopped_) return;
      char value[16], fields[160];
      if (returned) std::snprintf(value, sizeof(value), "\"%08x\"", unsigned(hr));
      else std::strcpy(value, "null");
      std::snprintf(fields, sizeof(fields), ",\"pso\":%llu,\"hr\":%s,\"returned\":%s",
                    (unsigned long long)id, value, returned ? "true" : "false");
      if (record(id, "", "pso.result", fields)) totals("pso.end", id);
    } catch (...) { emergency("capture_end_exception", ERROR_NOT_ENOUGH_MEMORY); }
  }
};

class Scope {
  uint64_t id_;
public:
  template<typename Desc>
  Scope(const char *kind, const void *owner, const Desc *desc) noexcept
      : id_(State::instance().begin(kind, owner, desc)) {}
  Scope(const Scope &) = delete;
  ~Scope() noexcept { if (id_) State::instance().end(id_, E_FAIL, false); }
  HRESULT finish(HRESULT hr) noexcept {
    State::instance().end(id_, hr, true);
    id_ = 0;
    return hr;
  }
};

} // namespace dxmt::shader_capture
