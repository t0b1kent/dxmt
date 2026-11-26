#include "dxmt_shader_cache.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "sha1/sha1_util.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace dxmt {

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  static dxmt::mutex mutex;
  static std::unordered_map<WMTMetalVersion, std::unique_ptr<ShaderCache>> caches;

  std::lock_guard<dxmt::mutex> lock(mutex);
  auto iter = caches.find(version);
  if (iter == caches.end()) {
    auto inserted = caches.insert({version, std::make_unique<ShaderCache>(version)});
    return *inserted.first->second;
  }
  return *iter->second;
}

/*
 * MacRunner HKGPU 03.11.2026 (MACRUNNER_DXMT_D11_CACHE_KEY=1, default 0). The D3D11 shader cache table was keyed by
 * AIRCONV_VERSION compiled into THIS PE module only, while the translator runs in the unix winemetal.so: a newer
 * winemetal.so (c18..c25: UNROLL, LICM/GVN, UNIFORM_DIV, VPULL_ONECHECK ...) or a flipped MACRUNNER_AIRCONV_* switch kept
 * reusing AIR produced by whatever translator first met the shader (HK cache: 60 rows in cache_27, file since 04.09).
 * With the key the table version also mixes the fingerprint the loaded winemetal.so reports and every emission switch
 * (MACRUNNER_AIRCONV_* / AIRCONV_* except *_TRACE and AIRCONV_VERIFY), the same family rule as the D3D12 pipeline cache
 * (d3d12_pso_cache.cpp, e14aab8). Key 0: table cache_<AIRCONV_VERSION>, as before.
 */
static uint64_t
ShaderCacheTableVersion() {
  if (env::getEnvVar("MACRUNNER_DXMT_D11_CACHE_KEY") != "1")
    return kDXMTShaderCacheVersion;
  char fingerprint[96] = {};
  WMTGetBuildFingerprint(fingerprint, sizeof(fingerprint));
  std::vector<std::string> environment;
  if (wchar_t *block = GetEnvironmentStringsW()) {
    for (wchar_t *entry = block; *entry; entry += wcslen(entry) + 1) {
      std::wstring e(entry);
      std::string narrow(e.size(), '?');
      for (size_t i = 0; i < e.size(); ++i)
        narrow[i] = e[i] < 128 ? char(e[i]) : '?';
      environment.push_back(std::move(narrow));
    }
    FreeEnvironmentStringsW(block);
  }
  std::string switches;
  const uint64_t version = ShaderCacheMixVersion(kDXMTShaderCacheVersion, fingerprint, environment, &switches);
  for (char &c : switches)
    if (c == '\n')
      c = ' ';
  fprintf(stderr, "macrunner-d11-shader-cache table=cache_%llu airconv=%d winemetal=%s switches=[%s]\n",
          (unsigned long long)version, (int)kDXMTShaderCacheVersion, fingerprint, switches.c_str());
  return version;
}

ShaderCache::ShaderCache(WMTMetalVersion metal_version) {
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0")
    return;
  std::string path;
  if (path = env::getEnvVar("DXMT_SHADER_CACHE_PATH"); !path.empty() && path.starts_with("/")) {
    if (!path.ends_with('/'))
      path += "/";
  } else {
    path = str::format("dxmt/", env::getExeName(), "/");
  }
  path += str::format("shaders_", (unsigned int)metal_version, ".db");
  const uint64_t table_version = ShaderCacheTableVersion();
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), table_version);
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), table_version);
}

} // namespace dxmt
