#pragma once
#include "sha1/sha1_util.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dxmt {

/*
 * MacRunner HKGPU 03.11.2026 (MACRUNNER_DXMT_D11_CACHE_KEY=1): table version of the D3D11 shader cache.
 * (base << 32) | first 4 bytes of sha1(fingerprint "\n" sorted emission switches): every MACRUNNER_AIRCONV_* / AIRCONV_*
 * entry of the environment except *_TRACE and AIRCONV_VERIFY (the family rule of d3d12_pso_cache.cpp, e14aab8).
 * Pure function (no Windows calls): unit-tested natively by tools/cachekey_test.cpp of the HKGPU lane.
 */
inline uint64_t
ShaderCacheMixVersion(uint32_t base, const std::string &fingerprint, const std::vector<std::string> &environment,
                      std::string *switches_out = nullptr) {
  std::vector<std::string> vars;
  for (const auto &e : environment) {
    const size_t eq = e.find('=');
    const std::string name = e.substr(0, eq == std::string::npos ? e.size() : eq);
    const bool trace_only = name.size() > 6 && name.compare(name.size() - 6, 6, "_TRACE") == 0;
    const bool family = !trace_only && name != "AIRCONV_VERIFY" &&
                        (e.rfind("MACRUNNER_AIRCONV_", 0) == 0 || e.rfind("AIRCONV_", 0) == 0);
    if (family)
      vars.push_back(e);
  }
  std::sort(vars.begin(), vars.end());
  std::string switches;
  for (const auto &v : vars)
    switches += v + "\n";
  const std::string mixed = fingerprint + "\n" + switches;
  const auto digest = Sha1HashState::compute(mixed.data(), mixed.size());
  uint32_t mix = 0;
  std::memcpy(&mix, digest.data, sizeof(mix));
  if (switches_out)
    *switches_out = switches;
  return (uint64_t(base) << 32) | mix;
}

} // namespace dxmt
